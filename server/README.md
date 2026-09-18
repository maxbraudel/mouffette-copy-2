# Mouffette Server

Node.js WebSocket coordinator for Mouffette protocol v10.

## Run and test

```bash
npm install
npm test
npm start
```

The checked-in `.env` listens on `0.0.0.0:8080`. Process environment values
override it. Invalid critical values fail startup.

## Protocol envelope

The server sends `auth_challenge` first. The client signs
`mouffette-v10\n<serverBootId>\n<nonce>\n<runtimeId>\n<instanceId>\n<instanceOrdinal>` with its
Ed25519 installation key and returns the SPKI public key and signature as
base64url. The SHA-256 of the SPKI key is the stable `installationId`; the
server domain-separates and hashes `installationId + instanceId` to derive and
verify the targetable `endpointId`.

`instanceOrdinal` is a signed immutable integer from 1 through 2,147,483,647.
Its only valid `instanceId` is `primary` for 1 and `instance-N` for N >= 2,
using canonical decimal digits. Both values are included in `welcome` before
metadata registration; `endpoint_snapshot` cannot change the ordinal. The
endpoint remains `base64url(SHA256("mouffette-endpoint-v1\n" + installationId +
"\n" + instanceId))`, preserving the existing primary endpoint. Reusing a free
ordinal reuses its endpoint; a new process has a new `runtimeId`. Every ordinal
is independently discoverable and may own or receive sessions, including from
another instance of the same installation.

Transport `connectionGeneration` values come from one strictly increasing safe
integer counter per `serverBootId`. Gaps for one endpoint are expected. The
server retains only an index of current transports, never a growing history of
endpoint counters. Exhaustion explicitly rejects authentication before replacing
an existing transport; only a new boot resets the counter. A healthy runtime
cannot be displaced by another process using the same endpoint. An expired
runtime's sessions are terminated before replacement; a same-runtime rebound
preserves only the original remaining recovery budget.

Every subsequent message uses:

```json
{
  "type": "message_type",
  "protocolVersion": 10,
  "serverBootId": "uuid-from-welcome",
  "messageId": "unique-uuid",
  "connectionGeneration": 1
}
```

The server rejects messages from an earlier boot or protocol version. It derives
the sender from the authenticated socket; sender IDs supplied by a client are
never authoritative.

## Remote sessions

An owner opens `remote_session_open` with `targetEndpointId` and a stable
`requestId`. Multiple incoming sessions and outgoing sessions may coexist on the
same endpoint. An existing scene, another controller, or local UI activity does
not make the endpoint unavailable for another session.

The v10 welcome uses timing policy version 4:

- `heartbeatIntervalMs`: 750 ms. Two missed intervals detect silent loss;
  `transportSuspectAfterMs` and `leaseTimeoutMs` both equal 1,500 ms (derived).
  The suspect socket is fenced immediately and replacement attempts may start.
- `sessionRecoveryTimeoutMs`: **3,000 ms after interruption detection**
  (`MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS=3000`). Socket closure detects
  immediately; silence is detected at the second missed heartbeat, even if the
  event loop wakes later. There is one fixed deadline for the whole session.
  Reauthentication, one returning participant and retries never extend it.
  Both participants must resume and apply the new state before it expires.
- While retained, the session is **Degraded** (internal phase Grace). Expiration
  makes it terminal/Disconnected; Disable or explicit CLOSE bypass recovery.
  Already Live scenes continue within this period; no scene is auto-restarted.
- New session opening allows 5,000 ms; authentication allows 10,000 ms. These
  operation timeouts never extend a retained session or stop background retries.

The old independent lease/degradation environment keys are removed. Deploy both
clients and server together; old clients reject the new timing policy clearly.

