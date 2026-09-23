# Live screen sharing

The connected client's desktop appears inside each existing canvas screen,
under the editable scene media. Pan, zoom, monitor layout and the existing
remote cursor keep their normal canvas coordinates. Each project retains the last
presented frame of each screen as a PNG snapshot. These are still images, not a
recording or received-media upload; live grants and decoder state remain transient.

Desktop publication with complete Mouffette exclusion is supported on macOS
and Windows. Linux and other platforms can receive remote desktops and render
their scene media, but cannot publish a desktop until their capture backend can
exclude the application. An unsupported publisher reports a capture error
instead of starting unfiltered Qt screen capture.

## Consent and lifecycle

In **Settings**, enable **Share my screen** and save.
The setting is off by default and belongs to the local runtime profile. Cancel
does not change it. It authorizes all screens for owners of an authenticated
active remote session. **Share my system audio** separately authorizes the audio
stream; neither option permits microphone capture or remote input. Existing
combined consent initializes both permissions, which are then saved independently.
All windows and audio owned by the publishing Mouffette process are excluded,
including its main interface, dialogs, previews and received scenes. The viewer
renders scene media over the captured desktop and plays their audio separately;
enabling either remote stream does not hide or mute those local scene media.
See [system audio sharing](system-audio-sharing.md).

The top-bar **Hide screen content / Show screen content** button controls the
viewer's desktop preview. Viewing is on by default. The choice is saved in the
viewer's runtime profile and applies to all projects and remote clients. The
primary instance remembers it across restarts; temporary instances keep it for
their session. Hiding immediately clears screen images across all projects,
durably invalidates their saved previews, and unsubscribes from the stream.
Showing subscribes again to the visible canvas and waits for fresh frames;
old queued writes or reads cannot resurrect an image cleared by Hide. Monitor outlines, scene
media and the remote cursor remain available. This preference does not change
either client's permission to share its own screens. The adjacent **Play system
audio / Stop system audio** button controls a separate listening subscription;
showing or hiding screen content does not change it. The former canvas mute button
has been removed, and its saved choice migrates to the new listening preference.

If disk failure prevents durable preview invalidation, Hide still removes the
displayed images and remains saved. Show retries that invalidation before saving
its preference; if it fails again, viewing stays hidden and reports an error.

The remote connection card shows **Screen disabled** when the viewer turns off
screen content locally. With viewing enabled, it shows **Screen loading** while
connecting and waiting for the first frame, then **Screen available** once frames
arrive. Disabled remote sharing shows **Screen not shared**; a capture, decode or
transport failure leaving no healthy displayed monitor shows **Screen error**.
Other unavailable states show **Screen not available**. A capture, decode or stale-frame failure
on one monitor freezes its last frame; another healthy monitor keeps the
connection card **Screen available**. A separate **Audio disabled / loading /
available / not shared / error / not available** indicator reports audio's own
state, and hovering either indicator exposes its detail. These
indicators share the connection statuses' font, foreground and background colors:
amber while loading, green when available, red when unavailable or locally disabled.
Screen availability follows decoded frames for the currently viewed client, not just
publishing consent. There is no permanent status message inside the canvas screens.

Screen-sharing problems use warning toasts and notification history: remote
sharing disabled, missing OS permission, capture or decode errors, interrupted
video transport, no first frame after the configured timeout (15 seconds by
default), or no decoded updates for the configured stale timeout (8 seconds).
Normal handshakes and successful recovery are silent. Monitor-specific warnings
identify the affected screen and suppress duplicate reasons until that screen
recovers or the viewing attempt changes. Other healthy screens keep their
images and availability. Intentional hiding and suspension do not create warnings.
Audio warnings and notifications are separate. Revoking or failing screen capture
leaves authorized audio running; audio denial, codec failure or playback failure
leaves healthy screen previews running. Shared timing observations still synchronize
both streams when available.

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
hiding the viewer window, disabling publisher consent, losing the session or
network degradation stops live updates while preserving the last project image.
Within that canvas, only monitors intersecting the viewport are requested.
An offscreen monitor is unsubscribed and its pending decode work is fenced,
while its last image remains available when it returns to view. Capture exists
only while at least one permitted viewer requests that monitor.

