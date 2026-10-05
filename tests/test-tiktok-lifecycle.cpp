// Exercise the real widget and OBS lifecycle without network/account access.
// The test client implementations below replace streamlabs-client.cpp at link time.
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <iostream>
#include "pch.h"
// libobs requires an initialized graphics context to allocate outputs.
// Fault-inject output allocation so this suite runs on headless CI too.
static std::atomic<int> outputAttempts{0};
static obs_output_t *FailCreateOutput(const char *, const char *, obs_data_t *, obs_data_t *)
{ ++outputAttempts; return nullptr; }
#define obs_output_create FailCreateOutput
#include "../src/push-widget.cpp"
#undef obs_output_create

namespace {
std::atomic<int> starts{0}, ends{0};
std::atomic<bool> failEnd{false}, holdStart{false}, startEntered{false}, startFinished{false};
std::atomic<TikTokFailure> endFailure{TikTokFailure::None}, startFailure{TikTokFailure::None};
}
StreamlabsTikTokClient::StreamlabsTikTokClient(std::string token, uint64_t epoch)
    : token_(std::move(token)), authEpoch_(epoch) {}
TikTokAccountResult StreamlabsTikTokClient::GetAccountInfo() { return {}; }
TikTokCategoryListResult StreamlabsTikTokClient::SearchCategories(const std::string &) { return {}; }
TikTokCategoryResult StreamlabsTikTokClient::ResolveCategory(const std::string &)
{ TikTokCategoryResult result; result.success = true; return result; }
TikTokStartResult StreamlabsTikTokClient::StartStream(const std::string &, const std::string &, TikTokAudience)
{
    ++starts;
    startEntered = true;
    while (holdStart) QThread::msleep(1);
    TikTokStartResult result;
    result.success = startFailure == TikTokFailure::None;
    result.failure = startFailure;
    startFinished = true;
    if (!result.success) return result;
    result.streamId = "fixture-" + std::to_string(starts.load());
    result.server = "rtmp://fixture.invalid/live";
    result.key = "FAKE-KEY";
    return result;
}
bool StreamlabsTikTokClient::EndStream(const std::string &, std::string *, TikTokFailure *failure)
{
    ++ends;
    const auto result = failEnd ? TikTokFailure::Network : endFailure.load();
    if (failure) *failure = result;
    return result == TikTokFailure::None;
}

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; return false; } } while (0)