Each nonterminal session state carries `generation`, `stateRevision`,
`serverMonotonicMs`, and `validUntilServerMonotonicMs`. Heartbeat acknowledgements
carry `sessionStates` so both parties know their absolute deadline before a
network failure. The connected party cannot extend the disconnected party's
lease. A scene already Live may continue within that lease; pre-start scenes are
cancelled on transport loss. New commands wait for successful recovery. STOP,
CLOSE and cleanup acknowledgements remain admissible during recovery or cleanup.
Clients must also enforce the deadline locally with a suspend-inclusive clock.

On Linux, `process.hrtime` excludes system suspend. The relay bridges it with
the kernel `/proc/uptime` counter, sampling at most every 100 ms for ordinary
reads and forcing a fresh sample before messages and authority deadline sweeps.
Its 10 ms quantization is covered by shortening internal recovery/open budgets
by 10 ms; the absolute deadline on the wire includes this conservative margin.
Civil clock changes never renew a lease. Linux requires readable `/proc/uptime`;
modern macOS uses libuv's `mach_continuous_time`, and Windows uses its native
suspend-inclusive performance counter. No native addon is required.

`remote_session_resume` has a stable request ID for a session/transport attempt.
The same authenticated runtime and resume proof can reconcile an older observed
generation; commands always require the current fenced generation. Replaying the
same resume does not advance it again. The server never treats `ws.send()` as a
client receipt. Clients send `remote_session_state_ack` only after applying a
state. `commandReady` becomes true only after both parties applied the current
state/generation; its notification retains that revision to avoid an ACK loop.

`remote_session_reconcile` accepts `{requestId, sessions: [{remoteSessionId,
generation, stateRevision}]}`. Its `remote_session_reconciled` result contains
full typed session states, `complete: true`, and `absentSessionIds` for the
caller's obsolete bindings. Active states include the current snapshot and
sequence. Duplicate OPEN or ACCEPT never resets that snapshot. Metadata refreshes
via `endpoint_snapshot` do not replay historical closes. Initial registration on
a new transport catches up terminal obligations; later reconciliation is explicit.

Session closure revokes command authority immediately and emits
`remote_session_closed` with `cleanupState: pending`. Cleanup obligations live
outside the active-session registry and retry with bounded exponential backoff.
A target's committed teardown acknowledgement proves renderer stop, upload reader
settlement and cache quarantine; the server then emits `cleanupState: confirmed`.
Failures remain visible as `cleanupState: error`. Physical deletion after
quarantine is not part of admission. Pending cleanup fences only the same
owner/target pair, never another controller's independent session. Capacity is
bounded at 4,096 active sessions plus unresolved obligations; saturation rejects
new admission explicitly rather than forgetting cleanup proof. Terminal results
and idempotency records are separately bounded and retained together.

`endpoint_disable` and `endpoint_disable_started` echo the same `requestId` on
the current connection generation. Disable is monotonic on that transport,
idempotent, immediately hides command availability, and rejects new outgoing as
well as incoming sessions. Re-enable authenticates a new transport.

Discovery remains presence-only. `client_list` includes an increasing `revision`,
an observation timestamp, and each endpoint's `installationId`, `endpointId`,
`instanceId`, `instanceOrdinal`, `runtimeId`, `status`, `lastSeenAt`,
`canAcceptSession` and `reason`. States distinguish Available, Degraded
and Disconnected; disabled endpoints report Disconnected with reason
`disabled`. Recent unavailable endpoints are retained for up to five minutes in
a bounded 4,096-entry presence cache. Pair cleanup and scene ownership remain
private and do not label an endpoint Busy. Offline entries retain the entire
authenticated identity tuple; `lastSeenAt` uses epoch milliseconds.

Protocol v10 is a coordinated client/server cut-over. Older versions receive an
explicit protocol-version rejection; the server does not silently translate
lease or cleanup semantics. Run `npm test` before deploying both artifacts.

## Uploads

