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
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network WebSockets Multimedia Svg SvgWidgets)

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
)

# Platform-specific sources
if(APPLE)
    list(APPEND SOURCES src/MacDockHider.mm src/MacCursorHider.mm src/MacVideoThumbnailer.mm)
else()
    # Provide a no-op implementation on non-macOS so linking succeeds
    list(APPEND SOURCES src/MacDockHider.cpp)
endif()

# Header files
set(HEADERS
    src/MainWindow.h
    src/WebSocketClient.h
    src/ClientInfo.h
    src/MacDockHider.h
    src/MacVideoThumbnailer.h
)

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
    Qt6::Svg
    Qt6::SvgWidgets
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
    )
endif()

if(APPLE)
    set_target_properties(MouffetteClient PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist
    )
    # Link with macOS frameworks for cursor and AVFoundation thumbnailing
    target_link_libraries(MouffetteClient "-framework AppKit" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreGraphics")
    
    # Set Objective-C++ properties for .mm files
    set_source_files_properties(src/MacDockHider.mm PROPERTIES
        COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/MacCursorHider.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
    set_source_files_properties(src/MacVideoThumbnailer.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()
