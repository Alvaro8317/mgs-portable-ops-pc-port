# SPDX-License-Identifier: GPL-3.0-or-later
#
# FFmpeg (LGPL-2.1-or-later) for the PMF videos (mpo_video, F17a). Only what
# the H.264 path needs: libavcodec with the h264 decoder and parser, libswscale
# (the same YUV -> RGB conversion PPSSPP uses) and libavutil. No GPL parts, no
# nonfree parts, no programs, no network, no autodetected system libraries.
#
# Provides the INTERFACE target mpo::ffmpeg, or nothing when FFmpeg is off.
#
# Options:
#   MPO_FFMPEG             auto | bundled | system | off
#                          auto: bundled where the build can run FFmpeg's
#                          configure (a POSIX shell and make, not MSVC), else
#                          system if found, else off.
#   MPO_FFMPEG_URL         where to get ffmpeg-7.1.5.tar.xz (a local path works
#                          too); the SHA-256 below is always checked
#   MPO_FFMPEG_SOURCE_DIR  an already unpacked FFmpeg 7.1.5 tree (offline)
#   MPO_FFMPEG_JOBS        parallel jobs for the bundled build (default 3)
#
# The bundled build runs at build time (ExternalProject), out of tree, with
# the C compiler of this build and ccache when MPO_USE_CCACHE found it. x86
# assembly needs nasm; without it the build uses --disable-x86asm (C and inline
# assembly only). Output does not depend on it: H.264 decoding is bit-exact and
# mpo_video asks swscale for SWS_BITEXACT | SWS_ACCURATE_RND.

if(TARGET mpo::ffmpeg)
    return()
endif()

set(MPO_FFMPEG "auto" CACHE STRING "FFmpeg for mpo_video: auto, bundled, system or off")
set_property(CACHE MPO_FFMPEG PROPERTY STRINGS auto bundled system off)
set(MPO_FFMPEG_VERSION "7.1.5")
set(MPO_FFMPEG_SHA256 "de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f")
set(MPO_FFMPEG_URL "https://ffmpeg.org/releases/ffmpeg-${MPO_FFMPEG_VERSION}.tar.xz"
    CACHE STRING "URL or local path of ffmpeg-${MPO_FFMPEG_VERSION}.tar.xz")
set(MPO_FFMPEG_SOURCE_DIR "" CACHE PATH "Unpacked FFmpeg ${MPO_FFMPEG_VERSION} tree (skips the download)")
set(MPO_FFMPEG_JOBS "3" CACHE STRING "Parallel jobs for the bundled FFmpeg build")

set(mpo_ffmpeg_mode "${MPO_FFMPEG}")
if(mpo_ffmpeg_mode STREQUAL "auto")
    find_program(MPO_FFMPEG_MAKE NAMES gmake make)
    find_program(MPO_FFMPEG_SH NAMES sh bash)
    if(NOT MSVC AND NOT WIN32 AND MPO_FFMPEG_MAKE AND MPO_FFMPEG_SH)
        set(mpo_ffmpeg_mode "bundled")
    else()
        set(mpo_ffmpeg_mode "system")
        set(mpo_ffmpeg_optional_system ON)
    endif()
endif()

if(mpo_ffmpeg_mode STREQUAL "off")
    message(STATUS "FFmpeg: off (mpo_video builds without the H.264 decoder)")
    return()
endif()

