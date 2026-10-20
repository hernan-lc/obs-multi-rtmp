#include "tiktok/auth-webview2.h"

#if defined(WITH_TIKTOK_WEBVIEW2)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <WebView2.h>

#undef min
#undef max

#include "obs-module.h"

#include "tiktok/auth-jsvalue.h"
#include "tiktok/auth-profile.h"
#include "tiktok/constants.h"

#include <QDir>
#include <QEvent>
#include <QPointer>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

namespace {

using Microsoft::WRL::ComPtr;

// WebView2 reports a missing Evergreen runtime as a "file not found" error.
constexpr HRESULT kRuntimeMissing = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

std::wstring WideString(const QString &text)
{
    return text.toStdWString();
}

QString FromWide(LPWSTR text)
{
    return text ? QString::fromWCharArray(text) : QString();
}

bool IsBlockedScheme(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    // The login flow never needs local files, script URLs or inline
    // documents; identity providers are reached over http(s) only.
    return scheme == QStringLiteral("file") || scheme == QStringLiteral("javascript");
}

} // namespace

struct TikTokWebView2Browser::Impl {
    // State shared with the WebView2 callbacks. The callbacks only touch
    // this object and never the browser itself, so a callback that is
    // delivered after the browser was destroyed is a harmless no-op.
    struct Shared {
        QPointer<TikTokWebView2Browser> browser;
        std::atomic<bool> shuttingDown{false};
        ComPtr<ICoreWebView2Environment> environment;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
        EventRegistrationToken navigationStarting{};
        EventRegistrationToken navigationCompleted{};
        EventRegistrationToken sourceChanged{};
        EventRegistrationToken newWindowRequested{};
        EventRegistrationToken processFailed{};
        EventRegistrationToken downloadStarting{};
        EventRegistrationToken permissionRequested{};
        EventRegistrationToken webResourceRequested{};
        QString userDataDir;
        bool handlersRegistered = false;
        bool comInitialized = false;
        bool released = false;
        bool cleanupPending = false;
        bool releaseDeferred = false;
        bool wipeRequested = false;
    };

    // Releases every WebView2 object deterministically. The controller is
    // closed first so the browser process shuts down instead of lingering.
    static void ReleaseComObjects(const std::shared_ptr<Shared> &shared)
    {
        if (!shared || shared->released)
            return;
        shared->released = true;

        if (shared->webview && shared->handlersRegistered) {
            shared->webview->remove_NavigationStarting(shared->navigationStarting);
            shared->webview->remove_NavigationCompleted(shared->navigationCompleted);
            shared->webview->remove_SourceChanged(shared->sourceChanged);
            shared->webview->remove_NewWindowRequested(shared->newWindowRequested);
            shared->webview->remove_ProcessFailed(shared->processFailed);
            ComPtr<ICoreWebView2_4> webview4;
            if (SUCCEEDED(shared->webview.As(&webview4)) && webview4)
                webview4->remove_DownloadStarting(shared->downloadStarting);
            shared->webview->remove_PermissionRequested(shared->permissionRequested);
            shared->handlersRegistered = false;
        }
        if (shared->controller) {
            shared->controller->Close();
            shared->controller.Reset();
        }
        shared->webview.Reset();
        shared->environment.Reset();
        if (shared->comInitialized) {
            shared->comInitialized = false;
            CoUninitialize();
        }
        if (shared->wipeRequested) {
            // The profile directory holds cookies, cache and local storage.
            // Removing it is the authoritative wipe; the browser process may
            // still hold a few files for a moment, so this is best effort.
            TikTokAuthProfile::clearStorage();
        }
    }

    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    QWidget *container = nullptr;
    QWidget *host = nullptr;
    bool started = false;
    bool released = false;
    QUrl currentUrl;
};

TikTokWebView2Browser::TikTokWebView2Browser(QWidget *parent)
    : TikTokAuthBrowser(parent), impl_(std::make_unique<Impl>())
{
    impl_->container = new QWidget(parent);
    impl_->container->setAttribute(Qt::WA_NativeWindow);
    impl_->container->setContextMenuPolicy(Qt::NoContextMenu);
    impl_->container->installEventFilter(this);
    // The host window (the dialog) reports activation: the browser gets the
    // keyboard focus back when the user returns to the window.
    if (auto *hostWidget = qobject_cast<QWidget *>(parent)) {
        impl_->host = hostWidget;
        hostWidget->installEventFilter(this);
    }
    impl_->shared->browser = this;
}

