#pragma once

// TikTok auth/account data models. Qt-free and dependency-free (STL only)
// so both the plugin and the unit tests can share them.
//
// Security note: none of these structs carry secrets. The OAuth bearer
// token, authorization codes and PKCE verifiers never appear in UI DTOs.

#include <string>

enum class TikTokAudience {
    Everyone = 0,
    AdultOnly = 1,
};

inline TikTokAudience TikTokAudienceFromBool(bool matureContent)
{
    return matureContent ? TikTokAudience::AdultOnly : TikTokAudience::Everyone;
}

inline bool TikTokAudienceIsMature(TikTokAudience audience)
{
    return audience == TikTokAudience::AdultOnly;
}

inline const char *TikTokAudienceApiValue(TikTokAudience audience)
{
    return audience == TikTokAudience::AdultOnly ? "1" : "0";
}

// Account state for the login state machine. Authentication (who the
// credential belongs to) and LIVE eligibility (whether TikTok/Streamlabs
// currently allows LIVE) are deliberately separate concepts.
enum class TikTokAuthState {
    Idle,
    TikTokLogin,
    StreamlabsAuth,
    Exchanging,
    Validating,
    Authenticated,
    Failed,
};

// Defensive view of GET /info identity fields. Every field may be missing
// on the wire; parsing must never fail just because an optional field is.
struct TikTokAccountInfo {
    std::string username;
    std::string displayName;

    bool liveEligibilityKnown = false;
    bool canGoLive = false;
    std::string liveUnavailableReason;

    // Raw application status from /info when the API provides it
    // (e.g. while an access application is under review). Displayed
    // verbatim; the plugin never guesses its vocabulary.
    std::string applicationStatus;

    std::string effectiveDisplayName() const { return displayName.empty() ? username : displayName; }
};

// Public UI-facing profile. Carries no credential.
struct TikTokUserProfile {
    std::string username;
    std::string displayName;
};
