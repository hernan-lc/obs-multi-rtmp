// OAuth URL builder / classifier tests. No network; fixture codes are
// fake and never real credentials.
#include <iostream>

#include <QUrl>
#include <QUrlQuery>

#include "tiktok/auth-url.h"
#include "tiktok/constants.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static int TestAuthUrlShape()
{
    const QUrl url = TikTokAuthUrl::BuildStreamlabsAuthUrl("CHALLENGE");
    CHECK(url.scheme() == "https");
    CHECK(url.host() == "streamlabs.com");
    CHECK(url.path() == "/m/login");
    const QUrlQuery query(url);
    CHECK(query.queryItemValue("force_verify") == "1");
    CHECK(query.queryItemValue("external") == "mobile");
    CHECK(query.queryItemValue("skip_splash") == "1");
    CHECK(query.queryItemValue("tiktok") == "1");
    CHECK(query.queryItemValue("code_challenge") == "CHALLENGE");
    return 0;
}

static int TestExchangeUrlEncoding()
{
    // Values needing encoding must round-trip (proves QUrlQuery use).
    const QUrl url = TikTokAuthUrl::BuildExchangeUrl("a b+c/d", "v=v&x");
    CHECK(url.host() == "streamlabs.com");
    CHECK(url.path() == "/api/v5/slobs/auth/data");
    const QUrlQuery query(url);
    CHECK(query.queryItemValue("code") == "a b+c/d");
    CHECK(query.queryItemValue("code_verifier") == "v=v&x");
    return 0;
}

static int TestAcceptedSuccessUrls()
{
    CHECK(TikTokAuthUrl::ExtractAuthorizationCode(
              QUrl("https://streamlabs.com/dashboard?success=true&code=ABC"))
              .value_or("") == "ABC");
    CHECK(TikTokAuthUrl::ExtractAuthorizationCode(
              QUrl("https://www.streamlabs.com/dashboard?code=ABC"))
              .value_or("") == "ABC");
    CHECK(TikTokAuthUrl::ExtractAuthorizationCode(
              QUrl("https://streamlabs.com/slobs/dashboard?code=ABC"))
              .value_or("") == "ABC");
    CHECK(TikTokAuthUrl::ExtractAuthorizationCode(
              QUrl("https://www.streamlabs.com/slobs/dashboard/extra?code=ABC"))
              .value_or("") == "ABC");
    // success=true without a dashboard path is still accepted on allowlisted hosts.
    CHECK(TikTokAuthUrl::ExtractAuthorizationCode(
              QUrl("https://streamlabs.com/m/login?success=true&code=ABC"))
              .value_or("") == "ABC");
    // A dashboard path suffices even with an explicit success=false.
    CHECK(TikTokAuthUrl::ExtractAuthorizationCode(
              QUrl("https://streamlabs.com/dashboard?success=false&code=ABC"))
              .value_or("") == "ABC");
    return 0;
}

static int TestRejectedSuccessUrls()
{
    const char *rejected[] = {
        "https://evil.com/?code=ABC",
        "https://streamlabs.com.evil.com/?code=ABC",
        "https://status.streamlabs.com/?code=ABC",
        "https://ads.streamlabs.com/?code=ABC",
        "https://streamlabs.com/dashboard",
        "https://streamlabs.com/dashboard?code=",
        "https://streamlabs.com/other?code=ABC",
        "https://www.streamlabs.com/other?code=ABC",
        "https://streamlabs.com/dashboard?success=true",
    };
    for (const char *raw : rejected) {
        CHECK(!TikTokAuthUrl::ExtractAuthorizationCode(QUrl(raw)).has_value());
    }
    // success=true on a non-allowlisted host must never yield a code.
    CHECK(!TikTokAuthUrl::ExtractAuthorizationCode(
               QUrl("https://evil.com/dashboard?success=true&code=ABC"))
               .has_value());
    return 0;
}

