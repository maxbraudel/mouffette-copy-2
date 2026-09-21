# Screen preview resilience: implementation and deployment plan

## Product and architecture

Mouffette is a media authoring and synchronized scene playback application.
The desktop video is an ephemeral supervision layer under editable media in a
zoomable multi-monitor canvas. Commands, clock synchronization, consent,
revocation and file integrity take priority over this preview. A preview failure
must neither restart a scene nor invalidate an otherwise healthy session.

The current Qt/C++17 client captures NV12 using ScreenCaptureKit on macOS and
QScreenCapture on Windows, encodes H.264 and renders decoded YUV through Qt Quick.
The Node relay authenticates separate control, upload and video WebSockets.
Uploads use durable acknowledgements and shared recipient windows. Video has
bounded receipts on both legs, but initially had fixed encoding parameters and
no downstream congestion feedback. Negotiated media v2 now uploads each source layer once per monitor and forwards
it independently to viewers. Legacy peers retain per-session upload copies.

## Delivery sequence

1. Harden the existing reliable transport end to end: validated runtime settings,
   conservative start, aggregate bitrate budget including subscriber copies,
   per-leg receipt baselines, downstream/decode feedback, rapid reduction and
   slow recovery, upload priority, bounded buffers and reconnect backoff.
2. Adjust capture and encoding resolution/FPS/bitrate together, retain native
   buffers, avoid repeated recovery keyframes, and keep only fresh work.
   Configure software compression explicitly rather than always using ultrafast.
   Isolate capture/decode/stale failures per monitor. Keep reduced encoder
   recovery ceilings for the capture lifetime and bound restart attempts.
3. Subscribe to the visible monitor set and request a useful resolution for the
   actual viewport and device pixel ratio. Quantize/debounce changes, fence
   stopped streams and keep project serialization independent of preview state.
4. **Implemented shared publication:** authenticate separate publish/view paths;
   capture once per monitor; encode main plus at most one useful low layer at
   source; upload each layer once; forward main/low or recent self-contained IDRs
   per viewer without server decoding/transcoding. Fence epochs on revocation,
   publisher replacement and topology changes. Bound cache, receiver windows,
   aggregate server egress and admission (ten viewers per source by default).
5. **Future transport work:** integrate a version-pinned native WebRTC stack and
   deployed TURN where measurements justify it; retain the working WSS path.
   Add ordinary HTTPS session/preview transport for networks blocking WSS.
6. Validate the complete chain against shaped networks, blocked transports,
   enterprise proxies, native hardware and concurrent file/scene operations.

Step 5 requires actual implementations and deployment verification; it must
never be represented by no-op `.env` flags. This document distinguishes the
implemented reliable path from those deployment-dependent parts in its final
validation record.

## Shared publication design decision

For the current requirement (up to ten viewers per source, Linux/Docker server
with no GPU), server transcoding is deliberately absent. Its per-source decoder
and multiple encoders would move a substantial CPU burden onto the constrained
host. Sending independent full-resolution JPEG/WebP images at a fixed 30 FPS
would discard inter-frame compression and cannot guarantee freshness on a weak
upload. A single predictive H.264 stream also cannot be arbitrarily frame-skipped:
P frames require their predecessors. The implemented compromise uses standard
H.264 access units and bounded source-side simulcast.

1. The source receives an aggregate publication grant independent of a viewer's
   session. It allocates the upload budget across screens, then main/optional
   low. The low layer is demand-driven, defaults to at most 960 pixels/20 FPS/
   750 kbit/s, and gets at most a quarter of a monitor's budget. Activation needs
   600 kbit/s total by default, with a 20% hold band. Redundant profiles collapse.
2. Source RTT growth, control RTT, uploads and encoding time govern the source;
   downstream feedback governs only that viewer at the relay. CPU guard uses a
   smoothed encode-time/frame-period ratio, warm-up and sustained overload;
   optional low is abandoned before reducing the remaining useful stream.
   Actual oversized frames additionally teach independent spatial/bitrate/FPS
   ceilings, with a bounded IDR exception, reduction as far as 160 pixels/1 FPS/
   32 kbit/s and slow recovery instead of endless rejection at the ordinary
   quality ladder's floor.
