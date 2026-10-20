# TikTok remote endpoint and OBS restream

TikTok endpoints are remote resources. OBS outputs consume their credentials;
encoder/output preparation, connection failures, and ordinary Stop never end a LIVE.

## Actions and state

Login (in the TikTok account settings) validates the credential independently of
encoders. Create TikTok Endpoint logs in if needed, resolves the title/category/
audience through the API, and stores `streamId`, server and key in memory. Start
also offers this sequence and continues to OBS preparation once the endpoint is ready.

```
Login -> Create Endpoint -> EndpointReady -> Start Restream
      -> Stop Restream [endpoint remains ready]
      -> Start Restream again [same endpoint]
      -> End TikTok LIVE [stop locally, end remotely, reset credentials]
```

Remote state: Idle, Authenticating, CreatingEndpoint, EndpointReady,
EndingEndpoint, Error. Local state: Stopped, Preparing, Connecting, Streaming,
Stopping. Remote errors and local errors are handled separately.

- Local failures release local resources and preserve the endpoint for retry.
- Explicit End waits for local stop, prevents overlapping End requests, and keeps
  credentials on transient failure for a deliberate retry. HTTP 401 prevents local
  restart; HTTP 404 clears the missing endpoint.
- End during creation waits for the creation result and ends that endpoint.
- Target deletion, switching away from its profile, and OBS exit explicitly end
  owned sessions. Merely changing the profile list stops the local output.
- Unadopted successful creation results are cleaned up even if the owner disappears
  between worker completion and its queued callback.
- Shutdown drains cleanup workers and discards queued worker completions before unload.
- No automatic `/stream/start` retry is made. Network/timeout/server/malformed
  responses can be ambiguous: creation is blocked until the user checks TikTok,
  ends any LIVE there, and confirms this through End TikTok LIVE. The API surface
  used here has no lookup by creation request ID or idempotency key.

Generated credentials never enter target configuration, ordinary logs, or OBS
configuration serialization. Secure account token storage retains its existing policy.

## Authentication comparison

