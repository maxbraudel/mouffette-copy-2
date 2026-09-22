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

## Control interface and received scenes

Native screen capture excludes the Mouffette control interface and dialogs, but
keeps explicitly registered received-scene windows. macOS uses an application
exclusion with scene-window exceptions; Windows applies WDA_EXCLUDEFROMCAPTURE
before showing control windows. Native filter failures stop the affected capture.
Windows display affinity also affects other tools using native capture APIs.

Audio uses the same distinction. `ReceivedScene` readers retain their normal
output in the main process. `ControlPreview` readers decode the existing resident
asset and share bounded PCM blocks with an audio worker. Received remote sound
also plays in that worker. The worker captures system audio while excluding its
own audio, so editor previews and remote monitoring never feed another stream.
Received scenes remain included.
The scene controller does not depend on the worker for playback or teardown.

On macOS the worker is a nested `MouffetteAudioWorker.app` with its own bundle
identifier (`<main bundle identifier>.audio-worker`), launched through macOS
LaunchServices so it has its own responsible application identity. A direct child
process still inherits the parent identity despite the distinct bundle.
ScreenCaptureKit audio exclusion is application-wide, so launching the same main
executable in a second process can also exclude received scenes. The separate
bundle and independent launch are required; a missing helper never falls back to
the main application. On Windows the worker
uses the packaged executable's internal mode and excludes only its own process
tree, preserving received-scene output in the parent process. Both paths start
before normal instance coordination, UI and persistence bootstrap. macOS may
require a separate recording authorization for **Mouffette Audio** under
**Privacy & Security > Screen & System Audio Recording**; denial remains an
explicit audio error and never enables unfiltered capture. A user-local socket
authenticates its parent connection; playback buffers are bounded shared memory.
Loss of the parent ends the worker. Loss of the worker never falls back to playing
control audio in the main process, which would violate the exclusion.

