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
API uses the same frozen Streamlabs UA constant as this project. Per the requested
Qt policy, WebEngine explicitly uses that constant too. This needs real login
verification on each packaged WebEngine version.

The browser has one persistent profile for the entire attempt, with cookies and
local storage enabled. Existing URL/load signals remain. A 1500 ms fallback polls
both the Qt URL and `window.location.href`, with attempt/lifetime guards. Timers
stop on success, failure, cancellation, dialog destruction and shutdown. The
in-page exchange guards in-flight fetches without disabling the committed-document
retry. Dialog pointers and the notification parent use QPointer.

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

The optional Node test executes the generated exchange script, checking the
expected-document fence, in-flight fetch guard, retained settled result and fresh
document retry. Existing response, auth-flow, PKCE, credential-store, auth-URL,
exchange and cleanup tests remain enabled.

## Results split: automated / manual / not run

Automated (run in this branch; see commands above): all 9 ctest suites pass,
including the extended `test-tiktok-lifecycle` checks listed above, on Linux
with system libobs 30 / Qt 6.12 (repo targets OBS 32.2.1; API used here is
stable across both).

Manual (requires a real OBS install and, where noted, a TikTok account):
scenarios 1-12 below plus shared-encoder, extra-audio-track, delayed-stop,
custom-scene and YouTube/Twitch/Kick/custom-RTMP checks on a real install.

Not run: TikTok-authenticated LIVE (no account in this environment), GPU
encoder runs (no NVENC/VAAPI hardware here), Windows/macOS builds and
packaged-WebEngine login (need their CI/platform environments).

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
| 13. Fresh OBS, primary Start Streaming never pressed, new TikTok target with independent H.264+AAC | Create endpoint + Start works; OBS streaming stays inactive; repeated stop/start reuses the endpoint; local Stop never calls `/stream/end` |
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
