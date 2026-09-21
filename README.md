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
Stopping the target (or closing OBS / switching profiles) ends the TikTok
LIVE session again.

TikTok stream access is controlled by TikTok/Streamlabs account eligibility.
This plugin does not grant LIVE permission to an account.

### Setup

1. Add or edit an output target.
2. Enable TikTok LIVE.
3. Click "Login with TikTok".
4. Sign into TikTok in the secure login window.
5. Authorize the Streamlabs integration.
6. When the plugin shows "Connected as @username", configure title/category.
7. Start the output normally.

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

### Security notes

- OAuth credentials are not stored in `obs-multi-rtmp.json`. The plugin
  uses the operating system credential store when available (Secret
  Service on Linux, Credential Manager on Windows, Keychain on macOS).
  If secure storage is unavailable, the login stays valid in memory for
  the current OBS session only, with a warning shown at login.
- The credential is only ever sent to `https://streamlabs.com` over TLS
  with certificate verification enabled. It is never logged and never
  placed in RTMP URLs.
- The login WebView handles TikTok/Streamlabs cookies in a dedicated
  profile; generated RTMP server/key pairs remain in memory only.
- Logging deliberately redacts authentication credentials and stream keys:
  auth URLs are logged as host + path only, without query strings.

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
- "The embedded TikTok login browser (Qt WebEngine) is not available in
  this build.": the package lacks the Qt WebEngine runtime; use a build
  with WebEngine support.
- The TikTok LIVE session is always ended when the output stops, including
  forced stops, failed starts, OBS shutdown and profile switches.
