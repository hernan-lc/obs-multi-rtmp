#include "streamlabs-tiktok.h"

#include <cctype>
#include <memory>

#include <curl/curl.h>
#include <json.hpp>

// NOTE: this plugin runs inside OBS, which already initializes libcurl
// globally. Never call curl_global_init/curl_global_cleanup here.

namespace {

constexpr char kBaseUrl[] = "https://streamlabs.com/api/v5/slobs/tiktok";
// The Streamlabs endpoints expect a Streamlabs Desktop client.
constexpr char kUserAgent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                              "AppleWebKit/537.36 (KHTML, like Gecko) "
                              "StreamlabsDesktop/1.17.0 Chrome/122.0.6261.156 "
                              "Electron/29.3.1 Safari/537.36";
constexpr long kConnectTimeoutMs = 5000;
constexpr long kTotalTimeoutMs = 15000;
constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr size_t kMaxCategoryQueryChars = 25;
constexpr size_t kMaxDetailChars = 200;

struct CurlEasyDeleter {
    void operator()(CURL *curl) const
    {
        if (curl)
            curl_easy_cleanup(curl);
    }
};
struct CurlSlistDeleter {
    void operator()(curl_slist *list) const
    {
        if (list)
            curl_slist_free_all(list);
    }
};
struct CurlMimeDeleter {
    void operator()(curl_mime *mime) const
    {
        if (mime)
            curl_mime_free(mime);
    }
};
struct CurlMemDeleter {
    void operator()(char *ptr) const
    {
        if (ptr)
            curl_free(ptr);
    }
};

using CurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;
using CurlHeaders = std::unique_ptr<curl_slist, CurlSlistDeleter>;
using CurlMime = std::unique_ptr<curl_mime, CurlMimeDeleter>;
using CurlEscaped = std::unique_ptr<char, CurlMemDeleter>;

struct ResponseBuffer {
    std::string data;
    bool overflow = false;
};

size_t WriteResponse(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    const size_t total = size * nmemb;
    auto *buf = static_cast<ResponseBuffer *>(userdata);
    if (buf->data.size() + total > kMaxResponseBytes) {
        buf->overflow = true;
        return 0; // aborts the transfer with CURLE_WRITE_ERROR
    }
    buf->data.append(ptr, total);
    return total;
}

void ApplyCommonOptions(CURL *curl, ResponseBuffer *response)
{
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTotalTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // TLS certificate verification stays at the libcurl default (enabled).
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
}

CurlHeaders MakeAuthHeaders(const std::string &token)
{
    curl_slist *headers = nullptr;
    const std::string auth = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, auth.c_str());
    return CurlHeaders(headers);
}

TikTokFailure ClassifyHttpStatus(long httpCode)
{
    if (httpCode == 400)
        return TikTokFailure::BadRequest;
    if (httpCode == 401)
        return TikTokFailure::Auth;
    if (httpCode == 403)
        return TikTokFailure::Forbidden;
    if (httpCode == 404)
        return TikTokFailure::NotFound;
    if (httpCode == 429)
        return TikTokFailure::RateLimited;
    if (httpCode >= 500 && httpCode <= 599)
        return TikTokFailure::Server;
    if (httpCode >= 400 && httpCode <= 499)
        return TikTokFailure::BadRequest;
    return TikTokFailure::InvalidResponse;
}

std::string DescribeHttpError(long httpCode)
{
    switch (httpCode) {
    case 400:
        return "Streamlabs rejected the request (HTTP 400).";
    case 401:
        return "Streamlabs authentication failed (HTTP 401).";
    case 403:
        return "TikTok LIVE access is not available for this account (HTTP 403).";
    case 404:
        return "Streamlabs endpoint not found (HTTP 404).";
    case 429:
        return "Streamlabs rate limit reached (HTTP 429).";
    default:
        break;
    }
    if (httpCode >= 500 && httpCode <= 599)
        return "Streamlabs service temporarily unavailable (HTTP " + std::to_string(httpCode) + ").";
    return "Streamlabs request failed (HTTP " + std::to_string(httpCode) + ").";
}

