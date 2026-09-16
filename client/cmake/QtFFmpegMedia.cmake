# Version-pinned streaming backend for macOS and Windows. The same decoder
# queues and accurate-seek patches must ship on both endpoints.
if(NOT Qt6Multimedia_VERSION VERSION_EQUAL "6.11.2")
    message(FATAL_ERROR "Review the streaming backend patches for Qt ${Qt6Multimedia_VERSION}; verified with 6.11.2.")
endif()
find_package(Qt6 REQUIRED COMPONENTS MultimediaPrivate CorePrivate)
if(NOT mouffette_qtmultimedia_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(mouffette_qtmultimedia
        URL https://github.com/qt/qtmultimedia/archive/refs/tags/v6.11.2.tar.gz
        URL_HASH SHA256=86dab3128cbe1b00cf2343a303d0df675c442ab0df2bf44afa35508673146cc0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR mouffette-unused)
    FetchContent_MakeAvailable(mouffette_qtmultimedia)
endif()
set(_ffmpeg "${mouffette_qtmultimedia_SOURCE_DIR}/src/plugins/multimedia/ffmpeg")
include(${CMAKE_CURRENT_LIST_DIR}/QtFFmpegPlaybackFixes.cmake)
file(GLOB _ffmpeg_sources
    "${_ffmpeg}/qffmpeg*.cpp" "${_ffmpeg}/qffmpeg*_p.h"
    "${_ffmpeg}/playbackengine/*.cpp" "${_ffmpeg}/playbackengine/*_p.h"
    "${_ffmpeg}/recordingengine/*.cpp" "${_ffmpeg}/recordingengine/*_p.h")
list(FILTER _ffmpeg_sources EXCLUDE REGEX "_vaapi[._]")
if(APPLE)
    list(FILTER _ffmpeg_sources EXCLUDE REGEX "(_d3d11|_dxgi|_uwp)[._]")
    file(GLOB _platform_sources "${_ffmpeg}/darwin/*.cpp" "${_ffmpeg}/darwin/*.mm" "${_ffmpeg}/darwin/*_p.h")
elseif(WIN32)
    file(GLOB _platform_sources "${_ffmpeg}/qwindowscamera*" "${_ffmpeg}/qgdiwindowcapture*" "${_ffmpeg}/qwincapturablewindows*")
    if(NOT QT_FEATURE_cpp_winrt)
        list(FILTER _ffmpeg_sources EXCLUDE REGEX "_uwp[._]")
    endif()
endif()
list(APPEND _ffmpeg_sources ${_platform_sources})
list(APPEND _ffmpeg_sources "${_ffmpeg}/qgrabwindowsurfacecapture.cpp"
    "${_ffmpeg}/qgrabwindowsurfacecapture_p.h")

if(APPLE)
# Qt's optional legacy window-capture implementation uses an API removed in
# macOS 15. Mouffette does not expose QWindowCapture; keep this unrelated capture
# capability unavailable rather than compiling against an obsolete SDK contract.
list(FILTER _ffmpeg_sources EXCLUDE REGEX "/qcgwindowcapture[._]")
set(_window_capture_stub "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-window-capture.cpp")
file(GENERATE OUTPUT "${_window_capture_stub}" CONTENT
    "#include <qffmpegdarwinintegrationfactory_p.h>\nQT_BEGIN_NAMESPACE\nnamespace QFFmpeg { std::unique_ptr<QPlatformSurfaceCapture> makeQCgWindowCapture() { return {}; } }\nQT_END_NAMESPACE\n")
list(APPEND _ffmpeg_sources "${_window_capture_stub}")

endif()

# Reproduce Qt's private forwarding includes without modifying upstream code.
set(_ffmpeg_headers "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-headers")
file(GLOB_RECURSE _headers "${_ffmpeg}/*_p.h")
foreach(_header IN LISTS _headers)
    get_filename_component(_name "${_header}" NAME)
    file(GENERATE OUTPUT "${_ffmpeg_headers}/QtFFmpegMediaPluginImpl/private/${_name}"
        CONTENT "#include \"${_header}\"\n")
endforeach()
add_library(MouffetteFFmpegMedia MODULE ${_ffmpeg_sources})
set_target_properties(MouffetteFFmpegMedia PROPERTIES
    AUTOMOC ON OUTPUT_NAME ffmpegmediaplugin
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/multimedia"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/multimedia")
if(APPLE)
    set_target_properties(MouffetteFFmpegMedia PROPERTIES SUFFIX .dylib)
elseif(WIN32)
    set_target_properties(MouffetteFFmpegMedia PROPERTIES PREFIX "")
endif()
target_include_directories(MouffetteFFmpegMedia PRIVATE
    "${_ffmpeg}" "${_ffmpeg}/darwin" "${_ffmpeg_headers}"
    "${mouffette_qtmultimedia_SOURCE_DIR}/src/3rdparty/signalsmith-stretch")
target_compile_definitions(MouffetteFFmpegMedia PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
if(NOT MSVC)
    target_compile_options(MouffetteFFmpegMedia PRIVATE -Wno-deprecated-declarations)
endif()
target_link_libraries(MouffetteFFmpegMedia PRIVATE
    Qt6::MultimediaPrivate Qt6::CorePrivate Qt6::GuiPrivate Qt6::Concurrent PkgConfig::FFMPEG)
if(APPLE)
    target_link_libraries(MouffetteFFmpegMedia PRIVATE "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreMedia"
    "-framework CoreVideo" "-framework Foundation" "-framework Metal"
    "-framework QuartzCore" "-framework AppKit" "-framework Security"
    "-framework VideoToolbox" "-framework ApplicationServices")
elseif(WIN32)
    target_link_libraries(MouffetteFFmpegMedia PRIVATE mf mfplat mfreadwrite mfuuid d3d11 dxgi)
    if(QT_FEATURE_cpp_winrt)
        target_compile_features(MouffetteFFmpegMedia PRIVATE cxx_std_20)
        if(MINGW)
            # Qt's WinRT headers can be included before qffmpegwindowcapture_uwp.cpp
            # reaches its own <unknwn.h> include.
            target_compile_options(MouffetteFFmpegMedia PRIVATE -include unknwn.h)
        endif()
        target_link_libraries(MouffetteFFmpegMedia PRIVATE Dwmapi Dxva2 windowsapp)
    endif()
endif()
add_dependencies(MouffetteClient MouffetteFFmpegMedia)
set_property(TARGET MouffetteClient APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:MouffetteFFmpegMedia>")
if(APPLE)
set(_plugin_destination "$<TARGET_BUNDLE_DIR:MouffetteClient>/Contents/PlugIns/multimedia")
else()
set(_plugin_destination "$<TARGET_FILE_DIR:MouffetteClient>/multimedia")
endif()
add_custom_command(TARGET MouffetteClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_plugin_destination}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:MouffetteFFmpegMedia>"
        "${_plugin_destination}/")
