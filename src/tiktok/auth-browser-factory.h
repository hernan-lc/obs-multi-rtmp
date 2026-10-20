#pragma once

// Platform aware factory for the TikTok login browser.
//
// The build system decides which backends are compiled in (see
// TIKTOK_BROWSER_BACKEND in CMakeLists.txt) and the factory picks the
// best available one for the host platform:
//
//   Windows -> WebView2 (native Edge), Qt WebEngine as compiled-in fallback
//   Linux   -> Qt WebEngine
//   macOS   -> Qt WebEngine
//
// The selection never falls back to a backend that cannot satisfy the
// authentication contract: a backend missing JavaScript execution or
// session cleanup is rejected instead of silently degrading the flow.

#include "tiktok/auth-browser.h"

#include <QString>

#include <memory>

// Backends that were compiled into this build.
struct TikTokAuthBackendSupport {
    bool webEngine = false;
    bool webView2 = false;

    // Backends available in the current build.
    static TikTokAuthBackendSupport compiledIn();
};

// Pure selection rule: returns the backend to use for the host platform,
// or TikTokAuthBackend::None when no usable backend is available.
// windowsHost selects the Windows preference order.
TikTokAuthBackend SelectTikTokAuthBackend(const TikTokAuthBackendSupport &support, bool windowsHost);

namespace TikTokAuthBrowserFactory {

// True when at least one backend is compiled in and usable.
bool isAvailable();

// The backend that would be created right now.
TikTokAuthBackend availableBackend();

// Name of the selected backend, for logs and the settings UI.
QString backendName();

// User facing reason why no backend is available.
QString unavailableReason();

// Creates and starts initializing the selected backend. Returns nullptr and
// fills error when no backend is usable. The caller owns the browser.
std::unique_ptr<TikTokAuthBrowser> create(QWidget *parent, QString *error);

} // namespace TikTokAuthBrowserFactory
