# Remote audio reliability audit

This audit concerns live system audio, received-scene inclusion and remote screen
preview. Capture and playback share the same processing policy on macOS and
Windows; their native device paths still need separate qualification. Screen
capture permissions and the exclusion of control-preview audio remain unchanged.

## Defects found and corrected

| Boundary | Defect | Correction |
| --- | --- | --- |
| Stereo routing | A device's preferred mono format could collapse the scene before capture. | Keep at least stereo through scene mixing and tap before mono averaging; retain native surround locally, downmix it separately for capture, and route monitoring as stereo. |
| Stereo encoding | Automatic Opus classification with FEC could collapse SILK/hybrid stereo width at 32 kbit/s even with a two-channel packet header. Four seconds of silence or mono noise followed by quiet 650/1350 Hz stereo reproduced the half-volume mono sum. | Set both forced two-channel encoding and the public music signal policy. Regression tests measure actual channel separation after both histories at 32/96 kbit/s, not just the packet header. |
| Capture → mixer | Tolerated timestamp jitter still positioned blocks independently in the absolute mixer, creating zero holes or overlap overwrites in continuous sound. | One stateful `AudioCaptureResampler` per native source/scene tap maintains contiguous PCM and uses bounded FFmpeg soft compensation for drift. Its epoch is quantized once onto the shared sample grid. |
| Capture discontinuities | Missing blocks, native overlap and restarted capture could splice unrelated samples; hard clipping distorted the combined system/scene peak. | Reject duplicates, trim repeated prefixes and mark real gaps/restarts. Apply 5 ms transition fades and stateful soft clipping after the buses are summed. |
| Native timestamps | A missing WASAPI/SCK timestamp became callback arrival time, including the duration of the already captured block. | Track the expected first sample, and use arrival minus block duration only when the native timeline cannot be continued. |
| Capture scheduling | Native audio work could compete at normal priority during UI/GPU load. Unsupported SCK formats could leave an apparently running silent capture. | macOS audio queue QoS and Windows MMCSS Audio scheduling; fail explicitly on unsupported native formats. |
| Codec | System audio used voice inactivity suppression; loss recovery and codec lookahead compensation were missing. Invalid float input could poison codec state. | Continuous Opus encoding, finite normalized input, checked initialization, PLC/FEC support, and timestamps corrected using Opus-reported lookahead. |
| Publication → relay | Renumbering successfully forwarded packets concealed earlier losses. | Preserve capture sequence gaps end to end while retaining separate authenticated stream epochs. |
| TCP accounting | Unacknowledged bytes, which include healthy network round-trip time, were treated as unsent audio. A fixed acknowledgement deadline retired healthy high-RTT sockets. | Separate bounded socket backlog from RTT-based receipt credit; keep freshness checks independent of acknowledgements. |
| Startup/recovery | An 80 ms initial target left little playback headroom after source collection and codec delay. A single packet could start before a stable reservoir existed. | For ordinary device quanta, start at 120 ms, adapt within 80–150 ms, immediately reserve 60 ms of reception headroom, and require 40 ms of contiguous PCM. Fade gaps and changed deadlines. |
| Device callback duration | A fixed reservoir and deadline could leave 40–100 ms hardware callbacks permanently starved, even when the same network path worked with 10 ms callbacks. | Measure the full device quantum and share its bounded extra budget between renderer, transport and video clock; preserve source-freshness checks. |
| Stale transport | A permanent timing shift could reject every packet indefinitely. | Retire stale pipes with bounded retries and an explicit unavailable state; rotate publication epochs after capture failures. |
| Audio callback | A contended shared mixer mutex silenced a complete callback. Mutable packet mapping could jump the playback position. | Fixed SPSC PCM storage, immutable mixer snapshots retired off the audio thread, and a continuous render cursor with bounded rate correction. |
| Resampling | Per-packet interpolation and timestamp changes could repeat/drop samples at packet boundaries. | Maintain contiguous sample positions independently from native source timestamps, with interpolation across packet boundaries. |
| A/V clock | The callback tail was reported without its corresponding local presentation time. | Carry a paired source/local output estimate through the engine, service and screen scheduler. Reject stale reports without reanchoring them. |
| Video presentation | Every new image replaced the pending image but inherited its older deadline. The 40 ms wait ceiling was less than the audio buffering target. | Bounded timestamp-ordered presentation queue: display the newest due image, preserve future images' own timestamps, and bound waiting by the same audio/device policy. |

