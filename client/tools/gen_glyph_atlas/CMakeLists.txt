cmake_minimum_required(VERSION 3.16)
project(gen_glyph_atlas VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

set(CMAKE_AUTOMOC ON)

add_executable(gen_glyph_atlas
    main.cpp
)

target_link_libraries(gen_glyph_atlas
    Qt6::Core
    Qt6::Gui
)

target_include_directories(gen_glyph_atlas PRIVATE .)

# On macOS, link CoreText (needed by Qt Gui for QRawFont)
if(APPLE)
    target_link_libraries(gen_glyph_atlas "-framework CoreText" "-framework CoreGraphics")
endif()