TikTokWebView2Browser::~TikTokWebView2Browser()
{
    impl_->shared->shuttingDown = true;
    // A pending session wipe keeps the COM objects alive through the
    // completion callback; everything else is released right away.
    if (!impl_->shared->cleanupPending)
        Impl::ReleaseComObjects(impl_->shared);
    impl_->released = true;
}

TikTokAuthBrowserCapabilities TikTokWebView2Browser::capabilities() const
{
    TikTokAuthBrowserCapabilities caps;
    caps.navigation = true;
    caps.urlObservation = true;
    caps.loadEvents = true;
    caps.javaScript = true;
    caps.sessionCleanup = true;
    return caps;
}

void TikTokWebView2Browser::initialize()
{
    if (isReady() || hasFailed() || impl_->started)
        return;
    impl_->started = true;

    // WebView2 needs a single threaded apartment on the thread that owns the
    // controller window. Qt already initializes COM on the GUI thread, so
    // this normally just bumps the reference count.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(apartment)) {
        impl_->shared->comInitialized = true;
    } else if (apartment == RPC_E_CHANGED_MODE) {
        markFailed(QString::fromUtf8(obs_module_text("TikTok.BrowserApartmentConflict")));
        return;
    } else {
        markFailed(QString::fromUtf8(obs_module_text("TikTok.BrowserInitFailed")));
        return;
    }

    // The profile lives in the user configuration directory, never inside
    // the (read-only) OBS installation folder.
    impl_->shared->userDataDir = TikTokAuthProfile::storagePath();
    if (!QDir().mkpath(impl_->shared->userDataDir)) {
        markFailed(QString::fromUtf8(obs_module_text("TikTok.BrowserInitFailed")));
        return;
    }

    HWND host = reinterpret_cast<HWND>(impl_->container->winId());
    if (host == nullptr) {
        markFailed(QString::fromUtf8(obs_module_text("TikTok.BrowserInitFailed")));
        return;
    }

    std::weak_ptr<Impl::Shared> weak = impl_->shared;
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        reinterpret_cast<LPCWSTR>(impl_->shared->userDataDir.utf16()),
        nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak, host](HRESULT envResult, ICoreWebView2Environment *environment) -> HRESULT {
                auto shared = weak.lock();
                if (!shared || shared->shuttingDown.load())
                    return S_OK;
                if (FAILED(envResult) || environment == nullptr) {
                    QString message;
                    if (envResult == kRuntimeMissing) {
                        message = QString::fromUtf8(obs_module_text("TikTok.BrowserRuntimeMissing"));
                    } else {
                        message = QString::fromUtf8(obs_module_text("TikTok.BrowserInitFailed"));
                    }
                    if (auto browser = shared->browser)
                        browser->notifyFailed(message);
                    return S_OK;
                }

                shared->environment = environment;

                const HRESULT controllerResult =
                    shared->environment->CreateCoreWebView2Controller(
                        host,
                        Microsoft::WRL::Callback<
                            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [weak](HRESULT ctlResult, ICoreWebView2Controller *ctl) -> HRESULT {
                                auto state = weak.lock();
                                if (!state || state->shuttingDown.load())
                                    return S_OK;
                                if (FAILED(ctlResult) || ctl == nullptr) {
                                    if (auto browser = state->browser) {
                                        browser->notifyFailed(
                                            QString::fromUtf8(obs_module_text(
                                                "TikTok.BrowserInitFailed")));
                                    }
                                    return S_OK;
                                }
                                state->controller = ctl;
                                if (auto browser = state->browser)
                                    browser->notifyControllerReady();
                                return S_OK;
                            })
                            .Get());
                if (FAILED(controllerResult)) {
                    if (auto browser = shared->browser) {
                        browser->notifyFailed(QString::fromUtf8(
                            obs_module_text("TikTok.BrowserInitFailed")));
                    }
                }
                return S_OK;
            })
            .Get());

    if (FAILED(result)) {
        QString message;
        if (result == kRuntimeMissing) {
            message = QString::fromUtf8(obs_module_text("TikTok.BrowserRuntimeMissing"));
        } else {
            message = QString::fromUtf8(obs_module_text("TikTok.BrowserInitFailed"));
        }
        markFailed(message);
    }
}

