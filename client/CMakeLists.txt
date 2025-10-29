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

# Source files
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/WebSocketClient.cpp
    src/ClientInfo.cpp
    src/OverlayPanels.cpp
    src/UploadManager.cpp
    src/WatchManager.cpp
    src/ScreenNavigationManager.cpp
    src/ResponsiveLayoutManager.cpp
    src/SpinnerWidget.cpp
    src/QtWaitingSpinner.cpp
    src/MediaItems.cpp
    src/ScreenCanvas.cpp
    src/MediaSettingsPanel.cpp
    src/FileWatcher.cpp
    src/FileManager.cpp
    src/Theme.cpp
    src/AppColors.cpp
    src/RemoteSceneController.cpp
    src/ToastNotificationSystem.cpp
    src/SessionManager.cpp
    src/LocalFileRepository.cpp
    src/RemoteFileTracker.cpp
    src/FileMemoryCache.cpp
    src/ui/widgets/RoundedContainer.cpp
    src/ui/widgets/ClippedContainer.cpp
    src/ui/widgets/ClientListDelegate.cpp
    src/ui/pages/ClientListPage.cpp
    src/managers/ThemeManager.cpp
    src/managers/ConnectionManager.cpp
)

# Platform-specific sources
if(APPLE)
    list(APPEND SOURCES src/MacVideoThumbnailer.mm src/MacWindowManager.mm)
endif()

if(WIN32)
    list(APPEND SOURCES src/WindowsVideoThumbnailer.cpp)
endif()

# Header files
set(HEADERS
    src/MainWindow.h
    src/WebSocketClient.h
    src/ClientInfo.h
    src/MacVideoThumbnailer.h
    src/MacWindowManager.h
    src/OverlayPanels.h
    src/RoundedRectItem.h
    src/UploadManager.h
    src/WatchManager.h
    src/ScreenNavigationManager.h
    src/ResponsiveLayoutManager.h
    src/SpinnerWidget.h
    src/QtWaitingSpinner.h
    src/ScreenCanvas.h
    src/MediaSettingsPanel.h
    src/FileWatcher.h
    src/FileManager.h
    src/Theme.h
    src/AppColors.h
    src/RemoteSceneController.h
    src/ToastNotificationSystem.h
    src/SessionManager.h
    src/LocalFileRepository.h
    src/RemoteFileTracker.h
    src/FileMemoryCache.h
    src/ui/StyleConfig.h
    src/ui/widgets/RoundedContainer.h
    src/ui/widgets/ClippedContainer.h
    src/ui/widgets/ClientListDelegate.h
    src/ui/pages/ClientListPage.h
    src/managers/ThemeManager.h
    src/managers/ConnectionManager.h
)

if(WIN32)
    list(APPEND HEADERS src/WindowsVideoThumbnailer.h)
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
    
    # Set Objective-C++ properties for .mm files
    set_source_files_properties(src/MacVideoThumbnailer.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/MacWindowManager.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()
