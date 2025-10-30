# 🏗️ PROJECT RESTRUCTURE PROPOSAL - Mouffette Client

## 📊 CURRENT STATE ANALYSIS

### Current Structure Issues
1. **Flat src/ directory**: 95+ files at root level - poor discoverability
2. **Inconsistent organization**: Some files organized (handlers/, managers/, ui/), most are not
3. **Missing domain separation**: Business logic, network, rendering mixed together
4. **Empty files**: MediaPreloader, ToastManager, UIManager, VolumeMonitor (technical debt)
5. **Unclear relationships**: Hard to understand dependencies between components
6. **Platform-specific files**: Mac*.mm scattered with cross-platform code

### What's Working Well ✅
- **handlers/**: Well-organized event handling (7 handlers)
- **managers/**: Good management layer (13 managers)
- **controllers/**: Clear control flow (2 controllers)
- **ui/pages/**: Dedicated UI pages (2 pages)
- **ui/widgets/**: Reusable UI components (3 widgets)

---

## 🎯 PROPOSED STRUCTURE

```
src/
├─ core/                         # Core application infrastructure
│  ├─ main.cpp
│  ├─ MainWindow.h
│  ├─ MainWindow.cpp             # [1,666 lines - your achievement! 🎉]
│  └─ AppColors.h/cpp            # Global color constants
│
├─ domain/                       # Business logic & domain models
│  ├─ models/                    # Data models
│  │  ├─ ClientInfo.h/cpp        # Client information structure
│  │  └─ ScreenInfo.h            # (if separate from ClientInfo)
│  │
│  ├─ session/                   # Session management
│  │  ├─ SessionManager.h/cpp    # Canvas session lifecycle
│  │  └─ CanvasSession.h         # Session data structure (if extracted)
│  │
│  └─ media/                     # Media domain logic
│     ├─ MediaItems.h/cpp        # Media item definitions
│     ├─ ResizableMediaItems.h   # Resizable media interfaces
│     ├─ MediaSettingsPanel.h/cpp # Media settings UI
│     └─ SelectionIndicators.h/cpp # Media selection visuals
│
├─ network/                      # Network & communication layer
│  ├─ WebSocketClient.h/cpp      # WebSocket connection management
│  ├─ UploadManager.h/cpp        # Upload/download protocol
│  ├─ WatchManager.h/cpp         # Screen watching protocol
│  └─ RemoteFileTracker.h/cpp    # Remote file state tracking
│
├─ files/                        # File system & storage
│  ├─ FileManager.h/cpp          # File operations façade
│  ├─ LocalFileRepository.h/cpp  # Local file ID mapping
│  ├─ FileMemoryCache.h/cpp      # In-memory file cache
│  ├─ FileWatcher.h/cpp          # File system monitoring
│  └─ Theme.h/cpp                # Theme file loading (if file-based)
│
├─ rendering/                    # Graphics & rendering
│  ├─ canvas/                    # Canvas rendering
│  │  ├─ ScreenCanvas.h/cpp      # Main canvas view
│  │  ├─ RoundedRectItem.h/cpp   # Rounded rectangle graphics
│  │  └─ OverlayPanels.h/cpp     # Canvas overlay panels
│  │
│  ├─ remote/                    # Remote scene rendering
│  │  └─ RemoteSceneController.h/cpp # Remote canvas mirroring
│  │
│  ├─ media/                     # Media rendering
│  │  └─ MediaPreloader.h/cpp    # Media preloading (currently empty)
│  │
│  └─ navigation/                # View navigation
│     └─ ScreenNavigationManager.h/cpp # Page navigation + animations
│
├─ ui/                           # User interface components
│  ├─ pages/                     # Full-page views
│  │  ├─ ClientListPage.h/cpp
│  │  └─ CanvasViewPage.h/cpp
│  │
│  ├─ widgets/                   # Reusable widgets
│  │  ├─ ClientListDelegate.h/cpp
│  │  ├─ ClippedContainer.h/cpp
│  │  ├─ RoundedContainer.h/cpp
│  │  ├─ SpinnerWidget.h/cpp
│  │  └─ QtWaitingSpinner.h/cpp  # Third-party spinner
│  │
│  ├─ layout/                    # Layout management
│  │  └─ ResponsiveLayoutManager.h/cpp
│  │
│  ├─ notifications/             # Notification system
│  │  ├─ ToastNotificationSystem.h/cpp
│  │  └─ ToastManager.h/cpp      # (currently empty - consolidate?)
│  │
│  ├─ theme/                     # Theming & styling
│  │  ├─ ThemeManager.h/cpp      # Theme management (already in managers/)
│  │  └─ StyleConfig.h           # Style constants
│  │
│  └─ UIManager.h/cpp            # (currently empty - remove or implement?)
│
├─ handlers/                     # Event handlers (KEEP AS-IS ✅)
│  ├─ ClientListEventHandler.h/cpp
│  ├─ ScreenEventHandler.h/cpp
│  ├─ UploadEventHandler.h/cpp
│  ├─ UploadSignalConnector.h/cpp
│  ├─ WebSocketMessageHandler.h/cpp
│  └─ WindowEventHandler.h/cpp
│
├─ managers/                     # Manager layer (REORGANIZE)
│  ├─ app/                       # Application-level managers
│  │  ├─ SettingsManager.h/cpp
│  │  ├─ SystemTrayManager.h/cpp
│  │  └─ MenuBarManager.h/cpp
│  │
│  ├─ ui/                        # UI-specific managers
│  │  ├─ TopBarManager.h/cpp
│  │  ├─ RemoteClientInfoManager.h/cpp
│  │  └─ UploadButtonStyleManager.h/cpp
│  │
│  ├─ network/                   # Network managers
│  │  ├─ ConnectionManager.h/cpp
│  │  └─ ClientListBuilder.h/cpp
│  │
│  └─ system/                    # System integration
│     └─ SystemMonitor.h/cpp
│
├─ controllers/                  # Controllers (KEEP AS-IS ✅)
│  ├─ CanvasSessionController.h/cpp
│  └─ TimerController.h/cpp
│
└─ platform/                     # Platform-specific code
   ├─ macos/                     # macOS implementations
   │  ├─ MacVideoThumbnailer.h/mm
   │  ├─ MacWindowManager.h/mm
   │  └─ NativeMenu.h/mm
   │
   └─ windows/                   # Windows implementations
      ├─ WindowsVideoThumbnailer.h/cpp
      └─ VolumeMonitor.h/cpp     # (currently empty)
```

---

## 📋 DETAILED MIGRATION PLAN

### Phase 1: Create Directory Structure ⚡ (5 minutes)
**Goal**: Establish new directories without moving files yet

```bash
# Core infrastructure
mkdir -p src/core

# Domain layer
mkdir -p src/domain/models
mkdir -p src/domain/session
mkdir -p src/domain/media

# Network layer
mkdir -p src/network

# File system
mkdir -p src/files

# Rendering layer
mkdir -p src/rendering/canvas
mkdir -p src/rendering/remote
mkdir -p src/rendering/media
mkdir -p src/rendering/navigation

# UI layer
mkdir -p src/ui/layout
mkdir -p src/ui/notifications
mkdir -p src/ui/theme

# Managers reorganization
mkdir -p src/managers/app
mkdir -p src/managers/ui
mkdir -p src/managers/network
mkdir -p src/managers/system

# Platform-specific
mkdir -p src/platform/macos
mkdir -p src/platform/windows
```

---

### Phase 2: Move Files by Category 🔄 (30-45 minutes)

#### 2.1 Core Infrastructure (3 files)
```bash
# Already in src/ - just note they stay
# src/main.cpp
# src/MainWindow.h
# src/MainWindow.cpp
mv src/AppColors.* src/core/
```

#### 2.2 Domain Models (2-3 files)
```bash
mv src/ClientInfo.* src/domain/models/
# ScreenInfo may be in ClientInfo.h - check first
```

#### 2.3 Domain Session (2 files)
```bash
mv src/SessionManager.* src/domain/session/
```

#### 2.4 Domain Media (8 files)
```bash
mv src/MediaItems.* src/domain/media/
mv src/ResizableMediaItems.h src/domain/media/
mv src/MediaSettingsPanel.* src/domain/media/
mv src/SelectionIndicators.* src/domain/media/
```

#### 2.5 Network Layer (8 files)
```bash
mv src/WebSocketClient.* src/network/
mv src/UploadManager.* src/network/
mv src/WatchManager.* src/network/
mv src/RemoteFileTracker.* src/network/
```

#### 2.6 File System (10 files)
```bash
mv src/FileManager.* src/files/
mv src/LocalFileRepository.* src/files/
mv src/FileMemoryCache.* src/files/
mv src/FileWatcher.* src/files/
mv src/Theme.* src/files/
```

#### 2.7 Rendering Layer (10 files)
```bash
# Canvas
mv src/ScreenCanvas.* src/rendering/canvas/
mv src/RoundedRectItem.* src/rendering/canvas/
mv src/OverlayPanels.* src/rendering/canvas/

# Remote scene
mv src/RemoteSceneController.* src/rendering/remote/

# Media rendering
mv src/MediaPreloader.* src/rendering/media/

# Navigation
mv src/ScreenNavigationManager.* src/rendering/navigation/
```

#### 2.8 UI Components (10 files)
```bash
# Layout
mv src/ResponsiveLayoutManager.* src/ui/layout/

# Notifications
mv src/ToastNotificationSystem.* src/ui/notifications/
mv src/ToastManager.* src/ui/notifications/

# Widgets
mv src/SpinnerWidget.* src/ui/widgets/
mv src/QtWaitingSpinner.* src/ui/widgets/

# Theme
mv src/ui/StyleConfig.h src/ui/theme/

# UIManager - DECISION NEEDED
# Option A: Delete if empty
# Option B: Move to src/ui/ if you plan to implement it
```

#### 2.9 Reorganize Managers (14 files)
```bash
# App-level managers
mv src/managers/SettingsManager.* src/managers/app/
mv src/managers/SystemTrayManager.* src/managers/app/
mv src/managers/MenuBarManager.* src/managers/app/

# UI managers
mv src/managers/TopBarManager.* src/managers/ui/
mv src/managers/RemoteClientInfoManager.* src/managers/ui/
mv src/managers/UploadButtonStyleManager.* src/managers/ui/
mv src/managers/ThemeManager.* src/ui/theme/  # Move ThemeManager to ui/theme

# Network managers
mv src/managers/ConnectionManager.* src/managers/network/
mv src/managers/ClientListBuilder.* src/managers/network/

# System managers
mv src/managers/SystemMonitor.* src/managers/system/
```

#### 2.10 Platform-Specific (8 files)
```bash
# macOS
mv src/MacVideoThumbnailer.* src/platform/macos/
mv src/MacWindowManager.* src/platform/macos/
mv src/NativeMenu.* src/platform/macos/

# Windows
mv src/WindowsVideoThumbnailer.* src/platform/windows/
mv src/VolumeMonitor.* src/platform/windows/
```

---

### Phase 3: Update Include Paths 🔧 (60-90 minutes)

**Strategy**: Use `grep` to find all includes, then batch-replace

#### 3.1 Generate Include Map
```bash
# Create a mapping file for search/replace
cat > /tmp/include_updates.txt << 'EOF'
"AppColors.h" → "core/AppColors.h"
"ClientInfo.h" → "domain/models/ClientInfo.h"
"SessionManager.h" → "domain/session/SessionManager.h"
"MediaItems.h" → "domain/media/MediaItems.h"
"ResizableMediaItems.h" → "domain/media/ResizableMediaItems.h"
"MediaSettingsPanel.h" → "domain/media/MediaSettingsPanel.h"
"SelectionIndicators.h" → "domain/media/SelectionIndicators.h"
"WebSocketClient.h" → "network/WebSocketClient.h"
"UploadManager.h" → "network/UploadManager.h"
"WatchManager.h" → "network/WatchManager.h"
"RemoteFileTracker.h" → "network/RemoteFileTracker.h"
"FileManager.h" → "files/FileManager.h"
"LocalFileRepository.h" → "files/LocalFileRepository.h"
"FileMemoryCache.h" → "files/FileMemoryCache.h"
"FileWatcher.h" → "files/FileWatcher.h"
"Theme.h" → "files/Theme.h"
"ScreenCanvas.h" → "rendering/canvas/ScreenCanvas.h"
"RoundedRectItem.h" → "rendering/canvas/RoundedRectItem.h"
"OverlayPanels.h" → "rendering/canvas/OverlayPanels.h"
"RemoteSceneController.h" → "rendering/remote/RemoteSceneController.h"
"MediaPreloader.h" → "rendering/media/MediaPreloader.h"
"ScreenNavigationManager.h" → "rendering/navigation/ScreenNavigationManager.h"
"ResponsiveLayoutManager.h" → "ui/layout/ResponsiveLayoutManager.h"
"ToastNotificationSystem.h" → "ui/notifications/ToastNotificationSystem.h"
"ToastManager.h" → "ui/notifications/ToastManager.h"
"SpinnerWidget.h" → "ui/widgets/SpinnerWidget.h"
"QtWaitingSpinner.h" → "ui/widgets/QtWaitingSpinner.h"
"StyleConfig.h" → "ui/theme/StyleConfig.h"
"ThemeManager.h" → "ui/theme/ThemeManager.h"
"SettingsManager.h" → "managers/app/SettingsManager.h"
"SystemTrayManager.h" → "managers/app/SystemTrayManager.h"
"MenuBarManager.h" → "managers/app/MenuBarManager.h"
"TopBarManager.h" → "managers/ui/TopBarManager.h"
"RemoteClientInfoManager.h" → "managers/ui/RemoteClientInfoManager.h"
"UploadButtonStyleManager.h" → "managers/ui/UploadButtonStyleManager.h"
"ConnectionManager.h" → "managers/network/ConnectionManager.h"
"ClientListBuilder.h" → "managers/network/ClientListBuilder.h"
"SystemMonitor.h" → "managers/system/SystemMonitor.h"
"MacVideoThumbnailer.h" → "platform/macos/MacVideoThumbnailer.h"
"MacWindowManager.h" → "platform/macos/MacWindowManager.h"
"NativeMenu.h" → "platform/macos/NativeMenu.h"
"WindowsVideoThumbnailer.h" → "platform/windows/WindowsVideoThumbnailer.h"
"VolumeMonitor.h" → "platform/windows/VolumeMonitor.h"
EOF
```

#### 3.2 Automated Include Update Script
```bash
#!/bin/bash
# save as: update_includes.sh

# Find all .h, .cpp, .mm files in src/
find src -type f \( -name "*.h" -o -name "*.cpp" -o -name "*.mm" \) -print0 | while IFS= read -r -d '' file; do
    echo "Processing: $file"
    
    # Update includes (using sed - adjust syntax for your system)
    sed -i '' \
        -e 's|#include "AppColors.h"|#include "core/AppColors.h"|g' \
        -e 's|#include "ClientInfo.h"|#include "domain/models/ClientInfo.h"|g' \
        -e 's|#include "SessionManager.h"|#include "domain/session/SessionManager.h"|g' \
        -e 's|#include "MediaItems.h"|#include "domain/media/MediaItems.h"|g' \
        -e 's|#include "ResizableMediaItems.h"|#include "domain/media/ResizableMediaItems.h"|g' \
        -e 's|#include "MediaSettingsPanel.h"|#include "domain/media/MediaSettingsPanel.h"|g' \
        -e 's|#include "SelectionIndicators.h"|#include "domain/media/SelectionIndicators.h"|g' \
        -e 's|#include "WebSocketClient.h"|#include "network/WebSocketClient.h"|g' \
        -e 's|#include "UploadManager.h"|#include "network/UploadManager.h"|g' \
        -e 's|#include "WatchManager.h"|#include "network/WatchManager.h"|g' \
        -e 's|#include "RemoteFileTracker.h"|#include "network/RemoteFileTracker.h"|g' \
        -e 's|#include "FileManager.h"|#include "files/FileManager.h"|g' \
        -e 's|#include "LocalFileRepository.h"|#include "files/LocalFileRepository.h"|g' \
        -e 's|#include "FileMemoryCache.h"|#include "files/FileMemoryCache.h"|g' \
        -e 's|#include "FileWatcher.h"|#include "files/FileWatcher.h"|g' \
        -e 's|#include "Theme.h"|#include "files/Theme.h"|g' \
        -e 's|#include "ScreenCanvas.h"|#include "rendering/canvas/ScreenCanvas.h"|g' \
        -e 's|#include "RoundedRectItem.h"|#include "rendering/canvas/RoundedRectItem.h"|g' \
        -e 's|#include "OverlayPanels.h"|#include "rendering/canvas/OverlayPanels.h"|g' \
        -e 's|#include "RemoteSceneController.h"|#include "rendering/remote/RemoteSceneController.h"|g' \
        -e 's|#include "MediaPreloader.h"|#include "rendering/media/MediaPreloader.h"|g' \
        -e 's|#include "ScreenNavigationManager.h"|#include "rendering/navigation/ScreenNavigationManager.h"|g' \
        -e 's|#include "ResponsiveLayoutManager.h"|#include "ui/layout/ResponsiveLayoutManager.h"|g' \
        -e 's|#include "ToastNotificationSystem.h"|#include "ui/notifications/ToastNotificationSystem.h"|g' \
        -e 's|#include "ToastManager.h"|#include "ui/notifications/ToastManager.h"|g' \
        -e 's|#include "SpinnerWidget.h"|#include "ui/widgets/SpinnerWidget.h"|g' \
        -e 's|#include "QtWaitingSpinner.h"|#include "ui/widgets/QtWaitingSpinner.h"|g' \
        -e 's|#include "StyleConfig.h"|#include "ui/theme/StyleConfig.h"|g' \
        -e 's|#include "ThemeManager.h"|#include "ui/theme/ThemeManager.h"|g' \
        -e 's|#include "SettingsManager.h"|#include "managers/app/SettingsManager.h"|g' \
        -e 's|#include "SystemTrayManager.h"|#include "managers/app/SystemTrayManager.h"|g' \
        -e 's|#include "MenuBarManager.h"|#include "managers/app/MenuBarManager.h"|g' \
        -e 's|#include "TopBarManager.h"|#include "managers/ui/TopBarManager.h"|g' \
        -e 's|#include "RemoteClientInfoManager.h"|#include "managers/ui/RemoteClientInfoManager.h"|g' \
        -e 's|#include "UploadButtonStyleManager.h"|#include "managers/ui/UploadButtonStyleManager.h"|g' \
        -e 's|#include "ConnectionManager.h"|#include "managers/network/ConnectionManager.h"|g' \
        -e 's|#include "ClientListBuilder.h"|#include "managers/network/ClientListBuilder.h"|g' \
        -e 's|#include "SystemMonitor.h"|#include "managers/system/SystemMonitor.h"|g' \
        -e 's|#include "MacVideoThumbnailer.h"|#include "platform/macos/MacVideoThumbnailer.h"|g' \
        -e 's|#include "MacWindowManager.h"|#include "platform/macos/MacWindowManager.h"|g' \
        -e 's|#include "NativeMenu.h"|#include "platform/macos/NativeMenu.h"|g' \
        -e 's|#include "WindowsVideoThumbnailer.h"|#include "platform/windows/WindowsVideoThumbnailer.h"|g' \
        -e 's|#include "VolumeMonitor.h"|#include "platform/windows/VolumeMonitor.h"|g' \
        "$file"
done

echo "✅ Include paths updated!"
```

---

### Phase 4: Update CMakeLists.txt 🛠️ (15 minutes)

Update your `CMakeLists.txt` to reflect new structure:

```cmake
# Update source file lists with new paths
set(PROJECT_SOURCES
    # Core
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/core/AppColors.cpp
    src/core/AppColors.h
    
    # Domain - Models
    src/domain/models/ClientInfo.cpp
    src/domain/models/ClientInfo.h
    
    # Domain - Session
    src/domain/session/SessionManager.cpp
    src/domain/session/SessionManager.h
    
    # Domain - Media
    src/domain/media/MediaItems.cpp
    src/domain/media/MediaItems.h
    src/domain/media/ResizableMediaItems.h
    src/domain/media/MediaSettingsPanel.cpp
    src/domain/media/MediaSettingsPanel.h
    src/domain/media/SelectionIndicators.cpp
    src/domain/media/SelectionIndicators.h
    
    # Network
    src/network/WebSocketClient.cpp
    src/network/WebSocketClient.h
    src/network/UploadManager.cpp
    src/network/UploadManager.h
    src/network/WatchManager.cpp
    src/network/WatchManager.h
    src/network/RemoteFileTracker.cpp
    src/network/RemoteFileTracker.h
    
    # Files
    src/files/FileManager.cpp
    src/files/FileManager.h
    src/files/LocalFileRepository.cpp
    src/files/LocalFileRepository.h
    src/files/FileMemoryCache.cpp
    src/files/FileMemoryCache.h
    src/files/FileWatcher.cpp
    src/files/FileWatcher.h
    src/files/Theme.cpp
    src/files/Theme.h
    
    # Rendering - Canvas
    src/rendering/canvas/ScreenCanvas.cpp
    src/rendering/canvas/ScreenCanvas.h
    src/rendering/canvas/RoundedRectItem.cpp
    src/rendering/canvas/RoundedRectItem.h
    src/rendering/canvas/OverlayPanels.cpp
    src/rendering/canvas/OverlayPanels.h
    
    # Rendering - Remote
    src/rendering/remote/RemoteSceneController.cpp
    src/rendering/remote/RemoteSceneController.h
    
    # Rendering - Media
    src/rendering/media/MediaPreloader.cpp
    src/rendering/media/MediaPreloader.h
    
    # Rendering - Navigation
    src/rendering/navigation/ScreenNavigationManager.cpp
    src/rendering/navigation/ScreenNavigationManager.h
    
    # UI - Pages
    src/ui/pages/ClientListPage.cpp
    src/ui/pages/ClientListPage.h
    src/ui/pages/CanvasViewPage.cpp
    src/ui/pages/CanvasViewPage.h
    
    # UI - Widgets
    src/ui/widgets/ClientListDelegate.cpp
    src/ui/widgets/ClientListDelegate.h
    src/ui/widgets/ClippedContainer.cpp
    src/ui/widgets/ClippedContainer.h
    src/ui/widgets/RoundedContainer.cpp
    src/ui/widgets/RoundedContainer.h
    src/ui/widgets/SpinnerWidget.cpp
    src/ui/widgets/SpinnerWidget.h
    src/ui/widgets/QtWaitingSpinner.cpp
    src/ui/widgets/QtWaitingSpinner.h
    
    # UI - Layout
    src/ui/layout/ResponsiveLayoutManager.cpp
    src/ui/layout/ResponsiveLayoutManager.h
    
    # UI - Notifications
    src/ui/notifications/ToastNotificationSystem.cpp
    src/ui/notifications/ToastNotificationSystem.h
    
    # UI - Theme
    src/ui/theme/StyleConfig.h
    src/ui/theme/ThemeManager.cpp
    src/ui/theme/ThemeManager.h
    
    # Handlers
    src/handlers/ClientListEventHandler.cpp
    src/handlers/ClientListEventHandler.h
    src/handlers/ScreenEventHandler.cpp
    src/handlers/ScreenEventHandler.h
    src/handlers/UploadEventHandler.cpp
    src/handlers/UploadEventHandler.h
    src/handlers/UploadSignalConnector.cpp
    src/handlers/UploadSignalConnector.h
    src/handlers/WebSocketMessageHandler.cpp
    src/handlers/WebSocketMessageHandler.h
    src/handlers/WindowEventHandler.cpp
    src/handlers/WindowEventHandler.h
    
    # Managers - App
    src/managers/app/SettingsManager.cpp
    src/managers/app/SettingsManager.h
    src/managers/app/SystemTrayManager.cpp
    src/managers/app/SystemTrayManager.h
    src/managers/app/MenuBarManager.cpp
    src/managers/app/MenuBarManager.h
    
    # Managers - UI
    src/managers/ui/TopBarManager.cpp
    src/managers/ui/TopBarManager.h
    src/managers/ui/RemoteClientInfoManager.cpp
    src/managers/ui/RemoteClientInfoManager.h
    src/managers/ui/UploadButtonStyleManager.cpp
    src/managers/ui/UploadButtonStyleManager.h
    
    # Managers - Network
    src/managers/network/ConnectionManager.cpp
    src/managers/network/ConnectionManager.h
    src/managers/network/ClientListBuilder.cpp
    src/managers/network/ClientListBuilder.h
    
    # Managers - System
    src/managers/system/SystemMonitor.cpp
    src/managers/system/SystemMonitor.h
    
    # Controllers
    src/controllers/CanvasSessionController.cpp
    src/controllers/CanvasSessionController.h
    src/controllers/TimerController.cpp
    src/controllers/TimerController.h
    
    # Platform - macOS
    src/platform/macos/MacVideoThumbnailer.h
    src/platform/macos/MacVideoThumbnailer.mm
    src/platform/macos/MacWindowManager.h
    src/platform/macos/MacWindowManager.mm
    src/platform/macos/NativeMenu.h
    src/platform/macos/NativeMenu.mm
    
    # Platform - Windows
    src/platform/windows/WindowsVideoThumbnailer.cpp
    src/platform/windows/WindowsVideoThumbnailer.h
)
```

---

### Phase 5: Clean Up Empty Files 🧹 (5 minutes)

**Decision Points**:

1. **MediaPreloader.h/cpp** - Empty files
   - **Option A**: Delete (no implementation)
   - **Option B**: Implement if needed for future features
   - **Recommendation**: Delete now, recreate when needed

2. **ToastManager.h/cpp** - Empty files, but `ToastNotificationSystem` exists
   - **Option A**: Delete (redundant with ToastNotificationSystem)
   - **Option B**: Implement if different from ToastNotificationSystem
   - **Recommendation**: Delete, consolidate into ToastNotificationSystem

3. **UIManager.h/cpp** - Empty files
   - **Option A**: Delete (no clear purpose)
   - **Option B**: Implement as UI coordinator (high-level)
   - **Recommendation**: Delete, not needed with current architecture

4. **VolumeMonitor.h/cpp** - Empty files
   - **Note**: Volume monitoring is in `SystemMonitor`
   - **Recommendation**: Delete, functionality exists elsewhere

```bash
# Remove empty/redundant files
rm src/rendering/media/MediaPreloader.*
rm src/ui/notifications/ToastManager.*
rm src/UIManager.*
rm src/platform/windows/VolumeMonitor.*
```

---

### Phase 6: Compile & Test ✅ (30 minutes)

```bash
cd client
rm -rf build/*
./build.sh
```

**Expected Issues & Solutions**:

1. **Missing includes**: grep error messages, add missing paths
2. **CMake file not found**: double-check file paths in CMakeLists.txt
3. **Circular dependencies**: check if any revealed by new structure
4. **MOC issues**: Qt's moc may need to rescan - clean build should fix

---

## 🎯 BENEFITS OF NEW STRUCTURE

### 1. **Discoverability** 📍
- **Before**: "Where is UploadManager?" → Search through 95 files
- **After**: "Where is UploadManager?" → Check `network/` directory (8 files)

### 2. **Logical Grouping** 🧩
- **Domain layer**: Business logic (models, session, media)
- **Network layer**: All communication in one place
- **Rendering layer**: All graphics code grouped
- **UI layer**: User interface components
- **Platform layer**: OS-specific implementations isolated

### 3. **Reduced Cognitive Load** 🧠
- Each directory has 3-10 files (manageable)
- Clear naming: `managers/app/` vs `managers/ui/` vs `managers/network/`
- Platform code separated from cross-platform code

### 4. **Better Testing** 🧪
- Domain logic easily testable (no UI dependencies)
- Network layer can be mocked
- UI components can be tested in isolation

### 5. **Onboarding** 👥
- New developers understand structure immediately
- Clear boundaries between layers
- Easy to find relevant code

### 6. **Future Scalability** 📈
- Easy to add new features in correct location
- Plugin architecture possible (platform/ directory)
- Microservice extraction easier (network/ is self-contained)

---

## 📊 METRICS COMPARISON

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| Files in src/ root | 95 | 3 | **-97%** 🎉 |
| Max files per directory | 95 | 10 | **-89%** |
| Directory depth | 2 | 4 | Manageable |
| Platform isolation | Mixed | Dedicated dir | ✅ Clear |
| Layer separation | Partial | Complete | ✅ Clean |
| Empty technical debt | 4 files | 0 files | ✅ Cleaned |

---

## ⚠️ IMPORTANT NOTES

### 1. **Backup First**
```bash
# Create backup branch
git checkout -b backup-before-restructure
git add -A
git commit -m "Backup before restructure"
git checkout main
git checkout -b feature/project-restructure
```

### 2. **Gradual Migration Option**
If full migration is too risky, do it in stages:
- **Stage 1**: Create directories + move 10 files → compile
- **Stage 2**: Move another 20 files → compile
- **Stage 3**: Move remaining files → compile

### 3. **IDE Configuration**
Update your IDE's include paths:
- Qt Creator: Add `src/` as include path
- VS Code: Update `includePath` in `.vscode/c_cpp_properties.json`

### 4. **Documentation**
After migration, update:
- `client/README.md` with new structure
- Any architecture diagrams
- Onboarding guides

---

## 🚀 EXECUTION CHECKLIST

- [ ] **Pre-Migration**
  - [ ] Create backup branch
  - [ ] Commit all current changes
  - [ ] Run tests to establish baseline
  - [ ] Note current compile time

- [ ] **Phase 1**: Create directory structure
- [ ] **Phase 2**: Move files (in batches)
- [ ] **Phase 3**: Update includes (automated script)
- [ ] **Phase 4**: Update CMakeLists.txt
- [ ] **Phase 5**: Delete empty files
- [ ] **Phase 6**: Compile & test
  - [ ] Clean build successful
  - [ ] All tests pass
  - [ ] Application launches
  - [ ] Basic functionality works

- [ ] **Post-Migration**
  - [ ] Update README.md
  - [ ] Create architecture diagram
  - [ ] Document new structure
  - [ ] Merge to main

---

## 🎊 FINAL THOUGHTS

This restructure complements your excellent MainWindow refactoring! You went from:
- **4,164 lines** → **1,666 lines** (60% reduction in MainWindow)
- **95 files in src/** → **3 files in src/** (97% reduction in root clutter)

Together, these changes transform Mouffette from a growing codebase into a **professionally organized, enterprise-grade application structure**! 🏆

**Estimated Total Time**: 2-3 hours
**Risk Level**: Low (if done systematically with backups)
**Reward**: Massive improvement in maintainability! ✨

---

## 📞 NEXT STEPS

**What would you like to do?**

**Option A**: 🚀 **Full Migration** - I'll help you execute all phases
**Option B**: 📋 **Staged Migration** - Move files in small, safe batches
**Option C**: 🔧 **Customized Structure** - Adjust the proposal to your preferences
**Option D**: 📊 **Analysis Only** - Review proposal, decide later

Let me know and I'll guide you through it! 💪