Uploads are bound to one `remoteSessionId` and generation. `upload_start`
declares normalized assets containing `assetId`, `fileId`, `sha256`, `size`,
`extension`, and `mediaIds`. Only PNG, JPG/JPEG, WebP, AVIF, and MP4 metadata is
accepted; both endpoints remain responsible for decoding and content checks.

`upload_chunk` contains `assetId`, contiguous byte `offset`, chunk `size`, file
`sha256`, and base64 data. Target `upload_progress` reports a durable contiguous
offset for each asset. `upload_resume` rewinds the relay to that durable offset.
`upload_complete` enters final validation only after every asset's durable offset
equals its declared size; bytes merely queued or relayed stay behind this barrier.
The server permits two concurrent outgoing uploads per endpoint and one per remote
session. Each sender has a 1 MiB durable-ACK window. Eight relay slots per target
bound its outstanding bytes to 8 MiB. Additional transfers wait before receiver
allocation and receive periodic `upload_resume_ready` capacity-wait receipts;
they do not consume a receiver timer or fail merely because another transfer is
active. A slot is released when bytes become fully durable, or on abort. Only an exact target `upload_finished` acknowledgement enters the
session asset inventory.

## Scene runs

`scene_prepare` carries an immutable revision, normalized manifest, scene, and
digest:

```text
SHA-256(canonical JSON { manifest, revision, scene })
```

Every manifest asset must be present in the exact session generation inventory.
If an asset is missing, the owner receives a correlated
`scene_asset_not_validated` error before a run is created. Once the run is
registered and the preparation request has reached the target, the server sends
the owner an aggregate `prepare_progress` acknowledgement with
`stage: "accepted"`; the owner waits for this barrier before reporting its own
progress or readiness.

Both endpoints then send `prepared` with a complete checklist and `armed` with
clock uncertainty no greater than the advertised policy. Uncertainty is the
NTP-style network bound (roughly half the best recent RTT), not a comparison of
the machines' wall clocks; timezone and manual clock settings are irrelevant.
The default 250 ms bound therefore supports an RTT up to roughly 500 ms. The
configuration enforces that the two endpoints' combined clock-error budget fits
inside `MOUFFETTE_SCENE_MAX_START_SKEW_MS`.

After both endpoints are armed, the server schedules `commit` on its monotonic
clock. `MOUFFETTE_SCENE_ACTIVATION_LEAD_MS` is the base presentation margin
(500 ms by default); the run adds twice the largest uncertainty reported by its
two endpoints so the COMMIT itself has time to cross the network, capped at the
protocol's 10-second maximum. The welcome policy advertises that maximum
possible lead, while each COMMIT carries
its exact effective lead. Both endpoints confirm the first presented frame with
`started` within five seconds of that deadline. Real compositor presentation
may differ by up to 750 ms before the run is considered unsafe. A run becomes
live only after both confirmations.

Scene messages include `prepare_progress`, `state_snapshot`, `stop`, and
`stopped`. The server derives both endpoints from the session, bounds payloads,
rejects stale generations, and preserves terminal tombstones for idempotent
retries. Removed `remote_scene_*` message routes do not exist.

The grid uses integer indices for keys (`slot`), clips (`startSlot`,
`sourceStartSlot`, `durationSlots`) and Stop. Cadence is an integer from 1 to 240
and is persisted per project. Video source coverage rounds up; its final partial
slot holds the last frame silently. Snapshots retain continuous milliseconds.
Projects v6 migrate to v7, adding full-scene clips to images/text and preserving video clips.
Projects v1–5 reset to v7 without resetting other profile components.