3. Publication and viewing use separately authenticated disposable sockets.
   Receiving congestion cannot close an otherwise healthy publisher. Old
   servers negotiate legacy behavior; the server rollout flag can also disable
   v2 without changing the control protocol version.
4. The relay assigns delivery sequences per viewer and changes layers only on
   complete IDRs with SPS/PPS. Below available video rates it sends independent
   recent IDRs. No P-frame history is retained; a skipped dependency requires
   recovery. Viewer budgets and server egress are aggregate, not per-screen
   allowances. Cache TTL, global bytes, pending receipts and viewers are bounded.
5. Consent, active command-ready session, requested monitor and transport epoch
   remain authority. Last-viewer departure stops publication and purges pixels;
   departure of one among several viewers preserves the shared source.

This follows the selective-forwarding principle (no media transcoding), while
remaining a custom WS/WSS relay, not a deployed WebRTC SFU. Primary references:
[mediasoup architecture](https://mediasoup.org/documentation/overview/) and
[H.264 decoder refresh procedure](https://www.rfc-editor.org/rfc/rfc6184.html#section-8.5.1).

## Transport strategy and prerequisites

The long-term native candidate is libwebrtc through libmediasoupclient, with
mediasoup integrated into the Node service and TURN/TLS for restrictive networks.
Current official libmediasoupclient installation targets C++20 and libwebrtc
m140/branch-heads/7339. Reproducible macOS and Windows builds, ABI/toolchain and
BoringSSL/OpenSSL coexistence must be verified before adopting it. The existing
Windows distribution uses MinGW; compatibility cannot be assumed.

WebRTC UDP -> TURN/TLS -> WSS -> ordinary HTTPS preview is a preference policy,
not a guarantee that all networks allow these protocols. TURN on port 443 is not
HTTPS. WSS can be blocked separately. The final HTTPS mode also needs command
and event transport; polling images alone does not rescue a WebSocket session.
Keep TLS verification enabled. Proxy configuration/authentication and system
trust must be tested on each supported OS.

Signal negotiations through the existing authenticated session, bind every media
transport to consent, endpoint, generation and stream identity, and validate a
decoded frame before promoting it. Limit duplicate traffic during transition.
Use retry backoff and stable recovery to avoid switching loops. Preserve the
shared allocator across transports; estimates are transport/path specific.

For independent viewer quality, publish bounded simulcast layers on demand,
with SFU layer selection per viewer. The shared H.264 fallback deliberately uses
a conservative common encoding profile; a slow viewer can limit that profile.
This limitation must remain visible in documentation until layered publishing
is integrated. Do not multiply a full bitrate allowance by monitor/viewer count.

## Next implementation: native WebRTC and transport selection

Everything in this section is proposed work. The file names below describe new
components to create; they are not currently present or callable interfaces.

1. **Freeze and prove the native dependency set before switching media.** Add a
   reviewed dependency manifest (proposed
   `client/vendor/webrtc/dependencies.lock.json`) with exact libwebrtc commit,
   libmediasoupclient release/commit, GN arguments, toolchain/SDK versions,
   artifact checksums and licenses. The currently documented compatibility
   candidate is libwebrtc m140 / `branch-heads/7339`; a moving branch name alone
   is not an acceptable production pin. Select and verify the concrete release
   pair, build both supported platforms and record the tested revisions. Add
   `client/cmake/WebRtcDependencies.cmake` and reproducible scripts under
   `client/scripts/internal/`. Isolate the C++20 adapter and resolve the Windows
   MinGW/MSVC boundary, OpenSSL/BoringSSL symbols and signing/package changes.
   A server mediasoup release must likewise be exact in `server/package.json`
   and the npm lockfile; pin coturn deployment images by digest. Do not add an
   optional configuration switch that merely reports success without these
   dependencies and working transport.
2. **Extract a transport contract without changing consent or rendering.** Add
   a proposed `IScreenMediaTransport` under
   `client/src/backend/screensharing/`, owned by `ScreenSharingService`.
   Its operations should start/stop an authorized session binding, apply screen
   demand and the shared outbound budget, and report path state, feedback and
   decoded frames with stream/generation identity. Implement the existing WSS
   path behind that contract first and retain its tests. Capture, authorization,
   session deadlines and scene/upload lifetime remain outside transport choice.
   This refactor must preserve monitor-specific failure fencing and native pixel
   buffer ownership; a media event must never mutate project data.
3. **Connect native media to libwebrtc.** Add `WebRtcScreenTransport` and a
   capture-to-video-track adapter. Provide the raw-frame hook needed by
   libwebrtc, preserve NV12/IOSurface when supported, and bridge decoded frames
   to the existing `QVideoFrame` signal. Keep the existing FFmpeg encoder for
   WSS. A bridge that only injects H.264 packets is insufficient to claim that
   libwebrtc controls encoding; prove the linkage between congestion estimates,
   pacing, encoder bitrate and capture constraints. Run negotiation and any
   blocking libmediasoupclient operations away from the GUI thread, and make
   cancellation reject work from retired stream generations.
4. **Add the authenticated media relay and real deployment.** Introduce
   `server/screen_webrtc_relay.js`, route signaling through the existing
   authenticated `server.js` session dispatcher, and bind each transport,
   producer and consumer to the same consent/session/monitor grants as
   `screen_share_relay.js`. Check authorization on every create/connect/consume
   operation; revoke media immediately on consent loss or session teardown.
   Supervise mediasoup workers and clean up failed transports without renewing
   session authority. Add deployment files under a new `server/deploy/media/`
   with announced reachable addresses, UDP ports, coturn TLS/443 certificates,
   temporary relay credentials, allocation quotas and restart/health checks.
   TURN needs a reachable listener/address plan; sharing a hostname/port with
   HTTPS must be designed explicitly rather than assumed to work through an
   HTTP reverse proxy.
5. **Implement and qualify promotion/fallback.** Add a transport selector owned
   by the media service. Preserve functioning WSS while trying WebRTC; confirm
   reception and decoding before promotion. A single shared budget includes
   both paths during their short overlap. Transitions create fenced media
   epochs and require a new decodable frame, with stable retry/backoff to avoid
   repeated switches. Implement UDP and TURN/TLS tests, then force ICE failure,
   TURN rejection and network changes and verify automatic WSS recovery while
   scene authority, commands and file transfer progress survive.

Acceptance requires clean reproducible macOS and Windows builds, real desktop
capture and hardware encode/decode on both, consent revocation during an
in-flight negotiation, stale-frame rejection across switches, and no accidental
clear of another healthy monitor. Record first-frame time, video age, control RTT,
file throughput, memory and CPU across the network matrix. Compare those metrics
to the same workload over the current WSS path. Ship WebRTC only after the
chosen dependency artifacts and deployed relay paths pass this gate.
Independent viewer quality is implemented on WSS with two demand-driven source
layers and an IDR-only mode. A future WebRTC path must preserve this source
budget, consent/session authorization and independent downstream selection.

## Next implementation: ordinary HTTPS fallback

This is also unimplemented work. It must cover the session, not just its images.

1. **Make authority independent of socket type.** Extract transport-neutral
   command dispatch from `server/server.js` and the client `WebSocketClient`
   while retaining existing authenticated identity, boot ID, connection/session
   generations, command order, applied-state ACKs and idempotency rules.
   Introduce a proposed `SessionTransport` interface and an
   `HttpsSessionTransport` client implemented with `QNetworkAccessManager`.
   The HTTPS authentication flow must issue its own fresh challenge and bind
   the resulting transport to the same proofs; a new transport never extends
   the original fixed recovery deadline.
2. **Implement authenticated command and event endpoints.** Add proposed
   `server/http_session_transport.js` and mount it using a shared Node HTTP
   server behind the deployment TLS frontend. Use bounded short POST requests
   for commands and bounded GET event batches with an acknowledged cursor and
   explicit full reconciliation after retention gaps. Bound body size, pending
   requests and retained event history. Requests must tolerate retries without
   replaying a command, and reject old generations or revoked participants.
   Maintain STOP/CLOSE priority and server-authoritative lease semantics.
   Test reverse proxies that buffer streams; this path cannot rely on WebSocket
   upgrade, server-sent events or an indefinitely open response.
3. **Add the low-rate preview.** Create proposed
   `server/screen_http_preview.js` and client `HttpsScreenPreviewTransport`.
   Publish/retrieve independent compressed still images using bounded requests,
   storing only the latest authorized in-memory image per screen. Choose and
   measure an interoperable image format and enforce dimensions, compressed
   size, decoded allocation and freshness limits. Validate consent, viewport,
   session generation and stream epoch on every upload and retrieval. Use
   non-cacheable responses, bounded TTLs and no disk persistence; revoke retained
   pixels immediately. Adapt resolution, image quality and request cadence to
   completed request timing and the same aggregate budget, with no backlog of
   historical screenshots. Report this as a limited preview, not smooth video.
4. **Preserve file behavior when WSS is unavailable.** Either implement an
   authenticated HTTPS upload transport reusing `UploadManager`'s existing
   durable-offset/resume/integrity protocol, or explicitly present file transfer
   as paused in this mode. Do not claim a fully usable WSS-blocked session while
   silently relying on the current upload WebSocket. Qualify concurrent commands,
   preview and transfers against one budget and fixed session deadlines.
5. **Exercise the actual restricted path.** Block both UDP and every WebSocket
   upgrade while allowing only ordinary authenticated HTTPS through a real
   proxy. Verify login, opening and resuming a remote session, command order,
   duplicate/replayed requests, STOP/CLOSE, consent changes and preview expiry.
   Validate enterprise CA/authentication without bypassing certificate checks,
   request cancellation, bounded server memory and a return to WSS when stable.

Only introduce `.env` settings for these components with their implemented
consumers, strict validation and tests. Transport availability must report the
actual negotiated/decoded path; absence of a dependency, listener or permission
must never masquerade as an enabled fallback. Keep the ordinary HTTPS prototype
and deployment disabled from release claims until these acceptance cases pass.

## Validation contract

Automate configuration bounds/precedence; frame dimension and IDR transitions;
capture cancellation; healthy high RTT versus growing queue; abrupt bitrate
collapse/recovery; invalid feedback and stale epochs; viewport subscription;
multiple screens/viewers; concurrent uploads; decode overload; video-only
reconnects and unchanged scene/session lifecycles. Measure displayed frames and
freezes, command RTT percentiles, upload progress, queue age and memory.

Deployment matrix: 1/5/20/100/1000 Mbit/s; independent uplink/downlink limits;
20/100/300 ms RTT; loss and bursts; UDP/TURN/WSS blocked individually; authenticated
HTTP proxy; enterprise CA; network change and sleep/wake. Synthetic codec tests
must never request desktop permission. Real capture, Windows GPUs, high bitrate
4K and enterprise network validation require explicit platform environments.

Primary references:

- https://www.rfc-editor.org/rfc/rfc8834.html
- https://www.rfc-editor.org/rfc/rfc8835.html
- https://www.w3.org/TR/webrtc-stats/
- https://mediasoup.org/documentation/v3/libmediasoupclient/installation/
- https://mediasoup.org/documentation/v3/mediasoup/installation/
- https://doc.qt.io/qt-6/qwebsocket.html


## Implementation status

| Work item | Status in this change |
| --- | --- |
| Adaptive H.264 over the existing WS/WSS channel | Implemented: aggregate budget, per-leg RTT baselines, feedback, pacing, slow recovery and upload caps |
| Capture/encoder profile changes | Implemented: resolution/FPS/bitrate changes, native macOS reconfiguration, software preset and at most three reduced-profile restarts per capture |
| Monitor failure isolation | Implemented: affected-screen clearing/recovery, rejection of delayed failed-screen packets, and retained availability for healthy monitors |
| Viewport demand | Implemented: visible monitors, DPR/zoom sizing, quantization/coalescence and independent clearing of offscreen frames |
| Runtime configuration | Implemented: validated client/server env settings, source precedence, atomic client reload and documented units |
| Proxy policy | Implemented: system/direct/HTTP CONNECT/SOCKS5, explicit authentication fencing and unchanged TLS validation |
| Native WebRTC, SFU and TURN deployment | Not implemented; requires the native/toolchain and deployment work above |
| Ordinary HTTPS preview and session fallback | Not implemented; WSS-blocked networks remain unsupported |
| Independent per-viewer quality | Implemented on WS/WSS: one shared publication, up to two source encoders per monitor, per-viewer selection and latest-IDR mode without server transcoding |
| AV1 and content-aware text/motion classification | Not implemented |
| Full shaped-network/hardware/enterprise qualification | Still required; automated synthetic and local integration results are not equivalent to this deployment matrix |

The implemented path is usable without WebRTC when WS/WSS is permitted. It does
not claim universal network traversal, WebRTC-grade congestion control or a
measured 4K/60 FPS performance guarantee. Packet/frame caps are safety ceilings;
RTT-relative duration and pacing guards continue to constrain queued work.
Encoder recovery remembers reduced maximum edge, FPS and bitrate until a new
capture starts; healthy network measurements cannot bypass a prior local
hardware failure. Capture-failed screens accept pixels again only after the
matching `starting`/`streaming` state or a new stream. Decode/stale recovery and
warnings are scoped to the affected monitor. Metadata-only fairness turns stay
valid through outstanding pacing debt plus the receipt margin, without keeping
obsolete frames. The regression record below includes these corrections.

## Previous adaptive-v1 validation record

The final rebuilt macOS/Qt 6.11.2 binary passed the four targeted
`CanvasSelectionBackend` cases below with `QT_QPA_PLATFORM=offscreen`:

- `proxyPolicyIsIdempotentAndFencesAuthentication`
- `screenPreviewDemandFollowsViewportAndCoalescesChanges`
- `screenPreviewDemandCapsPixelsAndIgnoresFrameDelivery`
- `remoteScreenFramesStayTransientAndRespectMonitorLifecycle`

The run reports six passes including initialization/cleanup, with no failures or
skips. Proxy challenges are simulated without opening network sockets; no
screen-recording permission is requested. These tests verify host/port/type
credential fencing, callback idempotence, TLS preservation, viewport selection,
resolution bounds and the separation of transient frames from project state.

Configuration documentation and test expectations agree on a 15,000 ms initial
frame timeout, an 8,000 ms stale timeout, a 2,048 KiB maximum buffered allowance
and 64 pending receipts by default. These limits remain configurable and subject
to the documented cross-field validation.

Local validation of the previous adaptive-v1 implementation on 2026-09-21:

- The macOS application and all affected regression executables built
  successfully using `client/out/build/macos-debug`.
- The final CTest selection passed all 13 suites in 264.83 seconds:
  `ScreenAdaptiveController`, `ScreenStreamCodec`, `MacScreenCaptureLifecycle`,
  `ConnectionManager`, `RemoteSessionIntegration`, `ScreenSharingService`,
  `AppConfig`, `ClientConnectionFlow`, `SceneRunCoordinator`,
  `UploadRemovalSecurity`, `UploadScheduler`, `NetworkDiagnostics` and
  `QmlArchitectureGate`. This includes exact receipts, pacing, fairness after a
  long RTT, viewport removal/readdition, consent and stream fences, screen-local
  failure/stale recovery, file-removal integrity and scene/session regressions.
- The optional live-desktop codec test was skipped because
  `MOUFFETTE_TEST_SCREEN_CAPTURE=1` was not requested. Other codec cases use
  synthetic frames; these results are not live-capture performance measurements.
- `npm test` in `server` passed all 19 scripts after the final relay changes.
  Screen protocol cases include healthy high RTT, growing queues, aged route
  baselines, eight-monitor fairness, scoped failures, IDR recovery, authorization,
  revocation and malformed/stale feedback.
- `git diff --check` passed.

Reproduce the client regression selection after building its targets:

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir client/out/build/macos-debug \
  -R '^(ScreenAdaptiveController|ScreenStreamCodec|MacScreenCaptureLifecycle|ConnectionManager|RemoteSessionIntegration|ScreenSharingService|AppConfig|ClientConnectionFlow|SceneRunCoordinator|UploadRemovalSecurity|UploadScheduler|NetworkDiagnostics|QmlArchitectureGate)$' \
  --output-on-failure -j3
```

Windows builds/drivers, real capture, end-to-end shaped bandwidth/loss,
4K throughput and enterprise proxy traversal have not been qualified by these
local automated runs. The network matrix and remaining transport stages above
remain release work; this record does not declare them complete.


## Shared-v2 validation record

The shared-publication implementation builds with Qt 6.11.2 on macOS. Its
synthetic capture tests exercise two independently decodable encoders, separate
cadence/idle periods, adding/removing low without breaking main, pending callback
fences, isolated low failure, invalidation after failure and bounded work while
the GUI is blocked. Profile tests check the aggregate layer allocation, demand
and CPU gates, activation hysteresis, configuration extremes and oversized-frame
ceilings/recovery. Network tests cover publication ACK tuples, consent, role
separation, old epochs, bounded recovery IDRs and debt surviving receipt.

The Qt/Node integration adds a single-upload/two-viewer decoded fanout,
independent main/low decoding with delivery sequence translation, one viewer
leaving while another continues, consent revocation and publisher survival
across its own viewing-socket failure. Existing v1 cases run with the server's
real rollout flag disabled to retain old-server compatibility coverage.

Relay regressions include ten viewers per source, heterogeneous feedback,
late-join/source-sequence continuity, snapshot-to-video recovery, bounded IDR
cache/TTL, consent/topology/authentication fences, long snapshot receipt timing,
two-monitor fairness under a 64 kbit/s budget, intrinsically oversized images,
conservative bandwidth across reconnects, aggregate egress and concurrent upload
budget reductions. These are deterministic tests, not a throughput benchmark
of a production Linux/Docker host. No server GPU or transcoder is introduced.

Final local outcomes on 2026-09-21:

- The macOS application and all affected test targets rebuilt successfully.
- All 15 selected client suites are validated: the 13 suites in the previous
  selection plus `ScreenCaptureLayers` and `ScreenPublicationProfiles`. The
  initial run passed 13 and exposed two test-fixture ordering issues. The new
  screen test now avoids evaluating a rate-limited send twice inside a QTRY
  assertion; the existing upload test waits for its exact durable chunk ACK
  before completion, matching the real protocol. After those fixture fixes,
  full `ScreenSharingService` passed in 61.71 seconds and full
  `UploadRemovalSecurity` passed in 13.56 seconds. Upload production code was
  unchanged. The other 13 suites passed in the initial run.
- The final `npm test` passed all 20 server scripts after the late-join,
  snapshot fairness, reconnect estimate and cache-allocation fixes.
- `git diff --check` passed. No live desktop permission was requested; the
  optional real-capture codec smoke remains excluded.

Reproduce the shared-v2 client selection by adding
`ScreenCaptureLayers|ScreenPublicationProfiles` to the CTest expression above.
The server command is `npm test` from `server`.

Windows native capture/encoders, Linux container sizing and the real shaped-link
matrix remain platform/deployment qualification. WSS-blocked networks still
require the future HTTPS session transport described above.
