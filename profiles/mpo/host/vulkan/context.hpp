// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vulkan instance, device, queue and memory allocator for the GE renderer.
//
// - Vulkan 1.2 is the minimum. When the device has 1.3, dynamic rendering is
//   used instead of render pass objects (MPO_VK_NO_13=1 forces the 1.2 path,
//   an A/B toggle in the sense of CLAUDE.md §7).
// - The validation layers (VK_LAYER_KHRONOS_validation, with synchronization
//   validation when the layer offers it) are requested on demand: Debug
//   builds and the tests turn them on. Every message is counted, so a caller
//   can fail on any validation error (CLAUDE.md §11).
// - Offscreen by default. A window system adds its instance extensions and a
//   surface factory; then the queue must also present and the device gets
//   VK_KHR_swapchain.
//
// volk keeps its function pointers in globals, so there is at most one live
// context per process (Create fails otherwise).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "vk_common.hpp"

namespace mpo::vk {

struct ContextOptions {
    std::string app_name = "mpo";
    // Requests VK_LAYER_KHRONOS_validation. When the layer is missing the
    // context is created without it (validation_active() says so), unless
    // `require_validation` is set.
    bool validation = false;
    bool require_validation = false;
    // Synchronization validation (VK_EXT_validation_features) on top.
    bool sync_validation = true;
    // Off: never use Vulkan 1.3 features even if the device has them.
    bool allow_vulkan_1_3 = true;
    // Optional device features the GE renderer can use (F15a). Each one also
    // has an environment toggle (MPO_VK_NO_DEPTH_CLAMP, MPO_VK_NO_RASTER_ORDER,
    // MPO_VK_NO_LOCAL_READ = 1 turns it off), so a frame can be bisected.
    bool allow_depth_clamp = true;
    bool allow_raster_order = true;
    bool allow_local_read = true;
    // Extra instance extensions (the window system's surface extensions).
    std::vector<std::string> instance_extensions;
    // The loader's vkGetInstanceProcAddr, when another library already loaded
    // it (SDL_Vulkan_GetVkGetInstanceProcAddr). Null: volk opens the system
    // loader itself.
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    // Creates the presentation surface once the instance exists. Returning
    // VK_NULL_HANDLE fails the creation.
    std::function<VkSurfaceKHR(VkInstance)> create_surface;
    // Substring of the device name to prefer (also MPO_VK_DEVICE).
    std::string preferred_device;
    // Validation and info messages, one line each. Null: stderr.
    std::function<void(std::string_view)> log;
};

struct DeviceCaps {
    std::uint32_t api_version = 0; // what the context uses (min of device and 1.3)
    std::uint32_t device_api_version = 0;
    std::string device_name;
    std::string driver_name;
    VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool dynamic_rendering = false;
    bool depth_clamp = false;
    // VK_EXT/ARM_rasterization_order_attachment_access (colour): input
    // attachment reads are ordered with the colour writes of earlier
    // fragments, so a shader can blend without barriers (F15a).
    bool raster_order = false;
    // VK_KHR_dynamic_rendering_local_read: input attachments and barriers
    // inside dynamic rendering (the 1.3 path). Without it the 1.3 path reads
    // the destination through a copy.
    bool local_read = false;
    // specVersion of VK_LAYER_KHRONOS_validation when it is active, else 0.
    std::uint32_t validation_layer_version = 0;
    bool validation_active = false;
    bool sync_validation_active = false;
    bool debug_utils = false;
    VkDeviceSize min_uniform_buffer_offset_alignment = 256;
    std::uint32_t max_image_dimension_2d = 4096;
    std::uint32_t max_viewport_dimension = 4096; // min of the two maxViewportDimensions
    float viewport_bounds_min = -8192.0f;
};

// Counted validation-layer messages (thread-safe: the layer may call from
// any thread).
struct ValidationLog {
    std::atomic<int> errors{0};
    std::atomic<int> warnings{0};
    std::atomic<int> infos{0};

    void Add(std::string message);
    // The first kMaxKept messages (errors and warnings).
    [[nodiscard]] std::vector<std::string> Messages() const;
    void Reset();

    static constexpr std::size_t kMaxKept = 64;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

class VulkanContext {
public:
    enum class Status {
        kOk,
        // No loader, no device or no device with Vulkan 1.2: tests skip.
        kUnavailable,
        kError,
    };

    static std::unique_ptr<VulkanContext> Create(const ContextOptions &options, Status *status,
                                                 std::string *error);
    ~VulkanContext();
    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] VkQueue queue() const { return queue_; }
    [[nodiscard]] std::uint32_t queue_family() const { return queue_family_; }
    [[nodiscard]] VmaAllocator allocator() const { return allocator_; }
    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] const DeviceCaps &caps() const { return caps_; }
    [[nodiscard]] ValidationLog &validation() { return validation_; }
    [[nodiscard]] const ValidationLog &validation() const { return validation_; }

    // Debug names and command buffer labels (VK_EXT_debug_utils, when
    // available): they show up in validation messages and in RenderDoc.
    void SetName(VkObjectType type, std::uint64_t handle, const char *name) const;
    template <typename T>
    void SetName(VkObjectType type, T handle, const char *name) const {
        if constexpr (std::is_pointer_v<T>) {
            SetName(type, reinterpret_cast<std::uint64_t>(handle), name);
        } else {
            SetName(type, static_cast<std::uint64_t>(handle), name);
        }
    }
    void BeginLabel(VkCommandBuffer cmd, const char *name) const;
    void EndLabel(VkCommandBuffer cmd) const;

    void Log(std::string_view line) const;

private:
    VulkanContext() = default;
    bool Init(const ContextOptions &options, Status *status, std::string *error);

    ContextOptions options_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    DeviceCaps caps_;
    ValidationLog validation_;
};

// "1.3.250".
std::string VersionString(std::uint32_t version);

// Validation default for tools: on in Debug builds, MPO_VK_VALIDATION=0/1
// overrides.
bool DefaultValidation();

} // namespace mpo::vk
