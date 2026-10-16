#include "edit-widget.h"
#include "output-config.h"
#include "json-util.hpp"
#include "obs.hpp"
#include "streamlabs-tiktok.h"
#include "tiktok/auth-manager.h"
#include "tiktok/streamlabs-client.h"
#include "tiktok/worker.h"
#include <QMenu>
#include <QTabWidget>
#include <qevent.h>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSettings>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>

#include <regex>

#include "obs-properties-widget.h"
#include "helpers.h"
#include "protocols.h"

static std::optional<int> ParseStringToInt(const QString& str) {
    try {
        return std::stoi(tostdu8(str));
    }
    catch(...)
    {
        return {};
    }
}


static nlohmann::json to_json(obs_data* data) {
    if (!data)
        return {};
    auto jsonstr = obs_data_get_json(data);
    if (!jsonstr)
        return {};
    try {
        return nlohmann::json::parse(jsonstr);
    }
    catch (...) {
        return {};
    }
}

static OBSData from_json(nlohmann::json j) {
    if (j.type() == nlohmann::json::value_t::null)
        return OBSDataAutoRelease(obs_data_create()).Get();
    
    auto jstr = j.dump();
    OBSData r = obs_data_create_from_json(jstr.c_str());
    if (!r)
        return {};
    obs_data_release(r);
    return r;
}

static obs_properties* AddBF(obs_properties* p) {
    auto bfp = obs_properties_get(p, "bf");
    if (!bfp) {
        bfp = obs_properties_get(p, "bframes");
        if (!bfp)
            obs_properties_add_int(p, "bf", obs_module_text("BFrames"), 0, 16, 1);
    }
    return p;
}


class EditOutputWidgetImpl: public EditOutputWidget
{
    // Stable-size dialog: per-tab scrolling, no content-driven top-level
    // resize. Each tab page is wrapped in its own QScrollArea with
    // widgetResizable=true; the top-level QTabWidget + QDialogButtonBox
    // keep a fixed initial size clamped to the screen.
    QTabWidget* topTabs_ = nullptr;
    QTabWidget* outputSettingsTabs_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;

    std::string targetid_;
    OutputTargetConfigPtr config_ = nullptr;
    // Transactional draft: every widget edits this copy plus the staged
    // encoder configs below. GlobalMultiOutputConfig() is untouched until
    // Apply/OK commits; Cancel/Escape discards everything.
    std::list<VideoEncoderConfigPtr> draftVideos_;
    std::list<AudioEncoderConfigPtr> draftAudios_;
    // True when the dialog opened a target with no saved encoder choice at
    // all (i.e. a brand-new target). Only such targets get the independent
    // TikTok preset; existing saved choices are never rewritten here.
    bool freshTarget_ = false;
    bool tiktokPresetApplied_ = false;

    QLineEdit* name_ = 0;

    class PropertiesWidget: public QWidget {
    public:
        PropertiesWidget(std::function<void()> onContentSizeChanged, QWidget* parent)
            : QWidget(parent)
            , onContentSizeChanged_(std::move(onContentSizeChanged))
        {
            layout_ = new QGridLayout(this);
            layout_->setColumnStretch(0, 1);
            layout_->setContentsMargins(0, 0, 0, 0);
            setLayout(layout_);
        }

        // owner of props and settings is transfered
        void UpdateProperties(obs_properties* props, obs_data* settings) {
            ResetWid();
            wid_ = createPropertyWidget(props, settings_ = settings, this);
            layout_->addWidget(wid_, 0, 0, 1, 1, Qt::AlignTop);
            obs_data_release(settings);
        }

        void ClearProperties() {
            ResetWid();
            placeholder_ = new QWidget(this);
            layout_->addWidget(placeholder_, 0, 0, 1, 1);
        }

        nlohmann::json Save() {
            if (!wid_ || !settings_)
                return {};
            wid_->Save();
            return to_json(settings_);
        }

    private:
        QGridLayout* layout_ = nullptr;
        QPropertiesWidget* wid_ = nullptr;
        QWidget* placeholder_ = nullptr;
        OBSData settings_;
        std::function<void()> onContentSizeChanged_;

        void ResetWid() {
            if (wid_) {
                delete wid_;
                wid_ = nullptr;
            }
            if (placeholder_) {
                delete placeholder_;
                placeholder_ = nullptr;
            }
        }
    };

    PropertiesWidget* serviceSettings_;
    PropertiesWidget* outputSettings_;
    std::string supported_video_encoders_;
    std::string supported_audio_encoders_;
    PropertiesWidget* videoEncoderSettings_;
    PropertiesWidget* audioEncoderSettings_;
   
    QComboBox* protocolSelector_ = 0;
    QComboBox* videoMode_ = 0;
    QComboBox* venc_ = 0;
    QComboBox* v_scene_ = 0;
    QLineEdit* v_resolution_ = 0;
    QComboBox* v_fpsdenumerator_ = 0;
    QLabel* v_share_notify_ = 0;
    QLabel* videoStatus_ = 0;
    QLabel* videoHint_ = 0;
    QWidget* videoCustomBox_ = 0;

    QComboBox* audioMode_ = 0;
    QComboBox* aenc_ = 0;
    QComboBox* a_mixer_ = 0;
    QComboBox* a_vod_track_ = nullptr;
    QLabel* a_share_notify_ = 0;
    QLabel* audioStatus_ = 0;
    QLabel* audioHint_ = 0;
    QWidget* audioCustomBox_ = 0;

    QCheckBox* syncStart_ = 0;
    QCheckBox *syncStop_ = 0;

    QCheckBox* tiktokEnable_ = 0;
    QLabel* tiktokAuthStatus_ = 0;
    QPushButton* tiktokLoginButton_ = 0;
    QPushButton* tiktokLogoutButton_ = 0;
    QPushButton* tiktokApplyButton_ = 0;
    QLineEdit* tiktokTitle_ = 0;
    QComboBox* tiktokCategory_ = 0;
    QComboBox* tiktokAudience_ = 0;
    QTimer* tiktokCategoryDebounce_ = 0;
    uint64_t tiktokCategoryGen_ = 0;

    std::vector<std::string> EnumEncodersByCodec(const char* codec)
    {
        if (!codec)
            return {};

        std::vector<std::string> result;
        int i = 0;
        for(;;)
        {
            const char* encid;
            if (!obs_enum_encoder_types(i++, &encid))
                break;
            auto caps = obs_get_encoder_caps(encid);
            if (caps & OBS_ENCODER_CAP_DEPRECATED)
                continue;
            auto enc_codec = obs_get_encoder_codec(encid);
            if (strcmp(enc_codec, codec) == 0)
                result.emplace_back(encid);
        }
        return result;
    }


