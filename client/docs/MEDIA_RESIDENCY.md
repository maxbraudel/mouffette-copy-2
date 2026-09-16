# Resident media and bounded video playback

Application bootstrap prepares the Qt media backend, decoder capabilities and
current audio devices before publishing `ApplicationController.ready`. The loading
window remains responsive; activation requests cannot reveal the main window early.
A failed preparation keeps startup on its retry screen. The startup probe does not
open media or retain a player, video sink, audio stream, decoder queue or polling timer.
Per-occurrence outputs still use the current audio device, including after hotplug.
The worker expires when idle; shutdown joins outstanding discovery before Qt teardown.

Every image/video occurrence has an asynchronous residency lease. The process-wide
`MediaResidencyManager` owns immutable assets shared by SHA-256. Text does not
need a file lease. RAM state is transient; project references retain the source
and an optional `pendingImport` flag until the asynchronous identity is known.
Accepted drops also receive an immediate media ID and an optional canvas
`pendingImports` record before metadata inspection. This preserves their source
identity and drop center across restart; changed or missing sources are rejected
on resume. A canvas with pending metadata imports cannot start a scene.

Drop performs geometry discovery in a dedicated, bounded worker pool before
inserting the exact-size skeleton. MP4 geometry comes from headers when complete;
only missing geometry requires an interruptible stream probe. Geometry inspection
does not initialize Qt multimedia or enumerate hardware codecs. Import cancellation,
source signatures and document generations still govern publication. Bulk hashing
and decoding cannot occupy the geometry pool.

The skeleton and its editable shell do not allocate image/video rendering surfaces.
For content that is still loading when its visual attaches, those surfaces are
created when resident content is available, after the first host skeleton frame,
and the content fades in. A visual attaching to already-resident content creates
its surface immediately and presents at full opacity, including after canvas page
navigation. Delegate creation and video-output rebinding are presentation events,
not residency transitions; neither may replay the loading reveal. A subsequent
loss of residency arms a new loading reveal. The passive remote renderer does not
have this presentation gate or loading fade; its scene owns its transitions.
Platform backend and audio device discovery also run outside the GUI thread, before
creating the first native video sink or QML VideoOutput. Volume, mute and cursor
changes are retained while it is pending, and the player receives its asset only
after its audio output exists. Deleting an occurrence discards its pending callback.
Hashing, validation and complete decoding run outside the GUI thread.
Images retain decoded pixels. Videos retain the **exact original compressed MP4**
(no transcoding) and one native-format poster frame, shared across occurrences.
Validation still decodes every video frame and the selected audio stream through
EOF, including delayed frames/audio drain, but discards the decoded data immediately.
The decoder reads the in-memory bytes it hashes; source mutation invalidates the job.
No partial or corrupt video is ready. Preparation cost is file size plus one poster
and bounded codec scratch, independent of duration/FPS except for compressed size.

`ResidentVideoPlayer` supplies a seekable, read-only `QBuffer` to Qt's streaming
player. Every occurrence shares the MP4 allocation but has independent playback
queues, audio, cursor and settings. Codecs run during playback/seeks, with native
hardware decoding where available. Playback never receives a filesystem URL.
Players prime their bounded decoder queues as residency becomes available, including
at cursor zero, so the first Play need not initialize the decoder. Opportunistic
preparation at zero can defer if its playback budget is unavailable. The macOS Qt plugin includes pinned memory-stream
fixes (UTI, metadata request completion, byte-range bounds) as well as precise seeks.

The application uses ordinary pageable memory, not physical page locking. The OS
may compress or swap memory. A one-second monitor and native memory-pressure
notifications enforce the configured reserve. The compiled fallback is
**548 MiB**; an embedded or external environment file can override it, including
with an explicit zero.
Configure `MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT` and
`MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB` in `client/.env` (or an external env file
selected with `--env-file` / `MOUFFETTE_ENV_FILE`). Rebuild after changing the
embedded `client/.env`; external env files only require an application restart.
Leave the optional percentage at `0` to use only the minimum MiB setting. A nonzero
percentage means a minimum free share of total physical RAM: the effective reserve
is `max(total RAM × percent / 100, minimum MiB)`, not a cap on media RAM. For example,
5% and 548 MiB reserve 819.2 MiB on a 16 GiB computer. There is no additional hidden
byte reserve when retrying a waiting import. The RAM popup shows the effective
reserve and the remaining budget for new media after outstanding reservations.
The macOS available value is an explicitly labelled estimate (free plus inactive
pages); speculative pages are already included in the free count. Process RAM and
controlled media allocations are separate measurements and need not match.
macOS pressure is sampled from the current system state as well as notifications,
so a past notification cannot leave admission blocked after pressure has recovered.
A native macOS warning is advisory: imports and playback may start when their
full preparation budgets fit above the configured reserve, including outstanding
reservations. A warning alone neither blocks the queue indefinitely nor discards
existing media or stops scenes. The loader still admits only one full validation
at a time and rechecks headroom during growth. Critical pressure or an actual
reserve deficit blocks allocations, cancels preparation and triggers reclamation,
even when both reserve settings are zero. Windows' low-physical-memory notification
is treated as a hard pressure constraint, not a macOS-style advisory warning.
Admission, recovery and reclamation share this distinction; recovery samples can
be healthy while an advisory warning remains. The available estimate is not a
guarantee of an allocation succeeding; a runtime budget rejection or caught
`std::bad_alloc` still defers loading through the same recovery path.

