#include "tiktok/auth-manager.h"

#include "pch.h"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <thread>

#include "output-config.h"
#include "streamlabs-tiktok.h"
#include "tiktok/auth-dialog.h"
#include "tiktok/auth-flow.h"
#include "tiktok/auth-url.h"
#include "tiktok/constants.h"
#include "tiktok/credential-store.h"
#include "tiktok/exchange.h"
#include "tiktok/streamlabs-client.h"

namespace {

constexpr int kPollIntervalMs = 250;
constexpr int kExchangeTimeoutMs = 30000;
constexpr int kOverallTimeoutMs = 10 * 60 * 1000;
constexpr int kTikTokRecheckDelayMs = 1500;
constexpr int kAccessRefreshMs = 15 * 60 * 1000;
constexpr int kMinRefreshGapSec = 60;

std::atomic<int> g_liveActivity{0};

} // namespace

namespace TikTokLiveRegistry {
void registerActivity()
{
    g_liveActivity.fetch_add(1);
}
void unregisterActivity()
{
    g_liveActivity.fetch_sub(1);
}
bool hasActiveActivity()
{
    return g_liveActivity.load() > 0;
}
} // namespace TikTokLiveRegistry

TikTokAuthManager &TikTokAuthManager::instance()
{
    // Leaky singleton: workers and queued callbacks may outlive any
    // destruction point during OBS shutdown.
    static auto *manager = new TikTokAuthManager();
    return *manager;
}

TikTokAuthManager::TikTokAuthManager(QObject *parent)
    : QObject(parent)
{
    secureStore_ = CreateSecureCredentialStore();
    memoryStore_ = std::make_unique<MemoryCredentialStore>();
    activeStore_ = secureStore_ ? secureStore_.get() : memoryStore_.get();
}

TikTokAuthManager::~TikTokAuthManager() = default;

bool TikTokAuthManager::isAuthenticated() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ == Status::Authenticated;
}

bool TikTokAuthManager::isLiveAccessGranted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ == Status::Authenticated && account_.has_value() &&
           account_->liveEligibilityKnown && account_->canGoLive;
}

std::optional<TikTokAccountInfo> TikTokAuthManager::account() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return account_;
}

std::optional<TikTokUserProfile> TikTokAuthManager::profile() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!account_.has_value())
        return std::nullopt;
    TikTokUserProfile profile;
    profile.username = account_->username;
    profile.displayName = account_->displayName;
    return profile;
}

TikTokAuthManager::Status TikTokAuthManager::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

QString TikTokAuthManager::statusText() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    switch (status_) {
    case Status::LoggingIn:
        return QString::fromUtf8(obs_module_text("TikTok.LoggingIn"));
    case Status::Validating:
        return QString::fromUtf8(obs_module_text("TikTok.Validating"));
    case Status::Authenticated: {
        QString name = QString::fromUtf8("@");
        if (account_.has_value() && !account_->username.empty())
            name += QString::fromUtf8(account_->username.c_str());
        else
            name += QString::fromUtf8("tiktok");
        QString text = QString::fromUtf8(obs_module_text("TikTok.Connected")).arg(name);
        const bool granted = account_.has_value() && account_->liveEligibilityKnown &&
                             account_->canGoLive;
        if (!granted && account_.has_value()) {
            if (!account_->applicationStatus.empty()) {
                text += QStringLiteral("\n") +
                        QString::fromUtf8(obs_module_text("TikTok.ApplicationStatus"))
                            .arg(QString::fromUtf8(account_->applicationStatus.c_str()));
            } else if (applyOpened_) {
                text += QStringLiteral("\n") +
                        QString::fromUtf8(obs_module_text("TikTok.ApplicationPending"));
            } else if (account_->liveEligibilityKnown && !account_->canGoLive) {
                text += QStringLiteral("\n") +
                        QString::fromUtf8(obs_module_text("TikTok.LiveUnavailable"));
            }
        }
        return text;
    }
    case Status::SessionExpired:
        return QString::fromUtf8(obs_module_text("TikTok.SessionExpired"));
    case Status::NetworkUnknown:
        return QString::fromUtf8(obs_module_text("TikTok.NetworkUnknown"));
    case Status::LoggedOut:
    default:
        return QString::fromUtf8(obs_module_text("TikTok.NotConnected"));
    }
}

