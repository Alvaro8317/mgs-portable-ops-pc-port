// SPDX-License-Identifier: GPL-3.0-or-later

#include "context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "env.hpp"

namespace mpo::vk {
namespace {

constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";

// One live context per process (volk's function pointers are global).
std::atomic<int> g_live_contexts{0};

bool HasExtension(const std::vector<VkExtensionProperties> &list, const char *name) {
    return std::any_of(list.begin(), list.end(), [name](const VkExtensionProperties &e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

std::vector<VkExtensionProperties> InstanceExtensions(const char *layer) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> out(count);
    if (vkEnumerateInstanceExtensionProperties(layer, &count, out.data()) != VK_SUCCESS) return {};
    out.resize(count);
    return out;
}

// The layer's specVersion, or 0 when it is not installed.
std::uint32_t LayerVersion(const char *name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return 0;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return 0;
    layers.resize(count);
    for (const VkLayerProperties &l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return std::max<std::uint32_t>(l.specVersion, 1);
    }
    return 0;
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice dev) {
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr) != VK_SUCCESS)
        return {};
    std::vector<VkExtensionProperties> out(count);
    if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, out.data()) != VK_SUCCESS)
        return {};
    out.resize(count);
    return out;
}

// The validation layer must know VK_KHR_dynamic_rendering_local_read to
// validate it: 1.3.275 (Ubuntu 24.04, the CI) reports its feature structure
// as unknown and every barrier inside dynamic rendering as an error. With an
// older layer active, the 1.3 path reads the destination through a copy
// (F15a). 1.3.276 is the first header version after the one that layer
// names; the exact first layer that knows the extension is not verified.
constexpr std::uint32_t kLocalReadLayerVersion = VK_MAKE_API_VERSION(0, 1, 3, 276);

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT *data,
                                             void *user) {
    auto *ctx = static_cast<VulkanContext *>(user);
    const char *level = "info";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        level = "error";
        ++ctx->validation().errors;
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        level = "warning";
        ++ctx->validation().warnings;
    } else {
        ++ctx->validation().infos;
        return VK_FALSE;
    }
    std::string line = std::string("vulkan ") + level;
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0) line += " (performance)";
    line += ": ";
    if (data && data->pMessageIdName) line += std::string("[") + data->pMessageIdName + "] ";
    if (data && data->pMessage) line += data->pMessage;
    ctx->Log(line);
    ctx->validation().Add(std::move(line));
    return VK_FALSE;
}

int DeviceTypeRank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 3;
    default:
        return 4;
    }
}

} // namespace

void ValidationLog::Add(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (messages_.size() < kMaxKept) messages_.push_back(std::move(message));
}

std::vector<std::string> ValidationLog::Messages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_;
}

void ValidationLog::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.clear();
    errors = 0;
    warnings = 0;
    infos = 0;
}

