// Real standalone-streaming integration: independent x264 video + AAC audio
// through the actual PushWidget paths, with real OBS video/audio contexts,
// real plugins (obs-outputs, rtmp-services, obs-x264, obs-ffmpeg) and real
// outputs. Needs a working video device (fails safe with SKIP when video
// init is unavailable, e.g. display-less CI). No network, no TikTok
// account, no main OBS streaming output is ever started.
//
// "rtmp_output" creations are redirected to "null_output" so the widget's
// full RTMP path (service with TikTok server/key, independent encoders,
// start/stop lifecycle) runs without a handshake peer. A separate section
// uses the real rtmp_output against a closed localhost port to prove the
// refused-connection mapping and endpoint preservation.
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include "pch.h"
#include <obs-module.h>
#include <util/platform.h>

static std::atomic<bool> redirectRtmpToNull{true};
static obs_output_t *InterceptCreateOutput(const char *id, const char *name,
                                           obs_data_t *settings,
                                           obs_data_t *hotkey_data)
{
#undef obs_output_create
    obs_output_t *out = nullptr;
    if (redirectRtmpToNull && id && std::strcmp(id, "rtmp_output") == 0)
        out = obs_output_create("null_output", name, settings, hotkey_data);
    else
        out = obs_output_create(id, name, settings, hotkey_data);
#define obs_output_create InterceptCreateOutput
    return out;
}
#define obs_output_create InterceptCreateOutput
#include "../src/push-widget.cpp"
#undef obs_output_create

namespace {
std::atomic<int> starts{0}, ends{0};
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
    TikTokStartResult result;
    result.success = true;
    result.streamId = "standalone-fixture";
    result.server = "rtmp://127.0.0.1/unused";
    result.key = "FIXTURE-KEY";
    return result;
}
bool StreamlabsTikTokClient::EndStream(const std::string &, std::string *, TikTokFailure *failure)
{
    ++ends;
    if (failure) *failure = TikTokFailure::None;
    return true;
}

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; return false; } } while (0)

struct StandaloneTest {
    using Remote = PushWidgetImpl::TikTokSessionState;
    using Local = PushWidgetImpl::RestreamState;
    template<class Predicate> static bool wait(Predicate predicate, int ms = 15000) {
        QElapsedTimer elapsed; elapsed.start();
        while (!predicate() && elapsed.elapsed() < ms) {
            QCoreApplication::processEvents(); QThread::msleep(5);
        }
        return predicate();
    }

