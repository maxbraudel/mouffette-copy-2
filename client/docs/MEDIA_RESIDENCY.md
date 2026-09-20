# Resident media and shared playback

## Identity and validation

`MediaResidencyManager` owns immutable assets shared by original SHA-256. Every
image/video occurrence has a lease; text needs none. Saved projects, source paths,
transfer payloads and hashes continue to refer to the original file on disk.
Runtime resident representations are rebuilt during background loading. Optional
editing derivatives may persist in the local disk cache; they never replace the
original identity, transfer payload or validation requirement.
Metadata discovery has its own bounded pool, so hashing/decoding does not delay
editable skeleton creation. Source signatures, import cancellation and document
generations fence publication, including after resume of pending imports.

`MediaDecoder` reads the original into RAM, hashes it, and strictly validates all
video frames and selected audio through EOF. Interactive imports retain these
original compressed bytes and build the frame index during that validation pass.
`IndexedMediaDecoder` then indexes compressed audio and prepares the initial audio
buffer without decoding the whole video again. Readiness no longer waits for
all-intra encoding and another complete verification decode. Original quality,
timestamps, SAR, rotation and color metadata are preserved. Both local and remote
residency use this path; duplicate occurrences share the same allocation.

Local authoring has a separate, display-only `ResidentMediaPreview`. After the
first decoded frame, the worker publishes immutable snapshots containing the
shared native poster and up to 16 cumulative thumbnails, each at most 192 × 108.
The canvas can show the poster and the timeline can fill its strip while the
rest of the source is still being validated. A coalesced mailbox keeps only the
latest pending snapshot. Its pixels share the importing asset's allocations;
they are not counted again as a second asset or copied for each snapshot.

Preview publication checks the original hash, source signature, owner,
generation and cancellation. Failure, source replacement, owner release and
memory cancellation remove the preview. `asset()`, `ready()`, local Play,
scene pins and remote PREPARE still require complete validation and initial
playback preparation. A visible poster is not a readiness acknowledgement.
The probe's hash supports early content deduplication; the hash made while
reading resident source bytes remains a second check that decoding uses the
same content. Removing either pass would require a different admitted-source
sharing contract.

The explicit conversion path remains available to the decoder qualification
harness (default low-level `MediaDecoder::decode`, without
`DecodeCallbacks::retainOriginalVideo`). It is not used by canvas imports:

* SDR 8-bit planar YUV420/422/444 becomes full-resolution H.264 all-intra using
  linked libx264, CRF 18, `veryfast`, no B frames, repeated SPS/PPS. Each packet is
  independently decodable. Actual timestamps and durations are indexed; there is
  no conversion to constant frame rate. SAR, rotation and color metadata survive.
* HDR, alpha, unsupported/changing pixel formats and unavailable encoders retain
  their original representation. The same FFmpeg worker pool handles them. HDR
  precision is preserved in native 10/16-bit formats; unsupported high-depth alpha
  fails explicitly rather than being quantized or losing transparency. Import
  still follows the existing MP4 contract; the engine's MOV alpha fixture tests
  the lower-level fallback, without extending accepted project formats.
* Audio remains compressed with codec parameters, packet timing and skip-sample
  metadata. Only the first 200 ms of float stereo audio is retained for initial
  preparation. There is no complete PCM track. The optional disk editing proxy
  described below is independent of this validated resident representation.
* Converted video/audio share one compact indexed payload. For original media,
  audio packets alias the source allocation when their container offsets permit
  it. Byte arrays and indexes are squeezed to useful capacity. The original RAM
  allocation is released after successful conversion verification; disk is untouched.

Admission checks allocation growth throughout validation, including temporary
original/conversion coexistence, vector reallocation and codec scratch. The
reported conversion peak is a conservative reservation, not a sampled physical
peak. Explicit conversion is slower and compressed storage can grow substantially; the
main saving is elimination of per-occurrence decoders and secondary video copies.
No production path launches an external FFmpeg process.

## Cursors, scheduling and readiness

`ResidentVideoPlayer` preserves the existing QML/transport facade. Its
`PlaybackCursor` contains timing, a generation, current image and lookahead,
without a `QMediaPlayer` or source device per occurrence. `DecodeScheduler` owns
`max(1, min(4, cores / 2))` workers. It coalesces identical source/frame requests,
prioritizes playback, scrubbing, preparation, prefetch, visible thumbnails and
offscreen thumbnails in that order, and cancels subscribers by owner/generation.
When the pool has more than one worker, optional prefetch and thumbnail jobs can
occupy at most all but one of them. Source affinity prefers a worker whose live
decoder session has already decoded a nearby earlier frame of the same source.
It tracks the last successful source decode; a disk/cache hit does not move that
position. Failed decodes invalidate it, and an expired worker session cannot
advertise reusable history. Obsolete seeks are interrupted cooperatively inside
FFmpeg packet/frame loops.

