#pragma once

// Authenticated Streamlabs TikTok client. The OAuth token comes from the
// authentication/session layer (TikTokAuthManager), never from target
// configuration or UI widgets. The token is private: there is no accessor.
//
// Threading: each call uses its own short-lived cURL handle, so calls may
// run on worker threads. Results must be marshalled back to the UI thread
// by the caller. Never touches Qt widgets or OBS APIs.

#include <cstdint>
#include <memory>
#include <string>

#include "streamlabs-tiktok.h"

class StreamlabsTikTokClient {
public:
    explicit StreamlabsTikTokClient(std::string token, uint64_t authEpoch = 0);

    // Epoch of the auth session this client was issued for. Lets the auth
    // manager ignore 401 reports from clients that predate a newer login.
    uint64_t authEpoch() const { return authEpoch_; }

    TikTokAccountResult GetAccountInfo();
    TikTokCategoryListResult SearchCategories(const std::string &query);
    TikTokCategoryResult ResolveCategory(const std::string &category);
    TikTokStartResult StartStream(const std::string &title, const std::string &categoryId,
                                  TikTokAudience audience);
    bool EndStream(const std::string &streamId, std::string *error = nullptr,
                   TikTokFailure *failure = nullptr);

private:
    std::string token_;
    uint64_t authEpoch_ = 0;
};