`ProjectScreenPreviewStore` coalesces snapshots and converts/encodes/writes them
on a serial background worker, independently of project authoring autosave.
Interruptions flush the latest presented frames, and clean shutdown drains them.
The versioned sidecar directory lives under the profile's projects directory,
keyed by project identity and screen ID. Restoring a project loads its images
offline, without opening a stream or granting permission to publish. A late disk
read cannot replace newer live pixels. Explicit Hide commits a new store
generation before cleanup, so a rapid Hide/Show or restart cannot expose old
snapshots. Deleting/expiring a project removes its snapshots; startup prunes
orphans left by a crash or project reset. The media inactivity deadline can unload
screen RAM without erasing disk snapshots; returning activity reloads them.
Removed monitors are not rendered, and existing IDs keep their last pixels
through layout changes until a new frame replaces them.

The viewport adapter requests the full monitor image at a useful resolution
from its displayed size, zoom, effective device pixel ratio and the configured
oversampling margin (125% by default). Requests round up in 160-pixel steps,
remain even and respect the native and configured maximum edge where possible.
Changes are coalesced over 200 ms by default and identical demands emit nothing;
panning does not crop pixels or change screen coordinates. An empty demand
means no monitor is visible. A host without a known viewport uses the legacy
subscription behavior until it can report its demand.

With negotiated media v2, multiple viewers reuse one capture per monitor and
at most two source encoders. Each layer is uploaded **once**, independently of
the number of viewers. The relay forwards compressed H.264 without decoding,
scaling or transcoding. It selects main video, low video or recent independent
IDRs separately for each viewer; one slow viewer no longer controls the source's
upload budget. The default admission limit is ten viewers per publisher.

The optional low layer is requested only when bandwidth, viewport or decoder
capabilities justify it. It defaults to a maximum edge of 960 pixels, 20 FPS and
750 kbit/s; these are ceilings, not a fixed output rate. It receives at most a
quarter of its monitor's budget and is suppressed on a weak uplink, redundant
profiles or sustained encoder overload. Only one useful layer remains in those
cases. Source quality and actual cadence still adapt to upload conditions and
CPU capacity. A static desktop supplies a freshness sample at the configured
idle interval (1 second by default); received images expire after the stale
timeout. Legacy servers retain the original per-viewer-copy mode.

## Capture and video path

- macOS uses ScreenCaptureKit with native display IDs and SDR NV12. A process
  exclusion filter has no window exceptions: all of Mouffette's windows,
  including scenes created after streaming starts, stay out of the capture.
  There is no scene window inventory or scene-triggered filter refresh. Missing
  application discovery fails capture instead of sharing an unfiltered display.
  Capture
  dimensions and requested FPS follow the largest active encoding profile, up to
  the configured 3,840-pixel edge and 30 FPS defaults. Cursor and audio are not
  captured by these per-monitor streams. A separate audio capture serves the
  whole endpoint. An IOSurface-backed CVPixelBuffer reaches VideoToolbox without a
  CPU pixel copy when its dimensions match the encoder. A smaller low layer
  uses software scaling of the same native surface; no second desktop capture
  is created. Native reconfiguration
  is coalesced and stale surfaces are rejected. The native queue has three
  surfaces.
- Windows uses Qt's FFmpeg-backed QScreenCapture and DXGI Desktop Duplication.
  Every top-level Mouffette window receives `WDA_EXCLUDEFROMCAPTURE`, including
  transparent scenes and dialogs, before showing and after native recreation.
  A GUI-thread native hook also protects dialogs and menus running their own
  modal event loops. Hidden infrastructure windows are excluded when shown.
  Scene stacking registration never removes this affinity. This requires
  Windows 10 version 2004 or later; an exclusion failure stops screen sharing.
  If DXGI fails (including `DuplicateOutput` returning `0x8000ffff`), the same
  publication switches once to Windows Graphics Capture's monitor API. The
  fallback keeps the window-exclusion policy, uses a two-surface native pool
  with at most one pending frame,
  and fences the old encoder chain before delivering frames with native QPC
  timestamps. It polls on an MTA worker and copies only at the requested FPS.
  D3D11 device creation can fall back to WARP. A failure of both capture APIs
  reports both diagnostics; it never captures an unfiltered desktop via GDI.
  WGC may display Windows' capture border. A missing first frame times out after
  five seconds. C++/WinRT headers are required by the Windows build.
  Screen IDs use the discovery/cursor monitor inventory. Portrait rotation and
  mirroring are normalized. Scaling and encoder cadence follow the same profile;
  the Windows capture backend itself can still produce samples more frequently.
