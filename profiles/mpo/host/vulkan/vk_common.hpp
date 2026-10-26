// SPDX-License-Identifier: GPL-3.0-or-later
//
// Common includes for the Vulkan renderer: volk (the loader, every vk*
// function is a pointer it fills in) and VulkanMemoryAllocator (declarations
// only; the implementation is compiled once in vma_impl.cpp).

#pragma once

#include <cstdint>
#include <string>

#include <volk.h>

// VMA gets its Vulkan entry points from volk at run time (see
// VulkanContext::Create), never from a static loader library.
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif
#include <vk_mem_alloc.h>

namespace mpo::vk {

// VkResult as its enum name (VK_ERROR_DEVICE_LOST...).
const char *ResultName(VkResult result);

// "<what>: <VkResult name>".
std::string ResultError(const char *what, VkResult result);

} // namespace mpo::vk
