# Mouffette Client

C++ Qt application for the Mouffette media sharing system.

## Prerequisites

### macOS
- Qt 6.11.2, including matching Qt Quick/Gui/Multimedia private headers
- CMake 3.25 or newer and Ninja
- Xcode Command Line Tools

```bash
# Install Qt6 via Homebrew
brew install qt cmake ninja ffmpeg pkg-config
```

### Windows
- MSYS2 UCRT64 at `C:\msys64`
- CMake, Ninja, Qt 6.11+, OpenSSL, C++/WinRT headers and the matching MinGW toolchain

```powershell
C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-qt6-imageformats mingw-w64-ucrt-x86_64-qt6-websockets mingw-w64-ucrt-x86_64-qt6-declarative mingw-w64-ucrt-x86_64-qt6-multimedia mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-cppwinrt"
```

## Building

### macOS

```bash
./scripts/build-development.sh
./scripts/build-release.sh
./scripts/build-development.sh --clean
```

### Windows PowerShell

```powershell
.\scripts\build-development.ps1
.\scripts\build-release.ps1
.\scripts\build-development.ps1 -Clean -ConsoleLogs
```

Development and production builds never share a build directory or instance
coordination channel. Their primary profile deliberately keeps the existing
`Mouffette` application paths and `Mouffette/Client` QSettings identity.

### Selecting Qt explicitly

```bash
export MOUFFETTE_QT_ROOT=/path/to/Qt/6.11.2/macos
export MOUFFETTE_MACOS_DEPLOYMENT_TARGET=14.0
./scripts/build-release.sh
```

Only select an older macOS deployment target when every linked Qt/OpenSSL
framework was built for that target. Homebrew builds can be host-version-only;
the script therefore defaults to the active macOS version rather than claiming
unsupported compatibility.

On macOS, CMake also builds the Qt Darwin media plugin with a precise-seek
fix. Its pinned Qt source archive is downloaded once per build tree and
verified by SHA-256; the installed Qt is untouched. See
[the backend patch notes](docs/qt-darwin-seek.md) for offline builds and Qt upgrades.

## Running

### macOS

```bash
./scripts/run-development.sh
./scripts/run-release.sh
./scripts/run-packaged-release.sh
```

### Windows PowerShell

```powershell
.\scripts\run-development.ps1 -ConsoleLogs
.\scripts\run-release.ps1
.\scripts\run-packaged-release.ps1
```

`run-packaged-release` deliberately does not add the Qt/MSYS2 development directories to
`PATH`, so it validates the standalone staged application.

## Production packages

Compiling Release and producing a distributable are separate operations.
Packaging always rebuilds Release, runs all tests by default, creates a clean
staging tree, deploys runtime dependencies, validates the result, and writes a
versioned artifact plus SHA-256 checksum under `out/packages`.

### macOS DMG

```bash
./scripts/package-release.sh

# Public signed and notarized release:
export MOUFFETTE_MACOS_SIGN_IDENTITY="Developer ID Application: Example (TEAMID)"
export MOUFFETTE_NOTARY_PROFILE="mouffette-notary"
./scripts/package-release.sh --require-signing
```

Create the notary profile once with `xcrun notarytool store-credentials`.
Without credentials, `package-release.sh` creates an ad-hoc signed DMG suitable only
for local testing.

### Windows ZIP

```powershell
.\scripts\package-release.ps1

# Public Authenticode-signed release:
$env:MOUFFETTE_WINDOWS_CERTIFICATE = 'C:\secure\mouffette.pfx'
$env:MOUFFETTE_WINDOWS_CERTIFICATE_PASSWORD = 'set-outside-source-control'
.\scripts\package-release.ps1 -RequireSigning
```

The standalone trees and final packages are separate from all compiler output:

```text
out/build/<os>-<configuration>   compiler output
out/stage/<os>-release           clean standalone application
out/packages                     DMG/ZIP and checksums
```

See [the scripts reference](scripts/README.md) for the command matrix,
configuration precedence, multi-instance behavior and packaging options. The
[build and release operations guide](docs/BUILD_AND_RELEASE.md) contains the
complete release contract and validation checklist.