- H.264 prefers VideoToolbox on macOS and NVENC, Quick Sync or AMF on Windows,
  with libx264 or OpenH264 as software fallback. A complete Windows DXGI adapter
  inventory skips manufacturers absent from the machine; an unavailable inventory
  preserves codec probing. It refreshes every minute to allow GPU/driver recovery.
  The current profile supplies
  bitrate, dimensions and FPS. There are no B frames. Output dimensions are even
  and aspect preserving; self-contained IDR frames include SPS/PPS.
- The software x264 preset defaults to `veryfast` and is configurable. Profile
  changes that affect encoding reopen the codec and begin with an IDR. Normal
  keyframes are requested every 4 seconds by default; recovery requests are
  rate limited to avoid repeated bursts. Encoder failure permits at most three
  reduced-profile restarts during one capture's lifetime. Reduced edge, FPS and
  bitrate ceilings remain in force until a new capture is created, so improving
  network feedback cannot repeatedly restore an already-failed hardware profile.
  Exhausting this recovery path stops only the affected monitor.
- Capture keeps one latest raw frame and at most one encoded delivery callback
  pending. Each captured screen has a worker thread. Backpressure pauses new
  encoding while keeping the newest raw sample. Changed frames follow the
  profile's FPS limit; unchanged input uses the idle interval. A delayed native
  recovery IDR receives follow-up samples at that same profile cadence.
- Decoding uses two workers, at most three waiting packets and 4 MiB per screen,
  plus a configurable queue-age limit (200 ms by default). Gaps, stale queued
  work and overload abandon dependent P frames and request an IDR. Stopped
  streams discard in-flight results. Slow decoding is reported to the relay;
  in legacy mode it is forwarded to the source.
  Decode errors and stale frames reset and recover the affected decoder while
  the canvas retains its last image. A screen explicitly reported as capture-failed rejects
  delayed video packets until a matching `starting`/`streaming` state or a new
  stream authorizes reception again; an old keyframe cannot revive it.
- Decoded FFmpeg YUV planes remain in QVideoFrame and use the existing
  SharedVideoNode/Qt Quick graphics path. Video frame delivery does not rebuild
  the screen model or invalidate document/project state. Replacement planes are
  prepared before releasing the displayed image; an import failure preserves
  the last renderable pixels. Saved-image/live-video format changes commit the
  texture uploads in the same draw, without an empty intermediate material.
  A genuinely black captured image remains valid screen content.

## Adaptive policy and network use

The initial aggregate video target is **1.2 Mbit/s**, with a default floor of
128 kbit/s and ceiling of **12 Mbit/s**. These cover all captured screens and
both layers together; viewer count does not multiply this source budget in v2.
The server separately budgets its aggregate outgoing traffic and each viewer's
incoming preview, shared across subscribed screens. When a local upload needs
capacity (or a reported remote upload in legacy mode), source preview is capped
at **1 Mbit/s** by default. Network headers and TCP retransmissions are additional
traffic, so these values are not exact wire-rate guarantees.

Source receipts and control RTT feed the source policy; downstream delivery and
decoder feedback feed the relay's independent viewer policy. In legacy mode those
viewer reports also affect the source. Each measured path has its own minimum RTT baseline: a healthy
long-RTT link is not itself congestion. Growing delay, dropped work, blocked
transport or sustained encoder overload can lower the target. With adaptation
on, a congestion step multiplies it by 0.65, at most once per feedback interval
(500 ms by default). Recovery waits 5 seconds by default, then raises the target
by 20% or at least 32 kbit/s when recent feedback remains healthy and no upload
is active. Reconnecting clears old measurements and returns conservatively.
These are application heuristics over TCP, not libwebrtc's congestion controller
or a measurement of the subscription's advertised Internet speed.