std::shared_ptr<StreamlabsTikTokClient> TikTokAuthManager::client() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (token_.empty())
        return nullptr;
    return std::make_shared<StreamlabsTikTokClient>(token_, authEpoch_);
}

bool TikTokAuthManager::secureStorageAvailable() const
{
    return secureStore_ != nullptr;
}

bool TikTokAuthManager::secureStoragePersistent() const
{
    return activeStore_ != nullptr && activeStore_->isPersistent();
}

bool TikTokAuthManager::webEngineAvailable() const
{
#ifdef WITH_TIKTOK_WEBENGINE
    return true;
#else
    return false;
#endif
}

void TikTokAuthManager::setStatus(Status status)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == status)
            return;
        status_ = status;
    }
    emit stateChanged();
    emit authenticationChanged();
}

void TikTokAuthManager::restoreSession()
{
    uint64_t restoreId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (restoring_ || loginActive_ || status_ == Status::Authenticated)
            return;
        restoring_ = true;
        restoreId = ++restoreGen_;
    }
    blog(LOG_INFO, TAG "TikTok: restoring saved session");
    setStatus(Status::Validating);

    std::thread([this, restoreId]() {
        std::optional<std::string> stored = activeStore_->load();
        const bool haveToken = stored.has_value() && !stored->empty();
        TikTokAccountResult validation;
        if (haveToken) {
            StreamlabsTikTokClient probe(*stored);
            validation = probe.GetAccountInfo();
        }
        QMetaObject::invokeMethod(
            this,
            [this, restoreId, stored = std::move(stored), haveToken,
             validation = std::move(validation)]() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (restoreId != restoreGen_)
                        return;
                    restoring_ = false;
                }
                if (!haveToken) {
                    blog(LOG_INFO, TAG "TikTok: no saved credential");
                    setStatus(Status::LoggedOut);
                    pumpLegacyMigration();
                    return;
                }
                if (validation.success) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        token_ = *stored;
                        account_ = validation.account;
                        authEpoch_++;
                    }
                    blog(LOG_INFO, TAG "TikTok: session restored");
                    setStatus(Status::Authenticated);
                    emit loginSucceeded();
                    scheduleAccessRefresh();
                    return;
                }
                if (validation.failure == TikTokFailure::Auth ||
                    validation.failure == TikTokFailure::Forbidden) {
                    blog(LOG_INFO, TAG "TikTok: saved credential rejected, removing");
                    activeStore_->remove();
                    memoryStore_->remove();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        token_.clear();
                        account_.reset();
                        authEpoch_++;
                    }
                    setStatus(Status::SessionExpired);
                    return;
                }
                // Temporary failure: retain the credential, status unknown.
                blog(LOG_INFO, TAG "TikTok: validation deferred (temporary failure)");
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    token_ = *stored;
                    account_.reset();
                }
                setStatus(Status::NetworkUnknown);
            },
            Qt::QueuedConnection);
    }).detach();
}

void TikTokAuthManager::migrateLegacyTokens(std::vector<std::string> tokens)
{
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                                [](const std::string &t) { return t.empty(); }),
                 tokens.end());
    if (tokens.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingLegacy_.insert(pendingLegacy_.end(), tokens.begin(), tokens.end());
    }
    QMetaObject::invokeMethod(this, [this]() { pumpLegacyMigration(); }, Qt::QueuedConnection);
}