Preview shared memory has an immutable magic/version/logical-size header followed
by lock-free PCM blocks. This is a private ABI between the application and worker from the same
build; layout or semantic changes require a version increment. The worker
checks mapped capacity **at least** equal to the expected state size before
reading the header, then requires an exact ABI match. Allocation size is not the
logical format size: [Qt permits larger segments](https://doc.qt.io/qt-6/qsharedmemory.html#size),
and Windows reports page-rounded capacity. No page-size assumption belongs in
the production validation.

Every preview registration receives a keyed acceptance or error response, even
without an output device. The parent times out unanswered registrations after
five seconds (checked once per second), resets attachment state on worker exit,
and replays live channels on restart. Preparation waits for attachment confirmation;
device availability remains separate so headless video still works. Rejected
channels produce a playback diagnostic and fail the affected media cursor without
resetting remote listening. No additional work is added to the audio callback.

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
the failing WASAPI stage, HRESULT, Windows version and native error text. A
worker exit also reports its exit code and whether QProcess observed a crash.
The viewer reports the audio failure separately from screen availability; the
publisher's diagnostics distinguish the failing process-loopback API, format,
permission or worker stage. Runtime activation
is authoritative for Windows 10 installations; the generic OS-version advice
is not substituted for the actual error.

## Network, timing and volume

Audio negotiates `audioVersion: 1` through protocol v12. Older servers/clients stay
video-only. Dedicated publish/view WS or WSS sockets reuse the control connection's
proxy policy and TLS verification and use single-use authenticated role tokens.
Session, connection, consent and audio epochs fence delayed work. Monitor topology
does not reset the audio epoch. See the [wire protocol](../../server/AUDIO_SHARING_PROTOCOL.md).

Opus uses 48 kHz stereo, 20 ms packets, complexity 7 and constrained VBR, with DTX
disabled to preserve quiet system-audio content. Decoder PLC and available in-band
FEC bridge short packet gaps; CELT music packets can use PLC without carrying FEC.
Capture sequences remain visible through IPC and relay drops. The normal
96 kbit/s mode falls to 32 kbit/s after one second of congestion or a total budget
below 256 kbit/s; recovery requires ten healthy seconds at or above 384 kbit/s.
One common audio quality serves all viewers, so a weak viewer may lower audio
quality without lowering the other viewers' video quality. Audio reservations
are deducted from existing source/viewer/relay preview budgets. Real TCP/TLS
headers and retransmissions add traffic beyond application byte budgets.

Native capture timestamps use a common monotonic source clock. Sample-aware
packetization preserves partial native buffers and marks capture discontinuities;
Opus lookahead is subtracted so packet timestamps name the decoded first sample.
On macOS this is the CoreMedia host clock's
`mach_absolute_time` epoch, including after sleep; native capture timestamps are
never reanchored to a delayed first callback. Windows uses QPC directly, matching
WASAPI's native sample timestamps. Network timeouts continue using local arrival
clocks.

The fastest observed audio/video transit establishes the source-to-viewer clock
mapping. Audio's jitter allowance starts at 80 ms and can adapt up to 150 ms
under sustained timing pressure, from that shared mapping. The helper
receives an absolute local playback deadline and discards expired packets. Source
and listener IPC also enforce packet age limits, including data buffered inside
the operating system's sockets. Audio acknowledgement windows account for network
RTT separately from bytes waiting in the socket; healthy propagation delay does
not itself discard packets. A sustained route change retires the disposable audio
epoch before learning a fresh mapping. A delayed burst cannot reanchor itself.

Playback uses fixed SPSC PCM storage and a continuous sample cursor. The native
audio callback takes no mixer mutex and does not allocate or release stream
objects. Interpolation spans packet boundaries and clock drift changes playback
rate gradually instead of jumping sample position at every packet.

Video may wait at most an additional 150 ms for the paired source/local audio
output estimate. A bounded queue holds each image at its own timestamp and skips
images already superseded at presentation time. IPC receipt does not become
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

Automated checks cover codec/framing, timing bounds, shared PCM generations,
native-window policy/lifecycle, relay authorization and congestion, independent
sharing/listening preferences and their migration, and existing media/scene/video
behavior. Relay regressions cover audio without screen consent or displays,
independent consent revocation, capture-error isolation and recovery, and listener
capacity status. Tests use synthetic audio
and do not implicitly request screen-recording permission.

`AudioWorker` exercises exact and padded allocations through the real helper on
every OS, without skipping registration checks on headless CI. It also rejects
truncated, missing and incompatible segments, verifies keyed client error
propagation, and checks local PCM consumption during remote playback and mute
when an output device is available. Windows release checks must include a local
canvas video while listening to a remote client, then Stop/Play system audio:
the local video must remain audible in both states.

`WindowsAudioActivation` exercises late completion after cancellation, agile
marshaling, HRESULT propagation and a malformed successful activation without
opening an audio device. Native capture is an explicit opt-in in
`AudioWorker::nativeCaptureSmokeOptIn` (`MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE=1`).

Native release qualification must additionally exercise two physical computers:
an animated desktop behind control windows, received scenes, three monitors,
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
The audio worker reuses the application executable, so no separate binary needs
to be installed or located after moving an application bundle.

Native API references: [Windows capture exclusion](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity),
[OBS process audio compatibility](https://obsproject.com/kb/application-audio-capture-guide),
[ScreenCaptureKit process audio exclusion](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/excludescurrentprocessaudio).

### Native routing regression probe

`AudioWorker::workerApplicationIdentityIsSeparate` checks the macOS helper bundle
contract. CTest supplies the production helper to the worker tests.

With native recording permission already granted, run
`tst_AudioWorker nativeCaptureIncludesSceneAndExcludesMonitoringOptIn` with
`MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE=1` and
`MOUFFETTE_TEST_AUDIO_WORKER_EXECUTABLE` pointing to the production helper. This
briefly plays quiet test tones and verifies the captured Opus contains the main
process's scene tone but excludes both the preview and remote monitoring tones.
It reports only measured amplitudes, never records system audio to disk. The
ordinary silent smoke test checks transport/timing, not this routing distinction.
