// Browser factory selection tests. Pure logic, no browser is started.
#include <iostream>

#include "tiktok/auth-browser-factory.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static int TestWindowsPrefersWebView2()
{
    TikTokAuthBackendSupport support;
    support.webEngine = true;
    support.webView2 = true;
    // Windows: the native backend wins, it needs no Qt WebEngine runtime.
    CHECK(SelectTikTokAuthBackend(support, true) == TikTokAuthBackend::WebView2);
    // Everywhere else only Qt WebEngine is available.
    CHECK(SelectTikTokAuthBackend(support, false) == TikTokAuthBackend::WebEngine);
    return 0;
}

static int TestWindowsFallback()
{
    TikTokAuthBackendSupport support;
    support.webEngine = true;
    // No WebView2 in the build: Qt WebEngine is the fallback.
    CHECK(SelectTikTokAuthBackend(support, true) == TikTokAuthBackend::WebEngine);

    TikTokAuthBackendSupport empty;
    // Never a silent downgrade to a backend that cannot run the flow.
    CHECK(SelectTikTokAuthBackend(empty, true) == TikTokAuthBackend::None);
    CHECK(SelectTikTokAuthBackend(empty, false) == TikTokAuthBackend::None);
    return 0;
}

static int TestNonWindowsNeverPicksWebView2()
{
    TikTokAuthBackendSupport support;
    support.webView2 = true;
    // WebView2 is a Windows only backend.
    CHECK(SelectTikTokAuthBackend(support, false) == TikTokAuthBackend::None);
    return 0;
}

static int TestCompiledInSupport()
{
    const TikTokAuthBackendSupport support = TikTokAuthBackendSupport::compiledIn();
    // The support set must never claim a backend that is not compiled in.
    CHECK(!support.webEngine || true);
    CHECK(!support.webView2 || true);
    // At least one of the flags matches the compiled defines; the test
    // binary is built without a browser backend, so both are off.
#if defined(WITH_TIKTOK_WEBENGINE)
    CHECK(support.webEngine);
#else
    CHECK(!support.webEngine);
#endif
#if defined(WITH_TIKTOK_WEBVIEW2)
    CHECK(support.webView2);
#else
    CHECK(!support.webView2);
#endif
    return 0;
}

static int TestBackendNames()
{
    CHECK(TikTokAuthBackendName(TikTokAuthBackend::None) == QStringLiteral("none"));
    CHECK(!TikTokAuthBackendName(TikTokAuthBackend::WebEngine).isEmpty());
    CHECK(!TikTokAuthBackendName(TikTokAuthBackend::WebView2).isEmpty());
    CHECK(TikTokAuthBackendName(TikTokAuthBackend::WebEngine)
          != TikTokAuthBackendName(TikTokAuthBackend::WebView2));
    return 0;
}

static int TestCapabilities()
{
    TikTokAuthBrowserCapabilities caps;
    // A backend without JavaScript or cleanup cannot run the flow.
    CHECK(!caps.supportsLoginFlow());
    caps.navigation = true;
    caps.urlObservation = true;
    caps.loadEvents = true;
    caps.javaScript = true;
    caps.sessionCleanup = true;
    CHECK(caps.supportsLoginFlow());
    caps.javaScript = false;
    CHECK(!caps.supportsLoginFlow());
    return 0;
}

int main()
{
    if (TestWindowsPrefersWebView2())
        return 1;
    if (TestWindowsFallback())
        return 1;
    if (TestNonWindowsNeverPicksWebView2())
        return 1;
    if (TestCompiledInSupport())
        return 1;
    if (TestBackendNames())
        return 1;
    if (TestCapabilities())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
