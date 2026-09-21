#pragma once

// One login attempt: fresh PKCE pair plus observed navigation state. Pure
// logic — every window or timer effect is returned as an action for the
// auth manager to execute, which keeps this unit-testable without a
// WebView. Ported from the reference AuthFlow with identical semantics:
//
// - OAuth success wins over everything and fires exactly once.
// - TikTok-login observation schedules a single recheck; the recheck
//   navigates to the Streamlabs auth URL only while still outside
//   Streamlabs.
// - The exchange starter may be re-run at most once, and only in the
//   committed success document (authorization codes may be single-use).
// - The settled exchange outcome is accepted exactly once.

#include <QUrl>

#include <cstdint>
#include <optional>

#include "tiktok/pkce.h"

class TikTokAuthFlow {
public:
    enum class State {
        TikTokLogin,
        StreamlabsAuth,
        Exchanging,
    };

    enum class NavAction {
        None,
        ScheduleRecheck,
        Exchange,
    };

    explicit TikTokAuthFlow(uint64_t attemptId);

    uint64_t attemptId() const { return attemptId_; }
    State state() const { return state_; }
    QUrl authUrl() const { return authUrl_; }
    const PkcePair &pkce() const { return pkce_; }
    QString lastUrl() const { return lastUrl_; }

    bool exchangeComplete() const { return exchangeComplete_; }
    // Claims the single settled outcome and scrubs the single-use code;
    // page fencing afterwards relies on exchangePage_ only.
    void markExchangeComplete()
    {
        exchangeComplete_ = true;
        exchangeCode_.clear();
    }

    // Record an observed navigation and decide the next effect. On
    // NavAction::Exchange, outCode receives the authorization code.
    NavAction onNavigate(const QUrl &url, QString *outCode);

    // A scheduled recheck fired. Returns the auth URL to navigate to
    // when the flow is still outside Streamlabs; nullopt means stay.
    std::optional<QUrl> onRecheck();

    // True when a starter re-run is still allowed: exchanging,
    // unsettled, single re-run not yet spent, and this page IS the
    // expected success document.
    bool shouldReeval(const QString &loadedPage) const;
    void markReevalDone() { reevalDone_ = true; }

    QString exchangePage() const { return exchangePage_; }
    QString exchangeCode() const { return exchangeCode_; }

private:
    uint64_t attemptId_ = 0;
    PkcePair pkce_;
    QUrl authUrl_;
    State state_ = State::TikTokLogin;
    QString lastUrl_;
    bool recheckPending_ = false;
    bool exchangeStarted_ = false;
    bool exchangeComplete_ = false;
    QString exchangeCode_; // SECRET
    QString exchangePage_; // secret-free host + path
    bool reevalDone_ = false;
};
