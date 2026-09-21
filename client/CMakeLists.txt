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
# Display/application names remain common. RuntimeProfile isolates durable
# data and credential namespaces by MOUFFETTE_BUILD_CHANNEL; package/release
# versions never select storage compatibility.
set(MOUFFETTE_APPLICATION_NAME "Mouffette")
set(MOUFFETTE_SETTINGS_APPLICATION "Client")
set(MOUFFETTE_EXECUTABLE_NAME "Mouffette")
set(MOUFFETTE_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/logo")
set(MOUFFETTE_MACOS_ICON_FILE "mouffette.icns")

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
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Widgets Network WebSockets Multimedia Concurrent Quick QuickControls2 QuickDialogs2 Qml ShaderTools GuiPrivate QuickPrivate MultimediaPrivate)
qt_policy(SET QTP0004 NEW)
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
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat>=61 libavcodec>=61 libavutil libswscale libswresample)
add_library(MouffetteResidentMedia STATIC
    src/backend/media/MediaBackendBootstrap.h
    src/backend/media/MediaBackendBootstrap.cpp
    src/backend/media/PlaybackAudio.h
    src/backend/media/PlaybackAudio.cpp
    src/backend/media/DecodeScheduler.h
    src/backend/media/DecodeScheduler.cpp
    src/backend/media/MediaPacketStore.h
    src/backend/media/IndexedMediaDecoder.h
    src/backend/media/IndexedMediaDecoder.cpp
    src/backend/media/MediaPreviewStore.h
    src/backend/media/MediaPreviewStore.cpp
    src/backend/media/EditingProxyCache.h
    src/backend/media/EditingProxyCache.cpp
    src/backend/media/ResidentMediaAsset.h
    src/backend/media/MediaDecoder.h
    src/backend/media/MediaDecoder.cpp
    src/backend/media/ResidentVideoPlayer.h
    src/backend/media/ResidentVideoPlayer.cpp
    src/backend/media/MediaResidencyManager.h
    src/backend/media/MediaResidencyManager.cpp
    src/backend/domain/media/MediaFilePolicy.cpp)
target_include_directories(MouffetteResidentMedia PUBLIC src)
target_link_libraries(MouffetteResidentMedia PUBLIC
    Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Concurrent PkgConfig::FFMPEG)
target_link_libraries(MouffetteResidentMedia PUBLIC Qt6::MultimediaPrivate Qt6::GuiPrivate)
if(WIN32)
    target_link_libraries(MouffetteResidentMedia PRIVATE Psapi)
endif()


# Shared by the application and focused storage tests; migrations are registered
# explicitly, never discovered by a filesystem glob.
set(MOUFFETTE_STORAGE_SOURCES
    src/backend/runtime/RuntimeStorageBootstrap.cpp
    src/backend/runtime/InstallationIdentityBootstrap.cpp
    src/backend/runtime/storage/StorageUpgradeEngine.cpp
    src/backend/runtime/storage/StorageIO.cpp
    src/backend/runtime/storage/StorageRegistry.cpp
    src/backend/runtime/storage/migrations/settings/v0_to_v1.cpp
    src/backend/runtime/storage/migrations/history/v1_to_v2.cpp
    src/backend/runtime/storage/migrations/projects/v6_to_v7.cpp
)

