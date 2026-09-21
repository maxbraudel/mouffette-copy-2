# Live screen preview (protocol v12 extension)

Video uses WebSockets independent of control and media uploads. Shared
publication v2, described below, separates publishing from viewing and retains
only bounded, short-lived encoded IDRs. The legacy channel relays complete H.264
Annex-B access units without retention. Neither mode transcodes, uses base64,
or applies WebSocket compression. No server GPU is needed.

## Legacy authorization and delivery

`request_screen_channel {requestId}` on authenticated control returns a random
256-bit, single-use token valid for 15 seconds. Opening
`?channel=screen&token=…` binds to that exact admitted control object and
connection generation. The authenticated `screen_channel_ready` envelope
confirms the binding. New clients include `feedbackVersion:1, maximumEdge:3840`
in the request; the token binds these capabilities to that exact video socket.
Omitted capabilities default to feedback disabled and a 1920-pixel decoder
limit. The new server announces `feedbackVersion:1, maximumEdge:3840` in ready.
Legacy publishers receive no new feedback message types. Effective screen
requests are capped by both participants' announced maximum edge. Grants expose
`receiverMaximumEdge` independently of viewport demand, so a shared encoder can
respect an older viewer's decoder limit. 4K frames
are refused if either participant did not announce support.

The target sends `screen_share_consent {enabled}`. Consent defaults to false
on every new server-side control connection; the client replays its persisted
preference after authentication. An owner sends `screen_share_subscribe
{remoteSessionId,generation,enabled}` only for its visible remote canvas.
Capture requires both consent and subscription, an active command-ready
session, and both current video sockets. The server sends
`screen_share_request` to the target and `screen_share_state` to the owner,
with `enabled`, `reason`, and a newly generated UUID `streamId`.

Consent, socket, session, subscription, or topology revocation invalidates the
stream UUID. Subsequent activation always receives a new UUID, fencing queued
frames across the independent control and video TCP connections. A topology
change invalidates the old grant before sending the new snapshot; the new
grant follows snapshot delivery. Identical periodic snapshots preserve it.
Failed consent/unsubscribe sends abort the local video socket and replay the
desired state on reconnection. Tokens and video payloads are never logged.

## Frames and receipt windows

A binary message contains ASCII `MSV1`, a two-byte big-endian JSON-header
length, the UTF-8 JSON header (at most 1024 bytes), and one complete H.264
Annex-B access unit (at most 2 MiB). Required header fields are
`remoteSessionId,generation,streamId,screenId,sequence,width,height,keyFrame,
codec`; `codec` is `h264`, and optional `timestampUs` is a safe nonnegative
integer. No extra fields are accepted. Generation and sequence are positive
safe integers. Width and height are even, between 2 and 3840. Screen IDs must
belong to the target's current physical topology.

`screen_frame_ack {streamId,screenId,sequence}` is a text message on the video
socket only. The relay acknowledges source receipt; viewers acknowledge
receipt before decoding, including obsolete-epoch packets they then discard.
Server ACKs include the usual authenticated envelope. Every ACK frees only its
exact tuple on that socket; a forged or stale tuple cannot free another frame.

The relay admits at most 64 unacknowledged frames and 2048 KiB outstanding
by default; desktop limits are configured separately.
One standalone IDR up to 2 MiB is admitted when the window and socket queue are
empty. Receipt accounting includes bytes already handed to OS buffers.
Congestion drops dependent frames until a new IDR; the relay requests one via
`screen_share_keyframe` on control, at most once per second per session by default.
Admission rotates among waiting display streams so displays beyond the available
receipt slots cannot starve in a stable capture callback order; no video frames are queued
for fairness. Inactive waiters expire at the configured receipt deadline, allowing
high-RTT displays to retain their turn; hidden or failed screens release it
immediately. Sources force an IDR
after local drops. An outstanding receipt older than
3000 ms aborts the disposable socket to discard stale kernel queues while the
control session remains intact. Revocation releases obsolete-epoch receipts.

