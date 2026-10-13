#include "pch.h"
#include "helpers.h"
#include <regex>
#include <atomic>
#include <optional>
#include <tuple>
#include <mutex>
#include <QCoreApplication>
#include <QThread>
#include <QPointer>
#include <QMetaObject>
#include "push-widget.h"
#include "edit-widget.h"
#include "output-config.h"
#include "protocols.h"
#include "streamlabs-tiktok.h"
#include "tiktok/auth-manager.h"
#include "tiktok/cleanup.h"
#include "tiktok/streamlabs-client.h"
#include "tiktok/worker.h"

#include "obs.hpp"

class IOBSOutputEventHanlder
{
public:
    virtual void OnStarting() {}
    static void OnOutputStarting(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarting();
    }

    virtual void OnStarted() {}
    static void OnOutputStarted(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarted();
    }

    virtual void OnStopping() {}
    static void OnOutputStopping(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopping();
    }
   
    virtual void OnStopped(int) {}
    static void OnOutputStopped(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopped(calldata_int(param, "code"));
    }

    virtual void OnReconnect() {}
    static void OnOutputReconnect(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnect();
    }

    virtual void OnReconnected() {}
    static void OnOutputReconnected(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnected();
    }

    virtual void onDeactive() {}
    static void OnOutputDeactive(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->onDeactive();
    }

    void SetMeAsHandler(obs_output_t* output)
    {
        auto outputSignal = obs_output_get_signal_handler(output);
        if (outputSignal)
        {
            signal_handler_connect(outputSignal, "starting", &IOBSOutputEventHanlder::OnOutputStarting, this);
            signal_handler_connect(outputSignal, "start", &IOBSOutputEventHanlder::OnOutputStarted, this);
            signal_handler_connect(outputSignal, "reconnect", &IOBSOutputEventHanlder::OnOutputReconnect, this);
            signal_handler_connect(outputSignal, "reconnect_success", &IOBSOutputEventHanlder::OnOutputReconnected, this);
            signal_handler_connect(outputSignal, "stopping", &IOBSOutputEventHanlder::OnOutputStopping, this);
            signal_handler_connect(outputSignal, "deactivate", &IOBSOutputEventHanlder::OnOutputDeactive, this);
            signal_handler_connect(outputSignal, "stop", &IOBSOutputEventHanlder::OnOutputStopped, this);
        }
    }

    void DisconnectSignals(obs_output_t* output)
    {
        auto outputSignal = obs_output_get_signal_handler(output);
        if (outputSignal)
        {
            signal_handler_disconnect(outputSignal, "starting", &IOBSOutputEventHanlder::OnOutputStarting, this);
            signal_handler_disconnect(outputSignal, "start", &IOBSOutputEventHanlder::OnOutputStarted, this);
            signal_handler_disconnect(outputSignal, "reconnect", &IOBSOutputEventHanlder::OnOutputReconnect, this);
            signal_handler_disconnect(outputSignal, "reconnect_success", &IOBSOutputEventHanlder::OnOutputReconnected, this);
            signal_handler_disconnect(outputSignal, "stopping", &IOBSOutputEventHanlder::OnOutputStopping, this);
            signal_handler_disconnect(outputSignal, "deactivate", &IOBSOutputEventHanlder::OnOutputDeactive, this);
            signal_handler_disconnect(outputSignal, "stop", &IOBSOutputEventHanlder::OnOutputStopped, this);
        }
    }
};


class PushWidgetImpl : public PushWidget, public IOBSOutputEventHanlder
{
    std::string targetid_;
    OutputTargetConfigPtr config_;

    QPushButton* btn_ = 0;
    QLabel* name_ = 0;
    QLabel* msg_ = 0;
    QLabel* status_dot_ = 0;
    QCheckBox* enabled_cb_ = 0;

    using clock = std::chrono::steady_clock;
    clock::time_point begin_time_;
    clock::time_point last_info_time_;
    uint64_t total_frames_ = 0;
    uint64_t total_bytes_ = 0;
    QTimer* timer_ = 0;

    QPushButton* edit_btn_ = 0;
    QPushButton* remove_btn_ = 0;

    obs_output_t* output_ = 0;
    bool using_main_video_encoder_ = false;
    bool using_main_audio_encoder_ = false;
    obs_view_t* scene_view_ = 0;
    bool isUseDelay_ = false;
    std::atomic_bool isStarting_ = false;

    bool isConnecting_ = false;
    bool isReconnecting_ = false;
    int lastErrorCode_ = 0;
    // Locale key for the last encoder-resolution failure (null when ok).
    // Set by GetVideoEncoder()/GetAudioEncoder(), consumed by
    // PrepareOutputEncoders() for an actionable message.
    const char* lastEncoderError_ = nullptr;

    // Registered under the stable target id (not the display name) so key
    // bindings survive renames. Their description label is set once at
    // registration time and won't update if the target is renamed later.
    obs_hotkey_id startHotkeyId_ = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id stopHotkeyId_ = OBS_INVALID_HOTKEY_ID;

    // Remote resources are independent from the local OBS output. UI thread only.
    enum class TikTokSessionState { Idle, Authenticating, CreatingEndpoint,
                                    EndpointReady, EndingEndpoint, Error };
    struct TikTokSession {
        TikTokSessionState state = TikTokSessionState::Idle;
        std::string streamId, server, streamKey;
        std::shared_ptr<StreamlabsTikTokClient> client;
        bool HasEndpoint() const
        { return !streamId.empty() && !server.empty() && !streamKey.empty(); }
        void Reset() { *this = TikTokSession{}; }
    } tiktokSession_;
    enum class RestreamState { Stopped, Preparing, Connecting, Streaming, Stopping };
    RestreamState restreamState_ = RestreamState::Stopped;
    uint64_t tiktokRequestGen_ = 0;
    std::atomic<uint64_t> outputGeneration_{0};
    friend struct TikTokLifecycleTest;
    bool startRestreamWhenReady_ = false;
    bool endAfterStop_ = false;
    bool endpointCreationUncertain_ = false;
    QPushButton *create_live_btn_ = nullptr;
    QPushButton *end_live_btn_ = nullptr;

    QPushButton* GetDeleteButton() {
        return remove_btn_;
    }

    bool PrepareOutputService(const std::string &server = {}, const std::string &streamKey = {})
    {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output service before output object is created.");
            return false;
        }
        
        ReleaseOutputService();
        
        OBSDataAutoRelease conf = obs_data_create_from_json(config_->serviceParam.dump().c_str());
        if (conf && config_->tiktok.enabled) {
            obs_data_set_string(conf, "server", server.c_str());
            obs_data_set_string(conf, "key", streamKey.c_str());
        }

        auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        assert(protocolInfo);
        if (!protocolInfo) {
        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
        	return false;
        }
        auto service_id = protocolInfo->serviceId;

        if (!conf)
            return false;
        
        auto service = obs_service_create(service_id, "multi-output-service", conf, nullptr);
        if (!service)
            return false;
        obs_output_set_service(output_, service);

