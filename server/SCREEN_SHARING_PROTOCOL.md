# Live screen preview (protocol v12 extension)

Video uses a third WebSocket, independent of control and media uploads. The
server relays complete H.264 Annex-B access units without transcoding, base64,
compression, or screen-content retention.

## Authorization

`request_screen_channel {requestId}` on authenticated control returns a random
256-bit, single-use token valid for 15 seconds. Opening
`?channel=screen&token=…` binds to that exact admitted control object and
connection generation. The authenticated `screen_channel_ready` envelope
confirms the binding.

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
safe integers. Width and height are even, between 2 and 1920. Screen IDs must
belong to the target's current physical topology.

`screen_frame_ack {streamId,screenId,sequence}` is a text message on the video
socket only. The relay acknowledges source receipt; viewers acknowledge
receipt before decoding, including obsolete-epoch packets they then discard.
Server ACKs include the usual authenticated envelope. Every ACK frees only its
exact tuple on that socket; a forged or stale tuple cannot free another frame.

Both legs admit at most three unacknowledged frames and 512 KiB outstanding.
One access unit up to 2 MiB is admitted when the window and socket queue are
empty. Receipt accounting includes bytes already handed to OS buffers.
Congestion drops dependent frames until a new IDR; the relay requests one via
`screen_share_keyframe` on control, at most four times per second per session.
Admission rotates among waiting display streams so more than three monitors
cannot starve in a stable capture callback order; no video frames are queued
for fairness, and inactive waiters expire after 500 ms. Sources force an IDR
after local drops. An outstanding receipt older than
1500 ms aborts the disposable socket to discard stale kernel queues while the
control session remains intact. Revocation releases obsolete-epoch receipts.

Targets can send `screen_share_status` with `starting`, `streaming`,
`permission_denied`, `unavailable`, `capture_error`, or `error`. The server
relays this reason in the owner state while preserving the capture request
for recovery.

`screen_share_protocol.test.js` covers authorization, real video sockets,
malformed framing, epoch revocation, topology changes, bounded congestion,
forged receipts, IDR recovery, and receipt deadlines.
