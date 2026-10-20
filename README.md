# [Homepage / 主页](https://sorayuki.github.io/obs-multi-rtmp)

## 为什么首页是日语？ / Why is the homepage in Japanese?

因为最初是做给管人用的。

Because it's originally made for virtual Youtubers (VTubers).

# 声明 

近日发现百度贴吧有个叫 maggot 的用户在售卖此插件。咸鱼上也有，没得救了。 

本插件免费使用，作者不收取费用。 

举报之后百度贴吧找我要软件著作权证明，累不爱。 


# Announcement

This plugin is provided for free, without a fee. 

Recently a Baidu Tieba account 'maggot' is selling this plugin. Please, don't buy it.


# お知らせ

本プラグインは無償で提供されるものです。

最近、Baidu Tiebaに「maggot」というアカウント名のユーザーがこのプラグインを販売する行為をしています。

決して購入はしないでください。


# Donate

如果你觉得这个工具很有用想要捐赠，这里是链接。注意：这不是提需求的渠道。

このツールの開発に支援もとい投げ銭をしたいと思った方は以下のリンクからお願いします。(機能のリクエストは受け付けていません)

If you find this tool useful and want to doante, here is the link. (Please do not donate for feature requests.)

## [paypal / 贝宝](https://paypal.me/sorayuki0)

## alipay / 支付宝

![alipay](./docs/zhi.png) 

## wechat / 微信
![wechat](./docs/wechat.jpg)

## Build

This project uses obs-plugintemplate.   
Please refer to obs-plugintemplate to understand how it works.

## TikTok LIVE via Streamlabs

Any output target can optionally stream to TikTok LIVE. When enabled, the
plugin calls the Streamlabs API to create a TikTok LIVE session, receives a
fresh RTMP server/key pair, and streams to it with the normal OBS output.
The remote LIVE endpoint and the local RTMP output have separate lifecycles:
stopping the local output (or a failed start) preserves a ready endpoint for
retry, while "End TikTok LIVE", target deletion, profile switches away from
its profile, and OBS exit end the remote session.

TikTok stream access is controlled by TikTok/Streamlabs account eligibility.
This plugin does not grant LIVE permission to an account.

### Setup

1. Add or edit an output target.
2. Enable TikTok LIVE.
3. Click "Login with TikTok".
4. Sign into TikTok in the secure login window.
5. Authorize the Streamlabs integration.
6. When the plugin shows "Connected as @username", configure title/category.
7. Use "Create TikTok Endpoint" to prepare the LIVE without starting the
   output, then Start the output normally (Start creates the endpoint first
   when none exists yet).

### Encoders: independent vs shared

A brand-new TikTok target defaults to independent H.264 video + AAC audio
encoders (software x264/AAC baseline when available), so it can stream
without starting OBS's primary streaming or recording output. Existing
targets keep whatever encoder choice they already saved.

In the target settings, each track shows either its sharing partners or a
dependency warning:

- "Shares the OBS streaming encoder: requires OBS Start Streaming to be
  active." — starting this target while OBS is not streaming fails with an
  actionable message instead of starting OBS for you.
- "Shares the OBS recording encoder: ..." — same for recording.

To go independent, open Modify and pick a concrete encoder (e.g. x264 for
video, FFmpeg AAC for audio) for each track independently. If a saved
encoder choice is missing or invalid when you press Start, the target
reports exactly which track is broken instead of silently using another
encoder.

No Streamlabs token, RTMP server, stream key, or authorization code ever
needs to be copied or pasted: the plugin obtains and stores the OAuth
credential automatically. If you previously used a manual Streamlabs token,
it is migrated once into secure storage (when still valid) and removed
from `obs-multi-rtmp.json`.

The category must match a TikTok category name exactly (case-insensitive);
unknown or empty categories stream under "Other". The plugin never
fuzzy-matches an unrelated game. Category suggestions appear as you type.

### Linux support

- Native packages: build with the `ubuntu-x86_64` preset
  (`cmake --preset ubuntu-x86_64`). Requires libcurl development files
  (`libcurl4-openssl-dev` on Ubuntu) and Qt 6 WebEngine development files
  for the embedded login browser.
- Flatpak: the existing `com.obsproject.Studio.Plugin.MultiRTMP` extension
  manifest works unchanged (libcurl ships with the Freedesktop SDK, network
  access is inherited from OBS). See [FLATPAK.md](./FLATPAK.md).

CI presets and Flatpak use `ENABLE_TIKTOK_LOGIN=AUTO`: embedded login is
enabled only when a browser backend could be configured. Packages built
without it show the browser-unavailable message. Use `ENABLE_TIKTOK_LOGIN=ON`
to require login support (configuration fails if no backend is available), or
`OFF` to disable it. Ubuntu CI installs `qt6-webengine-dev` and `libsecret-1-dev`.

### Embedded login browser

The interactive TikTok login needs an embedded browser. The plugin ships
one backend per platform and picks it automatically:

| Platform | Backend | Runtime requirement |
| --- | --- | --- |
| Windows | Microsoft Edge WebView2 (native) | Evergreen WebView2 runtime |
| Linux | Qt 6 WebEngine | Qt WebEngine |
| macOS | Qt 6 WebEngine | Qt WebEngine |