        return true;
    }


    bool ReleaseOutputService()
    {
        if (!output_)
            return true;

        if (obs_output_active(output_)) {
            return false;
        }

        // obs_output_get_service() returns a borrowed pointer, and the
        // setter takes no reference and ignores null, so dropping our
        // creation reference is the whole job. The caller must not destroy
        // the output afterwards while it still points at a freed service:
        // see ReleaseObsOutput().
        auto service = obs_output_get_service(output_);
        if (service)
            obs_service_release(service);
        return true;
    }


    bool PrepareEncoderSource() {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output scene before output object is created.");
            return false;
        }

        if (!using_main_video_encoder_) {
            auto venc = obs_output_get_video_encoder(output_);
            if (!venc) {
                blog(LOG_ERROR, TAG "Prepare output scene before encoder is created.");
                return false;
            }
            auto videoConfig = FindById(GlobalMultiOutputConfig().videoConfig, config_->videoConfig.value_or(""));

            if (!videoConfig || !videoConfig->outputScene.has_value()) {
                obs_encoder_set_video(venc, obs_get_video());
            } else {
                auto sceneName = *videoConfig->outputScene;
                OBSSourceAutoRelease scene = obs_get_source_by_name(sceneName.c_str());
                if (scene == nullptr) {
                    blog(LOG_ERROR, TAG "Output scene is not found.");
                    return false;
                }
                ReleaseOutputSceneView();

                scene_view_ = obs_view_create();
                obs_view_set_source(scene_view_, 0, scene);
                obs_source_inc_active(scene);
                auto scene_video = obs_view_add(scene_view_);
                obs_encoder_set_video(venc, scene_video);
            }
        }

        if (!using_main_audio_encoder_) {
            auto aenc = obs_output_get_audio_encoder(output_, 0);
            if (!aenc) {
                blog(LOG_ERROR, TAG "Prepare output scene before encoder is created.");
                return false;
            }
            obs_encoder_set_audio(aenc, obs_get_audio());

            // Set other audio tracks
            auto audioConfig = FindById(GlobalMultiOutputConfig().audioConfig, config_->audioConfig.value_or(""));
            if (audioConfig) {
                for (auto& track : audioConfig->audioTracks) {
                    auto enc = obs_output_get_audio_encoder(output_, track->output_track);
                    if (enc) {
                        obs_encoder_set_audio(enc, obs_get_audio());
                    }
                }
            }
        }
        
        return true;
    }


    bool ReleaseOutputSceneView() {
        if (!scene_view_)
            return true;

        obs_view_remove(scene_view_);
        OBSSourceAutoRelease source = obs_view_get_source(scene_view_, 0);
        if (source) {
            obs_source_dec_active(source);
        }
        obs_view_set_source(scene_view_, 0, nullptr);
        obs_view_destroy(scene_view_);
        scene_view_ = nullptr;

        return true;
    }


    const char *TikTokFailureText(TikTokFailure failure, bool resolvingCategory)
    {
        switch (failure) {
        case TikTokFailure::Auth:
        case TikTokFailure::Forbidden:
            return obs_module_text("Error.TikTokAuth");
        case TikTokFailure::RateLimited:
            return obs_module_text("Error.TikTokRateLimit");
        case TikTokFailure::Network:
        case TikTokFailure::Timeout:
            return obs_module_text("Error.TikTokNetwork");
        case TikTokFailure::None:
        case TikTokFailure::BadRequest:
        case TikTokFailure::NotFound:
        case TikTokFailure::Server:
        case TikTokFailure::InvalidResponse:
        default:
            return resolvingCategory ? obs_module_text("Error.TikTokCategory")
                                     : obs_module_text("Error.TikTokCreateLive");
        }
    }

    void ShowTikTokError(const char *prefix, const std::string &detail)
    {
        QString msg = QString::fromUtf8(prefix);
        // API details are displayed only, never sent to normal logs.
        if (!detail.empty())
            msg += "\n" + QString::fromUtf8(detail.c_str());
        SetMsg(msg);
    }

    // Ends a LIVE exactly once via a managed cleanup job. liveCounted
    // must be true iff TikTokLiveRegistry::addLive() was called for this
    // id (stored sessions); stranded ids were never counted. Safe from
    // any thread; logout stays blocked until the job finishes, and
    // module unload joins it.
    static void SubmitTikTokCleanup(std::shared_ptr<StreamlabsTikTokClient> client, std::string streamId,
                                    bool liveCounted)
    {
        if (!client || streamId.empty())
            return;
        if (!GlobalCleanupLedger().claim(streamId))
            return;
        auto runEnd = [client, streamId, liveCounted]() {
            TikTokNetworkCancel cleanupScope(nullptr);
            std::string error;
            if (!client->EndStream(streamId, &error))
                blog(LOG_WARNING, TAG "[TikTokStream] Failed to end remote endpoint");
            else
                blog(LOG_INFO, TAG "TikTok LIVE session ended.");
            if (liveCounted)
                TikTokLiveRegistry::removeLive();
        };
        auto job = TikTokWorkerService::instance().submit(
            nullptr,
            [runEnd](const std::shared_ptr<TikTokJobContext> &) { runEnd(); },
            TikTokWorkerService::Done(), true, true);
        if (!job) {
            // Shutdown already joining: finish synchronously in place.
            // Still safe: unload cannot return while this runs.
            runEnd();
        }
    }

    bool CreateTikTokEndpoint()
    {
        if (endpointCreationUncertain_) {
            startRestreamWhenReady_ = false;
            SetMsg(obs_module_text("TikTok.CreationUncertain"));
            return false;
        }
        if (tiktokSession_.HasEndpoint() ||
            tiktokSession_.state == TikTokSessionState::CreatingEndpoint ||
            tiktokSession_.state == TikTokSessionState::EndingEndpoint ||
            tiktokSession_.state == TikTokSessionState::Authenticating)
            return false;
        if (config_->protocol != "RTMP") {
            SetMsg(obs_module_text("Error.TikTokRequiresRTMP"));
            return false;
        }
        auto client = TikTokAuthManager::instance().client();
        if (!client || !TikTokAuthManager::instance().isAuthenticated()) {
            tiktokSession_.state = TikTokSessionState::Authenticating;
            SetMsg(obs_module_text("TikTok.LoggingIn"));
            TikTokAuthManager::instance().login(this);
            return false;
        }
        return BeginEndpointRequest(std::move(client));
    }

    bool BeginEndpointRequest(std::shared_ptr<StreamlabsTikTokClient> client)
    {
        if (tiktokSession_.HasEndpoint() ||
            tiktokSession_.state == TikTokSessionState::CreatingEndpoint ||
            tiktokSession_.state == TikTokSessionState::EndingEndpoint)
            return false;
        const std::string title = config_->tiktok.title;
        const std::string category = config_->tiktok.category;
        const TikTokAudience audience = config_->tiktok.audience;
        struct Prepared {
            TikTokCategoryResult category;
            TikTokStartResult start;
            std::shared_ptr<StreamlabsTikTokClient> client;
            bool adopted = false;
            ~Prepared()
            {
                // Covers destruction/shutdown after Work finishes but
                // before the queued Done callback adopts the endpoint.
                if (start.success && !adopted)
                    SubmitTikTokCleanup(client, start.streamId, false);
            }
        };
        auto outcome = std::make_shared<Prepared>();
        outcome->client = client;
        const auto gen = ++tiktokRequestGen_;
        tiktokSession_.state = TikTokSessionState::CreatingEndpoint;
        SetMsg(obs_module_text("TikTok.Preparing"));
        blog(LOG_INFO, TAG "[TikTokStream] Creating remote endpoint");

        // Managed job: no raw this crosses threads. If the widget dies,
        // the worker finishes safely and ends any LIVE it created; the
        // generation check drops superseded results.
        auto job = TikTokWorkerService::instance().submit(
            this,
            [client, title, category, audience, outcome](const std::shared_ptr<TikTokJobContext> &job) {
                outcome->category = client->ResolveCategory(category);
                if (outcome->category.success && !job->cancelled.load(std::memory_order_relaxed)) {
                    outcome->start =
                        client->StartStream(title, outcome->category.categoryId, audience);
                }
            },
            [client, outcome, gen](const std::shared_ptr<TikTokJobContext> &job) {
                // dynamic_cast: QObject is a virtual base of QWidget, so
                // static_cast cannot downcast here.
                auto *self = dynamic_cast<PushWidgetImpl *>(job->receiver.data());
                if (self == nullptr)
                    return;
                outcome->adopted = true;
                self->OnTikTokPrepared(gen, client, outcome->category, outcome->start);
            },
            true);
        if (!job) {
            blog(LOG_WARNING, TAG "TikTok preparation rejected during shutdown for '%s'.",
                 config_->name.c_str());
            tiktokSession_.state = TikTokSessionState::Error;
            startRestreamWhenReady_ = false;
            return false;
        }
        return true;
    }

    // UI-thread continuation of endpoint creation.
    void OnTikTokPrepared(uint64_t gen, std::shared_ptr<StreamlabsTikTokClient> client,
                          const TikTokCategoryResult &categoryResult, const TikTokStartResult &startResult)
    {
        if (gen != tiktokRequestGen_ ||
            tiktokSession_.state != TikTokSessionState::CreatingEndpoint) {
            if (startResult.success)
                SubmitTikTokCleanup(client, startResult.streamId, false);
            return;
        }

        if (!categoryResult.success) {
            {
                tiktokSession_.state = TikTokSessionState::Error;
                startRestreamWhenReady_ = false;
                endAfterStop_ = false;
                isStarting_ = false;
            }
            switch (ClassifyCredentialResult(categoryResult)) {
            case CredentialValidation::InvalidCredential:
                TikTokAuthManager::instance().handleAuthFailure(client->authEpoch());
                break;
            case CredentialValidation::ValidLiveForbidden:
                TikTokAuthManager::instance().noteLiveAccessDenied(client->authEpoch());
                break;
            default:
                break;
            }
            blog(LOG_WARNING, TAG "TikTok category lookup failed for '%s'.", config_->name.c_str());
            ShowTikTokError(TikTokFailureText(categoryResult.failure, true), categoryResult.error);
            return;
        }
        if (!config_->tiktok.category.empty() && !categoryResult.matchedExact)
            blog(LOG_INFO, TAG "TikTok category '%s' not found, streaming under Other.",
                 config_->tiktok.category.c_str());

        if (!startResult.success) {
            {
                tiktokSession_.state = TikTokSessionState::Error;
                startRestreamWhenReady_ = false;
                endAfterStop_ = false;
                isStarting_ = false;
            }
            switch (ClassifyCredentialResult(startResult)) {
            case CredentialValidation::InvalidCredential:
                TikTokAuthManager::instance().handleAuthFailure(client->authEpoch());
                break;
            case CredentialValidation::ValidLiveForbidden:
                TikTokAuthManager::instance().noteLiveAccessDenied(client->authEpoch());
                break;
            default:
                break;
            }
            endpointCreationUncertain_ = startResult.failure == TikTokFailure::Network ||
                startResult.failure == TikTokFailure::Timeout || startResult.failure == TikTokFailure::Server ||
                startResult.failure == TikTokFailure::InvalidResponse;
            blog(LOG_WARNING, TAG "TikTok LIVE creation failed for '%s'.", config_->name.c_str());
            ShowTikTokError(TikTokFailureText(startResult.failure, false), startResult.error);
            if (endpointCreationUncertain_) SetMsg(obs_module_text("TikTok.CreationUncertain"));
            return;
        }

        tiktokSession_.streamId = startResult.streamId;
        tiktokSession_.server = startResult.server;
        tiktokSession_.streamKey = startResult.key;
        tiktokSession_.client = client;
        tiktokSession_.state = TikTokSessionState::EndpointReady;
        TikTokLiveRegistry::addLive();
        TikTokAuthManager::instance().noteLiveStartSucceeded(client->authEpoch());
        blog(LOG_INFO, TAG "[TikTokStream] Endpoint created");
        SetMsg(obs_module_text("TikTok.EndpointReady"));
        if (endAfterStop_) { endAfterStop_ = false; EndTikTokEndpoint(); return; }
        if (startRestreamWhenReady_) {
            startRestreamWhenReady_ = false;
            StartRestream();
        }
    }

    void EndTikTokEndpoint()
    {
        startRestreamWhenReady_ = false;
        if (restreamState_ != RestreamState::Stopped || IsRunning()) {
            endAfterStop_ = true;
            StopRestream();
            return;
        }
        if (tiktokSession_.state == TikTokSessionState::EndingEndpoint)
            return;
        if (tiktokSession_.state == TikTokSessionState::CreatingEndpoint) {
            // Retain the generation until completion; end the resulting endpoint.
            endAfterStop_ = true;
            return;
        }
        if (!tiktokSession_.HasEndpoint()) {
            if (endpointCreationUncertain_) {
                // There is no stream id to end or query. Let the user
                // resolve the uncertain creation on TikTok before retrying.
                const auto answer = QMessageBox::question(this,
                    obs_module_text("TikTok.EndLive"), obs_module_text("TikTok.ResolveUncertain"));
                if (answer != QMessageBox::Yes) return;
                endpointCreationUncertain_ = false;
            }
            tiktokSession_.Reset();
            SetMsg(obs_module_text("TikTok.EndpointEnded"));
            return;
        }
        tiktokSession_.state = TikTokSessionState::EndingEndpoint;
        SetMsg(obs_module_text("TikTok.EndingEndpoint"));
        blog(LOG_INFO, TAG "[TikTokStream] Ending remote endpoint");
        const auto gen = ++tiktokRequestGen_;
        auto client = tiktokSession_.client;
        auto id = tiktokSession_.streamId;
        struct EndOutcome {
            bool success = false;
            TikTokFailure failure = TikTokFailure::None;
            bool retained = false;
            ~EndOutcome() {
                // If the owner disappears, no retry is possible. Keep
                // registry accounting balanced after the final attempt.
                if (!success && !retained) TikTokLiveRegistry::removeLive();
            }
        };
        auto outcome = std::make_shared<EndOutcome>();
        auto job = TikTokWorkerService::instance().submit(this,
            [client, id, outcome](const std::shared_ptr<TikTokJobContext> &) {
                outcome->success = client->EndStream(id, nullptr, &outcome->failure);
                // A missing remote stream no longer owns a reusable endpoint.
                if (outcome->failure == TikTokFailure::NotFound) outcome->success = true;
                if (outcome->success) TikTokLiveRegistry::removeLive();
                else blog(LOG_WARNING, TAG "[TikTokStream] Remote end failed; retry available while owner exists");
            },
            [gen, client, outcome](const std::shared_ptr<TikTokJobContext> &job) {
                auto *self = dynamic_cast<PushWidgetImpl *>(job->receiver.data());
                if (!self || gen != self->tiktokRequestGen_) return;
                if (outcome->success) {
                    self->tiktokSession_.Reset();
                    self->SetMsg(obs_module_text("TikTok.EndpointEnded"));
                    blog(LOG_INFO, TAG "[TikTokStream] Remote endpoint ended");
                } else {
                    outcome->retained = true;
                    self->tiktokSession_.state = outcome->failure == TikTokFailure::Auth ?
                        TikTokSessionState::Error : TikTokSessionState::EndpointReady;
                    if (outcome->failure == TikTokFailure::Auth)
                        TikTokAuthManager::instance().handleAuthFailure(client->authEpoch());
                    self->SetMsg(obs_module_text("Error.TikTokEndLive"));
                }
            }, true, true);
        if (!job) {
            outcome->retained = true;
            tiktokSession_.state = TikTokSessionState::EndpointReady;
            SetMsg(obs_module_text("Error.TikTokEndLive"));
        }
    }

    void ShutdownTikTokIntegration()
    {
        ++tiktokRequestGen_;
        startRestreamWhenReady_ = false;
        endAfterStop_ = false;
        endpointCreationUncertain_ = false;
        TikTokWorkerService::instance().cancelReceiver(this);
        ReleaseObsOutput();
        if (tiktokSession_.HasEndpoint() &&
            tiktokSession_.state != TikTokSessionState::EndingEndpoint)
            SubmitTikTokCleanup(tiktokSession_.client, tiktokSession_.streamId, true);
        tiktokSession_.Reset();
        QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    }

    std::string VideoEncoderName() {
        return "multi-rtmp-venc" + config_->videoConfig.value_or("");
    }

    std::string AudioEncoderName(int track) {
        return "multi-rtmp-aenc" + config_->audioConfig.value_or("") + "-track-idx-" + std::to_string(track);
    }

    std::optional<std::tuple<int, int>> ParseResolution(const std::optional<std::string>& res) {
        if (!res.has_value())
            return std::nullopt;
        std::regex res_pattern(R"__(\s*(\d{1,5})\s*x\s*(\d{1,5})\s*)__");
        std::smatch match;
        if (std::regex_match(*res, match, res_pattern))
        {
            auto width = std::stoi(match[1].str());
            auto height = std::stoi(match[2].str());
            return {{ width, height }};
        }

        return std::nullopt;
    }

    // Resolves the configured video encoder and returns an OWNING reference
    // (libobs output setters take their own reference from the raw pointer,
    // so callers must pass .Get() without an extra get_ref).
    // Never silently substitutes another encoder: a missing custom config,
    // an invalid resolution/FPS divisor, a failed creation, or an
    // unavailable shared OBS encoder yields null plus lastEncoderError_.
    OBSEncoder GetVideoEncoder() {
        lastEncoderError_ = nullptr;
        auto config_id = config_->videoConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id == "" || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_streaming_output();
            OBSEncoder enc = stream_output ? obs_output_get_video_encoder(stream_output) : nullptr;
            if (!enc) {
                blog(LOG_WARNING, TAG "Shared OBS streaming encoder unavailable for '%s'.",
                     config_->name.c_str());
                lastEncoderError_ = "Error.SharedEncoderUnavailable";
                return {};
            }
            using_main_video_encoder_ = true;
            return enc;
        } else if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_recording_output();
            OBSEncoder enc = stream_output ? obs_output_get_video_encoder(stream_output) : nullptr;
            if (!enc) {
                blog(LOG_WARNING, TAG "Shared OBS recording encoder unavailable for '%s'.",
                     config_->name.c_str());
                lastEncoderError_ = "Error.SharedEncoderUnavailable";
                return {};
            }
            using_main_video_encoder_ = true;
            return enc;
        } else {
            OBSEncoderAutoRelease enc = obs_get_encoder_by_name(VideoEncoderName().c_str());
            if (!enc) {
                auto& global = GlobalMultiOutputConfig();
                auto videoConfig = FindById(global.videoConfig, config_id);
                if (!videoConfig) {
                    blog(LOG_ERROR, TAG "Video encoder config '%s' for '%s' not found.",
                         config_id.c_str(), config_->name.c_str());
                    lastEncoderError_ = "Error.MissingVideoConfig";
                    return {};
                }
                if (videoConfig->resolution.has_value() &&
                    !ParseResolution(videoConfig->resolution).has_value()) {
                    blog(LOG_ERROR, TAG "Invalid video resolution '%s' for '%s'.",
                         videoConfig->resolution->c_str(), config_->name.c_str());
                    lastEncoderError_ = "Error.InvalidVideoConfig";
                    return {};
                }
                if (videoConfig->fpsDenumerator <= 0) {
                    blog(LOG_ERROR, TAG "Invalid video FPS divisor for '%s'.",
                         config_->name.c_str());
                    lastEncoderError_ = "Error.InvalidVideoConfig";
                    return {};
                }
                OBSDataAutoRelease settings = obs_data_create_from_json(videoConfig->encoderParams.dump().c_str());
                enc = obs_video_encoder_create(videoConfig->encoderId.c_str(), VideoEncoderName().c_str(), settings, nullptr);
                if (!enc) {
                    blog(LOG_ERROR, TAG "Failed to create video encoder '%s' for '%s'.",
                         videoConfig->encoderId.c_str(), config_->name.c_str());
                    lastEncoderError_ = "Error.CreateEncoder";
                    return {};
                }
                auto wh = ParseResolution(videoConfig->resolution);
                if (wh.has_value()) {
                    obs_encoder_set_gpu_scale_type(enc, obs_scale_type::OBS_SCALE_BICUBIC);
                    auto [w, h] = *wh;
                    obs_encoder_set_scaled_size(enc, w, h);
                }
                obs_encoder_set_frame_rate_divisor(enc, videoConfig->fpsDenumerator);
            }

            using_main_video_encoder_ = false;
            OBSEncoder owned = enc.Get();
            return owned;
        }
    }

    // Same ownership contract as GetVideoEncoder(). Mixer and output-track
    // indices are validated before creation; out-of-range values yield null
    // plus lastEncoderError_ instead of a misbound encoder.
    OBSEncoder GetAudioEncoder(int trackIdx = 0, std::optional<int> mixerId = std::nullopt) {
        lastEncoderError_ = nullptr;
        auto config_id = config_->audioConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id == "" || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_streaming_output();
            OBSEncoder enc = stream_output ? obs_output_get_audio_encoder(stream_output, 0) : nullptr;
            if (!enc) {
                blog(LOG_WARNING, TAG "Shared OBS streaming audio encoder unavailable for '%s'.",
                     config_->name.c_str());
                lastEncoderError_ = "Error.SharedEncoderUnavailable";
                return {};
            }
            using_main_audio_encoder_ = true;
            return enc;
        } else if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_recording_output();
            OBSEncoder enc = stream_output ? obs_output_get_audio_encoder(stream_output, 0) : nullptr;
            if (!enc) {
                blog(LOG_WARNING, TAG "Shared OBS recording audio encoder unavailable for '%s'.",
                     config_->name.c_str());
                lastEncoderError_ = "Error.SharedEncoderUnavailable";
                return {};
            }
            using_main_audio_encoder_ = true;
            return enc;
        } else {
            OBSEncoderAutoRelease enc = obs_get_encoder_by_name(AudioEncoderName(trackIdx).c_str());
            if (!enc) {
                auto& global = GlobalMultiOutputConfig();
                auto audioConfigId = *config_->audioConfig;
                auto audioConfig = FindById(global.audioConfig, audioConfigId);
                if (!audioConfig) {
                    blog(LOG_ERROR, TAG "Audio encoder config '%s' for '%s' not found.",
                         audioConfigId.c_str(), config_->name.c_str());
                    lastEncoderError_ = "Error.MissingAudioConfig";
                    return {};
                }
                // If we were provided with a mixerId, override the audioConfig's mixerId with it
                int defaultMixerId = audioConfig->mixerId;
                if (auto overrideMixerId = mixerId) {
                    defaultMixerId = *overrideMixerId;
                }
                if (defaultMixerId < 0 || defaultMixerId >= 6 || trackIdx < 0 || trackIdx >= 6) {
                    blog(LOG_ERROR, TAG "Invalid audio mixer/track index for '%s'.",
                         config_->name.c_str());
                    lastEncoderError_ = "Error.InvalidAudioConfig";
                    return {};
                }

                OBSDataAutoRelease settings = obs_data_create_from_json(audioConfig->encoderParams.dump().c_str());
                enc = obs_audio_encoder_create(audioConfig->encoderId.c_str(), AudioEncoderName(trackIdx).c_str(), settings, defaultMixerId, nullptr);
                if (!enc) {
                    blog(LOG_ERROR, TAG "Failed to create audio encoder '%s' for '%s'.",
                         audioConfig->encoderId.c_str(), config_->name.c_str());
                    lastEncoderError_ = "Error.CreateEncoder";
                    return {};
                }
            }

            using_main_audio_encoder_ = false;
            OBSEncoder owned = enc.Get();
            return owned;
        }
    }

    bool PrepareOutputEncoders()
    {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output encoder before output object is created.");
            return false;
        }

        ReleaseOutputEncoder();

        auto& global = GlobalMultiOutputConfig();

        OBSEncoder venc = GetVideoEncoder();
        const char* videoError = lastEncoderError_;
        OBSEncoder aenc = GetAudioEncoder();
        const char* audioError = lastEncoderError_;

        std::vector<std::tuple<int, OBSEncoder>> additionalTracks;
        const char* extraTrackError = nullptr;
        if (auto audioConfigId = config_->audioConfig) {
            auto audioConfig = FindById(global.audioConfig, *audioConfigId);

            if (!audioConfig) {
                blog(LOG_ERROR, TAG "Load audio encoder config failed for %s. Could not determine additional tracks.", config_->name.c_str());
            } else {
                additionalTracks.reserve(audioConfig->audioTracks.size());
                for (auto& track : audioConfig->audioTracks) {
                    OBSEncoder enc = GetAudioEncoder(track->output_track, track->mixer_track);
                    if (enc) {
                        // Record the output track index and the encoder for later when we set the encoders on the output
                        additionalTracks.emplace_back(track->output_track, enc);
                    } else if (!extraTrackError) {
                        // A configured extra track must not be dropped
                        // silently; fail with its specific cause below.
                        extraTrackError = lastEncoderError_ ? lastEncoderError_ : "Error.CreateEncoder";
                    }
                }
            }
        }

        if (!aenc || !venc || extraTrackError) {
            // Never substitute another encoder here. Either the user picked
            // a shared OBS encoder that is unavailable (start OBS streaming
            // or recording first), or the saved custom config is missing or
            // invalid: both are actionable, so report the specific cause.
            ReleaseOutputEncoder();

            const char* key = extraTrackError ? extraTrackError : (!venc ? videoError : audioError);
            if (!key)
                key = "Notice.GetEncoder";
            auto msgbox = new QMessageBox(QMessageBox::Icon::Critical,
                obs_module_text("Notice.Title"),
                obs_module_text(key),
                QMessageBox::StandardButton::Ok,
                this
                );
            msgbox->exec();
            return false;
        }

        // The setters take their own reference; the owning wrappers below
        // release ours at scope end. No extra get_ref: that over-counted by
        // one per encoder on every start.
        obs_output_set_audio_encoder(output_, aenc.Get(), 0);
        for (auto& track : additionalTracks) {
            auto trackIdx = std::get<0>(track);
            auto enc = std::get<1>(track);
            obs_output_set_audio_encoder(output_, enc.Get(), trackIdx);
        }
        obs_output_set_video_encoder(output_, venc.Get());

        return true;
    }


    // libobs can address 6 audio encoder slots per output (see
    // MAX_OUTPUT_AUDIO_ENCODERS in libobs/obs-output.h).
    static constexpr size_t kMaxOutputAudioEncoders = 6;

    bool ReleaseOutputEncoder()
    {
        if (!output_)
            return true;
        else if (obs_output_active(output_) == false)
        {
            // The setters already release the output's own reference when
            // clearing a slot, so no manual release follows: the getters
            // below return borrowed pointers and releasing them would
            // consume someone else's reference.
            auto venc = obs_output_get_video_encoder(output_);
            if (venc)
                obs_output_set_video_encoder(output_, nullptr);

            for (size_t i = 0; i < kMaxOutputAudioEncoders; ++i) {
                auto aenc = obs_output_get_audio_encoder(output_, i);
                if (aenc)
                    obs_output_set_audio_encoder(output_, nullptr, i);
            }

            return true;
        }
        else {
            blog(LOG_ERROR, TAG "Release output while it is active.");
            return false;
        }
    }


    bool ReleaseObsOutput()
    {
        ++outputGeneration_;
        restreamState_ = RestreamState::Stopped;
        if (timer_) timer_->stop();
        if (btn_) {
            btn_->setText(obs_module_text("Btn.Start"));
            btn_->setEnabled(true);
        }
        if (remove_btn_) remove_btn_->setEnabled(true);
        if (output_) {
            DisconnectSignals(output_);
        }

        if (output_ && obs_output_active(output_)) {
            obs_output_force_stop(output_);
        }

        if (output_ && obs_output_active(output_) == false)
        {
            bool ret = ReleaseOutputEncoder();

            // Destroy the output while our service reference is still alive:
            // obs_output_destroy() only clears the service back-pointer and
            // never releases the service itself, so releasing the service
            // first would leave destroy writing to freed memory.
            auto service = obs_output_get_service(output_);
            obs_output_release(output_);
            output_ = nullptr;
            if (service)
                obs_service_release(service);

            ret = ReleaseOutputSceneView() && ret;

            return ret;
        }
        else if (output_) {
            obs_output_release(output_);
            output_ = nullptr;

            return true;
        }
        else if (output_ == nullptr)
            return true;
        else
            return false;
    }


    void UpdateStreamStatus() {
        using namespace std::chrono;

        if (!output_)
            return;

        static const char* units[] = {
            "bps", "Kbps", "Mbps", "Gbps", "Tbps", "Pbps", "Ebps", "Zbps", "Ybps"
        };

        auto new_bytes = obs_output_get_total_bytes(output_);
        auto new_frames = obs_output_get_total_frames(output_);
        auto now = clock::now();

        auto interval = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_info_time_).count();
        if (interval > 0)
        {
            auto duration = now - begin_time_;
            auto hh = duration_cast<hours>(duration);
            duration -= hh;
            auto mm = duration_cast<minutes>(duration);
            duration -= mm;
            auto ss = duration_cast<seconds>(duration);
            duration -= ss;

            char strDuration[64] = { 0 };
            snprintf(strDuration, sizeof(strDuration), "%02d:%02d:%02d", (int)hh.count(), (int)mm.count(), (int)ss.count());

            char strFps[32] = { 0 };
            snprintf(strFps, sizeof(strFps), "%d FPS", static_cast<int>(std::round((new_frames - total_frames_) / interval)));

            auto bps = (new_bytes - total_bytes_) * 8 / interval;
            auto strBps = [&]()-> std::string {
                if (bps > 0)
                {
                    int unitMaxIndex = sizeof(units) / sizeof(*units);
                    int unitIndex = static_cast<int>(log10(bps) / 3);
                    if (unitIndex >= unitMaxIndex)
                        unitIndex = unitMaxIndex - 1;
                    auto strVal = std::to_string(bps / pow(1000, unitIndex)).substr(0, 4);
                    if (!strVal.empty() && strVal.back() == '.')
                        strVal.pop_back();
                    return strVal + " " + units[unitIndex];
                }
                else
                {
                    return "0 bps";
                }
            }();
            
            msg_->setText((std::string(strDuration) + "  " + strBps + "  " + strFps).c_str());
        }

        total_frames_ = new_frames;
        total_bytes_ = new_bytes;
        last_info_time_ = now;
    }

    void UpdateStatusDot() {
        if (!status_dot_)
            return;

        const char* color = "#888888"; // stopped / idle
        if (isReconnecting_ || isConnecting_)
            color = "#e6b800"; // connecting / reconnecting
        else if (IsRunning())
            color = "#2ecc71"; // live
        else if (lastErrorCode_ != 0)
            color = "#e74c3c"; // last attempt failed

        status_dot_->setStyleSheet(QString("color: %1;").arg(color));
    }

