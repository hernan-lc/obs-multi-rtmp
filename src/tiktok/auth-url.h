#pragma once

// Pure login-URL builders and auth-flow URL classifiers. Ported from the
// reference implementation with identical security properties:
//
// - OAuth URLs are built with QUrl/QUrlQuery, never string concatenation.
// - OAuth completion is restricted to an explicit host allowlist
//   (streamlabs.com, www.streamlabs.com) — never *.streamlabs.com.
// - Authorization codes are only accepted from parsed query items on
//   allowlisted hosts plus a success marker or dashboard path.
//
// No logging here: callers must only ever log LogSafeUrl() output.

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

namespace TikTokAuthUrl {

// https://streamlabs.com/m/login?force_verify=1&external=mobile&
// skip_splash=1&tiktok=1&code_challenge=<challenge>
QUrl BuildStreamlabsAuthUrl(const QByteArray &challenge);

// https://streamlabs.com/api/v5/slobs/auth/data?code=<code>&
// code_verifier=<verifier>
QUrl BuildExchangeUrl(const QString &code, const QByteArray &verifier);

// True for tiktok.com and *.tiktok.com.
bool IsTikTokUrl(const QUrl &url);

// True for streamlabs.com and *.streamlabs.com. Only used to decide
// whether a navigation to the fixed login URL is still needed; no
// secret ever flows on this path.
bool IsStreamlabsUrl(const QUrl &url);

// True only for the explicit OAuth allowlist.
bool IsOAuthHost(const QUrl &url);

// TikTok-login detection: a TikTok page that is not the login page,
// or any /foryou page.
bool IsTikTokLoggedInUrl(const QUrl &url);

// OAuth-success detection: non-empty `code` on an allowlisted host
// plus `success=true` or a known dashboard path. Returns the code.
std::optional<QString> ExtractAuthorizationCode(const QUrl &url);

// Secret-free document id: host + path (the code lives in the query
// string, so it never appears here). Used to fence the exchange to
// the expected Streamlabs success document.
QString SuccessPageId(const QUrl &url);

// True for TikTok's official LIVE-access application result page.
// Used only to notice, in our own embedded browser, that the user
// reached the result step (drives the pending state + auto-close).
// No page content is ever read.
bool IsLiveAccessResultUrl(const QUrl &url);

// Secret-free log form of any auth URL: host + path, no query.
QString LogSafeUrl(const QUrl &url);
QString LogSafeUrl(const QString &urlText);

} // namespace TikTokAuthUrl