void TikTokAuthManager::pumpLegacyMigration()
{
    std::string candidate;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingLegacy_.empty() || loginActive_ || restoring_)
            return;
        if (!token_.empty() || status_ == Status::Authenticated ||
            status_ == Status::NetworkUnknown) {
            // A session already exists: legacy plaintext is redundant,
            // drop it and sanitize the config file.
            pendingLegacy_.clear();
            blog(LOG_INFO, TAG "TikTok: discarding legacy token (session present)");
            SaveMultiOutputConfig();
            return;
        }
        candidate = std::move(pendingLegacy_.front());
        pendingLegacy_.clear();
        restoring_ = true;
    }
    blog(LOG_INFO, TAG "TikTok: migrating legacy token");
    setStatus(Status::Validating);

    std::thread([this, candidate = std::move(candidate)]() {
        StreamlabsTikTokClient probe(candidate);
        TikTokAccountResult validation = probe.GetAccountInfo();
        QMetaObject::invokeMethod(
            this,
            [this, candidate = std::move(candidate), validation = std::move(validation)]() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    restoring_ = false;
                }
                if (validation.success) {
                    const bool saved = activeStore_->save(candidate);
                    if (!saved && activeStore_ != memoryStore_.get()) {
                        memoryStore_->save(candidate);
                        activeStore_ = memoryStore_.get();
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        token_ = candidate;
                        account_ = validation.account;
                        authEpoch_++;
                    }
                    blog(LOG_INFO, TAG "TikTok: legacy token migrated");
                    SaveMultiOutputConfig();
                    setStatus(Status::Authenticated);
                    emit loginSucceeded();
                    scheduleAccessRefresh();
                    if (!secureStoragePersistent()) {
                        QMessageBox::warning(
                            nullptr, QString::fromUtf8(obs_module_text("TikTok.Settings")),
                            QString::fromUtf8(
                                obs_module_text("TikTok.SecureStorageUnavailable")));
                    }
                    return;
                }
                blog(LOG_INFO, TAG "TikTok: legacy token invalid, requiring login");
                SaveMultiOutputConfig();
                setStatus(Status::LoggedOut);
            },
            Qt::QueuedConnection);
    }).detach();
}

void TikTokAuthManager::login(QWidget *parent)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loginActive_) {
            if (dialog_)
                dialog_->raise();
            return;
        }
    }
    if (!webEngineAvailable()) {
        emit loginFailed(QString::fromUtf8(obs_module_text("TikTok.WebEngineUnavailable")));
        return;
    }

    const uint64_t attempt = ++loginAttempt_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only a validated session is worth keeping when re-login fails;
        // a merely retained (NetworkUnknown) credential is not.
        reauthing_ = (status_ == Status::Authenticated) && !token_.empty();
        loginActive_ = true;
    }
    flow_ = std::make_unique<TikTokAuthFlow>(attempt);
    if (flow_->pkce().verifier.isEmpty() || flow_->pkce().challenge.isEmpty()) {
        finishLoginAttempt(attempt, QString::fromUtf8(obs_module_text("TikTok.LoginFailed")));
        return;
    }

    blog(LOG_INFO, TAG "TikTok: login attempt started");
    setStatus(Status::LoggingIn);
    emit loginStarted();

    dialog_ = new TikTokAuthDialog(parent);
    if (!dialog_->browserAvailable()) {
        finishLoginAttempt(attempt,
                           QString::fromUtf8(obs_module_text("TikTok.WebEngineUnavailable")));
        return;
    }
    QObject::connect(dialog_, &TikTokAuthDialog::urlObserved, this,
                     [this, attempt](const QUrl &url) { onDialogUrl(attempt, url); });
    QObject::connect(dialog_, &TikTokAuthDialog::pageLoadFinished, this,
                     [this, attempt](bool ok, const QUrl &url) { onDialogLoad(attempt, ok, url); });
    QObject::connect(dialog_, &QDialog::rejected, this, [this, attempt]() {
        // Dialog closed: cancel only if this attempt is still active.
        std::lock_guard<std::mutex> lock(mutex_);
        if (loginActive_ && attempt == loginAttempt_)
            QMetaObject::invokeMethod(this, [this, attempt]() { cancelLogin(); },
                                      Qt::QueuedConnection);
    });

    stopLoginTimers();
    overallTimeout_ = new QTimer(this);
    overallTimeout_->setSingleShot(true);
    QObject::connect(overallTimeout_, &QTimer::timeout, this, [this, attempt]() {
        finishLoginAttempt(attempt, QString::fromUtf8(obs_module_text("TikTok.LoginTimeout")));
    });
    overallTimeout_->start(kOverallTimeoutMs);

    dialog_->navigate(QUrl(QString::fromLatin1(TikTokConstants::kTikTokLoginUrl)));
    dialog_->show();
}

void TikTokAuthManager::cancelLogin()
{
    uint64_t attempt = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_)
            return;
        attempt = loginAttempt_;
    }
    blog(LOG_INFO, TAG "TikTok: login attempt cancelled");
    finishLoginAttempt(attempt, QString::fromUtf8(obs_module_text("TikTok.LoginCancelled")));
}

