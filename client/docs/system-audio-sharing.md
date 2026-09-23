# System audio sharing

In **Settings**, **Share my system audio** authorizes system audio publication
independently of **Share my screen**. Both permissions are off by default, belong
to the local runtime profile and apply only after a successful Save. Cancel
discards pending changes. No microphone is opened. One capture and one Opus encode
serve all listeners of an endpoint, including when no screen is shared or visible.
The relay forwards compressed sound without transcoding or recording it. Neither
PCM nor received packets enter project/media storage.

The top-bar **Play system audio / Stop system audio** button, next to **Show screen
content / Hide screen content**, controls remote listening. There is no separate
mute button inside the canvas. Stop unsubscribes and clears playback; Play requests
the remote audio stream again. Listening starts enabled and its preference is
stored in the viewer's runtime profile, independently of project, peer, screen
visibility and publishing consent. Hiding monitor pixels or panning all monitors
outside the viewport leaves sound playing. Leaving the canvas, hiding the viewer
window, sleep/lock or losing its active session stops listening and clears old
playback. Publishing can continue with its window hidden.

Existing profiles retain both previous choices. When the new audio consent key is
absent, the former combined **Share my screens and system audio** consent supplies
the initial audio permission. Subsequent saves persist screen and audio permissions
separately. The old remote mute preference supplies the inverse initial listening
choice, so a muted profile stays stopped. Saving settings or changing listening
persists the new listening key and removes the old mute key. Explicit saved audio
permissions and listening choices take precedence over these migration defaults.

## Independent availability and errors

The remote connection card shows a separate audio indicator alongside the screen
indicator. **Audio disabled** means listening was stopped locally. **Audio loading**
covers startup, and **Audio available** means remote capture is active, including
when the remote system is silent. Playback failures override that status, and
only actual playback progress supplies the audio/video synchronization clock.
**Audio not shared** means the remote user disabled system audio sharing.
**Audio error** covers capture, codec, playback and transport failures;
**Audio not available** covers unsupported or otherwise unavailable service.
Hovering the indicator exposes its detail. A silent system mix can still be
available; the status does not require audible content.

Audio errors and remote consent denial produce warning toasts and notification
history independently of screen warnings. Duplicate warnings are suppressed until
recovery or a new listening attempt. Intentional Stop and suspension are silent.
A missing audio channel times out instead of remaining loading indefinitely.
The publishing client's audio status in Settings carries local capture diagnostics.
Capture failure retains its error during retry backoff before rotating the audio
epoch and retrying.

Audio consent, subscription, transport and capture failures do not revoke screen
sharing, and screen failures do not revoke audio. The paths retain their shared
bandwidth accounting and bounded A/V timing observations when both are active.

## Application exclusion and scene playback

Screen and system audio capture exclude the entire Mouffette application,
including its control interface, dialogs, received-scene windows and every audio
output. macOS excludes the application from ScreenCaptureKit video and enables
`excludesCurrentProcessAudio` for system sound. Windows applies
`WDA_EXCLUDEFROMCAPTURE` to all Mouffette windows before showing them and uses
WASAPI process-tree audio exclusion. Native filter failures stop the affected
capture. Windows display affinity also affects other native capture tools.

The remote canvas combines the captured desktop with its own scene media above
it. Showing remote screen content does not hide these media, and listening to
remote system sound does not mute their playback. A received scene can therefore
play on the destination computer while its image and sound are rendered locally
in the controller's canvas, without entering the remote capture a second time.
The listening button controls only the other applications' remote audio.

Audio capture and playback run entirely in the Mouffette process. `AudioEngine`
publishes only the native system capture. Local playback has no capture role,
scene tap or reinjection bus; received scenes, canvas previews and incoming remote
monitoring all stay excluded. There is no helper executable, local socket or
second macOS recording authorization. Only Mouffette needs capture permission.
Local playback retains supported surround layouts and uses stereo until final
mono output routing, independently of the shared stream's stereo format.

Native samples retain timestamps in `MediaCaptureClock`'s epoch.
`AudioCaptureResampler` preserves contiguous PCM, filters small timestamp jitter,
and corrects sustained clock drift with FFmpeg's soft resampling compensation,
bounded to 1000 ppm. The first sample is anchored once on the common 48 kHz grid;
later callback timestamps cannot create sample holes or overlaps. Duplicate
buffers are rejected, partial overlaps are trimmed, and real gaps or restarts
begin a marked segment.

`AudioCaptureBuffer` preserves the 60 ms collection deadline before encoding
20 ms Opus packets. Raw native callbacks enter this single-source buffer without
waiting for another packet boundary. Wire timestamps still name the original
samples, including Opus lookahead compensation. Missing input and segment
transitions use a 5 ms fade; stateful soft clipping handles levels above unity.
A silent native capture publishes silence even when Mouffette is playing a scene.
Native capture must report success before publication begins. Failure or revoked
consent stops publication without an unfiltered fallback. Stopping or restarting
capture clears queued native samples, and a stalled consumer drops obsolete
history and resumes live.

