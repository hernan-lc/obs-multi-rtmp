#include "tiktok/auth-dialog.h"

#include "tiktok/auth-browser.h"
#include "tiktok/auth-browser-factory.h"
#include "tiktok/auth-profile.h"
#include "tiktok/constants.h"

#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

#ifdef WITH_TIKTOK_WEBENGINE
#include <QWebEngineCookieStore>
#include <QWebEngineProfile>
#endif

QString TikTokAuthDialog::profileStoragePath()
{
    return TikTokAuthProfile::storagePath();
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
#endif
    // Remove the profile directory as well: cookies, cache and local
    // storage must not survive a logout. Best effort, the browser process
    // may still hold a few files for a short moment.
    TikTokAuthProfile::clearStorage();
}

TikTokAuthDialog::TikTokAuthDialog(QWidget *parent, const QString &title) : QDialog(parent)
{
    setWindowTitle(title.isEmpty() ? tr("Login with TikTok") : title);
    setModal(true);
    resize(1000, 750);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    stack_ = new QStackedWidget(this);
    notice_ = new QLabel(stack_);
    notice_->setWordWrap(true);
    notice_->setAlignment(Qt::AlignCenter);
    notice_->setMargin(16);
    stack_->addWidget(notice_);
    layout->addWidget(stack_);

    QString error;
    browser_ = TikTokAuthBrowserFactory::create(this, &error);
    if (!browser_) {
        notice_->setText(error.isEmpty()
                             ? tr("The embedded TikTok login browser is not available in this build.")
                             : error);
        setLayout(layout);
        return;
    }
    QObject::connect(browser_.get(), &TikTokAuthBrowser::urlObserved, this, &TikTokAuthDialog::urlObserved);
    QObject::connect(browser_.get(), &TikTokAuthBrowser::loadFinished, this,
                     &TikTokAuthDialog::pageLoadFinished);
    QObject::connect(browser_.get(), &TikTokAuthBrowser::ready, this, [this]() {
        notice_->setText(tr("Starting the embedded login browser..."));
        if (stack_->count() > 1)
            stack_->setCurrentIndex(1);
        emit browserReady();
    });
    QObject::connect(browser_.get(), &TikTokAuthBrowser::failed, this, [this](const QString &message) {
        notice_->setText(message.isEmpty()
                             ? tr("The embedded TikTok login browser could not be started.")
                             : message);
        stack_->setCurrentIndex(0);
        emit browserFailed(message);
    });

    notice_->setText(tr("Starting the embedded login browser..."));
    stack_->addWidget(browser_->widget());
    setLayout(layout);

    // The backend initializes asynchronously; the first navigation is
    // queued by the browser until it is ready.
    browser_->initialize();
}

TikTokAuthDialog::~TikTokAuthDialog()
{
    // Release the browser (and with it the native browser resources) while
    // the host widgets are still alive.
    browser_.reset();
}

bool TikTokAuthDialog::isBrowserReady() const
{
    return browser_ != nullptr && browser_->isReady();
}

QString TikTokAuthDialog::browserBackendName() const
{
    return browser_ ? browser_->backendName() : QString();
}

QUrl TikTokAuthDialog::currentUrl() const
{
    return browser_ ? browser_->currentUrl() : QUrl();
}

void TikTokAuthDialog::navigate(const QUrl &url)
{
    if (browser_)
        browser_->navigate(url);
}

void TikTokAuthDialog::runJavaScript(const QString &code,
                                     std::function<void(const QVariant &)> callback)
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
