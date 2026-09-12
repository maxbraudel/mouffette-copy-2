# Mouffette Client

C++ Qt application for the Mouffette media sharing system.

## Prerequisites

### macOS
- Qt 6.11 or newer, including matching Qt Quick/Gui private headers
- CMake 3.25 or newer and Ninja
- Xcode Command Line Tools

```bash
# Install Qt6 via Homebrew
brew install qt cmake ninja
```

### Windows
- MSYS2 UCRT64 at `C:\msys64`
- CMake, Ninja, Qt 6.11+, OpenSSL and the matching MinGW toolchain

```powershell
C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-qt6-websockets mingw-w64-ucrt-x86_64-qt6-declarative mingw-w64-ucrt-x86_64-qt6-multimedia mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-openssl"
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

## Features (Phase 1)

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

## Usage

1. **Start the Server**: Make sure the Mouffette server is running on `localhost:8080`
2. **Launch the Client**: Run the built executable
3. **System Tray**: The app starts minimized to the system tray and auto-connects
4. **Access Interface**: 
   - **macOS**: Click the blue icon in the menu bar
   - **Windows**: Click the icon in the system tray (taskbar area)
   - **Tray Menu**: Right-click for quick options (Show/Hide, Quit)
5. **View Clients**: Open the main window to see other connected Mouffette clients
6. **Notifications**: Get tray notifications when clients connect/disconnect

### System Tray Features
- **Auto-start**: Connects to server automatically on launch
- **Background operation**: Runs silently in the background
- **Tray notifications**: Shows connection status and new client alerts
- **Quick access**: Click tray icon to show/hide main window
- **Context menu**: Right-click tray icon for menu options

## Architecture

Canvas selection has one authority in the C++ scene. QML consumes its selection
projection; one per-canvas edit session and one pointer coordinator own their
respective lifecycles. See [the input ownership contract and Qt regression tests](docs/QUICK_CANVAS_INPUT_COORDINATOR.md).

The text outline renderer uses the existing `TextEdit` document, cached glyph
masks and Qt's image-node/texture-atlas renderer. Its Qt-private document access
is isolated in `TextOutlineItem.cpp`;
build and package it with the same Qt version. See
[the rendering investigation and validation notes](docs/text-outline-rendering.md).

The client is built with:
- **Qt6**: Cross-platform UI framework
- **WebSocket**: Real-time communication with server
- **CMake**: Build system
- **C++17**: Modern C++ features

### Key Components

- `MainWindow`: Main UI and application logic
- `WebSocketClient`: Handles all server communication
- `ClientInfo`: Data structures for client information
- `ScreenInfo`: Represents display/monitor information

## Architecture Guardrails (Phase -2)

To protect migration boundaries before the Quick renderer swap:

- Backend must not include Quick/QML types.
- Orchestration paths (`backend/controllers`, `backend/handlers`, `backend/managers`) must not add new concrete canvas-renderer includes.

Run boundary checks:

```bash
./tools/check_architecture_boundaries.sh
```

Boundary ownership/rules documentation:

- `docs/ARCHITECTURE_BOUNDARIES.md`
- `tools/architecture_screen_canvas_allowlist.txt` (temporary legacy exceptions to shrink over migration)

## Next Steps (Phase 2)

The next phase will add:
- Media file selection and preview
- Drag & drop interface for screen positioning
- Real-time media streaming
- Media display on recipient screens
- Context menus for media options

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