The policy divides this budget between demanded screens and their layers,
accounts for viewer copies only in legacy mode, and updates encoding profiles
at most once per second. The current
compiled quality ladder is:

| Per-screen encoding share | Maximum edge | Moving-content FPS target |
| --- | ---: | ---: |
| Below 220 kbit/s | 320 | 5 |
| From 220 kbit/s | 480 | 8 |
| From 400 kbit/s | 640 | 12 |
| From 700 kbit/s | 960 | 15 |
| From 1.1 Mbit/s | 1280 | 24 |
| From 2.2 Mbit/s | 1920 | 30 |
| From 4.5 Mbit/s | 2560 | 30 |
| From 8 Mbit/s | 3840 | 30 |
| From 16 Mbit/s | 3840 | 60 |

Viewport demand, native size and configured FPS/edge limits can lower these
values. A receiver's hard decoder cap is served by low where available; if low
is unavailable, main is capped to remain decodable for all subscribed viewers.
This capability constraint is separate from downstream congestion. With the default 12 Mbit/s total ceiling and
30 FPS cap, 60 FPS is not selected. Raising the ceiling and FPS cap only permits
it where the path, display demand and hardware support it. A 4K display is no
longer always forced to 1080p; equally, a 1 Gbit/s link does not justify sending
4K to a small canvas thumbnail. Codec/profile targets do not guarantee displayed
FPS, lossless text or HDR. Motion-versus-text classification and AV1 are not
implemented.

