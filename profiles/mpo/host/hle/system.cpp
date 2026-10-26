// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle/system.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace mpo::hle {

namespace {

constexpr std::uint32_t kErrnoFileNotFound = 0x80010002u;
constexpr std::uint32_t kErrnoInvalidArgument = 0x80010016u;
constexpr std::uint32_t kBadFd = 0x80020323u;
constexpr std::uint32_t kPspOpenWrite = 0x0002u;

// Ported from PPSSPP Core/HLE/sceUtility.cpp @ v1.20.4 (moduleLoadInfo): the
// user memory sceUtilityLoad(Av)Module reserves for each firmware module.
struct UtilityModuleInfo {
    std::uint32_t module;
    std::uint32_t size;
    const char *name;
    std::uint32_t dependencies[6]; // Zero-terminated.
};
constexpr UtilityModuleInfo kUtilityModules[] = {
    {0x100, 0x00014000, "net_common", {}},
    {0x101, 0x00020000, "net_adhoc", {}},
    {0x102, 0x00058000, "net_inet", {}},
    {0x103, 0x00006000, "net_parse_uri", {}},
    {0x104, 0x00002000, "net_parse_http", {}},
    {0x105, 0x00028000, "net_http", {0x102, 0x103, 0x104}},
    {0x106, 0x00044000, "net_ssl", {0x102}},
    {0x107, 0x00010000, "unk_0x107", {}},
    {0x108, 0x00008000, "usb_pspcm", {0x100, 0x102, 0x103, 0x104, 0x105}},
    {0x200, 0x00000000, "usb_mic", {}},
    {0x201, 0x00000000, "usb_cam", {}},
    {0x202, 0x00000000, "usb_gps", {}},
    {0x203, 0x00000000, "usb_unk_0x203", {}},
    {0x2ff, 0x00000000, "unk_0x2ff", {}},
    {0x300, 0x00000000, "av_avcodec", {}},
    {0x301, 0x00000000, "av_sascore", {}},
    {0x302, 0x00008000, "av_atrac3plus", {0x300}},
    {0x303, 0x0000c000, "av_mpegbase", {0x300}},
    {0x304, 0x00004000, "av_mp3", {}},
    {0x305, 0x0000a300, "av_vaudio", {}},
    {0x306, 0x00004000, "av_aac", {}},
    {0x307, 0x00000000, "av_g729", {}},
    {0x308, 0x0003c000, "av_mp4", {0x300}},
    {0x3fe, 0x00000000, "me_stuff", {}},
    {0x3ff, 0x00000000, "me_core", {}},
    {0x400, 0x0000c000, "np_common", {}},
    {0x401, 0x00018000, "np_service", {}},
    {0x402, 0x00048000, "np_matching2", {}},
    {0x403, 0x0000e000, "np_unk_0x403", {}},
    {0x500, 0x00000000, "np_drm", {}},
    {0x600, 0x00000000, "irda", {}},
    {0x601, 0x00000000, "unk_0x601", {}},
};
constexpr std::uint32_t kAvModuleBadId = 0x80110F01u;
constexpr std::uint32_t kAvModuleAlreadyLoaded = 0x80110F02u;
constexpr std::uint32_t kAvModuleNotLoaded = 0x80110F03u;
constexpr std::uint32_t kModuleBadId = 0x80111101u;
constexpr std::uint32_t kModuleAlreadyLoaded = 0x80111102u;
constexpr std::uint32_t kModuleNotLoaded = 0x80111103u;
constexpr std::uint32_t kLibraryNotFound = 0x8002013Cu;

} // namespace

// ---- BootIo -----------------------------------------------------------------

