#pragma once

// Global TikTok authentication manager. TikTok login is an account, not a
// per-target setting: every TikTok output shares this single session.
//
// Lifecycle:
//   exchange -> candidate token (memory only) -> GET /info validation ->
//   LIVE eligibility inspection -> secure-store save -> authenticated.
//
// A candidate token is never persisted before validation, and a failed or
// cancelled re-login never destroys the previous valid credential.
//
// Threading: all public methods run on the Qt UI thread. Streamlabs API
// calls run on worker threads; results return via queued invocations
// guarded by attempt/generation ids so stale callbacks do nothing.

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "tiktok/auth-models.h"

struct TikTokAccountResult;
class ICredentialStore;
class StreamlabsTikTokClient;
class TikTokAuthDialog;
class TikTokAuthFlow;
class QTimer;
class QWidget;

// Tracks in-flight TikTok activity (async start preparation + active LIVE
// sessions) so logout cannot orphan a session or race a pending start.
// Push widgets register on preparation start and unregister when the LIVE
// is ended or preparation settles.
namespace TikTokLiveRegistry {
void registerActivity();
void unregisterActivity();
bool hasActiveActivity();
} // namespace TikTokLiveRegistry

class TikTokAuthManager : public QObject {
    Q_OBJECT

public:
    enum class Status {
        LoggedOut,
        LoggingIn,
        Validating,
        Authenticated,
        SessionExpired,
        NetworkUnknown,
    };

    static TikTokAuthManager &instance();

    bool isAuthenticated() const;
    // True when LIVE access is positively known: the API reported
    // eligibility or a LIVE was started with this session. Auth and
    // eligibility stay separate: authenticated but unknown/unavailable
    // is not granted.
    bool isLiveAccessGranted() const;
    std::optional<TikTokAccountInfo> account() const;
    std::optional<TikTokUserProfile> profile() const;
    Status status() const;
    QString statusText() const;

    // Authenticated API client for the current session, or nullptr when
    // no credential is held (logged out / expired). Valid while the
    // manager lives; safe to use from worker threads.
    std::shared_ptr<StreamlabsTikTokClient> client() const;

    bool secureStorageAvailable() const;
    bool secureStoragePersistent() const;
    bool webEngineAvailable() const;

    // Loads the stored credential and validates it with GET /info:
    // valid -> authenticated; 401/403 -> credential removed; network or
    // server failure -> credential retained, status temporarily unknown.
    void restoreSession();

    // Starts an interactive login. When already authenticated this is an
    // explicit reauthentication: the old credential survives until the
    // new one validates.
    void login(QWidget *parent);
    void cancelLogin();
    void logout(QWidget *parent = nullptr);

    // Opens TikTok's official LIVE-access application page in a
    // pass-through embedded browser for the user to drive themselves.
    // No auth state machine is attached: the plugin never submits
    // applications or reads their result.
    void openLiveAccessApplication(QWidget *parent);

    // Called when an API request reports 401 (invalid credential).
    // Only acts when the epoch matches the current session, so stale
    // clients cannot log out a newer login.
    void handleAuthFailure(uint64_t epoch);

    // LIVE-access observations from API use (epoch-guarded like above).
    // A 403 keeps the session: the credential is valid, the account is
    // just not LIVE-eligible. A successful start proves eligibility.
    void noteLiveStartSucceeded(uint64_t epoch);
    void noteLiveAccessDenied(uint64_t epoch);

    // Background re-validation of profile + eligibility with our own
    // authorized GET /info (guarded against overlap and refresh storms).
    // Also runs on a timer while authenticated so the status and the
    // Apply button follow TikTok-side approvals automatically.
    void refreshAccessState();

    // One-time migration of legacy plaintext tokens from
    // obs-multi-rtmp.json: each candidate is validated before it may
    // replace the stored credential. Never logs tokens.
    void migrateLegacyTokens(std::vector<std::string> tokens);

signals:
    void authenticationChanged();
    void stateChanged();
    void loginStarted();
    void loginSucceeded();
    void loginFailed(QString error);
    void loggedOut();

private:
    explicit TikTokAuthManager(QObject *parent = nullptr);
    ~TikTokAuthManager() override;

    TikTokAuthManager(const TikTokAuthManager &) = delete;
    TikTokAuthManager &operator=(const TikTokAuthManager &) = delete;

    void setStatus(Status status);
    void finishLoginAttempt(uint64_t attempt, const QString &error);
    void acceptValidatedLogin(uint64_t attempt, const std::string &candidateToken,
                              TikTokAccountResult account);
    void validateCandidateOnWorker(uint64_t attempt, std::string candidateToken, bool isReauth);
    void startExchange(uint64_t attempt, const QString &code, const QString &expectedPage);
    void runExchangeStarter(uint64_t attempt);
    void pollExchange(uint64_t attempt);
    void onDialogUrl(uint64_t attempt, const QUrl &url);
    void onDialogLoad(uint64_t attempt, bool ok, const QUrl &url);
    void stopLoginTimers();
    void teardownDialog();
    void scrubDialogSession();
    void pumpLegacyMigration();
    void onApplyUrl(const QUrl &url);
    void onApplyClosed();
    void scheduleAccessRefresh();

    mutable std::mutex mutex_;
    std::string token_; // SECRET: memory only, never logged
    std::optional<TikTokAccountInfo> account_;
    Status status_ = Status::LoggedOut;
    uint64_t authEpoch_ = 0;
    bool restoring_ = false;
    uint64_t restoreGen_ = 0;
    // Legacy plaintext tokens awaiting one-time migration. Validated only
    // when no stored session exists; otherwise discarded with a config
    // sanitize. Never logged.
    std::vector<std::string> pendingLegacy_;

    // True once the user opened the LIVE-access application page this
    // session without access being granted since. Drives the pending
    // status line; reset on logout.
    bool applyOpened_ = false;
    bool refreshing_ = false;
    std::chrono::steady_clock::time_point lastRefresh_{};

    std::unique_ptr<ICredentialStore> secureStore_;
    std::unique_ptr<ICredentialStore> memoryStore_;
    ICredentialStore *activeStore_ = nullptr;

    // Active interactive attempt (UI thread only).
    uint64_t loginAttempt_ = 0;
    bool loginActive_ = false;
    bool reauthing_ = false;
    std::unique_ptr<TikTokAuthFlow> flow_;
    TikTokAuthDialog *dialog_ = nullptr;
    TikTokAuthDialog *applyDialog_ = nullptr;
    QTimer *pollTimer_ = nullptr;
    QTimer *exchangeTimeout_ = nullptr;
    QTimer *overallTimeout_ = nullptr;
    QTimer *refreshTimer_ = nullptr;
};
