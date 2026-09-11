cmake_minimum_required(VERSION 3.16)
project(MouffetteClient VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MOUFFETTE_QT_ROOT "" CACHE PATH "Root of the Qt distribution used to build Mouffette")
option(MOUFFETTE_REQUIRE_EXPLICIT_QT_ROOT "Fail unless MOUFFETTE_QT_ROOT is provided" OFF)
if(MOUFFETTE_REQUIRE_EXPLICIT_QT_ROOT AND NOT MOUFFETTE_QT_ROOT)
    message(FATAL_ERROR
        "MOUFFETTE_QT_ROOT is required by this preset. Point it to a Qt distribution "
        "compatible with the active compiler and platform SDK.")
endif()
if(MOUFFETTE_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${MOUFFETTE_QT_ROOT}")
endif()

# On Windows, allow building as a console app to see logs in the terminal
option(CONSOLE_OUTPUT "Build with console subsystem on Windows for visible qDebug logs" OFF)

# Enable Objective-C++ on macOS
if(APPLE)
    enable_language(OBJCXX)
endif()

# Find Qt6
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Widgets Network WebSockets Multimedia MultimediaWidgets Svg SvgWidgets Concurrent Quick QuickWidgets Qml ShaderTools GuiPrivate QuickPrivate)

# Recent macOS SDKs no longer ship AGL.  Some older/broken Qt packages still
# export it transitively from QtGui; detect that toolchain mismatch at configure
# time instead of hiding it with linker flags or failing after a full build.
if(APPLE)
    get_target_property(_mouffette_qt_gui_binary Qt6::Gui IMPORTED_LOCATION)
    if(_mouffette_qt_gui_binary AND EXISTS "${_mouffette_qt_gui_binary}")
        execute_process(
            COMMAND otool -L "${_mouffette_qt_gui_binary}"
            OUTPUT_VARIABLE _mouffette_qt_gui_dependencies
            ERROR_QUIET)
        if(_mouffette_qt_gui_dependencies MATCHES "AGL\\.framework")
            message(FATAL_ERROR
                "The selected QtGui (${_mouffette_qt_gui_binary}) depends on the removed "
                "macOS AGL framework. Install/select a Qt build compatible with the current "
                "SDK and configure with -DMOUFFETTE_QT_ROOT=<qt-prefix>.")
        endif()
    endif()
endif()