Scrubbing keeps one request in flight and coalesces pointer updates with a 16 ms
dispatch interval. After idle time, the leading request is scheduled immediately;
otherwise only the remaining interval is delayed. It first tries a cached editing
image for the requested source frame; a miss decodes the original.

On a cold long-GOP source, a completed image behind the moving pointer can provide
intermediate progress only after at least 100 ms without a new presentation and
while its request is at most 1,000 ms old. It must belong to the current direction
epoch and move monotonically toward the pointer within that epoch. Reversing
direction rejects pending results from the previous direction. These bounds use
elapsed wall-clock time, not a one-second distance on the source timeline. An
intermediate image cannot satisfy exact preparation for a different frame.
Releasing the scrub gesture cancels the old generation and requests the exact
full-resolution image and lookahead. Play and remote scene preparation also use
exact full-resolution frames. Preview images have separate cache identities and
cannot satisfy those readiness checks.

Each worker reuses a codec context for all-intra images. An original inter-frame
source instead uses one cached demux/decoder session per worker, preserving
reference history during sequential access and seeking for backwards/distant
requests. Sessions expire with idle workers; their count cannot exceed the pool.
One additional low-priority worker creates optional editing images, so the video
playback/editing pools have at most four plus one decoder sessions. The serialized
full-validation job and the audio workers are separate from those pools.
Decoding currently uses FFmpeg software contexts, so CPU cost can exceed the old
platform hardware decoder. See the measured tradeoffs in the validation report;
historical all-intra benchmarks do not qualify scrubbing on retained long-GOP
originals.

Open canvases retain prepared cursors and clip entry images. Paused positions warm
their current frame, two subsequent frames and bounded audio. On retained original
video, lookahead requests the next frame before requesting the second, so the
decoder can reuse the same GOP history. Required paused lookahead has preparation
priority; starting or active playback gives it playback priority. All-intra
lookahead can run independently. Entry images remain pinned separately from the
moving cursor. Shared images have one allocation per file/frame/format and remain
alive while required by any cursor or renderer.
Play checks current versioned readiness; already prepared frames are reused and
future clips do not block local scene start. Unprepared seeks can require a short
residual wait. Source/timing/position/output changes invalidate their affected
resources; generation guards reject results after deletion or cancellation.

## Audio and rendering

One `QAudioSink` callback mixer serves each audio device. Per-occurrence
`QAudioOutput` objects remain lightweight volume/mute/device controls only. Each
voice has four preallocated 50 ms float blocks. Two background workers decode and
resample compressed audio, including AAC priming/trimming and shifted timestamps.
The real-time callback mixes published blocks without decoding, allocation,
blocking locks or file/network I/O. Voice retirement protects callback lifetime;
output changes rebuild affected voices. The scene clock remains authoritative;
a continuous device sample clock is corrected gradually for drift.

Local and remote `RemoteVideoFrameItem` nodes consume the same native
`QVideoFrame` planes. The hardware scene graph uses Qt's YUV/HDR shaders and shares
texture planes per QRhi and immutable frame identity. Different graphics devices
get separate textures. Static images/thumbnails share textures per window and
pixel identity, with weak caches. The software renderer alone converts to RGBA.
Temporary `presentationFrame` wrappers prevent `toImage()` from retaining an extra
RGBA copy on shared native frames. Qt Multimedia private rendering APIs are pinned
to the existing Qt 6.11.2 build dependency.

## Caches and accounting

Timeline cells are anchored to source time, including a clip's trimmed source
offset. Their temporal step uses powers of two with overlapping zoom thresholds
(hysteresis), rather than selecting a new sample at every pixel-scale change.
Cell intervals fill the strip continuously; the image is cropped at clip and
viewport boundaries without squeezing an edge thumbnail. Variable-frame-rate
selection maps the nominal cell time to the frame index without moving the cell
to that frame's presentation timestamp.

Thumbnail magnification depends only on the strip height, never on the temporal
cell width. Narrow cells crop horizontally; wider cells repeat the same sample
in bounded image quads. Zoom therefore reveals/repeats horizontal content without
enlarging it vertically or cutting off its top and bottom. Each cell owns a reused
node group, and all source rectangles stay inside their texture, including atlases.

Visible thumbnails are requested before the one-viewport margin on either side.
Useful subscriptions survive scroll and zoom. Existing displayed images remain
pinned until their replacements arrive, with the nearest available image from
the same source or an import thumbnail as fallback. The per-strip visible pixel
budget is 2 MiB; weak handles retain no additional offscreen images. The optional
global LRU limits are 32 MiB for thumbnails and 64 MiB for reusable video frames;
neither is preallocated. Required cursor/entry/render frames and the bounded
visible-strip pins can outlive an LRU eviction and exceed its retained-byte limit.

