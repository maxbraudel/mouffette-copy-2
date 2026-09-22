# Live system audio (protocol v12 extension)

The authenticated `welcome` adds `audioVersion:1`. Older v12 clients ignore
this additive field; new clients send no audio controls to a server without
it. Audio uses independent WSS publishing and viewing sockets. Video, uploads
and command sessions retain their existing transport and lifetime.

## Consent, grants and identity

`audio_share_consent {audioVersion:1,enabled}` announces client support and
replays the local system-audio sharing preference. Audio consent is independent
of `screen_share_consent`; no screen consent, video subscription, physical display
or video socket is required. Consent is false on each new control connection;
no publication exists until an authorized viewer subscribes.

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
and 256 publisher limits. An admission rejection sends a disabled
`audio_share_state` with `reason: "capacity_limited"` and empty stream/publication
IDs so the viewer can explain the failure.

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
They survive subscription state queries while the publication exists and do not
change video publications. Screen failures and screen consent revocation likewise
leave an authorized audio publication running.
State additionally reports `disabled`, `unsupported`, `session_unavailable`,
`channel_unavailable`, `capacity_limited` or `unsubscribed` as appropriate.

## Binary framing and codec

All integers are unsigned big-endian and at most JavaScript's safe integer
maximum. Capture sequences start at one for a new publication; the first
delivered sequence can be larger after an upstream drop. An audio data message is:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 4 | ASCII `MAU1` |
| 4 | 16 | UUID, RFC 4122 byte order |
| 20 | 8 | Sequence |
| 28 | 8 | Source capture timestamp, microseconds |
| 36 | 1–1275 | One Opus packet |

The source uses its publication UUID. The relay rewrites only the UUID for each
viewer; it preserves capture sequence, timestamp and payload. Sequence gaps
remain visible across capture IPC, sender admission and relay admission, so the
receiver can apply Opus packet loss concealment and report actual loss. The
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
capture application exclusion and local listening preferences are client responsibilities.

## Receipts, budgets and recovery

An exact receipt is ASCII `MAA1`, the 16-byte epoch, then the uint64 sequence
(28 bytes total). The relay acknowledges authorized source ingress without
waiting for viewers. Viewers acknowledge transport consumption before decoding,
including obsolete epochs, then discard any packet without a current grant.
Forged or duplicate tuples free no other packet's credit.

Actual socket backlog is bounded separately at 4 KiB. Receipt credit includes
propagation and reverse-path latency, so a healthy long RTT does not create
artificial media loss. Before the first ACK, the receipt window permits one
second at the normal-profile application reservation. After measurement its duration is
`clamp(baseline RTT + 150 ms, 250 ms, 2000 ms)`, with at least 4 KiB of byte
credit and at most `min(128, floor(window / 20 ms) + 2)` packets. Byte credit
uses the normal profile even during fallback, so a bitrate reduction
does not invalidate larger packets already in flight. Pending age
above the baseline plus 150 ms stops admission (one second before measurement).
Baselines refresh after 30 seconds. Receipt timeout is two seconds before the
first ACK, then `clamp(baseline RTT + 500 ms, 500 ms, 2000 ms)`. Expiration
terminates only the disposable audio pipe; late receipts cannot normalize
unbounded queues. This RTT allowance never extends a stale packet's playback
deadline. No queue of unsent encoded audio is kept. Publisher
ingress is limited to a 20 ms cadence with at most 100 ms burst debt. Viewer
and aggregate egress are paced with a 100 ms debt bound; viewer ordering rotates.

The publisher discards capture timestamps more than 250 ms from its current
local media clock. Relay and viewer retain the best source-to-arrival clock
offset and discard audio more than 150 ms behind it, allowing only 200 ppm of
positive clock drift. Received video timestamps can seed this same viewer
estimate before audio arrives, so delayed first audio cannot legitimize an
initial TCP backlog. Audio remains independent when no screen is visible.
Sub-microsecond drift allowance accumulates across frequent interleaved audio
and video observations. Rejected backlog cannot move the accepted offset.

The viewer uses a separate bounded jitter target, initially 80 ms and at most
150 ms. After at least 200 ms of advancing packets under deadline pressure,
it can raise the target to the measured lag plus 40 ms, within that bound.
After 30 seconds with spare margin it can reduce the target by 1 ms per second.
This changes the playout margin without teaching old packets a new capture
time. Audio output clocks carry paired source/local timestamps back from the
worker; video presentation follows that pair without rebasing it at GUI receipt.

A permanent route change must not mute an epoch forever. Sustained late ingress
with one second of both source-time and arrival-time progress retires the
publishing pipe. A viewer similarly retires its pipe for persistent lateness
beyond the maximum target. If recent video demonstrates a faster path, its
source/local clock evidence survives the audio reconnect: a new late audio
packet cannot legitimize a late baseline. A compressed TCP backlog alone
cannot trigger that reanchor. Reconnection obtains a fresh epoch and flushes
queued bytes before relearning the path when no faster clock evidence exists.
Capture process replacement and capture failure also rotate the
publication, fencing old encoder state and restarted capture sequences.

This recovery handles flushable audio backlog and common-path route changes.
A permanent audio-only path delay beyond the 150 ms differential budget cannot
both play and satisfy that limit. The viewer reports local `timing_unavailable`
with a diagnostic issue and reconnects with bounded exponential backoff until
the path recovers; it does not silently present late audio as synchronized.
Only timely media, rather than authentication alone, resets that backoff.

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

Every five seconds with activity, the relay emits `audio_transport_summary`
with packet/drop counts, retired socket counts and maximum receipt RTT, socket
backlog and reported playback backlog. The desktop logging category
`mouffette.audio.transport` records the corresponding admission/staleness counts,
capture age, receipt state and current jitter target. No encoded packet content
is logged. These counters distinguish local capture/IPC stalls from relay
admission and receiver buffering.

This remains a reliable TCP transport, with TCP head-of-line blocking under
loss. Desktop packet dispatch also passes through the GUI event loop. Bounded
queues, concealment and recovery limit their impact; they do not provide the
loss-independent delivery of RTP/WebRTC or an exact physical DAC timestamp.

## Verification

`audio_share_protocol.test.js` runs with `npm test`. It covers compact framing,
safe integers, authorization, exact receipts, consent/revocation, role misuse,
single source fan-out, monitor independence, socket replacement, bounded slow
viewers, healthy long RTT, stale TCP bursts, route-shift epoch recovery, capture
sequence gaps, late first ACKs, failed writes, bounded diagnostics, profile
hysteresis, aggregate pacing and real WebSocket connections.
`tst_AudioTransport` exercises the desktop transport against the real Node
coordinator using synthetic packets, without capture permissions or playback.
It also covers fractional clock drift, bounded jitter adaptation, faster-video
clock fencing, capture-process epoch replacement and control-session survival.

## Build dependencies

The relay uses the existing Node.js `ws` and `node:crypto` dependencies; no
codec, native audio package or new npm dependency is required. Desktop
`AudioTransport` uses the existing Qt Core/Network/WebSockets modules and
`NetworkProxyPolicy` for the same authenticated HTTP CONNECT, SOCKS5 or system
proxy behavior as the other channels. `AudioWire.h` is header-only. The capture
worker and Opus codec are separate desktop components; the relay does not load
those libraries. Both relay and client should be upgraded for audio; mixed
versions keep their existing screen-video transport.
