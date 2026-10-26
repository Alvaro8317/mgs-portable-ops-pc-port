# SPDX-License-Identifier: GPL-3.0-or-later
#
# Window, presentation and input (F10a). Kept free of HLE registration (that
# is F10b):
# - mpo_frontend: framebuffer -> RGBA conversion and host input -> SceCtrlData
#   translation. C++ only, no SDL, so it is tested without a window.
# - mpo_frontend_sdl: the SDL3 window/presenter, SDL event -> translator glue
#   and the TOML input mapping (key and button names are SDL's).
# - mpo_frontend_demo: manual test tool (synthetic framebuffers in the four
#   formats, a replayed .ppdmp, input trace).

include(${PROJECT_SOURCE_DIR}/third_party/sdl3/sdl3.cmake)

add_library(mpo_frontend STATIC
    ctrl.cpp
    framebuffer.cpp
    input.cpp
)
target_include_directories(mpo_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mpo_frontend PUBLIC cxx_std_20)
# Analog scaling must round the same way everywhere (no fused multiply-add).
if(NOT MSVC)
    target_compile_options(mpo_frontend PRIVATE -ffp-contract=off)
endif()
mpo_target_warnings(mpo_frontend)

if(TARGET mpo::sdl3 AND NOT MPO_SDL3_AUDIO_ONLY)
    add_library(mpo_frontend_sdl STATIC
        input_config.cpp
        sdl_input.cpp
        sdl_presenter.cpp
    )
    target_include_directories(mpo_frontend_sdl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(mpo_frontend_sdl SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tomlplusplus/include)
    target_compile_definitions(mpo_frontend_sdl PRIVATE TOML_EXCEPTIONS=0)
    target_link_libraries(mpo_frontend_sdl PUBLIC mpo_frontend mpo::sdl3)
    mpo_target_warnings(mpo_frontend_sdl)

    add_executable(mpo_frontend_demo tools/frontend_demo.cpp)
    target_link_libraries(mpo_frontend_demo PRIVATE mpo_frontend_sdl mpo_ge_replay)
    mpo_target_warnings(mpo_frontend_demo)
endif()