    static bool loadPlugin(const char *name) {
        const char *dir = std::getenv("OBS_TEST_PLUGIN_PATH");
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/%s.so",
                      dir ? dir : "/usr/lib/obs-plugins", name);
        obs_module_t *module = nullptr;
        if (obs_open_module(&module, path, nullptr) != MODULE_SUCCESS) {
            std::cout << "SKIP: cannot open " << path << "\n";
            return false;
        }
        if (!obs_init_module(module)) {
            std::cout << "SKIP: cannot init " << name << "\n";
            return false;
        }
        return true;
    }

    static void presetEndpoint(PushWidgetImpl *widget, const std::string &id,
                               const std::string &server) {
        widget->tiktokSession_.streamId = id;
        widget->tiktokSession_.server = server;
        widget->tiktokSession_.streamKey = "FIXTURE-KEY";
        widget->tiktokSession_.client =
            std::make_shared<StreamlabsTikTokClient>("FIXTURE");
        widget->tiktokSession_.state = Remote::EndpointReady;
        // Mirror OnTikTokPrepared()'s adoption accounting so owner teardown
        // balances the registry exactly.
        TikTokLiveRegistry::addLive();
    }

    static bool run() {
        if (!loadPlugin("obs-outputs") || !loadPlugin("rtmp-services") ||
            !loadPlugin("obs-x264") || !loadPlugin("obs-ffmpeg"))
            return true; // SKIP printed above.

        struct obs_video_info ovi = {};
        ovi.adapter = 0;
        ovi.graphics_module = "libobs-opengl";
        ovi.base_width = 1280; ovi.base_height = 720;
        ovi.output_width = 1280; ovi.output_height = 720;
        ovi.fps_num = 30; ovi.fps_den = 1;
        ovi.output_format = VIDEO_FORMAT_NV12;
        ovi.colorspace = VIDEO_CS_SRGB;
        if (obs_reset_video(&ovi) != 0) {
            std::cout << "SKIP: obs_reset_video unavailable (no video device)\n";
            return true;
        }
        struct obs_audio_info oai = {};
        oai.samples_per_sec = 44100;
        oai.speakers = SPEAKERS_STEREO;
        if (!obs_reset_audio(&oai)) {
            std::cout << "SKIP: obs_reset_audio unavailable\n";
            return true;
        }

        // Baseline encoder availability (informational for VAAPI, required
        // for the x264/AAC proof below).
        {
            bool haveX264 = false, haveAac = false, haveVaapi = false;
            for (size_t i = 0;; ++i) {
                const char *id = nullptr;
                if (!obs_enum_encoder_types(i, &id))
                    break;
                if (std::strcmp(id, "obs_x264") == 0) haveX264 = true;
                if (std::strcmp(id, "ffmpeg_aac") == 0) haveAac = true;
                if (std::strcmp(id, "ffmpeg_vaapi") == 0) haveVaapi = true;
            }
            std::cout << "encoders: x264=" << haveX264 << " aac=" << haveAac
                      << " vaapi=" << haveVaapi << "\n";
            if (!haveX264 || !haveAac) {
                std::cout << "SKIP: software baseline encoders unavailable\n";
                return true;
            }
            if (haveVaapi) {
                obs_data_t *vs = obs_data_create();
                obs_encoder_t *va = obs_video_encoder_create(
                    "ffmpeg_vaapi", "standalone-vaapi-probe", vs, nullptr);
                std::cout << "vaapi_create=" << (va ? "ok" : "unavailable") << "\n";
                if (va) obs_encoder_release(va);
                obs_data_release(vs);
            }
        }

        auto config = std::make_shared<OutputTargetConfig>();
        config->id = "standalone"; config->name = "Standalone";
        config->protocol = "RTMP";
        config->tiktok.enabled = true;
        config->serviceParam = nlohmann::json::object();
        config->outputParam = nlohmann::json::object();
        auto video = std::make_shared<VideoEncoderConfig>();
        video->id = "sv"; video->encoderId = "obs_x264";
        video->encoderParams = {{"rate_control", "CBR"}, {"bitrate", 2000}};
        GlobalMultiOutputConfig().videoConfig.push_back(video);
        auto audio = std::make_shared<AudioEncoderConfig>();
        audio->id = "sa"; audio->encoderId = "ffmpeg_aac";
        audio->encoderParams = {{"bitrate", 128}};
        GlobalMultiOutputConfig().audioConfig.push_back(audio);
        config->videoConfig = "sv"; config->audioConfig = "sa";
        GlobalMultiOutputConfig().targets.push_back(config);

        auto widget = std::make_unique<PushWidgetImpl>("standalone", nullptr);

        // Full positive preflight with real video/audio: config legs plus
        // the codec check against a real output. Must return null.
        CHECK(widget->ValidateLocalEncoderConfiguration() == nullptr);

        // Independent start with a preset endpoint: no /stream/start call,
        // no main OBS streaming output, real x264+AAC encoding flowing.
        presetEndpoint(widget.get(), "standalone-fixture-a", "rtmp://127.0.0.1/unused");
        widget->StartStreaming();
        CHECK(wait([&] { return widget->IsRunning(); }));
        CHECK(starts.load() == 0);
        const std::string vencName = "multi-rtmp-vencsv";
        const std::string aencName = "multi-rtmp-aencsa-track-idx-0";
        {
            obs_encoder_t *venc = obs_get_encoder_by_name(vencName.c_str());
            obs_encoder_t *aenc = obs_get_encoder_by_name(aencName.c_str());
            CHECK(venc && aenc);
            if (venc) obs_encoder_release(venc);
            if (aenc) obs_encoder_release(aenc);
        }
        CHECK(wait([&] {
            return widget->output_ &&
                   obs_output_get_total_frames(widget->output_) > 0;
        }));
        CHECK(config->serviceParam.find("key") == config->serviceParam.end());
        widget->StopStreaming();
        CHECK(wait([&] {
            return !widget->IsRunning() &&
                   widget->restreamState_ == Local::Stopped;
        }));
        CHECK(widget->tiktokSession_.HasEndpoint());
        CHECK(widget->tiktokSession_.streamId == "standalone-fixture-a");
        CHECK(ends.load() == 0);

        // Second start reuses the same endpoint and encoder names.
        widget->StartStreaming();
        CHECK(wait([&] { return widget->IsRunning(); }));
        CHECK(widget->tiktokSession_.streamId == "standalone-fixture-a");
        CHECK(starts.load() == 0);
        CHECK(wait([&] {
            return widget->output_ &&
                   obs_output_get_total_frames(widget->output_) > 0;
        }));
        widget->StopStreaming();
        CHECK(wait([&] { return !widget->IsRunning(); }));
        CHECK(widget->tiktokSession_.HasEndpoint() && ends.load() == 0);

        // Explicit End closes the first session before the second preset
        // adopts another one, keeping registry accounting exact.
        widget->EndTikTokEndpoint();
        CHECK(wait([&] { return widget->tiktokSession_.state == Remote::Idle; }));
        CHECK(ends.load() == 1);

        // Real rtmp_output against a closed localhost port: encoders
        // prepare, the connection is refused, the stop is surfaced, and the
        // endpoint is preserved for retry (no /stream/end).
        redirectRtmpToNull = false;
        presetEndpoint(widget.get(), "standalone-fixture-b", "rtmp://127.0.0.1:9/live");
        widget->tiktokSession_.streamKey = "FIXTURE-KEY";
        widget->StartStreaming();
        CHECK(wait([&] {
            return widget->restreamState_ == Local::Stopped;
        }, 30000));
        std::cout << "refused stop code=" << widget->lastErrorCode_
                  << " msg=" << widget->msg_->text().toStdString() << "\n";
        CHECK(widget->tiktokSession_.HasEndpoint());
        CHECK(ends.load() == 1);
        CHECK(!widget->msg_->text().isEmpty());
        widget.reset();

        // Owner teardown ends every adopted endpoint exactly once and
        // balances the registry.
        CHECK(wait([] { return !TikTokLiveRegistry::hasActiveActivity(); }));
        CHECK(ends.load() == 2);
        return true;
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (!obs_startup("en-US", nullptr, nullptr)) return 1;
    const bool ok = StandaloneTest::run();
    redirectRtmpToNull = true;
    TikTokAuthManager::instance().shutdown();
    obs_shutdown();
    if (ok) std::cout << "TikTok standalone regression checks passed\n";
    return ok ? 0 : 1;
}