// Keeps a server-provided message safe for logs and UI: single line,
// bounded length. Only documented message fields are ever extracted.
std::string SanitizeDetail(std::string detail)
{
    for (char &c : detail) {
        if (std::iscntrl(static_cast<unsigned char>(c)))
            c = ' ';
    }
    if (detail.size() > kMaxDetailChars)
        detail.resize(kMaxDetailChars);
    return detail;
}

std::string ExtractServerMessage(const nlohmann::json &root)
{
    if (!root.is_object())
        return {};
    auto it = root.find("message");
    if (it != root.end() && it->is_string())
        return SanitizeDetail(it->get<std::string>());
    it = root.find("data");
    if (it != root.end() && it->is_object()) {
        auto msg = it->find("message");
        if (msg != it->end() && msg->is_string())
            return SanitizeDetail(msg->get<std::string>());
    }
    return {};
}

std::string WithServerMessage(const std::string &base, const std::string &body)
{
    try {
        const auto json = nlohmann::json::parse(body);
        const std::string message = ExtractServerMessage(json);
        if (!message.empty())
            return base + " " + message;
    } catch (...) {
    }
    return base;
}

std::string JsonValueToId(const nlohmann::json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    return {};
}

bool EqualsAsciiCaseInsensitive(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}

bool IsBlank(const std::string &text)
{
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

} // namespace

std::string StreamlabsTikTokAPI::TruncateToChars(const std::string &text, size_t maxChars)
{
    size_t bytes = 0;
    size_t chars = 0;
    while (bytes < text.size() && chars < maxChars) {
        const unsigned char c = static_cast<unsigned char>(text[bytes]);
        size_t width = 1;
        if ((c & 0x80) == 0x00) {
            width = 1;
        } else if ((c & 0xE0) == 0xC0) {
            width = 2;
        } else if ((c & 0xF0) == 0xE0) {
            width = 3;
        } else if ((c & 0xF8) == 0xF0) {
            width = 4;
        } else {
            // Invalid leading byte: stop rather than emit broken UTF-8.
            break;
        }
        if (bytes + width > text.size())
            break;
        bytes += width;
        ++chars;
    }
    return text.substr(0, bytes);
}

