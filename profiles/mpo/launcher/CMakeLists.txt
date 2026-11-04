# SPDX-License-Identifier: GPL-3.0-or-later
#
# The launcher (F22b, docs/analysis/distribution.md §4):
# - mpo_launcher_core: what it reads and runs (status JSON, progress lines,
#   paths, folder listing), C++ only, tested without a window;
# - mpo_launcher: the SDL3 + Dear ImGui window that drives setup.py;
# - F25d: its texts in English and Spanish (launcher_strings.inc, one line
#   per text) are part of mpo_launcher_core.
#
# The player's machine does not need it (setup.py turns it off): the
# releases carry it already built, since it holds nothing of the game.

option(MPO_BUILD_LAUNCHER "Build the launcher (downloads Dear ImGui)" ON)

add_library(mpo_launcher_core STATIC launcher_core.cpp launcher_text.cpp)
target_include_directories(mpo_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mpo_launcher_core PUBLIC cxx_std_20)
mpo_target_warnings(mpo_launcher_core)
# F25d: launcher_strings.inc is UTF-8 (the Spanish texts). MSVC reads a file
# without a BOM in the system's code page unless told; GCC and Clang read
# UTF-8 already. PUBLIC: the launcher and the tests include it too.
if(MSVC)
    target_compile_options(mpo_launcher_core PUBLIC /utf-8)
endif()

if(MPO_BUILD_LAUNCHER AND TARGET mpo::sdl3 AND NOT MPO_SDL3_AUDIO_ONLY)
    include(${PROJECT_SOURCE_DIR}/third_party/imgui/imgui.cmake)
    add_executable(mpo_launcher WIN32 launcher_main.cpp task.cpp)
    target_link_libraries(mpo_launcher PRIVATE mpo_launcher_core mpo_imgui mpo::sdl3)
    # A build from the tree finds setup.py there; a release finds it next to
    # the executable first (find_paths).
    target_compile_definitions(mpo_launcher PRIVATE
        MPO_LAUNCHER_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
    mpo_target_warnings(mpo_launcher)
    if(MINGW)
        # One file to carry: no libc++ or libunwind DLL next to it.
        target_link_options(mpo_launcher PRIVATE -static)
    endif()
endif()
