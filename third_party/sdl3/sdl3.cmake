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
#   MPO_SDL3_AUDIO_ONLY  build only the audio subsystem (no window or input)
#
# On Linux, the video drivers follow the headers found at configure time too:
# X11 needs libx11-dev and libxext-dev (libxcursor-dev, libxi-dev,
# libxrandr-dev, libxfixes-dev, libxss-dev and libxtst-dev add optional
# features), Wayland needs libwayland-dev and libxkbcommon-dev. The "dummy" and
# "offscreen" drivers are always built.
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

# Subsystems. F16a (audio) and F10a (window, renderer, keyboard and gamepads)
# need audio, video, render, joystick/gamepad (with HIDAPI, which the virtual
# joystick driver used by the tests depends on), haptic and sensor. GPU,
# camera, dialog and tray stay off: the GE renderer will be Vulkan (CLAUDE.md
# §9.3), but SDL_VULKAN stays on for SDL_Vulkan_CreateSurface.
#
# MPO_SDL3_AUDIO_ONLY=ON brings back the F16a audio-only build (no window).
option(MPO_SDL3_AUDIO_ONLY "Build SDL3 with only the audio subsystem" OFF)
set(SDL_GPU OFF)
set(SDL_CAMERA OFF)
set(SDL_DIALOG OFF)
set(SDL_TRAY OFF)
if(MPO_SDL3_AUDIO_ONLY)
    foreach(mpo_sdl_subsystem IN ITEMS VIDEO RENDER JOYSTICK HAPTIC HIDAPI POWER SENSOR)
        set(SDL_${mpo_sdl_subsystem} OFF)
    endforeach()
    # Skips SDL's "no X11 or Wayland found" configure error.
    set(SDL_UNIX_CONSOLE_BUILD ON)
else()
    foreach(mpo_sdl_subsystem IN ITEMS VIDEO RENDER JOYSTICK HAPTIC HIDAPI POWER SENSOR)
        set(SDL_${mpo_sdl_subsystem} ON)
    endforeach()
    if(UNIX AND NOT APPLE)
        # SDL's configure stops with a fatal error when an X11 extension it
        # wants (all ON by default) has no headers, and when neither X11
        # (libx11-dev, libxext-dev) nor Wayland (libwayland-dev,
        # libxkbcommon-dev) is there. Each extension is turned on only when its
        # header exists, and without a desktop the build goes on with only the
        # "dummy" and "offscreen" video drivers (enough for the headless
        # tests), saying so.
        foreach(mpo_x11_ext IN ITEMS
                "XCURSOR=X11/Xcursor/Xcursor.h" "XDBE=X11/extensions/Xdbe.h"
                "XINPUT=X11/extensions/XInput2.h" "XFIXES=X11/extensions/Xfixes.h"
                "XSYNC=X11/extensions/sync.h" "XRANDR=X11/extensions/Xrandr.h"
                "XSCRNSAVER=X11/extensions/scrnsaver.h" "XSHAPE=X11/extensions/shape.h"
                "XTEST=X11/extensions/XTest.h")
            string(REPLACE "=" ";" mpo_x11_pair "${mpo_x11_ext}")
            list(GET mpo_x11_pair 0 mpo_x11_name)
            list(GET mpo_x11_pair 1 mpo_x11_header)
            find_path(MPO_X11_${mpo_x11_name}_H NAMES ${mpo_x11_header})
            if(MPO_X11_${mpo_x11_name}_H)
                set(SDL_X11_${mpo_x11_name} ON)
            else()
                set(SDL_X11_${mpo_x11_name} OFF)
                list(APPEND mpo_x11_missing ${mpo_x11_name})
            endif()
        endforeach()
        # SDL's XFixes support also needs XInput2.
        if(NOT SDL_X11_XINPUT)
            set(SDL_X11_XFIXES OFF)
        endif()
        if(mpo_x11_missing)
            message(STATUS "SDL3: X11 extensions without headers (off): ${mpo_x11_missing}")
        endif()
        find_path(MPO_X11_XLIB_H NAMES X11/Xlib.h)
        find_path(MPO_X11_XEXT_H NAMES X11/extensions/Xext.h)
        find_path(MPO_WAYLAND_CLIENT_H NAMES wayland-client.h)
        find_path(MPO_XKBCOMMON_H NAMES xkbcommon/xkbcommon.h)
        if(NOT ((MPO_X11_XLIB_H AND MPO_X11_XEXT_H) OR (MPO_WAYLAND_CLIENT_H AND MPO_XKBCOMMON_H)))
            message(WARNING
                "SDL3: no X11 (libx11-dev, libxext-dev) or Wayland (libwayland-dev, "
                "libxkbcommon-dev) headers found. SDL3 is built without a desktop video "
                "driver: windows only work with SDL_VIDEO_DRIVER=dummy or offscreen.")
            set(SDL_UNIX_CONSOLE_BUILD ON)
        endif()
    endif()
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
