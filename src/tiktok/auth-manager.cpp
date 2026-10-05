#include "tiktok/auth-manager.h"

#include "pch.h"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QCoreApplication>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <atomic>

#include "output-config.h"
#include "streamlabs-tiktok.h"
#include "tiktok/auth-dialog.h"
#include "tiktok/auth-flow.h"
#include "tiktok/auth-url.h"
#include "tiktok/constants.h"
#include "tiktok/credential-store.h"
#include "tiktok/exchange.h"
#include "tiktok/streamlabs-client.h"
#include "tiktok/worker.h"

namespace {

constexpr int kPollIntervalMs = 250;
constexpr int kExchangeTimeoutMs = 30000;
constexpr int kOverallTimeoutMs = 10 * 60 * 1000;
constexpr int kTikTokRecheckDelayMs = 1500;
constexpr int kAccessRefreshMs = 15 * 60 * 1000;
constexpr int kMinRefreshGapSec = 60;
constexpr int kMigrationRetryMs = 30000;
constexpr int kMaxMigrationRetries = 3;

std::atomic<int> g_activeLives{0};

// Serializes credential-store saves against compare-and-delete removals
// (worker threads only; the UI thread merely submits jobs). The store
// holds a single entry, so without this a stale removal landing after a
// newer login's save would destroy the live credential.
std::mutex g_storeMutex;

} // namespace