void TikTokWebView2Browser::notifyFailed(const QString &error)
{
    markFailed(error);
}

void TikTokWebView2Browser::notifyControllerReady()
{
    auto shared = impl_->shared;
    if (!shared || !shared->controller)
        return;

    ComPtr<ICoreWebView2> webview;
    if (FAILED(shared->controller->get_CoreWebView2(webview.GetAddressOf())) || !webview) {
        notifyFailed(QString::fromUtf8(obs_module_text("TikTok.BrowserInitFailed")));
        return;
    }
    shared->webview = webview;

    applySettings();
    registerEventHandlers();
    updateBounds();
    shared->controller->put_IsVisible(impl_->container->isVisible() ? TRUE : FALSE);

    markReady();
}

void TikTokWebView2Browser::applySettings()
{
    auto shared = impl_->shared;
    if (!shared || !shared->webview)
        return;
    ComPtr<ICoreWebView2Settings> settings;
    if (FAILED(shared->webview->get_Settings(settings.GetAddressOf())) || !settings)
        return;

    settings->put_IsScriptEnabled(TRUE);
    // No host objects and no web messaging: the page can never talk back
    // into the plugin.
    settings->put_IsWebMessageEnabled(FALSE);
    settings->put_AreHostObjectsAllowed(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_IsBuiltInErrorPageEnabled(TRUE);

    // The same frozen Streamlabs user agent the WebEngine backend uses, so
    // the provider sees an identical client on every platform.
    ComPtr<ICoreWebView2Settings2> settings2;
    if (SUCCEEDED(settings.As(&settings2)) && settings2) {
        const std::wstring agent =
            WideString(QString::fromLatin1(TikTokConstants::kStreamlabsUserAgent));
        settings2->put_UserAgent(agent.c_str());
    }
}

void TikTokWebView2Browser::registerEventHandlers()
{
    auto shared = impl_->shared;
    if (!shared || !shared->webview || shared->handlersRegistered)
        return;

    std::weak_ptr<Impl::Shared> weak = shared;

    shared->webview->add_NavigationStarting(
        Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
            [weak](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                auto state = weak.lock();
                if (!state || state->shuttingDown.load() || args == nullptr)
                    return S_OK;
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                const QUrl url(FromWide(uri));
                if (uri)
                    CoTaskMemFree(uri);
                if (IsBlockedScheme(url)) {
                    // Local content and script URLs are never part of the
                    // login flow.
                    args->put_Cancel(TRUE);
                    return S_OK;
                }
                if (auto browser = state->browser)
                    browser->handleNavigationStarting(url);
                return S_OK;
            })
            .Get(),
        &shared->navigationStarting);

    shared->webview->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [weak](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                auto state = weak.lock();
                if (!state || state->shuttingDown.load() || args == nullptr)
                    return S_OK;
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                const QUrl url = state->browser ? state->browser->currentUrl() : QUrl();
                if (auto browser = state->browser)
                    browser->handleLoadFinished(success != FALSE, url);
                return S_OK;
            })
            .Get(),
        &shared->navigationCompleted);

    // SPA applications change the location without a navigation; the
    // source change event reports those updates.
    shared->webview->add_SourceChanged(
        Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
            [weak](ICoreWebView2 *, ICoreWebView2SourceChangedEventArgs *args) -> HRESULT {
                auto state = weak.lock();
                if (!state || state->shuttingDown.load() || args == nullptr)
                    return S_OK;
                LPWSTR source = nullptr;
                if (!state->webview || FAILED(state->webview->get_Source(&source)))
                    return S_OK;
                const QUrl url(FromWide(source));
                if (source)
                    CoTaskMemFree(source);
                if (auto browser = state->browser)
                    browser->handleSourceChanged(url);
                return S_OK;
            })
            .Get(),
        &shared->sourceChanged);

    shared->webview->add_NewWindowRequested(
        Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [weak](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                auto state = weak.lock();
                if (!state || state->shuttingDown.load() || args == nullptr)
                    return S_OK;
                // Popups stay inside the login window: the plugin never
                // opens an unmanaged browser window.
                args->put_Handled(TRUE);
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                const QUrl url(FromWide(uri));
                if (uri)
                    CoTaskMemFree(uri);
                if (url.isValid() && !url.isEmpty() && state->webview &&
                    !IsBlockedScheme(url)) {
                    state->webview->Navigate(reinterpret_cast<LPCWSTR>(url.toString().utf16()));
                }
                return S_OK;
            })
            .Get(),
        &shared->newWindowRequested);

    shared->webview->add_ProcessFailed(
        Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
            [weak](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
                auto state = weak.lock();
                if (!state || state->shuttingDown.load() || args == nullptr)
                    return S_OK;
                COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                args->get_ProcessFailedKind(&kind);
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                    if (auto browser = state->browser)
                        browser->handleProcessFailed();
                }
                return S_OK;
            })
            .Get(),
        &shared->processFailed);

    // Download events were added in ICoreWebView2_4; query runtime support.
    ComPtr<ICoreWebView2_4> webview4;
    if (SUCCEEDED(shared->webview.As(&webview4)) && webview4) {
        webview4->add_DownloadStarting(
            Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>(
                [weak](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
                    auto state = weak.lock();
                    if (!state || state->shuttingDown.load() || args == nullptr)
                        return S_OK;
                    args->put_Cancel(TRUE);
                    return S_OK;
                })
                .Get(),
                &shared->downloadStarting);
    }

    // Deny every permission request (camera, microphone, geolocation, ...).
    shared->webview->add_PermissionRequested(
        Microsoft::WRL::Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [weak](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                auto state = weak.lock();
                if (!state || state->shuttingDown.load() || args == nullptr)
                    return S_OK;
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                return S_OK;
            })
            .Get(),
        &shared->permissionRequested);

    // Block local file access from the page. The filter API lives on
    // ICoreWebView2_2 and is optional at runtime.
    ComPtr<ICoreWebView2_2> webview2;
    if (SUCCEEDED(shared->webview.As(&webview2)) && webview2) {
        webview2->add_WebResourceRequested(
            Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [weak](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
                    auto state = weak.lock();
                    if (!state || state->shuttingDown.load() || args == nullptr)
                        return S_OK;
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    if (FAILED(args->get_Request(request.GetAddressOf())) || !request)
                        return S_OK;
                    LPWSTR uri = nullptr;
                    request->get_Uri(&uri);
                    const QUrl url(FromWide(uri));
                    if (uri)
                        CoTaskMemFree(uri);
                    if (!IsBlockedScheme(url))
                        return S_OK;
                    // Answer with an empty 404 instead of the local file.
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    if (state->environment &&
                        SUCCEEDED(state->environment->CreateWebResourceResponse(
                            nullptr, 404, L"Blocked", L"Content-Type: text/plain",
                            response.GetAddressOf()))) {
                        args->put_Response(response.Get());
                    }
                    return S_OK;
                })
            .Get(),
            &shared->webResourceRequested);
    }

    shared->handlersRegistered = true;
}