macOS uses ScreenCaptureKit system audio (macOS 13+ API, subject to the application's
actual Qt/deployment minimum). Its minimal reference video output is discarded,
independent of viewer monitor demand. Windows uses native process loopback and
supports Windows 11 and Windows 10 2004+ with runtime activation checks; validation
on Windows 10 22H2 is required. No virtual audio driver is installed. Missing OS
permission or unavailable native support leaves audio unavailable rather than
capturing an unfiltered mix. Ordinary video operation remains independent.

Windows process loopback requests an explicit extensible 48 kHz stereo float
format with an FL/FR channel mask. Its asynchronous activation callback owns the
parameter blob until Windows releases the callback, including after a stop or
timeout. The callback is agile and supports free-threaded COM marshaling. The
MMDevice module remains loaded while late callbacks can still run.

Publisher logs now include `[AudioSharing] System audio capture failed:` with
the failing WASAPI stage, HRESULT, Windows version and native error text.
The viewer reports the audio failure separately from screen availability; the
publisher's diagnostics distinguish the failing process-loopback API, format,
permission or playback stage. Runtime activation
is authoritative for Windows 10 installations; the generic OS-version advice
is not substituted for the actual error.

## Network, timing and volume

Audio negotiates `audioVersion: 1` through protocol v12. Older servers/clients stay
video-only. Dedicated publish/view WS or WSS sockets reuse the control connection's
proxy policy and TLS verification and use single-use authenticated role tokens.
Session, connection, consent and audio epochs fence delayed work. Monitor topology
does not reset the audio epoch. See the [wire protocol](../../server/AUDIO_SHARING_PROTOCOL.md).

Opus explicitly retains two channels at both 96 and 32 kbit/s, using 48 kHz,
20 ms packets, complexity 7 and constrained VBR. The music signal policy prevents
automatic speech classification from collapsing spatial separation at low bitrate;
a stereo packet header alone is insufficient. DTX is disabled to preserve
quiet system-audio content. Decoder PLC and available in-band
FEC bridge short packet gaps; CELT music packets can use PLC without carrying FEC.
Capture sequences remain visible through local congestion and relay drops. The normal
96 kbit/s mode falls to 32 kbit/s after one second of congestion or a total budget
below 256 kbit/s; recovery requires ten healthy seconds at or above 384 kbit/s.
One common audio quality serves all viewers, so a weak viewer may lower audio
quality without lowering the other viewers' video quality. Audio reservations
are deducted from existing source/viewer/relay preview budgets. Real TCP/TLS
headers and retransmissions add traffic beyond application byte budgets.

Native capture timestamps use a common monotonic source clock. Continuous
per-source resampling preserves sample order while marking real discontinuities;
Opus lookahead is subtracted so packet timestamps name the decoded first sample.
On macOS this is the CoreMedia host clock's
`mach_absolute_time` epoch, including after sleep; native capture timestamps are
never reanchored to a delayed first callback. Windows uses QPC directly, matching
WASAPI's native sample timestamps. Network timeouts continue using local arrival
clocks.

The fastest observed audio/video transit establishes the source-to-viewer clock
mapping. With device callbacks up to 20 ms, audio's jitter allowance starts at
120 ms, adapts within 80–150 ms, and immediately increases when needed to retain
60 ms of reception headroom,
within that ceiling. Healthy conditions lower the target slowly. This includes
the publisher's collection and codec delay when video supplies faster clock
observations. The audio engine receives an absolute local playback deadline and
discards expired packets. Source
and listener transports also enforce packet age limits, including data buffered
inside the operating system's sockets. Audio acknowledgement windows account for network
RTT separately from bytes waiting in the socket; healthy propagation delay does
not itself discard packets. A sustained route change retires the disposable audio
epoch before learning a fresh mapping. A delayed burst cannot reanchor itself.

Playback uses fixed SPSC PCM storage and a continuous sample cursor. Startup and
recovery normally require 40 ms of contiguous audio at the live cursor;
insufficient data remains silent rather than producing isolated bursts. A short fade joins starts,
dropouts and changed deadlines without postponing the assigned live deadline.
The native audio callback takes no mixer mutex and does not allocate or release
stream objects. Interpolation spans packet boundaries and clock drift changes
playback rate gradually instead of jumping sample position at every packet.

Larger device callbacks need a larger reservoir. The measured callback duration
`q` (bounded to 100 ms) raises recovery to `max(40 ms, q)` and required headroom
to `q + recovery + 20 ms`. The excess over the ordinary 60 ms headroom is added
to the initial/minimum/maximum playout targets and the paired video wait budget.
For a 100 ms callback, the maximum becomes 310 ms. This device allowance is shared
across the audio renderer, transport and video clock; it does not relax the
independent 150 ms source-freshness check or reanchor old socket data.

