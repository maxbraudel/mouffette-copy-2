# Shared media engine: validation and release gates

This is an implementation and measurement record, not a cross-platform release
certification. The production facade now uses the indexed FFmpeg engine. The
manual benchmark retains the old per-occurrence Qt path for comparison. Do not
remove that comparison harness or declare Windows qualification complete without
running the native matrix and the hardware checks below.

## Reproduce

Build the client normally, then explicitly build the opt-in harness:

```sh
cmake --build client/out/build/macos-debug --target media_engine_benchmark
python3 client/tests/benchmarks/run_media_engine_benchmarks.py \
  --binary client/out/build/macos-debug/media_engine_benchmark \
  --source video-1080p.mp4 --output /tmp/mouffette-media-benchmark --generate
```

Use a 1080p30 source at least 26 seconds long. The harness produces ten different
files from segments of that source, then compares one occurrence, ten occurrences
of one file, four different files and ten different files. They are distinct
identities, but not ten unrelated types of content. Generation uses the developer
FFmpeg CLI; the application uses linked libraries exclusively. `--shared-only`
reuses existing fixtures; `--fixtures-only` generates the optional test cases
without benchmarking. Raw JSON and stderr are separate files.

```sh
MOUFFETTE_ENGINE_FIXTURES=/tmp/mouffette-media-benchmark/fixtures \
  client/out/build/macos-debug/tst_ResidentMedia
ctest --test-dir client/out/build/macos-debug --output-on-failure
```

The native macOS/Windows workflow also generates the 4K, HDR and alpha cases
before CTest. The tiny committed timeline fixture supplies its basic segments;
those CI fixtures do not qualify 1080p throughput. Run `MediaFrameItem` using the
native platform plugin to exercise real QRhi rendering. The benchmark deliberately
uses an offscreen video sink, so its FPS measures delivered decoded frames, not
monitor refresh or full-scene GPU throughput.

## Measurement definitions

* `storedBytes`: shared compressed video/audio plus packet/index metadata. In the
  Qt baseline it is the original compressed file only. The baseline excludes old
  JPEG proxies, posters and thumbnails, making the comparison conservative for
  the old application's total memory.
* `preparedDecodeIncrementBytes`: macOS allocator bytes in use after prepared
  cursors minus bytes in use after source loading. This includes contexts,
  required decoded frames, audio and control overhead. It is not an exact
  accounting of FFmpeg's private reference surfaces alone.
* `preparedFootprintBytes`, `runningFootprintBytes`, `releasedFootprintBytes`:
  macOS `phys_footprint`. An allocator can retain freed pages, so footprint need
  not immediately return to baseline when tracked live allocations reach zero.
* `conversionPeakBudgetBytes`: maximum tracked data plus reserved scratch/growth
  during one asset's conversion. It is not a sampled physical peak.
* `playToNextFrameP95Ms`: 25 prepared Play/seek/pause trials, waiting for a new
  `QVideoSink` frame on every cursor. The already prepared start frame precedes
  that first new frame.
* `playToVideoSinkAndAudioCallbackP95Ms`: same new-frame condition plus consumption
  of PCM by each shared audio callback. Muted voices still consume their buffers.
  This does not measure physical display scanout or DAC latency.
* `deliveredFpsPerCursor`: three seconds of delivery; `playCpuPercent` measures
  process CPU during that interval, where 100% equals one logical core.
* `scrubP50Ms` / `scrubP95Ms`: 30 completed seek targets, including preparation of
  the audio cursor. This measures latency, not every pointer event during a drag.
* `trackedVideoBytesAfterRelease`: live native frame allocations after releasing
  cursors/assets, clearing optional caches and letting idle workers expire.

Heap statistics are currently macOS-specific. The Windows harness reports process
private bytes, and returns zero for unavailable heap measurements. Never interpret
those zeros as zero allocated RAM.

## M1 measurements, 20 September 2026

Native Debug build, Apple M1 / 16 GiB, macOS 26.1, Qt 6.11.2, FFmpeg 8.0_1.
Each case ran in a separate process with one audio mixer/device. Raw values are
in [the measurement record](benchmarks/media-engine-m1-2026-09-20.json).
MiB columns below use 1,048,576 bytes. Preparation increments include required
frames, decoder contexts and audio/device startup; they are measured allocator
increments, not a claim about codec internals alone.