    // Draft-scoped duplication: the clone lives in the dialog's staged
    // lists and reaches GlobalMultiOutputConfig() only on Apply/OK, so
    // Cancel leaves every other destination untouched.
    template<class T>
    static std::optional<std::string> DuplicateConfig(std::list<T>& list, std::string oldid) {
        auto cfg = FindById(list, oldid);
        if (!cfg) {
            return {};
        }
        auto& global = GlobalMultiOutputConfig();
        auto newid = GenerateId(global);
        // Avoid colliding with ids already staged in this dialog.
        for (bool clash = true; clash;) {
            clash = false;
            for (auto &x : list) {
                if (x->id == newid) {
                    newid = GenerateId(global);
                    clash = true;
                    break;
                }
            }
        }
        auto newcfg = std::make_shared<typename T::element_type>(*cfg);
        newcfg->id = newid;
        list.push_back(newcfg);
        return newid;
    }


    // "Share from another destination" clones or reuses a *plugin config*
    // id. This is distinct from sharing a live OBS encoder instance. All
    // work happens on the dialog draft; Apply/OK commits, Cancel discards.
    void PopupShareMenu(bool isAudio) {
        auto menu = new QMenu(this);
        auto& global = GlobalMultiOutputConfig();

        {
            auto action = menu->addAction(obs_module_text("NoShare"));
            QObject::connect(action, &QAction::triggered, [=]() {
                if (isAudio) {
                    if (!config_->audioConfig.has_value() ||
                        IsSpecialEncoder(*config_->audioConfig))
                        return;
                    auto newid = DuplicateConfig(draftAudios_, *config_->audioConfig);
                    if (!newid.has_value())
                        return;
                    config_->audioConfig = *newid;
                }
                else {
                    if (!config_->videoConfig.has_value() ||
                        IsSpecialEncoder(*config_->videoConfig))
                        return;
                    auto newid = DuplicateConfig(draftVideos_, *config_->videoConfig);
                    if (!newid.has_value())
                        return;
                    config_->videoConfig = *newid;
                }
                LoadConfig();
                UpdateUI();
            });
        }

        std::unordered_map<std::string, std::string> items;
        for(auto& target: global.targets) {
            auto consider = [&](const std::optional<std::string> &id) {
                if (!id.has_value() || IsSpecialEncoder(*id))
                    return;
                // Staged draft configs are committable ids too.
                auto it = items.find(*id);
                if (it != items.end())
                    it->second += ", " + target->name;
                else
                    items.insert(std::make_pair(*id, target->name));
            };
            if (isAudio)
                consider(target->id == config_->id ? config_->audioConfig : target->audioConfig);
            else
                consider(target->id == config_->id ? config_->videoConfig : target->videoConfig);
        }

        for (auto& item : items) {
            auto action = menu->addAction(QString::fromUtf8(item.second));
            QObject::connect(action, &QAction::triggered, [=]() {
                if (isAudio)
                    config_->audioConfig = item.first;
                else
                    config_->videoConfig = item.first;
                LoadConfig();
                UpdateUI();
            });
        }

        menu->exec(QCursor::pos());
    }

    // No content-driven top-level resize: the dialog keeps a stable size.
    void ScheduleResizeToContent() {}

    QScrollArea *WrapInScroll(QWidget *page)
    {
        auto scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        scroll->setWidget(page);
        return scroll;
    }

    QTabWidget* CreateOutputSettingsWidget(QWidget* parent) {
        auto tab = outputSettingsTabs_ = new QTabWidget(parent);

        // service
        {
            serviceSettings_ = new PropertiesWidget([]() {}, tab);
            updateServiceTab();
            tab->addTab(serviceSettings_, obs_module_text("Tab.Service"));
        }

        // output
        {
            outputSettings_ = new PropertiesWidget([]() {}, tab);
            updateOutputTab();
            tab->addTab(outputSettings_, obs_module_text("Tab.Output"));
        }

        tab->setCurrentIndex(0);

        return tab;
    }

    void updateServiceTab()
    {
        auto protocol_info = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        assert(protocol_info);
        if (!protocol_info) {
        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
            protocol_info = GetProtocolInfos()->GetList();
        }

        auto service = obs_service_create(protocol_info->serviceId, ("tmp_service_" + targetid_).c_str(), from_json(config_->serviceParam), nullptr);
        serviceSettings_->UpdateProperties(
            obs_service_properties(service),
            obs_service_get_settings(service));
        obs_service_release(service);
    }

    void updateOutputTab()
    {
        auto protocol_info = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        assert(protocol_info);
        if (!protocol_info) {
        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
            protocol_info = GetProtocolInfos()->GetList();
        }

        auto output = obs_output_create(protocol_info->outputId, ("tmp_output_" + targetid_).c_str(), from_json(config_->outputParam), nullptr);
        outputSettings_->UpdateProperties(
            obs_output_properties(output),
            obs_output_get_settings(output));
        supported_audio_encoders_ = obs_output_get_supported_audio_codecs(output);
        supported_video_encoders_ = obs_output_get_supported_video_codecs(output);
        obs_output_release(output);

        if (aenc_ && venc_)
            LoadEncoders();
    }

public:
    ~EditOutputWidgetImpl() override
    {
        // Orphan in-flight category searches: their completions are
        // dropped while workers finish harmlessly on pool threads.
        TikTokWorkerService::instance().cancelReceiver(this);
    }

    void FillModeBox(QComboBox *box)
    {
        box->clear();
        box->addItem(obs_module_text("Encoder.ShareStreaming"), OBS_STREAMING_ENC_PLACEHOLDER);
        box->addItem(obs_module_text("Encoder.ShareRecording"), OBS_RECORDING_ENC_PLACEHOLDER);
        box->addItem(obs_module_text("Encoder.CopyStreaming"), OBS_STREAMING_COPY_PLACEHOLDER);
        box->addItem(obs_module_text("Encoder.CopyRecording"), OBS_RECORDING_COPY_PLACEHOLDER);
        box->addItem(obs_module_text("Encoder.Custom"), "custom");
    }

    // Atomically commit the draft target + staged encoder configs.
    bool CommitDraft()
    {
        SaveConfig();
        auto &global = GlobalMultiOutputConfig();
        auto it = FindById(global.targets, config_->id);
        if (it == nullptr)
            return false;
        *it = *config_;
        // Upsert staged encoder configs; never touch unrelated ids.
        for (auto &staged : draftVideos_) {
            auto cur = FindById(global.videoConfig, staged->id);
            if (cur)
                *cur = *staged;
            else
                global.videoConfig.push_back(staged);
        }
        for (auto &staged : draftAudios_) {
            auto cur = FindById(global.audioConfig, staged->id);
            if (cur)
                *cur = *staged;
            else
                global.audioConfig.push_back(staged);
        }
        SaveMultiOutputConfig();
        return true;
    }

