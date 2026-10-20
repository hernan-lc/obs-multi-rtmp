#include "tiktok/auth-browser.h"

TikTokAuthBrowser::TikTokAuthBrowser(QObject *parent) : QObject(parent) {}

TikTokAuthBrowser::~TikTokAuthBrowser() = default;

QString TikTokAuthBackendName(TikTokAuthBackend backend)
{
    switch (backend) {
    case TikTokAuthBackend::WebEngine:
        return QStringLiteral("Qt WebEngine");
    case TikTokAuthBackend::WebView2:
        return QStringLiteral("Microsoft Edge WebView2");
    case TikTokAuthBackend::None:
    default:
        return QStringLiteral("none");
    }
}

void TikTokAuthBrowser::navigate(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
        return;
    if (!ready_) {
        // Queue the first navigation: backends initialize asynchronously
        // and navigating before readiness is an error on every backend.
        pendingUrl_ = url;
        hasPendingUrl_ = true;
        return;
    }
    navigateNow(url);
}

void TikTokAuthBrowser::markReady()
{
    if (ready_ || failed_)
        return;
    ready_ = true;
    flushPendingNavigation();
    emit ready();
}

void TikTokAuthBrowser::markFailed(const QString &error)
{
    if (failed_)
        return;
    // A backend that was ready and then broke (crashed browser process)
    // reports the failure as well: the attempt cannot continue.
    failed_ = true;
    hasPendingUrl_ = false;
    pendingUrl_.clear();
    emit failed(error);
}

void TikTokAuthBrowser::flushPendingNavigation()
{
    if (!hasPendingUrl_)
        return;
    hasPendingUrl_ = false;
    const QUrl url = pendingUrl_;
    pendingUrl_.clear();
    navigateNow(url);
}