| Scene | Qt preparation increment | Shared preparation increment | Reduction | Original stored data | Internal stored data |
|---|---:|---:|---:|---:|---:|
| One file, one occurrence | 46.0 MiB | 40.9 MiB | 11.1% | 4.0 MiB | 12.3 MiB |
| One file, ten occurrences | 454.5 MiB | 41.7 MiB | 90.8% | 4.0 MiB | 12.3 MiB |
| Four distinct files | 182.2 MiB | 88.7 MiB | 51.3% | 24.0 MiB | 70.1 MiB |
| Ten distinct files | 455.1 MiB | 142.8 MiB | 68.6% | 68.0 MiB | 188.9 MiB |

The single-occurrence case saves little preparation memory and its compressed
storage grows more than that saving. The large benefit comes from shared
occurrences and the bounded pool. Do not apply the 90.8% figure to total process
RAM or to arbitrary files.

| Scene | Shared Play P95, sink + audio callback | Shared scrub P95 | Qt scrub P95 | Shared minimum delivered FPS | CPU Qt / shared |
|---|---:|---:|---:|---:|---:|
| One occurrence | 26.8 ms | 11.5 ms | 324.6 ms | 29.84 | 5.6% / 30.9% |
| Ten shared occurrences | 27.6 ms | 27.1 ms | 1181.5 ms | 30.09 | 33.8% / 49.3% |
| Four distinct files | 27.2 ms | 21.3 ms | 460.7 ms | 30.09 | 15.2% / 121.1% |
| Ten distinct files | 27.9 ms | 33.1 ms | 1161.0 ms | 29.15 | 35.6% / 276.0% |

100% CPU means one logical core. Software intra decoding uses substantially more
CPU than the Qt hardware-decoding baseline. The ten-file trial delivered
29.15–29.85 FPS per cursor over roughly three seconds: this does **not** certify
sustained, lossless 30 FPS presentation in a complete scene. The measured Play
proxy is below 50 ms; physical audiovisual latency remains unmeasured.

All four shared cases reported zero tracked native frame bytes after releasing
cursors/assets, evicting optional caches and allowing workers to expire. In the
ten-file case, allocator bytes in use returned to about 1.1 MiB, while process
footprint remained about 415.9 MiB. Freed allocator pages can remain resident;
these are different measurements and this short test does not establish a
long-term process-footprint bound.

On the first 182-frame excerpt, all-intra conversion produced SSIM 0.994737
(Y 0.994931, U 0.993594, V 0.995102), comparing matching decoded frame order.
This is one sample, not a quality guarantee for other footage or HDR output.

## Supplied import regression

### Interactive import and thumbnail recovery correction

The current residency path retains original compressed video and creates its
frame index during the strict EOF validation pass. It no longer synchronously
transcodes every SDR frame and then decodes that conversion again. The conversion
benchmarks above describe the earlier all-intra representation; their seek/FPS
figures must not be applied to the current original-source path without remeasurement.
Worker count, shared occurrences, full-resolution decoding and source validation
remain unchanged. Long-GOP random access can cost more than intra decoding.

On the supplied file below, a same-session native Debug regression measured
66,180 ms to `ready` before this change and 7,437 ms afterwards, with real memory
admission. Both runs prepared the beginning, intermediate positions and the end.
These are individual measurements, not a general throughput guarantee.

A new GPU regression reproduced missing timeline images after optional-cache
eviction with no viewport change. Thumbnail strips now restart that work, and
readiness reflects available images. Another regression reproduced a failed
thumbnail decode that never called its completion; it now completes with an empty
result. Additional tests cover retained-source B-frames, VFR, rotation/SAR,
RAM-only backward/end seeking, cancellation and corrupt final packets.

Admission now estimates original storage plus index overhead rather than four
times the source size. A padded, valid MP4 regression verifies readiness with
enough RAM for the original and decode scratch, but not the discarded transcode
estimate. Native supplied-file tests also verify canvas reveal/Play and two
timeline occurrences with their complete thumbnail work drained before and after
cache reclamation. The targeted decoder, residency, GPU, playback, timeline and
remote lifecycle checks passed. The broader native interaction suite was stopped
because its fixture waited for macOS window activation; this run does not certify
the complete desktop input matrix.