std::filesystem::path BootIo::host_path(const std::string &psp_path) const {
    std::string relative = psp_path;
    std::string device;
    if (const auto colon = relative.find(':'); colon != std::string::npos) {
        device = relative.substr(0, colon);
        relative.erase(0, colon + 1u);
    }
    std::transform(device.begin(), device.end(), device.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (device != "disc0" && device != "umd0" && device != "umd1") return {};
    std::replace(relative.begin(), relative.end(), '\\', '/');
    std::filesystem::path clean;
    for (const auto &part : std::filesystem::path(relative)) {
        const std::string name = part.string();
        if (name == "..") return {};
        if (name.empty() || name == "/" || name == ".") continue;
        clean /= part;
    }
    return clean.empty() ? std::filesystem::path() : disc_root_ / clean;
}

std::uint32_t BootIo::open(const std::string &psp_path, std::uint32_t flags) {
    if ((flags & kPspOpenWrite) != 0u) return kErrnoInvalidArgument;
    const std::filesystem::path path = host_path(psp_path);
    if (path.empty() || disc_root_.empty()) return kErrnoFileNotFound;
    std::ifstream in(path, std::ios::binary);
    if (!in) return kErrnoFileNotFound;
    File file;
    file.path = psp_path;
    file.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const std::uint32_t fd = next_fd_++;
    files_.emplace(fd, std::move(file));
    return fd;
}

std::uint32_t BootIo::close(std::uint32_t fd) {
    return files_.erase(fd) != 0u ? 0u : kBadFd;
}

BootIo::File *BootIo::file(std::uint32_t fd) {
    const auto found = files_.find(fd);
    return found != files_.end() ? &found->second : nullptr;
}

// ---- System ----------------------------------------------------------------

System::System(psprecomp::Runtime &runtime, const SystemConfig &config)
    : runtime_(runtime), config_(config), memory_(runtime.memory()), bridge_(runtime),
      modules_(*this), io_(config.disc_root) {
    kernel::KernelConfig kernel_config;
    kernel_config.memory = &memory_;
    kernel_config.clock = &clock_;
    kernel_config.hooks = &bridge_;
    kernel_config.fiber_stack_size = config.fiber_stack_size;
    kernel_ = std::make_unique<kernel::Kernel>(kernel_config);
    hle_ = std::make_unique<HleTable>(runtime_, *kernel_);
    hle_->set_unimplemented_policy(config.unimplemented);
    bridge_.set_callback_return(alloc_kernel_scratch(16u, "hle-callback-return"));
    bridge_.set_thread_return_handler(
        [this](kernel::Uid thread, std::uint32_t v0) { modules_.on_thread_return(thread, v0); });
    bridge_.set_thread_start_handler([this](const kernel::ThreadStart &start) {
        kernel::ThreadInfo info;
        if (!kernel_->thread_info(start.thread, info)) return;
        hle_->trace_event(
            "thread %d '%s' runs: entry=%08X gp=%08X sp=%08X a0=%08X a1=%08X priority=%02X",
            start.thread, info.name.c_str(), start.entry, start.gp, start.sp, start.a0, start.a1,
            static_cast<unsigned>(info.current_priority));
    });
    bridge_.install_preemption(*kernel_, config.preemption_interval,
                               [this] { clock_.advance(config_.us_per_preemption); });
    register_hle();
}

System::~System() {
    // The kernel abandons the fibers; nothing of the guest runs past here.
    hle_.reset();
    kernel_.reset();
}

std::uint32_t System::alloc_kernel_scratch(std::uint32_t size, const char *tag) {
    std::uint32_t rounded = size;
    return kernel_->kernel_memory().alloc(rounded, false, tag);
}

void System::register_hle() {
    register_kernel_hle(*this);
    register_module_hle(*this);
    register_boot_stubs(*this);
}

bool System::boot(std::span<const std::uint8_t> elf, const std::string &boot_path,
                  std::string *error) {
    const std::uint32_t uid = modules_.boot(elf, boot_path, error);
    return static_cast<std::int32_t>(uid) >= 0;
}

kernel::RunResult System::run(std::uint64_t until_us) {
    return kernel_->run(until_us);
}

std::uint32_t System::load_utility_module(std::uint32_t module, bool av) {
    const UtilityModuleInfo *info = nullptr;
    for (const UtilityModuleInfo &candidate : kUtilityModules)
        if (candidate.module == module) info = &candidate;
    if (info == nullptr) return av ? kAvModuleBadId : kModuleBadId;
    if (utility_modules_.contains(module))
        return av ? kAvModuleAlreadyLoaded : kModuleAlreadyLoaded;
    for (const std::uint32_t dependency : info->dependencies) {
        if (dependency == 0u) break;
        if (!utility_modules_.contains(dependency)) return kLibraryNotFound;
    }
    std::uint32_t address = 0;
    if (info->size != 0u) {
        std::uint32_t size = info->size;
        char name[64];
        std::snprintf(name, sizeof(name), "UtilityModule/%3x_%s", module, info->name);
        address = kernel_->user_memory().alloc(size, false, name);
    }
    utility_modules_[module] = address;
    hle_->trace_event("utility module %03X '%s': %u bytes at %08X", module, info->name, info->size,
                      address);
    return 0u;
}

std::uint32_t System::unload_utility_module(std::uint32_t module, bool av) {
    const auto found = utility_modules_.find(module);
    if (found == utility_modules_.end()) return av ? kAvModuleNotLoaded : kModuleNotLoaded;
    if (found->second != 0u) (void)kernel_->user_memory().free(found->second);
    utility_modules_.erase(found);
    return 0u;
}

} // namespace mpo::hle
