# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vulkan build dependencies for the GE renderer (F14a), each pinned to a tag
# with the SHA-256 of its GitHub archive and downloaded at configure time (like
# SDL3). Only their sources are used: their own CMake projects are not added.
#
# - Vulkan-Headers v1.4.350 (Apache-2.0 OR MIT): API headers. Nothing links
#   against a Vulkan SDK or a system loader library.
# - volk 1.4.350 (MIT): meta-loader. It opens the system loader
#   (libvulkan.so.1 / vulkan-1.dll) at run time, so the build needs no Vulkan
#   SDK and a machine without Vulkan still runs everything else.
# - VulkanMemoryAllocator v3.4.0 (MIT): GPU memory, single header.
#
# Targets: mpo::vulkan_headers (interface, VK_NO_PROTOTYPES), mpo::volk (static
# C library) and mpo::vma_headers (interface). All includes are SYSTEM, so the
# project warnings (/W4 /WX, -Wconversion...) do not apply to them.
#
# Options:
#   MPO_ENABLE_VULKAN               OFF skips every Vulkan target
#   MPO_VULKAN_HEADERS_SOURCE_DIR   unpacked Vulkan-Headers v1.4.350 (offline)
#   MPO_VOLK_SOURCE_DIR             unpacked volk 1.4.350 (offline)
#   MPO_VMA_SOURCE_DIR              unpacked VulkanMemoryAllocator v3.4.0 (offline)

if(TARGET mpo::volk)
    return()
endif()

option(MPO_ENABLE_VULKAN "Download Vulkan-Headers, volk and VMA and build the Vulkan renderer" ON)
set(MPO_VULKAN_HEADERS_SOURCE_DIR "" CACHE PATH "Unpacked Vulkan-Headers v1.4.350 (skips the download)")
set(MPO_VOLK_SOURCE_DIR "" CACHE PATH "Unpacked volk 1.4.350 (skips the download)")
set(MPO_VMA_SOURCE_DIR "" CACHE PATH "Unpacked VulkanMemoryAllocator v3.4.0 (skips the download)")

if(NOT MPO_ENABLE_VULKAN)
    return()
endif()

include(FetchContent)

# mpo_vulkan_fetch(<name> <url> <sha256> <override dir>): downloads (or takes
# the override) without adding the dependency's own CMake project
# (SOURCE_SUBDIR points to a directory that does not exist).
function(mpo_vulkan_fetch name url sha256 override)
    if(override)
        FetchContent_Declare(${name} SOURCE_DIR "${override}" SOURCE_SUBDIR mpo-no-cmake)
    else()
        FetchContent_Declare(${name}
            URL "${url}"
            URL_HASH SHA256=${sha256}
            DOWNLOAD_EXTRACT_TIMESTAMP ON
            SOURCE_SUBDIR mpo-no-cmake)
    endif()
    FetchContent_MakeAvailable(${name})
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

mpo_vulkan_fetch(mpo_vulkan_headers
    "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.350.tar.gz"
    "6dd105e5cc7ddab6e7b611ae2c1872740d1727557cc8bf9daf13d6de1e4b3999"
    "${MPO_VULKAN_HEADERS_SOURCE_DIR}")
mpo_vulkan_fetch(mpo_volk
    "https://github.com/zeux/volk/archive/refs/tags/1.4.350.tar.gz"
    "e47c6efe5294bb03729a976b385864bba5daf46ec60f0bdea11e1d1446345f9a"
    "${MPO_VOLK_SOURCE_DIR}")
mpo_vulkan_fetch(mpo_vma
    "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz"
    "822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1"
    "${MPO_VMA_SOURCE_DIR}")

foreach(mpo_vk_file IN ITEMS
        "${mpo_vulkan_headers_SOURCE_DIR}/include/vulkan/vulkan_core.h"
        "${mpo_volk_SOURCE_DIR}/volk.c"
        "${mpo_vma_SOURCE_DIR}/include/vk_mem_alloc.h")
    if(NOT EXISTS "${mpo_vk_file}")
        message(FATAL_ERROR "Vulkan dependencies: missing ${mpo_vk_file}")
    endif()
endforeach()

add_library(mpo_vulkan_headers INTERFACE)
target_include_directories(mpo_vulkan_headers SYSTEM INTERFACE
    "${mpo_vulkan_headers_SOURCE_DIR}/include")
# Every Vulkan call goes through volk's function pointers.
target_compile_definitions(mpo_vulkan_headers INTERFACE VK_NO_PROTOTYPES)
add_library(mpo::vulkan_headers ALIAS mpo_vulkan_headers)

# volk is C. Built without the project warnings.
enable_language(C)
add_library(mpo_volk STATIC "${mpo_volk_SOURCE_DIR}/volk.c")
target_include_directories(mpo_volk SYSTEM PUBLIC "${mpo_volk_SOURCE_DIR}")
target_link_libraries(mpo_volk PUBLIC mpo_vulkan_headers ${CMAKE_DL_LIBS})
add_library(mpo::volk ALIAS mpo_volk)

add_library(mpo_vma_headers INTERFACE)
target_include_directories(mpo_vma_headers SYSTEM INTERFACE "${mpo_vma_SOURCE_DIR}/include")
add_library(mpo::vma_headers ALIAS mpo_vma_headers)