An ordinary cache purge keeps displayed fallback images. Disabling optional
allocation under memory pressure also releases visible-strip pins and cancels
optional work; requests resume after admission recovers. Source changes and
failed imports clear their old images immediately. `hasThumbnails` reflects
actual pixels, not merely the existence of a video index. Failed thumbnail
requests complete explicitly; destroyed strips cancel their pending work.

`MediaPreviewStore` adds two independent disk caches under Qt's application
`CacheLocation/media-previews-v1`: **64 MiB** for thumbnails and **512 MiB** for
scrub images, each also limited to **12,000 files**. Versioned keys include the
original hash, video track, source frame timestamp/duration, geometry and
rotation. All reads and writes happen on workers; writes are atomic. Missing,
invalid or evicted derivatives fall back to decoding the original and can be
regenerated. These are disposable local caches, not project assets or files sent
to remote receivers. JPEG thumbnail storage preserves the alpha fallback by
skipping sources whose transparency would be lost.

`EditingProxyCache` creates independently readable JPEG images, with a maximum
long edge of **960 pixels**, for retained original video that is SDR, at most
8-bit, and has no alpha. It skips the explicit all-intra representation, HDR,
high-depth/alpha formats and formats without usable metadata; these continue
through the original/native path. Its single low-priority FFmpeg worker starts
only for admitted resident players and processes batches of at most 32 frames.
It pauses cooperatively during scrubbing, Play or memory-pressure suspension.
After a gesture, a 32-frame focus window around the last requested position takes
priority over the continuing scan. This revisits useful regions of long clips
whose earlier images have already exceeded the disk budget; a complete proxy is
neither required nor guaranteed. Releasing the last owner cancels that source's
job. Idle worker expiry releases its cached decoder/source lease after one
second; no decoder is retained per clip.

The default minimum system reserve is **512 MiB**. Configure
`MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB` and optional
`MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT` in `client/.env` or an external env file.
The effective reserve is the larger of the fixed minimum and total RAM times the
percentage. Rebuild for embedded env changes; restart for external changes.
Memory is ordinary pageable RAM: the OS may compress or swap it.

A reserve deficit preserves already-ready media and defers/cancels new loading.
Critical pressure retains the coordinated scene-stop/reclamation policy. The
one-second monitor and native notifications share the same admission/hysteresis
rules. Only one full validation job runs at a time. Pending playback reservations
cover the bounded decoder pool, one shared proxy-decoder allowance for supported
sources, worst-case distinct cursor frames and 200 ms of audio; unused
reservations are retired after preparation. These estimates are
not multiplied full decoder budgets per clip and are not counted twice against
fresh OS available-memory measurements.

The memory summary exposes overlapping measurements separately:

* **Shared stored data**: compressed runtime video/audio/indexes and decoded static
  images, counted once per original identity.
* **Tracked CPU buffers**: actual native frame lifetimes (including reduced scrub
  frames and frames still held by renderers), thumbnail pixels retained by the
  LRU or visible strips, asset import/static thumbnails, initial audio and
  per-voice PCM. Asset posters are a subset, not an additional total.
* **Decoder/graphics estimate**: a conservative pool/surface estimate, separately
  from outstanding preparation reservations. Driver allocations are opaque.
* **Available memory estimate**, configured reserve and remaining load budget.
* **Process footprint**: macOS `phys_footprint`; resident set is reported separately.
  Free plus inactive pages estimates macOS availability.

The popup restores the horizontal Mouffette/system/available overview and its
color legend, with plain preparation totals in place of metric cards. This is
explicitly an estimated distribution: available RAM is bounded by physical RAM,
the process share is bounded by the remaining occupied portion, and the system
share is the remainder. It does not add overlapping media/decoder counters to
the process measurement. All asset details remain in the same scrolling list.

`mediaBytes` remains the sum of asset-owned disjoint categories for compatibility:
video, static pixels, initial video frames, import/static thumbnails and initial audio.
`sharedStoredBytes` is the narrower stored-data total. Global caches and live
playback buffers are additional tracked CPU allocations. A per-asset conversion
peak and representation reason make storage growth visible.
`optionalThumbnailBytes` reports only the LRU's retained pixels;
`thumbnailBufferBytes` tracks their full shared lifetime, including visible-strip
pins after LRU eviction. `cpuBufferBytes` uses the latter once, rather than adding
both overlapping counters. Disk JPEG sizes are storage usage, not resident RAM.

## Remote protocol and packaging

