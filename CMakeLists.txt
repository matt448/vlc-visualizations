cmake_minimum_required(VERSION 3.20)

project(vlc_trackinfo_visualizer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(VLC_TARGET_BITS "64" CACHE STRING "Target VLC architecture: 32 or 64")
set_property(CACHE VLC_TARGET_BITS PROPERTY STRINGS 32 64)

if(WIN32)
    if(VLC_TARGET_BITS STREQUAL "32" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR
            "VLC_TARGET_BITS=32 requires a 32-bit compiler. "
            "Use the MSYS2 MINGW32 shell, or configure with -DVLC_TARGET_BITS=64 for 64-bit VLC."
        )
    elseif(VLC_TARGET_BITS STREQUAL "64" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "VLC_TARGET_BITS=64 requires a 64-bit compiler."
        )
    endif()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VLC_PLUGIN QUIET vlc-plugin)
endif()

set(VLC_INCLUDE_DIR "" CACHE PATH "Path to VLC development headers, containing vlc_plugin.h")
set(VLC_LIB_DIR "" CACHE PATH "Path to VLC import libraries")
set(VLC_PLUGIN_LIBRARIES "" CACHE STRING "VLC libraries for plugin linking, for example vlccore;vlc")

if(VLC_PLUGIN_FOUND)
    set(TRACKINFO_VLC_INCLUDE_DIRS ${VLC_PLUGIN_INCLUDE_DIRS})
    set(TRACKINFO_VLC_LIBRARY_DIRS ${VLC_PLUGIN_LIBRARY_DIRS})
    set(TRACKINFO_VLC_LIBRARIES ${VLC_PLUGIN_LIBRARIES})
    set(TRACKINFO_VLC_CFLAGS_OTHER ${VLC_PLUGIN_CFLAGS_OTHER})
elseif(VLC_INCLUDE_DIR AND VLC_LIB_DIR AND VLC_PLUGIN_LIBRARIES)
    set(TRACKINFO_VLC_INCLUDE_DIRS ${VLC_INCLUDE_DIR})
    set(TRACKINFO_VLC_LIBRARY_DIRS ${VLC_LIB_DIR})
    set(TRACKINFO_VLC_LIBRARIES ${VLC_PLUGIN_LIBRARIES})
    set(TRACKINFO_VLC_CFLAGS_OTHER "")
else()
    message(FATAL_ERROR
        "VLC development files were not found. The normal VLC installer only includes the runtime. "
        "Install/build the matching VLC SDK, then either make pkg-config find vlc-plugin.pc or pass "
        "-DVLC_INCLUDE_DIR=... -DVLC_LIB_DIR=... -DVLC_PLUGIN_LIBRARIES=..."
    )
endif()

include(CheckIncludeFile)
set(CMAKE_REQUIRED_INCLUDES ${TRACKINFO_VLC_INCLUDE_DIRS})
check_include_file("vlc_playlist_legacy.h" HAVE_VLC_PLAYLIST_LEGACY_H)

function(add_vlc_visualizer target output_name module_string visualizer_source)
    add_library(${target} MODULE
        ${visualizer_source}
        src/visualizer_common.c
    )

    target_include_directories(${target} PRIVATE
        ${TRACKINFO_VLC_INCLUDE_DIRS}
    )

    target_compile_options(${target} PRIVATE
        ${TRACKINFO_VLC_CFLAGS_OTHER}
    )

    target_link_directories(${target} PRIVATE
        ${TRACKINFO_VLC_LIBRARY_DIRS}
    )

    target_link_libraries(${target} PRIVATE
        ${TRACKINFO_VLC_LIBRARIES}
        ws2_32
        user32
        gdi32
        msimg32
    )

    if(HAVE_VLC_PLAYLIST_LEGACY_H)
        target_compile_definitions(${target} PRIVATE HAVE_VLC_PLAYLIST_LEGACY_H=1)
    endif()

    target_compile_definitions(${target} PRIVATE
        MODULE_STRING=\"${module_string}\"
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )

    set_target_properties(${target} PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "${output_name}"
    )
endfunction()

add_vlc_visualizer(trackinfo_visualizer trackinfo_visualizer_plugin trackinfo_visualizer src/spectrum_info.c)
add_vlc_visualizer(led_segment_visualizer led_segment_visualizer_plugin led_segment_visualizer src/led_segments.c)
add_vlc_visualizer(led_peak_visualizer led_peak_visualizer_plugin led_peak_visualizer src/led_segments.c)
target_compile_definitions(led_peak_visualizer PRIVATE LED_PEAK_INDICATORS=1)
add_vlc_visualizer(breakout_visualizer breakout_visualizer_plugin breakout_visualizer src/breakout_visualizer.c)
add_vlc_visualizer(tempest_visualizer tempest_visualizer_plugin tempest_visualizer src/tempest_visualizer.c)
