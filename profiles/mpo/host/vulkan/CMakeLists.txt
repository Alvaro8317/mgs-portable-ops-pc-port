# SPDX-License-Identifier: GPL-3.0-or-later
#
# F14a: the GE renderer on Vulkan (CLAUDE.md §9.3).
# - mpo_vulkan: context (instance, device, validation), resources, the
#   pipeline cache over the GE ubershader (SPIR-V embedded from
#   shaders/ge_shaders_spv.inc) and mpo::vk::GeRenderer, a mpo::ge::DrawBackend.
#   No SDL: it renders offscreen.
# - mpo_vulkan_sdl + mpo_vulkan_view: presentation in an SDL3 window
#   (swapchain), when SDL3 is built with video.
# mpo_ppdmp_replay (ge/tools) gets --renderer=vulkan from here.
#
# The build needs no Vulkan SDK: the headers, volk and VMA are downloaded
# pinned (third_party/vulkan), the loader is opened at run time, and the
# shaders are precompiled (shaders/compile_shaders.py).

include(${PROJECT_SOURCE_DIR}/third_party/vulkan/vulkan.cmake)
if(NOT TARGET mpo::volk)
    message(STATUS "F14a: Vulkan renderer disabled (MPO_ENABLE_VULKAN=OFF)")
    return()
endif()

# VMA's implementation is third-party code: no project warnings.
add_library(mpo_vulkan_vma STATIC vma_impl.cpp)
target_include_directories(mpo_vulkan_vma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpo_vulkan_vma PUBLIC mpo::volk mpo::vma_headers)
if(MSVC)
    target_compile_options(mpo_vulkan_vma PRIVATE /w)
else()
    target_compile_options(mpo_vulkan_vma PRIVATE -w)
endif()

add_library(mpo_vulkan STATIC
    context.cpp
    ge_renderer.cpp
    pipelines.cpp
    resources.cpp
    swapchain.cpp
    texture_decode.cpp
)
target_include_directories(mpo_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpo_vulkan PUBLIC mpo_ge mpo::volk mpo::vma_headers PRIVATE mpo_vulkan_vma)
# The line DDA follows mpo_ge's float maths: no fused multiply-add.
if(NOT MSVC)
    target_compile_options(mpo_vulkan PRIVATE -ffp-contract=off)
endif()
mpo_target_warnings(mpo_vulkan)

# --renderer=vulkan in the frame dump replayer.
target_link_libraries(mpo_ppdmp_replay PRIVATE mpo_vulkan)
target_compile_definitions(mpo_ppdmp_replay PRIVATE MPO_HAVE_VULKAN=1)

# Presentation in an SDL3 window (swapchain) and the dump viewer.
if(TARGET mpo_frontend_sdl)
    add_library(mpo_vulkan_sdl STATIC sdl_window.cpp)
    target_link_libraries(mpo_vulkan_sdl PUBLIC mpo_vulkan mpo_frontend_sdl)
    mpo_target_warnings(mpo_vulkan_sdl)

    add_executable(mpo_vulkan_view tools/vulkan_view.cpp)
    target_link_libraries(mpo_vulkan_view PRIVATE mpo_vulkan_sdl mpo_ge_replay)
    mpo_target_warnings(mpo_vulkan_view)
endif()
