# System audio sharing

The existing **Share my screens and system audio** setting authorizes screen
and system audio publication; it remains off by default and retains saved consent.
No microphone is opened. One capture and one Opus encode serve all monitors and
viewers of an endpoint. The relay forwards compressed sound without transcoding
or recording it. Neither PCM nor received packets enter project/media storage.

The canvas toolbar's speaker button controls only remote listening. Listening
starts enabled and its mute preference is stored in the viewer's runtime profile,
independently of project, peer and screen visibility. Hiding monitor pixels or
panning all monitors outside the viewport leaves sound playing. Leaving the
canvas, hiding the viewer window, sleep/lock or losing its active session stops
listening and clears old playback. Publishing can continue with its window hidden.

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
own process, so editor previews and remote monitoring never feed another stream.
The scene controller does not depend on the worker for playback or teardown.

The worker is launched from the packaged executable with an internal mode before
normal instance coordination, UI and persistence bootstrap. A user-local socket
authenticates its parent connection; playback buffers are bounded shared memory.
Loss of the parent ends the worker. Loss of the worker never falls back to playing
control audio in the main process, which would violate the exclusion.

macOS uses ScreenCaptureKit system audio (macOS 13+ API, subject to the application's
actual Qt/deployment minimum). Its minimal reference video output is discarded,
independent of viewer monitor demand. Windows uses native process loopback and
supports Windows 11 and Windows 10 2004+ with runtime activation checks; validation
on Windows 10 22H2 is required. No virtual audio driver is installed. Missing OS
permission or unavailable native support leaves audio unavailable rather than
capturing an unfiltered mix. Ordinary video operation remains independent.

## Network, timing and volume

Audio negotiates `audioVersion: 1` through protocol v12. Older servers/clients stay
video-only. Dedicated publish/view WS or WSS sockets reuse the control connection's
proxy policy and TLS verification and use single-use authenticated role tokens.
Session, connection, consent and audio epochs fence delayed work. Monitor topology
does not reset the audio epoch. See the [wire protocol](../../server/AUDIO_SHARING_PROTOCOL.md).

Opus uses 48 kHz stereo, 20 ms packets, complexity 5 and constrained VBR. The normal
96 kbit/s mode falls to 32 kbit/s after one second of congestion or a total budget
below 256 kbit/s; recovery requires ten healthy seconds at or above 384 kbit/s.
One common audio quality serves all viewers, so a weak viewer may lower audio
quality without lowering the other viewers' video quality. Audio reservations
are deducted from existing source/viewer/relay preview budgets. Real TCP/TLS
headers and retransmissions add traffic beyond application byte budgets.

Native capture timestamps are mapped to a common monotonic source clock and kept
through the codec and relay. Network timeouts continue using local arrival clocks.
Audio starts with an 80 ms jitter allowance and drops stale queued sound instead
of accumulating delay. Video may wait at most an additional 40 ms for the current
audio consumption clock, with only one decoded image waiting per screen. Without
an active recent audio clock, video presents immediately. This is bounded preview
synchronization, not a promise of perfect synchronization on a congested link.

The capture backend applies the default output device's exposed digital gain and
mute once. The receiver adds no remote-volume attenuation; its own speaker/device
volume still applies normally. The displayed percentage remains a UI reading,
not an assumed linear multiplier. Multiple physical outputs, fixed-volume HDMI
devices and external amplifiers can prevent exact reconstruction of acoustic level.

## Validation

Automated checks cover codec/framing, timing bounds, shared PCM generations,
native-window policy/lifecycle, relay authorization and congestion, toolbar
preferences, and existing media/scene/video behavior. Tests use synthetic audio
and do not implicitly request screen-recording permission.

Native release qualification must additionally exercise two physical computers:
an animated desktop behind control windows, received scenes, three monitors,
mutual listening without echo, system volume/mute and balance, device hotplug,
permission denial, sleep/reconnect, concurrent upload and weak-link recovery.
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
