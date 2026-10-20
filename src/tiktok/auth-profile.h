#pragma once

// On-disk location of the dedicated TikTok login browser profile.
//
// The login browser keeps its own profile (cookies, cache, local storage)
// outside of the OBS installation directory, under the user configuration
// directory, so the plugin never writes into a read-only install folder and
// never touches the user's regular browser profile.

#include <QString>

namespace TikTokAuthProfile {

// <config>/obs-studio/plugin_config/obs-multi-rtmp/tiktok-auth-webview
QString storagePath();

// Removes the profile directory from disk. Best effort: returns true when
// the directory is gone (or was never there).
bool clearStorage();

} // namespace TikTokAuthProfile
