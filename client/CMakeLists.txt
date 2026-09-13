cmake_minimum_required(VERSION 3.25)
project(MouffetteClient VERSION 1.0.0)

include(GNUInstallDirs)

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

set(MOUFFETTE_CHANNEL "development" CACHE STRING
    "Application channel: development or production")
set_property(CACHE MOUFFETTE_CHANNEL PROPERTY STRINGS development production)
if(NOT MOUFFETTE_CHANNEL MATCHES "^(development|production)$")
    message(FATAL_ERROR "MOUFFETTE_CHANNEL must be 'development' or 'production'")
endif()

if(MOUFFETTE_CHANNEL STREQUAL "production")
    set(MOUFFETTE_BUNDLE_IDENTIFIER "com.mouffette.client")
else()
    set(MOUFFETTE_BUNDLE_IDENTIFIER "com.mouffette.client.dev")
endif()
# The primary profile deliberately retains the pre-v3 application and
# QSettings identities in both channels. Development/production coordination
# remains separate through MOUFFETTE_BUILD_CHANNEL, not through data paths.
set(MOUFFETTE_APPLICATION_NAME "Mouffette")
set(MOUFFETTE_SETTINGS_APPLICATION "Client")
set(MOUFFETTE_EXECUTABLE_NAME "Mouffette")

configure_file(
    cmake/AppBuildConfig.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/AppBuildConfig.h"
    @ONLY)

# One machine-readable protocol contract owns the extensions accepted by the
# server and every client-side boundary. Generate the Qt representation at
# configure time so the desktop application cannot silently drift from it.
set(MOUFFETTE_MEDIA_FORMAT_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../server/media_format_contract.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MOUFFETTE_MEDIA_FORMAT_CONTRACT_FILE}")
file(READ "${MOUFFETTE_MEDIA_FORMAT_CONTRACT_FILE}"
    MOUFFETTE_MEDIA_FORMAT_CONTRACT_JSON)
string(JSON MOUFFETTE_MEDIA_FORMAT_SCHEMA_VERSION GET
    "${MOUFFETTE_MEDIA_FORMAT_CONTRACT_JSON}" schemaVersion)
if(NOT MOUFFETTE_MEDIA_FORMAT_SCHEMA_VERSION EQUAL 1)
    message(FATAL_ERROR "Unsupported media format contract schema")
endif()

set(MOUFFETTE_ALL_MEDIA_EXTENSIONS "")
foreach(_group IN ITEMS imageExtensions videoExtensions)
    string(JSON _extension_count LENGTH
        "${MOUFFETTE_MEDIA_FORMAT_CONTRACT_JSON}" "${_group}")
    if(_extension_count LESS 1)
        message(FATAL_ERROR "Media format contract ${_group} must not be empty")
    endif()
    math(EXPR _last_extension_index "${_extension_count} - 1")
    set(_extension_literals "")
    foreach(_extension_index RANGE 0 ${_last_extension_index})
        string(JSON _extension GET "${MOUFFETTE_MEDIA_FORMAT_CONTRACT_JSON}"
            "${_group}" ${_extension_index})
        string(LENGTH "${_extension}" _extension_length)
        if(NOT _extension MATCHES "^[a-z0-9]+$"
            OR _extension_length GREATER 16)
            message(FATAL_ERROR
                "Media format contract ${_group} contains invalid extension '${_extension}'")
        endif()
        if(_extension IN_LIST MOUFFETTE_ALL_MEDIA_EXTENSIONS)
            message(FATAL_ERROR
                "Media format contract contains duplicate extension '${_extension}'")
        endif()
        list(APPEND MOUFFETTE_ALL_MEDIA_EXTENSIONS "${_extension}")
        string(APPEND _extension_literals
            "        QStringLiteral(\"${_extension}\"),\n")
    endforeach()
    if(_group STREQUAL "imageExtensions")
        set(MOUFFETTE_IMAGE_EXTENSION_LITERALS "${_extension_literals}")
    else()
        set(MOUFFETTE_VIDEO_EXTENSION_LITERALS "${_extension_literals}")
    endif()