public:
    PushWidgetImpl(const std::string& targetid, QWidget* parent = 0)
        : QWidget(parent)
        , targetid_(targetid)
    {
        QObject::setObjectName("push-widget");

        auto& global = GlobalMultiOutputConfig();
        config_ = FindById(global.targets, targetid_);
        if (!config_)
            return;

        timer_ = new QTimer(this);
        timer_->setInterval(std::chrono::milliseconds(1000));
        QObject::connect(timer_, &QTimer::timeout, [this]() {
            UpdateStreamStatus();
        });

        auto layout = new QGridLayout(this);
        layout->addWidget(status_dot_ = new QLabel(u8"●", this), 0, 0);
        layout->addWidget(name_ = new QLabel(obs_module_text("NewStreaming"), this), 0, 1, 1, 3);

        layout->addWidget(enabled_cb_ = new QCheckBox(this), 1, 0);
        enabled_cb_->setToolTip(obs_module_text("Target.Enabled"));
        QObject::connect(enabled_cb_, &QCheckBox::clicked, [this](bool checked) {
            SetEnabled(checked);
        });

        layout->addWidget(btn_ = new QPushButton(obs_module_text("Btn.Start"), this), 1, 1);
        QObject::connect(btn_, &QPushButton::clicked, [this]() {
            StartStop();
        });

        layout->addWidget(edit_btn_ = new QPushButton(obs_module_text("Btn.Edit"), this), 1, 2);
        QObject::connect(edit_btn_, &QPushButton::clicked, [this]() {
            ShowEditDlg();
        });

        layout->addWidget(remove_btn_ = new QPushButton(obs_module_text("Btn.Delete"), this), 1, 3);

        // Keep action buttons usable in narrow docks across OBS themes.
        for (auto button : { btn_, edit_btn_, remove_btn_ }) {
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            button->setMinimumWidth(0);
            button->setMinimumHeight(button->fontMetrics().height() + 8);
        }
        layout->setColumnStretch(1, 1);
        layout->setColumnStretch(2, 1);
        layout->setColumnStretch(3, 1);

        layout->addWidget(msg_ = new QLabel(u8"", this), 2, 0, 1, 4);
        msg_->setWordWrap(true);
        create_live_btn_ = new QPushButton(obs_module_text("TikTok.CreateEndpoint"), this);
        end_live_btn_ = new QPushButton(obs_module_text("TikTok.EndLive"), this);
        layout->addWidget(create_live_btn_, 3, 0, 1, 2);
        layout->addWidget(end_live_btn_, 3, 2);
        QObject::connect(create_live_btn_, &QPushButton::clicked, this, [this]() { CreateTikTokEndpoint(); });
        QObject::connect(end_live_btn_, &QPushButton::clicked, this, [this]() { EndTikTokEndpoint(); });
        auto &auth = TikTokAuthManager::instance();
        QObject::connect(&auth, &TikTokAuthManager::stateChanged, this, [this]() {
            if (tiktokSession_.HasEndpoint() &&
                tiktokSession_.state != TikTokSessionState::EndingEndpoint &&
                TikTokAuthManager::instance().status() == TikTokAuthManager::Status::SessionExpired) {
                tiktokSession_.state = TikTokSessionState::Error;
                StopRestream();
                SetMsg(obs_module_text("TikTok.SessionExpired"));
            }
        });
        QObject::connect(&auth, &TikTokAuthManager::loginSucceeded, this, [this]() {
            if (tiktokSession_.state == TikTokSessionState::Authenticating) {
                tiktokSession_.state = TikTokSessionState::Idle;
                CreateTikTokEndpoint();
            }
        });
        QObject::connect(&auth, &TikTokAuthManager::loginFailed, this, [this](const QString &error) {
            if (tiktokSession_.state == TikTokSessionState::Authenticating) {
                tiktokSession_.state = TikTokSessionState::Error;
                startRestreamWhenReady_ = false;
                isStarting_ = false;
                SetMsg(error);
            }
        });
        layout->addItem(new QSpacerItem(0, 10), 4, 0);
        setLayout(layout);

        LoadConfig();
        UpdateStatusDot();

        auto startDesc = std::string(obs_module_text("Hotkey.StartTarget")) + " - " + config_->name;
        startHotkeyId_ = obs_hotkey_register_frontend(
            ("obs-multi-rtmp.start." + targetid_).c_str(),
            startDesc.c_str(),
            [](void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
                if (!pressed)
                    return;
                static_cast<PushWidgetImpl*>(data)->StartStreaming();
            },
            this
        );

        auto stopDesc = std::string(obs_module_text("Hotkey.StopTarget")) + " - " + config_->name;
        stopHotkeyId_ = obs_hotkey_register_frontend(
            ("obs-multi-rtmp.stop." + targetid_).c_str(),
            stopDesc.c_str(),
            [](void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
                if (!pressed)
                    return;
                static_cast<PushWidgetImpl*>(data)->StopStreaming();
            },
            this
        );
    }

    ~PushWidgetImpl()
    {
        // Orphan in-flight jobs before teardown: their completions are
        // dropped while their workers finish safely on pool threads.
        // ReleaseObsOutput() runs inside ShutdownTikTokIntegration().
        ShutdownTikTokIntegration();
        if (startHotkeyId_ != OBS_INVALID_HOTKEY_ID)
            obs_hotkey_unregister(startHotkeyId_);
        if (stopHotkeyId_ != OBS_INVALID_HOTKEY_ID)
            obs_hotkey_unregister(stopHotkeyId_);
    }

    void StartStreaming() override {
        if (!config_->enabled) {
            SetMsg(obs_module_text("Status.Disabled"));
            return;
        }
        // Re-entry guard (upstream crash fix): StartStreaming() re-entry
        // during connection used to crash. The flag stays set across the
        // async TikTok endpoint creation below; terminal states (OnStarted,
        // OnStopped, endpoint/auth failure, RestreamFailed) clear it.
        bool expected = false;
        if (IsRunning() || !isStarting_.compare_exchange_strong(expected, true))
            return;
        auto resetStarting = finally([this] { isStarting_ = false; });

        if (endAfterStop_ || restreamState_ != RestreamState::Stopped) return;
        if (config_->tiktok.enabled && !tiktokSession_.HasEndpoint()) {
            startRestreamWhenReady_ = true;
            CreateTikTokEndpoint();
            // Keep the guard only while async work is actually in flight
            // (endpoint creation or login); otherwise release it.
            if (tiktokSession_.state == TikTokSessionState::CreatingEndpoint ||
                tiktokSession_.state == TikTokSessionState::Authenticating)
                resetStarting.Dismiss();
            else
                startRestreamWhenReady_ = false;
            return;
        }
        if (StartRestream())
            resetStarting.Dismiss();
    }

    // Returns true when an async OBS start is in flight (OnStarted/
    // OnStopped will clear the starting guard); false otherwise.
    bool StartRestream() {
        auto clearStarting = finally([this] { isStarting_ = false; });
        if (IsRunning())
            return false;

        if (restreamState_ != RestreamState::Stopped) return false;
        if (config_->tiktok.enabled && (!tiktokSession_.HasEndpoint() ||
            tiktokSession_.state != TikTokSessionState::EndpointReady)) return false;
        ReleaseObsOutput();
        restreamState_ = RestreamState::Preparing;
        if (config_->tiktok.enabled) SetMsg(obs_module_text("TikTok.StartingRestream"));
        if (config_->tiktok.enabled)
            blog(LOG_INFO, TAG "[TikTokStream] OBS output preparation started");

        if (!CreateObsOutput()) {
            RestreamFailed("Error.StartOutput");
            return false;
        }

        if (!PrepareOutputService(tiktokSession_.server, tiktokSession_.streamKey))
        {
            RestreamFailed("Error.CreateRtmpService");
            return false;
        }

        if (!PrepareOutputEncoders())
        {
            RestreamFailed("Error.CreateEncoder");
            return false;
        }

        if (!PrepareEncoderSource())
        {
            RestreamFailed("Error.SceneNotExist");
            return false;
        }

        restreamState_ = RestreamState::Connecting;
        if (!obs_output_start(output_)) {
            RestreamFailed("Error.StartOutput");
            return false;
        }
        // obs_output_start() is asynchronous: keep the starting guard set
        // until OnStarted()/OnStopped() clears it.
        clearStarting.Dismiss();
        return true;
    }

    bool CreateObsOutput()
    {
        if (output_ == nullptr)
        {
            obs_data* output_settings = obs_data_create_from_json(config_->outputParam.dump().c_str());

            auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
            assert(protocolInfo);
            if (!protocolInfo) {
	        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
	        	protocolInfo = GetProtocolInfos()->GetList();
	        }
            auto output_id = protocolInfo->outputId;

            blog(LOG_DEBUG, "Streaming to output: %s", output_id);

            output_ = obs_output_create(output_id, "multi-output", output_settings, nullptr);
            obs_data_release(output_settings);
            if (!output_) {
                return false;
            }
            SetMeAsHandler(output_);
        }

        if (output_) {
            isUseDelay_ = false;

            auto profileConfig = obs_frontend_get_profile_config();
            if (profileConfig) {
                bool useDelay = config_get_bool(profileConfig, "Output", "DelayEnable");
                bool preserveDelay = config_get_bool(profileConfig, "Output", "DelayPreserve");
                int delaySec = config_get_int(profileConfig, "Output", "DelaySec");
                obs_output_set_delay(output_,
                    useDelay ? delaySec : 0,
                    preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0
                );

                if (useDelay && delaySec > 0)
                    isUseDelay_ = true;
            }
        }

        return output_ != nullptr;
    }

    // Reads the output's last error BEFORE the output is destroyed.
    // obs_output_get_last_error() also falls back to encoder errors, so a
    // failed obs_output_start() reports the encoder's cause here.
    std::string TakeOutputLastError()
    {
        if (!output_)
            return {};
        const char* err = obs_output_get_last_error(output_);
        return err ? err : "";
    }

    void RestreamFailed(const char *message)
    {
        const std::string detail = TakeOutputLastError();
        ReleaseObsOutput();
        blog(LOG_WARNING, TAG "Local output failed for '%s' (%s)%s%s", config_->name.c_str(), message,
             detail.empty() ? "" : ": ", detail.empty() ? "" : detail.c_str());
        if (tiktokSession_.HasEndpoint())
            blog(LOG_WARNING, TAG "[TikTokStream] Local failure (%s); endpoint preserved", message);
        QString msg = QString::fromUtf8(obs_module_text(message));
        if (!detail.empty())
            msg += "\n" + QString::fromUtf8(detail.c_str());
        if (tiktokSession_.HasEndpoint())
            msg += "\n" + QString::fromUtf8(obs_module_text("TikTok.EndpointReady"));
        SetMsg(msg);
    }

    void StopStreaming() override { StopRestream(); }

    void StopRestream()
    {
        startRestreamWhenReady_ = false;
        // A deferred start (endpoint creation/login in flight) is cancelled
        // here; release the starting guard so a later Start can proceed.
        // An already-created endpoint is preserved (see OnTikTokPrepared).
        isStarting_ = false;
        if (!IsRunning()) {
            ReleaseObsOutput();
            if (tiktokSession_.HasEndpoint())
                SetMsg(obs_module_text(tiktokSession_.state == TikTokSessionState::Error ?
                    "TikTok.SessionExpired" : "TikTok.RestreamStopped"));
            if (endAfterStop_) { endAfterStop_ = false; EndTikTokEndpoint(); }
            return;
        }
        if (restreamState_ == RestreamState::Stopping) return;
        bool useForce = false;
        if (isUseDelay_) {
            const auto res = QMessageBox(QMessageBox::Icon::Information, "?",
                obs_module_text("Ques.DropDelay"),
                QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No, this).exec();
            useForce = res == QMessageBox::Yes;
        }
        // A delayed output can finish while the confirmation dialog is open.
        if (!IsRunning()) { StopRestream(); return; }
        restreamState_ = RestreamState::Stopping;
        if (useForce) obs_output_force_stop(output_);
        else obs_output_stop(output_);
    }

    void OnOBSEvent(obs_frontend_event ev) override
    {
        if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_EXIT
            || ev == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_CHANGED
        ) {
            Stop();
        } else if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED) {
            StopRestream();
        } else if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_STREAMING_STARTING) {
            if (!IsRunning() && config_->syncStart) {
                StartStop();
            }
        } else if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
            if (config_->syncStop && (IsRunning() || restreamState_ != RestreamState::Stopped ||
                                      startRestreamWhenReady_)) {
                StopStreaming();
            }
        }
    }

    void LoadConfig()
    {
        name_->setText(QString::fromUtf8(config_->name));
        create_live_btn_->setVisible(config_->tiktok.enabled);
        end_live_btn_->setVisible(config_->tiktok.enabled);
        UpdateTikTokActions();
        if (enabled_cb_)
            enabled_cb_->setChecked(config_->enabled);
    }

    void ResetInfo()
    {
        total_frames_ = 0;
        total_bytes_ = 0;
        last_info_time_ = clock::now();
        msg_->setText("");
    }

    bool IsRunning() override
    {
        return output_ != nullptr && obs_output_active(output_);
    }

    bool GetEnabled() override { return config_->enabled; }

    void SetEnabled(bool enabled) override
    {
        config_->enabled = enabled;
        if (enabled_cb_ && enabled_cb_->isChecked() != enabled)
            enabled_cb_->setChecked(enabled);
        // Disabling must also cancel a deferred start: otherwise a pending
        // endpoint creation could start a now-disabled target.
        if (!enabled)
            startRestreamWhenReady_ = false;
        if (!enabled && IsRunning())
            obs_output_force_stop(output_);
        SaveMultiOutputConfig();
        UpdateStatusDot();
    }

    void StartStop()
    {
        if (IsRunning() || restreamState_ != RestreamState::Stopped)
        {
            StopStreaming();
            return;
        }

        StartStreaming();
    }

    void Stop() { ShutdownTikTokIntegration(); }

    bool ShowEditDlg() override
    {
        std::unique_ptr<EditOutputWidget> dlg{ createEditOutputWidget(targetid_, (QMainWindow*)obs_frontend_get_main_window()) };

        if (dlg->exec() == QDialog::DialogCode::Accepted)
        {
            if (!config_->tiktok.enabled && tiktokSession_.HasEndpoint()) EndTikTokEndpoint();
            SaveMultiOutputConfig();
            LoadConfig();
            return true;
        }
        else
            return false;
    }

    void SetMsg(QString msg)
    {
        UpdateTikTokActions();
        msg_->setText(msg);
        msg_->setToolTip(msg);
    }

    void UpdateTikTokActions()
    {
        if (!create_live_btn_ || !config_->tiktok.enabled) return;
        const auto state = tiktokSession_.state;
        const bool busy = state == TikTokSessionState::Authenticating ||
            state == TikTokSessionState::CreatingEndpoint || state == TikTokSessionState::EndingEndpoint;
        edit_btn_->setEnabled(!busy);
        create_live_btn_->setEnabled(!busy && !tiktokSession_.HasEndpoint() && !endpointCreationUncertain_);
        end_live_btn_->setEnabled(state != TikTokSessionState::EndingEndpoint &&
            (tiktokSession_.HasEndpoint() || state == TikTokSessionState::CreatingEndpoint || endpointCreationUncertain_));
        btn_->setEnabled(!busy);
    }

    void QueueOutputUI(std::function<void()> callback)
    {
        QPointer<QObject> receiver(this);
        const auto generation = outputGeneration_.load();
        QMetaObject::invokeMethod(this, [receiver, generation, callback = std::move(callback)]() {
            auto *self = dynamic_cast<PushWidgetImpl *>(receiver.data());
            if (self && generation == self->outputGeneration_.load()) callback();
        }, Qt::QueuedConnection);
    }

    // obs logical
    void OnStarting() override
    {
        QueueOutputUI([this]() {
            restreamState_ = RestreamState::Connecting;
            begin_time_ = clock::now();
            isConnecting_ = true;
            isReconnecting_ = false;
            lastErrorCode_ = 0;
            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Connecting"));
            remove_btn_->setEnabled(false);
            UpdateStatusDot();
        });
    }

    void OnStarted() override
    {
        // QueueOutputUI == RunInUIThread plus the output-generation guard
        // that drops stale callbacks from replaced outputs.
        QueueOutputUI([this]() {
            isStarting_ = false;
            isConnecting_ = false;
            isReconnecting_ = false;
            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Streaming"));

            restreamState_ = RestreamState::Streaming;
            if (config_->tiktok.enabled) blog(LOG_INFO, TAG "[TikTokStream] OBS output started");
            ResetInfo();
            timer_->start();
            UpdateStatusDot();
        });
    }

    void OnReconnect() override
    {
        QueueOutputUI([this]() {
            timer_->stop();
            isReconnecting_ = true;

            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Reconnecting"));
            UpdateStatusDot();
        });
    }

    void OnReconnected() override
    {
        QueueOutputUI([this]() {
            isReconnecting_ = false;
            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Streaming"));

            restreamState_ = RestreamState::Streaming;
            if (config_->tiktok.enabled) blog(LOG_INFO, TAG "[TikTokStream] OBS output started");
            ResetInfo();
            timer_->start();
            UpdateStatusDot();
        });
    }

    void OnStopping() override
    {
        QueueOutputUI([this]() {
            timer_->stop();

            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Stopping"));
        });
    }

    void OnStopped(int code) override
    {
        QueueOutputUI([this, code]() {
            isStarting_ = false;
            isConnecting_ = false;
            isReconnecting_ = false;
            lastErrorCode_ = code;
            // Capture before ReleaseObsOutput() destroys the output.
            const std::string detail = TakeOutputLastError();
            ReleaseObsOutput();
            if (tiktokSession_.HasEndpoint())
                blog(LOG_INFO, TAG "[TikTokStream] OBS output stopped; endpoint preserved");
            if (code != 0 && !detail.empty())
                blog(LOG_WARNING, TAG "Output '%s' stopped with code %d: %s",
                     config_->name.c_str(), code, detail.c_str());
            ResetInfo();
            timer_->stop();

            remove_btn_->setEnabled(true);
            btn_->setText(obs_module_text("Btn.Start"));
            btn_->setEnabled(true);
            SetMsg(u8"");

            switch(code)
            {
                case 0:
                    if (tiktokSession_.HasEndpoint())
                        SetMsg(obs_module_text("TikTok.RestreamStopped"));
                    break;
                case -1:
                    SetMsg(obs_module_text("Error.WrongRTMPUrl"));
                    break;
                case -2:
                    SetMsg(obs_module_text("Error.ServerConnect"));
                    break;
                case -3:
                    SetMsg(obs_module_text("Error.ServerHandshake"));
                    break;
                case -4:
                    SetMsg(obs_module_text("Error.ServerRefuse"));
                    break;
                default:
                    SetMsg(obs_module_text("Error.Unknown"));
                    break;
            }
            // Surface the concrete libobs/encoder cause when one was
            // recorded; the message above stays as the category.
            if (code != 0 && !detail.empty())
                SetMsg(msg_->text() + "\n" + QString::fromUtf8(detail.c_str()));
            if (code != 0 && tiktokSession_.state == TikTokSessionState::EndpointReady)
                SetMsg(msg_->text() + "\n" + QString::fromUtf8(obs_module_text("TikTok.EndpointReady")));
            if (tiktokSession_.state == TikTokSessionState::Error)
                SetMsg(obs_module_text("TikTok.SessionExpired"));
            if (endAfterStop_) { endAfterStop_ = false; EndTikTokEndpoint(); }

            UpdateStatusDot();
        });
    }
};

PushWidget* createPushWidget(const std::string& targetid, QWidget* parent) {
    return new PushWidgetImpl(targetid, parent);
}