Targets can send `screen_share_status` with `starting`, `streaming`,
`permission_denied`, `unavailable`, `capture_error`, or `error`. The server
relays this reason in the owner state while preserving the capture request
for recovery. Optional `screenId` scopes the status to one currently requested
physical screen; omission preserves the legacy global status. The scoped owner
`screen_share_state` contains the same `screenId` and does not change the global
status or stream UUID. Failed screens reject new frames while other screens
continue. `starting` or `streaming` clears that screen's failure, and recovery
requires an IDR. Refresh replays cached screen statuses after the global state;
hiding a screen or revoking the grant clears its cached status. Only the current
publisher in a command-ready session may change either kind of status.

`screen_share_protocol.test.js` covers authorization, real video sockets,
malformed framing, epoch revocation, topology changes, bounded congestion,
forged receipts, IDR recovery, and receipt deadlines.

## Viewport demand and receiver feedback

`screen_share_subscribe` accepts optional `screens`, at most 64 unique entries
`{screenId, maximumEdge}` from the session's physical topology. `maximumEdge`
is an even integer from 2 to 3840 and is a demand hint, not a rejection threshold
for a previously encoded frame in flight. Omission preserves legacy all-screen
behavior; `[]` requests no pixels while preserving the active grant. Changes
are echoed in `screen_share_request` and `screen_share_state` without rotating
the stream UUID. Hidden screens are rejected immediately at relay admission;
re-adding a screen requires an IDR. Consent, topology and session fencing remain
independent and unchanged.

The relay sends authenticated `screen_share_feedback` on the publisher's video
socket, bound to `remoteSessionId,generation,streamId,screenId`, containing:

- `deliveryRttMs`: elapsed relay send-to-exact-viewer-ACK time, or zero before
  the first measured receipt. It includes queueing and is not end-to-end render
  latency or a measurement of total Internet bandwidth.
- `bufferedBytes`: outstanding recipient receipt/socket backlog, with overlapping
  counts combined using their maximum.
- `congested`: relay admission pressure or receiver decode/drop feedback observed
  during this report interval.
- `uploadActive`: a live transfer targets the viewing endpoint; video should yield
  capacity on that shared download path.

Reports are coalesced per stream/screen every 500 ms by default. Pending state
contains metadata only, never video frames. Revoking the grant clears it.
Unknown/duplicate receipts release no credit and produce no measurement.

A viewer can send `screen_view_feedback` on its own video socket with exactly
`type,remoteSessionId,generation,streamId,screenId,decodeMs,droppedFrames`.
All counters are bounded integers (`decodeMs` 0–60000, `droppedFrames` 0–10000).
Only the current, command-ready session owner may report a requested screen.
Syntactically valid reports arriving after revocation or hiding are discarded
without closing the socket; they release no credit and create no samples.
Reports are rate limited per lane. A decode taking over 100 ms or any dropped
frame marks congestion; this telemetry never grants permission or releases
transport credit.

The following server `.env` keys are validated at startup:

| Key (prefix `MOUFFETTE_`) | Default | Range |
| --- | ---: | ---: |
| `SCREEN_FEEDBACK_INTERVAL_MS` | 500 | 100–5000 |
| `SCREEN_ACK_TIMEOUT_MS` | 3000 | 500–15000 |
| `SCREEN_MAX_BUFFERED_KIB` | 2048 | 32–8192 |
| `SCREEN_MAX_INFLIGHT_FRAMES` | 64 | 1–256 |
| `SCREEN_QUEUE_TARGET_MS` | 150 | 25–2000 |
| `SCREEN_KEYFRAME_REQUEST_INTERVAL_MS` | 1000 | 250–10000 |

The receipt deadline must exceed both the feedback interval and the queue target. One standalone IDR
may exceed the queue watermark up to the 2 MiB hard packet limit; a dependent
frame cannot use that exception. Increasing queue sizes does not increase link
capacity and can increase delay.

