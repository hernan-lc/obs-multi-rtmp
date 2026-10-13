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

        // Standalone-encoder preflight: an explicitly selected custom
        // config id that is missing is an actionable error, never a silent
        // fallback to the shared OBS encoder, and the saved selection is
        // left untouched for the user to fix.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        config->videoConfig = "no-such-video-config";
        config->audioConfig = "no-such-audio-config";
        CHECK(!widget->GetVideoEncoder().Get());
        CHECK(std::string(widget->lastEncoderError_ ? widget->lastEncoderError_ : "")
              == "Error.MissingVideoConfig");
        CHECK(config->videoConfig == "no-such-video-config");
        CHECK(!widget->GetAudioEncoder().Get());
        CHECK(std::string(widget->lastEncoderError_ ? widget->lastEncoderError_ : "")
              == "Error.MissingAudioConfig");
        CHECK(config->audioConfig == "no-such-audio-config");

        // Invalid resolution, FPS divisor, and mixer index fail the same
        // way instead of starting a misconfigured output.
        auto badVideo = std::make_shared<VideoEncoderConfig>();
        badVideo->id = "bad-video"; badVideo->encoderId = "obs_x264";
        badVideo->resolution = "not-a-resolution"; badVideo->fpsDenumerator = 1;
        GlobalMultiOutputConfig().videoConfig.push_back(badVideo);
        config->videoConfig = "bad-video";
        CHECK(!widget->GetVideoEncoder().Get());
        CHECK(std::string(widget->lastEncoderError_ ? widget->lastEncoderError_ : "")
              == "Error.InvalidVideoConfig");
        badVideo->resolution.reset(); badVideo->fpsDenumerator = 0;
        CHECK(!widget->GetVideoEncoder().Get());
        CHECK(std::string(widget->lastEncoderError_ ? widget->lastEncoderError_ : "")
              == "Error.InvalidVideoConfig");
        auto badAudio = std::make_shared<AudioEncoderConfig>();
        badAudio->id = "bad-audio"; badAudio->encoderId = "ffmpeg_aac";
        badAudio->mixerId = 99;
        GlobalMultiOutputConfig().audioConfig.push_back(badAudio);
        config->audioConfig = "bad-audio";
        CHECK(!widget->GetAudioEncoder().Get());
        CHECK(std::string(widget->lastEncoderError_ ? widget->lastEncoderError_ : "")
              == "Error.InvalidAudioConfig");
        config->videoConfig.reset(); config->audioConfig.reset();

        // A disabled target refuses Start without any remote call, and a
        // failed local start releases the starting guard so the next
        // Start is not blocked.
        config->enabled = false;
        widget->StartStreaming();
        CHECK(starts == beforeRetry);
        CHECK(widget->restreamState_ == Local::Stopped);
        config->enabled = true;
        config->tiktok.enabled = false;
        widget->StartStreaming();
        CHECK(!widget->isStarting_);
        CHECK(widget->restreamState_ == Local::Stopped);
        CHECK(starts == beforeRetry && ends == 7);
        config->tiktok.enabled = true;
        widget.reset();

        // Export roundtrip keeps TikTok settings but never credentials;
        // malformed input yields nullopt instead of wiping targets.
        config->tiktok.title = "QA Title";
        const std::string exported = SerializeMultiOutputConfig(GlobalMultiOutputConfig());
        CHECK(exported.find("QA Title") != std::string::npos);
        CHECK(exported.find("token") == std::string::npos);
        CHECK(exported.find("FAKE-KEY") == std::string::npos);
        CHECK(DeserializeMultiOutputConfig(exported).has_value());
        CHECK(!DeserializeMultiOutputConfig("{not json").has_value());

        // Strict import validation: valid JSON with a foreign shape must be
        // rejected, never applied as an empty configuration. The current
        // targets must survive every rejection below.
        const size_t targetsBefore = GlobalMultiOutputConfig().targets.size();
        CHECK(!DeserializeMultiOutputConfig("{}").has_value());
        CHECK(!DeserializeMultiOutputConfig("{\"hello\":\"world\"}").has_value());
        CHECK(!DeserializeMultiOutputConfig("[]").has_value());
        CHECK(!DeserializeMultiOutputConfig("{\"targets\":{}}").has_value());
        CHECK(!DeserializeMultiOutputConfig("{\"targets\":[{\"name\":\"x\"}]}").has_value());
        CHECK(!DeserializeMultiOutputConfig("{\"targets\":[{\"id\":42}]}").has_value());
        CHECK(!DeserializeMultiOutputConfig("{\"targets\":[],\"video_configs\":{}}").has_value());
        CHECK(!DeserializeMultiOutputConfig(
            "{\"targets\":[{\"id\":\"a\"}],\"video_configs\":[{}]}").has_value());
        CHECK(DeserializeMultiOutputConfig(
            "{\"targets\":[{\"id\":\"a\"}],\"video_configs\":[{\"id\":\"v\"}],\"audio_configs\":[]}").has_value());
        CHECK(GlobalMultiOutputConfig().targets.size() == targetsBefore);

        // P0 guard regression, exact reported sequence: Start arms the
        // guard and defers on endpoint creation; disabling cancels the
        // deferred start; the orphaned creation finishes with no local
        // continuation; re-enabling must allow a new Start.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        widget->isStarting_ = true;
        widget->startRestreamWhenReady_ = true;
        holdStart = true; startEntered = false;
        CHECK(widget->BeginEndpointRequest(client));
        widget->SetEnabled(false);
        CHECK(!widget->startRestreamWhenReady_);
        holdStart = false;
        CHECK(wait([&] { return widget->tiktokSession_.HasEndpoint(); }));
        QCoreApplication::processEvents();
        CHECK(!widget->isStarting_);
        CHECK(widget->tiktokSession_.HasEndpoint());
        widget->SetEnabled(true);
        const int startsBeforeGuard = starts.load();
        const int attemptsBeforeGuard = outputAttempts;
        widget->StartStreaming();
        QCoreApplication::processEvents();
        // The compare_exchange accepted the Start (a stuck guard would have
        // refused before any local work): the local stage ran and failed on
        // the fault-injected output, releasing the guard again.
        CHECK(outputAttempts == attemptsBeforeGuard + 1);
        CHECK(!widget->isStarting_);
        CHECK(widget->restreamState_ == Local::Stopped);
        CHECK(starts.load() == startsBeforeGuard);
        widget->EndTikTokEndpoint();
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Idle; }));
        widget.reset();

        // Preflight blocks automatic endpoint creation on a broken local
        // configuration (here: a scene that does not exist), while the
        // manual Create path stays available.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        auto sceneVideo = std::make_shared<VideoEncoderConfig>();
        sceneVideo->id = "v-scene"; sceneVideo->encoderId = "obs_x264";
        sceneVideo->outputScene = "no-such-scene";
        GlobalMultiOutputConfig().videoConfig.push_back(sceneVideo);
        config->videoConfig = "v-scene";
        config->audioConfig.reset();
        const int startsBeforePreflight = starts.load();
        widget->StartStreaming();
        QCoreApplication::processEvents();
        CHECK(starts.load() == startsBeforePreflight);
        CHECK(!widget->tiktokSession_.HasEndpoint());
        CHECK(widget->tiktokSession_.state == Remote::Idle);
        CHECK(!widget->isStarting_);
        // ...but an explicit endpoint request still works without encoders.
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([&] { return widget->tiktokSession_.HasEndpoint(); }));
        widget->EndTikTokEndpoint();
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Idle; }));
        config->videoConfig.reset();
        widget.reset();

        // Preflight legs, called directly: default shared selections report
        // the unavailable shared encoder headlessly, and valid independent
        // configs pass every creation-free leg (the codec leg then stops at
        // the fault-injected output allocation, which the real-OBS
        // standalone test covers end to end).
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        config->videoConfig.reset(); config->audioConfig.reset();
        CHECK(std::string(widget->ValidateLocalEncoderConfiguration())
              == "Error.SharedEncoderUnavailable");
        auto goodVideo = std::make_shared<VideoEncoderConfig>();
        goodVideo->id = "v-good"; goodVideo->encoderId = "obs_x264";
        GlobalMultiOutputConfig().videoConfig.push_back(goodVideo);
        auto goodAudio = std::make_shared<AudioEncoderConfig>();
        goodAudio->id = "a-good"; goodAudio->encoderId = "ffmpeg_aac";
        GlobalMultiOutputConfig().audioConfig.push_back(goodAudio);
        config->videoConfig = "v-good"; config->audioConfig = "a-good";
        CHECK(std::string(widget->ValidateLocalEncoderConfiguration())
              == "Error.StartOutput");
        config->videoConfig.reset(); config->audioConfig.reset();
        widget.reset();

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
        // (+2 remote Ends consumed by the guard-regression and preflight
        // blocks above.)
        CHECK(wait([] { return ends.load() == 10; }));
        CHECK(wait([] { return !TikTokLiveRegistry::hasActiveActivity(); }));

        // Explicit End failure after owner destruction balances the registry.
        widget = std::make_unique<PushWidgetImpl>("test", nullptr);
        CHECK(widget->BeginEndpointRequest(client));
        CHECK(wait([&] { return widget->tiktokSession_.HasEndpoint(); }));
        failEnd = true;
        widget->EndTikTokEndpoint(); widget.reset();
        CHECK(wait([] { return ends.load() == 11; }));
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
        CHECK(ends == 12);
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