    void ApplyStableDialogSize()
    {
        QSize initial(820, 640);
        QSettings settings("obs-multi-rtmp", "EditDialog");
        QVariant saved = settings.value("size");
        if (saved.isValid()) {
            QSize s = saved.toSize();
            if (s.width() >= 640 && s.width() <= 1600 && s.height() >= 480 && s.height() <= 1200)
                initial = s;
        }
        QScreen *scr = QGuiApplication::primaryScreen();
        if (windowHandle() && windowHandle()->screen())
            scr = windowHandle()->screen();
        if (scr) {
            QSize avail = scr->availableGeometry().size();
            // Keep window decorations and small screens (1366x768) usable.
            const int margin = 60;
            QSize maxSize((std::max)(640, avail.width() - margin),
                          (std::max)(480, avail.height() - margin));
            initial = initial.boundedTo(maxSize);
        }
        resize(initial);
    }

    void SaveDialogSize()
    {
        QSettings settings("obs-multi-rtmp", "EditDialog");
        settings.setValue("size", size());
    }

    EditOutputWidgetImpl(const std::string& targetid, QWidget* parent = 0)
        : QDialog(parent)
        , targetid_(targetid)
    {
        auto& global = GlobalMultiOutputConfig();
        auto orig = FindById(global.targets, targetid_);
        if (orig == nullptr) {
            return;
        }
        config_ = std::make_shared<OutputTargetConfig>(*orig);
        // Deep-copy encoder configs into the draft so Cancel discards all
        // edits, shares, and duplicates without touching global state.
        for (auto &v : global.videoConfig)
            draftVideos_.push_back(std::make_shared<VideoEncoderConfig>(*v));
        for (auto &a : global.audioConfig)
            draftAudios_.push_back(std::make_shared<AudioEncoderConfig>(*a));
        freshTarget_ = !config_->videoConfig.has_value() && !config_->audioConfig.has_value();

        setWindowTitle(obs_module_text("StreamingSettings"));

        auto fullLayout = new QVBoxLayout(this);
        topTabs_ = new QTabWidget(this);
        fullLayout->addWidget(topTabs_, 1);

        // ---- Destino ----
        {
            auto page = new QWidget(this);
            auto layout = new QVBoxLayout(page);
            auto form = new QFormLayout();
            form->addRow(obs_module_text("StreamingName"), name_ = new QLineEdit(page));
            form->addRow(obs_module_text("Protocol"), protocolSelector_ = new QComboBox(page));
            layout->addLayout(form);
            layout->addWidget(CreateOutputSettingsWidget(page), 1);
            page->setLayout(layout);
            topTabs_->addTab(WrapInScroll(page), obs_module_text("Tab.Destination"));
        }
        // ---- Video ----
        {
            auto page = new QWidget(this);
            auto layout = new QVBoxLayout(page);
            auto form = new QFormLayout();
            form->addRow(obs_module_text("Encoder"), videoMode_ = new QComboBox(page));
            layout->addLayout(form);
            layout->addWidget(videoStatus_ = new QLabel(page));
            videoStatus_->setWordWrap(true);
            layout->addWidget(videoHint_ = new QLabel(page));
            videoHint_->setWordWrap(true);
            videoCustomBox_ = new QWidget(page);
            {
                auto encLayout = new QGridLayout(videoCustomBox_);
                int currow = 0;
                {
                    auto sublayout = new QHBoxLayout();
                    sublayout->addWidget(v_share_notify_ = new QLabel(videoCustomBox_), 1);
                    auto shareButton = new QPushButton(obs_module_text("Btn.EncoderShare"), videoCustomBox_);
                    sublayout->addWidget(shareButton);
                    encLayout->addLayout(sublayout, currow, 0, 1, 2);
                    QObject::connect(shareButton, &QPushButton::clicked, [this]() {
                        SaveConfig();
                        PopupShareMenu(false);
                        LoadConfig();
                        UpdateUI();
                    });
                }
                ++currow;
                encLayout->addWidget(new QLabel(obs_module_text("Encoder"), videoCustomBox_), currow, 0);
                encLayout->addWidget(venc_ = new QComboBox(videoCustomBox_), currow, 1);
                ++currow;
                encLayout->addWidget(new QLabel(obs_module_text("Scene"), videoCustomBox_), currow, 0);
                encLayout->addWidget(v_scene_ = new QComboBox(videoCustomBox_), currow, 1);
                ++currow;
                encLayout->addWidget(new QLabel(obs_module_text("VideoResolution"), videoCustomBox_), currow, 0);
                encLayout->addWidget(v_resolution_ = new QLineEdit("", videoCustomBox_), currow, 1);
                v_resolution_->setPlaceholderText(obs_module_text("SameAsOBSNow"));
                ++currow;
                encLayout->addWidget(new QLabel(obs_module_text("VideoFPSDenumerator"), videoCustomBox_), currow, 0);
                encLayout->addWidget(v_fpsdenumerator_ = new QComboBox(videoCustomBox_), currow, 1);
                ++currow;
                encLayout->addWidget(videoEncoderSettings_ = new PropertiesWidget([]() {}, videoCustomBox_), currow, 0, 1, 2);
                videoCustomBox_->setLayout(encLayout);
            }
            layout->addWidget(videoCustomBox_);
            layout->addStretch(1);
            page->setLayout(layout);
            topTabs_->addTab(WrapInScroll(page), obs_module_text("Tab.Video"));
        }
        // ---- Audio ----
        {
            auto page = new QWidget(this);
            auto layout = new QVBoxLayout(page);
            auto form = new QFormLayout();
            form->addRow(obs_module_text("Encoder"), audioMode_ = new QComboBox(page));
            layout->addLayout(form);
            layout->addWidget(audioStatus_ = new QLabel(page));
            audioStatus_->setWordWrap(true);
            layout->addWidget(audioHint_ = new QLabel(page));
            audioHint_->setWordWrap(true);
            audioCustomBox_ = new QWidget(page);
            {
                auto encLayout = new QGridLayout(audioCustomBox_);
                int currow = 0;
                {
                    auto sublayout = new QHBoxLayout();
                    sublayout->addWidget(a_share_notify_ = new QLabel(audioCustomBox_), 1);
                    auto shareButton = new QPushButton(obs_module_text("Btn.EncoderShare"), audioCustomBox_);
                    sublayout->addWidget(shareButton);
                    encLayout->addLayout(sublayout, currow, 0, 1, 2);
                    QObject::connect(shareButton, &QPushButton::clicked, [this]() {
                        SaveConfig();
                        PopupShareMenu(true);
                        LoadConfig();
                        UpdateUI();
                    });
                }
                ++currow;
                encLayout->addWidget(new QLabel(obs_module_text("Encoder"), audioCustomBox_), currow, 0);
                encLayout->addWidget(aenc_ = new QComboBox(audioCustomBox_), currow, 1);
                ++currow;
                const int NUM_MIXER_TRACKS = 6;
                encLayout->addWidget(new QLabel(obs_module_text("AudioMixerID"), audioCustomBox_), currow, 0);
                encLayout->addWidget(a_mixer_ = new QComboBox(audioCustomBox_), currow, 1);
                for(int i = 1; i <= NUM_MIXER_TRACKS; ++i)
                    a_mixer_->addItem(QString(std::to_string(i).c_str()), i - 1);
                ++currow;
                encLayout->addWidget(new QLabel(obs_module_text("AudioVODTrack"), audioCustomBox_), currow, 0);
                encLayout->addWidget(a_vod_track_ = new QComboBox(audioCustomBox_), currow, 1);
                a_vod_track_->addItem("", -1);
                for(int i = 1; i <= NUM_MIXER_TRACKS; ++i)
                    a_vod_track_->addItem(QString(std::to_string(i).c_str()), i - 1);
                ++currow;
                encLayout->addWidget(audioEncoderSettings_ = new PropertiesWidget([]() {}, audioCustomBox_), currow, 0, 1, 2);
                audioCustomBox_->setLayout(encLayout);
            }
            layout->addWidget(audioCustomBox_);
            layout->addStretch(1);
            page->setLayout(layout);
            topTabs_->addTab(WrapInScroll(page), obs_module_text("Tab.Audio"));
        }
        // ---- TikTok ----
        {
            auto page = new QWidget(this);
            auto tiktokLayout = new QGridLayout(page);
            tiktokLayout->setColumnStretch(1, 1);
            tiktokLayout->addWidget(tiktokEnable_ = new QCheckBox(obs_module_text("TikTok.Enable"), page), 0, 0, 1, 3);
            tiktokLayout->addWidget(new QLabel(obs_module_text("TikTok.Status"), page), 1, 0);
            tiktokLayout->addWidget(tiktokAuthStatus_ = new QLabel(page), 1, 1);
            tiktokAuthStatus_->setWordWrap(true);
            tiktokLayout->addWidget(tiktokApplyButton_ = new QPushButton(obs_module_text("TikTok.ApplyAccess"), page), 1, 2);
            QObject::connect(tiktokApplyButton_, &QPushButton::clicked, [this]() {
                TikTokAuthManager::instance().openLiveAccessApplication(this);
            });
            tiktokLayout->addWidget(tiktokLoginButton_ = new QPushButton(obs_module_text("TikTok.Login"), page), 2, 0);
            tiktokLayout->addWidget(tiktokLogoutButton_ = new QPushButton(obs_module_text("TikTok.Logout"), page), 2, 1, 1, 2);
            QObject::connect(tiktokLoginButton_, &QPushButton::clicked, [this]() {
                TikTokAuthManager::instance().login(this);
            });
            QObject::connect(tiktokLogoutButton_, &QPushButton::clicked, [this]() {
                TikTokAuthManager::instance().logout(this);
            });
            tiktokLayout->addWidget(new QLabel(obs_module_text("TikTok.Title"), page), 3, 0);
            tiktokLayout->addWidget(tiktokTitle_ = new QLineEdit("", page), 3, 1, 1, 2);
            tiktokLayout->addWidget(new QLabel(obs_module_text("TikTok.Category"), page), 4, 0);
            tiktokLayout->addWidget(tiktokCategory_ = new QComboBox(page), 4, 1, 1, 2);
            tiktokCategory_->setEditable(true);
            tiktokCategory_->setInsertPolicy(QComboBox::NoInsert);
            tiktokCategoryDebounce_ = new QTimer(this);
            tiktokCategoryDebounce_->setSingleShot(true);
            QObject::connect(tiktokCategory_->lineEdit(), &QLineEdit::textEdited, [this]() {
                tiktokCategoryDebounce_->start(400);
            });
            QObject::connect(tiktokCategoryDebounce_, &QTimer::timeout, [this]() {
                RunTikTokCategorySearch();
            });
            tiktokLayout->addWidget(new QLabel(obs_module_text("TikTok.Audience"), page), 5, 0);
            tiktokLayout->addWidget(tiktokAudience_ = new QComboBox(page), 5, 1, 1, 2);
            tiktokAudience_->addItem(obs_module_text("TikTok.AudienceEveryone"), "0");
            tiktokAudience_->addItem(obs_module_text("TikTok.AudienceAdult"), "1");
            page->setLayout(tiktokLayout);
            QObject::connect(&TikTokAuthManager::instance(), &TikTokAuthManager::authenticationChanged,
                             this, [this]() { UpdateTikTokAuth(); });
            QObject::connect(&TikTokAuthManager::instance(), &TikTokAuthManager::loginFailed,
                             this, [this](const QString &message) {
                                 if (message.isEmpty() ||
                                     message == QString::fromUtf8(obs_module_text("TikTok.LoginCancelled")))
                                     return;
                                 QMessageBox::warning(this,
                                     QString::fromUtf8(obs_module_text("TikTok.Settings")), message);
                             });
            UpdateTikTokAuth();
            TikTokAuthManager::instance().refreshAccessState();
            // Keep auth controls reachable: a stretch row swallows extra height.
            tiktokLayout->setRowStretch(6, 1);
            topTabs_->addTab(WrapInScroll(page), obs_module_text("Tab.TikTok"));
        }
        // ---- Avanzado ----
        {
            auto page = new QWidget(this);
            auto advLayout = new QGridLayout(page);
            auto gp = new QGroupBox(obs_module_text("OtherSettings"), page);
            auto otherLayout = new QGridLayout(gp);
            otherLayout->addWidget(syncStart_ = new QCheckBox(obs_module_text("SyncStart"), gp), 0, 0);
            otherLayout->addWidget(syncStop_ = new QCheckBox(obs_module_text("SyncStop"), gp), 1, 0);
            gp->setLayout(otherLayout);
            advLayout->addWidget(gp, 0, 0);
            advLayout->setRowStretch(1, 1);
            page->setLayout(advLayout);
            topTabs_->addTab(WrapInScroll(page), obs_module_text("Tab.Advanced"));
        }

        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                        QDialogButtonBox::Apply,
                                        this);
        buttons_->button(QDialogButtonBox::Ok)->setText(obs_module_text("OK"));
        buttons_->button(QDialogButtonBox::Cancel)->setText(obs_module_text("Btn.Cancel"));
        buttons_->button(QDialogButtonBox::Apply)->setText(obs_module_text("Btn.Apply"));
        QObject::connect(buttons_, &QDialogButtonBox::accepted, [this]() {
            if (CommitDraft()) {
                SaveDialogSize();
                done(DialogCode::Accepted);
            }
        });
        QObject::connect(buttons_, &QDialogButtonBox::rejected, [this]() {
            reject();
        });
        QObject::connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked, [this]() {
            CommitDraft();
        });
        fullLayout->addWidget(buttons_);
        setLayout(fullLayout);

        FillModeBox(videoMode_);
        FillModeBox(audioMode_);
        LoadProtocols();
        LoadFPSDenumerator();
        LoadEncoders();
        LoadScenes();

        LoadConfig();
        ConnectWidgetSignals();
        UpdateUI();
        ApplyStableDialogSize();
    }

    void ConnectWidgetSignals()
    {
        // Draft-only: switching widgets never touches global state; Apply/OK
        // commits via CommitDraft(), Cancel discards.
        QObject::connect(videoMode_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            SaveConfig();
            LoadConfig();
            UpdateUI();
        });
        QObject::connect(audioMode_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            SaveConfig();
            LoadConfig();
            UpdateUI();
        });
        QObject::connect(venc_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            SaveConfig();
            LoadConfig();
            UpdateUI();
        });
        QObject::connect(aenc_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            SaveConfig();
            LoadConfig();
            UpdateUI();
        });

        QObject::connect(protocolSelector_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            blog(LOG_DEBUG, "Changing protocol to %s", tostdu8(protocolSelector_->currentText()).c_str());
            SaveConfig();
            LoadConfig();
            UpdateUI();
            updateServiceTab();
            updateOutputTab();
        });

        QObject::connect(tiktokEnable_, &QCheckBox::toggled, [this]() {
            SaveConfig();
            LoadConfig();
            UpdateUI();
        });
    }

    void LoadProtocols()
    {
        auto protocol_cur = GetProtocolInfos()->GetList();
        while(protocol_cur->protocol) {
            protocolSelector_->addItem(protocol_cur->label, protocol_cur->protocol);
            ++protocol_cur;
        }
    }

    int max_video_encoder_placeholder_index = 0;
    int max_audio_encoder_placeholder_index = 0;
    void LoadEncoders()
    {
        auto ui_text = [](auto id) {
            return std::string(obs_encoder_get_display_name(id.c_str())) + " [" + id + "]";
        };

        std::regex sp(";");
        {
            // Video encoders: custom ids only. Share/Copy modes live in the
            // Mode selector above; they are not encoder choices here.
            auto old_venc = venc_->currentData();
            std::sregex_token_iterator it(supported_video_encoders_.begin(), supported_video_encoders_.end(), sp, -1), itend;
            venc_->clear();
            max_video_encoder_placeholder_index = -1;

	        while(it != itend) {
                for(auto x : EnumEncodersByCodec(it->str().c_str()))
                    venc_->addItem(ui_text(x).c_str(), x.c_str());
                ++it;
            }
            auto idx = venc_->findData(old_venc);
            if (idx >= 0)
                venc_->setCurrentIndex(idx);
        }

        {
            // Audio encoders: custom ids only (see above).
            auto old_aenc = aenc_->currentData();
            std::sregex_token_iterator it(supported_audio_encoders_.begin(), supported_audio_encoders_.end(), sp, -1), itend;
            aenc_->clear();
            max_audio_encoder_placeholder_index = -1;

            while(it != itend) {
                for(auto x : EnumEncodersByCodec(it->str().c_str()))
                    aenc_->addItem(ui_text(x).c_str(), x.c_str());
                ++it;
            }
            auto idx = aenc_->findData(old_aenc);
            if (idx >= 0)
                aenc_->setCurrentIndex(idx);
        }
    }

    void LoadFPSDenumerator()
    {
        static const char* scales[] = {
            nullptr,
            (const char*)u8"1x",
            (const char*)u8"½x", 
            (const char*)u8"⅓x", 
            (const char*)u8"¼x", 
        };
        obs_video_info ovi;
        obs_get_video_info(&ovi);
        for(int i = 1; i <= 4; ++i) {
            auto fps = 1.0 * ovi.fps_num / ovi.fps_den / i;
            auto fps_decimal_part = (int)(fps * 100) - (int)((int)fps * 100);
            std::string strfps;
            if (fps_decimal_part)
                strfps = std::to_string((int)fps) + "." + std::to_string(fps_decimal_part);
            else
                strfps = std::to_string((int)fps);
            auto text = std::string(scales[i]) + " (" + strfps + " FPS)";
            v_fpsdenumerator_->addItem(text.c_str(), i);
        }
    }

    void LoadScenes()
    {
        v_scene_->addItem(obs_module_text("SameAsOBSScene"), "");

        using EnumParam = std::vector<std::string>;
        std::vector<std::string> scenes;
        obs_enum_scenes([](void* p, obs_source_t* src) {
            auto scenes = (EnumParam*)p;
            auto name = obs_source_get_name(src);
            if (name && *name)
                scenes->emplace_back(name);
            return true;
        }, &scenes);

        for(auto& x: scenes) {
            v_scene_->addItem(x.c_str(), x.c_str());
        }
    }

    // Availability text for Share/Copy modes without a blocking modal.
    QString ModeStatusText(EncoderSourceMode mode, bool isVideo)
    {
        OBSOutputAutoRelease streamOut = obs_frontend_get_streaming_output();
        OBSOutputAutoRelease recOut = obs_frontend_get_recording_output();
        auto ready = [&](OBSOutputAutoRelease &out, bool video) -> bool {
            if (!out)
                return false;
            if (video)
                return obs_output_get_video_encoder(out) != nullptr;
            return obs_output_get_audio_encoder(out, 0) != nullptr;
        };
        switch (mode) {
        case EncoderSourceMode::ShareStreaming:
            return QString::fromUtf8(obs_module_text(
                ready(streamOut, isVideo) ? "Encoder.StatusAvailable"
                                          : "Encoder.StatusWaitingStreaming"));
        case EncoderSourceMode::ShareRecording:
            return QString::fromUtf8(obs_module_text(
                ready(recOut, isVideo) ? "Encoder.StatusAvailable"
                                       : "Encoder.StatusWaitingRecording"));
        case EncoderSourceMode::CopyStreaming:
        case EncoderSourceMode::CopyRecording:
            return QString::fromUtf8(obs_module_text("Encoder.StatusAvailable"));
        case EncoderSourceMode::Custom:
        default:
            return QString();
        }
    }

    void UpdateUI()
    {
        auto newProtocolIndex = protocolSelector_->findData(QString::fromUtf8(config_->protocol));
        // fallback to RTMP if protocol is invalid - do we really need this line?
        if (newProtocolIndex == -1) newProtocolIndex = 0;
        protocolSelector_->blockSignals(true);
        protocolSelector_->setCurrentIndex(newProtocolIndex);
        protocolSelector_->blockSignals(false);

        auto videoModeData = videoMode_->currentData().toString().toStdString();
        EncoderSourceMode videoMode = videoModeData == "custom"
                                          ? EncoderSourceMode::Custom
                                          : ResolveEncoderSourceMode(videoModeData);
        const bool customVideo = videoMode == EncoderSourceMode::Custom;
        videoCustomBox_->setVisible(customVideo);
        if (!customVideo) {
            videoStatus_->setText(ModeStatusText(videoMode, true));
            videoHint_->setText(QString::fromUtf8(obs_module_text(
                IsCopyEncoderSource(videoMode) ? "Encoder.CopyNote" : "Encoder.ShareNote")));
            v_share_notify_->setText(QString());
        } else {
            videoStatus_->setText(QString());
            videoHint_->setText(QString());
        }

        auto makeShareNotify = [&](auto& targets) {
            std::string sharedTargets;
            for (auto& x : targets) {
                if (!sharedTargets.empty())
                    sharedTargets += ", ";
                sharedTargets += x;
            }
            if (sharedTargets.empty())
                sharedTargets = obs_module_text("NoShare");
            return sharedTargets;
        };

        if (customVideo) {
            auto sharedVideoTargets = GetEncoderShareTargets(false);
            v_share_notify_->setText(QString::fromUtf8(
                obs_module_text("EncoderShare") + makeShareNotify(sharedVideoTargets)));
        }

        auto audioModeData = audioMode_->currentData().toString().toStdString();
        EncoderSourceMode audioMode = audioModeData == "custom"
                                          ? EncoderSourceMode::Custom
                                          : ResolveEncoderSourceMode(audioModeData);
        const bool customAudio = audioMode == EncoderSourceMode::Custom;
        audioCustomBox_->setVisible(customAudio);
        if (!customAudio) {
            audioStatus_->setText(ModeStatusText(audioMode, false));
            audioHint_->setText(QString::fromUtf8(obs_module_text(
                IsCopyEncoderSource(audioMode) ? "Encoder.CopyNote" : "Encoder.ShareNote")));
            a_share_notify_->setText(QString());
        } else {
            audioStatus_->setText(QString());
            audioHint_->setText(QString());
        }

        if (customAudio) {
            auto sharedAudioTargets = GetEncoderShareTargets(true);
            a_share_notify_->setText(QString::fromUtf8(
                obs_module_text("EncoderShare") + makeShareNotify(sharedAudioTargets)));
        }
    }

    std::vector<std::string> GetEncoderShareTargets(bool isAudio) {
        std::string configId;
        if (isAudio) {
            if (!config_->audioConfig.has_value())
                return { "OBS" };
            configId = *config_->audioConfig;
        }
        else {
            if (!config_->videoConfig.has_value())
                return { "OBS" };
            configId = *config_->videoConfig;
        }

        if (IsSpecialEncoder(configId)) {
		    return { obs_module_text("NoShare") };
	    }

        std::vector<std::string> ret;
        auto& global = GlobalMultiOutputConfig();
        for(auto& x: global.targets) {
            if (isAudio) {
                if (x->id != config_->id && x->audioConfig.has_value() && *x->audioConfig == configId)
                    ret.emplace_back(x->name);
            }
            else {
                if (x->id != config_->id && x->videoConfig.has_value() && *x->videoConfig == configId)
                    ret.emplace_back(x->name);
            }
        }
        return ret;
    }



    void SaveVideoConfig() {
        if (!config_->videoConfig.has_value() || IsSpecialEncoder(*config_->videoConfig))
            return;

        auto it = FindById(draftVideos_, *config_->videoConfig);
        if (it == nullptr) {
            auto new_item = draftVideos_.emplace_back(new VideoEncoderConfig{});
            it = new_item;
        }
        it->id = *config_->videoConfig;
        it->encoderId = tostdu8(venc_->currentData().toString());

        if (v_scene_->currentIndex() > 0)
            it->outputScene = tostdu8(v_scene_->currentData().toString());
        else
            it->outputScene.reset();
        
        auto resolution = v_resolution_->text().toUtf8();
        if (!resolution.isEmpty())
            it->resolution = resolution.constData();
        else
            it->resolution.reset();

        it->fpsDenumerator = v_fpsdenumerator_->currentData().toInt();
        
        it->encoderParams = videoEncoderSettings_->Save();
    }

    void SaveAudioConfig() {
        if (!config_->audioConfig.has_value() || IsSpecialEncoder(*config_->audioConfig))
            return;

        auto it = FindById(draftAudios_, *config_->audioConfig);
        if (it == nullptr) {
            auto new_item = draftAudios_.emplace_back(new AudioEncoderConfig{});
            it = new_item;
        }
        it->id = *config_->audioConfig;
        it->encoderId = tostdu8(aenc_->currentData().toString());
        it->mixerId = a_mixer_->currentData().toInt();
        it->encoderParams = audioEncoderSettings_->Save();
        
        it->audioTracks.clear();

        // First item is a placeholder
        if (a_vod_track_->currentIndex() > 0) {
            auto audio_track = std::make_shared<AudioTrackConfig>();

            audio_track->mixer_track = a_vod_track_->currentData().toInt();
            audio_track->output_track = 1;
            it->audioTracks.push_back(audio_track);
        }
    }

    void SaveConfig()
    {
        auto& global = GlobalMultiOutputConfig();

        config_->name = tostdu8(name_->text());
        if (protocolSelector_->currentIndex() >= 0)
            config_->protocol = tostdu8(protocolSelector_->itemData(protocolSelector_->currentIndex()).toString());
        config_->syncStart = syncStart_->isChecked();
        config_->syncStop = syncStop_->isChecked();
        config_->tiktok.enabled = tiktokEnable_->isChecked();
        config_->tiktok.title = tostdu8(tiktokTitle_->text());
        config_->tiktok.category = tostdu8(tiktokCategory_->currentText());
        config_->tiktok.audience = (tiktokAudience_->currentData().toString() == QStringLiteral("1"))
                                       ? TikTokAudience::AdultOnly
                                       : TikTokAudience::Everyone;
        if (outputSettings_)
            config_->outputParam = outputSettings_->Save();
        if (serviceSettings_)
            config_->serviceParam = serviceSettings_->Save();

        // Video: Mode selector is the source of truth. Custom reuses the
        // encoder dropdown + staged plugin config; Share/Copy store their
        // placeholder directly and skip custom persistence.
        {
            auto data = videoMode_ ? videoMode_->currentData() : QVariant();
            std::string modeId = data.isValid() ? data.toString().toStdString() : "custom";
            if (modeId == "custom") {
                if (venc_ && venc_->currentData().isValid()) {
                    if (!config_->videoConfig.has_value() ||
                        IsSpecialEncoder(*config_->videoConfig))
                        config_->videoConfig = GenerateId(global);
                    SaveVideoConfig();
                } else if (!config_->videoConfig.has_value() ||
                           IsSpecialEncoder(*config_->videoConfig)) {
                    // Custom mode with no encoder listed yet: keep unset so
                    // LoadConfig falls back to Share streaming display.
                    config_->videoConfig.reset();
                } else {
                    SaveVideoConfig();
                }
            } else {
                config_->videoConfig = modeId;
            }
        }

        {
            auto data = audioMode_ ? audioMode_->currentData() : QVariant();
            std::string modeId = data.isValid() ? data.toString().toStdString() : "custom";
            if (modeId == "custom") {
                if (aenc_ && aenc_->currentData().isValid()) {
                    if (!config_->audioConfig.has_value() ||
                        IsSpecialEncoder(*config_->audioConfig))
                        config_->audioConfig = GenerateId(global);
                    SaveAudioConfig();
                } else if (!config_->audioConfig.has_value() ||
                           IsSpecialEncoder(*config_->audioConfig)) {
                    config_->audioConfig.reset();
                } else {
                    SaveAudioConfig();
                }
            } else {
                config_->audioConfig = modeId;
            }
        }
    }


    void LoadTargetConfig(OutputTargetConfig& target) {
        name_->setText(QString::fromUtf8(target.name));
        auto protocolIndex = protocolSelector_->findData(QString::fromUtf8(target.protocol));
        if (protocolIndex < 0) protocolIndex = 0;
        protocolSelector_->setCurrentIndex(protocolIndex);
        syncStart_->setChecked(target.syncStart);
        syncStop_->setChecked(target.syncStop);
        tiktokEnable_->setChecked(target.tiktok.enabled);
        tiktokTitle_->setText(QString::fromUtf8(target.tiktok.title));
        tiktokCategory_->blockSignals(true);
        tiktokCategory_->clear();
        if (!target.tiktok.category.empty())
            tiktokCategory_->addItem(QString::fromUtf8(target.tiktok.category));
        tiktokCategory_->setEditText(QString::fromUtf8(target.tiktok.category));
        tiktokCategory_->blockSignals(false);
        const int audienceIndex =
            tiktokAudience_->findData(target.tiktok.matureContent() ? "1" : "0");
        tiktokAudience_->setCurrentIndex(audienceIndex < 0 ? 0 : audienceIndex);
    }

    void UpdateTikTokAuth()
    {
        if (!tiktokAuthStatus_ || !tiktokLoginButton_ || !tiktokLogoutButton_ || !tiktokApplyButton_)
            return;
        auto &auth = TikTokAuthManager::instance();
        tiktokAuthStatus_->setText(auth.statusText());
        const bool authenticated = auth.isAuthenticated();
        const auto status = auth.status();
        const bool busy = status == TikTokAuthManager::Status::LoggingIn ||
                          status == TikTokAuthManager::Status::Validating;
        tiktokLoginButton_->setVisible(!authenticated);
        tiktokLogoutButton_->setVisible(authenticated);
        tiktokLoginButton_->setEnabled(!busy);
        tiktokLogoutButton_->setEnabled(!busy);
        // The application page needs a TikTok login and is pointless
        // once access is granted: authenticated but not granted only.
        tiktokApplyButton_->setVisible(authenticated && !auth.isLiveAccessGranted());
        tiktokApplyButton_->setEnabled(!busy);
    }

    void RunTikTokCategorySearch()
    {
        auto client = TikTokAuthManager::instance().client();
        if (!client || !tiktokCategory_)
            return;
        const std::string query = tostdu8(tiktokCategory_->currentText());
        if (query.size() < 2)
            return;
        const uint64_t gen = ++tiktokCategoryGen_;
        auto result = std::make_shared<TikTokCategoryListResult>();
        // Managed job: no raw this crosses threads. Destroying the
        // dialog orphans the job (completion dropped); the generation
        // check covers superseded searches.
        TikTokWorkerService::instance().submit(
            this,
            [client, query, result](const std::shared_ptr<TikTokJobContext> &) {
                *result = client->SearchCategories(query);
            },
            [result, gen, epoch = client->authEpoch()](const std::shared_ptr<TikTokJobContext> &job) {
                // dynamic_cast: QObject is a virtual base of QWidget, so
                // static_cast cannot downcast here.
                auto *self = dynamic_cast<EditOutputWidgetImpl *>(job->receiver.data());
                if (self == nullptr)
                    return;
                self->ApplyTikTokCategories(gen, *result, epoch);
            },
            false);
    }

    void ApplyTikTokCategories(uint64_t gen, const TikTokCategoryListResult &result, uint64_t epoch)
    {
        if (gen != tiktokCategoryGen_ || !tiktokCategory_)
            return;
        if (!result.success) {
            switch (ClassifyCredentialResult(result)) {
            case CredentialValidation::InvalidCredential:
                TikTokAuthManager::instance().handleAuthFailure(epoch);
                break;
            case CredentialValidation::ValidLiveForbidden:
                TikTokAuthManager::instance().noteLiveAccessDenied(epoch);
                break;
            default:
                break;
            }
            return;
        }
        const QString current = tiktokCategory_->currentText();
        tiktokCategory_->blockSignals(true);
        tiktokCategory_->clear();
        for (const auto &entry : result.categories) {
            tiktokCategory_->addItem(QString::fromUtf8(entry.fullName),
                                     QString::fromUtf8(entry.gameMaskId));
        }
        tiktokCategory_->setEditText(current);
        tiktokCategory_->blockSignals(false);
        if (!result.categories.empty() && tiktokCategory_->hasFocus())
            tiktokCategory_->showPopup();
    }

    void LoadVideoConfig(VideoEncoderConfig *pconfig) {
        if (pconfig == nullptr) {
            videoEncoderSettings_->ClearProperties();
            return;
        }
        {
            QString encoderId = QString::fromStdString(pconfig->encoderId);
            auto idx = venc_->findData(encoderId);
            if (idx >= 0) {
                venc_->blockSignals(true);
                venc_->setCurrentIndex(idx);
                venc_->blockSignals(false);
            } else {
                videoEncoderSettings_->ClearProperties();
                return;
            }
        }

        auto& config = *pconfig;

        if (config.outputScene.has_value()) {
            auto idx = v_scene_->findData(QString::fromUtf8(*config.outputScene));
            if (idx >= 0)
                v_scene_->setCurrentIndex(idx);
        } else {
            v_scene_->setCurrentIndex(0);
        }

        v_resolution_->setText(QString::fromUtf8(
            config.resolution.value_or("")
        ));

        {
            auto idx = v_fpsdenumerator_->findData(config.fpsDenumerator);
            if (idx < 0)
                idx = 0;
            v_fpsdenumerator_->setCurrentIndex(idx);
        }

        {
            auto encoder = obs_video_encoder_create(config.encoderId.c_str(), ("tmp_video_encoder_" + targetid_ + "_" + config.id).c_str(), from_json(config.encoderParams), nullptr);
            videoEncoderSettings_->UpdateProperties(
                AddBF(obs_encoder_properties(encoder)),
                obs_encoder_get_settings(encoder)
            );
            obs_encoder_release(encoder);
        }
    }

    void LoadAudioConfig(AudioEncoderConfig *pconfig) {
        if (pconfig == nullptr) {
            audioEncoderSettings_->ClearProperties();
            return;
        }
        {
            QString encoderId = QString::fromStdString(pconfig->encoderId);
            auto idx = aenc_->findData(encoderId);
            if (idx >= 0) {
                aenc_->blockSignals(true);
                aenc_->setCurrentIndex(idx);
                aenc_->blockSignals(false);
            } else {
                audioEncoderSettings_->ClearProperties();
                return;
            }
        }

        auto& config = *pconfig;

        {
            auto idx = a_mixer_->findData(config.mixerId);
            if (idx < 0)
                idx = 0;
            a_mixer_->setCurrentIndex(idx);
        }

        {
            if (config.audioTracks.empty()) {
                a_vod_track_->setCurrentIndex(0);
            } else {
                // NOTE: at this time we assume only one audio track. This needs to be updated
                // if we ever actually add UI support for more.
                auto idx = a_vod_track_->findData(config.audioTracks.front()->mixer_track);
                if (idx < 0)
                    idx = 0;
                // This is a combobox index, not the track index
                a_vod_track_->setCurrentIndex(idx);
            }
        }

        {
            auto encoder = obs_audio_encoder_create(config.encoderId.c_str(), ("tmp_audio_encoder_" + targetid_ + "_" + config.id).c_str(), from_json(config.encoderParams), 0, nullptr);
            audioEncoderSettings_->UpdateProperties(
                obs_encoder_properties(encoder),
                obs_encoder_get_settings(encoder)
            );
            obs_encoder_release(encoder);
        }
    }

    // Picks a concrete encoder id for the given codec from a filled
    // dropdown, preferring the software baseline. Returns -1 when none is
    // listed (e.g. no matching encoder installed).
    int PickIndependentEncoder(QComboBox* box, const char* codec, const char* preferredId)
    {
        int fallback = -1;
        for (int i = 0; i < box->count(); ++i) {
            const std::string id = box->itemData(i).toString().toStdString();
            const char* encCodec = obs_get_encoder_codec(id.c_str());
            if (!encCodec || strcmp(encCodec, codec) != 0)
                continue;
            if (preferredId && id == preferredId)
                return i;
            if (fallback < 0)
                fallback = i;
        }
        return fallback;
    }

    // For a brand-new TikTok target still on shared-encoder selections,
    // default to independent H.264 video + AAC audio so it can stream
    // without OBS's primary output. Only Share selections are touched.
    void ApplyTikTokIndependentPreset()
    {
        tiktokPresetApplied_ = true;
        auto modeData = [](QComboBox *box) {
            return box->currentData().toString().toStdString();
        };
        if (modeData(videoMode_) == OBS_STREAMING_ENC_PLACEHOLDER ||
            modeData(videoMode_) == OBS_RECORDING_ENC_PLACEHOLDER) {
            int idx = PickIndependentEncoder(venc_, "h264", "obs_x264");
            if (idx >= 0) {
                videoMode_->setCurrentIndex(videoMode_->findData("custom"));
                venc_->setCurrentIndex(idx);
                SaveConfig();
            }
        }
        if (modeData(audioMode_) == OBS_STREAMING_ENC_PLACEHOLDER ||
            modeData(audioMode_) == OBS_RECORDING_ENC_PLACEHOLDER) {
            int idx = PickIndependentEncoder(aenc_, "aac", "ffmpeg_aac");
            if (idx >= 0) {
                audioMode_->setCurrentIndex(audioMode_->findData("custom"));
                aenc_->setCurrentIndex(idx);
                SaveConfig();
            }
        }
        LoadConfig();
    }

    void SetModeBox(QComboBox *box, const std::optional<std::string> &configId)
    {
        std::string id = configId.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        int idx;
        if (IsSpecialEncoder(id))
            idx = box->findData(QString::fromUtf8(id));
        else
            idx = box->findData("custom");
        if (idx < 0)
            idx = 0;
        box->blockSignals(true);
        box->setCurrentIndex(idx);
        box->blockSignals(false);
    }

    void LoadConfig() {
        if (config_->name.empty())
            config_->name = obs_module_text("NewStreaming");

        LoadTargetConfig(*config_);

        SetModeBox(videoMode_, config_->videoConfig);
        SetModeBox(audioMode_, config_->audioConfig);

        if (!config_->videoConfig.has_value() || IsSpecialEncoder(*config_->videoConfig)) {
            videoEncoderSettings_->ClearProperties();
        } else {
            auto it = FindById(draftVideos_, *config_->videoConfig);
            LoadVideoConfig(it ? it.get() : nullptr);
        }

        if (!config_->audioConfig.has_value() || IsSpecialEncoder(*config_->audioConfig)) {
            audioEncoderSettings_->ClearProperties();
        } else {
            auto it = FindById(draftAudios_, *config_->audioConfig);
            LoadAudioConfig(it ? it.get() : nullptr);
        }

        if (freshTarget_ && !tiktokPresetApplied_ && config_->tiktok.enabled)
            ApplyTikTokIndependentPreset();

        UpdateUI();
    }
};


EditOutputWidget* createEditOutputWidget(const std::string& targetid, QWidget* parent) {
    return new EditOutputWidgetImpl(targetid, parent);
}
