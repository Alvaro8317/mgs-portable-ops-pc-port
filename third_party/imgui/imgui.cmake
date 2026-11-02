# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dear ImGui (MIT) for the launcher (F22b), pinned to the v1.93.0 tag archive
# with its SHA-256. Downloaded at configure time like SDL3, never versioned.
# Provides the static library mpo_imgui (core, std::string input helpers, the
# SDL3 platform backend and the SDL_Renderer backend); needs mpo::sdl3.
#
# Options:
#   MPO_IMGUI_SOURCE_DIR  an already unpacked imgui 1.93.0 tree (offline builds)

if(TARGET mpo_imgui)
    return()
endif()
if(NOT TARGET mpo::sdl3)
    message(FATAL_ERROR "imgui.cmake needs SDL3 (third_party/sdl3/sdl3.cmake) first")
endif()

set(MPO_IMGUI_VERSION "1.93.0")
set(MPO_IMGUI_URL "https://github.com/ocornut/imgui/archive/refs/tags/v${MPO_IMGUI_VERSION}.tar.gz")
set(MPO_IMGUI_SHA256 "f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee")
set(MPO_IMGUI_SOURCE_DIR "" CACHE PATH "Unpacked Dear ImGui ${MPO_IMGUI_VERSION} tree (skips the download)")

include(FetchContent)
if(MPO_IMGUI_SOURCE_DIR)
    FetchContent_Declare(mpo_imgui_src SOURCE_DIR "${MPO_IMGUI_SOURCE_DIR}")
else()
    FetchContent_Declare(mpo_imgui_src
        URL "${MPO_IMGUI_URL}"
        URL_HASH SHA256=${MPO_IMGUI_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()
# imgui has no CMakeLists.txt, so this only downloads and unpacks it.
FetchContent_MakeAvailable(mpo_imgui_src)

set(mpo_imgui_dir "${mpo_imgui_src_SOURCE_DIR}")
add_library(mpo_imgui STATIC
    "${mpo_imgui_dir}/imgui.cpp"
    "${mpo_imgui_dir}/imgui_draw.cpp"
    "${mpo_imgui_dir}/imgui_tables.cpp"
    "${mpo_imgui_dir}/imgui_widgets.cpp"
    "${mpo_imgui_dir}/misc/cpp/imgui_stdlib.cpp"
    "${mpo_imgui_dir}/backends/imgui_impl_sdl3.cpp"
    "${mpo_imgui_dir}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(mpo_imgui SYSTEM PUBLIC "${mpo_imgui_dir}" "${mpo_imgui_dir}/backends"
                           "${mpo_imgui_dir}/misc/cpp")
target_compile_features(mpo_imgui PUBLIC cxx_std_20)
target_link_libraries(mpo_imgui PUBLIC mpo::sdl3)
# Third-party code: none of the project's warnings.
if(MSVC)
    target_compile_options(mpo_imgui PRIVATE /W0)
else()
    target_compile_options(mpo_imgui PRIVATE -w)
endif()
message(STATUS "Dear ImGui: ${MPO_IMGUI_VERSION} (${mpo_imgui_dir})")