namespace TikTokLiveRegistry {
void addLive()
{
    g_activeLives.fetch_add(1);
}
void removeLive()
{
    g_activeLives.fetch_sub(1);
}
bool hasActiveActivity()
{
    return g_activeLives.load() > 0 || TikTokWorkerService::instance().hasActiveActivity();
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
    deferredContext_ = new QObject(this);
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

    struct RestoreOutcome {
        std::optional<std::string> stored;
        TikTokAccountResult validation;
    };
    auto outcome = std::make_shared<RestoreOutcome>();
    ICredentialStore *store = activeStore_;
    auto job = TikTokWorkerService::instance().submit(
        this,
        [store, outcome](const std::shared_ptr<TikTokJobContext> &) {
            outcome->stored = store->load();
            if (outcome->stored.has_value() && !outcome->stored->empty()) {
                StreamlabsTikTokClient probe(*outcome->stored);
                outcome->validation = probe.GetAccountInfo();
            }
        },
        [this, restoreId, outcome](const std::shared_ptr<TikTokJobContext> &) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (restoreId != restoreGen_)
                    return;
                restoring_ = false;
            }
            const bool haveToken = outcome->stored.has_value() && !outcome->stored->empty();
            if (!haveToken) {
                blog(LOG_INFO, TAG "TikTok: no saved credential");
                setStatus(Status::LoggedOut);
                pumpLegacyMigration();
                return;
            }
            switch (ClassifyCredentialResult(outcome->validation)) {
            case CredentialValidation::Valid: {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    token_ = *outcome->stored;
                    account_ = outcome->validation.account;
                    authEpoch_++;
                }
                blog(LOG_INFO, TAG "TikTok: session restored");
                setStatus(Status::Authenticated);
                emit loginSucceeded();
                scheduleAccessRefresh();
                return;
            }
            case CredentialValidation::ValidLiveForbidden: {
                // 403: the credential stays saved and the session stays
                // authenticated; only LIVE eligibility is marked false.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    token_ = *outcome->stored;
                    account_ = TikTokAccountInfo{};
                    account_->liveEligibilityKnown = true;
                    account_->canGoLive = false;
                    authEpoch_++;
                }
                blog(LOG_INFO, TAG "TikTok: session restored without LIVE access (HTTP 403)");
                setStatus(Status::Authenticated);
                emit loginSucceeded();
                scheduleAccessRefresh();
                return;
            }
            case CredentialValidation::InvalidCredential: {
                blog(LOG_INFO, TAG "TikTok: saved credential rejected, removing");
                const std::string rejected = *outcome->stored;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    token_.clear();
                    account_.reset();
                    authEpoch_++;
                }
                removeStoredCredentialAsync(rejected);
                setStatus(Status::SessionExpired);
                return;
            }
            case CredentialValidation::TemporaryFailure:
                break;
            }
            // Temporary failure: retain the credential, status unknown.
            blog(LOG_INFO, TAG "TikTok: validation deferred (temporary failure)");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                token_ = *outcome->stored;
                account_.reset();
            }
            setStatus(Status::NetworkUnknown);
        },
        false);
    if (!job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            restoring_ = false;
        }
        setStatus(Status::LoggedOut);
    }
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

    auto validation = std::make_shared<TikTokAccountResult>();
    auto job = TikTokWorkerService::instance().submit(
        this,
        [candidate, validation](const std::shared_ptr<TikTokJobContext> &) {
            StreamlabsTikTokClient probe(candidate);
            *validation = probe.GetAccountInfo();
        },
        [this, candidate, validation](const std::shared_ptr<TikTokJobContext> &) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                restoring_ = false;
            }
            switch (ClassifyCredentialResult(*validation)) {
            case CredentialValidation::Valid:
            case CredentialValidation::ValidLiveForbidden: {
                // 403 migrates too: the credential is kept and the
                // session authenticates without LIVE access.
                TikTokAccountInfo account = validation->account;
                if (!validation->success) {
                    account = TikTokAccountInfo{};
                    account.liveEligibilityKnown = true;
                    account.canGoLive = false;
                }
                saveMigratedCredential(candidate, account);
                return;
            }
            case CredentialValidation::InvalidCredential:
                blog(LOG_INFO, TAG "TikTok: legacy token invalid, requiring login");
                SaveMultiOutputConfig();
                setStatus(Status::LoggedOut);
                return;
            case CredentialValidation::TemporaryFailure:
                break;
            }
            // Transient failure: keep the candidate for a bounded retry
            // instead of destroying a possibly good token.
            int retries = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pendingLegacy_.insert(pendingLegacy_.begin(), candidate);
                retries = ++migrationRetries_;
            }
            if (retries > kMaxMigrationRetries) {
                blog(LOG_INFO, TAG "TikTok: legacy migration gave up after transient failures");
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pendingLegacy_.clear();
                }
                SaveMultiOutputConfig();
                setStatus(Status::LoggedOut);
                return;
            }
            blog(LOG_INFO, TAG "TikTok: legacy migration deferred (temporary failure)");
            setStatus(Status::NetworkUnknown);
            QTimer::singleShot(kMigrationRetryMs, deferredContext_, [this]() { pumpLegacyMigration(); });
        },
        false);
    if (!job) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingLegacy_.insert(pendingLegacy_.begin(), candidate);
        restoring_ = false;
    }
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

    QObject::connect(dialog_, &QObject::destroyed, this, [this, attempt]() {
        if (loginActive_ && attempt == loginAttempt_) cancelLogin();
    });

    stopLoginTimers();
    overallTimeout_ = new QTimer(this);
    overallTimeout_->setSingleShot(true);
    QObject::connect(overallTimeout_, &QTimer::timeout, this, [this, attempt]() {
        finishLoginAttempt(attempt, QString::fromUtf8(obs_module_text("TikTok.LoginTimeout")));
    });
    overallTimeout_->start(kOverallTimeoutMs);

    lastPolledUrl_ = QUrl();
    authUrlPollInProgress_ = false;
    authUrlPollTimer_ = new QTimer(this);
    authUrlPollTimer_->setInterval(1500);
    QObject::connect(authUrlPollTimer_, &QTimer::timeout, this, [this, attempt]() {
        if (!loginActive_ || attempt != loginAttempt_ || !dialog_ || authUrlPollInProgress_)
            return;
        const auto current = dialog_->currentUrl();
        if (current != lastPolledUrl_) {
            lastPolledUrl_ = current;
            onDialogUrl(attempt, current);
        }
        // Read the document URL too: history.pushState/replaceState may
        // change SPA location without a normal navigation notification.
        if (!dialog_) return;
        authUrlPollInProgress_ = true;
        QPointer<TikTokAuthManager> self(this);
        dialog_->runJavaScript(QStringLiteral("window.location.href"),
            [self, attempt](const QVariant &result) {
                if (!self || !self->loginActive_ || attempt != self->loginAttempt_) return;
                self->authUrlPollInProgress_ = false;
                const QUrl url(result.toString());
                if (!url.isEmpty() && url != self->lastPolledUrl_) {
                    self->lastPolledUrl_ = url;
                    self->onDialogUrl(attempt, url);
                }
            });
    });
    authUrlPollTimer_->start();
    blog(LOG_INFO, TAG "[TikTokAuth] Browser opened");

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
    std::string dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped = token_;
        token_.clear();
        account_.reset();
        authEpoch_++;
        loginActive_ = false;
        reauthing_ = false;
        applyOpened_ = false;
    }
    // Secure-store I/O blocks on IPC: persist the removal off-UI. The
    // compare-and-delete keeps a newer login's credential safe.
    removeStoredCredentialAsync(dropped);
    TikTokAuthDialog::clearPersistentData();
    blog(LOG_INFO, TAG "TikTok: logged out");
    setStatus(Status::LoggedOut);
    emit loggedOut();
}

