#include "tiktok/auth-url.h"
#include "tiktok/constants.h"

#include <QUrlQuery>

namespace TikTokAuthUrl {

QUrl BuildStreamlabsAuthUrl(const QByteArray &challenge)
{
    QUrl url(QString::fromLatin1(TikTokConstants::kStreamlabsLoginUrl));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("force_verify"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("external"), QStringLiteral("mobile"));
    query.addQueryItem(QStringLiteral("skip_splash"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("tiktok"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    url.setQuery(query);
    return url;
}

QUrl BuildExchangeUrl(const QString &code, const QByteArray &verifier)
{
    QUrl url(QString::fromLatin1(TikTokConstants::kAuthDataUrl));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("code"), code);
    query.addQueryItem(QStringLiteral("code_verifier"), QString::fromLatin1(verifier));
    url.setQuery(query);
    return url;
}

bool IsTikTokUrl(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QStringLiteral("tiktok.com") || host.endsWith(QStringLiteral(".tiktok.com"));
}

bool IsStreamlabsUrl(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QStringLiteral("streamlabs.com") ||
           host.endsWith(QStringLiteral(".streamlabs.com"));
}

bool IsOAuthHost(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QStringLiteral("streamlabs.com") || host == QStringLiteral("www.streamlabs.com");
}

bool IsTikTokLoggedInUrl(const QUrl &url)
{
    const QString text = url.toString();
    if (text.contains(QStringLiteral("/foryou")))
        return true;
    return IsTikTokUrl(url) && !text.contains(QStringLiteral("login")) &&
           !text.contains(QStringLiteral("streamlabs"));
}

std::optional<QString> ExtractAuthorizationCode(const QUrl &url)
{
    if (!IsOAuthHost(url))
        return std::nullopt;
    const QUrlQuery query(url);
    const QString code = query.queryItemValue(QStringLiteral("code"));
    if (code.isEmpty())
        return std::nullopt;
    const bool hasSuccess = query.queryItemValue(QStringLiteral("success")) == QStringLiteral("true");
    const QString path = url.path();
    const bool isDashboard = path == QStringLiteral("/dashboard") ||
                             path.startsWith(QStringLiteral("/dashboard/")) ||
                             path == QStringLiteral("/slobs/dashboard") ||
                             path.startsWith(QStringLiteral("/slobs/dashboard/"));
    if (hasSuccess || isDashboard)
        return code;
    return std::nullopt;
}

QString SuccessPageId(const QUrl &url)
{
    const QString host = url.host().isEmpty() ? QStringLiteral("?") : url.host();
    return host + url.path();
}

bool IsExpectedExchangePage(const QUrl &url, const QString &expectedPage)
{
    if (expectedPage.isEmpty())
        return false;
    // The exchange runs privileged JavaScript: only an HTTPS document on
    // the OAuth allowlist may ever carry it.
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return false;
    if (!IsOAuthHost(url))
        return false;
    return SuccessPageId(url) == expectedPage;
}

bool IsLiveAccessResultUrl(const QUrl &url)
{
    if (!IsTikTokUrl(url))
        return false;
    return url.path().contains(QStringLiteral("live_access_pc_apply/result"));
}

QString LogSafeUrl(const QUrl &url)
{
    return SuccessPageId(url);
}

QString LogSafeUrl(const QString &urlText)
{
    return LogSafeUrl(QUrl(urlText));
}

} // namespace TikTokAuthUrl