void TikTokWebView2Browser::handleNavigationStarting(const QUrl &url)
{
    impl_->currentUrl = url;
    emit urlObserved(url);
}

void TikTokWebView2Browser::handleSourceChanged(const QUrl &url)
{
    if (url.isEmpty())
        return;
    impl_->currentUrl = url;
    emit urlObserved(url);
}

void TikTokWebView2Browser::handleLoadFinished(bool ok, const QUrl &url)
{
    emit loadFinished(ok, url);
}

void TikTokWebView2Browser::handleProcessFailed()
{
    markFailed(QString::fromUtf8(obs_module_text("TikTok.BrowserCrashed")));
}

void TikTokWebView2Browser::updateBounds()
{
    auto shared = impl_->shared;
    if (!shared || !shared->controller)
        return;
    const qreal ratio = impl_->container->devicePixelRatioF();
    const int width = static_cast<int>(impl_->container->width() * ratio);
    const int height = static_cast<int>(impl_->container->height() * ratio);
    const RECT bounds{0, 0, width, height};
    shared->controller->put_Bounds(bounds);
}

QUrl TikTokWebView2Browser::currentUrl() const
{
    return impl_->currentUrl;
}

void TikTokWebView2Browser::navigateNow(const QUrl &url)
{
    auto shared = impl_->shared;
    if (!shared || !shared->webview)
        return;
    shared->webview->Navigate(reinterpret_cast<LPCWSTR>(url.toString().utf16()));
}