void TikTokAuthManager::handleAuthFailure(uint64_t epoch)
{
    std::string rejected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch != authEpoch_ || token_.empty())
            return;
        rejected = token_;
        token_.clear();
        account_.reset();
        authEpoch_++;
        applyOpened_ = false;
    }
    removeStoredCredentialAsync(rejected);
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
    auto result = std::make_shared<TikTokAccountResult>();
    auto job = TikTokWorkerService::instance().submit(
        this,
        [token = std::move(token), result](const std::shared_ptr<TikTokJobContext> &) {
            StreamlabsTikTokClient probe(token);
            *result = probe.GetAccountInfo();
        },
        [this, result, epoch](const std::shared_ptr<TikTokJobContext> &) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                refreshing_ = false;
                if (epoch != authEpoch_)
                    return; // stale: a newer session exists
                lastRefresh_ = std::chrono::steady_clock::now();
            }
            switch (ClassifyCredentialResult(*result)) {
            case CredentialValidation::Valid: {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (account_.has_value() && !result->account.liveEligibilityKnown &&
                        account_->liveEligibilityKnown) {
                        // A refresh without eligibility fields must not
                        // wipe a positively observed state.
                        result->account.liveEligibilityKnown = true;
                        result->account.canGoLive = account_->canGoLive;
                        result->account.liveUnavailableReason =
                            account_->liveUnavailableReason;
                        if (result->account.applicationStatus.empty())
                            result->account.applicationStatus =
                                account_->applicationStatus;
                    }
                    account_ = result->account;
                    if (account_->liveEligibilityKnown && account_->canGoLive)
                        applyOpened_ = false;
                }
                emit authenticationChanged();
                return;
            }
            case CredentialValidation::ValidLiveForbidden:
                noteLiveAccessDenied(epoch);
                return;
            case CredentialValidation::InvalidCredential:
                handleAuthFailure(epoch);
                return;
            case CredentialValidation::TemporaryFailure:
                // Transient failures: keep everything, the timer retries.
                return;
            }
        },
        false);
    if (!job) {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshing_ = false;
    }
}

bool TikTokAuthManager::credentialEpochCurrent(uint64_t epoch) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch == authEpoch_;
}

void TikTokAuthManager::removeStoredCredentialAsync(std::string expectedToken)
{
    ICredentialStore *secure = secureStore_ ? secureStore_.get() : nullptr;
    ICredentialStore *memory = memoryStore_.get();
    TikTokWorkerService::instance().submit(
        this,
        [secure, memory, expectedToken = std::move(expectedToken)](
            const std::shared_ptr<TikTokJobContext> &) {
            // Serialized with async saves: the load-compare-remove is
            // atomic against a newer login persisting its credential, so
            // a stale removal can never destroy the live credential.
            std::lock_guard<std::mutex> lock(g_storeMutex);
            ICredentialStore *stores[] = {secure, memory};
            for (ICredentialStore *store : stores) {
                if (store == nullptr)
                    continue;
                if (!expectedToken.empty()) {
                    const auto stored = store->load();
                    if (!stored.has_value() || *stored != expectedToken)
                        continue;
                }
                store->remove();
            }
        },
        TikTokWorkerService::Done(), false);
}

