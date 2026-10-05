#pragma once

// WebView abstraction for interactive TikTok login. The authentication
// state machine talks only to this interface; the WebEngine specifics
// live in the implementation so platforms without Qt WebEngine can
// provide a separate backend without contaminating auth logic.
//
// The browser is only a renderer, cookie/session container and JS
// execution environment. All auth decisions stay in C++.

#include <QObject>
#include <QUrl>
#include <QVariant>
#include <QWidget>

#include <functional>

class TikTokAuthBrowser : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QUrl currentUrl() const = 0;
    virtual void navigate(const QUrl &url) = 0;
    virtual void runJavaScript(const QString &code, std::function<void(const QVariant &)> callback) = 0;
    virtual QWidget *widget() = 0;
    // Clears session data (cookies, HTTP cache) on the live profile.
    // Must run before the owning view is destroyed so nothing is
    // re-persisted on teardown.
    virtual void clearSessionData() = 0;

signals:
    void urlObserved(const QUrl &url);
    void loadFinished(bool ok, const QUrl &url);
};