[AppConfig reference](../src/backend/config/README.md#adaptive-screen-preview)
lists the validated `.env` knobs, units, precedence and bounds. Edit the embedded
`client/.env` and rebuild, or restart with an external `--env-file <path>`.
Settings are read at startup; the quality controller adapts continuously during
the session without rewriting configuration. Disabling `SCREEN_ADAPTIVE_ENABLED`
uses the maximum configured budget while retaining upload caps, pacing, bounded
queues and viewport selection. Session recovery deadlines remain server authority.

## Transport and restrictive networks

The implemented transport is **adaptive WebSocket video**, using `wss://` when
the server URL selects TLS. It has a separate socket from commands and files.
In v2 there are separate authenticated video sockets for publishing and viewing,
so a client can do both without cross-blocking their receipts. The control
connection issues single-use tokens valid for 15 seconds, bound to its identity,
connection generation and media role. TLS protects each client/server leg; the trusted
relay can access compressed video. There is no end-to-end encryption, UDP,
WebRTC, TURN, automatic transport selection or ordinary HTTPS polling in this
implementation.

Source publications use `MSV2`, a big-endian 16-bit JSON header length, a header
of at most 1,024 bytes and one H.264 Annex B access unit of at most 2 MiB.
Metadata binds the publication epoch, monitor, layer and source sequence. The
relay sends `MSV1` to viewers, using their own authenticated `remoteSessionId`,
`generation`, server-issued `streamId` and monotonically increasing delivery
sequence. The receiver therefore remains compatible with independent layer
switches. It resets the decoder only on a self-contained IDR containing SPS/PPS.
Legacy publication also retains `MSV1`. No base64 image,
temporary video file or per-frame control message is involved. Feedback and
viewport capabilities are negotiated; older receivers retain a conservative
1,920-pixel limit rather than receiving unsupported 4K packets.

The relay validates publisher role, current connections, session lease, opt-in,
monitor inventory, viewport demand and stream generation. Revocation, channel
replacement and monitor topology changes issue new stream identities, preventing
old TCP or decoder work from reappearing after teardown. Source receipts bind publication/screen/layer/sequence; viewer receipts bind
stream/screen/sequence. The source receipt acknowledges relay ingress without
waiting for any viewer. An ACK confirms transport consumption, not necessarily
a displayed frame. Decode feedback is a separate signal.

The sender spaces encoded packet admission against the aggregate budget, counting
each uploaded layer once in v2 and every copied payload in legacy mode. Its ordinary byte window uses current bitrate multiplied
by baseline receipt RTT plus the configured **150 ms** target margin, with a
32 KiB minimum. Hard caps default to **2,048 KiB and 64 unacknowledged frames**;
they accommodate data already in transit and are not a desired backlog. A
standalone packet may exceed the ordinary byte window only within the packet
and serialization-time guards. One recovery IDR may use a larger bounded allowance only when source byte
credit, socket backlog and pacing debt are all empty. It repays its entire cost;
an ACK does not erase pacing debt. If a frame still exceeds its admission bound,
a separate per-layer ceiling lowers edge (down to 160 pixels), bitrate (down to
32 kbit/s) and FPS (down to one). These bounds recover progressively after five
seconds without a size rejection. Repeated rejections at the floor do not keep
reopening the encoder. The source rejects excessive bursts rather than putting
them behind a long TCP queue. Relay byte/frame limits are independently
configured in `server/.env`, with downstream RTT and queue-age guards.

After the configured receipt timeout (3 seconds by default), the disposable
video connection is aborted. Retry waits grow exponentially from 500 ms to
10 seconds by default and reset on successful recovery. Video replacement does
not recreate a healthy command session or cancel an upload. Fair admission keeps
only waiting stream identifiers, never a queue of obsolete encoded frames.
Their expiry includes outstanding pacing debt plus the receipt-timeout margin,
so a stream waiting during a budgeted send pause does not lose its turn merely
because no raw frame was admitted. No expired image is retained for that turn.
Upload sends are also spaced when the control RTT grows above its own baseline.
TCP reliability still causes head-of-line delays under packet loss, and an
unrelated application can saturate the shared network.

The client supports system proxy policy, explicit HTTP CONNECT or SOCKS5, and
direct mode, consistently across control, upload and both media sockets. Explicit credentials are
supplied only for the configured proxy host/port; TLS certificate verification
remains enabled. See the [proxy settings](../src/backend/config/README.md#network-proxies).
System discovery and enterprise authentication depend on Qt and the OS and must
be tested on the target network. The Node service itself listens on WebSocket;
production WSS requires the deployment's TLS frontend.

If WebRTC is blocked but WSS is allowed, the current implementation is already
usable because it does not require WebRTC. If the network blocks WSS or access
to the service, this implementation cannot establish a session via ordinary
HTTPS instead. A port-443 address alone is not a universal connectivity promise.
The [adaptability plan](screen-stream-adaptability-plan.md) documents the remaining
native WebRTC/TURN and HTTPS session fallback. Independent-viewer layers and
latest-IDR delivery are implemented on the existing reliable transport.

Both relay and desktop clients should be upgraded together. The extension uses
the existing v12 authenticated session envelope without reviving retired
messages; legacy targets without consent do not publish.

## Relay resource limits and slow viewers

The relay caches only the newest self-contained IDR for each active layer,
with a global default cap of 16 MiB and a 5-second TTL. It does not retain a GOP
history or raw pixels. Snapshot delivery waits for the previous receipt and
sends a newer cached image, never repeats an old frame as a fresh update. A
larger snapshot can occupy the empty receiver window alone, with its complete
pacing cost and a receipt deadline accounting for serialization. The configured
transfer estimate defaults to at most four seconds, within cache freshness;
a frame too large for this bound cannot monopolize other monitors' turns.
Dropping a dependent H.264 frame fences that viewer's chain until another IDR;
recovery requests are coalesced across viewers of the same publication.
Quality recovery requires stable feedback rather than immediate oscillation.
Replacing the viewer socket retains a conservative estimate for that
control connection while discarding old RTT samples and byte debt.

A Linux Docker server needs no GPU or FFmpeg for this path. Its remaining costs
are networking, TLS where terminated locally, packet validation/copying and
bounded per-session metadata. Outgoing bandwidth still grows with viewer count:
ten viewers at 2 Mbit/s need roughly 20 Mbit/s of server egress plus overhead.
Defaults cap total preview egress at 100 Mbit/s and active publications at 256;
these are resource guards, not a capacity benchmark for a small server. Configure
these values for the actual host and measure alongside file transfers. See
[server protocol and configuration](../../server/SCREEN_SHARING_PROTOCOL.md).

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

The historical native baseline below was recorded on macOS 26.1 with Qt 6.11.2
before this adaptive policy. It validates native capture/rendering mechanics;
its timings and test counts do not benchmark the adaptive release:

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
- Screen frame delivery leaves the canvas authoring model unchanged; snapshots
  use project-owned sidecars. Disconnects and surviving-monitor layout changes
  retain pixels. Explicit Hide clears displayed and durable snapshots, switching
  peers clears that host, and monitor removal detaches its display source.
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
`AppConfig`, `ScreenAdaptiveController`, `ScreenStreamCodec`,
`ScreenSharingService`, `ConnectionManager`, `ClientProfile` and
`CanvasSelectionBackend`. Synthetic tests do not request desktop capture
permission. Native capture, OS consent, multiple physical displays and Windows
GPU/driver combinations require the platform smoke tests below.

Adaptive regressions cover setting bounds/precedence and atomic reloads,
healthy high RTT versus growing queues, bitrate reduction/recovery, upload caps,
viewport selection/coalescence, per-screen clearing, native profile transitions,
and explicit proxy authentication matching without changing TLS configuration.
The proxy helper test is synthetic; it does not certify an enterprise proxy.
The viewport/proxy/transient-frame cases passed offscreen (four test cases plus
setup/cleanup) during this change. Broader test results and deployment validation
belong in the implementation plan's current validation record.

Viewer feedback regressions cover capture-error toast deduplication, silent
recovery/hiding, missing and stale frames, and runtime notification routing in
`ScreenSharingService`. `MediaOverlay::screenAvailabilitySharesStatusCardWithVolume`
checks both labels/icons and stable geometry at the minimum window width.

`WindowsCaptureFailover` injects DXGI failures into the real facade using a fake
WGC backend: duplicate errors switch only once, publication profiles survive,
old frames are fenced, and callbacks after stop/replacement cannot affect a new
capture. It runs offscreen and never captures the desktop. The native WGC path
still requires Windows qualification with the failing GPU/session, multiple
displays, scaling/rotation, exclusion, reconnect and lock/unlock.

1. On two clients, open the target canvas with consent disabled: no pixels.
2. Enable sharing on the target, grant macOS permission if requested, and save.
   Verify every physical monitor, including duplicate names and portrait/DPI
   combinations, maps to its canvas rectangle.
3. Move windows and play a video; verify canvas pan/zoom and editing remain
   responsive. Test while simultaneously uploading a large file.
4. Disable screen sharing during motion: the viewer must freeze the last image
   while authorized audio continues. Hide screen content on the viewer: all last
   images must disappear and stay absent after Show until fresh frames arrive. Disable audio sharing with screen sharing enabled:
   healthy screens must remain visible. Repeat using the two viewer controls and
   independently denied/failed captures; verify each status and warning identifies
   only the affected medium. Re-enable, disconnect/reconnect, hide/reopen the viewer,
   and lock/unlock the target.
5. Unplug/reorder/rotate a monitor during streaming: removed monitor rectangles
   disappear, surviving screen IDs retain their last pixels until fresh frames
   arrive. Restore the project offline and verify its saved layout and images.
6. Constrain uplink and downlink independently, add latency/loss, then restore
   them. Verify profile reduction/recovery, bounded memory and current frames.
   Measure command RTT and file progress alongside displayed video FPS.
7. Pan until one or every monitor leaves the viewport; verify unsubscribed
   capture stops where no other viewer needs it. Zoom back and check recovery.
8. Test WSS through the actual HTTP/SOCKS5 proxy, authentication and enterprise
   trust configuration. A WSS-blocked network is an expected unsupported path
   until the HTTPS session fallback has been implemented.
9. With sharing active, open the target's main interface, dialogs and a received
   scene. None of these windows should appear in the desktop stream. Repeat
   after closing/recreating scenes and switching between monitors. The viewer's
   scene media must remain visible over the desktop and their audio must keep
   playing as screen sharing and system-audio listening are toggled independently.
   Verify on both DXGI and WGC on Windows, including transparent scene surfaces.

API references: [Qt QScreenCapture](https://doc.qt.io/qt-6/qscreencapture.html),
[Windows monitor capture](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor),
[Windows capture exclusion](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity),
[Qt QVideoSink](https://doc.qt.io/qt-6/qvideosink.html),
[Apple ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit),
[Apple application exclusion](https://developer.apple.com/videos/play/wwdc2022/10155/),
[FFmpeg codec options](https://ffmpeg.org/ffmpeg-codecs.html).
