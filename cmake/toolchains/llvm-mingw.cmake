# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-compiling for Windows x86-64 from Linux with llvm-mingw (F22b). The
# release workflow uses it for what the Windows release carries already built
# (the launcher and FFmpeg); the game itself is always compiled on the
# player's machine, natively, with the same llvm-mingw (toolchain.toml).
#
#   cmake -S . -B build-win -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw.cmake \
#       -DMPO_LLVM_MINGW_ROOT=/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64
#
# MPO_LLVM_MINGW_ROOT (or the LLVM_MINGW environment variable) is the unpacked
# llvm-mingw release; without it the compilers are looked up in PATH.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(mpo_triple x86_64-w64-mingw32)

if(NOT MPO_LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW})
    set(MPO_LLVM_MINGW_ROOT "$ENV{LLVM_MINGW}")
endif()
# try_compile projects get the toolchain file again, without the cache.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MPO_LLVM_MINGW_ROOT)

if(MPO_LLVM_MINGW_ROOT)
    set(mpo_bin "${MPO_LLVM_MINGW_ROOT}/bin/")
    set(CMAKE_FIND_ROOT_PATH "${MPO_LLVM_MINGW_ROOT}/${mpo_triple}")
else()
    set(mpo_bin "")
endif()

set(CMAKE_C_COMPILER "${mpo_bin}${mpo_triple}-clang")
set(CMAKE_CXX_COMPILER "${mpo_bin}${mpo_triple}-clang++")
set(CMAKE_RC_COMPILER "${mpo_bin}${mpo_triple}-windres")
set(CMAKE_AR "${mpo_bin}llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${mpo_bin}llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_NM "${mpo_bin}llvm-nm" CACHE FILEPATH "")
set(CMAKE_STRIP "${mpo_bin}llvm-strip" CACHE FILEPATH "")

# Programs (Python, Ninja) come from the host; headers and libraries only
# from the toolchain's sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