# Enable Qt MOC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Source files - Frontend/Backend Architecture
set(SOURCES
    # ═══════════════════════════════════════════════════════
    # CORE
    # ═══════════════════════════════════════════════════════
    src/main.cpp
    src/MainWindow.cpp
    
    # ═══════════════════════════════════════════════════════
    # FRONTEND - User Interface & Rendering
    # ═══════════════════════════════════════════════════════
    
    # UI - Pages
    src/frontend/ui/pages/ClientListPage.cpp
    src/frontend/ui/pages/CanvasViewPage.cpp
    
    # UI - Widgets
    src/frontend/ui/widgets/RoundedContainer.cpp
    src/frontend/ui/widgets/ClippedContainer.cpp
    src/frontend/ui/widgets/ClientListDelegate.cpp
    src/frontend/ui/widgets/SpinnerWidget.cpp
    src/frontend/ui/widgets/QtWaitingSpinner.cpp
    
    # UI - Layout
    src/frontend/ui/layout/ResponsiveLayoutManager.cpp
    
    # UI - Notifications
    src/frontend/ui/notifications/ToastNotificationSystem.cpp
    
    # UI - Theme
    src/frontend/ui/theme/ThemeManager.cpp
    src/frontend/ui/theme/AppColors.cpp
    
    # Rendering - Canvas
    src/frontend/rendering/canvas/ScreenCanvas.cpp
    src/frontend/rendering/canvas/LegacyCanvasHost.cpp
    src/frontend/rendering/canvas/LegacySceneMirror.cpp
    src/frontend/rendering/canvas/QuickCanvasHost.cpp
    src/frontend/rendering/canvas/QuickCanvasController.cpp
    src/frontend/rendering/canvas/GestureCommands.cpp
    src/frontend/rendering/canvas/PointerSession.cpp
    src/frontend/rendering/canvas/ModelPublisher.cpp
    src/frontend/rendering/canvas/SelectionStore.cpp
    src/frontend/rendering/canvas/SnapStore.cpp
    src/frontend/rendering/canvas/SnapEngine.cpp
    src/frontend/rendering/canvas/SnapGuidePublisher.cpp
    src/frontend/rendering/canvas/DragSnapEngine.cpp
    src/frontend/rendering/canvas/QuickDragSnapSession.cpp
    src/frontend/rendering/canvas/CanvasSceneStore.cpp
    src/frontend/rendering/canvas/QuickCanvasViewAdapter.cpp
    src/frontend/rendering/canvas/MediaListModel.cpp
    src/frontend/rendering/canvas/OverlayPanels.cpp
    src/frontend/rendering/canvas/TextOutlineItem.cpp
    src/frontend/rendering/canvas/TextEditHelper.cpp
    src/frontend/rendering/canvas/CanvasQmlTypes.cpp
    
    # Rendering - Remote
    src/frontend/rendering/remote/RemoteSceneController.cpp
    src/frontend/rendering/remote/RemoteVideoFrameItem.cpp
    
    # Rendering - Navigation
    src/frontend/rendering/navigation/ScreenNavigationManager.cpp
    
    # Frontend Handlers
    src/frontend/handlers/WindowEventHandler.cpp
    src/frontend/handlers/UploadSignalConnector.cpp
    
    # Frontend Managers
    src/frontend/managers/ui/TopBarManager.cpp
    src/frontend/managers/ui/RemoteClientInfoManager.cpp
    src/frontend/managers/ui/UploadButtonStyleManager.cpp
    
    # ═══════════════════════════════════════════════════════
    # BACKEND - Business Logic & Data
    # ═══════════════════════════════════════════════════════
    
    # Domain - Models
    src/backend/domain/models/ClientInfo.cpp
    
    # Domain - Session
    src/backend/domain/session/SessionManager.cpp
    
    # Domain - Media
    src/backend/domain/media/MediaItems.cpp
    src/backend/domain/media/MediaRuntimeHooks.cpp
    src/backend/domain/media/TextRenderState.cpp
    src/backend/domain/media/TextMediaItem.cpp

    # UI - Overlays (Canvas)
    src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.cpp
    src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.cpp
    
    # Network
    src/backend/network/WebSocketClient.cpp
    src/backend/network/UploadManager.cpp
    src/backend/network/WatchManager.cpp
    src/backend/network/RemoteFileTracker.cpp
    
    # Files
    src/backend/files/FileManager.cpp
    src/backend/files/LocalFileRepository.cpp
    src/backend/files/FileMemoryCache.cpp
    src/backend/files/FileWatcher.cpp
    src/backend/files/Theme.cpp
    
    # Backend Handlers
    src/backend/handlers/WebSocketMessageHandler.cpp
    src/backend/handlers/ScreenEventHandler.cpp
    src/backend/handlers/ClientListEventHandler.cpp
    src/backend/handlers/UploadEventHandler.cpp
    
    # Backend Managers - App
    src/backend/managers/app/SettingsManager.cpp
    src/backend/managers/app/SystemTrayManager.cpp
    src/backend/managers/app/MenuBarManager.cpp
    src/backend/managers/app/MigrationTelemetryManager.cpp
    
    # Backend Managers - Network
    src/backend/managers/network/ConnectionManager.cpp
    src/backend/managers/network/ClientListBuilder.cpp
    
    # Backend Managers - System
    src/backend/managers/system/SystemMonitor.cpp
    
    # Controllers
    src/backend/controllers/CanvasSessionController.cpp
    src/backend/controllers/TimerController.cpp
)

# Platform-specific sources
if(APPLE)
    list(APPEND SOURCES 
        src/backend/platform/macos/MacVideoThumbnailer.mm 
        src/backend/platform/macos/MacWindowManager.mm
    )
endif()

if(WIN32)
    list(APPEND SOURCES src/backend/platform/windows/WindowsVideoThumbnailer.cpp)
endif()