void TikTokWebView2Browser::runJavaScript(const QString &code,
                                           std::function<void(const QVariant &)> callback)
{
    auto shared = impl_->shared;
    if (!shared || !shared->webview) {
        if (callback)
            callback(QVariant());
        return;
    }

    const std::wstring script = WideString(code);
    const HRESULT result = shared->webview->ExecuteScript(
        script.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback](HRESULT errorCode, LPCWSTR json) -> HRESULT {
                QVariant value;
                if (SUCCEEDED(errorCode) && json != nullptr) {
                    // The JSON string belongs to WebView2 and is valid only
                    // for the duration of this callback.
                    value = TikTokAuthJsValue::JsonToVariant(QString::fromWCharArray(json));
                }
                if (callback)
                    callback(value);
                return S_OK;
            })
            .Get());

    if (FAILED(result) && callback)
        callback(QVariant());
}

QWidget *TikTokWebView2Browser::widget()
{
    return impl_->container;
}

void TikTokWebView2Browser::clearSessionData()
{
    auto shared = impl_->shared;
    if (!shared || !shared->webview || shared->cleanupPending)
        return;

    // The profile directory is removed when the resources are released; the
    // cookie jar is cleared right away.
    shared->wipeRequested = true;

    // Preferred path: the profile API clears cookies, cache and local
    // storage in one go.
    ComPtr<ICoreWebView2_13> webview13;
    if (SUCCEEDED(shared->webview.As(&webview13)) && webview13) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(webview13->get_Profile(profile.GetAddressOf())) && profile) {
            ComPtr<ICoreWebView2Profile2> profile2;
            if (SUCCEEDED(profile.As(&profile2)) && profile2) {
                shared->cleanupPending = true;
                // The completion keeps the COM objects alive until the wipe
                // finished, so the resources are never pulled away mid-flight.
                auto strong = shared;
                const HRESULT result = profile2->ClearBrowsingDataAll(
                    Microsoft::WRL::Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                        [strong](HRESULT) -> HRESULT {
                            strong->cleanupPending = false;
                            if (strong->releaseDeferred) {
                                strong->releaseDeferred = false;
                                Impl::ReleaseComObjects(strong);
                            }
                            return S_OK;
                        })
                        .Get());
                if (SUCCEEDED(result))
                    return;
                shared->cleanupPending = false;
            }
        }
    }

    // Fallback for older runtimes: drop the cookies at least.
    ComPtr<ICoreWebView2CookieManager> cookies;
    ComPtr<ICoreWebView2_2> webview2;
    if (SUCCEEDED(shared->webview.As(&webview2)) && webview2 &&
        SUCCEEDED(webview2->get_CookieManager(cookies.GetAddressOf())) && cookies)
        cookies->DeleteAllCookies();
}

void TikTokWebView2Browser::shutdown()
{
    auto shared = impl_->shared;
    if (!shared)
        return;
    shared->shuttingDown = true;
    if (shared->cleanupPending) {
        // Wait for the session wipe before the resources are released.
        shared->releaseDeferred = true;
        return;
    }
    Impl::ReleaseComObjects(shared);
    impl_->released = true;
}

bool TikTokWebView2Browser::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == impl_->container || watched == impl_->host) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
            updateBounds();
            if (auto shared = impl_->shared) {
                if (shared->controller) {
                    shared->controller->put_IsVisible(impl_->container->isVisible() ? TRUE : FALSE);
                }
            }
            break;
        case QEvent::WindowActivate:
            // The host window was activated: hand the keyboard focus to the
            // browser so the user can type into the login form.
            if (auto shared = impl_->shared) {
                if (shared->controller) {
                    shared->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
            }
            break;
        default:
            break;
        }
    }
    return TikTokAuthBrowser::eventFilter(watched, event);
}

#endif // WITH_TIKTOK_WEBVIEW2
