#pragma once

// Centralized TikTok/Streamlabs constants. Single source of truth for the
// frozen Streamlabs User-Agent, endpoints and platform identifier so auth,
// API and WebView code can never drift apart. Qt-free and OBS-free so unit
// tests can include it directly.

#include <string>

namespace TikTokConstants {

// Production Streamlabs TikTok API base.
constexpr const char *kApiBase = "https://streamlabs.com/api/v5/slobs/tiktok";

// OAuth token-exchange endpoint (executed in the WebView page context).
constexpr const char *kAuthDataUrl = "https://streamlabs.com/api/v5/slobs/auth/data";

// First page of the embedded login flow.
constexpr const char *kTikTokLoginUrl = "https://www.tiktok.com/login";

// Official TikTok LIVE PC-access application entry, as published in
// Streamlabs' public guide. Opened for the user to drive themselves
// (the plugin never submits applications); approval is TikTok's decision.
constexpr const char *kLiveAccessApplyUrl =
    "https://www.tiktok.com/falcon/live_g/live_access_pc_apply/intro/index.html"
    "?id=GL6399433079641606942&lang=en-US";

// Streamlabs login entry; the PKCE challenge is attached as query params.
constexpr const char *kStreamlabsLoginUrl = "https://streamlabs.com/m/login";

// Working Streamlabs-compatible User-Agent. Verbatim parity with the
// reference implementation (the Electron token is intentional).
constexpr const char *kStreamlabsUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "StreamlabsDesktop/1.17.0 Chrome/122.0.6261.156 "
    "Electron/29.3.1 Safari/537.36";

// Secure credential-store identifiers. One entry for the whole plugin:
// TikTok login is an account, not a per-target setting.
constexpr const char *kCredentialService = "com.nglmercer.obs-multi-rtmp.tiktok";
constexpr const char *kCredentialAccount = "oauth_token";

// Dedicated WebEngine profile name for TikTok authentication.
constexpr const char *kWebViewProfileName = "obs-multi-rtmp-tiktok";
constexpr const char *kWebViewDirName = "tiktok-auth-webview";

constexpr long kConnectTimeoutMs = 5000;
constexpr long kTotalTimeoutMs = 15000;
constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr size_t kMaxCategoryQueryChars = 25;

// `device_platform` form value for POST /stream/start.
inline std::string DevicePlatform()
{
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

} // namespace TikTokConstants