# Header files - Frontend/Backend Architecture
set(HEADERS
    # ═══════════════════════════════════════════════════════
    # CORE
    # ═══════════════════════════════════════════════════════
    src/MainWindow.h
    
    # ═══════════════════════════════════════════════════════
    # FRONTEND - User Interface & Rendering
    # ═══════════════════════════════════════════════════════
    
    # UI - Pages
    src/frontend/ui/pages/ClientListPage.h
    src/frontend/ui/pages/CanvasViewPage.h
    
    # UI - Widgets
    src/frontend/ui/widgets/RoundedContainer.h
    src/frontend/ui/widgets/ClippedContainer.h
    src/frontend/ui/widgets/ClientListDelegate.h
    src/frontend/ui/widgets/SpinnerWidget.h
    src/frontend/ui/widgets/QtWaitingSpinner.h
    
    # UI - Layout
    src/frontend/ui/layout/ResponsiveLayoutManager.h
    
    # UI - Notifications
    src/frontend/ui/notifications/ToastNotificationSystem.h

    # UI - Overlays (Canvas)
    src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.h
    src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.h
    
    # UI - Theme
    src/frontend/ui/theme/StyleConfig.h
    src/frontend/ui/theme/ThemeManager.h
    src/frontend/ui/theme/AppColors.h
    
    # Rendering - Canvas
    src/frontend/rendering/canvas/ScreenCanvas.h
    src/frontend/rendering/canvas/LegacyCanvasHost.h
    src/frontend/rendering/canvas/LegacySceneMirror.h
    src/frontend/rendering/canvas/QuickCanvasHost.h
    src/frontend/rendering/canvas/QuickCanvasController.h
    src/frontend/rendering/canvas/GestureCommands.h
    src/frontend/rendering/canvas/PointerSession.h
    src/frontend/rendering/canvas/ModelPublisher.h
    src/frontend/rendering/canvas/SelectionStore.h
    src/frontend/rendering/canvas/SnapStore.h
    src/frontend/rendering/canvas/SnapEngine.h
    src/frontend/rendering/canvas/SnapGuidePublisher.h
    src/frontend/rendering/canvas/DragSnapEngine.h
    src/frontend/rendering/canvas/QuickDragSnapSession.h
    src/frontend/rendering/canvas/CanvasSceneStore.h
    src/frontend/rendering/canvas/QuickCanvasViewAdapter.h
    src/frontend/rendering/canvas/MediaListModel.h
    src/frontend/rendering/canvas/OverlayPanels.h
    src/frontend/rendering/canvas/TextOutlineItem.h
    src/frontend/rendering/canvas/TextEditHelper.h
    src/frontend/rendering/canvas/CanvasQmlTypes.h
    src/frontend/rendering/canvas/RoundedRectItem.h
    src/frontend/rendering/canvas/SegmentedButtonItem.h

    # Shared - Rendering Contracts
    src/shared/rendering/ICanvasHost.h
    src/shared/rendering/IMediaSceneAdapter.h
    src/shared/rendering/IOverlayProjection.h
    
    # Rendering - Remote
    src/frontend/rendering/remote/RemoteSceneController.h
    src/frontend/rendering/remote/RemoteVideoFrameItem.h
    
    # Rendering - Navigation
    src/frontend/rendering/navigation/ScreenNavigationManager.h
    
    # Frontend Handlers
    src/frontend/handlers/WindowEventHandler.h
    src/frontend/handlers/UploadSignalConnector.h
    
    # Frontend Managers
    src/frontend/managers/ui/TopBarManager.h
    src/frontend/managers/ui/RemoteClientInfoManager.h
    src/frontend/managers/ui/UploadButtonStyleManager.h
    
    # ═══════════════════════════════════════════════════════
    # BACKEND - Business Logic & Data
    # ═══════════════════════════════════════════════════════
    
    # Domain - Models
    src/backend/domain/models/ClientInfo.h
    
    # Domain - Session
    src/backend/domain/session/SessionManager.h
    
    # Domain - Media
    src/backend/domain/media/MediaItems.h
    src/backend/domain/media/MediaRuntimeHooks.h
    src/backend/domain/media/TextRenderState.h
    src/backend/domain/media/TextMediaItem.h
    
    # Network
    src/backend/network/WebSocketClient.h
    src/backend/network/UploadManager.h
    src/backend/network/WatchManager.h
    src/backend/network/RemoteFileTracker.h
    
    # Files
    src/backend/files/FileManager.h
    src/backend/files/LocalFileRepository.h
    src/backend/files/FileMemoryCache.h
    src/backend/files/FileWatcher.h
    src/backend/files/Theme.h
    
    # Backend Handlers
    src/backend/handlers/WebSocketMessageHandler.h
    src/backend/handlers/ScreenEventHandler.h
    src/backend/handlers/ClientListEventHandler.h
    src/backend/handlers/UploadEventHandler.h
    
    # Backend Managers - App
    src/backend/managers/app/SettingsManager.h
    src/backend/managers/app/SystemTrayManager.h
    src/backend/managers/app/MenuBarManager.h
    src/backend/managers/app/MigrationTelemetryManager.h
    
    # Backend Managers - Network
    src/backend/managers/network/ConnectionManager.h
    src/backend/managers/network/ClientListBuilder.h
    
    # Backend Managers - System
    src/backend/managers/system/SystemMonitor.h
    
    # Controllers
    src/backend/controllers/CanvasSessionController.h
    src/backend/controllers/TimerController.h
)