std::string VersionString(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

bool DefaultValidation() {
    const std::string env = GetEnv("MPO_VK_VALIDATION");
    if (!env.empty()) return env != "0";
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

std::unique_ptr<VulkanContext> VulkanContext::Create(const ContextOptions &options, Status *status,
                                                     std::string *error) {
    Status local_status = Status::kError;
    std::string local_error;
    if (!status) status = &local_status;
    if (!error) error = &local_error;
    if (g_live_contexts.load() != 0) {
        *status = Status::kError;
        *error = "another VulkanContext is alive (volk keeps global function pointers)";
        return nullptr;
    }
    std::unique_ptr<VulkanContext> ctx(new VulkanContext());
    ++g_live_contexts;
    if (!ctx->Init(options, status, error)) return nullptr;
    *status = Status::kOk;
    return ctx;
}

void VulkanContext::Log(std::string_view line) const {
    if (options_.log) {
        options_.log(line);
    } else {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
    }
}

bool VulkanContext::Init(const ContextOptions &options, Status *status, std::string *error) {
    options_ = options;
    const VkResult init = options.get_instance_proc_addr
                              ? (volkInitializeCustom(options.get_instance_proc_addr), VK_SUCCESS)
                              : volkInitialize();
    if (init != VK_SUCCESS) {
        *status = Status::kUnavailable;
        *error = "no Vulkan loader (libvulkan.so.1 / vulkan-1.dll) found";
        return false;
    }
    const std::uint32_t loader_version = volkGetInstanceVersion();
    if (loader_version < VK_API_VERSION_1_2) {
        *status = Status::kUnavailable;
        *error = "the Vulkan loader only supports " + VersionString(loader_version) +
                 " (1.2 is the minimum)";
        return false;
    }

    // Instance layers and extensions.
    std::vector<const char *> layers;
    std::vector<const char *> extensions;
    for (const std::string &e : options.instance_extensions) extensions.push_back(e.c_str());
    const auto available = InstanceExtensions(nullptr);
    if (HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        caps_.debug_utils = true;
    }
    bool use_validation_features = false;
    if (options.validation) {
        if (const std::uint32_t layer_version = LayerVersion(kValidationLayer);
            layer_version != 0) {
            layers.push_back(kValidationLayer);
            caps_.validation_active = true;
            caps_.validation_layer_version = layer_version;
            const auto layer_ext = InstanceExtensions(kValidationLayer);
            if (options.sync_validation &&
                HasExtension(layer_ext, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
                extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                use_validation_features = true;
            }
            if (!caps_.debug_utils && HasExtension(layer_ext, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                caps_.debug_utils = true;
            }
        } else if (options.require_validation) {
            *status = Status::kError;
            *error = std::string(kValidationLayer) + " is not installed";
            return false;
        } else {
            Log("vulkan: validation requested but VK_LAYER_KHRONOS_validation is not installed; "
                "running without it");
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = options.app_name.c_str();
    app.applicationVersion = 1;
    app.pEngineName = "mpo";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback = DebugCallback;
    dci.pUserData = this;

    VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT vf{};
    vf.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    vf.enabledValidationFeatureCount = 1;
    vf.pEnabledValidationFeatures = enables;

    // Messages during vkCreateInstance/vkDestroyInstance go to the same
    // callback.
    const void **next = &ici.pNext;
    if (caps_.validation_active && caps_.debug_utils) {
        *next = &dci;
        next = &dci.pNext;
    }
    if (use_validation_features) {
        *next = &vf;
        caps_.sync_validation_active = true;
    }

    VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER || r == VK_ERROR_LAYER_NOT_PRESENT) {
        *status = r == VK_ERROR_INCOMPATIBLE_DRIVER ? Status::kUnavailable : Status::kError;
        *error = ResultError("vkCreateInstance", r);
        return false;
    }
    if (r != VK_SUCCESS) {
        *status = Status::kError;
        *error = ResultError("vkCreateInstance", r);
        return false;
    }
    volkLoadInstanceOnly(instance_);

    if (caps_.validation_active && caps_.debug_utils) {
        dci.pNext = nullptr;
        r = vkCreateDebugUtilsMessengerEXT(instance_, &dci, nullptr, &messenger_);
        if (r != VK_SUCCESS) {
            *status = Status::kError;
            *error = ResultError("vkCreateDebugUtilsMessengerEXT", r);
            return false;
        }
    }

    if (options.create_surface) {
        surface_ = options.create_surface(instance_);
        if (surface_ == VK_NULL_HANDLE) {
            *status = Status::kError;
            *error = "the window system could not create a Vulkan surface";
            return false;
        }
    }

    // Physical device.
    std::uint32_t count = 0;
    r = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (r != VK_SUCCESS || count == 0) {
        *status = Status::kUnavailable;
        *error = "no Vulkan device (no ICD installed?)";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    devices.resize(count);

    std::string preferred = options.preferred_device;
    if (preferred.empty()) preferred = GetEnv("MPO_VK_DEVICE");

    struct Candidate {
        VkPhysicalDevice dev;
        std::uint32_t family;
        VkPhysicalDeviceProperties props;
        int rank;
    };
    std::vector<Candidate> candidates;
    std::string rejected;
    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.apiVersion < VK_API_VERSION_1_2) {
            rejected += std::string(" ") + props.deviceName + " (Vulkan " +
                        VersionString(props.apiVersion) + ")";
            continue;
        }
        std::uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, families.data());
        bool found = false;
        std::uint32_t family = 0;
        for (std::uint32_t i = 0; i < qcount && !found; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
            if (surface_ != VK_NULL_HANDLE) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present);
                if (!present) continue;
            }
            found = true;
            family = i;
        }
        if (!found) {
            rejected += std::string(" ") + props.deviceName + " (no graphics/present queue)";
            continue;
        }
        if (surface_ != VK_NULL_HANDLE) {
            const auto exts = DeviceExtensions(dev);
            if (!HasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                rejected += std::string(" ") + props.deviceName + " (no VK_KHR_swapchain)";
                continue;
            }
        }
        int rank = DeviceTypeRank(props.deviceType);
        if (!preferred.empty() &&
            std::string(props.deviceName).find(preferred) != std::string::npos)
            rank = -1;
        candidates.push_back({dev, family, props, rank});
    }
    if (candidates.empty()) {
        *status = Status::kUnavailable;
        *error = "no usable Vulkan 1.2 device:" + rejected;
        return false;
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.rank < b.rank; });
    const Candidate &chosen = candidates.front();
    physical_ = chosen.dev;
    queue_family_ = chosen.family;
    caps_.device_name = chosen.props.deviceName;
    caps_.device_type = chosen.props.deviceType;
    caps_.device_api_version = chosen.props.apiVersion;
    caps_.min_uniform_buffer_offset_alignment =
        std::max<VkDeviceSize>(chosen.props.limits.minUniformBufferOffsetAlignment, 16);
    caps_.max_image_dimension_2d = chosen.props.limits.maxImageDimension2D;
    caps_.max_viewport_dimension = std::min(chosen.props.limits.maxViewportDimensions[0],
                                            chosen.props.limits.maxViewportDimensions[1]);
    caps_.viewport_bounds_min = chosen.props.limits.viewportBoundsRange[0];

    const bool use_13 = options.allow_vulkan_1_3 && GetEnv("MPO_VK_NO_13") != "1" &&
                        chosen.props.apiVersion >= VK_API_VERSION_1_3;
    caps_.api_version = use_13 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

    // Optional extensions (F15a): reading the destination in the shader.
    const auto dev_exts = DeviceExtensions(physical_);
    const char *raster_order_ext = nullptr;
    if (options.allow_raster_order && GetEnv("MPO_VK_NO_RASTER_ORDER") != "1") {
        if (HasExtension(dev_exts, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) {
            raster_order_ext = VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
        } else if (HasExtension(dev_exts,
                                VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) {
            raster_order_ext = VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
        }
    }
    const bool layer_knows_local_read =
        !caps_.validation_active || caps_.validation_layer_version >= kLocalReadLayerVersion;
    const bool try_local_read =
        use_13 && options.allow_local_read && GetEnv("MPO_VK_NO_LOCAL_READ") != "1" &&
        HasExtension(dev_exts, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    if (try_local_read && !layer_knows_local_read) {
        Log("vulkan: VK_KHR_dynamic_rendering_local_read not used: the validation layer (" +
            VersionString(caps_.validation_layer_version) + ") predates it");
    }

    // Features.
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT fro{};
    fro.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr{};
    flr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    void **tail = &f12.pNext;
    if (use_13) {
        *tail = &f13;
        tail = &f13.pNext;
    }
    if (raster_order_ext) {
        *tail = &fro;
        tail = &fro.pNext;
    }
    if (try_local_read && layer_knows_local_read) *tail = &flr;
    vkGetPhysicalDeviceFeatures2(physical_, &f2);

    VkPhysicalDeviceVulkan13Features want13{};
    want13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features want12{};
    want12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 want{};
    want.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    want.pNext = &want12;
    if (f2.features.depthClamp && options.allow_depth_clamp &&
        GetEnv("MPO_VK_NO_DEPTH_CLAMP") != "1") {
        want.features.depthClamp = VK_TRUE;
        caps_.depth_clamp = true;
    }
    std::vector<const char *> dev_ext;
    if (surface_ != VK_NULL_HANDLE) dev_ext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    void **want_tail = &want12.pNext;
    if (use_13 && f13.dynamicRendering) {
        want13.dynamicRendering = VK_TRUE;
        *want_tail = &want13;
        want_tail = &want13.pNext;
        caps_.dynamic_rendering = true;
    }
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT want_ro{};
    want_ro.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
    if (raster_order_ext && fro.rasterizationOrderColorAttachmentAccess) {
        want_ro.rasterizationOrderColorAttachmentAccess = VK_TRUE;
        *want_tail = &want_ro;
        want_tail = &want_ro.pNext;
        dev_ext.push_back(raster_order_ext);
        caps_.raster_order = true;
    }
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR want_lr{};
    want_lr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR;
    if (caps_.dynamic_rendering && flr.dynamicRenderingLocalRead) {
        want_lr.dynamicRenderingLocalRead = VK_TRUE;
        *want_tail = &want_lr;
        dev_ext.push_back(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
        caps_.local_read = true;
    }

    // Driver name (core in 1.2).
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical_, &props2);
    caps_.driver_name = std::string(driver.driverName) + " " + driver.driverInfo;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci2{};
    dci2.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci2.pNext = &want;
    dci2.queueCreateInfoCount = 1;
    dci2.pQueueCreateInfos = &qci;
    dci2.enabledExtensionCount = static_cast<std::uint32_t>(dev_ext.size());
    dci2.ppEnabledExtensionNames = dev_ext.data();
    r = vkCreateDevice(physical_, &dci2, nullptr, &device_);
    if (r != VK_SUCCESS) {
        *status = Status::kError;
        *error = ResultError("vkCreateDevice", r);
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VmaVulkanFunctions funcs{};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.vulkanApiVersion = caps_.api_version;
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.pVulkanFunctions = &funcs;
    r = vmaCreateAllocator(&aci, &allocator_);
    if (r != VK_SUCCESS) {
        *status = Status::kError;
        *error = ResultError("vmaCreateAllocator", r);
        return false;
    }
    SetName(VK_OBJECT_TYPE_DEVICE, device_, "mpo device");
    SetName(VK_OBJECT_TYPE_QUEUE, queue_, "mpo queue");
    return true;
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    if (allocator_ != VK_NULL_HANDLE) vmaDestroyAllocator(allocator_);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_ != VK_NULL_HANDLE)
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    --g_live_contexts;
}

void VulkanContext::SetName(VkObjectType type, std::uint64_t handle, const char *name) const {
    if (!caps_.debug_utils || device_ == VK_NULL_HANDLE || !vkSetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &info);
}

void VulkanContext::BeginLabel(VkCommandBuffer cmd, const char *name) const {
    if (!caps_.debug_utils || !vkCmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
}

void VulkanContext::EndLabel(VkCommandBuffer cmd) const {
    if (!caps_.debug_utils || !vkCmdEndDebugUtilsLabelEXT) return;
    vkCmdEndDebugUtilsLabelEXT(cmd);
}

const char *ResultName(VkResult result) {
    switch (result) {
#define MPO_VK_RESULT(x)                                                                           \
    case x:                                                                                        \
        return #x;
        MPO_VK_RESULT(VK_SUCCESS)
        MPO_VK_RESULT(VK_NOT_READY)
        MPO_VK_RESULT(VK_TIMEOUT)
        MPO_VK_RESULT(VK_EVENT_SET)
        MPO_VK_RESULT(VK_EVENT_RESET)
        MPO_VK_RESULT(VK_INCOMPLETE)
        MPO_VK_RESULT(VK_SUBOPTIMAL_KHR)
        MPO_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY)
        MPO_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        MPO_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED)
        MPO_VK_RESULT(VK_ERROR_DEVICE_LOST)
        MPO_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED)
        MPO_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT)
        MPO_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT)
        MPO_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT)
        MPO_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER)
        MPO_VK_RESULT(VK_ERROR_TOO_MANY_OBJECTS)
        MPO_VK_RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED)
        MPO_VK_RESULT(VK_ERROR_FRAGMENTED_POOL)
        MPO_VK_RESULT(VK_ERROR_OUT_OF_POOL_MEMORY)
        MPO_VK_RESULT(VK_ERROR_SURFACE_LOST_KHR)
        MPO_VK_RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        MPO_VK_RESULT(VK_ERROR_OUT_OF_DATE_KHR)
#undef MPO_VK_RESULT
    default:
        return "VK_ERROR_(unknown)";
    }
}

std::string ResultError(const char *what, VkResult result) {
    return std::string(what) + ": " + ResultName(result) + " (" +
           std::to_string(static_cast<int>(result)) + ")";
}

} // namespace mpo::vk
