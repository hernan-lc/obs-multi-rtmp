#pragma once

// Embedded login dialog. Hosts the TikTok/Streamlabs authentication pages
// in a dedicated, hardened WebEngine profile:
//
//   <OBS config>/obs-multi-rtmp/tiktok-auth-webview/
//
// Closing the dialog cancels the active attempt; it never touches a
// previously valid credential. When Qt WebEngine is unavailable in the
// build, the dialog shows a packaging notice and reports failure.

#include <QDialog>
#include <QUrl>
#include <QVariant>

#include <functional>
#include <memory>

class TikTokAuthBrowser;

class TikTokAuthDialog : public QDialog {
    Q_OBJECT

public:
    // Empty title keeps the default login caption; pass-through browsers
    // (e.g. the LIVE-access application page) set their own.
    explicit TikTokAuthDialog(QWidget *parent = nullptr, const QString &title = QString());
    ~TikTokAuthDialog() override;

    bool browserAvailable() const { return browser_ != nullptr; }

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

private:
    TikTokAuthBrowser *browser_ = nullptr;
};