void TikTokAuthManager::saveMigratedCredential(const std::string &candidate,
                                              const TikTokAccountInfo &account)
{
    ICredentialStore *primary = secureStore_ ? secureStore_.get() : memoryStore_.get();
    ICredentialStore *memory = memoryStore_.get();
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch = authEpoch_;
    }
    struct SaveOutcome {
        bool secureSaved = false;
        bool memorySaved = false;
    };
    auto outcome = std::make_shared<SaveOutcome>();
    auto job = TikTokWorkerService::instance().submit(
        this,
        [this, primary, memory, candidate, outcome, epoch](const std::shared_ptr<TikTokJobContext> &) {
            if (!credentialEpochCurrent(epoch))
                return;
            std::lock_guard<std::mutex> lock(g_storeMutex);
            outcome->secureSaved = primary->save(candidate);
            if (!outcome->secureSaved && primary != memory)
                outcome->memorySaved = memory->save(candidate);
        },
        [this, candidate, account, outcome, epoch](const std::shared_ptr<TikTokJobContext> &) {
            if (!credentialEpochCurrent(epoch)) {
                // Superseded after our save landed: drop only our own
                // value, never a newer session's credential.
                removeStoredCredentialAsync(candidate);
                return;
            }
            if (outcome->secureSaved && secureStore_ != nullptr)
                activeStore_ = secureStore_.get();
            else
                activeStore_ = memoryStore_.get();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                token_ = candidate;
                account_ = account;
                authEpoch_++;
                migrationRetries_ = 0;
            }
            blog(LOG_INFO, TAG "TikTok: legacy token migrated");
            SaveMultiOutputConfig();
            setStatus(Status::Authenticated);
            emit loginSucceeded();
            scheduleAccessRefresh();
            if (!secureStoragePersistent()) {
                QMessageBox::warning(nullptr, QString::fromUtf8(obs_module_text("TikTok.Settings")),
                                     QString::fromUtf8(obs_module_text("TikTok.SecureStorageUnavailable")));
            }
        },
        false);
    if (!job)
        blog(LOG_INFO, TAG "TikTok: legacy migration dropped during shutdown");
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
    blog(LOG_DEBUG, TAG "[TikTokAuth] Navigation observed");

    QString code;
    const auto action = flow_->onNavigate(url, &code);
    switch (action) {
    case TikTokAuthFlow::NavAction::ScheduleRecheck:
        QTimer::singleShot(kTikTokRecheckDelayMs, deferredContext_, [this, attempt]() {
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
        blog(LOG_INFO, TAG "[TikTokAuth] Authorization code detected; token exchange started");
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
            blog(LOG_DEBUG, TAG "[TikTokAuth] Token exchange retry requested in committed document");
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
        blog(LOG_INFO, TAG "[TikTokAuth] Token received; token validation started");
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
    auto validation = std::make_shared<TikTokAccountResult>();
    auto job = TikTokWorkerService::instance().submit(
        this,
        [candidateToken, validation](const std::shared_ptr<TikTokJobContext> &) {
            StreamlabsTikTokClient probe(candidateToken);
            *validation = probe.GetAccountInfo();
        },
        // Both submit arguments capture the token by value. Moving it in
        // either argument can empty the other capture: argument evaluation
        // order is unspecified.
        [this, attempt, candidateToken, validation, isReauth](
            const std::shared_ptr<TikTokJobContext> &) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!loginActive_ || attempt != loginAttempt_)
                    return; // superseded: discard candidate
            }
            switch (ClassifyCredentialResult(*validation)) {
            case CredentialValidation::Valid:
                acceptValidatedLogin(attempt, candidateToken, *validation);
                return;
            case CredentialValidation::ValidLiveForbidden: {
                // 403: the credential is valid, the account just lacks
                // LIVE access. Authenticate without eligibility.
                TikTokAccountResult account;
                account.success = true;
                account.account.liveEligibilityKnown = true;
                account.account.canGoLive = false;
                acceptValidatedLogin(attempt, candidateToken, account);
                return;
            }
            case CredentialValidation::InvalidCredential:
            case CredentialValidation::TemporaryFailure:
                break;
            }
            QString message;
            blog(LOG_WARNING, TAG "[TikTokAuth] Token validation failed (failure=%d)",
                 static_cast<int>(validation->failure));
            if (validation->failure == TikTokFailure::Network ||
                validation->failure == TikTokFailure::Timeout) {
                message = QString::fromUtf8(obs_module_text("Error.TikTokNetwork"));
            } else {
                message = QString::fromUtf8(obs_module_text("TikTok.LoginFailed"));
            }
            Q_UNUSED(isReauth);
            finishLoginAttempt(attempt, message);
        },
        false);
    if (!job) {
        // Shutdown raced validation: drop the attempt silently. A
        // re-login keeps its previous session via finishLoginAttempt.
        finishLoginAttempt(attempt, QString());
    }
}