Protocol v12, project schema and original-file transfers are unchanged. Residency
snapshots remain session-generation-bound and sequenced. Durable upload completion
is separate from background media readiness. Remote PREPARE pins ready data and
prepares requested start cursors; stale readiness cannot launch a partial scene.
The receiver renders native shared planes without an RGBA image per video frame.
After a transient transport degradation, the client reconciles session state and
the server replays outstanding scene barriers. Repeated PREPARE preserves primed
players, RAM pins and the preparation deadline; recovery cannot substitute an
editing preview for the original start frame or revive an expired scene. This
recovery path requires the corresponding client and server changes; see
[scene run delivery](../../server/README.md#scene-runs).

Qt 6.11.2 and linked avformat/avcodec/avutil/swscale/swresample are required. Native
package scripts retain the pinned Qt multimedia plugin for device support and the
legacy comparison harness. The application itself no longer instantiates the old
per-clip Qt playback engine. The manual benchmark keeps an explicit Qt baseline;
Windows qualification and physical audiovisual latency measurements remain release
gates described in [media engine validation](MEDIA_ENGINE_VALIDATION.md).

## Tests

`ResidentMedia` covers indexed payload sharing, independent intra frames, variable
PTS, SAR/rotation, full decode, corruption, AAC samples, memory-only playback,
rapid seeks, repeated Play and late cancellation/deletion. Optional generated
fixtures cover 4K/HDR/alpha. Repeated cursor cycles check live allocations return
to their baseline. `MediaResidencyManager` injects deterministic memory snapshots
for reservations, deduplication, cache reclamation and protected scenes. Its
preview tests stop validation at the first image, check immutable shared pixels
and a valid native poster before readiness, and cover corrupt tails, source
replacement, cancellation, owner rebinding and pressure cleanup. Timeline strip
tests cover source anchoring, zoom/LOD stability, fallback continuity and bounded
visible memory; scheduler/proxy tests cover request priority, cancellation,
derivative reuse and exact full-resolution recovery. Native
`MediaFrameItem` tests exercise YUV, alpha and rotation on the GPU. Canvas, remote
lifecycle, overlay and bootstrap suites cover integration. Benchmark procedures,
raw measurements and limits are in the validation report.

A development measurement on 20 September 2026, using
`VID_20260920_013247.mp4`, observed the first timeline import preview at **192 ms**
and strict readiness at **6,357 ms**. This was one Debug/offscreen run on macOS
26.1 with Qt 6.11.2 during the progressive-preview implementation; it did not
measure mouse-to-screen latency or purge OS caches. It measures the early
thumbnail publication path, before the subsequent canvas-poster integration.
Cold long-GOP scrubbing still depends on original decoding until useful proxy
frames exist. Windows packaging/performance, native display latency and physical
audio/video synchronization require their own qualification; neither this run
nor older full-resolution all-intra tables establish those results.

The final [interaction implementation and validation report](MEDIA_INTERACTION_IMPLEMENTATION_2026-09-20.md)
adds a native canvas-preview check and an isolated cold/warm benchmark of the
current retained-original path, with raw measurements for the supplied long-GOP
video and a synthetic 4K fixture. It explicitly separates sink delivery, full
quality refinement and physical display latency.

## Inactive project media

`MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS=0` keeps all open canvases prepared,
including during inactivity. Project closure and critical memory pressure retain
their own cleanup rules. Set a positive timeout to opt into releasing media RAM
while inactive and rebuilding its representation on return. The optional policy
uses the same activity source as session disconnection and project retention: a visible control window
with the pointer inside keeps all projects active. Pointer departure, hiding the
window or system suspension starts their deadlines; repeated inactive events do
not extend them. A positive media delay must be at least 1,000 ms and less
than project retention. It is independent of the remote session delay.

`ProjectManager` owns this transient deadline, polls it alongside project deletion
and reports expiry once per inactive interval. `ApplicationRuntime` applies the
result to each workspace's `CanvasDocument`. The document suspends file leases,
cancels outstanding metadata imports and retains their durable intent. Each
`CanvasMedia` clears its decoded image/video resources, playback queues and frame
references, and fences queued acquisitions until activity resumes. Source files,
media IDs, geometry, settings and preview cursors remain intact. On return, the
same media nodes reacquire residency asynchronously through the normal validation
and memory-admission path; scene actions remain gated on readiness.

Scene preparation and playback keep their leases until the canvas restores its
draft and unlocks. An expired media deadline then applies immediately; activity
resumption before the scene ends cancels it. Assets shared with another active
project or an incoming session stay resident for those owners. This local deadline
does not close a remote session or remove its receiver's cache; their existing
session teardown policy continues to apply.

When RAM-only expiry is enabled, the client list shows `Free RAM in m:ss`
alongside the session and project deadlines. Timer values use the shared monospace font while labels retain the UI
font. Expired RAM countdowns disappear, including when a scene defers reclamation.
`AppConfig`, `ProjectManager`, `ClientInfoDisplay`, `ClientConnectionFlow` and
canvas/video lifecycle tests cover configuration, boundary expiry, cancellation,
shared assets, pending imports, scene protection and rehydration.
