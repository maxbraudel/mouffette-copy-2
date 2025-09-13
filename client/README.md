# Mouffette Client

C++ Qt application for the Mouffette media sharing system.

## Prerequisites

### macOS
- Qt6 (install via Homebrew)
- CMake
- Xcode Command Line Tools

```bash
# Install Qt6 via Homebrew
brew install qt@6 cmake

# Make sure Qt6 is in your PATH
export PATH="/opt/homebrew/opt/qt@6/bin:$PATH"
```

### Windows
- Qt6 (download from Qt website)
- CMake
- Visual Studio or MinGW

## Building

### Quick Build (macOS/Linux)
```bash
./build.sh
```

### Quick Build (Windows PowerShell)
```powershell
cd .\client
./build.ps1 -ConsoleLogs
```

### Manual Build
```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
```

### Finding Qt6 (if CMake can't find it)
```bash
# On macOS with Homebrew
cmake .. -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt@6"

# On other systems, specify your Qt installation path
cmake .. -DCMAKE_PREFIX_PATH="/path/to/qt6"
```

## Running

After building, you can run the client:

### macOS
```bash
cd build
./MouffetteClient.app/Contents/MacOS/MouffetteClient
```

### Linux/Windows
```bash
cd build
./MouffetteClient
```

### Windows (PowerShell, with logs)
```powershell
cd .\client
./run.ps1 -ConsoleLogs
```

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
- Check CMake version (3.16+ required)
- Verify Qt6 installation
