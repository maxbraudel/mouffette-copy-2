# Remote audio reliability audit

This audit concerns live system audio and its relationship to remote screen
preview. The reported symptoms affect macOS and Windows in both directions.
It does not change received-scene media playback or screen capture permissions.

## Defects found and corrected

| Boundary | Defect | Correction |
| --- | --- | --- |
| Native capture → packets | Arbitrary native blocks were joined across short capture gaps. The next block could move the timestamp of already accumulated PCM. | Sample-aware packetization retains native timestamps and partial blocks, rejects duplicate callbacks and fences discontinuities/overruns. |
| Native timestamps | A missing WASAPI/SCK timestamp became callback arrival time, including the duration of the already captured block. | Track the expected first sample, and use arrival minus block duration only when the native timeline cannot be continued. |
| Capture scheduling | Native audio work could compete at normal priority during UI/GPU load. Unsupported SCK formats could leave an apparently running silent capture. | macOS audio queue QoS and Windows MMCSS Audio scheduling; fail explicitly on unsupported native formats. |
| Codec | System audio used voice inactivity suppression; loss recovery and codec lookahead compensation were missing. Invalid float input could poison codec state. | Continuous Opus encoding, finite normalized input, checked initialization, PLC/FEC support, and timestamps corrected using Opus-reported lookahead. |
| Publication → relay | Renumbering successfully forwarded packets concealed earlier losses. | Preserve capture sequence gaps end to end while retaining separate authenticated stream epochs. |
| TCP accounting | Unacknowledged bytes, which include healthy network round-trip time, were treated as unsent audio. A fixed acknowledgement deadline retired healthy high-RTT sockets. | Separate bounded socket backlog from RTT-based receipt credit; keep freshness checks independent of acknowledgements. |
| Recovery | A permanent timing shift could reject every packet indefinitely. Replacing the audio process restarted its sequence inside the old publication. | Adapt the jitter target within 80–150 ms; retire stale pipes with bounded retries and an explicit unavailable state; rotate publication epochs after capture/worker failures. |
| Audio callback | A contended shared mixer mutex silenced a complete callback. Mutable packet mapping could jump the playback position. | Fixed SPSC PCM storage, immutable mixer snapshots retired off the audio thread, and a continuous render cursor with bounded rate correction. |
| Resampling | Per-packet interpolation and timestamp changes could repeat/drop samples at packet boundaries. | Maintain contiguous sample positions independently from native source timestamps, with interpolation across packet boundaries. |
| A/V clock | The callback tail was reported without its corresponding local presentation time, and IPC receipt became the new clock origin. | Carry a paired source/local output estimate through the worker, service and screen scheduler. Reject stale reports without reanchoring them. |
| Video presentation | Every new image replaced the pending image but inherited its older deadline. The 40 ms wait ceiling was less than the audio buffering target. | Bounded timestamp-ordered presentation queue: display the newest due image, preserve future images' own timestamps, and stop waiting after 150 ms. |
| IPC | The backlog check ignored the size of the next message; a partial write could corrupt subsequent framing. | Check the prospective complete frame size; tear down a partially written stream and keep mute/stop/reset commands reliable. |

The implementation stays within the negotiated audio v1 wire format. New clients
retain interoperability with older peers, but both publisher and relay need the
updated sequence behavior for all losses to remain visible. The audio worker is
the application's own executable and must be updated with the application.

## Timing and resource policy

The source uses one native monotonic epoch for audio and video. The receiver
maps this epoch onto its own media clock using transit observations. Packet
arrival does not directly reposition the sample cursor. Actual PCM storage,
IPC backlog and socket queues remain bounded; receipt credit is a separate
accounting window, not a queue of audio waiting for playback.

The jitter target starts at 80 ms and adapts up to 150 ms after sustained timing
pressure. Recovering a stale socket flushes its old data before establishing a
new audio epoch. When recent video supplies faster clock evidence, that evidence
survives the audio reconnect. An audio-only path permanently more than 150 ms
behind video cannot satisfy this policy; it reports `timing_unavailable` and
retries with bounded backoff instead of silently replaying late sound or claiming
that audio is available. Queue recovery does not make such physical delay vanish.

Each screen retains at most 12 pending decoded surfaces and uses a 64 MiB
per-screen budget, reduced when multiple screens share the presentation budget.
A single surface is retained even when it exceeds its share. Costs use the actual
decoded YUV plane sizes, including stride. Overflow preserves the next due image
and thins future images; repeatedly replacing the oldest image would otherwise
starve continuous high-resolution video. Muting, an audio epoch
change, or loss of a recent clock releases the video wait. Video decoding and
capture remain independent of audio availability.

