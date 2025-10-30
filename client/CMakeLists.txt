cmake_minimum_required(VERSION 3.16)
project(MouffetteClient VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# On Windows, allow building as a console app to see logs in the terminal
option(CONSOLE_OUTPUT "Build with console subsystem on Windows for visible qDebug logs" OFF)

# Enable Objective-C++ on macOS
if(APPLE)
    enable_language(OBJCXX)
endif()

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network WebSockets Multimedia MultimediaWidgets Svg SvgWidgets Concurrent)

# Enable Qt MOC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Source files - Reorganized by layer
set(SOURCES
    # Core
    src/main.cpp
    src/MainWindow.cpp
    src/core/AppColors.cpp
    
    # Domain - Models
    src/domain/models/ClientInfo.cpp
    
    # Domain - Session
    src/domain/session/SessionManager.cpp
    
    # Domain - Media
    src/domain/media/MediaItems.cpp
    src/domain/media/MediaSettingsPanel.cpp
    src/domain/media/SelectionIndicators.cpp
    
    # Network
    src/network/WebSocketClient.cpp
    src/network/UploadManager.cpp
    src/network/WatchManager.cpp
    src/network/RemoteFileTracker.cpp
    
    # Files
    src/files/FileManager.cpp
    src/files/LocalFileRepository.cpp
    src/files/FileMemoryCache.cpp
    src/files/FileWatcher.cpp
    src/files/Theme.cpp
    
    # Rendering - Canvas
    src/rendering/canvas/ScreenCanvas.cpp
    src/rendering/canvas/OverlayPanels.cpp
    src/rendering/canvas/RoundedRectItem.cpp
    
    # Rendering - Remote
    src/rendering/remote/RemoteSceneController.cpp
    
    # Rendering - Navigation
    src/rendering/navigation/ScreenNavigationManager.cpp
    
    # UI - Pages
    src/ui/pages/ClientListPage.cpp
    src/ui/pages/CanvasViewPage.cpp
    
    # UI - Widgets
    src/ui/widgets/RoundedContainer.cpp
    src/ui/widgets/ClippedContainer.cpp
    src/ui/widgets/ClientListDelegate.cpp
    src/ui/widgets/SpinnerWidget.cpp
    src/ui/widgets/QtWaitingSpinner.cpp
    
    # UI - Layout
    src/ui/layout/ResponsiveLayoutManager.cpp
    
    # UI - Notifications
    src/ui/notifications/ToastNotificationSystem.cpp
    
    # UI - Theme
    src/ui/theme/ThemeManager.cpp
    
    # Handlers
    src/handlers/WebSocketMessageHandler.cpp
    src/handlers/ScreenEventHandler.cpp
    src/handlers/ClientListEventHandler.cpp
    src/handlers/UploadEventHandler.cpp
    src/handlers/WindowEventHandler.cpp
    src/handlers/UploadSignalConnector.cpp
    
    # Managers - App
    src/managers/app/SettingsManager.cpp
    src/managers/app/SystemTrayManager.cpp
    src/managers/app/MenuBarManager.cpp
    
    # Managers - UI
    src/managers/ui/TopBarManager.cpp
    src/managers/ui/RemoteClientInfoManager.cpp
    src/managers/ui/UploadButtonStyleManager.cpp
    
    # Managers - Network
    src/managers/network/ConnectionManager.cpp
    src/managers/network/ClientListBuilder.cpp
    
    # Managers - System
    src/managers/system/SystemMonitor.cpp
    
    # Controllers
    src/controllers/CanvasSessionController.cpp
    src/controllers/TimerController.cpp
)

# Platform-specific sources
if(APPLE)
    list(APPEND SOURCES 
        src/platform/macos/MacVideoThumbnailer.mm 
        src/platform/macos/MacWindowManager.mm
        src/platform/macos/NativeMenu.mm
    )
endif()

if(WIN32)
    list(APPEND SOURCES src/platform/windows/WindowsVideoThumbnailer.cpp)
endif()

