#include "tiktok/auth-dialog.h"
#include "tiktok/auth-browser.h"
#include "tiktok/auth-url.h"
#include "tiktok/constants.h"

#include <QLabel>
#include <QStandardPaths>
#include <QVBoxLayout>

#ifdef WITH_TIKTOK_WEBENGINE
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

class WebEngineBrowser : public TikTokAuthBrowser {
public:
    explicit WebEngineBrowser(QWidget *parent)
    {
        profile_ = new QWebEngineProfile(QString::fromLatin1(TikTokConstants::kWebViewProfileName),
                                         this);
        profile_->setPersistentStoragePath(TikTokAuthDialog::profileStoragePath());
        profile_->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
        profile_->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);

        auto *settings = profile_->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
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
        QObject::connect(
            page_, &QWebEnginePage::newWindowRequested, this,
            [this](QWebEngineNewWindowRequest &request) { request.openIn(page_); });

        view_ = new QWebEngineView(parent);
        view_->setPage(page_);
        view_->setContextMenuPolicy(Qt::NoContextMenu);

        QObject::connect(view_, &QWebEngineView::urlChanged, this,
                         [this](const QUrl &url) { emit urlObserved(url); });
        QObject::connect(view_, &QWebEngineView::loadFinished, this, [this](bool ok) {
            emit loadFinished(ok, view_ ? view_->url() : QUrl());
        });
    }

    void navigate(const QUrl &url) override
    {
        if (view_)
            view_->setUrl(url);
    }

    void runJavaScript(const QString &code, std::function<void(const QVariant &)> callback) override
    {
        if (page_)
            page_->runJavaScript(code, std::move(callback));
        else if (callback)
            callback(QVariant());
    }

    QWidget *widget() override { return view_; }

    void clearSessionData() override
    {
        if (!profile_)
            return;
        profile_->cookieStore()->deleteAllCookies();
        profile_->cookieStore()->deleteSessionCookies();
        profile_->clearHttpCache();
    }

private:
    QWebEngineProfile *profile_ = nullptr;
    HardenedPage *page_ = nullptr;
    QWebEngineView *view_ = nullptr;
};

} // namespace

#endif // WITH_TIKTOK_WEBENGINE

QString TikTokAuthDialog::profileStoragePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return base + QStringLiteral("/obs-studio/plugin_config/obs-multi-rtmp/") +
           QString::fromLatin1(TikTokConstants::kWebViewDirName);
}

void TikTokAuthDialog::clearPersistentData()
{
#ifdef WITH_TIKTOK_WEBENGINE
    // A short-lived profile object over the same storage name clears the
    // shared cookie jar and HTTP cache without needing a live view.
    QWebEngineProfile profile(QString::fromLatin1(TikTokConstants::kWebViewProfileName));
    profile.setPersistentStoragePath(profileStoragePath());
    profile.cookieStore()->deleteAllCookies();
    profile.cookieStore()->deleteSessionCookies();
    profile.clearHttpCache();
#else
    (void)0;
#endif
}

TikTokAuthDialog::TikTokAuthDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Login with TikTok"));
    setModal(true);
    resize(1000, 750);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

#ifdef WITH_TIKTOK_WEBENGINE
    auto *browser = new WebEngineBrowser(this);
    browser_ = browser;
    QObject::connect(browser_, &TikTokAuthBrowser::urlObserved, this,
                     &TikTokAuthDialog::urlObserved);
    QObject::connect(browser_, &TikTokAuthBrowser::loadFinished, this,
                     &TikTokAuthDialog::pageLoadFinished);
    layout->addWidget(browser->widget());
#else
    auto *notice =
        new QLabel(tr("The embedded TikTok login browser (Qt WebEngine) is not available in this "
                      "build. Please use a packaged build with WebEngine support."),
                   this);
    notice->setWordWrap(true);
    notice->setMargin(16);
    layout->addWidget(notice);
#endif
    setLayout(layout);
}

TikTokAuthDialog::~TikTokAuthDialog() = default;

void TikTokAuthDialog::navigate(const QUrl &url)
{
    if (browser_)
        browser_->navigate(url);
}

void TikTokAuthDialog::runJavaScript(const QString &code, std::function<void(const QVariant &)> callback)
{
    if (browser_)
        browser_->runJavaScript(code, std::move(callback));
    else if (callback)
        callback(QVariant());
}

void TikTokAuthDialog::clearSessionData()
{
    if (browser_)
        browser_->clearSessionData();
}