The hard frame/byte bounds accommodate a healthy bandwidth-delay product;
reaching a credit limit or waiting for another display is not by itself a
congestion signal. Each receiving socket measures its own minimum exact-ACK
RTT. A lower or equal sample refreshes the baseline; after 30 seconds without
one, a new sample replaces it, allowing a changed route to recover without
replacing TCP. Once established, an outstanding frame older than that RTT plus
`SCREEN_QUEUE_TARGET_MS` pauses new admission and reports congestion. Before
the first measurement, the grace is `min(1000 ms, ACK_TIMEOUT_MS / 2)`.
The sweep reports growing delay even when the sender has already stopped.
The hard ACK timeout remains independent and discards an irrecoverable pipe.
Tests cover two healthy 500 ms / sixteen-frame windows, 300 ms of additional
queueing after a 40 ms baseline, and eight displays fairly sharing 1 Mbit/s
under an intentionally restricted six-frame test window.

## Shared publication v2

An opt-in `request_screen_channel` adds `mediaVersion:2` and `role:"view"`
or `role:"publish"`; tokens and ready messages repeat these capabilities.
Tokens are single-use and bound to the exact control transport and role.
Concurrent role requests do not invalidate one another. The two sockets have
independent failure/replacement lifetimes: losing a viewing socket does not
stop that endpoint publishing. Control loss revokes both. Setting
`MOUFFETTE_SCREEN_SHARED_ENABLED=false` returns legacy capabilities, allowing a
modern client to use its existing combined-channel fallback.

For a v2 publisher, the server aggregates authorized session subscriptions into
`screen_publication_request {publicationId,enabled,screens}`, where each screen
contains `screenId,maximumEdge,layers:["main", "low"]` (low is optional).
The UUID belongs to the source transport/consent/topology epoch, not one viewer.
Joining, leaving, viewport changes and layer demand preserve it while a useful
subscription remains. Last departure stops publication and deletes its cache.
The default admission limit is ten distinct viewers per publisher. Server
publication capacity is separately bounded.

The source sends each requested encoded layer once, independent of viewer
count. This implementation uses main plus an optional low encoding at the
source; it does not manufacture lower-resolution pictures on the server.
Main is always an authorized fallback for a requested physical screen.
`screen_publication_status {publicationId,screenId,layer,reason}` accepts
`starting`, `streaming`, `inactive`, `permission_denied`, or `capture_error`,
and optional `bitrateBps` (0–400000000), `fps` (0–120). Positive hints describe
the layer. Inactive/failed layers cannot publish until starting/streaming;
starting is idempotent for an already usable layer. Failure is scoped to that
screen/layer. If optional low is inactive and a viewer has a smaller hard decoder
limit, main is capped to that limit; a viewer's weak bandwidth alone never
lowers the shared main request.

Ingress binary uses `MSV2`, uint16 big-endian JSON length, then one Annex-B
access unit. Required header fields are
`publicationId,screenId,layer,sequence,width,height,keyFrame,codec:"h264"`;
`timestampUs,bitrateBps,fps` are optional. The payload limit remains 2 MiB,
header 1024 bytes, even dimensions 2–3840, bitrate hints 1–400000000 and FPS
1–120. Unknown fields and invalid roles are rejected. A declared IDR must
contain SPS, PPS and IDR NAL units before it can enter the snapshot cache.
This framing check does not decode or certify the bitstream's visual contents.

`screen_publication_ack {publicationId,screenId,layer,sequence}` is sent on
the publisher socket only after validating current publication authority. It
acknowledges ingress consumption independently of all downstream ACKs. A
late packet from a just-hidden lane may be acknowledged and discarded without
restoring its demand. Old publication UUIDs, another publisher's UUID, obsolete
transports and revoked consent release no source credit.

