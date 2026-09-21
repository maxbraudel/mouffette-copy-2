# Live screen sharing

The connected client's desktop appears inside each existing canvas screen,
under the editable scene media. Pan, zoom, monitor layout and the existing
remote cursor keep their normal canvas coordinates. Screen pixels are ephemeral:
they are never saved in a project, recorded, or written to the media cache.

## Consent and lifecycle

In **Settings**, enable **Share my screens with connected clients** and save.
The setting is off by default and belongs to the local runtime profile. Cancel
does not change it. It authorizes all screens for owners of an authenticated
active remote session. It does not enable audio capture or remote input.

The top-bar **Hide screen content / Show screen content** button controls the
viewer's desktop preview. Viewing is on by default. The choice is saved in the
viewer's runtime profile and applies to all projects and remote clients. The
primary instance remembers it across restarts; temporary instances keep it for
their session. Hiding immediately clears the screen frames and unsubscribes from the
stream; showing subscribes again to the visible canvas. Monitor outlines, scene
media and the remote cursor remain available. This preference does not change
either client's permission to share its own screens.

The remote connection card shows **Screen disabled** when the viewer turns off
screen content locally. With viewing enabled, it shows **Screen available /
Screen not available**, with a monitor icon immediately to the left of the
volume. Availability follows decoded frames for the currently viewed client,
not just publishing consent. Waiting or losing the stream makes it unavailable.
There is no permanent status message inside the canvas screens.

Screen-sharing problems use warning toasts and notification history: remote
sharing disabled, missing OS permission, capture or decode errors, interrupted
video transport, no first frame after ten seconds, or no updates for five
seconds. Normal handshakes and successful recovery are silent. Repeated reports
of the same problem are suppressed until a frame arrives or the user starts a
new viewing attempt. Intentional hiding and suspension do not create warnings.

macOS also requires the operating system's Screen Recording permission for the
application. The checkbox only authorizes sharing within Mouffette; it does not
grant this separate OS permission. Permission denial has its own warning toast
on the viewing client. The publishing instance's Settings show
the detailed local error and permission recovery advice.

To recover from a macOS permission error:

1. Open **System Settings → Privacy & Security → Screen & System Audio
   Recording** (called **Screen Recording** on some macOS versions).
2. Allow the application named in the actual permission request: normally
   Mouffette; a development process can instead be attributed to Terminal or to
   an IDE such as **Visual Studio Code** when using its integrated terminal.
   Match the reported identity instead of assuming that an existing enabled
   Mouffette entry belongs to the current build. In particular, a request
   attributed to Visual Studio Code needs that app's permission.
3. Quit every running Mouffette instance, including instances whose windows are
   hidden, then reopen them. When testing two instances, relaunch both after the
   OS grant. Reopen the remote canvas to retry the subscription.
4. If it still fails, read the publishing instance's Settings status for the
   native error. Changing the sharing checkbox cannot fix an OS denial or an
   encoder failure.

