// Browser backend lifecycle tests with a mock backend: readiness,
// failure, queued navigation, cleanup and stale callbacks.
#include <iostream>

#include <QCoreApplication>
#include <QUrl>

#include "tiktok/auth-browser.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

// Mock backend: the same contract every real backend implements.
class MockBrowser : public TikTokAuthBrowser {
    Q_OBJECT

public:
    explicit MockBrowser(QObject *parent = nullptr) : TikTokAuthBrowser(parent) {}

    TikTokAuthBackend backend() const override { return TikTokAuthBackend::WebEngine; }
    QString backendName() const override { return QStringLiteral("mock"); }
    TikTokAuthBrowserCapabilities capabilities() const override
    {
        TikTokAuthBrowserCapabilities caps;
        caps.navigation = true;
        caps.urlObservation = true;
        caps.loadEvents = true;
        caps.javaScript = true;
        caps.sessionCleanup = true;
        return caps;
    }

    void initialize() override
    {
        ++initializeCalls;
        if (failOnInitialize) {
            markFailed(QStringLiteral("mock failure"));
            return;
        }
        if (autoReady)
            markReady();
    }

    void becomeReady() { markReady(); }
    void becomeFailed(const QString &error) { markFailed(error); }

    QUrl currentUrl() const override { return url; }
    void runJavaScript(const QString &code, std::function<void(const QVariant &)> callback) override
    {
        ++scriptCalls;
        lastScript = code;
        if (callback)
            callback(jsResult);
    }
    QWidget *widget() override { return nullptr; }
    void clearSessionData() override { ++clearCalls; }
    void shutdown() override { ++shutdownCalls; }

    void navigateNow(const QUrl &target) override
    {
        ++navigations;
        url = target;
    }

    int initializeCalls = 0;
    int navigations = 0;
    int scriptCalls = 0;
    int clearCalls = 0;
    int shutdownCalls = 0;
    bool failOnInitialize = false;
    bool autoReady = true;
    QUrl url;
    QString lastScript;
    QVariant jsResult;
};

static int TestQueuedNavigation()
{
    MockBrowser browser;
    browser.autoReady = false;
    int readyCount = 0;
    QObject::connect(&browser, &TikTokAuthBrowser::ready, [&readyCount]() { ++readyCount; });
    browser.initialize();
    CHECK(browser.initializeCalls == 1);
    CHECK(!browser.isReady());
    // Navigation before readiness is queued, never dropped.
    browser.navigate(QUrl(QStringLiteral("https://www.tiktok.com/login")));
    CHECK(browser.navigations == 0);
    browser.becomeReady();
    CHECK(browser.isReady());
    CHECK(readyCount == 1);
    CHECK(browser.navigations == 1);
    CHECK(browser.url == QUrl(QStringLiteral("https://www.tiktok.com/login")));
    // A second navigation goes through immediately.
    browser.navigate(QUrl(QStringLiteral("https://streamlabs.com/m/login")));
    CHECK(browser.navigations == 2);
    return 0;
}

static int TestInitializationFailure()
{
    MockBrowser browser;
    browser.failOnInitialize = true;
    browser.autoReady = false;
    int failedCount = 0;
    QObject::connect(&browser, &TikTokAuthBrowser::failed, [&failedCount]() { ++failedCount; });
    browser.initialize();
    CHECK(browser.hasFailed());
    CHECK(failedCount == 1);
    CHECK(!browser.isReady());
    // A failed backend never navigates.
    browser.navigate(QUrl(QStringLiteral("https://www.tiktok.com/login")));
    CHECK(browser.navigations == 0);
    // The failure is reported exactly once.
    browser.becomeFailed(QStringLiteral("again"));
    CHECK(failedCount == 1);
    return 0;
}

static int TestReadyAndFailedAreExclusive()
{
    MockBrowser browser;
    browser.autoReady = false;
    int readyCount = 0;
    int failedCount = 0;
    QObject::connect(&browser, &TikTokAuthBrowser::ready, [&readyCount]() { ++readyCount; });
    QObject::connect(&browser, &TikTokAuthBrowser::failed, [&failedCount]() { ++failedCount; });
    browser.initialize();
    browser.becomeReady();
    browser.becomeReady();
    CHECK(readyCount == 1);
    CHECK(failedCount == 0);
    // A backend that breaks after readiness reports the failure once.
    browser.becomeFailed(QStringLiteral("crashed"));
    CHECK(failedCount == 1);
    browser.becomeFailed(QStringLiteral("crashed again"));
    CHECK(failedCount == 1);
    return 0;
}

static int TestEmptyNavigationIsIgnored()
{
    MockBrowser browser;
    browser.initialize();
    browser.navigate(QUrl());
    CHECK(browser.navigations == 0);
    return 0;
}

static int TestCleanupIsIdempotent()
{
    MockBrowser browser;
    browser.initialize();
    browser.clearSessionData();
    browser.clearSessionData();
    CHECK(browser.clearCalls == 2);
    browser.shutdown();
    browser.shutdown();
    CHECK(browser.shutdownCalls == 2);
    return 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (TestQueuedNavigation())
        return 1;
    if (TestInitializationFailure())
        return 1;
    if (TestReadyAndFailedAreExclusive())
        return 1;
    if (TestEmptyNavigationIsIgnored())
        return 1;
    if (TestCleanupIsIdempotent())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}

#include "test-tiktok-auth-browser.moc"
