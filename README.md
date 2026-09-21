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

## TikTok LIVE via Streamlabs (Linux)

Any output target can optionally stream to TikTok LIVE. When enabled, the
plugin calls the Streamlabs API to create a TikTok LIVE session, receives a
fresh RTMP server/key pair, and streams to it with the normal OBS output.
Stopping the target (or closing OBS / switching profiles) ends the TikTok
LIVE session again.

TikTok stream access is controlled by TikTok/Streamlabs account eligibility.
This plugin does not grant LIVE permission to an account.

### Setup

1. Get a Streamlabs token with the
   [StreamLabsTikTokStreamKeyGenerator](https://github.com/Loukious/StreamLabsTikTokStreamKeyGenerator)
   project (the "Get Streamlabs Token" button in the target settings opens it).
2. In the target settings, open the "TikTok / Streamlabs" section.
3. Check "Enable TikTok via Streamlabs" and fill in the Streamlabs token,
   stream title, category and mature-content flag.
4. Keep the target protocol on RTMP and press Start.

The category must match a TikTok category name exactly (case-insensitive);
unknown or empty categories stream under "Other". The plugin never
fuzzy-matches an unrelated game.

### Linux support

- Native packages: build with the `ubuntu-x86_64` preset
  (`cmake --preset ubuntu-x86_64`). Requires libcurl development files
  (`libcurl4-openssl-dev` on Ubuntu).
- Flatpak: the existing `com.obsproject.Studio.Plugin.MultiRTMP` extension
  manifest works unchanged (libcurl ships with the Freedesktop SDK, network
  access is inherited from OBS). See [FLATPAK.md](./FLATPAK.md).

### Security notes

- The Streamlabs credential is stored in plain text in the OBS profile file
  `obs-multi-rtmp.json`. Anyone with read access to your OBS profile can read
  it. OS keychain storage is a planned follow-up.
- The credential is only ever sent to `https://streamlabs.com` over TLS with
  certificate verification enabled. It is never logged and never placed in
  RTMP URLs.
- Generated RTMP server/key pairs and the TikTok session id are kept in
  memory only and are never written to disk.

### Troubleshooting

- "TikTok Streamlabs token is missing.": enablement requires the dedicated
  Streamlabs token field, not the RTMP password field.
- "Streamlabs authentication failed.": the token is invalid or expired;
  generate a new one.
- "TikTok LIVE access is not available for this account.": the linked
  TikTok/Streamlabs account is not eligible for LIVE.
- "Streamlabs rate limit reached." / "Streamlabs service temporarily
  unavailable.": wait and retry.
- "Could not contact Streamlabs.": check network access and proxy settings.
  Requests time out after 5 s (connect) / 15 s (total).
- The TikTok LIVE session is always ended when the output stops, including
  forced stops, OBS shutdown and profile switches.