endforeach()
configure_file(
    cmake/MediaFormatContract.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/MediaFormatContract.h"
    @ONLY)
# AppConfig.cpp is intentionally reused by focused test executables, so the
# generated build identity must be visible to every target in this directory.
include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated")

if(APPLE)
    configure_file(
        Info.plist
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Info.plist"
        @ONLY)
endif()

# Enable Objective-C++ on macOS
if(APPLE)
    enable_language(OBJCXX)
endif()

# Find Qt6
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Widgets Network WebSockets Multimedia MultimediaWidgets Svg SvgWidgets Concurrent Quick QuickWidgets Qml ShaderTools GuiPrivate QuickPrivate)
find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)
if(NOT TARGET Qt6::QWebpPlugin)
    message(FATAL_ERROR
        "Mouffette advertises WebP as an accepted canvas/upload format, but the "
        "selected Qt installation has no QWebp image plugin. Install the matching "
        "Qt Image Formats package before building.")
endif()
if(UNIX AND NOT APPLE)
    # systemd-logind is the native source for sleep and session-lock events.
    # Keep it optional so non-systemd Linux targets still build with a no-op
    # backend while exposing the same application-level lifecycle signal.
    find_package(Qt6 6.11 QUIET COMPONENTS DBus)
endif()

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
    src/frontend/ui/pages/HistoryPage.cpp
    
    # UI - Widgets
    src/frontend/ui/widgets/RoundedContainer.cpp
    src/frontend/ui/widgets/ClippedContainer.cpp
    src/frontend/ui/widgets/ClientListDelegate.cpp
    src/frontend/ui/widgets/SpinnerWidget.cpp
    src/frontend/ui/widgets/QtWaitingSpinner.cpp
    src/frontend/ui/widgets/BootstrapWindow.cpp
    
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

    # Protocol v3 scene lifecycle
    src/backend/network/RemoteSessionCoordinator.cpp
    src/backend/network/SceneRunCoordinator.cpp
    
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
    
    # Configuration
    src/backend/config/AppConfig.cpp

    # Runtime profile / instance coordination
    src/backend/runtime/RuntimeProfile.cpp
    src/backend/runtime/ApplicationInstanceManager.cpp
    src/backend/runtime/RuntimeStorageBootstrap.cpp

    # Security
    src/backend/security/DeviceIdentityStore.cpp

    # Domain - Models
    src/backend/domain/models/ClientInfo.cpp

    # Domain - Projects
    src/backend/domain/project/ProjectModel.cpp
    src/backend/domain/project/ProjectStore.cpp
    src/backend/domain/project/ProjectManager.cpp

    # Domain - Live scene activity
    src/backend/domain/scene/SceneActivityModel.cpp
    
    # Domain - Session
    src/backend/domain/session/SessionManager.cpp

    # Notifications
    src/backend/notifications/HistoryStore.cpp
    src/backend/notifications/NotificationCenter.cpp
    
    # Domain - Media
    src/backend/domain/media/MediaItems.cpp
    src/backend/domain/media/MediaFilePolicy.cpp
    src/backend/domain/media/MediaRuntimeHooks.cpp
    src/backend/domain/media/TextRenderState.cpp
    src/backend/domain/media/TextMediaItem.cpp

    # UI - Overlays (Canvas)
    src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.cpp
    src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.cpp
    
    # Network
    src/backend/network/WebSocketClient.cpp
    src/backend/network/UploadManager.cpp
    src/backend/network/RemoteCacheStore.cpp
    src/backend/network/UploadScheduler.cpp
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
    src/backend/managers/system/SystemLifecycleMonitor.cpp
    
    # Controllers
    src/backend/controllers/CanvasSessionController.cpp
    src/backend/controllers/TimerController.cpp
)