struct TikTokLifecycleTest {
    using Remote = PushWidgetImpl::TikTokSessionState;
    using Local = PushWidgetImpl::RestreamState;
    template<class Predicate> static bool wait(Predicate predicate) {
        QElapsedTimer elapsed; elapsed.start();
        while (!predicate() && elapsed.elapsed() < 5000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        return predicate();
    }
    static bool run() {
        auto config = std::make_shared<OutputTargetConfig>();
        config->id = "test"; config->name = "Fixture"; config->tiktok.enabled = true;
        config->serviceParam = nlohmann::json::object();
        config->outputParam = nlohmann::json::object();
        GlobalMultiOutputConfig().targets.push_back(config);
        auto client = std::make_shared<StreamlabsTikTokClient>("FAKE");
        auto widget = std::make_unique<PushWidgetImpl>("test", nullptr);

        // No OBS output or encoders: creation is independent. Rapid clicks
        // must submit only one request, even before completion reaches the UI.
        holdStart = true;
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(!widget->BeginEndpointRequest(client));
        CHECK(wait([] { return startEntered.load(); }));
        CHECK(widget->output_ == nullptr);
        CHECK(starts == 1);
        widget->startRestreamWhenReady_ = true;
        config->syncStop = true;
        widget->OnOBSEvent(OBS_FRONTEND_EVENT_STREAMING_STOPPING);
        CHECK(!widget->startRestreamWhenReady_);
        CHECK(widget->tiktokSession_.state == Remote::CreatingEndpoint);
        config->syncStop = false;
        holdStart = false;
        CHECK(wait([&] { return widget->tiktokSession_.HasEndpoint(); }));
        CHECK(widget->tiktokSession_.state == Remote::EndpointReady);
        CHECK(widget->output_ == nullptr);
        CHECK(widget->tiktokSession_.streamKey == "FAKE-KEY");
        CHECK(config->serviceParam.find("key") == config->serviceParam.end());

        // Missing OBS output implementation and repeated local retries preserve
        // the endpoint and never submit another remote creation request.
        widget->StartStreaming();
        CHECK(widget->restreamState_ == Local::Stopped);
        CHECK(widget->tiktokSession_.state == Remote::EndpointReady);
        widget->StartStreaming();
        CHECK(starts == 1 && ends == 0);
        widget->StopStreaming();
        CHECK(widget->tiktokSession_.HasEndpoint() && ends == 0);
        const int beforeConnectingStop = outputAttempts;
        widget->restreamState_ = Local::Connecting;
        widget->StartStop();
        CHECK(widget->restreamState_ == Local::Stopped);
        CHECK(outputAttempts == beforeConnectingStop);
        widget->OnStopped(OBS_OUTPUT_CONNECT_FAILED);
        QCoreApplication::processEvents();
        CHECK(widget->tiktokSession_.state == Remote::EndpointReady && ends == 0);

        // Queued callbacks must not update a replacement output or dead widget.
        widget->OnStarted();
        widget->ReleaseObsOutput();
        QCoreApplication::processEvents();
        CHECK(widget->restreamState_ == Local::Stopped);
        widget->OnStarted();
        widget->ShutdownTikTokIntegration();
        CHECK(wait([] { return ends.load() == 1; }));
        CHECK(!widget->tiktokSession_.HasEndpoint());
        widget.reset();
        QCoreApplication::processEvents();
        CHECK(ends == 1);

        // Explicit End guards overlap; a failed End retains credentials for retry.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([&] { return widget->tiktokSession_.HasEndpoint(); }));
        failEnd = true;
        widget->EndTikTokEndpoint(); widget->EndTikTokEndpoint();
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::EndpointReady; }));
        CHECK(ends == 2);
        CHECK(widget->tiktokSession_.HasEndpoint());
        failEnd = false;
        widget->EndTikTokEndpoint(); widget->EndTikTokEndpoint();
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Idle; }));
        CHECK(ends == 3 && !widget->tiktokSession_.HasEndpoint());
        CHECK(widget->tiktokSession_.streamKey.empty() && !widget->tiktokSession_.client);
        widget.reset();

        // Closing the owner during /start must safely end the unadopted result.
        holdStart = true; startEntered = false;
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([] { return startEntered.load(); }));
        widget.reset(); holdStart = false;
        CHECK(wait([] { return ends.load() == 4; }));
        CHECK(wait([] { return !TikTokLiveRegistry::hasActiveActivity(); }));

        // End requested while creation is in flight waits for its result.
        holdStart = true; startEntered = false;
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([] { return startEntered.load(); }));
        widget->EndTikTokEndpoint(); holdStart = false;
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Idle; }));
        CHECK(ends == 5);
        widget.reset();
        CHECK(wait([] { return !TikTokLiveRegistry::hasActiveActivity(); }));

        // A confirmed missing remote stream clears credentials. An invalid
        // token is a remote error and must prevent local restart.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([&] { return widget->tiktokSession_.HasEndpoint(); }));
        endFailure = TikTokFailure::Auth;
        widget->EndTikTokEndpoint();
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Error; }));
        widget->StartStreaming();
        CHECK(widget->restreamState_ == Local::Stopped);
        CHECK(widget->tiktokSession_.HasEndpoint());
        endFailure = TikTokFailure::NotFound;
        widget->EndTikTokEndpoint();
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Idle; }));
        CHECK(ends == 7);
        widget.reset(); endFailure = TikTokFailure::None;

        // Uncertain /start responses cannot trigger a duplicate LIVE via
        // repeated Start. There is no lookup-by-request API to reconcile it.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        startFailure = TikTokFailure::Timeout;
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Error; }));
        CHECK(widget->endpointCreationUncertain_);
        const int beforeRetry = starts;
        widget->StartStreaming(); widget->StartStreaming();
        CHECK(starts == beforeRetry);
        CHECK(!widget->startRestreamWhenReady_);
        widget.reset(); startFailure = TikTokFailure::None;

        // Generic RTMP takes only the local path, without any TikTok calls.
        config->tiktok.enabled = false;
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        widget->StartStreaming(); widget->StopStreaming();
        CHECK(starts == beforeRetry && ends == 7);
        CHECK(widget->create_live_btn_->isHidden());
        widget.reset(); config->tiktok.enabled = true;

        // Owner disappears after worker completion, before UI adoption.
        startFinished = false;
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        QElapsedTimer timeout; timeout.start();
        while (!startFinished && timeout.elapsed() < 5000) QThread::msleep(1);
        CHECK(startFinished);
        while (TikTokWorkerService::instance().hasActiveActivity() && timeout.elapsed() < 5000)
            QThread::msleep(1);
        CHECK(!TikTokWorkerService::instance().hasActiveActivity());
        widget.reset();
        CHECK(wait([] { return ends.load() == 8; }));
        CHECK(wait([] { return !TikTokLiveRegistry::hasActiveActivity(); }));

        // Explicit End failure after owner destruction balances the registry.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([&] { return widget->tiktokSession_.HasEndpoint(); }));
        failEnd = true;
        widget->EndTikTokEndpoint(); widget.reset();
        CHECK(wait([] { return ends.load() == 9; }));
        CHECK(wait([] { return !TikTokLiveRegistry::hasActiveActivity(); }));
        failEnd = false;

        // Shutdown discards queued completions before plugin code unloads,
        // and ends a successful creation that was never adopted by the UI.
        startFinished = false;
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        timeout.restart();
        while (!startFinished && timeout.elapsed() < 5000) QThread::msleep(1);
        CHECK(startFinished);
        TikTokWorkerService::instance().shutdown();
        CHECK(ends == 10);
        widget.reset();
        CHECK(!TikTokLiveRegistry::hasActiveActivity());
        return true;
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (!obs_startup("en-US", nullptr, nullptr)) return 1;
    const bool ok = TikTokLifecycleTest::run();
    holdStart = false;
    TikTokAuthManager::instance().shutdown();
    obs_shutdown();
    if (ok) std::cout << "TikTok lifecycle regression checks passed\n";
    return ok ? 0 : 1;
}
