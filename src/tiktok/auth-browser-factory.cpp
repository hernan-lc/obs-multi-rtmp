#include "tiktok/auth-browser-factory.h"

#if defined(WITH_TIKTOK_WEBENGINE)
#include "tiktok/auth-webengine.h"
#endif

#if defined(WITH_TIKTOK_WEBVIEW2)
#include "tiktok/auth-webview2.h"
#endif

TikTokAuthBackendSupport TikTokAuthBackendSupport::compiledIn()
{
    TikTokAuthBackendSupport support;
#if defined(WITH_TIKTOK_WEBENGINE)
    support.webEngine = true;
#endif
#if defined(WITH_TIKTOK_WEBVIEW2)
    support.webView2 = true;
#endif
    return support;
}

TikTokAuthBackend SelectTikTokAuthBackend(const TikTokAuthBackendSupport &support, bool windowsHost)
{
    if (windowsHost) {
        // WebView2 is the native Windows backend and does not depend on
        // the Qt WebEngine runtime shipped with OBS.
        if (support.webView2)
            return TikTokAuthBackend::WebView2;
        if (support.webEngine)
            return TikTokAuthBackend::WebEngine;
        return TikTokAuthBackend::None;
    }
    if (support.webEngine)
        return TikTokAuthBackend::WebEngine;
    return TikTokAuthBackend::None;
}

namespace {

bool IsWindowsHost()
{
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

} // namespace

namespace TikTokAuthBrowserFactory {

TikTokAuthBackend availableBackend()
{
    return SelectTikTokAuthBackend(TikTokAuthBackendSupport::compiledIn(), IsWindowsHost());
}

bool isAvailable()
{
    return availableBackend() != TikTokAuthBackend::None;
}

QString backendName()
{
    return TikTokAuthBackendName(availableBackend());
}

QString unavailableReason()
{
    switch (availableBackend()) {
    case TikTokAuthBackend::WebEngine:
    case TikTokAuthBackend::WebView2:
        return QString();
    case TikTokAuthBackend::None:
    default:
        break;
    }
#if defined(_WIN32) && !defined(WITH_TIKTOK_WEBVIEW2) && !defined(WITH_TIKTOK_WEBENGINE)
    return QStringLiteral(
        "This build contains no embedded login browser. The Microsoft Edge WebView2 runtime "
        "and the WebView2 SDK are required to build the Windows login backend.");
#elif !defined(WITH_TIKTOK_WEBENGINE) && !defined(WITH_TIKTOK_WEBVIEW2)
    return QStringLiteral(
        "This build contains no embedded login browser. Qt 6 WebEngine development files "
        "are required to build the login backend.");
#else
    return QStringLiteral("No usable embedded login browser is available in this build.");
#endif
}

std::unique_ptr<TikTokAuthBrowser> create(QWidget *parent, QString *error)
{
    (void)parent;
    const TikTokAuthBackend backend = availableBackend();
    std::unique_ptr<TikTokAuthBrowser> browser;

    if (backend == TikTokAuthBackend::WebEngine) {
#if defined(WITH_TIKTOK_WEBENGINE)
        browser = std::make_unique<TikTokWebEngineBrowser>(parent);
#endif
    } else if (backend == TikTokAuthBackend::WebView2) {
#if defined(WITH_TIKTOK_WEBVIEW2)
        browser = std::make_unique<TikTokWebView2Browser>(parent);
#endif
    }

    if (!browser) {
        if (error)
            *error = unavailableReason();
        return nullptr;
    }

    // A backend that cannot run the authentication flow is never used:
    // silently degrading would break the token exchange in confusing ways.
    if (!browser->capabilities().supportsLoginFlow()) {
        if (error) {
            *error = QStringLiteral("The %1 login backend does not support the required "
                                    "authentication features.")
                            .arg(browser->backendName());
        }
        return nullptr;
    }

    return browser;
}

} // namespace TikTokAuthBrowserFactory