void TikTokAuthManager::openLiveAccessApplication(QWidget *parent)
{
    if (applyDialog_) {
        applyDialog_->raise();
        return;
    }
    if (!webEngineAvailable()) {
        // Help link only (not a credential flow), so the external
        // browser is an acceptable fallback here.
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(TikTokConstants::kLiveAccessApplyUrl)));
        return;
    }
    blog(LOG_INFO, TAG "TikTok: opening LIVE-access application page");
    applyDialog_ = new TikTokAuthDialog(
        parent, QString::fromUtf8(obs_module_text("TikTok.ApplyAccess")));
    QObject::connect(applyDialog_, &QObject::destroyed, this, [this]() { onApplyClosed(); });
    // Navigation is observed only to notice the result step (pending +
    // auto-close). No page content is read and no auth flow is attached:
    // the window stays user-driven.
    QObject::connect(applyDialog_, &TikTokAuthDialog::urlObserved, this,
                     [this](const QUrl &url) { onApplyUrl(url); });
    applyDialog_->setAttribute(Qt::WA_DeleteOnClose);
    applyDialog_->navigate(QUrl(QString::fromLatin1(TikTokConstants::kLiveAccessApplyUrl)));
    applyDialog_->show();
}

void TikTokAuthManager::onApplyUrl(const QUrl &url)
{
    if (!applyDialog_ || !TikTokAuthUrl::IsLiveAccessResultUrl(url))
        return;
    blog(LOG_INFO, TAG "TikTok: LIVE-access result page reached, marking pending");
    // Closing destroys the window (WA_DeleteOnClose), which lands in
    // onApplyClosed() for the pending state + follow-up refresh.
    applyDialog_->close();
}

void TikTokAuthManager::onApplyClosed()
{
    applyDialog_ = nullptr;
    const bool authenticated = isAuthenticated();
    const bool granted = isLiveAccessGranted();
    if (authenticated && !granted) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            applyOpened_ = true;
        }
        emit authenticationChanged();
        // One cheap check; catches fast approvals without waiting for
        // the periodic refresh.
        refreshAccessState();
    }
}

void TikTokAuthManager::logout(QWidget *parent)
{
    if (TikTokLiveRegistry::hasActiveActivity()) {
        QMessageBox::warning(parent, QString::fromUtf8(obs_module_text("TikTok.Settings")),
                             QString::fromUtf8(obs_module_text("TikTok.LogoutBlockedActive")));
        return;
    }
    // Any in-flight validation worker callback finds loginActive_ cleared
    // and discards its candidate.
    stopLoginTimers();
    // Scrub the live WebView session before tearing the dialog down so
    // cookies cannot be re-persisted on destruction.
    scrubDialogSession();
    teardownDialog();
    if (refreshTimer_)
        refreshTimer_->stop();
    if (applyDialog_) {
        // Its TikTok session is being cleared; close it so it cannot
        // linger on a half-logged-out page.
        applyDialog_->close();
    }
    flow_.reset();
    if (secureStore_)
        secureStore_->remove();
    memoryStore_->remove();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_.clear();
        account_.reset();
        authEpoch_++;
        loginActive_ = false;
        reauthing_ = false;
        applyOpened_ = false;
    }
    TikTokAuthDialog::clearPersistentData();
    blog(LOG_INFO, TAG "TikTok: logged out");
    setStatus(Status::LoggedOut);
    emit loggedOut();
}

void TikTokAuthManager::handleAuthFailure(uint64_t epoch)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch != authEpoch_ || token_.empty())
            return;
        token_.clear();
        account_.reset();
        authEpoch_++;
        applyOpened_ = false;
    }
    if (secureStore_)
        secureStore_->remove();
    memoryStore_->remove();
    if (refreshTimer_)
        refreshTimer_->stop();
    blog(LOG_INFO, TAG "TikTok: session expired (credential rejected)");
    setStatus(Status::SessionExpired);
}

void TikTokAuthManager::noteLiveStartSucceeded(uint64_t epoch)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch != authEpoch_ || token_.empty())
            return;
        if (!account_.has_value())
            account_ = TikTokAccountInfo{};
        account_->liveEligibilityKnown = true;
        account_->canGoLive = true;
        account_->liveUnavailableReason.clear();
        applyOpened_ = false;
    }
    blog(LOG_INFO, TAG "TikTok: LIVE access confirmed by successful start");
    emit authenticationChanged();
}