# Source files - Frontend/Backend Architecture
set(SOURCES
    # ═══════════════════════════════════════════════════════
    # CORE
    # ═══════════════════════════════════════════════════════
    src/main.cpp
    src/backend/runtime/ApplicationRuntime.cpp
    src/backend/runtime/ApplicationActivityMonitor.cpp
    src/backend/screensharing/ScreenSharingService.cpp
    src/backend/screensharing/ScreenCaptureSource.cpp
    src/backend/screensharing/ScreenStreamCodec.cpp

    # Qt Quick application shell and presentation models
    src/frontend/qml/ApplicationController.cpp
    src/frontend/qml/ClientWorkspaceViewModel.cpp
    src/frontend/qml/ClientListModel.cpp
    src/frontend/qml/NotificationListModels.cpp
    src/frontend/qml/MediaSettingsViewModel.cpp
    src/frontend/qml/TimelineController.cpp
    src/frontend/qml/QmlRuntime.cpp
    src/frontend/qml/WindowPresentation.cpp
    src/backend/platform/WindowStackingCoordinator.cpp
    src/backend/platform/WindowStackingCoordinator.h
    src/frontend/qml/WindowPresentation.h
    src/frontend/qml/SceneActivityListModel.cpp
    
    # ═══════════════════════════════════════════════════════
    # FRONTEND - User Interface & Rendering
    # ═══════════════════════════════════════════════════════
    
    # The application shell, pages, dialogs, controls and toast surfaces are QML.
    
    # UI - Notifications
    src/frontend/ui/notifications/ToastNotificationSystem.cpp
    
    # Rendering - Canvas
    src/frontend/rendering/canvas/QuickCanvasHost.cpp
    src/frontend/rendering/canvas/QuickCanvasController.cpp
    src/frontend/rendering/canvas/MediaListModel.cpp
    src/frontend/rendering/canvas/TextOutlineItem.cpp
    src/frontend/rendering/canvas/TextEditHelper.cpp
    src/frontend/rendering/canvas/CanvasQmlTypes.cpp
    
    # Rendering - Remote
    src/frontend/rendering/remote/RemoteSceneController.cpp
    src/frontend/rendering/remote/RemoteVideoFrameItem.cpp
    src/frontend/rendering/canvas/TimelineThumbnailItem.cpp
    src/shared/rendering/MediaFrameSource.cpp

    # Protocol v4 scene lifecycle
    src/backend/network/RemoteSessionCoordinator.cpp
    src/backend/network/SceneRunCoordinator.cpp
    
    # Rendering - Navigation
    src/frontend/rendering/navigation/ScreenNavigationManager.cpp
    
    # Frontend Handlers
    src/frontend/handlers/UploadSignalConnector.cpp
    
    # ═══════════════════════════════════════════════════════
    # BACKEND - Business Logic & Data
    # ═══════════════════════════════════════════════════════
    
    # Configuration
    src/backend/config/AppConfig.cpp

    # Runtime profile / instance coordination
    src/backend/runtime/RuntimeProfile.cpp
    src/backend/runtime/ApplicationInstanceManager.cpp
    ${MOUFFETTE_STORAGE_SOURCES}

    # Security
    src/backend/security/DeviceIdentityStore.cpp

    # Domain - Models
    src/backend/domain/models/ClientInfo.cpp
    src/backend/domain/profile/ProfileImage.cpp
    src/backend/domain/profile/ClientProfileCache.cpp

    # Domain - Projects
    src/backend/domain/project/ProjectModel.cpp
    src/backend/domain/project/ProjectStore.cpp
    src/backend/domain/project/ProjectManager.cpp

    # Domain - Live scene activity
    src/backend/domain/scene/SceneActivityModel.cpp
    src/backend/domain/scene/SceneTimeline.cpp
    
    # Domain - Session
    src/backend/domain/session/IncomingSessionOrphanWatchdog.cpp
    src/backend/domain/workspace/WorkspaceManager.cpp

    # Notifications
    src/backend/notifications/HistoryStore.cpp
    src/backend/notifications/NotificationCenter.cpp
    
    # Domain - Media
    src/backend/domain/canvas/CanvasDocument.cpp
    src/backend/domain/media/CanvasMedia.cpp
    src/backend/domain/media/MediaSettingsState.cpp
    src/backend/domain/media/TextRenderState.cpp
    
    # Network
    src/backend/network/WebSocketClient.cpp
    src/backend/network/UploadManager.cpp
    src/backend/network/RemoteCacheStore.cpp
    src/backend/network/UploadScheduler.cpp
    src/backend/network/RemoteFileTracker.cpp
    
    # Files
    src/backend/files/FileManager.cpp
    src/backend/files/LocalFileRepository.cpp
    src/backend/files/FileWatcher.cpp
    
    # Backend Handlers
    src/backend/handlers/WebSocketMessageHandler.cpp
    src/backend/handlers/ScreenEventHandler.cpp
    src/backend/handlers/ClientListEventHandler.cpp
    src/backend/handlers/UploadEventHandler.cpp
    
    # Backend Managers - App
    src/backend/managers/app/SettingsManager.cpp
    src/backend/managers/app/SystemTrayManager.cpp
    
    # Backend Managers - Network
    src/backend/managers/network/ConnectionManager.cpp
    src/backend/managers/network/ClientListBuilder.cpp
    
    # Backend Managers - System
    src/backend/managers/system/SystemMonitor.cpp
    src/backend/platform/LocalScreenTopology.cpp
    src/backend/platform/LocalScreenTopology.h
    src/backend/managers/system/SystemLifecycleMonitor.cpp
    
    # Controllers
    src/backend/controllers/ClientWorkspaceController.cpp
)

# Platform-specific sources
if(APPLE)
    list(APPEND SOURCES 
        src/backend/platform/macos/MacWindowManager.mm
        src/backend/managers/system/SystemLifecycleMonitor_mac.mm
        src/backend/screensharing/MacScreenCapture.mm
    )
endif()

if(WIN32)
    list(APPEND SOURCES
        src/backend/platform/windows/WindowsWindowManager.cpp
        src/backend/managers/system/SystemLifecycleMonitor_windows.cpp
    )
elseif(UNIX AND NOT APPLE AND TARGET Qt6::DBus)
    list(APPEND SOURCES src/backend/managers/system/SystemLifecycleMonitor_linux.cpp)
elseif(NOT APPLE)
    list(APPEND SOURCES src/backend/managers/system/SystemLifecycleMonitorStub.cpp)
endif()

# Native system volume monitoring is shared by the application and isolated tests.
if(APPLE)
    set(MOUFFETTE_VOLUME_BACKEND src/backend/managers/system/SystemVolumeMonitor_mac.mm)
elseif(WIN32)
    set(MOUFFETTE_VOLUME_BACKEND src/backend/managers/system/SystemVolumeMonitor_windows.cpp)
else()
    set(MOUFFETTE_VOLUME_BACKEND src/backend/managers/system/SystemVolumeMonitor_stub.cpp)
endif()
list(APPEND SOURCES ${MOUFFETTE_VOLUME_BACKEND})