# Platform-specific headers
if(APPLE)
    list(APPEND HEADERS
        src/backend/platform/macos/MacVideoThumbnailer.h
        src/backend/platform/macos/MacWindowManager.h
    )
endif()

if(WIN32)
    list(APPEND HEADERS src/backend/platform/windows/WindowsVideoThumbnailer.h)
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
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::Qml
    Qt6::GuiPrivate
    Qt6::QuickPrivate
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
    
    # Set Objective-C++ properties for .mm files (updated paths for backend/platform)
    set_source_files_properties(src/backend/platform/macos/MacVideoThumbnailer.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/backend/platform/macos/MacWindowManager.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(tst_TextOutlineItem
        tests/qt/tst_TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextOutlineItem.h
        src/frontend/rendering/canvas/TextEditHelper.cpp
        src/frontend/rendering/canvas/TextEditHelper.h
    )
    target_include_directories(tst_TextOutlineItem PRIVATE src)
    target_compile_definitions(tst_TextOutlineItem PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_TextOutlineItem PRIVATE Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Test)
    add_test(NAME TextOutlineItem COMMAND tst_TextOutlineItem)

    add_executable(tst_TextOutlineMotion
        tests/qt/tst_TextOutlineMotion.cpp
        src/frontend/rendering/canvas/TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextOutlineItem.h
        src/frontend/rendering/canvas/TextEditHelper.cpp
        src/frontend/rendering/canvas/TextEditHelper.h
    )
    target_include_directories(tst_TextOutlineMotion PRIVATE src)
    target_compile_definitions(tst_TextOutlineMotion PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_TextOutlineMotion PRIVATE Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Test)
    add_test(NAME TextOutlineMotion COMMAND tst_TextOutlineMotion)

    add_executable(tst_TextItemQml
        tests/qt/tst_TextItemQml.cpp
        src/frontend/rendering/canvas/TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextOutlineItem.h
        src/frontend/rendering/canvas/TextEditHelper.cpp
        src/frontend/rendering/canvas/TextEditHelper.h
    )
    target_include_directories(tst_TextItemQml PRIVATE src)
    target_compile_definitions(tst_TextItemQml PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_TextItemQml PRIVATE Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Test)
    add_test(NAME TextItemQml COMMAND tst_TextItemQml)
    add_test(NAME TextItemQmlScaled COMMAND tst_TextItemQml)
    set_tests_properties(TextItemQmlScaled PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

    # Reference implementation retained for before/after profiling only. The
    # application no longer registers, compiles or bundles this SVG renderer.
    add_executable(tst_TextGlyphPath
        tests/qt/tst_TextGlyphPath.cpp
        src/frontend/rendering/canvas/TextGlyphPath.cpp
        src/frontend/rendering/canvas/TextGlyphPath.h
        src/frontend/rendering/canvas/GlyphBorderAtlas.cpp
        src/frontend/rendering/canvas/GlyphBorderAtlas.h
        ${ATLAS_QRC}
    )
    target_include_directories(tst_TextGlyphPath PRIVATE src)
    target_link_libraries(tst_TextGlyphPath PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
    add_test(NAME TextGlyphPath COMMAND tst_TextGlyphPath)
    set_tests_properties(TextGlyphPath PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_MediaRuntimeContext
        tests/qt/tst_MediaRuntimeContext.cpp
        src/backend/domain/media/MediaRuntimeHooks.cpp
        src/backend/domain/media/MediaRuntimeHooks.h
    )
    target_include_directories(tst_MediaRuntimeContext PRIVATE src)
    target_link_libraries(tst_MediaRuntimeContext PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME MediaRuntimeContext COMMAND tst_MediaRuntimeContext)
endif()