void TikTokAuthManager::noteLiveAccessDenied(uint64_t epoch)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch != authEpoch_ || token_.empty())
            return;
        if (!account_.has_value())
            account_ = TikTokAccountInfo{};
        // Eligibility only: the credential stays valid and saved.
        account_->liveEligibilityKnown = true;
        account_->canGoLive = false;
    }
    blog(LOG_INFO, TAG "TikTok: LIVE access denied for this account (session kept)");
    emit authenticationChanged();
}

void TikTokAuthManager::refreshAccessState()
{
    std::string token;
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loginActive_ || restoring_ || refreshing_ || token_.empty())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (lastRefresh_ != std::chrono::steady_clock::time_point{} &&
            now - lastRefresh_ < std::chrono::seconds(kMinRefreshGapSec))
            return;
        refreshing_ = true;
        token = token_;
        epoch = authEpoch_;
    }
    std::thread([this, token = std::move(token), epoch]() {
        StreamlabsTikTokClient probe(token);
        TikTokAccountResult result = probe.GetAccountInfo();
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result), epoch]() mutable {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    refreshing_ = false;
                    if (epoch != authEpoch_)
                        return; // stale: a newer session exists
                    lastRefresh_ = std::chrono::steady_clock::now();
                }
                if (result.success) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (account_.has_value() && !result.account.liveEligibilityKnown &&
                            account_->liveEligibilityKnown) {
                            // A refresh without eligibility fields must not
                            // wipe a positively observed state.
                            result.account.liveEligibilityKnown = true;
                            result.account.canGoLive = account_->canGoLive;
                            result.account.liveUnavailableReason =
                                account_->liveUnavailableReason;
                            if (result.account.applicationStatus.empty())
                                result.account.applicationStatus =
                                    account_->applicationStatus;
                        }
                        account_ = result.account;
                        if (account_->liveEligibilityKnown && account_->canGoLive)
                            applyOpened_ = false;
                    }
                    emit authenticationChanged();
                    return;
                }
                if (result.failure == TikTokFailure::Auth)
                    handleAuthFailure(epoch);
                else if (result.failure == TikTokFailure::Forbidden)
                    noteLiveAccessDenied(epoch);
                // Transient failures: keep everything, the timer retries.
            },
            Qt::QueuedConnection);
    }).detach();
}

void TikTokAuthManager::scheduleAccessRefresh()
{
    if (!refreshTimer_) {
        refreshTimer_ = new QTimer(this);
        refreshTimer_->setSingleShot(true);
        QObject::connect(refreshTimer_, &QTimer::timeout, this, [this]() {
            bool active = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active = (status_ == Status::Authenticated);
            }
            if (!active)
                return;
            refreshAccessState();
            scheduleAccessRefresh();
        });
    }
    refreshTimer_->start(kAccessRefreshMs);
}

void TikTokAuthManager::onDialogUrl(uint64_t attempt, const QUrl &url)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_ || attempt != loginAttempt_ || !flow_)
            return;
    }
    blog(LOG_DEBUG, TAG "TikTok: auth navigation %s",
         TikTokAuthUrl::LogSafeUrl(url).toUtf8().constData());

    QString code;
    const auto action = flow_->onNavigate(url, &code);
    switch (action) {
    case TikTokAuthFlow::NavAction::ScheduleRecheck:
        QTimer::singleShot(kTikTokRecheckDelayMs, this, [this, attempt]() {
            std::optional<QUrl> target;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!loginActive_ || attempt != loginAttempt_ || !flow_ || !dialog_)
                    return;
                target = flow_->onRecheck();
            }
            if (target.has_value()) {
                blog(LOG_INFO, TAG "TikTok: TikTok login detected, continuing to Streamlabs");
                dialog_->navigate(*target);
            }
        });
        break;
    case TikTokAuthFlow::NavAction::Exchange:
        blog(LOG_INFO, TAG "TikTok: authorization received, exchanging token");
        startExchange(attempt, code, flow_->exchangePage());
        break;
    case TikTokAuthFlow::NavAction::None:
        break;
    }
}