## Features

Clients can choose an optional username and profile picture in Settings.
Pictures are centered, cropped and converted locally to a 250 × 250 JPEG.
Only the owning client persists its profile; the server relays it in memory.
See [client profiles](docs/client-profiles.md) for lifecycle and compatibility.

- ✅ WebSocket connection to Mouffette server
- ✅ Client registration with machine name and screen info
- ✅ Real-time client discovery
- ✅ Clean Qt-based user interface
- ✅ **System tray integration** (menu bar on macOS, taskbar on Windows)
- ✅ **Background operation** with tray notifications
- ✅ Cross-platform support (macOS, Windows, Linux)
- ✅ Automatic reconnection handling
- ✅ Multi-screen detection
- ✅ **Auto-connect on startup**

Active Canvas sessions recover automatically when a peer becomes available.
See [session recovery](docs/remote-session-recovery.md) for the activity policy,
close barriers, and regression coverage.

The canvas shows the target's live mouse position through the active session.
See [remote cursor](docs/remote-cursor.md) for coordinate mapping, freshness,
regression coverage and the required client/server update.

Clients can opt in to live screen sharing in Settings. Their desktop then
appears inside the remote canvas's monitor rectangles, using bounded H.264
streams and the Qt Quick video renderer. Sharing is off by default. See
[screen sharing](docs/screen-sharing.md) for platform permissions, architecture,
transport limits and validation.

The primary selected media shows a transparency checkerboard in the authoring
canvas, including when hidden, fully transparent or outside its timeline clip.
Outside the clip, its content stays hidden while its first/last clip geometry
remains movable and resizable. Its opacity (50% by default)
and cell size (8 pixels by default) are configurable in `client/.env`. The grid
stays fixed in the viewport during pan, zoom and resize. See
[selected-media transparency](docs/transparency-checkerboard.md).

Dragging or trimming a timeline clip previews its provisional timing in the
canvas immediately, including video seeking and track order. The saved clip
changes only on release; cancelling the gesture restores the original preview.

Imported images and videos display a canvas skeleton and a pulsating timeline
background until their content is ready. Posters and thumbnails collected during
validation stay hidden. Local Play starts immediately with ready media; pending
media are absent from the playing canvas, and late videos join the current
playhead with synchronized audio. Per-media failures leave an error marker and
do not stop other local media. Remote scenes still require every media ready.
The filmstrip uses a source-time grid with zoom levels
and keeps displayed images until replacements arrive, avoiding empty cells during
zoom changes. Identical source/frame requests share allocations across occurrences.

Playback and exact positioning use a bounded FFmpeg pool of up to four workers.
One additional background worker prepares optional SDR editing images up to a
960-pixel long edge, pausing during interaction, playback or memory pressure.
Scrubbing uses these images when available and otherwise decodes the original;
the first request after idle is immediate, with later pointer updates coalesced
within a 16 ms dispatch interval. During a slow original decode, an intermediate
image can appear after 100 ms without a new presentation if its request is at
most 1,000 ms old and advances in the current drag direction. Direction changes
reject earlier pending results. Release and Play require exact full-resolution
frames; original-video lookahead prepares the next two frames sequentially using
the workers' actual decoder history. Disk caches are limited to
64 MiB for thumbnails and 512 MiB for editing images, with 12,000 files maximum
in each. HDR, alpha and unsupported proxy formats retain the native path. See
[media residency](docs/MEDIA_RESIDENCY.md) for cache and readiness contracts, and
[interaction corrections and measurements](docs/MEDIA_INTERACTION_IMPLEMENTATION_2026-09-20.md)
for the native validation and retained-original cold/warm benchmark.

## Usage

1. **Start the Server**: Make sure the Mouffette server is running on `localhost:8080`
2. **Launch the Client**: Run the built executable
3. **System Tray**: The app starts minimized to the system tray and auto-connects
4. **Access Interface**: 
   - **macOS**: Click the blue icon in the menu bar
   - **Windows**: Click the icon in the system tray (taskbar area)
5. **View Clients**: Open the main window to see other connected Mouffette clients
6. **Notifications**: View connection and operation notifications in the QML interface