# Header files - Frontend/Backend Architecture
set(HEADERS
    # ═══════════════════════════════════════════════════════
    # CORE
    # ═══════════════════════════════════════════════════════
    src/backend/runtime/ApplicationRuntime.h

    # Qt Quick application shell and presentation models
    src/frontend/qml/ApplicationController.h
    src/frontend/qml/ClientWorkspaceViewModel.h
    src/frontend/qml/ClientListModel.h
    src/frontend/qml/NotificationListModels.h
    src/frontend/qml/MediaSettingsViewModel.h
    src/frontend/qml/TimelineController.h
    src/frontend/qml/QmlRuntime.h
    src/frontend/qml/SceneActivityListModel.h
    
    # ═══════════════════════════════════════════════════════
    # FRONTEND - User Interface & Rendering
    # ═══════════════════════════════════════════════════════
    
    # UI - Notifications
    src/frontend/ui/notifications/ToastNotificationSystem.h

    # Rendering - Canvas
    src/frontend/rendering/canvas/QuickCanvasHost.h
    src/frontend/rendering/canvas/QuickCanvasController.h
    src/frontend/rendering/canvas/MediaListModel.h
    src/frontend/rendering/canvas/TextOutlineItem.h
    src/frontend/rendering/canvas/TextEditHelper.h
    src/frontend/rendering/canvas/CanvasQmlTypes.h

    # Shared - Rendering Contracts
    src/shared/rendering/ICanvasHost.h
    
    # Rendering - Remote
    src/frontend/rendering/remote/RemoteSceneController.h
    src/frontend/rendering/remote/RemoteVideoFrameItem.h
    src/frontend/rendering/canvas/TimelineThumbnailItem.h

    # Protocol v3 scene lifecycle
    src/backend/network/RemoteSessionCoordinator.h
    src/backend/network/SceneRunCoordinator.h
    
    # Rendering - Navigation
    src/frontend/rendering/navigation/ScreenNavigationManager.h
    
    # Frontend Handlers
    src/frontend/handlers/UploadSignalConnector.h
    
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
    src/backend/domain/workspace/WorkspaceManager.h
    src/backend/domain/session/IncomingSessionOrphanWatchdog.h

    # Notifications
    src/backend/notifications/HistoryStore.h
    src/backend/notifications/NotificationCenter.h
    
    # Domain - Media
    src/backend/domain/canvas/CanvasDocument.h
    src/backend/domain/media/CanvasMedia.h
    src/backend/domain/media/MediaSettingsState.h
    src/backend/domain/media/MediaFilePolicy.h
    src/backend/domain/media/TextRenderState.h
    
    # Network
    src/backend/network/WebSocketClient.h
    src/backend/network/UploadManager.h
    src/backend/network/RemoteCacheStore.h
    src/backend/network/UploadScheduler.h
    src/backend/network/RemoteFileTracker.h
    
    # Files
    src/backend/files/FileManager.h
    src/backend/files/LocalFileRepository.h
    src/backend/files/FileWatcher.h
    src/backend/files/PathSafety.h
    
    # Backend Handlers
    src/backend/handlers/WebSocketMessageHandler.h
    src/backend/handlers/ScreenEventHandler.h
    src/backend/handlers/ClientListEventHandler.h
    src/backend/handlers/UploadEventHandler.h
    
    # Backend Managers - App
    src/backend/managers/app/SettingsManager.h
    src/backend/managers/app/SystemTrayManager.h
    
    # Backend Managers - Network
    src/backend/managers/network/ConnectionManager.h
    src/backend/managers/network/ClientListBuilder.h
    
    # Backend Managers - System
    src/backend/managers/system/SystemMonitor.h
    src/backend/managers/system/SystemLifecycleMonitor.h
    src/backend/managers/system/SystemLifecycleMonitorBackend.h
    
    # Controllers
    src/backend/controllers/ClientWorkspaceController.h
)

# Platform-specific headers
if(APPLE)
    list(APPEND HEADERS
        src/backend/platform/macos/MacWindowManager.h
    )
endif()

if(WIN32)
endif()

# Resource files
set(RESOURCE_FILES
    resources/resources.qrc
)

# Share the compiled shader resource with standalone canvas/rendering tests.
add_library(MouffetteCanvasShaders OBJECT)
target_link_libraries(MouffetteCanvasShaders PRIVATE Qt6::Core)
qt_add_shaders(MouffetteCanvasShaders "canvas_shaders"
    PREFIX "/shaders"
    BASE "resources/shaders"
    FILES resources/shaders/transparency-checkerboard.frag)

# Create executable
add_executable(MouffetteClient ${SOURCES} ${HEADERS} ${RESOURCE_FILES})
set_target_properties(MouffetteClient PROPERTIES
    OUTPUT_NAME "${MOUFFETTE_EXECUTABLE_NAME}")