void TikTokAuthManager::onDialogLoad(uint64_t attempt, bool ok, const QUrl &url)
{
    Q_UNUSED(ok);
    bool retry = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_ || attempt != loginAttempt_ || !flow_ || !dialog_)
            return;
        // The first exchange may have executed in the stale pre-commit
        // document; permit a single retry once the correct success
        // document finishes loading.
        if (flow_->shouldReeval(TikTokAuthUrl::SuccessPageId(url))) {
            blog(LOG_DEBUG, TAG "TikTok: retrying exchange in committed document");
            flow_->markReevalDone();
            retry = true;
        }
    }
    if (retry)
        runExchangeStarter(attempt);
}

void TikTokAuthManager::startExchange(uint64_t attempt, const QString &code, const QString &expectedPage)
{
    Q_UNUSED(code);
    Q_UNUSED(expectedPage);
    setStatus(Status::Validating);
    runExchangeStarter(attempt);

    if (pollTimer_) {
        pollTimer_->stop();
        pollTimer_->deleteLater();
    }
    pollTimer_ = new QTimer(this);
    QObject::connect(pollTimer_, &QTimer::timeout, this, [this, attempt]() { pollExchange(attempt); });
    pollTimer_->start(kPollIntervalMs);

    if (exchangeTimeout_) {
        exchangeTimeout_->stop();
        exchangeTimeout_->deleteLater();
    }
    exchangeTimeout_ = new QTimer(this);
    exchangeTimeout_->setSingleShot(true);
    QObject::connect(exchangeTimeout_, &QTimer::timeout, this, [this, attempt]() {
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active = loginActive_ && attempt == loginAttempt_ && flow_ && !flow_->exchangeComplete();
        }
        if (!active)
            return;
        blog(LOG_INFO, TAG "TikTok: token exchange timed out");
        finishLoginAttempt(attempt, QString::fromUtf8(obs_module_text("TikTok.LoginTimeout")));
    });
    exchangeTimeout_->start(kExchangeTimeoutMs);
}

void TikTokAuthManager::runExchangeStarter(uint64_t attempt)
{
    // Only runs for the attempt that observed an allowlisted OAuth-success
    // URL; the script itself is fenced to the expected success document.
    TikTokAuthDialog *dialog = nullptr;
    std::string script;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_ || attempt != loginAttempt_ || !flow_ || !dialog_ ||
            flow_->exchangeComplete())
            return;
        dialog = dialog_;
        const QUrl exchangeUrl =
            TikTokAuthUrl::BuildExchangeUrl(flow_->exchangeCode(), flow_->pkce().verifier);
        script = BuildExchangeStartJs(exchangeUrl.toString().toStdString(),
                                      flow_->exchangePage().toStdString());
    }
    dialog->runJavaScript(QString::fromStdString(script), [](const QVariant &) {});
}

void TikTokAuthManager::pollExchange(uint64_t attempt)
{
    TikTokAuthDialog *dialog = nullptr;
    std::string script;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_ || attempt != loginAttempt_ || !flow_ || !dialog_ ||
            flow_->exchangeComplete())
            return;
        dialog = dialog_;
        script = BuildExchangePollJs(flow_->exchangePage().toStdString());
    }
    QPointer<TikTokAuthManager> self(this);
    dialog->runJavaScript(QString::fromStdString(script), [this, self, attempt](const QVariant &result) {
        if (!self)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!loginActive_ || attempt != loginAttempt_ || !flow_ || flow_->exchangeComplete())
                return;
        }
        if (!result.isValid() || result.isNull())
            return; // still in flight
        const std::string payload =
            QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact).toStdString();
        const ExchangeOutcome outcome = ParseExchangePayload(payload);
        if (!outcome.settled)
            return;
        flow_->markExchangeComplete();
        if (pollTimer_)
            pollTimer_->stop();
        if (exchangeTimeout_)
            exchangeTimeout_->stop();
        if (!outcome.success) {
            blog(LOG_INFO, TAG "TikTok: token exchange failed");
            finishLoginAttempt(attempt, QString::fromUtf8(obs_module_text("TikTok.LoginFailed")));
            return;
        }
        blog(LOG_INFO, TAG "TikTok: token received, validating account");
        bool isReauth = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            isReauth = reauthing_;
        }
        validateCandidateOnWorker(attempt, outcome.token, isReauth);
    });
}

