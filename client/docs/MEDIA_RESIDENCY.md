# Resident media and bounded video playback

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
Those surfaces are created when resident content is available, after the first host
skeleton frame; the passive remote renderer does not have that presentation gate.
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
notifications enforce a reserve of max(20% physical RAM, 2 GiB) by default.
Configure `MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT` and
`MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB` in `client/.env` (or an external env file
selected with `--env-file` / `MOUFFETTE_ENV_FILE`). Rebuild after changing the
embedded `client/.env`; external env files only require an application restart.
The effective reserve is the larger of the percentage and the minimum, and is
shown in the RAM popup. The extra reload headroom remains max(512 MiB, 5% RAM). The macOS available
value is an explicitly labelled estimate (free plus inactive pages). Process RAM
and controlled media allocations are separate measurements and need not match.

Only one full validation job runs at a time. It reserves estimated final storage plus
codec/conversion scratch, and checks growth before allocation. Playback budgets
are admitted separately per independent player, including atomic scene admission.
They are conservative estimates of codec, queue and rendering overhead, not a
measurement of allocated RAM. Qt controls its streaming queues; the system monitor
remains the authority for actual pressure. Unprotected media
are evicted largest first; all referring occurrences become skeletons. Scene
leases protect data from PREPARE until stop/teardown. Persistent pressure first
requests a coordinated stop before those leases can be reclaimed. Reloading waits
for two healthy samples and max(512 MiB, 5% RAM) extra headroom, and never evicts
another ready asset merely to retry a waiting asset.

The RAM popup next to Settings shows process/system/available RAM, media bytes,
additional preparation budgets, estimated playback budgets, system reserve, and
per-asset local/remote state. Retained bytes exclude scratch and future allocations;
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
