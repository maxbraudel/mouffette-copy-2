# 🎯 Mouffette

**Cross-platform media sharing application for playful interactions**

Mouffette allows users to share and display media (images/videos) on other connected users' screens in real-time, creating fun interactions and "pranks" between friends and colleagues.

## 🌟 Features

- **Real-time Media Sharing**: Place images and videos on remote screens
- **Multi-Screen Support**: Target specific screens on multi-monitor setups  
- **Cross-Platform**: Works on Windows and macOS
- **Live Control**: Move and resize media in real-time
- **Auto-Disappear**: Set media to disappear after time or video playback
- **Background Operation**: Runs quietly in the background
- **Secure**: Direct peer discovery through central server

## 🏗️ Architecture

The system consists of two main components:

### 🖥️ Server (Node.js)
- Central WebSocket server for client discovery
- Handles real-time communication between clients
- Manages file transfers and media coordination
- Located in `/server/`

### 📱 Client (C++ Qt)
- Cross-platform desktop application
- Rich UI for client selection and media management
- Real-time media display and control
- Located in `/client/`

## 🚀 Quick Start

### 1. Start the Server

```bash
cd server
npm install
npm start
```

The server will start on `ws://localhost:8080`

### 2. Build and Run the Client

#### macOS (with Homebrew)
```bash
# Install dependencies
brew install qt@6 cmake

# Build client
cd client
./build.sh

# Run client
cd build
./MouffetteClient.app/Contents/MacOS/MouffetteClient
```

#### Windows
```bash
# Install Qt6 and CMake first
cd client
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2019_64"
cmake --build .
./MouffetteClient.exe
```

### 3. Connect and Share

1. Launch multiple clients on different machines
2. Click "Connect to Server" in each client
3. Select a target client from the list
4. Start sharing media! (Phase 2 feature)

## 📋 Current Status (Phase 1)

✅ **Completed Features:**
- Node.js WebSocket server with client discovery
- C++ Qt client with modern UI
- **System tray integration** (menu bar/taskbar operation)
- **Background operation with notifications**
- Real-time client registration and listing
- Multi-screen detection and reporting
- Cross-platform build system
- Automatic reconnection handling
- **Auto-connect functionality**

🚧 **Phase 2 (Coming Next):**
- Media file selection and drag-drop interface
- Real-time media streaming and positioning
- Media display on recipient screens
- Context menus for disappear options
- File transfer optimization

## 🛠️ Development

### Server Development
```bash
cd server
npm run dev  # Auto-restart on changes
```

### Client Development
```bash
cd client
./build.sh   # Rebuild after changes
```

### Project Structure
```
mouffette/
├── server/           # Node.js WebSocket server
│   ├── server.js     # Main server logic
│   ├── package.json  # Dependencies
│   └── README.md     # Server documentation
├── client/           # C++ Qt client application
│   ├── src/          # Source code
│   ├── ui/           # UI files
│   ├── CMakeLists.txt # Build configuration
│   ├── build.sh      # Build script
│   └── README.md     # Client documentation
└── README.md         # This file
```

## 📖 Usage Scenarios

### Typical Workflow (Phase 2)
1. **Alice** launches Mouffette and sees **Bob** connected
2. She selects Bob and views all his screens side-by-side
3. Alice drags an image onto Bob's primary screen
4. She sets it to disappear after 10 seconds
5. Alice clicks "Send" - the image appears on Bob's screen
6. Alice can move/resize the image in real-time
7. The image automatically disappears after 10 seconds

### Multi-Screen Example
- Target user has 3 monitors
- Sender sees all 3 screens scaled to fit their window
- Can place different media on each screen
- Single "Send" button shares all media across all screens
- Real-time control works across all screens simultaneously

## 🔧 Technical Details

### Communication Protocol
- **WebSocket** for real-time bidirectional communication
- **JSON** message format for all client-server communication
- **Binary transfer** for media files (Phase 2)

### Platform Support
- **macOS**: Native .app bundle with Qt6
- **Windows**: Native .exe with Qt6
- **Linux**: Planned for future release

### Dependencies
- **Server**: Node.js 16+, `ws`, `uuid`
- **Client**: Qt6, CMake 3.16+, C++17 compiler

## 🤝 Contributing

This is currently a personal project in early development. The architecture is designed to be modular and extensible for future enhancements.

## 📄 License

MIT License - See LICENSE file for details

---

**Built with ❤️ using Node.js, Qt6, and WebSocket technology**