The implementation stays within the negotiated audio v1 wire format. New clients
retain interoperability with older peers, but both publisher and relay need the
updated sequence behavior for all losses to remain visible. The audio engine runs
inside the sole Mouffette process; there is no audio worker or local audio IPC.
Native capture excludes this process, and only rendered received-scene samples
are added back. Canvas and incoming monitoring audio remain excluded.

## Timing and resource policy

The source uses one native monotonic epoch for audio and video. The receiver
maps this epoch onto its own media clock using transit observations. Packet
arrival does not directly reposition the sample cursor. Actual PCM storage and
socket queues remain bounded; receipt credit is a separate accounting window,
not a queue of audio waiting for playback.

Each source clock is corrected independently with a stateful FFmpeg resampler.
Small timestamp errors are filtered; compensation changes the sample rate by at
most 1000 ppm instead of inserting zeros or dropping individual samples. Native
blocks do not wait for packetization before entering the fixed 60 ms collection
window. Both capture buses use a 5 ms transition envelope, and the final combined
level uses stateful soft clipping. The codec state remains continuous within its
publication epoch, including after a local backlog drop.

For callbacks up to 20 ms, the transport target starts at 120 ms and adapts within
80–150 ms. It immediately increases to preserve 60 ms of arrival-to-playout headroom, subject
to that ceiling; recovery toward lower latency requires sustained healthy
conditions. The receiver starts or resumes only with 40 ms of contiguous PCM at
the live cursor. Waiting for that reservoir does not move an expired deadline:
missing data produces a faded interruption, not a growing playback backlog.

For a measured device quantum `q > 20 ms`, bounded to 100 ms, recovery becomes
`max(40 ms, q)` and required headroom becomes `q + recovery + 20 ms`. The excess
over 60 ms is added to all playout target bounds and the screen wait ceiling;
the maximum is 310 ms at a 100 ms quantum. The renderer receives the complete
hardware quantum even when the mixer processes smaller internal chunks. Changes
apply immediately and the device measurement survives an audio-policy reset.
The same budget reaches the audio timeline and screen clock, avoiding conflicting
deadlines. This allows for coarse hardware delivery without weakening the
separate 150 ms source-freshness limit or treating socket backlog as live data.

Recovering a stale socket flushes its old data before establishing a
new audio epoch. When recent video supplies faster clock evidence, that evidence
survives the audio reconnect. An audio-only path permanently more than 150 ms
behind video cannot satisfy this policy; it reports `timing_unavailable` and
retries with bounded backoff instead of silently replaying late sound or claiming
that audio is available. Sustained delivery that is fresh but leaves insufficient
room for the device quantum and recovery reservoir also triggers this timing
failure after one second of source and arrival progress. Temporary jitter and
a single queued burst do not trigger it. Queue recovery does not make such
physical delay vanish.

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
duplicate/overlapping buffers, bounded backlog, codec input/packet validation,
concealment, stereo channel separation, timestamp jitter and ±200 ppm drift,
continuous waveforms, startup reservoirs, SPSC ownership, authenticated relay
behavior, screen scheduling and session lifecycle. Synthetic timing tests use
virtual time so they can check exact deadlines without depending on scheduler
luck. A real local relay/decode test also checks the screen service's clock wiring.

