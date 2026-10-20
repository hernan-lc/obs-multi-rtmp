#include "tiktok/exchange.h"

#include <cctype>
#include <cstdio>

#include <json.hpp>

namespace {

constexpr size_t kMaxErrorChars = 120;
constexpr size_t kMaxPageChars = 80;

std::string Truncate(std::string text, size_t maxChars)
{
    if (text.size() > maxChars)
        text.resize(maxChars);
    return text;
}

std::string Sanitize(std::string text, size_t maxChars)
{
    for (char &c : text) {
        if (std::iscntrl(static_cast<unsigned char>(c)))
            c = ' ';
    }
    return Truncate(text, maxChars);
}

bool IsBlankPayload(const std::string &payload)
{
    for (char c : payload) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

} // namespace

std::string JsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string BuildExchangeStartJs(const std::string &exchangeUrl, const std::string &expectedPage)
{
    const std::string urlJson = JsonEscape(exchangeUrl);
    const std::string pageJson = JsonEscape(expectedPage);
    std::string js;
    js += "(function() {\n";
    js += "  var EP = " + pageJson + ";\n";
    js += "  var P = window.location.host + window.location.pathname;\n";
    js += "  if (P !== EP) return 'wrong-page';\n";
    js += "  if (window.location.protocol !== 'https:') return 'wrong-page';\n";
    js += "  var r = window.__multirtmp_tiktok_exchange;\n";
    js += "  if (r != null && (r.page === EP || r.success === true)) return 'kept';\n";
    js += "  if (window.__multirtmp_tiktok_exchange_inflight) return 'in-progress';\n";
    js += "  window.__multirtmp_tiktok_exchange_inflight = true;\n";
    js += "  window.__multirtmp_tiktok_exchange_seq = ((window.__multirtmp_tiktok_exchange_seq | 0) + 1);\n";
    js += "  var mySeq = window.__multirtmp_tiktok_exchange_seq;\n";
    js += "  window.__multirtmp_tiktok_exchange = null;\n";
    js += "  function errText(err) {\n";
    js += "    var name = (err && err.name) ? err.name + ': ' : '';\n";
    js += "    return name + String((err && err.message) || err);\n";
    js += "  }\n";
    js += "  function store(obj) {\n";
    js += "    window.__multirtmp_tiktok_exchange_inflight = false;\n";
    js += "    obj.page = P;\n";
    js += "    if (window.__multirtmp_tiktok_exchange_seq === mySeq) window.__multirtmp_tiktok_exchange = obj;\n";
    js += "  }\n";
    js += "  fetch(" + urlJson + ", {\n";
    js += "    method: 'GET',\n";
    js += "    credentials: 'include',\n";
    js += "    headers: { 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' }\n";
    js += "  }).then(function(res) {\n";
    js += "    res.text().then(function(text) {\n";
    js += "      try {\n";
    js += "        store({ success: true, data: JSON.parse(text), status: res.status });\n";
    js += "      } catch (e) {\n";
    js += "        store({ success: false, error: 'JSON Parse Error', body: text.slice(0, 2048), status: res.status });\n";
    js += "      }\n";
    js += "    }, function(err) {\n";
    js += "      store({ success: false, error: 'Body Read Error: ' + errText(err), status: res.status });\n";
    js += "    });\n";
    js += "  }, function(err) {\n";
    js += "    store({ success: false, error: errText(err) });\n";
    js += "  });\n";
    js += "  return 'started';\n";
    js += "})()";
    return js;
}

std::string BuildExchangePollJs(const std::string &expectedPage)
{
    const std::string pageJson = JsonEscape(expectedPage);
    std::string js;
    js += "(function() {\n";
    js += "  var EP = " + pageJson + ";\n";
    js += "  var P = window.location.host + window.location.pathname;\n";
    js += "  if (P !== EP) return null;\n";
    js += "  if (window.location.protocol !== 'https:') return null;\n";
    js += "  var r = window.__multirtmp_tiktok_exchange;\n";
    js += "  if (r == null) return null;\n";
    js += "  if (r.success === true) return r;\n";
    js += "  if (r.page === EP) return r;\n";
    js += "  return null;\n";
    js += "})()";
    return js;
}

ExchangeOutcome ParseExchangePayload(const std::string &payload)
{
    ExchangeOutcome outcome;
    if (IsBlankPayload(payload) || payload == "null")
        return outcome;

    nlohmann::json result;
    try {
        result = nlohmann::json::parse(payload);
    } catch (...) {
        outcome.settled = true;
        outcome.error = "login response unreadable";
        return outcome;
    }
    if (result.is_null())
        return outcome;
    outcome.settled = true;
    if (!result.is_object()) {
        outcome.error = "login response unreadable";
        return outcome;
    }

    const auto pageIt = result.find("page");
    if (pageIt != result.end() && pageIt->is_string())
        outcome.page = Sanitize(pageIt->get<std::string>(), kMaxPageChars);
    const auto statusIt = result.find("status");
    if (statusIt != result.end() && statusIt->is_number_integer())
        outcome.httpStatus = statusIt->get<int>();

    const auto successIt = result.find("success");
    const bool success = successIt != result.end() && successIt->is_boolean() && successIt->get<bool>();
    if (!success) {
        std::string detail = "token exchange failed";
        const auto errorIt = result.find("error");
        if (errorIt != result.end() && errorIt->is_string()) {
            const std::string message = Sanitize(errorIt->get<std::string>(), kMaxErrorChars);
            if (!message.empty())
                detail += ": " + message;
        }
        if (!outcome.page.empty())
            detail += " (page " + outcome.page + ")";
        outcome.error = detail;
        return outcome;
    }

    // success == true: require {success:true, data:{oauth_token}} envelope.
    const auto dataIt = result.find("data");
    if (dataIt == result.end() || !dataIt->is_object()) {
        outcome.error = "streamlabs rejected the login";
        return outcome;
    }
    const auto innerSuccess = dataIt->find("success");
    if (innerSuccess == dataIt->end() || !innerSuccess->is_boolean() || !innerSuccess->get<bool>()) {
        outcome.error = "streamlabs rejected the login";
        return outcome;
    }
    const auto tokenIt = dataIt->find("data");
    if (tokenIt == dataIt->end() || !tokenIt->is_object()) {
        outcome.error = "login response incomplete";
        return outcome;
    }
    const auto oauthIt = tokenIt->find("oauth_token");
    if (oauthIt == tokenIt->end() || !oauthIt->is_string() || oauthIt->get<std::string>().empty()) {
        outcome.error = "login response incomplete";
        return outcome;
    }
    outcome.success = true;
    outcome.token = oauthIt->get<std::string>();
    return outcome;
}