# Platform-specific sources
if(APPLE)
    list(APPEND SOURCES 
        src/backend/platform/macos/MacVideoThumbnailer.mm 
        src/backend/platform/macos/MacWindowManager.mm
        src/backend/managers/system/SystemLifecycleMonitor_mac.mm
    )
endif()

if(WIN32)
    list(APPEND SOURCES
        src/backend/platform/windows/WindowsVideoThumbnailer.cpp
        src/backend/managers/system/SystemLifecycleMonitor_windows.cpp
    )
elseif(UNIX AND NOT APPLE AND TARGET Qt6::DBus)
    list(APPEND SOURCES src/backend/managers/system/SystemLifecycleMonitor_linux.cpp)
elseif(NOT APPLE)
    list(APPEND SOURCES src/backend/managers/system/SystemLifecycleMonitorStub.cpp)
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
    src/frontend/ui/pages/HistoryPage.h
    
    # UI - Widgets
    src/frontend/ui/widgets/RoundedContainer.h
    src/frontend/ui/widgets/ClippedContainer.h
    src/frontend/ui/widgets/ClientListDelegate.h
    src/frontend/ui/widgets/SpinnerWidget.h
    src/frontend/ui/widgets/QtWaitingSpinner.h
    src/frontend/ui/widgets/BootstrapWindow.h
    
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

    # Protocol v3 scene lifecycle
    src/backend/network/RemoteSessionCoordinator.h
    src/backend/network/SceneRunCoordinator.h
    
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
    
    # Configuration
    src/backend/config/AppConfig.h

    # Runtime profile / instance coordination
    src/backend/runtime/RuntimeProfile.h
    src/backend/runtime/ApplicationInstanceManager.h
    src/backend/runtime/RuntimeStorageBootstrap.h

    # Security
    src/backend/security/DeviceIdentityStore.h

    # Domain - Models
    src/backend/domain/models/ClientInfo.h

    # Domain - Projects
    src/backend/domain/project/ProjectModel.h
    src/backend/domain/project/ProjectStore.h
    src/backend/domain/project/ProjectManager.h

    # Domain - Live scene activity
    src/backend/domain/scene/SceneActivityModel.h
    
    # Domain - Session
    src/backend/domain/session/SessionManager.h

    # Notifications
    src/backend/notifications/HistoryStore.h
    src/backend/notifications/NotificationCenter.h
    
    # Domain - Media
    src/backend/domain/media/MediaItems.h
    src/backend/domain/media/MediaFilePolicy.h
    src/backend/domain/media/MediaRuntimeHooks.h
    src/backend/domain/media/TextRenderState.h
    src/backend/domain/media/TextMediaItem.h
    
    # Network
    src/backend/network/WebSocketClient.h
    src/backend/network/UploadManager.h
    src/backend/network/RemoteCacheStore.h
    src/backend/network/UploadScheduler.h
    src/backend/network/RemoteFileTracker.h
    
    # Files
    src/backend/files/FileManager.h
    src/backend/files/LocalFileRepository.h
    src/backend/files/FileMemoryCache.h
    src/backend/files/FileWatcher.h
    src/backend/files/PathSafety.h
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
    src/backend/managers/system/SystemLifecycleMonitor.h
    src/backend/managers/system/SystemLifecycleMonitorBackend.h
    
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
set_target_properties(MouffetteClient PROPERTIES
    OUTPUT_NAME "${MOUFFETTE_EXECUTABLE_NAME}")

if(WIN32)
    configure_file(
        cmake/MouffetteVersion.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/MouffetteVersion.rc"
        @ONLY)
    target_sources(MouffetteClient PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/MouffetteVersion.rc")
endif()

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
    OpenSSL::Crypto
)

# Include directories
target_include_directories(MouffetteClient PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

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
        evr
        mfplat
        mfreadwrite
        mfuuid
        Advapi32
        Wtsapi32
    )
endif()