## Evidence and limits

The regression suites exercise arbitrary native block sizes, capture gaps,
duplicate buffers, bounded backlog, codec input/packet validation, concealment,
timestamp drift, continuous rendering, SPSC ownership, authenticated relay
behavior, screen scheduling and session lifecycle. Synthetic timing tests use
virtual time so they can check exact deadlines without depending on scheduler
luck. A real local relay/decode test also checks the screen service's clock wiring.

Playback diagnostics use the `mouffette.audio.playback` logging category and
report concealment, underruns, rebuffers and retained duration at most once every
five seconds when recovery occurs. Transport summaries separately expose receive
and admission drops and the jitter target. These records contain counters, not
PCM or encoded audio. A worker failure or native capture failure retires the
publication before retrying, so new codec state and sequence numbers cannot be
mistaken for the previous stream.

These checks establish software behavior. They do not establish acoustic latency
on every physical sound device. Qt's callback API provides samples but does not
provide the native DAC presentation timestamp or Bluetooth transport latency.
The local output timestamp is therefore an estimate based on observed callback
periods and a continuous device frame counter. `setBufferSize()` does not control
the Qt callback mode; assuming a requested 20 ms buffer represented actual device
latency was incorrect. See the [Qt callback contract](https://doc.qt.io/qt-6/qaudiosink.html#callback-interface).

The dedicated audio sockets still use TCP and the application's network event
loop. TCP retransmission can delay later packets, and a sufficiently long GUI
stall can prevent new encoded data reaching the worker. Bounded queues and loss
concealment limit the damage; they cannot reconstruct a long missing signal.
The remaining architectural improvements for stronger guarantees are a media
transport independent of GUI scheduling, a standard realtime datagram media
stack with congestion control, and native output presentation timestamps.
These require coordinated client/server and native-device qualification;
they are not properties conferred by changing a buffer constant.

The reference behavior comes from [RTP's sequence/timestamp separation and
interarrival jitter model](https://www.rfc-editor.org/info/rfc3550/), the
[Opus decoder's PLC/FEC contract](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html),
and [Opus encoder lookahead controls](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoderctls.html).
The custom WebSocket framing is not presented as RTP or WebRTC compliance.

Physical qualification must use a common flash/beep source on two machines,
measure offset and discontinuities over a long session, and include macOS →
macOS, Windows → macOS, macOS → Windows and Windows → Windows. Repeat with
concurrent uploads, CPU/GPU load, Wi-Fi loss, output-device hotplug,
sleep/reconnect and mutual listening. Qualify wired/default speakers separately
from Bluetooth and external interfaces. Synthetic tests and a macOS build do
not certify Windows native capture or any particular device's acoustic sync.

## Validation performed on 2026-09-22

Environment: macOS 26.1, arm64, Qt 6.11.2, FFmpeg 8, development Debug build.

- Full CMake build succeeded; the application and focused tests were rebuilt
  again after the final playback, transport and presentation changes.
- `npm test` passed for the server. The audio relay suite passed again after
  the final RTT/bitrate-transition and stale-pipe changes.
- Final focused Qt results: AudioWorker 19 passed (one opt-in native capture
  test skipped), AudioCapturePipeline 14 passed, AudioTransport 14 passed,
  ScreenAudioClock 16 passed, ScreenSharingService 15 passed.
- A standalone renderer/SPSC harness passed AddressSanitizer,
  UndefinedBehaviorSanitizer and ThreadSanitizer, including a 20,000-packet
  producer/consumer stress run. This is focused sanitizer coverage, not a
  whole-application sanitizer claim.
- The complete client CTest run finished with 57/61 suites passing. The failures
  were MediaOverlayScaled, CanvasInteractionScaled, TimelineControllerScaled
  and CanvasSelectionBackend. The first three include native window geometry
  constrained by the available desktop at 2× scale; the last reported three
  window-activation timeouts. All three activation cases passed when rerun alone.
  Serial comparisons with untouched Release executables built at 02:53 before
  this task reproduced both selected MediaOverlay failures, five selected
  CanvasInteraction geometry failures and both selected Timeline failures under
  the same Qt/macOS/2× environment. Those executables embed their pre-change
  project code and QML. The UI assertions were not changed or disabled.

The native capture smoke test was not enabled. Windows compilation/runtime and
two-machine acoustic flash/beep measurements remain unverified in this session.
