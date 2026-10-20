#pragma once

// Embedded login dialog. Hosts the TikTok/Streamlabs authentication pages
// in a dedicated, hardened browser profile:
//
//   <OBS config>/obs-multi-rtmp/tiktok-auth-webview/
//
// The concrete browser backend (Qt WebEngine or the native Edge WebView2)
// is chosen by the browser factory, so the dialog itself stays backend
// agnostic.
//
// Closing the dialog cancels the active attempt; it never touches a
// previously valid credential. When no browser backend is available in the
// build, the dialog shows a packaging notice and reports failure.

#include <QDialog>
#include <QUrl>
#include <QVariant>

#include <functional>
#include <memory>

class TikTokAuthBrowser;
class QLabel;
class QStackedWidget;

class TikTokAuthDialog : public QDialog {
    Q_OBJECT

public:
    // Empty title keeps the default login caption; pass-through browsers
    // (e.g. the LIVE-access application page) set their own.
    explicit TikTokAuthDialog(QWidget *parent = nullptr, const QString &title = QString());
    ~TikTokAuthDialog() override;

    bool browserAvailable() const { return browser_ != nullptr; }
    // True once the browser backend finished initializing.
    bool isBrowserReady() const;
    // Name of the active browser backend, for logs and error messages.
    QString browserBackendName() const;

    QUrl currentUrl() const;
    void navigate(const QUrl &url);
    void runJavaScript(const QString &code, std::function<void(const QVariant &)> callback);
    void clearSessionData();

    // Clears session data of the dedicated auth profile (cookies, HTTP
    // cache). Best effort; safe to call with no live dialog.
    static void clearPersistentData();

    static QString profileStoragePath();

signals:
    void urlObserved(const QUrl &url);
    void pageLoadFinished(bool ok, const QUrl &url);
    // The browser backend finished initializing and can navigate.
    void browserReady();
    // The browser backend cannot be used (missing runtime, crash, ...).
    void browserFailed(const QString &error);

private:
    std::unique_ptr<TikTokAuthBrowser> browser_;
    QStackedWidget *stack_ = nullptr;
    QLabel *notice_ = nullptr;
};