void TikTokAuthManager::acceptValidatedLogin(uint64_t attempt, const std::string &candidateToken,
                                            TikTokAccountResult account)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loginActive_ || attempt != loginAttempt_)
            return;
    }
    // The candidate validated: persist it off-UI (secure-store I/O blocks
    // on IPC), then complete the login from the save continuation.
    saveLoginCredential(attempt, candidateToken, std::move(account));
}

void TikTokAuthManager::saveLoginCredential(uint64_t attempt, const std::string &candidateToken,
                                           TikTokAccountResult account)
{
    ICredentialStore *primary = secureStore_ ? secureStore_.get() : memoryStore_.get();
    ICredentialStore *memory = memoryStore_.get();
    QPointer<QWidget> notifyParent = dialog_ ? dialog_->parentWidget() : nullptr;
    struct SaveOutcome {
        bool secureSaved = false;
    };
    auto outcome = std::make_shared<SaveOutcome>();
    auto job = TikTokWorkerService::instance().submit(
        this,
        [primary, memory, candidateToken, outcome](const std::shared_ptr<TikTokJobContext> &) {
            std::lock_guard<std::mutex> lock(g_storeMutex);
            outcome->secureSaved = primary->save(candidateToken);
            if (!outcome->secureSaved && primary != memory)
                memory->save(candidateToken);
        },
        [this, attempt, candidateToken, account = std::move(account), outcome, notifyParent](
            const std::shared_ptr<TikTokJobContext> &) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!loginActive_ || attempt != loginAttempt_) {
                    // Superseded (logout/cancel raced the save) after our
                    // save landed: drop only our own value, never a newer
                    // session's credential.
                    removeStoredCredentialAsync(candidateToken);
                    return;
                }
            }
            if (outcome->secureSaved && secureStore_ != nullptr)
                activeStore_ = secureStore_.get();
            else
                activeStore_ = memoryStore_.get();
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
                QMessageBox::warning(notifyParent,
                                     QString::fromUtf8(obs_module_text("TikTok.Settings")),
                                     QString::fromUtf8(
                                         obs_module_text("TikTok.SecureStorageUnavailable")));
            }
        },
        false);
    if (!job) {
        // Shutdown raced the save: drop the attempt silently. A
        // re-login keeps its previous session via finishLoginAttempt.
        finishLoginAttempt(attempt, QString());
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
    // Scrub the live WebView session before teardown so a failed or
    // cancelled attempt cannot re-persist its cookies on destruction.
    scrubDialogSession();
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
    if (authUrlPollTimer_) {
        authUrlPollTimer_->stop();
        authUrlPollTimer_->deleteLater();
        authUrlPollTimer_ = nullptr;
    }
    authUrlPollInProgress_ = false;
    lastPolledUrl_ = QUrl();
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

void TikTokAuthManager::shutdown()
{
    const std::vector<QTimer *> timers = {authUrlPollTimer_, pollTimer_, exchangeTimeout_, overallTimeout_, refreshTimer_};
    auto *loginDialog = dialog_.data();
    auto *applyDialog = applyDialog_.data();
    stopLoginTimers();
    if (refreshTimer_)
        refreshTimer_->stop();
    // Invalidate every in-flight attempt first: any completion that
    // still runs finds loginActive_ cleared and discards its candidate.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loginActive_ = false;
        reauthing_ = false;
        restoring_ = false;
        refreshing_ = false;
        token_.clear();
        account_.reset();
        status_ = Status::LoggedOut;
        ++authEpoch_;
    }
    teardownDialog();
    if (applyDialog_)
        applyDialog_->close();
    flow_.reset();
    // Destroy browser callbacks and timer functors while the plugin is
    // still mapped; deleteLater alone is insufficient during unload.
    delete loginDialog;
    delete applyDialog;
    for (auto *timer : timers) delete timer;
    refreshTimer_ = nullptr;
    delete deferredContext_;
    deferredContext_ = nullptr;
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    // Joins every worker thread (cancelling normal jobs, running cleanup
    // jobs to completion), so no plugin code runs after module unload.
    TikTokWorkerService::instance().shutdown();
}
