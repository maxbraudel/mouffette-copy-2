# Connection and session recovery (protocol v6)

Deploy the v6 server and clients together. Authentication is versioned; an
incompatible client receives a terminal compatibility error instead of retrying.
A process/server restart ends old sessions. Retained selection can create a new
empty session, but recovery never issues PLAY or COMMIT.

## Connection authority and user intent

`ConnectionManager::setConnectionEnabled(bool)` is the idempotent authority for
network intent and the status exposed to QML. Each process starts enabled. Disable
immediately fences new operations and cancels retries; the UI shows Disconnecting
until drainage finishes, then Disconnected. Enable during drainage is retained;
the latest intent wins. URL changes go through the same transition. Completion
callbacks carry a transition ID and cannot complete a newer connection cycle.

The supervised states are Disconnected, Connecting, Authenticating,
Synchronizing, Connected, Degraded, Reconnecting, Disconnecting, Failed and
CleanupPending (receiver recovery has not finished). Authentication alone does
not mean Connected: endpoint registration, authoritative inventory reconciliation
and receiver readiness must finish. Session command readiness remains separate.

While network intent is enabled, an unavailable server is retried indefinitely,
including after all sessions expire. Background delays grow through 1, 2, 4, 8,
16 and 30 seconds, with 20% jitter (24–36 seconds at the plateau). There is no
attempt limit or outage-duration limit. Their counters reset only after
30 seconds of stable connection. DNS failures, refused connections, timeouts,
network loss and TLS handshake failures remain retryable; certificate validation
is never bypassed. A repaired server is authenticated and synchronized on the
next successful attempt without another Enable click. Intentional Disable stops
this loop; protocol incompatibility and invalid authentication envelopes remain
explicit terminal errors.

Each connection/authentication attempt allows 10 seconds independently of the
remaining session recovery budget. Expiration revokes the old session, not the
new TCP/authentication attempt or network intent. A stale transport timer cannot
abort a newly authenticating socket.

## Transport health and session deadlines

The server advertises these policy values in the signed connection's welcome:

| Policy | Default | Configuration in `server/.env` |
| --- | ---: | --- |
| Heartbeat interval | 750 ms | `MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS` |
| Degraded threshold | 1,500 ms | `MOUFFETTE_REMOTE_SESSION_DEGRADED_AFTER_MS` |
| Replace silent transport | 3,000 ms | `MOUFFETTE_PEER_LEASE_TIMEOUT_MS` |
| Total session recovery budget | 5,000 ms | `MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS` |
| New session opening deadline | 5,000 ms | `MOUFFETTE_REMOTE_SESSION_OPEN_TIMEOUT_MS` |

Five seconds is a total budget from the last relevant proof of life, not a fresh
interval per retry, authentication or RESUME. The server distributes each
session's absolute monotonic deadline and revision during normal operation and
in heartbeat acknowledgements. A healthy participant cannot renew an absent
participant's proof. Clients map server deadlines conservatively to their local
clock, including observed transport uncertainty, and enforce expiration before
processing a late command. Local clocks include system sleep (continuous Mach
time on macOS, GetTickCount64 on Windows and CLOCK_BOOTTIME on Linux). Civil
clock adjustments do not extend running deadlines.

During degradation/recovery only a scene already Live may continue. Preparation,
activation, new commands and upload streaming stop. STOP and cleanup remain
possible. At expiration each participant revokes the session locally even when
the server is unreachable; renderers stop and transfers are cancelled. Recovery
of selection never restarts the old scene.

## Reconciliation, replay and command fencing

Generation fences commands; `stateRevision` describes observed authoritative
state. RESUME and Disable have request IDs and replay results. A valid resume
proof can reconcile an older observed revision, but cannot authorize an old
command generation. Sending an envelope does not acknowledge its application.

On authentication the client exchanges its retained session inventory using
`remote_session_reconcile`; the response supplies live state, relevant terminal
state and outstanding cleanup. Clients send `remote_session_state_ack` after
application. Both participants must acknowledge the current state before
`commandReady` permits work. Lost ACKs/readiness notifications are recovered by
inventory reconciliation and heartbeat state observations.