Only one full validation job runs at a time. It reserves estimated final storage plus
codec/conversion scratch, and checks growth before allocation. Playback budgets
are admitted separately per independent player, including atomic scene admission.
They are conservative estimates of codec, queue and rendering overhead, not a
measurement of allocated RAM. A player's preparation reservation remains pending
until its first decoded frame proves that its decoder has initialized. Only budgets
for pending players or pinned scene slots that still need players reduce admission
headroom. Once prepared, a player's allocations are reflected in the fresh system
measurement and its estimated budget is not subtracted a second time. The total
playback estimate (`playbackBudgetBytes`) remains visible separately from the
outstanding reservation (`pendingPlaybackBudgetBytes`). Qt controls its streaming
queues; the system monitor remains the authority for actual pressure. Unprotected media
are evicted largest first; all referring occurrences become skeletons. Scene
leases protect data from PREPARE until stop/teardown. Persistent pressure first
requests a coordinated stop before those leases can be reclaimed. Reloading waits
for two noncritical samples above the reserve at least one second apart, using
the same configured reserve as first-time admission, and never evicts another
ready asset merely to retry a waiting asset.

The RAM popup next to Settings shows process/system/available RAM, media bytes,
additional preparation budgets, estimated and pending playback budgets, system
reserve, the budget available for new media (`loadableBytes`), native pressure,
and per-asset local/remote state. Retained bytes exclude scratch and future allocations;
preparation reservations show only the remainder beyond those retained bytes.
Opaque platform decoder/GPU memory appears in process/system measurements, not as
fictional retained media allocations. Errors are reported there
and through notifications, not overlays on loading media.

## Remote protocol

Protocol v5 adds session-generation-bound, sequenced `media_residency` snapshots
and a `media_memory_ready` preparation checklist stage. Upload completion still
means validated durable transfer, independently of background decoding. The
receiver validates and caches assets after receipt, outside the scene preparation
deadline. Scene preparation then primes each requested video start frame from the
compressed cache; it must finish before launch.
Test launches require every file-backed medium in that canvas ready locally;
remote launches additionally require current receiver readiness. PREPARE pins
already-ready data and decoder budgets atomically. A stale readiness report fails preparation without
starting a partial scene or scheduling an automatic retry of the launch.

Upgrade the server and both endpoints together. Protocol v4 peers cannot join a
v5 session; existing saved projects remain readable. File format/animation rules
and the 64-megapixel image limit remain; the old cumulative 1 GiB image-only cap
is replaced by shared dynamic admission.

## Build and validation

Qt 6.11.2 and public FFmpeg libraries are required: avformat, avcodec, avutil,
swscale and swresample. No external ffmpeg process is used by the application.
Native package scripts include directly linked FFmpeg runtimes and dependencies,
and verify the streaming FFmpeg Qt plugin. On macOS and Windows, that plugin is
built from pinned Qt 6.11.2 sources, including when Qt only ships AVFoundation.
Its seek fixes account for reordered timestamps and variable frame durations.
It uses Qt's bounded video/audio queues (3 video frames plus one frame of
lookahead / 9 audio buffers in this version); codec reference surfaces, textures
and compressed packet queues add
platform-dependent overhead. The unrelated legacy QWindowCapture implementation
uses a removed macOS API and is unavailable in this private plugin; Mouffette
does not expose window capture. The supported native Darwin plugin remains bundled.

`ResidentMedia` tests generate small real MP4 fixtures for full decoding, delayed
frames, audio, timestamps, corruption and memory-only playback. `MediaResidencyManager`
tests inject memory snapshots for deterministic admission, deduplication, eviction,
hysteresis and protected-scene behavior. Canvas and remote lifecycle suites cover
skeleton interaction, launch gating and asynchronous transfer readiness. Run CTest
and the server's npm test suite after changing these contracts.