if(mpo_ffmpeg_mode STREQUAL "system")
    set(mpo_ffmpeg_found OFF)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        # avcodec 59 = FFmpeg 5.0, the first release with the const AVCodec API.
        pkg_check_modules(MPO_FFMPEG_PC QUIET IMPORTED_TARGET
                          libavcodec>=59 libavutil libswscale)
        if(MPO_FFMPEG_PC_FOUND)
            add_library(mpo_ffmpeg INTERFACE)
            target_link_libraries(mpo_ffmpeg INTERFACE PkgConfig::MPO_FFMPEG_PC)
            set(mpo_ffmpeg_found ON)
            message(STATUS "FFmpeg: system (pkg-config, libavcodec ${MPO_FFMPEG_PC_libavcodec_VERSION})")
        endif()
    endif()
    if(NOT mpo_ffmpeg_found)
        # vcpkg and hand-made installs without pkg-config.
        find_path(MPO_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
        find_library(MPO_FFMPEG_AVCODEC NAMES avcodec)
        find_library(MPO_FFMPEG_SWSCALE NAMES swscale)
        find_library(MPO_FFMPEG_AVUTIL NAMES avutil)
        if(MPO_FFMPEG_INCLUDE_DIR AND MPO_FFMPEG_AVCODEC AND MPO_FFMPEG_SWSCALE
           AND MPO_FFMPEG_AVUTIL)
            add_library(mpo_ffmpeg INTERFACE)
            target_include_directories(mpo_ffmpeg SYSTEM INTERFACE ${MPO_FFMPEG_INCLUDE_DIR})
            target_link_libraries(mpo_ffmpeg INTERFACE ${MPO_FFMPEG_AVCODEC}
                                  ${MPO_FFMPEG_SWSCALE} ${MPO_FFMPEG_AVUTIL})
            set(mpo_ffmpeg_found ON)
            message(STATUS "FFmpeg: system (${MPO_FFMPEG_AVCODEC})")
        endif()
    endif()
    if(NOT mpo_ffmpeg_found)
        if(mpo_ffmpeg_optional_system)
            message(STATUS "FFmpeg: not found (mpo_video builds without the H.264 decoder)")
            return()
        endif()
        message(FATAL_ERROR "MPO_FFMPEG=system but libavcodec (>= 59), libavutil and "
                            "libswscale were not found")
    endif()
    add_library(mpo::ffmpeg ALIAS mpo_ffmpeg)
    return()
endif()

if(NOT mpo_ffmpeg_mode STREQUAL "bundled")
    message(FATAL_ERROR "MPO_FFMPEG must be auto, bundled, system or off (got ${MPO_FFMPEG})")
endif()

if(MSVC)
    message(FATAL_ERROR "MPO_FFMPEG=bundled needs a POSIX shell and GCC/Clang; with MSVC use "
                        "MPO_FFMPEG=system (for example vcpkg's ffmpeg[avcodec,swscale]) or off")
endif()
find_program(MPO_FFMPEG_MAKE NAMES gmake make REQUIRED)

# FFmpeg is C; the root project only enables C++.
enable_language(C)
include(ExternalProject)
find_package(Threads REQUIRED)

set(mpo_ffmpeg_prefix "${CMAKE_BINARY_DIR}/third_party/ffmpeg")
set(mpo_ffmpeg_install "${mpo_ffmpeg_prefix}/install")
set(mpo_ffmpeg_cc "${CMAKE_C_COMPILER}")
if(MPO_CCACHE_PROGRAM)
    set(mpo_ffmpeg_cc "${MPO_CCACHE_PROGRAM} ${CMAKE_C_COMPILER}")
endif()

set(mpo_ffmpeg_configure_args
    --prefix=${mpo_ffmpeg_install}
    "--cc=${mpo_ffmpeg_cc}"
    --enable-static --disable-shared --enable-pic
    --disable-programs --disable-doc --disable-network
    --disable-autodetect --disable-iconv --enable-pthreads
    --disable-everything --enable-decoder=h264 --enable-parser=h264
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample
    --enable-swscale --disable-debug)
find_program(MPO_FFMPEG_NASM nasm)
if(MPO_FFMPEG_NASM)
    list(APPEND mpo_ffmpeg_configure_args --x86asmexe=${MPO_FFMPEG_NASM})
else()
    list(APPEND mpo_ffmpeg_configure_args --disable-x86asm)
endif()

set(mpo_ffmpeg_libs
    ${mpo_ffmpeg_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}avcodec${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${mpo_ffmpeg_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}swscale${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${mpo_ffmpeg_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}avutil${CMAKE_STATIC_LIBRARY_SUFFIX})

if(MPO_FFMPEG_SOURCE_DIR)
    set(mpo_ffmpeg_source_args SOURCE_DIR "${MPO_FFMPEG_SOURCE_DIR}" DOWNLOAD_COMMAND "")
else()
    set(mpo_ffmpeg_source_args
        URL "${MPO_FFMPEG_URL}"
        URL_HASH SHA256=${MPO_FFMPEG_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()

ExternalProject_Add(mpo_ffmpeg_ep
    ${mpo_ffmpeg_source_args}
    PREFIX "${mpo_ffmpeg_prefix}"
    CONFIGURE_COMMAND <SOURCE_DIR>/configure ${mpo_ffmpeg_configure_args}
    BUILD_COMMAND ${MPO_FFMPEG_MAKE} -j${MPO_FFMPEG_JOBS}
    INSTALL_COMMAND ${MPO_FFMPEG_MAKE} install
    BUILD_BYPRODUCTS ${mpo_ffmpeg_libs}
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON
    USES_TERMINAL_BUILD OFF)

add_library(mpo_ffmpeg INTERFACE)
add_library(mpo::ffmpeg ALIAS mpo_ffmpeg)
file(MAKE_DIRECTORY "${mpo_ffmpeg_install}/include")
target_include_directories(mpo_ffmpeg SYSTEM INTERFACE "${mpo_ffmpeg_install}/include")
target_link_libraries(mpo_ffmpeg INTERFACE ${mpo_ffmpeg_libs} Threads::Threads)
if(NOT WIN32)
    target_link_libraries(mpo_ffmpeg INTERFACE m)
    # FFmpeg's configure adds -latomic where the toolchain has it.
    find_library(MPO_FFMPEG_LIBATOMIC NAMES atomic libatomic.so.1)
    if(MPO_FFMPEG_LIBATOMIC)
        target_link_libraries(mpo_ffmpeg INTERFACE ${MPO_FFMPEG_LIBATOMIC})
    endif()
endif()
# Dependencies of an INTERFACE library are followed by everything that links it.
add_dependencies(mpo_ffmpeg mpo_ffmpeg_ep)
message(STATUS "FFmpeg: bundled ${MPO_FFMPEG_VERSION} (x86asm: ${MPO_FFMPEG_NASM})")