### Earlier all-intra qualification

`VID_20260920_013247.mp4` is supplied separately and is not committed as a test
fixture. It contains 2,111 H.264 frames at 1080×1920 with variable timing over
about 71.5 seconds, and AAC starting about 726 ms after the video. The original
is 9.54 MiB; its indexed intra/audio representation is about 156.8 MiB. That
increase follows the chosen high-quality, independently decodable representation.

The native drop test passed import, full preparation, QML reveal, rendered-frame
capture, moving the media and Play/pause. The final complete test took 61.9 seconds
on this Mac. A separate residency test also reached `ready` with real memory
admission and prepared the beginning, intermediate positions and the end.
The source file remains unchanged.

A deterministic regression reproduced a stranded preparation: cancelling a scrub
before initial lookahead completed left empty entries whose cancelled callbacks
could never fill them. The cursor now removes those pending entries on generation
cancellation, preserves completed frames and requests missing frames again.
Lookahead decode errors are propagated explicitly. Progress includes internal
verification, and packet memory accounting no longer rescans the complete index
for every packet. Open canvases retain media by default; a positive
`MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS` explicitly opts into RAM-only inactivity
expiry and subsequent reconversion.

To qualify another supplied file without adding it to the repository:

```sh
MOUFFETTE_IMPORT_TEST_FILE=/absolute/path/video.mp4 \
  client/out/build/macos-debug/tst_MediaResidencyManager suppliedSourceFinishesPreparation
MOUFFETTE_TEST_VIDEO_FILE=/absolute/path/video.mp4 \
  client/out/build/macos-debug/tst_VideoPlaybackBackend droppedVideoFullyLoadsBeforePlayback
```

## Functional coverage

The engine tests verify compact shared packets, original SHA identity, independent
intra frames, variable frame timing, SAR/rotation, strict EOF validation, corrupt
packet recovery, delayed video, shifted/trimmed AAC samples, exact seek release,
rapid seek followed by Play, repeated playback of consumed audio buffers and late
results after cancellation/deletion. Thirty rounds of six cursors check live
frame allocations return to their initial level after release.

Optional fixtures verify full-resolution 3840×2160 SDR conversion, 10-bit PQ/BT.2020
retention and half-transparent alpha pixels through the original-source fallback.
The alpha MOV fixture exercises the lower-level engine: the public import contract
remains MP4-only. Metadata checks do not certify HDR output on an HDR monitor.
Native GPU tests compare YUV pixels and exercise alpha and rotation (16 native
cases passed). Full ResidentMedia, MediaResidencyManager, AppConfig, ProjectManager
and ClientConnectionFlow suites passed after the final implementation changes.
TimelineController and MediaOverlay passed at normal and doubled scale; navigation
and coordinated-pressure regressions passed targeted reruns. The complete native
48-suite matrix is not certified: window-order checks failed in this desktop
session, and software-only checkerboard tests require a real QRhi backend. Residency
checks simulate reserve deficits and critical pressure separately. Existing canvas,
remote-scene, project and server protocol tests cover integration.

## Remaining release gates

* Run the native Windows build/tests and exercise real audio device hotplug,
  mute/volume, long playback, clip boundaries and drift with recorded output.
* Measure Play to actual audiovisual presentation on the reference M1. Sink and
  callback observations alone do not establish the physical <50 ms P95 target.
* Qualify sustained ten-video rendering in complete local and remote scenes,
  with independent content and 4K/high-complexity material. Short sink throughput
  measurements cannot certify GPU presentation or a sustained show.
* Inspect converted quality on representative footage and HDR output on suitable
  displays. Preserve original files and keep original-format fallback explicit.
* Repeat process-footprint/CPU measurements over long edit/load/delete cycles and
  device changes, in addition to deterministic live-allocation tests.

Software all-intra decoding trades CPU and compressed storage for fewer decoder
contexts and fast independent seeks. The 32 MiB thumbnail and 64 MiB frame caches
are optional ceilings, not limits on required scene frames or total RAM. The
512 MiB system reserve is still an admission policy, not physical page locking.
