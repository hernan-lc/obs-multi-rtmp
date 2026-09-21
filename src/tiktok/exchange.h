#pragma once

// WebView-context token-exchange helpers. The exchange fetch must run
// inside the authenticated Streamlabs page (cookies included), so the
// plugin injects a starter script and polls for its settled result:
//
//   window.__multirtmp_tiktok_exchange = null   (until settled)
//
// QWebEnginePage::runJavaScript does not await Promises, hence the
// store-then-poll design. Every result is tagged with the executing
// document's secret-free page id (host + path) so stale-document
// failures can be fenced from definitive ones.
//
// Qt-free and OBS-free for unit testing. Secrets (code, verifier,
// token) pass through here but are never logged.

#include <cstdint>
#include <optional>
#include <string>

// Starter script for one authorization code. Runs synchronously and
// returns 'started' (or 'kept' when a previous accepted result must
// not be clobbered). Both dynamic values travel as JSON string
// literals, never raw interpolation.
std::string BuildExchangeStartJs(const std::string &exchangeUrl, const std::string &expectedPage);

// Poll script: returns the accepted outcome object, or null while the
// fetch is still in flight. Accepted = any success, or a failure from
// the expected success document. Failures from other documents stay
// hidden so the single starter re-run can retry in the committed page.
std::string BuildExchangePollJs(const std::string &expectedPage);

struct ExchangeOutcome {
    bool settled = false; // false while the fetch is still in flight
    bool success = false; // valid credential received
    std::string token;    // SECRET: candidate oauth_token, memory only
    std::string error;    // safe, truncated, single-line detail
    std::string page;     // secret-free executing-document id
    std::optional<int> httpStatus;
};

// Parses one poll payload: "null"/empty/whitespace means in flight,
// anything else is the settled outcome. Never throws.
ExchangeOutcome ParseExchangePayload(const std::string &payload);

// JSON string literal encoding for embedding dynamic values in scripts.
std::string JsonEscape(const std::string &text);
