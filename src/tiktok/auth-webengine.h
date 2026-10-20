#pragma once

// Qt WebEngine backend for the TikTok login browser.
//
// Used on Linux and macOS (and on Windows when it is explicitly requested).
// The browser renders the login pages in a dedicated, hardened profile and
// never decides anything about the authentication itself: it only reports
// navigations, load results and JavaScript results to the auth manager.

#include "tiktok/auth-browser.h"

#include <QWidget>

class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

class TikTokWebEngineBrowser : public TikTokAuthBrowser {
    Q_OBJECT

public:
    explicit TikTokWebEngineBrowser(QWidget *parent);
    ~TikTokWebEngineBrowser() override;

    TikTokAuthBackend backend() const override { return TikTokAuthBackend::WebEngine; }
    QString backendName() const override { return TikTokAuthBackendName(TikTokAuthBackend::WebEngine); }
    TikTokAuthBrowserCapabilities capabilities() const override;

    void initialize() override;

    QUrl currentUrl() const override;
    void runJavaScript(const QString &code, std::function<void(const QVariant &)> callback) override;
    QWidget *widget() override;
    void clearSessionData() override;
    void shutdown() override;

protected:
    void navigateNow(const QUrl &url) override;

private:
    QWidget *container_ = nullptr;
    QWebEngineProfile *profile_ = nullptr;
    QWebEnginePage *page_ = nullptr;
    QWebEngineView *view_ = nullptr;
    bool released_ = false;
    bool wipeRequested_ = false;
};
