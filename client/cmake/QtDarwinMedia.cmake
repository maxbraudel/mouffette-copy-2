# Qt's Darwin backend builds seek times with currentTime.timescale, which is
# initially 1. Fractional seconds are truncated until the first playback.
# Rebuild only that plugin against the selected Qt, with a millisecond timebase.
# Keep the patch version/hash explicit: this plugin uses Qt private interfaces.
if(NOT Qt6Multimedia_VERSION VERSION_EQUAL "6.11.2")
    message(FATAL_ERROR "Review the Darwin seek patch for Qt ${Qt6Multimedia_VERSION}; currently verified with Qt 6.11.2.")
endif()
find_package(Qt6 REQUIRED COMPONENTS MultimediaPrivate CorePrivate)
include(FetchContent)
FetchContent_Declare(mouffette_qtmultimedia
    URL https://github.com/qt/qtmultimedia/archive/refs/tags/v6.11.2.tar.gz
    URL_HASH SHA256=86dab3128cbe1b00cf2343a303d0df675c442ab0df2bf44afa35508673146cc0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR mouffette-unused)
FetchContent_MakeAvailable(mouffette_qtmultimedia)
set(_darwin "${mouffette_qtmultimedia_SOURCE_DIR}/src/plugins/multimedia/darwin")
set(_player "${_darwin}/mediaplayer/avfmediaplayer.mm")
file(READ "${_player}" _source)
set(_old "CMTime newTime = [playerItem currentTime];\n    newTime.value = (pos / 1000.0f) * newTime.timescale;")
set(_new "CMTime newTime = CMTimeMake(pos, 1000);")
string(FIND "${_source}" "${_old}" _offset)
if(_offset EQUAL -1)
    string(FIND "${_source}" "${_new}" _offset)
    if(_offset EQUAL -1)
        message(FATAL_ERROR "Qt Darwin seek source changed; review the millisecond precision patch.")
    endif()
else()
    string(REPLACE "${_old}" "${_new}" _source "${_source}")
    file(WRITE "${_player}" "${_source}")
endif()

# QIODevice playback also needs valid UTI metadata and completion of metadata-
# only AVAsset requests. Upstream 6.11.2 leaves those requests pending forever.
# Bound each response to the requested range instead of over-reading its tail.
function(mouffette_patch_darwin old_text new_text)
    string(FIND "${_source}" "${new_text}" _found)
    if(NOT _found EQUAL -1)
        return()
    endif()
    string(FIND "${_source}" "${old_text}" _found)
    if(NOT _found EQUAL -1)
        string(REPLACE "${old_text}" "${new_text}" _patched "${_source}")
        set(_source "${_patched}" PARENT_SCOPE)
    else()
        string(FIND "${_source}" "${new_text}" _found)
        if(_found EQUAL -1)
            message(FATAL_ERROR "Qt Darwin memory stream source changed; review the resident stream patch.")
        endif()
    endif()
endfunction()
mouffette_patch_darwin("#include <QtCore/qmimedatabase.h>"
    "#include <QtCore/qmimedatabase.h>\n#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>")
mouffette_patch_darwin("device->seek(loadingRequest.dataRequest.requestedOffset);"
    "if (loadingRequest.dataRequest) device->seek(loadingRequest.dataRequest.currentOffset);")
mouffette_patch_darwin("loadingRequest.contentInformationRequest.contentType = m_mimeType;"
    "loadingRequest.contentInformationRequest.contentType = [UTType typeWithFilenameExtension:m_mimeType].identifier;")
mouffette_patch_darwin("NSInteger requestedLength = loadingRequest.dataRequest.requestedLength;"
    "NSInteger requestedLength = loadingRequest.dataRequest.requestsAllDataToEndOfResource ? device->size() - device->pos() : loadingRequest.dataRequest.requestedOffset + loadingRequest.dataRequest.requestedLength - device->pos();")
mouffette_patch_darwin("qint64 len = device->read(buffer.data(), maxBytes);"
    "qint64 len = device->read(buffer.data(), qMin<qint64>(maxBytes, requestedLength - submitted));")
mouffette_patch_darwin([=[            // Finish loading even if not all bytes submitted.
            [loadingRequest finishLoading];
        }]=] [=[        }
        // Complete metadata-only requests as well as data requests.
        [loadingRequest finishLoading];]=])
file(WRITE "${_player}" "${_source}")

# All sources in this pinned directory belong to the macOS plugin.
file(GLOB_RECURSE _darwin_sources "${_darwin}/*.mm" "${_darwin}/*_p.h")
add_library(MouffetteDarwinMedia MODULE ${_darwin_sources})
set_target_properties(MouffetteDarwinMedia PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME darwinmediaplugin
    SUFFIX .dylib
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/multimedia")
target_include_directories(MouffetteDarwinMedia PRIVATE
    "${_darwin}" "${_darwin}/audio" "${_darwin}/camera"
    "${_darwin}/common" "${_darwin}/mediaplayer")
target_compile_definitions(MouffetteDarwinMedia PRIVATE
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
target_compile_options(MouffetteDarwinMedia PRIVATE -Wno-c++20-extensions)
target_link_libraries(MouffetteDarwinMedia PRIVATE
    Qt6::MultimediaPrivate Qt6::CorePrivate Qt6::Concurrent
    "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreMedia"
    "-framework CoreVideo" "-framework Foundation" "-framework Metal"
    "-framework QuartzCore" "-framework AppKit" "-framework AudioUnit"
    "-framework VideoToolbox" "-framework ApplicationServices"
    "-framework UniformTypeIdentifiers")
add_dependencies(MouffetteClient MouffetteDarwinMedia)
set_property(TARGET MouffetteClient APPEND PROPERTY LINK_DEPENDS
    "$<TARGET_FILE:MouffetteDarwinMedia>")
add_custom_command(TARGET MouffetteClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_BUNDLE_DIR:MouffetteClient>/Contents/PlugIns/multimedia"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:MouffetteDarwinMedia>"
        "$<TARGET_BUNDLE_DIR:MouffetteClient>/Contents/PlugIns/multimedia/")
