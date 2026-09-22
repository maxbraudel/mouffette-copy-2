# Live system audio (protocol v12 extension)

The authenticated `welcome` adds `audioVersion:1`. Older v12 clients ignore
this additive field; new clients send no audio controls to a server without
it. Audio uses independent WSS publishing and viewing sockets. Video, uploads
and command sessions retain their existing transport and lifetime.

## Consent, grants and identity

`audio_share_consent {audioVersion:1,enabled}` announces client support and
replays the local screen-and-audio sharing preference. Both this consent and
`screen_share_consent` must be enabled. Consent is false on each new control
connection; no publication exists until an authorized viewer subscribes.

`request_audio_channel {requestId,audioVersion:1,role:"publish"|"view"}` returns
`audio_channel_token` with the same request, version and role, and a random
single-use token expiring after 15 seconds. Opening `?channel=audio&token=…`
binds to the exact admitted control object, its generation and role.
`audio_channel_ready` repeats version and role in the usual authenticated v12
envelope. Each role can be replaced independently; loss of control revokes both.

The viewer sends `audio_share_subscribe {remoteSessionId,generation,enabled}`
on control. The relay checks the current endpoint/runtime/generation, active
command-ready session, leases, consent and both sockets. The subscription has
no screen ID or viewport demand. Monitor changes do not change its identity.
The default admission limit follows the existing ten viewers per publisher
and 256 publisher limits.

The publisher receives `audio_publication_request {publicationId,enabled,
bitrateBps}`. One source packet is uploaded once and relayed to every listening viewer,
independent of monitor count. The publication UUID belongs to the publishing
transport and consent lifetime. Last unsubscribe stops it. Each viewer receives
`audio_share_state {remoteSessionId,generation,streamId,publicationId,enabled,
reason,bitrateBps}` with its own UUID. Replacing either relevant audio socket,
revoking consent or changing session generation invalidates that viewer epoch.

Publishers report `audio_publication_status {publicationId,reason}` using
`starting`, `streaming`, `permission_denied`, `unavailable` or `capture_error`.
Failure statuses suppress late media until `starting` or `streaming` resumes.
State additionally reports `disabled`, `unsupported`, `session_unavailable`,
`channel_unavailable`, `capacity_limited` or `unsubscribed` as appropriate.

## Binary framing and codec

All integers are unsigned big-endian and at most JavaScript's safe integer
maximum. Sequences start at one. An audio data message is:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 4 | ASCII `MAU1` |
| 4 | 16 | UUID, RFC 4122 byte order |
| 20 | 8 | Sequence |
| 28 | 8 | Source capture timestamp, microseconds |
| 36 | 1–1275 | One Opus packet |

The source uses its publication UUID. The relay rewrites only UUID and delivery
sequence for each viewer; it preserves the capture timestamp and payload. The
grant binds that compact UUID to authenticated publication/session authority.
There is no repeated JSON header, base64, WebSocket compression, container,
transcoding or retained audio history.

The negotiated format is stereo, 48 kHz, 20 ms per packet. The normal Opus
target is 96 kbit/s constrained VBR; the congestion fallback is 32 kbit/s.
Both are one source representation shared by all viewers. A weak viewer may
lower the common audio bitrate, but cannot lower other viewers' video source
profiles through this audio feedback.

Source timestamps use the same monotonic media domain as screen video. The
relay never substitutes wall time or receipt time. Decode/playout buffering,
capture application exclusion and local mute are client responsibilities.

## Receipts, budgets and recovery

An exact receipt is ASCII `MAA1`, the 16-byte epoch, then the uint64 sequence
(28 bytes total). The relay acknowledges authorized source ingress without
waiting for viewers. Viewers acknowledge transport consumption before decoding,
including obsolete epochs, then discard any packet without a current grant.
Forged or duplicate tuples free no other packet's credit.

Each viewer has at most 16 outstanding packets and 4 KiB, including socket
backlog. After the first measured ACK, a pending age above baseline RTT plus
150 ms stops new admission, with an absolute 250 ms cap including initial
admission. Baselines expire after 30 seconds. An unacknowledged packet older
than 500 ms terminates only the disposable audio pipe; a late first receipt
cannot establish a slow baseline. No queue of unsent encoded audio is kept. Publisher
ingress is limited to a 20 ms cadence with at most 100 ms burst debt. Viewer
and aggregate egress are paced with a 100 ms debt bound; viewer ordering rotates.

The publisher discards capture timestamps more than 250 ms from its current
local media clock. Relay and viewer retain the best source-to-arrival clock
offset and discard audio more than 150 ms behind it, allowing only 200 ppm of
positive clock drift. Received video timestamps can seed this same viewer
estimate before audio arrives, so delayed first audio cannot legitimize an
initial TCP backlog. Audio remains independent when no screen is visible.

`audio_source_budget {totalBps,congested}` is sent at most twice per second.
`audio_view_feedback {remoteSessionId,generation,streamId,droppedPackets,
bufferedMs}` is coalesced at 500 ms. Low total budget (below 256 kbit/s) or
audio congestion lasting one second selects 32 kbit/s. Recovery needs ten
healthy seconds and at least 384 kbit/s. The source reserves 112/48 kbit/s
(normal/fallback) from its existing combined media target. The relay similarly
reserves per-viewer and aggregate audio capacity from its existing media
egress budget. Aggregate audio reservation cannot exceed that server budget
minus 16 kbit/s; encoded packets exceeding available capacity are dropped.
These are application-byte budgets, excluding TCP/TLS overhead.

Clients retry failed audio sockets from 500 ms up to ten seconds, replaying
desired consent/subscription. Replacement epochs fence queued transport and
decoder work. Stopping or muting a viewer clears its local stream immediately.

## Verification

`audio_share_protocol.test.js` runs with `npm test`. It covers compact framing,
safe integers, authorization, exact receipts, consent/revocation, role misuse,
single source fan-out, monitor independence, socket replacement, bounded slow
viewers, stale TCP bursts, late first ACKs, profile hysteresis, aggregate pacing
and real WebSocket connections.
`tst_AudioTransport` exercises the desktop transport against the real Node
coordinator using synthetic packets, without capture permissions or playback.

## Build dependencies

The relay uses the existing Node.js `ws` and `node:crypto` dependencies; no
codec, native audio package or new npm dependency is required. Desktop
`AudioTransport` uses the existing Qt Core/Network/WebSockets modules and
`NetworkProxyPolicy` for the same authenticated HTTP CONNECT, SOCKS5 or system
proxy behavior as the other channels. `AudioWire.h` is header-only. The capture
worker and Opus codec are separate desktop components; the relay does not load
those libraries. Both relay and client should be upgraded for audio; mixed
versions keep their existing screen-video transport.