Render schema 5 stores a scene `timeline` (`maxDurationMs`, `slotsPerSecond`, `stopSlot`) and,
for each media, a complete intrinsic element state plus a `timeline` containing
full-state `keyframes`, non-overlapping presence `clips`, and `clipsInitialized`.
Slot indices are integers; source duration and the configured maximum use milliseconds.
The maximum supported duration is seven days.
Each project normally uses the client's configured three-minute maximum.
A Stop is either absent (`-1`) or a nonnegative slot at or before the last complete boundary. Keyframes
may exist beyond Stop. Clip intervals are half-open; outside clips all media are absent.
`sourceStartSlot` is null for image/text and a signed integer for video. A video's
occupied interval may extend beyond its source in either direction; these parts
hold the first/last image silently. The actual `durationMs` bounds moving playback,
not clip length. The receiver validates actual loaded media before preparation. Initially offscreen media still
participate in preparation because their keyframes can move them onto a screen.

Schemas 2–4 and automatic display/play/mute delays, fades, repeat and source-range
markers are rejected. State snapshots now contain only
`{ "timelinePositionMs": <finite milliseconds> }`, sampled at the enclosing
`sampledServerMonotonicMs`. The receiver advances its immutable timeline from
that clock correction; snapshots cannot replace media properties or programming.

## Fully resident media

Upload validation only confirms the durable file identity. The target then decodes
all image pixels, or validates the entire video/audio while retaining the original
compressed MP4 and one poster, before reporting `media_residency`. Video decoding
continues from memory during playback using bounded queues. Preparation primes
the configured start frames before a scene may launch.
These reports contain a session generation, monotonically increasing `sequence`,
and an `assets` array (`assetId`, `sha256`, `state`, `progress`, `error`). Only the
current authenticated target can report; stale sequences and unknown assets fail.
States are `analysing`, `queued`, `decoding`, `ready`, `waiting_for_memory`,
`capacity_insufficient`, or `error`; progress ranges from 0 to 1.

`scene_prepare` requires a current `ready` report for every manifest asset, and
both endpoints must acknowledge the `media_memory_ready` checklist stage. A new
report that invalidates a preparing or running scene stops that scene. Transfers
and memory reports remain independent so completion does not wait for RAM space.
Clients using a protocol version other than 10 are rejected; deploy the client
and server version together.

Targets publish full `remote_session_snapshot` updates on change and refresh
active sessions every five seconds. The relay keeps at most one pending snapshot
per session when the owner's buffered control traffic exceeds 64 KiB. The lease
sweep retries the latest replacement; cursor samples wait behind it and use the
session's most recent screen bounds. Empty display arrays are authoritative.

## Recovery diagnostics and verification

Structured state/rejection records include endpoint/session IDs, expected and
observed generations, revision and reason without resume tokens. Heartbeats are
aggregated. Metrics track lease expiry, resumed/rejected/reconciled sessions,
unresolved cleanup count/age and event-loop delays. A delayed event loop is
observable; it cannot revive a terminal session. Metric deduplication is bounded.

`instance_identity_protocol.test.js` covers signed ordinal validation, identity
collisions, immutable registration, retained offline identity, global generation
gaps, stale control/upload fencing and bounded transport history.
`transport_retirement_callbacks.test.js` exercises an old socket error followed
by authentication/resume and delayed close/error events; obsolete callbacks may
not affect the replacement session, presence or transport index.
`connection_recovery_v6.test.js` covers the fixed 3-second recovery deadline,
partial recovery without renewal, exact late-ACK rejection, lost RESUME
results, applied-ACK readiness, terminal replay suppression, disable races,
independent incoming sessions, bounded obligations and STOP acknowledgements
after logical closure. `upload_transport.integration.test.js` exercises real
WebSockets and authentication; upload tests include the 8 × 1 MiB relay limit.

`connection_recovery_soak.test.js` uses a fixed seed for 600 session cycles with
single/double partitions, lost state/cleanup ACKs, duplicate recovery commands,
late STOP ACKs, exact expiry boundaries and wall-clock jumps. It asserts the
configured history limits and empty live registries after every cleanup. Its
separate cache exercise uses real files across 128 cleanup cycles, four restarts
and injected deletion failures, then verifies empty work/data directories and
at most eight durable proofs for its eight reused cache scopes.
