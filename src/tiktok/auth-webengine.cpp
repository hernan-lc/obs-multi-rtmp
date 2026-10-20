#include "tiktok/auth-webengine.h"

#include "tiktok/auth-profile.h"
#include "tiktok/constants.h"

#include <QLabel>
#include <QVBoxLayout>

#include <QWebEngineCookieStore>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

namespace {

class HardenedPage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(type);
        // Never let the login browser touch local content.
        if (isMainFrame && (url.scheme() == QStringLiteral("file"))) {
            return false;
        }
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }
};

} // namespace

TikTokWebEngineBrowser::TikTokWebEngineBrowser(QWidget *parent) : TikTokAuthBrowser(parent)
{
    container_ = new QWidget(parent);
    auto *layout = new QVBoxLayout(container_);
    layout->setContentsMargins(0, 0, 0, 0);
}

TikTokWebEngineBrowser::~TikTokWebEngineBrowser()
{
    shutdown();
}

TikTokAuthBrowserCapabilities TikTokWebEngineBrowser::capabilities() const
{
    TikTokAuthBrowserCapabilities caps;
    caps.navigation = true;
    caps.urlObservation = true;
    caps.loadEvents = true;
    caps.javaScript = true;
    caps.sessionCleanup = true;
    return caps;
}

void TikTokWebEngineBrowser::initialize()
{
    if (isReady() || hasFailed())
        return;

    profile_ = new QWebEngineProfile(QString::fromLatin1(TikTokConstants::kWebViewProfileName), this);
    profile_->setPersistentStoragePath(TikTokAuthProfile::storagePath());
    profile_->setHttpUserAgent(QString::fromLatin1(TikTokConstants::kStreamlabsUserAgent));
    profile_->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
    profile_->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);

    auto *settings = profile_->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    settings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);

    // Reject downloads: the login flow never needs them.
    QObject::connect(profile_, &QWebEngineProfile::downloadRequested, this,
                     [](QWebEngineDownloadRequest *download) {
                         if (download)
                             download->cancel();
                     });

    page_ = new HardenedPage(profile_, this);
    // Keep popup navigations in the same login view instead of
    // spawning windows outside the hardened dialog.
    QObject::connect(page_, &QWebEnginePage::newWindowRequested, this,
                     [this](QWebEngineNewWindowRequest &request) { request.openIn(page_); });

    view_ = new QWebEngineView(container_);
    view_->setPage(page_);
    view_->setContextMenuPolicy(Qt::NoContextMenu);

    QObject::connect(view_, &QWebEngineView::urlChanged, this,
                     [this](const QUrl &url) { emit urlObserved(url); });
    QObject::connect(view_, &QWebEngineView::loadFinished, this, [this](bool ok) {
        emit loadFinished(ok, view_ ? view_->url() : QUrl());
    });

    container_->layout()->addWidget(view_);

    markReady();
}

QUrl TikTokWebEngineBrowser::currentUrl() const
{
    return view_ ? view_->url() : QUrl();
}

void TikTokWebEngineBrowser::navigateNow(const QUrl &url)
{
    if (view_)
        view_->setUrl(url);
}

void TikTokWebEngineBrowser::runJavaScript(const QString &code,
                                           std::function<void(const QVariant &)> callback)
{
    if (page_) {
        page_->runJavaScript(code, std::move(callback));
    } else if (callback) {
        callback(QVariant());
    }
}

QWidget *TikTokWebEngineBrowser::widget()
{
    return container_;
}

void TikTokWebEngineBrowser::clearSessionData()
{
    if (!profile_)
        return;
    // The cookie deletion is asynchronous: the profile directory is removed
    // once the profile is released, which is the authoritative wipe.
    wipeRequested_ = true;
    profile_->cookieStore()->deleteAllCookies();
    profile_->cookieStore()->deleteSessionCookies();
    profile_->clearHttpCache();
}

void TikTokWebEngineBrowser::shutdown()
{
    if (released_)
        return;
    released_ = true;
    // Pages must be destroyed before their profile; the view belongs to the
    // container widget.
    delete view_;
    view_ = nullptr;
    delete page_;
    page_ = nullptr;
    delete profile_;
    profile_ = nullptr;
    if (wipeRequested_) {
        // Best effort: the browser process may still hold a few files.
        TikTokAuthProfile::clearStorage();
    }
}
