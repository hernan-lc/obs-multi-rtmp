#include "tiktok/auth-flow.h"
#include "tiktok/auth-url.h"

TikTokAuthFlow::TikTokAuthFlow(uint64_t attemptId)
    : attemptId_(attemptId)
    , pkce_(GeneratePkce())
{
    authUrl_ = TikTokAuthUrl::BuildStreamlabsAuthUrl(pkce_.challenge);
}

TikTokAuthFlow::NavAction TikTokAuthFlow::onNavigate(const QUrl &url, QString *outCode)
{
    // Only the scheme, host and path are remembered: the query string of
    // the authorization URL carries the single-use code and must never be
    // kept (or logged).
    lastUrl_ = url.adjusted(QUrl::RemoveQuery).toString();
    // Success wins over everything and fires exactly once.
    if (!exchangeStarted_) {
        if (auto code = TikTokAuthUrl::ExtractAuthorizationCode(url)) {
            exchangeStarted_ = true;
            state_ = State::Exchanging;
            exchangePage_ = TikTokAuthUrl::SuccessPageId(url);
            exchangeCode_ = *code;
            // The recheck path is disabled from here on, so the raw URL
            // (whose query carries the single-use code) can be scrubbed.
            lastUrl_ = exchangePage_;
            if (outCode)
                *outCode = *code;
            return NavAction::Exchange;
        }
    }
    if (state_ == State::TikTokLogin && !recheckPending_ &&
        TikTokAuthUrl::IsTikTokLoggedInUrl(url)) {
        recheckPending_ = true;
        return NavAction::ScheduleRecheck;
    }
    return NavAction::None;
}

std::optional<QUrl> TikTokAuthFlow::onRecheck()
{
    recheckPending_ = false;
    if (state_ != State::TikTokLogin || exchangeStarted_)
        return std::nullopt;
    const bool stillOutside = lastUrl_.isEmpty() || !TikTokAuthUrl::IsStreamlabsUrl(QUrl(lastUrl_));
    if (!stillOutside)
        return std::nullopt;
    state_ = State::StreamlabsAuth;
    return authUrl_;
}

bool TikTokAuthFlow::shouldReeval(const QString &loadedPage) const
{
    return state_ == State::Exchanging && !exchangeComplete_ && !reevalDone_ &&
           !exchangeCode_.isEmpty() && !exchangePage_.isEmpty() && loadedPage == exchangePage_;
}