if(UNIX AND NOT APPLE AND TARGET Qt6::DBus)
    target_link_libraries(MouffetteClient Qt6::DBus)
endif()

if(APPLE)
    set_target_properties(MouffetteClient PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/generated/Info.plist"
        MACOSX_BUNDLE_BUNDLE_NAME "${MOUFFETTE_APPLICATION_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${MOUFFETTE_BUNDLE_IDENTIFIER}"
    )
    # Link with macOS frameworks used by native integrations
    target_link_libraries(MouffetteClient "-framework AppKit" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreGraphics" "-framework Security")
    
    # Set Objective-C++ properties for .mm files (updated paths for backend/platform)
    set_source_files_properties(src/backend/platform/macos/MacVideoThumbnailer.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/backend/platform/macos/MacWindowManager.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/backend/managers/system/SystemLifecycleMonitor_mac.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

# The install tree is the only input accepted by the packaging scripts. Runtime
# deployment happens in that disposable tree, never in the compiler output.
install(TARGETS MouffetteClient
    BUNDLE DESTINATION .
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(WIN32)
    qt_generate_deploy_app_script(
        TARGET MouffetteClient
        OUTPUT_SCRIPT MOUFFETTE_DEPLOY_SCRIPT
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${MOUFFETTE_DEPLOY_SCRIPT}")
endif()

if(WIN32)
    set(CPACK_PACKAGE_NAME "Mouffette")
    set(CPACK_PACKAGE_VENDOR "Mouffette")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Mouffette desktop client")
    set(CPACK_PACKAGE_FILE_NAME
        "Mouffette-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    # A portable ZIP has no external installer-tool dependency.
    set(CPACK_GENERATOR "ZIP")
    include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(tst_DeviceIdentityStore
        tests/qt/tst_DeviceIdentityStore.cpp
        src/backend/security/DeviceIdentityStore.cpp
        src/backend/security/DeviceIdentityStore.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
    )
    target_include_directories(tst_DeviceIdentityStore PRIVATE src)
    target_link_libraries(tst_DeviceIdentityStore PRIVATE Qt6::Core Qt6::Test OpenSSL::Crypto)
    if(APPLE)
        target_link_libraries(tst_DeviceIdentityStore PRIVATE "-framework Security")
    elseif(WIN32)
        target_link_libraries(tst_DeviceIdentityStore PRIVATE Advapi32)
    endif()
    add_test(NAME DeviceIdentityStore COMMAND tst_DeviceIdentityStore)

    add_executable(tst_ConnectionManager
        tests/qt/tst_ConnectionManager.cpp
        src/backend/managers/network/ConnectionManager.cpp
        src/backend/managers/network/ConnectionManager.h
        src/backend/network/WebSocketClient.cpp
        src/backend/network/WebSocketClient.h
        src/backend/network/SceneRunCoordinator.cpp
        src/backend/network/SceneRunCoordinator.h
        src/backend/network/RemoteSessionCoordinator.cpp
        src/backend/network/RemoteSessionCoordinator.h
        src/backend/security/DeviceIdentityStore.cpp
        src/backend/security/DeviceIdentityStore.h
        src/backend/domain/models/ClientInfo.cpp
        src/backend/domain/models/ClientInfo.h
        src/backend/config/AppConfig.cpp
        src/backend/config/AppConfig.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
        resources/resources.qrc
    )
    target_include_directories(tst_ConnectionManager PRIVATE src)
    target_link_libraries(tst_ConnectionManager PRIVATE
        Qt6::Core Qt6::Network Qt6::WebSockets Qt6::Test OpenSSL::Crypto)
    if(APPLE)
        target_link_libraries(tst_ConnectionManager PRIVATE "-framework Security")
    elseif(WIN32)
        target_link_libraries(tst_ConnectionManager PRIVATE Advapi32)
    endif()
    add_test(NAME ConnectionManager COMMAND tst_ConnectionManager)

    add_executable(tst_AppConfig
        tests/qt/tst_AppConfig.cpp
        src/backend/config/AppConfig.cpp
        src/backend/config/AppConfig.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
        resources/resources.qrc
    )
    target_include_directories(tst_AppConfig PRIVATE src)
    target_link_libraries(tst_AppConfig PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME AppConfig COMMAND tst_AppConfig)

    add_executable(tst_ApplicationInstanceManager
        tests/qt/tst_ApplicationInstanceManager.cpp
        src/backend/runtime/ApplicationInstanceManager.cpp
        src/backend/runtime/ApplicationInstanceManager.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
    )
    target_include_directories(tst_ApplicationInstanceManager PRIVATE src)
    target_link_libraries(tst_ApplicationInstanceManager PRIVATE
        Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME ApplicationInstanceManager COMMAND tst_ApplicationInstanceManager)

    add_executable(tst_RuntimeStorageBootstrap
        tests/qt/tst_RuntimeStorageBootstrap.cpp
        src/backend/runtime/RuntimeStorageBootstrap.cpp
        src/backend/runtime/RuntimeStorageBootstrap.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
        src/backend/security/DeviceIdentityStore.cpp
        src/backend/security/DeviceIdentityStore.h
        src/backend/config/AppConfig.cpp
        src/backend/config/AppConfig.h
        src/backend/domain/models/ClientInfo.cpp
        src/backend/domain/models/ClientInfo.h
        src/backend/domain/project/ProjectModel.cpp
        src/backend/domain/project/ProjectModel.h
        src/backend/domain/project/ProjectStore.cpp
        src/backend/domain/project/ProjectStore.h
    )
    target_include_directories(tst_RuntimeStorageBootstrap PRIVATE src)
    target_link_libraries(tst_RuntimeStorageBootstrap PRIVATE
        Qt6::Core Qt6::Network Qt6::Test OpenSSL::Crypto)
    if(APPLE)
        target_link_libraries(tst_RuntimeStorageBootstrap PRIVATE "-framework Security")
    elseif(WIN32)
        target_link_libraries(tst_RuntimeStorageBootstrap PRIVATE Advapi32)
    endif()
    add_test(NAME RuntimeStorageBootstrap COMMAND tst_RuntimeStorageBootstrap)

    add_executable(tst_ClientInfoDisplay
        tests/qt/tst_ClientInfoDisplay.cpp
        src/backend/domain/models/ClientInfo.cpp
        src/backend/domain/models/ClientInfo.h
    )
    target_include_directories(tst_ClientInfoDisplay PRIVATE src)
    target_link_libraries(tst_ClientInfoDisplay PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ClientInfoDisplay COMMAND tst_ClientInfoDisplay)

    add_executable(tst_SystemLifecycleMonitor
        tests/qt/tst_SystemLifecycleMonitor.cpp
        src/backend/managers/system/SystemLifecycleMonitor.cpp
        src/backend/managers/system/SystemLifecycleMonitor.h
        src/backend/managers/system/SystemLifecycleMonitorBackend.h
        src/backend/managers/system/SystemLifecycleMonitorStub.cpp
    )
    target_include_directories(tst_SystemLifecycleMonitor PRIVATE src)
    target_link_libraries(tst_SystemLifecycleMonitor PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME SystemLifecycleMonitor COMMAND tst_SystemLifecycleMonitor)

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

    add_executable(tst_CanvasInteraction
        tests/qt/tst_CanvasInteraction.cpp
        src/frontend/rendering/canvas/CanvasQmlTypes.cpp
        src/frontend/rendering/canvas/MediaListModel.cpp
        src/frontend/rendering/canvas/TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextEditHelper.cpp
        src/frontend/rendering/remote/RemoteVideoFrameItem.cpp
        resources/resources.qrc
    )
    target_include_directories(tst_CanvasInteraction PRIVATE src)
    target_compile_definitions(tst_CanvasInteraction PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_CanvasInteraction PRIVATE Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Multimedia Qt6::Test)
    add_test(NAME CanvasInteraction COMMAND tst_CanvasInteraction)
    add_test(NAME CanvasInteractionScaled COMMAND tst_CanvasInteraction)
    set_tests_properties(CanvasInteractionScaled PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

    # Exercise the real controller, scene and packaged QML without production
    # main() (which initializes services and clears upload caches). Reuse the
    # app's compiled objects and dependencies rather than duplicating its source
    # list. Only the exact application entry-point object is excluded.
    add_executable(tst_CanvasSelectionBackend
        tests/qt/tst_CanvasSelectionBackend.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_CanvasSelectionBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_CanvasSelectionBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/../video-1080p.mp4"
        TEST_WEBP_FILE="${CMAKE_SOURCE_DIR}/../image-1920.webp")
    target_link_libraries(tst_CanvasSelectionBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME CanvasSelectionBackend COMMAND tst_CanvasSelectionBackend)

    add_executable(tst_VideoPlaybackBackend
        tests/qt/tst_VideoPlaybackBackend.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_VideoPlaybackBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_VideoPlaybackBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/../video-1080p.mp4")
    target_link_libraries(tst_VideoPlaybackBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME VideoPlaybackBackend COMMAND tst_VideoPlaybackBackend)

    add_executable(tst_UploadRemovalSecurity
        tests/qt/tst_UploadRemovalSecurity.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_UploadRemovalSecurity PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_UploadRemovalSecurity PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_WEBP_FILE="${CMAKE_SOURCE_DIR}/../image-1920.webp")
    target_link_libraries(tst_UploadRemovalSecurity PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME UploadRemovalSecurity COMMAND tst_UploadRemovalSecurity)

    add_executable(tst_RemoteSceneControllerLifecycle
        tests/qt/tst_RemoteSceneControllerLifecycle.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_RemoteSceneControllerLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_RemoteSceneControllerLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/../video-1080p.mp4")
    target_link_libraries(tst_RemoteSceneControllerLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME RemoteSceneControllerLifecycle COMMAND tst_RemoteSceneControllerLifecycle)

    add_executable(tst_RemoteSceneLifecycle
        tests/qt/tst_RemoteSceneLifecycle.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_RemoteSceneLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_RemoteSceneLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/../video-1080p.mp4")
    target_link_libraries(tst_RemoteSceneLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME RemoteSceneLifecycle COMMAND tst_RemoteSceneLifecycle)

    add_executable(tst_SceneRunCoordinator
        tests/qt/tst_SceneRunCoordinator.cpp
        src/backend/network/SceneRunCoordinator.cpp
        src/backend/network/SceneRunCoordinator.h
        src/backend/network/RemoteSessionCoordinator.cpp
        src/backend/network/RemoteSessionCoordinator.h
    )
    target_include_directories(tst_SceneRunCoordinator PRIVATE src)
    target_link_libraries(tst_SceneRunCoordinator PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME SceneRunCoordinator COMMAND tst_SceneRunCoordinator)
    # Native window focus/grabs are part of these tests. Do not race other GUI
    # suites when CI invokes ctest with parallel jobs.
    set_tests_properties(TextOutlineMotion CanvasInteraction CanvasInteractionScaled CanvasSelectionBackend VideoPlaybackBackend UploadRemovalSecurity RemoteSceneControllerLifecycle RemoteSceneLifecycle
        PROPERTIES RUN_SERIAL TRUE)

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

    add_executable(tst_MediaFilePolicy
        tests/qt/tst_MediaFilePolicy.cpp
        src/backend/domain/media/MediaFilePolicy.cpp
        src/backend/domain/media/MediaFilePolicy.h
    )
    target_include_directories(tst_MediaFilePolicy PRIVATE src)
    target_compile_definitions(tst_MediaFilePolicy PRIVATE
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/../video-1080p.mp4"
        TEST_WEBP_FILE="${CMAKE_SOURCE_DIR}/../image-1920.webp")
    target_link_libraries(tst_MediaFilePolicy PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME MediaFilePolicy COMMAND tst_MediaFilePolicy)
    set(MOUFFETTE_MEDIA_POLICY_TEST_ENV "QT_MEDIA_BACKEND=ffmpeg")
    if(NOT APPLE)
        # Headless Unix CI needs a platform plugin. AVFoundation on macOS,
        # however, cannot deliver QVideoSink frames through the offscreen
        # plugin, so macOS uses its native GUI context without showing a window.
        list(APPEND MOUFFETTE_MEDIA_POLICY_TEST_ENV "QT_QPA_PLATFORM=offscreen")
    endif()
    set_tests_properties(MediaFilePolicy UploadRemovalSecurity
        PROPERTIES ENVIRONMENT "${MOUFFETTE_MEDIA_POLICY_TEST_ENV}")

    add_executable(tst_LocalFileRepository
        tests/qt/tst_LocalFileRepository.cpp
        src/backend/files/LocalFileRepository.cpp
        src/backend/files/LocalFileRepository.h
    )
    target_include_directories(tst_LocalFileRepository PRIVATE src)
    target_link_libraries(tst_LocalFileRepository PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME LocalFileRepository COMMAND tst_LocalFileRepository)

    add_executable(tst_ProjectManager
        tests/qt/tst_ProjectManager.cpp
        src/backend/domain/models/ClientInfo.cpp
        src/backend/domain/models/ClientInfo.h
        src/backend/domain/project/ProjectModel.cpp
        src/backend/domain/project/ProjectModel.h
        src/backend/domain/project/ProjectStore.cpp
        src/backend/domain/project/ProjectStore.h
        src/backend/domain/project/ProjectManager.cpp
        src/backend/domain/project/ProjectManager.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
    )
    target_include_directories(tst_ProjectManager PRIVATE src)
    target_link_libraries(tst_ProjectManager PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ProjectManager COMMAND tst_ProjectManager)

    add_executable(tst_NotificationCenter
        tests/qt/tst_NotificationCenter.cpp
        src/backend/notifications/HistoryStore.cpp
        src/backend/notifications/HistoryStore.h
        src/backend/notifications/NotificationCenter.cpp
        src/backend/notifications/NotificationCenter.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
    )
    target_include_directories(tst_NotificationCenter PRIVATE src)
    target_link_libraries(tst_NotificationCenter PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME NotificationCenter COMMAND tst_NotificationCenter)

    add_executable(tst_SceneActivityModel
        tests/qt/tst_SceneActivityModel.cpp
        src/backend/domain/scene/SceneActivityModel.cpp
        src/backend/domain/scene/SceneActivityModel.h
    )
    target_include_directories(tst_SceneActivityModel PRIVATE src)
    target_link_libraries(tst_SceneActivityModel PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME SceneActivityModel COMMAND tst_SceneActivityModel)

    add_executable(tst_UploadScheduler
        tests/qt/tst_UploadScheduler.cpp
        src/backend/network/UploadScheduler.cpp
        src/backend/network/UploadScheduler.h
    )
    target_include_directories(tst_UploadScheduler PRIVATE src)
    target_link_libraries(tst_UploadScheduler PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME UploadScheduler COMMAND tst_UploadScheduler)

    add_executable(tst_RemoteCacheStore
        tests/qt/tst_RemoteCacheStore.cpp
        src/backend/network/RemoteCacheStore.cpp
        src/backend/network/RemoteCacheStore.h
        src/backend/files/PathSafety.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
    )
    target_include_directories(tst_RemoteCacheStore PRIVATE src)
    target_link_libraries(tst_RemoteCacheStore PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test)
    add_test(NAME RemoteCacheStore COMMAND tst_RemoteCacheStore)
endif()
