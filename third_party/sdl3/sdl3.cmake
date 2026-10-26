# SPDX-License-Identifier: GPL-3.0-or-later
#
# SDL3 (zlib license), pinned to a release tarball with its SHA-256. It is
# downloaded at configure time and built as a static library, so the build does
# not depend on a system SDL. Provides the target SDL3::SDL3-static (aliased as
# mpo::sdl3).
#
# Options:
#   MPO_ENABLE_SDL3      build SDL3 at all (OFF skips every SDL3-based target)
#   MPO_SDL3_SOURCE_DIR  use an already unpacked SDL3 3.4.18 tree (offline builds)
#
# On Linux, SDL3 only gets the audio back ends whose development headers exist at
# configure time (ALSA: libasound2-dev, PulseAudio: libpulse-dev, PipeWire:
# libpipewire-0.3-dev). Without them only the "dummy" and "disk" drivers are
# built, which is enough for the tests.

if(TARGET mpo::sdl3)
    return()
endif()

option(MPO_ENABLE_SDL3 "Download and build SDL3 (window, input and audio back end)" ON)
set(MPO_SDL3_SOURCE_DIR "" CACHE PATH "Unpacked SDL3 3.4.18 source tree (skips the download)")

if(NOT MPO_ENABLE_SDL3)
    return()
endif()

set(MPO_SDL3_VERSION "3.4.18")
set(MPO_SDL3_URL
    "https://github.com/libsdl-org/SDL/releases/download/release-${MPO_SDL3_VERSION}/SDL3-${MPO_SDL3_VERSION}.tar.gz")
set(MPO_SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")

# SDL is C; the root project only enables C++.
enable_language(C)

# Normal variables override SDL's option() defaults (policy CMP0077).
set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_DISABLE_INSTALL ON)
set(SDL_DISABLE_UNINSTALL ON)

# F16a only needs audio. With the video subsystem on, SDL's configure fails on
# Linux unless the X11/Wayland development packages are installed, so the other
# subsystems stay off until the phase that needs them (F10a: window and input)
# sets MPO_SDL3_AUDIO_ONLY=OFF and adds those packages to CI.
option(MPO_SDL3_AUDIO_ONLY "Build SDL3 with only the audio subsystem" ON)
if(MPO_SDL3_AUDIO_ONLY)
    foreach(mpo_sdl_subsystem IN ITEMS VIDEO GPU RENDER CAMERA JOYSTICK HAPTIC HIDAPI POWER
                                      SENSOR DIALOG TRAY)
        set(SDL_${mpo_sdl_subsystem} OFF)
    endforeach()
    # Skips SDL's "no X11 or Wayland found" configure error.
    set(SDL_UNIX_CONSOLE_BUILD ON)
endif()

include(FetchContent)
if(MPO_SDL3_SOURCE_DIR)
    FetchContent_Declare(mpo_sdl3 SOURCE_DIR "${MPO_SDL3_SOURCE_DIR}")
else()
    FetchContent_Declare(mpo_sdl3
        URL "${MPO_SDL3_URL}"
        URL_HASH SHA256=${MPO_SDL3_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()

FetchContent_MakeAvailable(mpo_sdl3)

if(NOT TARGET SDL3::SDL3-static)
    message(FATAL_ERROR "SDL3 did not provide SDL3::SDL3-static")
endif()
add_library(mpo::sdl3 ALIAS SDL3-static)