Windows uses the native WebView2 control, so a stock OBS installation
without the Qt WebEngine runtime can still log in. The Evergreen WebView2
runtime is installed together with Microsoft Edge; when it is missing, the
login window explains how to install it
(https://developer.microsoft.com/microsoft-edge/webview2/).

Build configuration:

- `ENABLE_TIKTOK_LOGIN=ON|OFF|AUTO` decides whether a missing browser
  backend is a configuration error (`ON`), a warning (`AUTO`) or disables
  the login (`OFF`).
- `TIKTOK_BROWSER_BACKEND=auto|webview2|qtwebengine|none` selects the
  backend explicitly. `auto` prefers WebView2 on Windows and Qt WebEngine
  everywhere else. `webview2` is only valid on Windows, `none` disables
  the embedded login.

Official Windows builds use `ENABLE_TIKTOK_LOGIN=ON` and
`TIKTOK_BROWSER_BACKEND=webview2`: the release pipeline fails when the
WebView2 backend cannot be built, and the packaged plugin is verified to
contain the WebView2 loader. The SDK is downloaded from a pinned NuGet
package with a verified SHA-256 checksum; no browser is bundled.

### Security notes

- OAuth credentials are not stored in `obs-multi-rtmp.json`. The plugin
  uses the operating system credential store when available (Secret
  Service on Linux, Credential Manager on Windows, Keychain on macOS).
  If secure storage is unavailable, the login stays valid in memory for
  the current OBS session only, with a warning shown at login.
- The credential is only ever sent to `https://streamlabs.com` over TLS
  with certificate verification enabled. It is never logged and never
  placed in RTMP URLs.
- The login browser runs in a dedicated profile under the user
  configuration directory
  (`<config>/obs-multi-rtmp/tiktok-auth-webview`), never in the OBS
  installation folder and never in the user's regular browser profile.
  The browser has no host objects, no web messaging, no downloads, no
  permissions and no local file access; the privileged token exchange only
  runs on the expected HTTPS Streamlabs success document.
- Logging deliberately redacts authentication credentials and stream keys:
  auth URLs are logged as host + path only, without query strings. The
  observed navigation URL is stored without its query string, so the
  single-use authorization code is never kept in memory or logs.
- Generated RTMP server/key pairs remain in memory only.

### Troubleshooting

- "TikTok session expired. Please log in again.": the stored credential
  was rejected; use "Login with TikTok" to reconnect.
- "TikTok LIVE is not available for this account.": the linked
  TikTok/Streamlabs account is not eligible for LIVE. Use the
  "Apply for TikTok Live Permission" button next to the account status
  to open TikTok's official application page in the embedded browser
  and apply yourself; approval is TikTok's decision and may take time.
  After applying, the status changes to pending, and the plugin
  re-checks eligibility automatically (the button hides once access is
  granted).
- "Streamlabs rate limit reached." / "Streamlabs service temporarily
  unavailable.": wait and retry.
- "Could not contact Streamlabs." / "Network unavailable — account status
  unknown": check network access and proxy settings. Requests time out
  after 5 s (connect) / 15 s (total); saved credentials are kept and
  revalidated later.
- "The embedded TikTok login browser is not available in this build.": the
  package was built without a login backend. On Windows use a package
  built with the WebView2 backend, on Linux/macOS one built with the Qt
  WebEngine backend.
- "The Microsoft Edge WebView2 runtime is not installed. ...": install the
  Evergreen WebView2 runtime
  (https://developer.microsoft.com/microsoft-edge/webview2/) and retry. It
  normally comes with Microsoft Edge.
- "The embedded login browser could not be started.": the browser backend
  could not initialize. Restart OBS and retry; if it persists, check the
  OBS log for the reported error code.
- "The shared OBS encoder is unavailable. ...": the target shares OBS's
  streaming (or recording) encoder but that OBS output is not running.
  Either start the corresponding OBS output first, or open Modify and pick
  independent encoders (recommended: x264 video + AAC audio).
- "The saved video/audio encoder configuration is missing/invalid. ...":
  open Modify and choose the encoder for the named track again. The plugin
  never silently substitutes another encoder.
- If the local output fails to start or drops, the message now includes the
  underlying OBS/encoder error text when libobs reports one. A ready TikTok
  endpoint is preserved across local failures and local Stop: press Start
  again to retry on the same endpoint, or "End TikTok LIVE" to close it.
  Only explicit End, target deletion, profile switches and OBS exit end the
  remote session.
- If the output cannot start with a hardware encoder (NVENC/VAAPI/AMF),
  first verify the software baseline (x264 + AAC) starts: that separates a
  driver/hardware problem from a configuration problem. Hardware encoders
  are only usable where their drivers exist; their presence in the encoder
  list does not guarantee initialization succeeds. Optional manual checks:
  NVIDIA NVENC (e.g. RTX 3060, `nvidia-smi` must list the GPU) and AMD VAAPI
  (e.g. BC-250, `vainfo` must list an H.264 encoder entry).