The development launcher currently executes the bundle's binary directly and
the debug executable is linker-signed ad hoc. Its signing requirement changes
when the binary is rebuilt, so macOS may require permission again even when the
bundle name is unchanged. A stable Apple Development or Developer ID signing
identity is needed to preserve authorization across builds; an ad-hoc re-sign
does not provide that continuity. This behavior is confirmed by
[Apple Developer Technical Support](https://developer.apple.com/forums/thread/819406).

Capture continues when the publisher's control window is hidden; native system
lock/sleep suspends sharing.

Only the visible canvas with screen content enabled subscribes. Leaving it,
hiding screen content, hiding the viewer window,
disabling consent, losing the session, or disconnecting removes its frames.
Capture is created only while a permitted subscriber exists. Multiple viewers
reuse the same capture and encoder for each screen. A static desktop supplies
a low-rate freshness frame; the viewer clears an image after five seconds
without decoded frames.

## Capture and video path

- macOS uses ScreenCaptureKit with the native display ID, avoiding ambiguous
  monitor names. It captures SDR NV12, scales at capture time to a maximum
  1,920-pixel edge, requests 30 fps, and disables cursor/audio capture. Its
  IOSurface-backed CVPixelBuffer goes directly to VideoToolbox, without a CPU
  pixel copy or color conversion. The native capture queue has three surfaces.
- Windows uses Qt's FFmpeg-backed QScreenCapture and DXGI Desktop Duplication.
  Screen IDs come from the same native monitor inventory used for discovery
  and cursor mapping. Portrait rotation and mirroring are normalized.
- H.264 prefers VideoToolbox on macOS and NVENC, Quick Sync or AMF on Windows.
  If a hardware encoder cannot initialize, libx264 or OpenH264 is selected.
  The nominal bitrate is 4 Mbps per screen with no B frames and periodic
  self-contained IDR frames. Output dimensions are even and aspect preserving.
- Capture keeps one latest raw frame and at most one encoded callback pending.
  Conversion and encoding run on a dedicated worker per captured screen.
  Changed content is capped at 30 fps; unchanged content is refreshed at 1 fps.
  An asynchronous encoder awaiting a recovery IDR receives follow-up samples
  at 30 fps even on a static desktop. More than three pending native frames
  triggers a software encoder restart at an IDR instead of growing the queue.
- Decoding uses a two-worker pool, with at most three waiting packets and
  4 MiB per screen. A gap or overload abandons dependent P frames and requests
  an IDR. Stopped streams discard results from in-flight work.
- Decoded FFmpeg YUV planes are retained directly by QVideoFrame and rendered
  through the existing SharedVideoNode/Qt Quick graphics path. Frame delivery
  does not rebuild the screen model or invalidate document/project state.

The cap is appropriate for a desktop preview in a zoomable canvas. It is not a
promise of 30 fps on every GPU or network, nor a pixel-perfect 4K/HDR stream.

## Transport

Screen video uses its own bidirectional WebSocket, separately from control and
file uploads. The authenticated control connection issues a single-use token,
valid for 15 seconds and bound to that transport. The video socket inherits the
server URL's ws/wss scheme. With wss, TLS protects each client/server connection;
the trusted relay can access the compressed video. This is not end-to-end
encryption or a peer-to-peer WebRTC connection.

Each binary message contains `MSV1`, a big-endian 16-bit JSON header length,
a header of at most 1,024 bytes, and one H.264 Annex B access unit of at most
2 MiB. The header binds `remoteSessionId`, `generation`, server-issued
`streamId`, `screenId`, `sequence`, `width`, `height`, `keyFrame` and `codec`.
No image base64, JPEG polling, temporary video file or per-frame control message
is involved.

The relay checks the authenticated publisher role, current connections, session
lease, opt-in, viewer subscription, monitor inventory and stream generation.
Revocation, channel replacement and changed screen topology issue a new stream
identity. This prevents frames already on another TCP connection, or already
being decoded, from reappearing after an old stream is stopped.

Both video legs use receipt acknowledgements on the video socket itself.
Each socket admits at most three unacknowledged access units and 512 KiB,
including bytes already copied into the OS network buffers. A standalone
keyframe may use the 2 MiB packet allowance when the window is empty. Only an
exact stream/screen/sequence acknowledgement releases credit. After 1.5 seconds
without a receipt, the disposable video connection is aborted and replaced.
Backlogs are bounded, and congestion drops dependent frames until a fresh IDR.
Freed slots rotate among waiting display streams so a fourth monitor cannot
starve behind a fixed capture callback order. This fairness queue contains
only stream identifiers, never old video frames; inactive entries expire.
The transport retains TCP reliability; latency on severely constrained links
still depends on the network. A future WebRTC/QUIC transport can reuse the
capture, codec, ephemeral stream identity and canvas interfaces, but requires
separate deployment, negotiation and traversal infrastructure.

The full Node server suite and all 37 offscreen `ConnectionManager` QtTest
cases passed locally. Transport tests include actual authenticated video
sockets, opt-out during queued frames, malformed handshakes, forged receipts,
source receipt windows, and a simulated 1 Mbps receiver shared fairly by four
continuously updating screens. The simulation verifies bounded admission and
progress for every stream; it does not measure real desktop frame rate.

Both the relay and desktop clients must contain this extension. It uses the
existing v12 authentication/session envelope without reviving retired messages.
Older targets have no consent and do not publish a stream.

## Validation

The macOS stop callback retains its capture state by value until native stop
completion and the subsequent main-queue cleanup finish. Capturing the helper's
C++ reference parameter previously left that cleanup with a dangling reference,
causing `EXC_BAD_ACCESS` in `objc_storeStrong` at `MacScreenCapture.mm` when a
session stopped or was replaced. `MacScreenCaptureLifecycle` exercises this
deferred lifecycle with a fake native stream and checks resource release without
requesting screen-recording access. Objective-C blocks preserve C++ references
instead of copying the referred object; see the
[Clang Blocks specification](https://clang.llvm.org/docs/BlockLanguageSpec.html#c-extensions).

Automated coverage includes codec dimensions, color planes, native encoder
selection, orientation, self-contained keyframes, frame lifetime, malformed
packets, default-off settings, atomic Save/Cancel, per-profile persistence,
Qt Quick rendered pixels, monitor lifecycle, real Qt/Node socket authentication,
decoding, in-flight cancellation, generation fencing, role/consent checks and
congestion recovery.

Verified locally on macOS 26.1 with Qt 6.11.2:

- Codec coverage passed 28 cases, with the optional live desktop case skipped.
  This includes both Qt surface and presentation rotations/mirroring, native
  IOSurface lifetime, VideoToolbox asynchronous keyframe recovery, and the
  software fallback reading the same native NV12 buffer. The 1080p synthetic
  IOSurface sequence measured approximately 1.8 ms per encode and 1.9 ms per
  decode; CPU BGRA input measured 23.0 ms for scaling/encoding and 1.2 ms for
  decoding. These local synthetic timings exclude capture, network transport
  and canvas rendering; they are not an end-to-end latency measurement.
- The explicitly enabled live ScreenCaptureKit smoke checked
  `CGPreflightScreenCaptureAccess` and skipped because this test executable had
  no existing screen-recording permission. It did not prompt for permission or
  capture desktop pixels. On an already authorized executable, run
  `QT_QPA_PLATFORM=cocoa MOUFFETTE_TEST_SCREEN_CAPTURE=1 ./tst_ScreenStreamCodec nativeDesktopSmokeWhenExplicitlyEnabled`.
  This case captures transient samples, encodes and decodes them, and reports
  only counts; it never saves or displays screenshots.
- The setting defaults to off, survives reload in its own profile, and changes
  only after a successful Save. Cancel discards the checkbox change; invalid
  settings or a failed disk write leave the saved consent unchanged. A malformed
  persisted consent value remains disabled.
- Screen frame delivery leaves the canvas model and project serialization
  unchanged. Monitor removal, layout replacement, switching peers, hiding
  content and disconnecting clear retained frames.
- A synthetic `QVideoFrame` renders inside the screen rectangle, follows
  canvas zoom, and disappears when cleared. This test passed with both the
  offscreen renderer and the native Metal scene graph
  (`QSG_RHI_BACKEND=metal`); it does not capture the desktop.
- The `ScreenSharingService` cases `consentDecodeDetachAndRevocation` and
  `missingPredecessorRecoversOnlyWithAKeyframe` passed against the real Qt/Node
  relay in 5.7 seconds (4 QtTest passes including setup and cleanup). They verify
  decoded BT.709 color planes, consent, revocation, cancellation during a
  decode, stale stream rejection, missing-frame recovery and a 36-frame burst
  that discards obsolete work before recovering on an IDR.
- The existing canvas tests
  `notificationPeerPicturesResolveProfilesAndKeepCircularFallback` and
  `altScrollScalesSelectionAroundItsCenter:video-0-0` failed in the full
  offscreen run. Both passed when rerun with native Metal (1.2 seconds); those
  offscreen results are not evidence of a native rendering failure.
- The broader regression run passed `ClientConnectionFlow`, `SystemMonitor`,
  `SceneRunCoordinator`, `MediaOverlay` at both scales, and the architecture gate.
  `MediaFrameItem` requires a native RHI and failed to initialize it offscreen;
  its entire suite passed with Metal. Four checkerboard rendering cases in
  each `CanvasInteraction` scale failed under the software/offscreen backend;
  all passed in native Metal reruns at both scales. The remaining interaction
  cases passed offscreen.

Run `npm test` in `server`, then build and run CTest. Focused targets are
`ScreenStreamCodec`, `ScreenSharingService`, `ConnectionManager`, `ClientProfile`
and `CanvasSelectionBackend`. Synthetic tests do not request desktop capture
permission. Native capture, OS consent, multiple physical displays and Windows
GPU/driver combinations require the platform smoke tests below.

Viewer feedback regressions cover capture-error toast deduplication, silent
recovery/hiding, missing and stale frames, and runtime notification routing in
`ScreenSharingService`. `MediaOverlay::screenAvailabilitySharesStatusCardWithVolume`
checks both labels/icons and stable geometry at the minimum window width.

1. On two clients, open the target canvas with consent disabled: no pixels.
2. Enable sharing on the target, grant macOS permission if requested, and save.
   Verify every physical monitor, including duplicate names and portrait/DPI
   combinations, maps to its canvas rectangle.
3. Move windows and play a video; verify canvas pan/zoom and editing remain
   responsive. Test while simultaneously uploading a large file.
4. Disable sharing during motion: the viewer must clear immediately. Re-enable,
   disconnect/reconnect, hide/reopen the viewer, and lock/unlock the target.
5. Unplug/reorder/rotate a monitor during streaming and verify no old monitor
   image is retained under a reassigned ID.
6. Constrain network throughput, then restore it: verify bounded memory and
   recovery to current frames rather than a long delayed video queue.

API references: [Qt QScreenCapture](https://doc.qt.io/qt-6/qscreencapture.html),
[Qt QVideoSink](https://doc.qt.io/qt-6/qvideosink.html),
[Apple ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit),
[FFmpeg codec options](https://ffmpeg.org/ffmpeg-codecs.html).