Playback retains canonical stereo until the final device write. The output
selector tries supported stereo formats across common sample rates before a
device's preferred mono format. Known FL/FR speaker positions receive their
respective channels on multichannel devices; a genuinely mono-only output is
downmixed locally, without changing the captured or transmitted stereo stream.

Video may wait up to 150 ms plus the measured device allowance for the paired
source/local audio output estimate. A bounded queue holds each image at its own
timestamp and skips images already superseded at presentation time. Network receipt does not become
the audio clock's origin. Without a recent audio clock, video presents immediately.
Qt's public callback API does not expose the DAC timestamp or Bluetooth latency;
the output clock is an estimate based on callback periods and device frame count.
`setBufferSize()` does not control callback mode. This is bounded preview
synchronization, not a guarantee of acoustic synchronization on every device.
See the [reliability audit](audio-reliability-audit.md) for causes, regression
coverage and remaining transport/device limits.

The capture backend forwards the system mix at its native level. It does not
read or reapply the sharing computer's speaker volume or mute. The viewer's
own speaker/device volume and the top-bar listening button control listening. This
does not normalize media or undo individual applications' own mix levels.

## Validation

Automated checks cover codec/framing, timing bounds, application audio exclusion,
native-window policy/lifecycle, relay authorization and congestion, independent
sharing/listening preferences and their migration, and existing media/scene/video
behavior. Relay regressions cover audio without screen consent or displays,
independent consent revocation, capture-error isolation and recovery, and listener
capacity status. Tests use synthetic audio
and do not implicitly request screen-recording permission.

`AudioEngine` checks native audio buffering, silence, bounded queues and backlog
recovery, capture permission/failure/restart, stereo publication at both bitrates,
and the real resident-decoder/output path. Distinct synthetic frequencies verify
that received scenes, canvas previews and remote monitoring are absent while
native system audio remains present. Live media volume, mute, pause, replay and
monitoring toggles cannot affect publication. Headless tests retain buffer,
stereo and capture lifecycle coverage; tests needing an output device skip
explicitly.

`AudioCaptureStability` exercises variable native block sizes, timestamp jitter,
positive and negative 200 ppm clock drift, absolute sample-grid alignment,
duplicates, partial overlaps, discontinuities and malformed float samples.
Distinct left/right tones and waveform-boundary assertions detect mono collapse,
zero holes and discontinuous sample jumps. These synthetic checks complement
native device tests; they do not establish Windows runtime behavior.

`WindowsAudioActivation` exercises late completion after cancellation, agile
marshaling, HRESULT propagation and a malformed successful activation without
opening an audio device. Native capture is an explicit opt-in in
`AudioEngine::applicationPlaybackIsExcluded:native-capture-opt-in` (`MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE=1`).

Native release qualification must additionally exercise two physical computers:
an animated desktop behind control windows and received scenes (both excluded),
three monitors,
mutual listening without echo, system volume/mute and balance, device hotplug,
permission denial, sleep/reconnect, concurrent upload and weak-link recovery.
Changing the publisher's output-device volume or mute must not change the shared
mix; changing an application's own level still does. The viewer's **Stop system
audio** button must stop only remote monitoring. Exercise all four combinations
of publishing consent and all four combinations of viewing/listening choices.
Deny or fail each capture independently and verify that the other medium keeps
working, with its own availability indicator and warning history.
Use a flash/bip source and measure A/V offset over a long session; target less than
100 ms on a stable link. Qualify macOS, Windows 10 22H2 and Windows 11 separately.
Platform behavior and acoustic synchronization cannot be certified by synthetic
tests or by a build on another OS.

Builds require the `opus` pkg-config package alongside FFmpeg. Packaging verifies
the Opus dynamic library is present and includes its redistribution notice.
The audio engine is linked into Mouffette; packaging contains no audio helper.

Native API references: [Windows capture exclusion](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity),
[Windows process-tree audio exclusion](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ne-audioclientactivationparams-process_loopback_mode),
[OBS process audio compatibility](https://obsproject.com/kb/application-audio-capture-guide),
[ScreenCaptureKit process audio exclusion](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/excludescurrentprocessaudio).

### Native routing regression probe

With native recording permission granted, run
`tst_AudioEngine applicationPlaybackIsExcluded:native-capture-opt-in` with
`MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE=1` (and `QT_QPA_PLATFORM=cocoa` on macOS).
On macOS, the probe skips unless recording access is already granted; it never
requests permission.
This briefly plays quiet tones through actual received-scene, canvas and monitoring
outputs in the same process. It checks that all three frequencies are absent
from the outgoing Opus stream, including after changes to volume, mute, playback
and monitoring. It logs amplitudes, never records system audio to disk.
A second application playing sound is still required for native end-to-end
qualification of the other-application capture path.
