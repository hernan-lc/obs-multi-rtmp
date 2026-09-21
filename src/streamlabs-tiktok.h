#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "tiktok/auth-models.h"

// Machine-readable failure class for a Streamlabs request. Lets callers pick
// a localized message without parsing the human-readable detail text.
enum class TikTokFailure {
    None,
    Network,        // DNS / connect / TLS / other transport failure
    Timeout,        // connect or total request timeout
    BadRequest,     // HTTP 400 or other 4xx
    Auth,           // HTTP 401 (invalid or expired credential)
    Forbidden,      // HTTP 403 (account not authorized for TikTok LIVE)
    NotFound,       // HTTP 404
    RateLimited,    // HTTP 429
    Server,         // HTTP 5xx
    InvalidResponse, // HTTP 2xx but the payload is unusable
    Cancelled       // aborted via TikTokNetworkCancel (shutdown/cancel path)
};

// RAII cancellation for Streamlabs requests on the calling thread. The
// managed worker sets this around every job so shutdown can abort
// in-flight transfers instead of waiting out full timeouts.
class TikTokNetworkCancel {
public:
    explicit TikTokNetworkCancel(const std::atomic<bool> *flag);
    ~TikTokNetworkCancel();

    TikTokNetworkCancel(const TikTokNetworkCancel &) = delete;
    TikTokNetworkCancel &operator=(const TikTokNetworkCancel &) = delete;

private:
    const std::atomic<bool> *previous_;
};

// Result of POST /slobs/tiktok/stream/start. `server` and `key` are secrets:
// callers must never log them or persist them.
struct TikTokStartResult {
    bool success = false;
    TikTokFailure failure = TikTokFailure::None;
    std::string error;
    std::string streamId;
    std::string server;
    std::string key;
};

// Result of resolving a category name to a Streamlabs game_mask_id.
// An unknown category is NOT a failure: it resolves to an empty id
// ("Other") so LIVE creation can still proceed.
struct TikTokCategoryResult {
    bool success = false;
    TikTokFailure failure = TikTokFailure::None;
    std::string error;
    std::string categoryId;
    bool matchedExact = false;
};

// Result of GET /slobs/tiktok/info for account validation. Parsed
// defensively: identity fields may be missing without failing auth.
struct TikTokAccountResult {
    bool success = false;
    TikTokFailure failure = TikTokFailure::None;
    std::string error;
    TikTokAccountInfo account;
};

// One category entry for autocomplete: display `fullName`, submit
// `gameMaskId` as the stream category value.
struct TikTokCategoryEntry {
    std::string fullName;
    std::string gameMaskId;
};

struct TikTokCategoryListResult {
    bool success = false;
    TikTokFailure failure = TikTokFailure::None;
    std::string error;
    std::vector<TikTokCategoryEntry> categories;
};

// One credential decision for every Streamlabs response shape. 401 means
// the Bearer [REDACTED] is invalid; 403 means the credential may still be valid
// while TikTok LIVE access is unavailable; everything else is temporary.
enum class CredentialValidation {
    Valid,
    ValidLiveForbidden,
    InvalidCredential,
    TemporaryFailure
};

template<typename TikTokResult>
inline CredentialValidation ClassifyCredentialResult(const TikTokResult &result)
{
    if (result.success)
        return CredentialValidation::Valid;
    if (result.failure == TikTokFailure::Auth)
        return CredentialValidation::InvalidCredential;
    if (result.failure == TikTokFailure::Forbidden)
        return CredentialValidation::ValidLiveForbidden;
    return CredentialValidation::TemporaryFailure;
}

// Minimal synchronous client for the Streamlabs TikTok endpoints used by the
// StreamLabsTikTokStreamKeyGenerator project:
//
//   GET  /slobs/tiktok/info
//   GET  /slobs/tiktok/info?category=<category>
//   POST /slobs/tiktok/stream/start          (multipart form)
//   POST /slobs/tiktok/stream/<id>/end
//
// All requests use short timeouts and HTTPS certificate verification is left
// at the libcurl default (enabled). This class never logs credentials.
class StreamlabsTikTokAPI {
public:
    static TikTokStartResult StartStream(const std::string &token, const std::string &title,
                                         const std::string &categoryId, bool matureContent);
    static TikTokStartResult StartStream(const std::string &token, const std::string &title,
                                         const std::string &categoryId, TikTokAudience audience);

    static bool EndStream(const std::string &token, const std::string &streamId, std::string *error = nullptr);

    static TikTokCategoryResult ResolveCategory(const std::string &token, const std::string &category);

    static TikTokAccountResult GetAccountInfo(const std::string &token);
    static TikTokCategoryListResult ListCategories(const std::string &token, const std::string &query);

    // Pure response parsing (no network); exposed as a test seam.
    static TikTokStartResult ParseStartResponse(long httpCode, const std::string &body);
    static TikTokCategoryResult ParseCategoryResponse(long httpCode, const std::string &body,
                                                      const std::string &wantedCategory);
    static TikTokAccountResult ParseAccountResponse(long httpCode, const std::string &body);
    static TikTokCategoryListResult ParseCategoryListResponse(long httpCode, const std::string &body);
    static bool ParseEndResponse(long httpCode, const std::string &body, std::string *error);

    // Truncates to at most maxChars Unicode code points without splitting a
    // UTF-8 sequence. Used for the category query (the API rejects long
    // values with HTTP 500).
    static std::string TruncateToChars(const std::string &text, size_t maxChars);
};