set_source_files_properties(
    resources/qml/app/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(MouffetteClient
    URI Mouffette.App
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    NO_GENERATE_QMLTYPES
    QML_FILES
        resources/qml/app/Theme.qml
        resources/qml/app/Main.qml
        resources/qml/app/components/AppButton.qml
        resources/qml/app/components/ProfilePicture.qml
        resources/qml/app/components/NotificationPeers.qml
        resources/qml/app/components/AppCheckBox.qml
        resources/qml/app/components/AppDialog.qml
        resources/qml/app/components/AppPanel.qml
        resources/qml/app/components/AppSpinner.qml
        resources/qml/app/components/AppTextField.qml
        resources/qml/app/components/StateTextMetrics.qml
        resources/qml/app/components/ConnectionStatusMetrics.qml
        resources/qml/app/components/SegmentedStatusCard.qml
        resources/qml/app/components/ToastStack.qml
        resources/qml/app/pages/ClientListPanel.qml
        resources/qml/app/pages/ClientsPage.qml
        resources/qml/app/pages/CanvasPage.qml
        resources/qml/app/pages/HistoryPage.qml
        resources/qml/app/canvas/CanvasToolbar.qml
        resources/qml/app/canvas/MediaListPanel.qml
        resources/qml/app/canvas/OverlayActionButton.qml
        resources/qml/app/canvas/SceneElementPanel.qml
        resources/qml/app/canvas/TimelinePanel.qml
        resources/qml/app/canvas/TimelineClip.qml
        resources/qml/app/dialogs/BootstrapWindow.qml
        resources/qml/app/dialogs/MemoryUsagePopup.qml
        resources/qml/app/dialogs/SettingsDialog.qml
        resources/qml/BaseMediaItem.qml
        resources/qml/CanvasRoot.qml
        resources/qml/CanvasViewport.qml
        resources/qml/ImageItem.qml
        resources/qml/InputLayer.qml
        resources/qml/MediaLayer.qml
        resources/qml/MediaNamePill.qml
        resources/qml/MediaLoadingSkeleton.qml
        resources/qml/MediaSurface.qml
        resources/qml/MediaTextOverlay.qml
        resources/qml/MediaTopOverlay.qml
        resources/qml/MediaVideoOverlay.qml
        resources/qml/MediaVisual.qml
        resources/qml/OverlayButton.qml
        resources/qml/OverlaySlider.qml
        resources/qml/RemoteCursor.qml
        resources/qml/RemoteSceneRoot.qml
        resources/qml/RemoteSceneWindow.qml
        resources/qml/ScreenItem.qml
        resources/qml/SelectionChrome.qml
        resources/qml/SelectionLayer.qml
        resources/qml/SnapGuides.qml
        resources/qml/TextEditSession.qml
        resources/qml/TextItem.qml
        resources/qml/TransparencyCheckerboard.qml
        resources/qml/UiZone.qml
        resources/qml/VideoItem.qml)

if(WIN32)
    enable_language(RC)
    configure_file(
        cmake/MouffetteVersion.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/MouffetteVersion.rc"
        @ONLY)
    target_sources(MouffetteClient PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/MouffetteVersion.rc")
    target_include_directories(MouffetteClient PRIVATE "${MOUFFETTE_ICON_DIR}")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/MouffetteVersion.rc"
        APPEND PROPERTY OBJECT_DEPENDS "${MOUFFETTE_ICON_DIR}/mouffette.ico")
endif()

# Link Qt libraries
target_link_libraries(MouffetteClient PRIVATE
    MouffetteCanvasShaders
    MouffetteResidentMedia
    Qt6::Core 
    Qt6::Widgets 
    Qt6::Network 
    Qt6::WebSockets
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
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
    target_link_libraries(MouffetteClient PRIVATE
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
    target_link_libraries(MouffetteClient PRIVATE Qt6::DBus)
endif()

if(APPLE)
    set_source_files_properties("${MOUFFETTE_ICON_DIR}/${MOUFFETTE_MACOS_ICON_FILE}"
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(MouffetteClient PRIVATE
        "${MOUFFETTE_ICON_DIR}/${MOUFFETTE_MACOS_ICON_FILE}")
    set_target_properties(MouffetteClient PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "${MOUFFETTE_MACOS_ICON_FILE}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/generated/Info.plist"
        MACOSX_BUNDLE_BUNDLE_NAME "${MOUFFETTE_APPLICATION_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${MOUFFETTE_BUNDLE_IDENTIFIER}"
    )
    # Link with macOS frameworks used by native integrations
    target_link_libraries(MouffetteClient PRIVATE "-framework CoreAudio" "-framework AppKit" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreGraphics" "-framework Security")
    target_link_libraries(MouffetteClient PRIVATE "-framework ScreenCaptureKit" "-framework CoreVideo")
    set_source_files_properties(src/backend/screensharing/MacScreenCapture.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    
    # Set Objective-C++ properties for .mm files (updated paths for backend/platform)
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

    add_executable(tst_ScreenAdaptiveController tests/qt/tst_ScreenAdaptiveController.cpp)
    target_include_directories(tst_ScreenAdaptiveController PRIVATE src)
    target_link_libraries(tst_ScreenAdaptiveController PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ScreenAdaptiveController COMMAND tst_ScreenAdaptiveController)

    add_executable(tst_ScreenPublicationProfiles tests/qt/tst_ScreenPublicationProfiles.cpp)
    target_include_directories(tst_ScreenPublicationProfiles PRIVATE src)
    target_link_libraries(tst_ScreenPublicationProfiles PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME ScreenPublicationProfiles COMMAND tst_ScreenPublicationProfiles)

    add_executable(tst_ScreenCaptureLayers
        tests/qt/tst_ScreenCaptureLayers.cpp
        src/backend/screensharing/ScreenCaptureSource.h
        src/backend/screensharing/ScreenStreamCodec.cpp)
    target_include_directories(tst_ScreenCaptureLayers PRIVATE src)
    target_link_libraries(tst_ScreenCaptureLayers PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test PkgConfig::FFMPEG)
    if(APPLE)
        target_sources(tst_ScreenCaptureLayers PRIVATE src/backend/screensharing/MacScreenCapture.mm)
        target_link_libraries(tst_ScreenCaptureLayers PRIVATE "-framework ScreenCaptureKit"
            "-framework CoreVideo" "-framework CoreMedia" "-framework CoreGraphics" "-framework AppKit")
    endif()
    add_test(NAME ScreenCaptureLayers COMMAND tst_ScreenCaptureLayers)
    set_tests_properties(ScreenCaptureLayers PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_ScreenStreamCodec
        tests/qt/tst_ScreenStreamCodec.cpp
        src/backend/screensharing/ScreenStreamCodec.cpp)
    target_include_directories(tst_ScreenStreamCodec PRIVATE src)
    target_link_libraries(tst_ScreenStreamCodec PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test PkgConfig::FFMPEG)
    if(APPLE)
        target_sources(tst_ScreenStreamCodec PRIVATE src/backend/screensharing/MacScreenCapture.mm)
        target_link_libraries(tst_ScreenStreamCodec PRIVATE "-framework ScreenCaptureKit"
            "-framework CoreVideo" "-framework CoreMedia" "-framework CoreGraphics" "-framework AppKit")
    endif()
    add_test(NAME ScreenStreamCodec COMMAND tst_ScreenStreamCodec)

    if(APPLE)
        # Compile the native implementation inside this test to exercise its
        # asynchronous lifecycle with a fake stream, without desktop access.
        add_executable(tst_MacScreenCaptureLifecycle
            tests/qt/tst_MacScreenCaptureLifecycle.mm)
        set_source_files_properties(tests/qt/tst_MacScreenCaptureLifecycle.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_include_directories(tst_MacScreenCaptureLifecycle PRIVATE src)
        target_link_libraries(tst_MacScreenCaptureLifecycle PRIVATE
            Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test PkgConfig::FFMPEG
            "-framework ScreenCaptureKit" "-framework CoreVideo"
            "-framework CoreMedia" "-framework CoreGraphics" "-framework AppKit")
        add_test(NAME MacScreenCaptureLifecycle COMMAND tst_MacScreenCaptureLifecycle)
        set_tests_properties(MacScreenCaptureLifecycle PROPERTIES TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_executable(tst_WindowPresentation
        tests/qt/tst_WindowPresentation.cpp
        src/frontend/qml/WindowPresentation.cpp
        src/frontend/qml/WindowPresentation.h
        src/backend/platform/WindowStackingCoordinator.cpp
        src/backend/platform/WindowStackingCoordinator.h)
    target_include_directories(tst_WindowPresentation PRIVATE src)
    target_link_libraries(tst_WindowPresentation PRIVATE Qt6::Quick Qt6::Test)
    if(APPLE)
        add_executable(tst_FullscreenHost tests/qt/FullscreenTestHost.mm)
        target_link_libraries(tst_FullscreenHost PRIVATE "-framework AppKit")
        add_dependencies(tst_WindowPresentation tst_FullscreenHost)
        target_sources(tst_WindowPresentation PRIVATE src/backend/platform/macos/MacWindowManager.mm)
        set_source_files_properties(tests/qt/tst_WindowPresentation.cpp PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(tst_WindowPresentation PRIVATE "-framework AppKit" "-framework CoreGraphics")
    elseif(WIN32)
        target_sources(tst_WindowPresentation PRIVATE src/backend/platform/windows/WindowsWindowManager.cpp)
        target_link_libraries(tst_WindowPresentation PRIVATE ole32 uuid user32)
    endif()
    add_test(NAME WindowPresentation COMMAND tst_WindowPresentation)

    add_executable(tst_SystemMonitor
        tests/qt/tst_SystemMonitor.cpp
        src/backend/managers/system/SystemMonitor.cpp
        ${MOUFFETTE_VOLUME_BACKEND}
        src/backend/platform/LocalScreenTopology.cpp
        src/backend/domain/models/ClientInfo.cpp
        src/backend/config/AppConfig.cpp
        src/backend/runtime/RuntimeProfile.cpp)
    target_include_directories(tst_SystemMonitor PRIVATE src)
    target_link_libraries(tst_SystemMonitor PRIVATE Qt6::GuiPrivate Qt6::Network Qt6::Test)
    if(APPLE)
        target_sources(tst_SystemMonitor PRIVATE src/backend/platform/macos/MacWindowManager.mm)
        target_link_libraries(tst_SystemMonitor PRIVATE "-framework CoreAudio" "-framework AppKit" "-framework CoreGraphics")
    elseif(WIN32)
        target_link_libraries(tst_SystemMonitor PRIVATE ole32 uuid)
    endif()
    add_test(NAME SystemMonitor COMMAND tst_SystemMonitor)
    set_tests_properties(SystemMonitor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_MediaBackendBootstrap tests/qt/tst_MediaBackendBootstrap.cpp)
    target_link_libraries(tst_MediaBackendBootstrap PRIVATE MouffetteResidentMedia Qt6::Test)
    add_test(NAME MediaBackendBootstrap COMMAND tst_MediaBackendBootstrap)

    add_executable(tst_ResidentMedia tests/qt/tst_ResidentMedia.cpp)
    target_link_libraries(tst_ResidentMedia PRIVATE MouffetteResidentMedia Qt6::Test)
    target_compile_definitions(tst_ResidentMedia PRIVATE TEST_SAMPLE_VIDEO_FILE="${CMAKE_SOURCE_DIR}/../video-1080p.mp4")
    add_test(NAME ResidentMedia COMMAND tst_ResidentMedia)

    add_executable(tst_MediaEditingCache tests/qt/tst_MediaEditingCache.cpp)
    target_link_libraries(tst_MediaEditingCache PRIVATE MouffetteResidentMedia Qt6::Test)
    target_compile_definitions(tst_MediaEditingCache PRIVATE TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4")
    add_test(NAME MediaEditingCache COMMAND tst_MediaEditingCache)

    add_executable(tst_MediaResidencyManager tests/qt/tst_MediaResidencyManager.cpp)
    target_link_libraries(tst_MediaResidencyManager PRIVATE MouffetteResidentMedia Qt6::Test)
    target_compile_definitions(tst_MediaResidencyManager PRIVATE TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4")
    add_test(NAME MediaResidencyManager COMMAND tst_MediaResidencyManager)
    add_test(NAME MediaResidencyUnavailableBackend COMMAND tst_MediaResidencyManager unavailablePlayerFailsBeforeResidency)
    set_tests_properties(ResidentMedia MediaResidencyManager PROPERTIES
        ENVIRONMENT "QT_MEDIA_BACKEND=ffmpeg;QT_QPA_PLATFORM=offscreen")
    set_tests_properties(MediaResidencyUnavailableBackend PROPERTIES
        ENVIRONMENT "QT_MEDIA_BACKEND=unavailable;QT_QPA_PLATFORM=offscreen")

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
        Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets Qt6::Test OpenSSL::Crypto)
    if(APPLE)
        target_link_libraries(tst_ConnectionManager PRIVATE "-framework Security")
    elseif(WIN32)
        target_link_libraries(tst_ConnectionManager PRIVATE Advapi32)
    endif()
    add_test(NAME ConnectionManager COMMAND tst_ConnectionManager)

    add_executable(tst_RemoteSessionIntegration
        tests/qt/tst_RemoteSessionIntegration.cpp
        tests/fixtures/MultiInstanceProtocolWorker.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_RemoteSessionIntegration PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_RemoteSessionIntegration PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        MOUFFETTE_RELAY_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/remote-session-relay.js")
    target_link_libraries(tst_RemoteSessionIntegration PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME RemoteSessionIntegration COMMAND tst_RemoteSessionIntegration)
    set_tests_properties(RemoteSessionIntegration PROPERTIES TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_CONTROLS_STYLE=Basic")

    add_executable(tst_ScreenSharingService
        tests/qt/tst_ScreenSharingService.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>")
    target_include_directories(tst_ScreenSharingService PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_ScreenSharingService PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        MOUFFETTE_RELAY_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/remote-session-relay.js")
    target_link_libraries(tst_ScreenSharingService PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME ScreenSharingService COMMAND tst_ScreenSharingService)
    set_tests_properties(ScreenSharingService PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_CONTROLS_STYLE=Basic")

    add_executable(tst_ScreenCoordinateMapping
        tests/qt/tst_ScreenCoordinateMapping.cpp
        src/backend/managers/system/ScreenCoordinateMapping.h
    )
    target_include_directories(tst_ScreenCoordinateMapping PRIVATE src)
    target_link_libraries(tst_ScreenCoordinateMapping PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ScreenCoordinateMapping COMMAND tst_ScreenCoordinateMapping)

    add_executable(tst_RetryPolicy tests/qt/tst_RetryPolicy.cpp src/backend/config/AppConfig.cpp src/backend/runtime/RuntimeProfile.cpp)
    target_include_directories(tst_RetryPolicy PRIVATE src)
    target_link_libraries(tst_RetryPolicy PRIVATE Qt6::Network Qt6::Test)
    add_test(NAME RetryPolicy COMMAND tst_RetryPolicy)

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
        src/backend/config/AppConfig.cpp
        src/backend/config/AppConfig.h
    )
    target_include_directories(tst_ApplicationInstanceManager PRIVATE src)
    target_link_libraries(tst_ApplicationInstanceManager PRIVATE
        Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME ApplicationInstanceManager COMMAND tst_ApplicationInstanceManager)

    add_executable(tst_RuntimeStorageBootstrap
        tests/qt/tst_RuntimeStorageBootstrap.cpp
        src/backend/notifications/NotificationCenter.cpp
        src/backend/notifications/NotificationCenter.h
        ${MOUFFETTE_STORAGE_SOURCES}
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
        src/backend/notifications/HistoryStore.cpp
        src/backend/notifications/HistoryStore.h
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

    add_executable(tst_ClientConnectionFlow
        tests/qt/tst_ClientConnectionFlow.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_ClientConnectionFlow PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_ClientConnectionFlow PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>")
    target_link_libraries(tst_ClientConnectionFlow PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME ClientConnectionFlow COMMAND tst_ClientConnectionFlow)

    add_executable(tst_ClientProfile
        tests/qt/tst_ClientProfile.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>")
    target_include_directories(tst_ClientProfile PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_ClientProfile PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>")
    target_link_libraries(tst_ClientProfile PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME ClientProfile COMMAND tst_ClientProfile)

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

    add_executable(tst_MediaFrameItem
        tests/qt/tst_MediaFrameItem.cpp
        src/frontend/rendering/canvas/TimelineThumbnailItem.cpp
        src/frontend/rendering/canvas/TimelineThumbnailItem.h
        src/frontend/rendering/remote/RemoteVideoFrameItem.cpp
        src/frontend/rendering/remote/RemoteVideoFrameItem.h
        src/shared/rendering/MediaFrameSource.cpp
        src/shared/rendering/MediaFrameSource.h
    )
    target_include_directories(tst_MediaFrameItem PRIVATE src)
    target_link_libraries(tst_MediaFrameItem PRIVATE MouffetteResidentMedia Qt6::Quick Qt6::GuiPrivate Qt6::Test)
    target_compile_definitions(tst_MediaFrameItem PRIVATE TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4")
    add_test(NAME MediaFrameItem COMMAND tst_MediaFrameItem)

    add_executable(tst_TextOutlineItem
        tests/qt/tst_TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextOutlineItem.h
        src/frontend/rendering/canvas/TextEditHelper.cpp
        src/frontend/rendering/canvas/TextEditHelper.h
    )
    target_include_directories(tst_TextOutlineItem PRIVATE src)
    target_compile_definitions(tst_TextOutlineItem PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_TextOutlineItem PRIVATE Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Concurrent Qt6::Test)
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
    target_link_libraries(tst_TextOutlineMotion PRIVATE Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Concurrent Qt6::Test)
    add_test(NAME TextOutlineMotion COMMAND tst_TextOutlineMotion)

    add_executable(tst_TextItemQml
        tests/qt/tst_TextItemQml.cpp
        src/backend/domain/media/TextRenderState.cpp
        src/frontend/rendering/canvas/TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextOutlineItem.h
        src/frontend/rendering/canvas/TextEditHelper.cpp
        src/frontend/rendering/canvas/TextEditHelper.h
    )
    target_include_directories(tst_TextItemQml PRIVATE src)
    target_compile_definitions(tst_TextItemQml PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_TextItemQml PRIVATE Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Concurrent Qt6::Test)
    add_test(NAME TextItemQml COMMAND tst_TextItemQml)
    add_test(NAME TextItemQmlScaled COMMAND tst_TextItemQml)
    set_tests_properties(TextItemQmlScaled PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

    add_executable(tst_CanvasInteraction
        tests/qt/tst_CanvasInteraction.cpp
        src/frontend/rendering/canvas/CanvasQmlTypes.cpp
        src/frontend/rendering/canvas/TimelineThumbnailItem.cpp
        src/frontend/rendering/canvas/MediaListModel.cpp
        src/frontend/rendering/canvas/TextOutlineItem.cpp
        src/frontend/rendering/canvas/TextEditHelper.cpp
        src/frontend/rendering/remote/RemoteVideoFrameItem.cpp
    src/shared/rendering/MediaFrameSource.cpp
        src/backend/config/AppConfig.cpp
        src/backend/runtime/RuntimeProfile.cpp
        resources/resources.qrc
    )
    target_include_directories(tst_CanvasInteraction PRIVATE src)
    target_compile_definitions(tst_CanvasInteraction PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_CanvasInteraction PRIVATE MouffetteCanvasShaders MouffetteResidentMedia Qt6::QuickPrivate Qt6::GuiPrivate Qt6::Qml Qt6::Multimedia Qt6::Test)
    add_test(NAME CanvasInteraction COMMAND tst_CanvasInteraction)
    add_test(NAME CanvasInteractionScaled COMMAND tst_CanvasInteraction)
    set_tests_properties(CanvasInteractionScaled PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

    add_executable(tst_TransparencyCheckerboard tests/qt/tst_TransparencyCheckerboard.cpp)
    target_compile_definitions(tst_TransparencyCheckerboard PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tst_TransparencyCheckerboard PRIVATE
        MouffetteCanvasShaders Qt6::Quick Qt6::GuiPrivate Qt6::Qml Qt6::Test)
    add_test(NAME TransparencyCheckerboard COMMAND tst_TransparencyCheckerboard)

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
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4"
        TEST_WEBP_FILE="${CMAKE_SOURCE_DIR}/../image-1920.webp")
    target_link_libraries(tst_CanvasSelectionBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME CanvasSelectionBackend COMMAND tst_CanvasSelectionBackend)
    if(APPLE)
        target_sources(tst_CanvasSelectionBackend PRIVATE tests/qt/FinderDropTestSupport.mm)
    endif()

    add_executable(tst_SceneTimeline tests/qt/tst_SceneTimeline.cpp
        src/backend/domain/scene/SceneTimeline.cpp)
    target_include_directories(tst_SceneTimeline PRIVATE src)
    target_link_libraries(tst_SceneTimeline PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME SceneTimeline COMMAND tst_SceneTimeline)

    add_executable(tst_TimelineController
        tests/qt/tst_TimelineController.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>")
    target_include_directories(tst_TimelineController PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_TimelineController PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4")
    target_link_libraries(tst_TimelineController PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME TimelineController COMMAND tst_TimelineController)
    add_test(NAME TimelineControllerScaled COMMAND tst_TimelineController)
    set_tests_properties(TimelineControllerScaled PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

    add_executable(tst_VideoPlaybackBackend
        tests/qt/tst_VideoPlaybackBackend.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_VideoPlaybackBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_VideoPlaybackBackend PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4")
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
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4")
    target_link_libraries(tst_RemoteSceneControllerLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME RemoteSceneControllerLifecycle COMMAND tst_RemoteSceneControllerLifecycle)
    foreach(_video_regression_target tst_VideoPlaybackBackend tst_RemoteSceneControllerLifecycle)
        target_compile_definitions(${_video_regression_target} PRIVATE
            TEST_REPORTED_VIDEO_FILE="${CMAKE_SOURCE_DIR}/../scream.mp4")
    endforeach()
    add_test(NAME RemoteSceneDisplayTopology COMMAND tst_RemoteSceneControllerLifecycle
        screenHotplugPreservesRunAndMatchesIdentity
        absentDisplayCannotCompletePreparationOrActivation
        displayLostBeforeFirstFrameDoesNotSatisfyBarrier
        allOutputsAbsentKeepVideoAndAudioTimeline)
    set_tests_properties(RemoteSceneDisplayTopology PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic"
        RUN_SERIAL TRUE)

    add_executable(tst_RemoteSceneLifecycle
        tests/qt/tst_RemoteSceneLifecycle.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_RemoteSceneLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_RemoteSceneLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>"
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4")
    target_link_libraries(tst_RemoteSceneLifecycle PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME RemoteSceneLifecycle COMMAND tst_RemoteSceneLifecycle)

    add_executable(tst_MediaOverlay
        tests/qt/tst_MediaOverlay.cpp
        "$<FILTER:$<TARGET_OBJECTS:MouffetteClient>,EXCLUDE,/src/main[.]cpp[.](o|obj)$>"
    )
    target_include_directories(tst_MediaOverlay PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,INCLUDE_DIRECTORIES>")
    target_compile_definitions(tst_MediaOverlay PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,COMPILE_DEFINITIONS>")
    target_link_libraries(tst_MediaOverlay PRIVATE
        "$<TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES>" Qt6::Test)
    add_test(NAME MediaOverlay COMMAND tst_MediaOverlay)
    add_test(NAME MediaOverlayScaled COMMAND tst_MediaOverlay)
    set_tests_properties(MediaOverlayScaled PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

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
    set_tests_properties(WindowPresentation TextOutlineMotion CanvasInteraction CanvasInteractionScaled CanvasSelectionBackend TimelineController TimelineControllerScaled ClientConnectionFlow VideoPlaybackBackend UploadRemovalSecurity RemoteSceneControllerLifecycle RemoteSceneLifecycle MediaOverlay MediaOverlayScaled
        PROPERTIES RUN_SERIAL TRUE)
    # Match the production style explicitly. Loading the native Controls
    # plugin concurrently with asynchronous QML compilation is unstable on
    # some Windows Qt builds and does not represent the shipped application.
    set_tests_properties(TextOutlineMotion CanvasInteraction CanvasSelectionBackend ClientConnectionFlow VideoPlaybackBackend UploadRemovalSecurity RemoteSceneControllerLifecycle RemoteSceneLifecycle MediaOverlay
        PROPERTIES ENVIRONMENT "QT_QUICK_CONTROLS_STYLE=Basic")
    set_tests_properties(CanvasInteractionScaled MediaOverlayScaled
        PROPERTIES ENVIRONMENT "QT_QUICK_CONTROLS_STYLE=Basic;QT_SCALE_FACTOR=2")

    add_executable(tst_MediaFilePolicy
        tests/qt/tst_MediaFilePolicy.cpp
        src/backend/domain/media/MediaFilePolicy.cpp
        src/backend/domain/media/MediaFilePolicy.h
        src/backend/config/AppConfig.cpp
        src/backend/config/AppConfig.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
    )
    target_include_directories(tst_MediaFilePolicy PRIVATE src)
    target_compile_definitions(tst_MediaFilePolicy PRIVATE
        TEST_VIDEO_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/resident-timeline.mp4"
        TEST_WEBP_FILE="${CMAKE_SOURCE_DIR}/../image-1920.webp")
    target_link_libraries(tst_MediaFilePolicy PRIVATE PkgConfig::FFMPEG
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME MediaFilePolicy COMMAND tst_MediaFilePolicy)
    set(MOUFFETTE_MEDIA_POLICY_TEST_ENV
        "QT_MEDIA_BACKEND=ffmpeg"
        "QT_QUICK_CONTROLS_STYLE=Basic")
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
        src/backend/domain/scene/SceneTimeline.cpp
        src/backend/domain/scene/SceneTimeline.h
        src/backend/domain/models/ClientInfo.cpp
        src/backend/domain/models/ClientInfo.h
        src/backend/domain/project/ProjectModel.cpp
        src/backend/domain/project/ProjectModel.h
        src/backend/domain/project/ProjectStore.cpp
        src/backend/domain/project/ProjectStore.h
        src/backend/domain/project/ProjectManager.cpp
        src/backend/domain/project/ProjectManager.h
        src/backend/domain/workspace/WorkspaceManager.cpp
        src/backend/domain/workspace/WorkspaceManager.h
        src/backend/domain/session/IncomingSessionOrphanWatchdog.cpp
        src/backend/domain/session/IncomingSessionOrphanWatchdog.h
        src/backend/config/AppConfig.cpp
        src/backend/config/AppConfig.h
        src/backend/runtime/RuntimeProfile.cpp
        src/backend/runtime/RuntimeProfile.h
    )
    target_include_directories(tst_ProjectManager PRIVATE src)
    target_link_libraries(tst_ProjectManager PRIVATE Qt6::Gui Qt6::Test)
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

    add_executable(tst_RemoteCacheHistory tests/qt/tst_RemoteCacheHistory.cpp)
    target_include_directories(tst_RemoteCacheHistory PRIVATE src)
    target_link_libraries(tst_RemoteCacheHistory PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME RemoteCacheHistory COMMAND tst_RemoteCacheHistory)

    add_test(NAME QmlArchitectureGate
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/cmake/verify_qml_architecture.cmake)
endif()

if(APPLE)
    include(cmake/QtDarwinMedia.cmake)
endif()
if(APPLE OR WIN32)
    include(cmake/QtFFmpegMedia.cmake)
    if(BUILD_TESTING)
        # Exercise Darwin explicitly even when the selected Qt also has FFmpeg.
        if(APPLE)
            add_test(NAME VideoPlaybackDarwinSeek COMMAND tst_VideoPlaybackBackend
                seekBeforeFirstPlay seekAfterPauseAndSourceReload)
            set_tests_properties(VideoPlaybackDarwinSeek PROPERTIES
                RUN_SERIAL TRUE
                ENVIRONMENT "QT_MEDIA_BACKEND=darwin;QT_QPA_PLATFORM=cocoa;QT_QUICK_CONTROLS_STYLE=Basic")
        endif()
        get_property(_tests DIRECTORY PROPERTY TESTS)
        set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "QT_PLUGIN_PATH=path_list_prepend:${CMAKE_CURRENT_BINARY_DIR}/plugins")
        get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            if(_target MATCHES "^tst_")
                add_dependencies(${_target} MouffetteFFmpegMedia)
                if(APPLE)
                    add_dependencies(${_target} MouffetteDarwinMedia)
                endif()
            endif()
        endforeach()
    endif()
endif()

# Shared worker logger. Static dependencies are pulled only when a test does
# not already provide its own config/profile object files.
add_library(MouffetteNetworkDiagnostics STATIC
    src/backend/network/NetworkDiagnostics.cpp
    src/backend/config/AppConfig.cpp
    src/backend/runtime/RuntimeProfile.cpp)
target_include_directories(MouffetteNetworkDiagnostics PUBLIC src)
target_link_libraries(MouffetteNetworkDiagnostics PUBLIC Qt6::Core Qt6::Network)
if(BUILD_TESTING)
    add_executable(tst_NetworkDiagnostics tests/qt/tst_NetworkDiagnostics.cpp)
    target_link_libraries(tst_NetworkDiagnostics PRIVATE Qt6::Test)
    add_test(NAME NetworkDiagnostics COMMAND tst_NetworkDiagnostics)
endif()
get_property(mouffette_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(mouffette_target IN LISTS mouffette_targets)
    get_target_property(mouffette_target_type ${mouffette_target} TYPE)
    if(mouffette_target_type STREQUAL "EXECUTABLE")
        get_target_property(mouffette_target_links ${mouffette_target} LINK_LIBRARIES)
        # Tests which consume the application object files already inherit its
        # link libraries through this generator expression.
        if(NOT "${mouffette_target_links}" MATCHES "TARGET_PROPERTY:MouffetteClient,LINK_LIBRARIES")
            target_link_libraries(${mouffette_target} PRIVATE MouffetteNetworkDiagnostics)
        endif()
    endif()
endforeach()

# Manual benchmarks are excluded from CTest and normal builds.
add_executable(media_engine_benchmark EXCLUDE_FROM_ALL tests/benchmarks/media_engine_benchmark.cpp)
target_link_libraries(media_engine_benchmark PRIVATE MouffetteResidentMedia)
add_executable(media_interaction_benchmark EXCLUDE_FROM_ALL tests/benchmarks/media_interaction_benchmark.cpp)
target_link_libraries(media_interaction_benchmark PRIVATE MouffetteResidentMedia)
add_executable(media_seek_benchmark EXCLUDE_FROM_ALL tests/benchmarks/media_seek_benchmark.cpp)
target_link_libraries(media_seek_benchmark PRIVATE MouffetteResidentMedia)