static int TestHostClassifiers()
{
    CHECK(TikTokAuthUrl::IsTikTokUrl(QUrl("https://www.tiktok.com/login")));
    CHECK(TikTokAuthUrl::IsTikTokUrl(QUrl("https://tiktok.com/@user")));
    CHECK(!TikTokAuthUrl::IsTikTokUrl(QUrl("https://tiktok.com.evil.com/")));
    CHECK(!TikTokAuthUrl::IsTikTokUrl(QUrl("https://streamlabs.com/")));

    CHECK(TikTokAuthUrl::IsOAuthHost(QUrl("https://streamlabs.com/x")));
    CHECK(TikTokAuthUrl::IsOAuthHost(QUrl("https://www.streamlabs.com/x")));
    CHECK(!TikTokAuthUrl::IsOAuthHost(QUrl("https://ads.streamlabs.com/x")));
    CHECK(!TikTokAuthUrl::IsOAuthHost(QUrl("https://streamlabs.com.evil.com/x")));

    CHECK(TikTokAuthUrl::IsStreamlabsUrl(QUrl("https://www.streamlabs.com/x")));
    CHECK(TikTokAuthUrl::IsStreamlabsUrl(QUrl("https://ads.streamlabs.com/x")));
    return 0;
}

static int TestTikTokLoginDetection()
{
    CHECK(TikTokAuthUrl::IsTikTokLoggedInUrl(QUrl("https://www.tiktok.com/foryou")));
    CHECK(TikTokAuthUrl::IsTikTokLoggedInUrl(QUrl("https://www.tiktok.com/@user")));
    CHECK(!TikTokAuthUrl::IsTikTokLoggedInUrl(QUrl("https://www.tiktok.com/login")));
    CHECK(!TikTokAuthUrl::IsTikTokLoggedInUrl(QUrl("https://www.tiktok.com/login?x=streamlabs")));
    CHECK(!TikTokAuthUrl::IsTikTokLoggedInUrl(QUrl("https://streamlabs.com/m/login")));
    return 0;
}

static int TestLiveAccessApplyUrl()
{
    // Regression guard: the in-app application entry must stay on
    // TikTok's official page (never a lookalike host).
    const QUrl url(QString::fromLatin1(TikTokConstants::kLiveAccessApplyUrl));
    CHECK(url.scheme() == "https");
    CHECK(url.host() == "www.tiktok.com");
    CHECK(url.path().contains("live_access_pc_apply"));
    CHECK(TikTokAuthUrl::IsTikTokUrl(url));
    CHECK(!TikTokAuthUrl::IsOAuthHost(url));
    CHECK(!TikTokAuthUrl::ExtractAuthorizationCode(url).has_value());
    return 0;
}

static int TestLiveAccessResultUrl()
{
    CHECK(TikTokAuthUrl::IsLiveAccessResultUrl(
        QUrl("https://www.tiktok.com/falcon/live_g/live_access_pc_apply/result/index.html?id=GL1")));
    CHECK(TikTokAuthUrl::IsLiveAccessResultUrl(
        QUrl("https://tiktok.com/falcon/live_g/live_access_pc_apply/result/")));
    // Intro page is not the result step.
    CHECK(!TikTokAuthUrl::IsLiveAccessResultUrl(
        QUrl("https://www.tiktok.com/falcon/live_g/live_access_pc_apply/intro/index.html?id=GL1")));
    // Lookalike hosts never match.
    CHECK(!TikTokAuthUrl::IsLiveAccessResultUrl(
        QUrl("https://tiktok.com.evil.com/falcon/live_g/live_access_pc_apply/result/")));
    CHECK(!TikTokAuthUrl::IsLiveAccessResultUrl(QUrl("https://www.tiktok.com/login")));
    return 0;
}

static int TestLogSafeForms()
{
    // Query strings (code, verifier) must never appear in log output.
    CHECK(TikTokAuthUrl::LogSafeUrl(
              QUrl("https://streamlabs.com/dashboard?success=true&code=SECRET")) ==
          "streamlabs.com/dashboard");
    CHECK(TikTokAuthUrl::SuccessPageId(QUrl("https://www.streamlabs.com/slobs/dashboard?code=SECRET")) ==
          "www.streamlabs.com/slobs/dashboard");
    const QString safe = TikTokAuthUrl::LogSafeUrl(
        "https://streamlabs.com/api/v5/slobs/auth/data?code=A&code_verifier=B");
    CHECK(!safe.contains("code"));
    CHECK(!safe.contains("verifier"));
    return 0;
}

int main()
{
    if (TestAuthUrlShape())
        return 1;
    if (TestExchangeUrlEncoding())
        return 1;
    if (TestAcceptedSuccessUrls())
        return 1;
    if (TestRejectedSuccessUrls())
        return 1;
    if (TestHostClassifiers())
        return 1;
    if (TestTikTokLoginDetection())
        return 1;
    if (TestLogSafeForms())
        return 1;
    if (TestLiveAccessApplyUrl())
        return 1;
    if (TestLiveAccessResultUrl())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