# Header files - Reorganized by layer
set(HEADERS
    # Core
    src/MainWindow.h
    src/core/AppColors.h
    
    # Domain - Models
    src/domain/models/ClientInfo.h
    
    # Domain - Session
    src/domain/session/SessionManager.h
    
    # Domain - Media
    src/domain/media/MediaItems.h
    src/domain/media/ResizableMediaItems.h
    src/domain/media/MediaSettingsPanel.h
    src/domain/media/SelectionIndicators.h
    
    # Network
    src/network/WebSocketClient.h
    src/network/UploadManager.h
    src/network/WatchManager.h
    src/network/RemoteFileTracker.h
    
    # Files
    src/files/FileManager.h
    src/files/LocalFileRepository.h
    src/files/FileMemoryCache.h
    src/files/FileWatcher.h
    src/files/Theme.h
    
    # Rendering - Canvas
    src/rendering/canvas/ScreenCanvas.h
    src/rendering/canvas/OverlayPanels.h
    src/rendering/canvas/RoundedRectItem.h
    
    # Rendering - Remote
    src/rendering/remote/RemoteSceneController.h
    
    # Rendering - Navigation
    src/rendering/navigation/ScreenNavigationManager.h
    
    # UI - Pages
    src/ui/pages/ClientListPage.h
    src/ui/pages/CanvasViewPage.h
    
    # UI - Widgets
    src/ui/widgets/RoundedContainer.h
    src/ui/widgets/ClippedContainer.h
    src/ui/widgets/ClientListDelegate.h
    src/ui/widgets/SpinnerWidget.h
    src/ui/widgets/QtWaitingSpinner.h
    
    # UI - Layout
    src/ui/layout/ResponsiveLayoutManager.h
    
    # UI - Notifications
    src/ui/notifications/ToastNotificationSystem.h
    
    # UI - Theme
    src/ui/theme/StyleConfig.h
    src/ui/theme/ThemeManager.h
    
    # Handlers
    src/handlers/WebSocketMessageHandler.h
    src/handlers/ScreenEventHandler.h
    src/handlers/ClientListEventHandler.h
    src/handlers/UploadEventHandler.h
    src/handlers/WindowEventHandler.h
    src/handlers/UploadSignalConnector.h
    
    # Managers - App
    src/managers/app/SettingsManager.h
    src/managers/app/SystemTrayManager.h
    src/managers/app/MenuBarManager.h
    
    # Managers - UI
    src/managers/ui/TopBarManager.h
    src/managers/ui/RemoteClientInfoManager.h
    src/managers/ui/UploadButtonStyleManager.h
    
    # Managers - Network
    src/managers/network/ConnectionManager.h
    src/managers/network/ClientListBuilder.h
    
    # Managers - System
    src/managers/system/SystemMonitor.h
    
    # Controllers
    src/controllers/CanvasSessionController.h
    src/controllers/TimerController.h
)

# Platform-specific headers
if(APPLE)
    list(APPEND HEADERS
        src/platform/macos/MacVideoThumbnailer.h
        src/platform/macos/MacWindowManager.h
        src/platform/macos/NativeMenu.h
    )
endif()

if(WIN32)
    list(APPEND HEADERS src/platform/windows/WindowsVideoThumbnailer.h)
endif()

# UI files
set(UI_FILES
    ui/MainWindow.ui
)

# Resource files
set(RESOURCE_FILES
    resources/resources.qrc
)

# Create executable
add_executable(MouffetteClient ${SOURCES} ${HEADERS} ${UI_FILES} ${RESOURCE_FILES})

# Link Qt libraries
target_link_libraries(MouffetteClient 
    Qt6::Core 
    Qt6::Widgets 
    Qt6::Network 
    Qt6::WebSockets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::Concurrent
)

# Include directories
target_include_directories(MouffetteClient PRIVATE src)

# Platform-specific settings
if(WIN32)
    if(NOT CONSOLE_OUTPUT)
        # GUI subsystem (no console). Set CONSOLE_OUTPUT=ON to see logs in terminal.
        set_target_properties(MouffetteClient PROPERTIES WIN32_EXECUTABLE TRUE)
    else()
        message(STATUS "Building MouffetteClient with console subsystem (logs visible in terminal)")
    endif()
    # Link Core Audio COM interfaces used for volume detection
    target_link_libraries(MouffetteClient
        ole32
        uuid
        windowscodecs
        mfplat
        mfreadwrite
        mfuuid
    )
endif()

if(APPLE)
    set_target_properties(MouffetteClient PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist
    )
    # Link with macOS frameworks used by native integrations
    target_link_libraries(MouffetteClient "-framework AppKit" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreGraphics" "-framework QuickLookThumbnailing")
    
    # Set Objective-C++ properties for .mm files (updated paths)
    set_source_files_properties(src/platform/macos/MacVideoThumbnailer.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/platform/macos/MacWindowManager.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/platform/macos/NativeMenu.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()
