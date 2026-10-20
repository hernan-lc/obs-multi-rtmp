#pragma once

// Native Microsoft Edge WebView2 backend for the TikTok login browser.
//
// Windows only. The controller is hosted in a native child window owned by
// a Qt widget, so the login flow works in a plain OBS installation without
// the Qt WebEngine runtime.
//
// The Evergreen WebView2 runtime is used as installed on the machine; no
// browser is bundled with the plugin.

#if defined(WITH_TIKTOK_WEBVIEW2)

#include "tiktok/auth-browser.h"

#include <QWidget>

#include <memory>

class TikTokWebView2Browser : public TikTokAuthBrowser {
    Q_OBJECT

public:
    explicit TikTokWebView2Browser(QWidget *parent);
    ~TikTokWebView2Browser() override;

    TikTokAuthBackend backend() const override { return TikTokAuthBackend::WebView2; }
    QString backendName() const override { return TikTokAuthBackendName(TikTokAuthBackend::WebView2); }
    TikTokAuthBrowserCapabilities capabilities() const override;

    void initialize() override;

    QUrl currentUrl() const override;
    void runJavaScript(const QString &code, std::function<void(const QVariant &)> callback) override;
    QWidget *widget() override;
    void clearSessionData() override;
    void shutdown() override;

    // Internal: invoked by the WebView2 host callbacks on the UI thread.
    void notifyControllerReady();
    void notifyFailed(const QString &error);
    void handleNavigationStarting(const QUrl &url);
    void handleSourceChanged(const QUrl &url);
    void handleLoadFinished(bool ok, const QUrl &url);
    void handleProcessFailed();

protected:
    void navigateNow(const QUrl &url) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Impl;

    void applySettings();
    void registerEventHandlers();
    void updateBounds();
    void releaseResources();

    std::unique_ptr<Impl> impl_;
};

#endif // WITH_TIKTOK_WEBVIEW2
