# Decoded media residency

Every image/video occurrence has an asynchronous residency lease. The process-wide
`MediaResidencyManager` owns immutable assets shared by SHA-256. Text does not
need a file lease. RAM state is transient; project references retain the source
and an optional `pendingImport` flag until the asynchronous identity is known.

Drop only performs asynchronous metadata discovery before inserting the exact-size
skeleton. Hashing, validation and complete decoding run outside the GUI thread.
Images retain decoded pixels; videos retain all video frames and the selected
full audio stream, including decoder drain at EOF. No partial video is ready.
`ResidentVideoPlayer` reads timestamped frames/PCM exclusively from these assets;
codec execution and source reads end before the ready state. Native CPU YUV
formats are retained where supported. Video RAM cost is therefore unrelated to
its compressed MP4 file size and can be many gigabytes.

The application uses ordinary pageable memory, not physical page locking. The OS
may compress or swap memory. A one-second monitor and native memory-pressure
notifications enforce a reserve of max(20% physical RAM, 2 GiB). The macOS available
value is an explicitly labelled estimate (free plus inactive pages). Process RAM
and controlled media allocations are separate measurements and need not match.

Only one full decoder runs at a time. It reserves estimated final storage plus
codec/conversion scratch, and checks growth before allocation. Unprotected media
are evicted largest first; all referring occurrences become skeletons. Scene
leases protect data from PREPARE until stop/teardown. Persistent pressure first
requests a coordinated stop before those leases can be reclaimed. Reloading waits
for two healthy samples and max(512 MiB, 5% RAM) extra headroom, and never evicts
another ready asset merely to retry a waiting asset.

The RAM popup next to Settings shows process/system/available RAM, media bytes,
reservations, reserve, and per-asset local/remote state. Errors are reported there
and through notifications, not overlays on loading media.

## Remote protocol

Protocol v5 adds session-generation-bound, sequenced `media_residency` snapshots
and a `media_memory_ready` preparation checklist stage. Upload completion still
means validated durable transfer, independently of background decoding. The
receiver warms assets after receipt, outside the scene preparation deadline.
Test launches require every file-backed medium in that canvas ready locally;
remote launches additionally require current receiver readiness. PREPARE pins
already-ready data atomically. A stale readiness report fails preparation without
starting a partial scene or scheduling an automatic retry of the launch.

Upgrade the server and both endpoints together. Protocol v4 peers cannot join a
v5 session; existing saved projects remain readable. File format/animation rules
and the 64-megapixel image limit remain; the old cumulative 1 GiB image-only cap
is replaced by shared dynamic admission.

## Build and validation

Qt 6.11 and public FFmpeg libraries are required: avformat, avcodec, avutil,
swscale and swresample. No external ffmpeg process is used by the application.
Native package scripts include directly linked FFmpeg runtimes and dependencies.

`ResidentMedia` tests generate small real MP4 fixtures for full decoding, delayed
frames, audio, timestamps, corruption and memory-only playback. `MediaResidencyManager`
tests inject memory snapshots for deterministic admission, deduplication, eviction,
hysteresis and protected-scene behavior. Canvas and remote lifecycle suites cover
skeleton interaction, launch gating and asynchronous transfer readiness. Run CTest
and the server's npm test suite after changing these contracts.
