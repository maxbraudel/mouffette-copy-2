# Protocol v6 implementation validation — 2026-09-17

Implementation and test configuration are described in
[remote-session-recovery.md](remote-session-recovery.md).

This is the historical v6 validation record. The current protocol is v7; see
[multiple-instance identity](multi-instance-identity.md) for the subsequent
identity migration and its validation. The graphical limitations recorded below
are not superseded by the network/identity checks.

## Verified

- macOS Debug build with Qt 6.11.2; `git diff --check`.
- All 13 server test scripts (`npm --prefix server test`).
- Deterministic protocol soak: 600 cycles, 480 transport losses, 382 resumes and
  replayed responses, 755 lost state ACKs, 359 lost cleanup ACKs, 149 recovered
  cleanup errors, 75 expirations and 291 civil-clock jumps.
- Server cache harness: 128 cycles, four restarts and eight recovered deletion
  failures; no active payload or pending deletion remains. This harness uses
  eight reused scopes; production Qt history bounds have separate tests.
- Linux suspend-clock bridge: 500 injected sleep/quantization sequences,
  refresh before processing authority messages, and conservative expiration.
- Complete real Qt/Node integration suite: 2/4/6-second interruptions, both
  reconnect orders, lost RESUME responses, lost state ACKs/readiness notices,
  exact OPEN replay, local expiry despite a healthy transport, and a delayed
  removal receipt after a generation change.
- Real recipient renderer and an opposite-direction transfer run concurrently
  on two sessions. After transport loss the same Live window remains; a roughly
  1.2 MiB PNG resumes from durable offsets and its final SHA-256 matches.
- ConnectionManager, ClientConnectionFlow (54 Qt cases including setup/teardown),
  RemoteSceneLifecycle, RemoteSceneControllerLifecycle, SceneRunCoordinator,
  UploadRemovalSecurity, UploadScheduler, RemoteCacheStore, RemoteCacheHistory,
  ProjectManager, AppConfig and DeviceIdentityStore pass.
- Server-return regression: after the real Node server is killed and the old
  recovery budget expires, 32 refused TCP connections do not disable retries.
  Restarting Node on the same address restores authentication, registration and
  reconciliation through the actual 24–36 second retry timer, without Enable.
  Disable during another outage prevents automatic reconnection; Enable restores
  it. Only the waits between the 32 refused attempts are accelerated.
- DNS, refusal, remote closure, timeout, network and TLS handshake errors remain
  retryable in the ConnectionManager regression suite. TLS certificate checking
  remains enabled. The TLS error is injected; certificate repair is not exercised
  against a real TLS listener by these tests.
- Cache regressions cover cancelled writers never acknowledging stale bytes,
  independent scopes remaining usable, asynchronous quarantine, autonomous
  deletion retry, bounded admission, reserved cleanup capacity and retention of
  young/provisional proofs with an injected monotonic clock.

## Broader graphical checks

The expanded macOS run reached 39 distinct passing CTest suites, using native
rendering or the offscreen platform as appropriate. This is not an all-green
42-suite native CTest run: the original native run was interrupted while Canvas
interaction tests waited for window activation. Both Canvas interaction suites
then passed offscreen; TextOutlineMotion passed with the native renderer.

Three general graphical suites still have failed assertions:

| Suite | Observed failure |
| --- | --- |
| CanvasSelectionBackend | Two video resize cases expect blended transition pixels; the offscreen renderer did not produce the asserted pixels. |
| MediaOverlay | Native window activation/title flags; offscreen title flags and a text-width assertion. |
| MediaOverlayScaled | The corresponding scaled window/focus and layout assertions. |

These are recorded as unresolved validation failures, not dismissed as proven
pre-existing bugs. The connection/session tests above are green. No production
rendering/layout changes were made to work around these assertions.

## Rollout limits

That release required server and clients together as v6 (superseded by v7). Old clients receive a clear
incompatibility failure. This work did not deploy a server, publish an installer
or commit changes. Windows execution and physical sleep/wake on hardware were
not performed locally; CI is configured to run server and Qt integration tests
on macOS and Windows. Sleep/deadline and disk-failure validation here uses
controlled clocks, transport faults and filesystem obstructions rather than
suspending the user's machine or filling their disk.