### System Tray Features
- **Auto-start**: Connects to server automatically on launch
- **Background operation**: Runs silently in the background
- **Quick access**: Click tray icon to open or restore the main window and bring it to the front with focus

The control window stays above other application windows across desktops and
opens centered at 90% of the available screen width and height. See
[window presentation](docs/window-presentation.md) for platform behavior and validation.

## Media memory

Images and videos are completely validated before their content is revealed or
played. Local transport may run while other imports are still preparing; remote
scene execution requires complete readiness. Original files remain the save/transfer
identity; validated runtime video/audio packets stay in RAM. Optional disk
derivatives are disposable and do not replace those originals.
Lightweight cursors share decoding, native rendering frames and one audio mixer per
device. The RAM popup separates shared storage, tracked CPU buffers, pool/graphics
estimates, reservations, available RAM and process footprint. Visible thumbnail
buffers remain tracked after cache eviction, and shared preview posters are
counted once. The default system
reserve is 512 MiB. See [media residency](docs/MEDIA_RESIDENCY.md) and the
[validation report](docs/MEDIA_ENGINE_VALIDATION.md) for contracts and tradeoffs.

See [scene playback](docs/scene-playback.md) for timing, fades, end actions and
the editor controls' lifecycle during test and remote scenes.

## Architecture

The complete application interface is Qt Quick/QML. One
`QQmlApplicationEngine` loads the `ApplicationWindow`; C++ exposes typed
controllers, list models, and commands without locating or manipulating QML
controls. `QApplication` remains only for the native tray icon.

Canvas selection has one authority in `CanvasDocument`. QML consumes its
projection; one per-canvas edit session and one pointer coordinator own their
respective lifecycles. See [the architecture report](CLIENT_ARCHITECTURE_REPORT.md)
and [the input ownership contract](docs/QUICK_CANVAS_INPUT_COORDINATOR.md).

The text outline renderer uses the existing `TextEdit` document, cached glyph
masks and Qt's image-node/texture-atlas renderer. Its Qt-private document access
is isolated in `TextOutlineItem.cpp`;
build and package it with the same Qt version. See
[the rendering investigation and validation notes](docs/text-outline-rendering.md).
Fit sizing and remote text layout share document metrics and outline rounding;
see [the canvas/remote text investigation](docs/text-layout-parity.md) for the
wrapping incident, layout contract and regression coverage.

Images and remote video frames use source-sized scene-graph textures. Resizing
their canvas rectangles reuses those textures without allocating larger painted
surfaces. See [the image rendering incident and regression coverage](docs/media-frame-rendering.md).

The client is built with:
- **Qt 6.11 / Qt Quick**: Cross-platform UI and rendering framework
- **WebSocket**: Real-time communication with server
- **CMake**: Build system
- **C++17**: Modern C++ features

### Key Components

- `ApplicationController`: Non-visual presentation composition root
- `ApplicationRuntime`: Business, session, upload and system orchestration
- `CanvasDocument`: Renderer-independent canvas state
- `CanvasRoot.qml`: Local canvas presentation and interaction surface
- `WebSocketClient`: Handles all server communication
- `ClientInfo`: Data structures for client information
- `ScreenInfo`: Represents display/monitor information

## Architecture guardrails

The shipped application has no Widget/QGraphics UI, no `QQuickWidget`, no QSS,
and no alternate renderer. The only Widgets exception is the native tray.

Run boundary checks:

```bash
./tools/check_architecture_boundaries.sh
```

Boundary ownership and rules are documented in
[`docs/ARCHITECTURE_BOUNDARIES.md`](docs/ARCHITECTURE_BOUNDARIES.md).

## Troubleshooting

### Qt6 Not Found
If CMake can't find Qt6, make sure it's installed and set the correct path:
```bash
cmake .. -DCMAKE_PREFIX_PATH="/path/to/qt6"
```

### Connection Issues
- Ensure the server is running on `localhost:8080`
- Check firewall settings
- Verify WebSocket connectivity

### Build Issues
- Make sure you have all prerequisites installed
- Check CMake version (3.25+ required)
- Verify Qt6 installation