void TikTokAuthManager::validateCandidateOnWorker(uint64_t attempt, std::string candidateToken,
                                                 bool isReauth)
{
    setStatus(Status::Validating);
    std::thread([this, attempt, candidateToken = std::move(candidateToken), isReauth]() {
        StreamlabsTikTokClient probe(candidateToken);
        TikTokAccountResult validation = probe.GetAccountInfo();
        QMetaObject::invokeMethod(
            this,
            [this, attempt, candidateToken = std::move(candidateToken),
             validation = std::move(validation), isReauth]() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!loginActive_ || attempt != loginAttempt_)
                        return; // superseded: discard candidate
                }
                if (validation.success) {
                    acceptValidatedLogin(attempt, candidateToken, validation);
                    return;
                }
                QString message;
                if (validation.failure == TikTokFailure::Network ||
                    validation.failure == TikTokFailure::Timeout) {
                    message = QString::fromUtf8(obs_module_text("Error.TikTokNetwork"));
                } else {
                    message = QString::fromUtf8(obs_module_text("TikTok.LoginFailed"));
                }
                Q_UNUSED(isReauth);
                finishLoginAttempt(attempt, message);
            },
            Qt::QueuedConnection);
    }).detach();
}

void TikTokAuthManager::acceptValidatedLogin(uint64_t attempt, const std::string &candidateToken,
                                            TikTokAccountResult account)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_ || attempt != loginAttempt_)
            return;
    }
    const bool secureSaved = activeStore_->save(candidateToken);
    bool memoryFallback = false;
    if (!secureSaved && activeStore_ != memoryStore_.get()) {
        memoryFallback = memoryStore_->save(candidateToken);
        if (memoryFallback)
            activeStore_ = memoryStore_.get();
    }
    QWidget *notifyParent = dialog_ ? dialog_->parentWidget() : nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_ = candidateToken;
        account_ = account.account;
        authEpoch_++;
        loginActive_ = false;
        reauthing_ = false;
        // A new account may have signed in; pending memory belongs to
        // the previous session.
        applyOpened_ = false;
    }
    stopLoginTimers();
    teardownDialog();
    flow_.reset();
    blog(LOG_INFO, TAG "TikTok: login succeeded");
    setStatus(Status::Authenticated);
    emit loginSucceeded();
    scheduleAccessRefresh();
    if (!secureStoragePersistent()) {
        QMessageBox::warning(notifyParent, QString::fromUtf8(obs_module_text("TikTok.Settings")),
                             QString::fromUtf8(obs_module_text("TikTok.SecureStorageUnavailable")));
    }
}

void TikTokAuthManager::finishLoginAttempt(uint64_t attempt, const QString &error)
{
    bool keepOld = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_ || attempt != loginAttempt_)
            return;
        keepOld = reauthing_ && !token_.empty();
        loginActive_ = false;
        reauthing_ = false;
    }
    stopLoginTimers();
    teardownDialog();
    flow_.reset();
    if (keepOld) {
        // Re-login failed: the previous valid session survives.
        blog(LOG_INFO, TAG "TikTok: re-login failed, keeping previous session");
        setStatus(Status::Authenticated);
    } else {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            token_.clear();
            account_.reset();
        }
        setStatus(Status::LoggedOut);
    }
    if (!error.isEmpty())
        emit loginFailed(error);
}

void TikTokAuthManager::stopLoginTimers()
{
    if (pollTimer_) {
        pollTimer_->stop();
        pollTimer_->deleteLater();
        pollTimer_ = nullptr;
    }
    if (exchangeTimeout_) {
        exchangeTimeout_->stop();
        exchangeTimeout_->deleteLater();
        exchangeTimeout_ = nullptr;
    }
    if (overallTimeout_) {
        overallTimeout_->stop();
        overallTimeout_->deleteLater();
        overallTimeout_ = nullptr;
    }
}

void TikTokAuthManager::teardownDialog()
{
    if (dialog_) {
        dialog_->disconnect(this);
        dialog_->close();
        dialog_->deleteLater();
        dialog_ = nullptr;
    }
}

void TikTokAuthManager::scrubDialogSession()
{
    if (dialog_)
        dialog_->clearSessionData();
}
