# Network v12 / policy v5 validation

Validated locally on macOS arm64 with Qt 6.11.2. Server and clients must be
updated together. This implementation has not been deployed or packaged here.

## Results

- Full client build and `git diff --check` pass.
- All server scripts in `npm --prefix server test` pass, including the v12
  reliability regressions and real WebSocket upload integration.
- The constrained-link matrix passes all 18 cases: 64, 256 and 512 KiB/s,
  uplink/downlink, and 1, 32 or 256 files. Each case transfers 1 MiB and verifies
  every SHA-256. No upload chunks entered control; the largest measured heartbeat
  gap was 770 ms. The probe uses real TCP shaping and a synthetic Node recipient;
  native disk and renderer behavior are covered separately by Qt tests.
- The real Qt/Node `RemoteSessionIntegration` suite passes, including durable
  upload resume, concurrent opposite-direction transfer, retained Live rendering,
  4/6-second recovery and terminal expiry beyond the default fifteen seconds.
- Final `ConnectionManager`, `ClientConnectionFlow`, `NetworkDiagnostics`,
  `UploadRemovalSecurity`, `RemoteCacheStore` and `RemoteSceneLifecycle` reruns pass.
  These cover conservative immutable deadlines, transport retirement when a
  configured recovery budget is shorter than transport timeout, lost PREPARE/
  PREPARED/STARTED acknowledgements, new-session upload IDs, cancellation without
  resurrection, cache restart/expiry/isolation/corruption/cap, failed physical
  deletion after durable invalidation, and bounded diagnostics with unavailable
  storage.

All 47 CTest suites were exercised. The initial offscreen run passed 38 suites;
the remaining failures were resolved and checked individually as follows:

| Suites | Final verification |
| --- | --- |
| ConnectionManager, ClientConnectionFlow, UploadRemovalSecurity, RemoteSceneLifecycle | Fixed regressions or asynchronous fixture expectations; rebuilt and rerun successfully offscreen. |
| MediaFrameItem, TextOutlineItem, TextOutlineMotion, TextItemQmlScaled | Complete suites pass with native macOS rendering; their offscreen run lacked the required rendering backend. |
| CanvasSelectionBackend | 191 cases passed offscreen, two were skipped, and both failing video-resize pixel cases pass natively. |

This is combined verification across rendering backends, not a single all-green
native CTest invocation. Native interaction tests can wait on desktop window
activation; the interaction suites pass offscreen. No unrelated rendering
production code was changed to satisfy those checks.

## Reproduction

From the repository root, after configuring the platform build:

```sh
npm --prefix server ci
npm --prefix server test
npm --prefix server run test:slow-link
cmake --build client/build -j 6
ctest --test-dir client/build --output-on-failure --timeout 300
```

Use the configured build directory when it differs from `client/build`. On macOS,
run GPU pixel checks with the native platform; `QT_QPA_PLATFORM=offscreen` is
suitable for network and interaction tests without window-activation dependency.

## Remaining external validation

Windows execution was not available on this host. The native CI workflow runs
server, constrained-link and Qt tests on Windows and macOS; it has not been
triggered or observed here. Physical suspend/wake and a physically full disk were
not induced: tests use suspend-inclusive clocks and controlled I/O failures.

Attributing a real incident requires the new JSONL logs from both clients and
the relay, correlated by session, upload, scene, generation and server boot.
See the [network guide](../src/backend/network/README.md) for locations, privacy
limits, configuration and protocol bounds. In particular, scene preparation has
an explicit 236 KiB content limit to preserve room in the bounded control queue.