Reference: [keygenerator_tiktok](https://github.com/nglmercer/keygenerator_tiktok),
commit `1e6a37cbb1304ac3f6b5d75eab22059237a8f1c4`.

The OAuth URL parameters, 128-character random hexadecimal PKCE verifier,
unpadded SHA-256 base64url challenge, allowlisted success URL detection, in-page
credentialed token exchange, committed-document retry, and `/info` validation
match the reference behavior. Existing C++ authentication was retained.

The reference's current Rust/Wry browser uses its platform-default UA, while its
API uses the same frozen Streamlabs UA constant as this project. Both browser
backends (Qt WebEngine and Edge WebView2) use that constant too, so the
provider sees an identical client on every platform. This needs real login
verification on each packaged backend version.

The browser has one persistent profile for the entire attempt, with cookies and
local storage enabled. Existing URL/load signals remain. A 1500 ms fallback polls
both the Qt URL and `window.location.href`, with attempt/lifetime guards. Timers
stop on success, failure, cancellation, dialog destruction and shutdown. The
in-page exchange guards in-flight fetches without disabling the committed-document
retry. Dialog pointers and the notification parent use QPointer.

## Browser backends

The authentication state machine talks to one interface (`TikTokAuthBrowser`)
and never to a concrete browser. Backends implement asynchronous
initialization (`ready`/`failed`), navigation, URL observation, load events,
JavaScript execution and session cleanup; a backend that cannot provide these
capabilities is never selected.

| Platform | Backend | Notes |
| --- | --- | --- |
| Windows | Microsoft Edge WebView2 | Native control in a child window; Evergreen runtime required |
| Linux | Qt 6 WebEngine | Profile in the user config directory |
| macOS | Qt 6 WebEngine | Profile in the user config directory |

The first navigation is queued until the backend reports readiness, so a slow
WebView2 start can never lose the login URL. A backend failure (missing
runtime, initialization error, crashed browser process) aborts the attempt with
an actionable message, and a backend that does not become ready within 30 s
times out.

The WebView2 backend blocks local files, script URLs, downloads, popups outside
the login window, all permission requests, host objects and web messaging. The
privileged token exchange script only runs on the expected HTTPS Streamlabs
success document (checked in C++ and again inside the page). JavaScript results
are converted from the WebView2 JSON serialization into the same QVariant shapes
the WebEngine backend returns.

## Automated validation

```
cmake --build build_x86_64 -j 4
ctest --test-dir build_x86_64 --output-on-failure
```

`test-tiktok-lifecycle` runs the actual widget paths with a deterministic client
and output-allocation fault injection. It requires neither a graphics context nor
an account and verifies:

- Endpoint creation with no OBS output or encoders; duplicate creation guard.
- Runtime credentials absent from target configuration.
- Local allocation failure, retries, Stop and fatal-stop notification preserve the endpoint.
- Explicit End overlap guard, transient failure retry, and credential reset.
- Remote authorization failure blocks restart; missing endpoint resets state.
- Start timeout blocks blind duplicate creation.
- Generic RTMP bypasses TikTok creation and hides TikTok actions.
- Stale output events are ignored; destruction before/after worker completion is safe.
- End during creation; failed End after owner destruction balances activity tracking.
- Shutdown discards queued completion and cleans up an unadopted endpoint.
- Missing/invalid saved video/audio encoder configs fail with a specific
  error key and leave the saved selection untouched (no silent fallback to
  the shared OBS encoder).
- Disabled targets refuse Start without any remote call; a failed local
  start releases the starting guard so the next Start is not blocked.
- Config export roundtrip keeps TikTok settings but never credentials;
  malformed import input yields nullopt instead of wiping targets.
- Strict import shape validation: `{}`, foreign objects, non-array
  targets, id-less or wrongly-typed entries are rejected; current targets
  survive every rejection.
- Starting-guard regression for the exact disable-during-creation sequence;
  preflight blocks automatic endpoint creation on a missing scene while the
  manual Create path still works; preflight legs verified directly.
- `test-tiktok-standalone` (real OBS video 720p30 + audio 44.1 kHz, real
  plugins, needs a display for GL; without one it exits 77, reported by
  CTest as Skipped — never as Passed):
  independent x264+AAC start, flow frames, stop, same-endpoint restart and
  encoder reuse with no `/stream/start` call and no main streaming output;
  real `rtmp_output` against a closed localhost port maps stop code -2
  (ServerConnect) with a message and preserves the endpoint; owner teardown
  ends every adopted session exactly once with a balanced registry and no
  leaked services/encoders in the shutdown log.
- VAAPI datum (AMD BC-250, this machine): `ffmpeg_vaapi` enumerates and
  `obs_video_encoder_create("ffmpeg_vaapi")` succeeds. VAAPI *streaming*
  (not just creation) remains untested.

The optional Node test executes the generated exchange script, checking the
expected-document fence, the HTTPS-only fence, in-flight fetch guard, retained
settled result and fresh document retry. Existing response, auth-flow, PKCE,
credential-store, auth-URL, exchange and cleanup tests remain enabled.

New suites for the browser refactor:

- `test-tiktok-auth-browser-factory`: platform selection (WebView2 first on
  Windows, Qt WebEngine elsewhere), no silent downgrade to an unusable
  backend, capability contract.
- `test-tiktok-auth-browser`: mock backend lifecycle - queued navigation until
  readiness, initialization failure, ready/failed exclusivity, cleanup.
- `test-tiktok-auth-jsvalue`: WebView2 JSON result conversion (primitives,
  objects, arrays, null, malformed input).

The Windows backend cannot be compiled outside a Windows toolchain. It was
syntax checked against a transcription of the pinned SDK header (two real API
mistakes were found that way: `Cancel` is a property and navigation is driven
by the web view, not by the controller). A real MSVC build, an installed OBS
and an authorized TikTok account are still required before any claim about
the Windows login working.

## Results split: automated / manual / not run

Automated (run in this branch; see commands above): all 14 ctest suites pass,
including the extended `test-tiktok-lifecycle` checks listed above, on Linux
with system libobs 30 / Qt 6.12 (repo targets OBS 32.2.1; API used here is
stable across both).

Manual (requires a real OBS install and, where noted, a TikTok account):
scenarios 1-12 below plus shared-encoder, extra-audio-track, delayed-stop,
custom-scene and YouTube/Twitch/Kick/custom-RTMP checks on a real install.

Not run: TikTok-authenticated LIVE (no account in this environment), real
RTMP handshake bytes (no RTMP peer here; covered via refused-connection
mapping), VAAPI/NVENC streaming runs (creation proven for VAAPI, streaming
not attempted), the Windows/macOS builds and any packaged login (they need
their CI/platform environments and, for the login, a real account). Note:
repo CI previously triggered only on tags/`workflow_dispatch`; `push.yaml`
now also triggers on pull requests to master/main, so this PR should gain
build checks once reviewed on GitHub. Until those are green, results above
are from local runs quoted verbatim.

## Windows / WebView2 manual checklist

Requires a Windows machine with OBS, the Evergreen WebView2 runtime and an
authorized TikTok account.

| Scenario | Expected behavior |
| --- | --- |
| 1. Fresh login | Login window opens with the native browser; login succeeds |
| 2. Existing TikTok session | The saved session is reused, no re-authentication needed |
| 3. QR / two factor | The factor step completes inside the login window |
| 4. Cancel during startup | The window closes, no credential is stored |
| 5. Cancel during the exchange | No token is stored, the previous session survives |
| 6. Login timeout | An actionable timeout message, no stuck window |
| 7. Missing WebView2 runtime | The login reports the missing runtime with install guidance |
| 8. Browser process killed | The attempt fails with a clear message, OBS stays responsive |
| 9. Resize / move / DPI change | The browser follows the window, input keeps working |
| 10. LIVE-access application page | The page opens in the same native browser |
| 11. Logout | The profile directory is removed, the credential is deleted |
| 12. 20x login/logout | No growing handle or memory usage in the task manager |
| 13. OBS closed during login | No crash, no leftover browser process |
| 14. Installed release build | Works without any Qt WebEngine runtime present |

## Live manual validation still required

The development environment has not been used to authenticate a real TikTok
account or broadcast a LIVE. The following are pending, not claimed as passed:

| Scenario | Expected behavior |
| --- | --- |
| 1. Login with no encoders | Validated login, no output preparation |
| 2. Create with no encoders | One real `/stream/start`; endpoint ready |
| 3. Start with missing encoders | Local error; same endpoint remains ready |
| 4. Configure encoders and retry | Existing credentials reused; OBS connects |
| 5. Stop restream | Local output released; no remote End |
| 6. Start again | Same endpoint and no second `/stream/start` |
| 7. End LIVE while streaming | Local stop then one remote End; credentials cleared |
| 8. Unreachable RTMP / encoder initialization failure | Local error; endpoint retained |
| 9. Fresh/existing TikTok/Streamlabs cookies and SPA login | Polling/signals find redirect; validation succeeds |
| 10. Cancel/close login | Timers stop; stale token callbacks cannot accept a credential |
| 11. Close OBS during creation | Safe shutdown; any known created endpoint is ended |
| 12. Rapid Start clicks | One remote creation and one local start |
| 13. Fresh OBS, primary Start Streaming never pressed, new TikTok target with independent H.264+AAC | Create endpoint + Start works; OBS streaming stays inactive; repeated stop/start reuses the endpoint; local Stop never calls `/stream/end`. Encoder/output half automated by `test-tiktok-standalone`; real RTMP bytes + authenticated LIVE still manual |
| 14. Disabled target + Start All / hotkeys / sync start | Disabled target is skipped; disabling a live target stops local output only, endpoint stays consistent |

Also verify shared video/audio encoders, additional audio tracks, delayed stop,
custom scenes, and YouTube/Twitch/Kick/custom RTMP on a real OBS installation.
The refactor retains existing encoder preparation methods.

The Linux WebEngine-enabled build is checked locally. Windows/macOS compilation
and packaged browser login require their existing CI/platform environments. Other
locales use the English fallback for new strings. There is no proactive remote
endpoint expiry probe: remote validity changes only when the available API reports
it. Cleanup at shutdown is best effort and can fail if the network is unavailable
or credentials have expired; an unknown creation response cannot supply an ID to end.

## Files changed and additional bugs fixed

- `src/push-widget.cpp`
- `src/streamlabs-tiktok.cpp`, `src/streamlabs-tiktok.h`
- `src/tiktok/auth-browser.h`
- `src/tiktok/auth-dialog.cpp`, `src/tiktok/auth-dialog.h`
- `src/tiktok/auth-manager.cpp`, `src/tiktok/auth-manager.h`
- `src/tiktok/cleanup.h`
- `src/tiktok/exchange.cpp`
- `src/tiktok/streamlabs-client.cpp`, `src/tiktok/streamlabs-client.h`
- `src/tiktok/worker.cpp`
- `data/locale/en-US.ini`, `CMakeLists.txt`
- `tests/test-streamlabs-tiktok.cpp`, `tests/test-tiktok-exchange.cpp`
- `tests/test-tiktok-exchange-runtime.js`, `tests/test-tiktok-lifecycle.cpp`
- `tests/tiktok-manual-qa.md`

Beyond the lifecycle coupling, this fixes leaked output settings, missing output
allocation checks before attaching signal handlers, an unparented browser adapter,
raw dialog/notification-parent pointers, stale queued output updates, a gap between
worker completion and endpoint adoption, and queued worker/timer/browser functors
surviving shutdown. A delayed output finishing during the stop confirmation now
returns to Stopped instead of leaving the local state stuck in Stopping.