Playback diagnostics use the opt-in `mouffette.audio.engine` logging category
and report concealment, underruns, recoveries, retained duration and measured
output quantum at most twice per second when recovery occurs. Enable them with
`QT_LOGGING_RULES="mouffette.audio.engine.debug=true"`. Transport summaries separately expose receive
and admission drops and the jitter target. These records contain counters, not
PCM or encoded audio. Relay congestion feedback subtracts the known extra
hardware reservoir, so a slow output alone does not trigger a bitrate downgrade.
Local diagnostics retain the actual queue depth. A native capture failure retires the publication before
retrying, so new codec state and sequence numbers cannot be
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
stall can prevent encoding or publication. Bounded queues, fades and loss
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
The stereo policy follows the public [Opus signal controls](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoderctls.html),
with the regression grounded in the [encoder mode selection](https://github.com/xiph/opus/blob/v1.5.2/src/opus_encoder.c)
and [SILK stereo-width adaptation](https://github.com/xiph/opus/blob/v1.5.2/silk/stereo_LR_to_MS.c).
Capture-clock compensation uses the public
[FFmpeg soft compensation API](https://www.ffmpeg.org/doxygen/trunk/group__lswr.html).
The custom WebSocket framing is not presented as RTP or WebRTC compliance.

Physical qualification must use a common flash/beep source on two machines,
measure offset and discontinuities over a long session, and include macOS →
macOS, Windows → macOS, macOS → Windows and Windows → Windows. Repeat with
concurrent uploads, CPU/GPU load, Wi-Fi loss, output-device hotplug,
sleep/reconnect and mutual listening. Qualify wired/default speakers separately
from Bluetooth and external interfaces. Synthetic tests and a macOS build do
not certify Windows native capture or any particular device's acoustic sync.

## Validation performed on 2026-09-22

Historical results below precede the current stereo and startup changes and are
not validation of this revision. Some suite names refer to the retired worker.

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


## Stereo and startup revision — 2026-09-23

The macOS Debug client builds successfully with Qt 6.11.2, FFmpeg 8.0 and
libopus 1.5.2. Eleven focused Qt suites were validated during this revision:
AudioEngine, AudioStereo, AudioOutputRouting, AudioCaptureStability,
AudioPlaybackStability, AudioSharingContinuity, AudioCapturePipeline,
AudioTransport, ScreenAudioClock, ResidentMedia and ScreenSharingService.

The final native macOS AudioEngine run passed all 31 cases with no skips,
including native capture at both 32/96 kbit/s, distinct left/right scene tones,
preview/monitor exclusion, live mute/volume, restart and 5.1/7.1 tap conversion.
For native 650/1350 Hz tones at amplitude 0.03, the final 32 kbit/s run measured
approximately 0.02963/0.03050 in the intended channels and 0.00012/0.00013 in the
opposite channels. The quiet-history codec regression failed deterministically
before the music policy and passes after it; AudioStereo now has 15 passing cases.

Early native runs also exposed unrelated energy already present in the native
input at 400 Hz. The test now observes source spectra before mixing/encoding and
uses a separate 650/1350 Hz pair for native scene qualification. This separated
native-input interference from the independently reproduced Opus mono defect;
no application was stopped or muted to make the tests pass. These native checks
assume that other system audio does not dominate their test frequencies.

The 30-case playback suite also passed separate AddressSanitizer plus
UndefinedBehaviorSanitizer and ThreadSanitizer builds, including the 10,000-packet
concurrent producer/discard/consumer test, with no diagnostics. Instrumentation
covers the playback implementation and tests; the linked Qt libraries themselves
were not rebuilt with sanitizers. Simulated device callbacks include 40/80/100 ms,
44.1/48 kHz output, size changes, source jitter, gaps and clock drift. The complete
capture → mix → Opus → render suite covers both bitrates and 44.1/48/96 kHz output.

Windows native format/channel mapping and the shared processing code were
reviewed, and the portable regressions are registered for the existing Windows
CTest build. No Windows host was available: Windows compilation, native WASAPI
execution and a physical Windows↔macOS two-client listening test are not claimed.
Both endpoints need the updated client to benefit from both source and receiver
fixes. No additional Mouffette process or network protocol change is introduced.