TikTokStartResult StreamlabsTikTokAPI::ParseStartResponse(long httpCode, const std::string &body)
{
    TikTokStartResult result;
    if (httpCode < 200 || httpCode > 299) {
        result.failure = ClassifyHttpStatus(httpCode);
        result.error = WithServerMessage(DescribeHttpError(httpCode), body);
        return result;
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (const std::exception &) {
        result.failure = TikTokFailure::InvalidResponse;
        result.error = "Invalid JSON in Streamlabs response.";
        return result;
    }
    if (!json.is_object()) {
        result.failure = TikTokFailure::InvalidResponse;
        result.error = "Invalid JSON in Streamlabs response.";
        return result;
    }

    // The stream id comes from the explicit response field; it must never be
    // reconstructed by parsing the RTMP key.
    std::string missing;
    const std::string streamId = JsonValueToId(json.value("id", nlohmann::json()));
    const auto rtmpIt = json.find("rtmp");
    const auto keyIt = json.find("key");
    const std::string server = (rtmpIt != json.end() && rtmpIt->is_string()) ? rtmpIt->get<std::string>() : "";
    const std::string key = (keyIt != json.end() && keyIt->is_string()) ? keyIt->get<std::string>() : "";
    if (streamId.empty())
        missing += " id";
    if (server.empty())
        missing += " rtmp";
    if (key.empty())
        missing += " key";
    if (!missing.empty()) {
        result.failure = TikTokFailure::InvalidResponse;
        result.error = "Streamlabs response is missing required field(s):" + missing + ".";
        return result;
    }

    result.success = true;
    result.failure = TikTokFailure::None;
    result.streamId = streamId;
    result.server = server;
    result.key = key;
    return result;
}

TikTokCategoryResult StreamlabsTikTokAPI::ParseCategoryResponse(long httpCode, const std::string &body,
                                                                const std::string &wantedCategory)
{
    TikTokCategoryResult result;
    if (httpCode < 200 || httpCode > 299) {
        result.failure = ClassifyHttpStatus(httpCode);
        result.error = WithServerMessage(DescribeHttpError(httpCode), body);
        return result;
    }

    // An empty category intentionally resolves to "Other" (empty id) without
    // consulting the response.
    if (IsBlank(wantedCategory)) {
        result.success = true;
        return result;
    }

    // Category data is advisory: a 2xx response with an unexpected shape must
    // not block LIVE creation, so it degrades to "Other" instead of failing.
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (const std::exception &) {
        result.success = true;
        return result;
    }
    const auto categoriesIt = (json.is_object()) ? json.find("categories") : json.end();
    if (categoriesIt == json.end() || !categoriesIt->is_array()) {
        result.success = true;
        return result;
    }

    // 1. Case-insensitive exact match. Never fuzzy-match: picking the
    // "closest" game could stream under an unrelated category.
    for (const auto &entry : *categoriesIt) {
        if (!entry.is_object())
            continue;
        const auto nameIt = entry.find("full_name");
        if (nameIt == entry.end() || !nameIt->is_string())
            continue;
        if (EqualsAsciiCaseInsensitive(nameIt->get<std::string>(), wantedCategory)) {
            result.success = true;
            result.matchedExact = true;
            const auto idIt = entry.find("game_mask_id");
            result.categoryId = (idIt != entry.end()) ? JsonValueToId(*idIt) : "";
            return result;
        }
    }

    // 2. Explicit "Other" fallback when the API provides one.
    for (const auto &entry : *categoriesIt) {
        if (!entry.is_object())
            continue;
        const auto nameIt = entry.find("full_name");
        if (nameIt == entry.end() || !nameIt->is_string())
            continue;
        if (EqualsAsciiCaseInsensitive(nameIt->get<std::string>(), "Other")) {
            result.success = true;
            const auto idIt = entry.find("game_mask_id");
            result.categoryId = (idIt != entry.end()) ? JsonValueToId(*idIt) : "";
            return result;
        }
    }

    // 3. Unknown category: empty id ("Other"), still a success.
    result.success = true;
    return result;
}

bool StreamlabsTikTokAPI::ParseEndResponse(long httpCode, const std::string &body, std::string *error)
{
    if (httpCode < 200 || httpCode > 299) {
        if (error)
            *error = WithServerMessage(DescribeHttpError(httpCode), body);
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(body);
        if (json.is_object()) {
            const auto it = json.find("success");
            if (it != json.end() && it->is_boolean() && !it->get<bool>()) {
                if (error) {
                    const std::string message = ExtractServerMessage(json);
                    *error = message.empty() ? "Streamlabs refused to end the TikTok LIVE session."
                                             : "Streamlabs refused to end the TikTok LIVE session. " + message;
                }
                return false;
            }
        }
    } catch (...) {
        // A 2xx with a non-JSON body still counts as success.
    }
    return true;
}

TikTokCategoryResult StreamlabsTikTokAPI::ResolveCategory(const std::string &token, const std::string &category)
{
    TikTokCategoryResult result;
    if (token.empty()) {
        result.failure = TikTokFailure::Auth;
        result.error = "Streamlabs credential is missing.";
        return result;
    }
    if (IsBlank(category)) {
        result.success = true;
        return result;
    }

    CurlEasy curl(curl_easy_init());
    if (!curl) {
        result.failure = TikTokFailure::Network;
        result.error = "Failed to initialize the HTTP client.";
        return result;
    }

    CurlEscaped escaped(
        curl_easy_escape(curl.get(), TruncateToChars(category, kMaxCategoryQueryChars).c_str(), 0));
    if (!escaped) {
        result.failure = TikTokFailure::Network;
        result.error = "Failed to encode the category query.";
        return result;
    }
    const std::string url = std::string(kBaseUrl) + "/info?category=" + escaped.get();

    CurlHeaders headers = MakeAuthHeaders(token);
    ResponseBuffer response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
    ApplyCommonOptions(curl.get(), &response);

    const CURLcode code = curl_easy_perform(curl.get());
    if (code == CURLE_WRITE_ERROR && response.overflow) {
        result.failure = TikTokFailure::InvalidResponse;
        result.error = "Streamlabs response exceeded the size limit.";
        return result;
    }
    if (code == CURLE_OPERATION_TIMEDOUT) {
        result.failure = TikTokFailure::Timeout;
        result.error = "Request to Streamlabs timed out.";
        return result;
    }
    if (code != CURLE_OK) {
        result.failure = TikTokFailure::Network;
        result.error = std::string("Could not contact Streamlabs: ") + curl_easy_strerror(code) + ".";
        return result;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpCode);
    return ParseCategoryResponse(httpCode, response.data, category);
}

TikTokStartResult StreamlabsTikTokAPI::StartStream(const std::string &token, const std::string &title,
                                                  const std::string &categoryId, bool matureContent)
{
    TikTokStartResult result;
    if (token.empty()) {
        result.failure = TikTokFailure::Auth;
        result.error = "Streamlabs credential is missing.";
        return result;
    }

    CurlEasy curl(curl_easy_init());
    if (!curl) {
        result.failure = TikTokFailure::Network;
        result.error = "Failed to initialize the HTTP client.";
        return result;
    }

    const std::string url = std::string(kBaseUrl) + "/stream/start";
    CurlHeaders headers = MakeAuthHeaders(token);

    CurlMime mime(curl_mime_init(curl.get()));
    const std::string audienceType = matureContent ? "1" : "0";
    const char *fields[][2] = {
        {"title", title.c_str()},
        {"category", categoryId.c_str()},
        {"audience_type", audienceType.c_str()},
        {"device_platform", "win32"},
    };
    for (const auto &field : fields) {
        curl_mimepart *part = curl_mime_addpart(mime.get());
        curl_mime_name(part, field[0]);
        curl_mime_data(part, field[1], CURL_ZERO_TERMINATED);
    }

    ResponseBuffer response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
    ApplyCommonOptions(curl.get(), &response);

    const CURLcode code = curl_easy_perform(curl.get());
    if (code == CURLE_WRITE_ERROR && response.overflow) {
        result.failure = TikTokFailure::InvalidResponse;
        result.error = "Streamlabs response exceeded the size limit.";
        return result;
    }
    if (code == CURLE_OPERATION_TIMEDOUT) {
        result.failure = TikTokFailure::Timeout;
        result.error = "Request to Streamlabs timed out.";
        return result;
    }
    if (code != CURLE_OK) {
        result.failure = TikTokFailure::Network;
        result.error = std::string("Could not contact Streamlabs: ") + curl_easy_strerror(code) + ".";
        return result;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpCode);
    return ParseStartResponse(httpCode, response.data);
}

bool StreamlabsTikTokAPI::EndStream(const std::string &token, const std::string &streamId, std::string *error)
{
    if (token.empty()) {
        if (error)
            *error = "Streamlabs credential is missing.";
        return false;
    }
    if (streamId.empty()) {
        if (error)
            *error = "TikTok stream id is missing.";
        return false;
    }

    CurlEasy curl(curl_easy_init());
    if (!curl) {
        if (error)
            *error = "Failed to initialize the HTTP client.";
        return false;
    }

    CurlEscaped escaped(curl_easy_escape(curl.get(), streamId.c_str(), 0));
    if (!escaped) {
        if (error)
            *error = "Failed to encode the TikTok stream id.";
        return false;
    }
    const std::string url = std::string(kBaseUrl) + "/stream/" + escaped.get() + "/end";

    CurlHeaders headers = MakeAuthHeaders(token);
    ResponseBuffer response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    ApplyCommonOptions(curl.get(), &response);

    const CURLcode code = curl_easy_perform(curl.get());
    if (code == CURLE_WRITE_ERROR && response.overflow) {
        if (error)
            *error = "Streamlabs response exceeded the size limit.";
        return false;
    }
    if (code == CURLE_OPERATION_TIMEDOUT) {
        if (error)
            *error = "Request to Streamlabs timed out.";
        return false;
    }
    if (code != CURLE_OK) {
        if (error)
            *error = std::string("Could not contact Streamlabs: ") + curl_easy_strerror(code) + ".";
        return false;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpCode);
    return ParseEndResponse(httpCode, response.data, error);
}