Each viewer receives the existing session-bound `MSV1` envelope, with server
delivery sequence numbers independent of source sequence/layer. Every delivery
rechecks consent, current transports, generation, command-ready session and
physical screen demand. Main/low switches require an IDR. A dependent frame
also requires a contiguous source sequence for that viewer: a cached IDR does
not permit skipping intervening source references merely because delivery
sequence numbers are contiguous. Requests for source IDRs are coalesced and
rate-limited per publication, not multiplied by viewer count.

Viewer ACK/decode feedback controls only that consuming endpoint's target.
Its shared bandwidth budget covers all of its visible screens and publishers;
layer selection divides that budget between screens. A healthy viewer can
retain main while another receives low or IDR-only snapshots. Viewer feedback
is not forwarded into a v2 publisher's uplink controller. Reopening a disposable
view socket preserves a conservatively capped learned target for the same
authenticated control object, while discarding old receipt/RTT/socket state.
Upload reservations reduce only the relevant viewer's download target.

Snapshot storage is the latest complete encoded IDR per publication/screen/layer,
bounded globally by packet bytes and age. There are no raw pixels, files, GOP
queues, or per-viewer frame histories. Repeated ticks never send the identical
cached source image as a new delivery; expired cache is discarded. A snapshot
has at most one outstanding receipt per screen. A long snapshot reserves the
endpoint window alone and incurs its entire serialization debt. Its receipt
deadline includes a bounded serialization allowance, avoiding false timeouts
on a useful slow delivery. An image that cannot fit the transfer/age bounds is
skipped without taking fairness priority or repeatedly demanding an equally
large IDR. Thus extremely low rates do not guarantee that every image is
deliverable; they remain bounded and do not stall other eligible screens.

These settings join the existing receipt/queue limits (`MOUFFETTE_SCREEN_`
prefix on every key):

| Suffix | Default | Purpose |
| --- | ---: | --- |
| `SHARED_ENABLED` | true | Opt-in shared publication negotiation/rollout |
| `MAX_VIEWERS_PER_PUBLISHER` | 10 | Distinct subscribed viewers per source |
| `MAX_PUBLICATIONS` | 256 | Active source publication admission cap |
| `KEYFRAME_CACHE_MIB` | 16 | Global retained encoded IDR bytes |
| `KEYFRAME_CACHE_TTL_MS` | 5000 | Maximum cache age |
| `SERVER_EGRESS_BPS` | 100000000 | Aggregate paced video egress, shared with legacy |
| `VIEWER_MIN_BPS` | 64000 | Adaptive receiver target floor |
| `VIEWER_INITIAL_BPS` | 3000000 | Initial adaptive receiver target |
| `VIEWER_MAX_BPS` | 20000000 | Adaptive receiver target ceiling |
| `VIEWER_RECOVERY_MS` | 3000 | Healthy hold before gradual recovery |
| `SNAPSHOT_INTERVAL_MS` | 1000 | Minimum interval between new snapshots |
| `SNAPSHOT_MAX_TRANSFER_MS` | 4000 | Maximum estimated snapshot serialization |
| `VIEWER_UPLOAD_PERCENT` | 40 | Video target percentage during incoming uploads |

Targets are policy bounds, not Internet line-speed measurements or guaranteed
throughput. Viewer minimum/initial/maximum must be ordered; snapshot interval
and maximum transfer must be smaller than cache TTL. Long snapshot deadlines
must remain consistent with the clients' stale/first-frame policy when changing
defaults. Source and delivery shaping bound video; separate TCP sockets and a
video cap alone do not provide strict cross-application file/control QoS.

`screen_publications.test.js` covers ten-viewer fanout, single ingress ACKs,
heterogeneous layer choices, legacy decoder caps, independent bidirectional
roles, source-chain continuity on late join, long snapshot deadlines, snapshot
fairness, learned-budget reconnects, authorization/epoch fences, cache budgets,
global paced byte bounds and rollout fallback.