Exact repeated OPEN/snapshot/state/terminal envelopes are idempotent. A replay
cannot roll back a snapshot, readiness or revision. A more recent authenticated
terminal envelope may skip intermediate revisions, while session identity and
runtime checks prevent closing a replacement session. Metadata-only endpoint
updates do not replay all historical closes.

## Selection and activity

Selection is retained in process memory independently of the Project, Canvas and
session. Automatic project purge retains it; explicit navigation/deletion clears
it. Returning activity, discovery, authentication and completion of cleanup
reconcile selection. Only one OPEN per intended target is in flight; a lost
reply is retried with the same request ID and backoff. Transient failures retry
with delay; permanent failures stay visible until a relevant capability/runtime
change or explicit selection permits reevaluation.

`client/.env` retains the existing outgoing idle thresholds:

- `MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS=60000`
- `MOUFFETTE_REMOTE_SESSION_HIDDEN_TIMEOUT_MS=120000`
- `MOUFFETTE_PROJECT_HIDDEN_RETENTION_MS=240000`

After purge, a provisional workspace waits for a valid authenticated snapshot
before creating an empty Project. Offline targets stay selected and wait for
presence. These rules govern outgoing activity only: incoming sessions do not
require a visible Canvas, pointer activity or another session to close. Multiple
incoming sessions are allowed. The renderer's single simultaneous Live scene
is an independent admission rule with an explicit conflict response.

Presence is revisioned and includes the latest confirmed observation, ability to
accept sessions and an unavailability reason. The server rechecks both endpoints
on OPEN, including the Disable/drain fence.

## Disk work and cleanup

Logical session termination, resource release, durable quarantine and physical
deletion are distinct. STOP and teardown acknowledgements belong to their own
transactions, so logical termination does not invalidate required cleanup.
Old cleanup obligations do not block unrelated session scopes.

Incoming chunk writes and fsync run on a serialized worker queue. Only persisted
bytes advance progress; a cancelled generation's late completion cannot emit an
ACK. Resume waits for outstanding writers before rebinding/truncating to durable
offsets. The client bounds queued bytes to 8 MiB; the server allows eight target
upload slots with 1 MiB unacknowledged per slot and queues additional starts.

Runtime quarantine, targeted asset quarantine, durable recovery and physical
deletion metadata run on a serialized I/O queue. A scope is fenced before work is
queued. Quarantine admission is bounded to 64 transactions, physical cleanup to
64 jobs, and in-memory teardown result history to 4,096 entries.
Each durable catalogue reserves up to 4,096 scopes or removal transactions,
counting live scopes and outstanding intents as well as completed records.
Saturation blocks new admission; already reserved cleanup remains possible. Completed official proofs are collected only after
24 hours of unchanged monotonic observation (the maximum supported server replay
horizon); restarting the client restarts that conservative observation period.
Provisional proofs, failed cleanup and outstanding intents are retained. Asset
removal proofs follow the same retention rule rather than blind oldest-first
eviction. Unresolved obligations remain durable; queue saturation returns an explicit error. Only a
durable logical commit is acknowledged. Physical deletion and global receiver
recovery retry automatically with bounded backoff without requiring a reconnect.
Startup initialization runs before networking and replays durable intents.

Live media retain their existing residency protections. An idle-media timer does
not free resources needed by a running scene.

## Validation and diagnostics

See the [implementation validation record](connection-recovery-validation.md) for
results and the remaining general graphical test failures.

`server/connection_recovery_v6.test.js` exercises deterministic deadlines,
simultaneous recovery, state acknowledgements, replay, admission and cleanup.
`RemoteSessionIntegration` starts the real Node relay and uses actual Qt clients;
Node and `npm ci` in `server` are required. It covers 2/4/6-second disruptions,
peer ordering, lost replies/ACKs and duplicate OPEN. The cache/upload and renderer
suites check delayed resource release, cancelled writers, durable offsets and
local expiration. CI installs Node and runs protocol plus native suites on macOS
and Windows.

From the repository root:

```sh
npm --prefix server ci
npm --prefix server test
cmake --build client/out/build/macos-debug
ctest --test-dir client/out/build/macos-debug --output-on-failure
```

Server structured logs/metrics include transition cause, request, generation,
revision, replay, recovery outcomes and cleanup age. Client rejection diagnostics
identify the mismatched authority fields; heartbeat ACK logs are aggregated.
Never log authentication signatures, private keys or resume/upload tokens.
