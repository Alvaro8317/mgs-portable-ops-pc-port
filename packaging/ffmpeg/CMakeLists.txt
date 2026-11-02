# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds only the FFmpeg the port uses (third_party/ffmpeg/ffmpeg.cmake, the
# same flags as MPO_FFMPEG=bundled) and installs it as the tree that
# MPO_FFMPEG=prebuilt reads: include/, lib/, the licenses and the exact
# configure line. The Windows release carries it, built by the CI with the
# llvm-mingw pinned in profiles/mpo/config/toolchain.toml (F22b), because the
# player's Windows has no POSIX shell to run FFmpeg's configure.
#
#   cmake -S packaging/ffmpeg -B build-ffmpeg -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw.cmake -DMPO_LLVM_MINGW_ROOT=<dir>
#   cmake --build build-ffmpeg
#   cmake --install build-ffmpeg --prefix <release>/ffmpeg
#
# FFmpeg is LGPL-2.1-or-later: the release carries its source tarball next to
# the libraries (the release workflow copies it from the download).

cmake_minimum_required(VERSION 3.22)
project(mpo_ffmpeg_prebuilt LANGUAGES C)

set(MPO_FFMPEG "bundled" CACHE STRING "" FORCE)
get_filename_component(mpo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${mpo_root}/third_party/ffmpeg/ffmpeg.cmake")

add_custom_target(mpo_ffmpeg_prebuilt ALL DEPENDS mpo_ffmpeg_ep)
install(DIRECTORY "${CMAKE_BINARY_DIR}/third_party/ffmpeg/install/include/" DESTINATION include)
install(DIRECTORY "${CMAKE_BINARY_DIR}/third_party/ffmpeg/install/lib/" DESTINATION lib
        FILES_MATCHING PATTERN "*.a" PATTERN "*.lib" PATTERN "*.pc")
install(FILES "${mpo_root}/third_party/ffmpeg/LICENSE" "${mpo_root}/third_party/ffmpeg/NOTICE.md"
        DESTINATION .)
string(REPLACE ";" " " mpo_ffmpeg_configure_line "${mpo_ffmpeg_configure_args}")
file(WRITE "${CMAKE_BINARY_DIR}/BUILD.txt"
     "FFmpeg ${MPO_FFMPEG_VERSION} (LGPL-2.1-or-later), source: ffmpeg-${MPO_FFMPEG_VERSION}.tar.xz\n"
     "SHA-256 of the source: ${MPO_FFMPEG_SHA256}\n"
     "Compiler: ${CMAKE_C_COMPILER} (${CMAKE_C_COMPILER_VERSION})\n"
     "configure ${mpo_ffmpeg_configure_line}\n")
install(FILES "${CMAKE_BINARY_DIR}/BUILD.txt" DESTINATION .)
