#pragma once

// WebView abstraction for interactive TikTok login. The authentication
// state machine talks only to this interface; the backend specifics
// (Qt WebEngine, native WebView2) live in separate implementations so
// platforms without Qt WebEngine can provide a native backend without
// contaminating auth logic.
//
// The browser is only a renderer, cookie/session container and JS
// execution environment. All auth decisions stay in C++.
//
// Lifecycle contract shared by every backend:
//
//   initialize()          -> asynchronous; emits ready() or failed()
//   navigate(url)         -> queued until the backend is ready
//   runJavaScript(code)   -> callback receives a QVariant with the same
//                             shape Qt WebEngine's runJavaScript() returns
//   clearSessionData()    -> scrubs cookies/cache/storage
//   shutdown()            -> releases backend resources, idempotent
//
// Backends must never emit ready() or failed() more than once, and must
// not touch the authentication state machine: they only report what the
// page did.

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <functional>

class QWidget;

// Concrete browser backends the plugin can be built with.
enum class TikTokAuthBackend {
    None,
    WebEngine,
    WebView2,
};

// Human readable backend name for logs and the settings UI.
QString TikTokAuthBackendName(TikTokAuthBackend backend);

// Capabilities a backend must provide to run the TikTok login flow.
// The flow needs navigation, URL observation, load events, JavaScript
// execution and session cleanup; a backend missing any of them can never
// satisfy the authentication contract and must not be selected.
struct TikTokAuthBrowserCapabilities {
    bool navigation = false;
    bool urlObservation = false;
    bool loadEvents = false;
    bool javaScript = false;
    bool sessionCleanup = false;

    bool supportsLoginFlow() const
    {
        return navigation && urlObservation && loadEvents && javaScript && sessionCleanup;
    }
};

class TikTokAuthBrowser : public QObject {
    Q_OBJECT

public:
    explicit TikTokAuthBrowser(QObject *parent = nullptr);
    ~TikTokAuthBrowser() override;

    virtual TikTokAuthBackend backend() const = 0;
    virtual QString backendName() const = 0;
    virtual TikTokAuthBrowserCapabilities capabilities() const = 0;

    // Starts asynchronous initialization. Implementations emit ready() or
    // failed() exactly once; initialize() must never block the UI thread.
    virtual void initialize() = 0;

    // True once the backend finished initializing and can navigate.
    bool isReady() const { return ready_; }
    // True once initialization failed; the browser must not be used.
    bool hasFailed() const { return failed_; }

    // Navigates to the URL. The navigation is queued while the backend is
    // still initializing, so callers never have to wait for readiness.
    void navigate(const QUrl &url);

    virtual QUrl currentUrl() const = 0;
    virtual void runJavaScript(const QString &code, std::function<void(const QVariant &)> callback) = 0;
    virtual QWidget *widget() = 0;
    // Clears session data (cookies, HTTP cache, local storage) on the live
    // profile. Must run before the owning view is destroyed so nothing is
    // re-persisted on teardown.
    virtual void clearSessionData() = 0;
    // Releases backend resources (COM objects, browser processes, profiles).
    // Idempotent and safe to call from a destructor.
    virtual void shutdown() {}

signals:
    void urlObserved(const QUrl &url);
    void loadFinished(bool ok, const QUrl &url);
    // Emitted once the backend can accept navigation and JavaScript.
    void ready();
    // Emitted once when the backend cannot be used (missing runtime,
    // initialization error, crashed browser process).
    void failed(const QString &error);

protected:
    // Marks the backend ready and flushes a queued navigation.
    void markReady();
    // Marks the backend unusable; later ready() calls are ignored.
    void markFailed(const QString &error);
    // Backend specific navigation, only called once the backend is ready.
    virtual void navigateNow(const QUrl &url) = 0;

private:
    void flushPendingNavigation();

    bool ready_ = false;
    bool failed_ = false;
    bool hasPendingUrl_ = false;
    QUrl pendingUrl_;
};
