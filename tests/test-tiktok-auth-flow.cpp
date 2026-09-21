// AuthFlow state-machine tests. Pure navigation logic, no WebView.
#include <iostream>

#include <QUrl>

#include "tiktok/auth-flow.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static int TestFreshAttempt()
{
    TikTokAuthFlow flow(7);
    CHECK(flow.attemptId() == 7);
    CHECK(flow.state() == TikTokAuthFlow::State::TikTokLogin);
    CHECK(!flow.authUrl().isEmpty());
    CHECK(flow.authUrl().host() == "streamlabs.com");
    CHECK(!flow.pkce().verifier.isEmpty());
    CHECK(!flow.pkce().challenge.isEmpty());
    CHECK(flow.authUrl().toString().contains("code_challenge"));
    return 0;
}

static int TestTikTokRecheckFlow()
{
    TikTokAuthFlow flow(1);
    QString code;
    // Login page: nothing scheduled.
    CHECK(flow.onNavigate(QUrl("https://www.tiktok.com/login"), &code) ==
          TikTokAuthFlow::NavAction::None);
    // Logged-in page: exactly one recheck.
    CHECK(flow.onNavigate(QUrl("https://www.tiktok.com/@user"), &code) ==
          TikTokAuthFlow::NavAction::ScheduleRecheck);
    CHECK(flow.onNavigate(QUrl("https://www.tiktok.com/@user"), &code) ==
          TikTokAuthFlow::NavAction::None);
    // Recheck navigates to Streamlabs while still outside it.
    auto target = flow.onRecheck();
    CHECK(target.has_value());
    CHECK(target->host() == "streamlabs.com");
    CHECK(flow.state() == TikTokAuthFlow::State::StreamlabsAuth);
    CHECK(!flow.onRecheck().has_value());
    return 0;
}

static int TestRecheckStaysOnStreamlabs()
{
    TikTokAuthFlow flow(1);
    QString code;
    CHECK(flow.onNavigate(QUrl("https://www.tiktok.com/@user"), &code) ==
          TikTokAuthFlow::NavAction::ScheduleRecheck);
    // User already reached Streamlabs manually: no forced navigation.
    CHECK(flow.onNavigate(QUrl("https://www.streamlabs.com/m/login?tiktok=1"), &code) ==
          TikTokAuthFlow::NavAction::None);
    CHECK(!flow.onRecheck().has_value());
    return 0;
}

static int TestExchangeFiresOnce()
{
    TikTokAuthFlow flow(1);
    QString code;
    CHECK(flow.onNavigate(QUrl("https://streamlabs.com/dashboard?success=true&code=C1"), &code) ==
          TikTokAuthFlow::NavAction::Exchange);
    CHECK(code == "C1");
    CHECK(flow.state() == TikTokAuthFlow::State::Exchanging);
    CHECK(flow.exchangeCode() == "C1");
    CHECK(flow.exchangePage() == "streamlabs.com/dashboard");
    // Second success URL never re-fires (single-use code protection).
    QString code2;
    CHECK(flow.onNavigate(QUrl("https://streamlabs.com/dashboard?success=true&code=C2"), &code2) ==
          TikTokAuthFlow::NavAction::None);
    CHECK(code2.isEmpty());
    return 0;
}

static int TestExchangeWinsOverRecheck()
{
    TikTokAuthFlow flow(1);
    QString code;
    CHECK(flow.onNavigate(QUrl("https://www.tiktok.com/@user"), &code) ==
          TikTokAuthFlow::NavAction::ScheduleRecheck);
    // Success observed before the recheck fires: exchange wins.
    CHECK(flow.onNavigate(QUrl("https://www.streamlabs.com/dashboard?code=C9"), &code) ==
          TikTokAuthFlow::NavAction::Exchange);
    CHECK(!flow.onRecheck().has_value());
    return 0;
}

static int TestSingleReevalInSuccessDocument()
{
    TikTokAuthFlow flow(1);
    QString code;
    flow.onNavigate(QUrl("https://www.streamlabs.com/dashboard?code=C1"), &code);
    const QString page = "www.streamlabs.com/dashboard";
    CHECK(flow.shouldReeval(page));
    CHECK(!flow.shouldReeval("www.tiktok.com/authorize"));
    CHECK(!flow.shouldReeval("www.streamlabs.com/other"));
    flow.markReevalDone();
    CHECK(!flow.shouldReeval(page));
    return 0;
}

static int TestReevalBlockedAfterSettle()
{
    TikTokAuthFlow flow(1);
    QString code;
    flow.onNavigate(QUrl("https://www.streamlabs.com/dashboard?code=C1"), &code);
    flow.markExchangeComplete();
    CHECK(!flow.shouldReeval("www.streamlabs.com/dashboard"));
    CHECK(flow.exchangeComplete());
    return 0;
}

static int TestForeignCodesIgnored()
{
    TikTokAuthFlow flow(1);
    QString code = "SENTINEL";
    CHECK(flow.onNavigate(QUrl("https://evil.com/dashboard?success=true&code=C1"), &code) ==
          TikTokAuthFlow::NavAction::None);
    CHECK(code == "SENTINEL");
    CHECK(flow.state() == TikTokAuthFlow::State::TikTokLogin);
    return 0;
}

int main()
{
    if (TestFreshAttempt())
        return 1;
    if (TestTikTokRecheckFlow())
        return 1;
    if (TestRecheckStaysOnStreamlabs())
        return 1;
    if (TestExchangeFiresOnce())
        return 1;
    if (TestExchangeWinsOverRecheck())
        return 1;
    if (TestSingleReevalInSuccessDocument())
        return 1;
    if (TestReevalBlockedAfterSettle())
        return 1;
    if (TestForeignCodesIgnored())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
