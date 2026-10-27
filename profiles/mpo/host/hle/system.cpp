// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle/system.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace mpo::hle {

namespace {

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
constexpr std::uint32_t kAvModuleAtrac3plus = 0x302u;
constexpr std::uint32_t kAvModuleBadId = 0x80110F01u;
constexpr std::uint32_t kAvModuleAlreadyLoaded = 0x80110F02u;
constexpr std::uint32_t kAvModuleNotLoaded = 0x80110F03u;
constexpr std::uint32_t kModuleBadId = 0x80111101u;
constexpr std::uint32_t kModuleAlreadyLoaded = 0x80111102u;
constexpr std::uint32_t kModuleNotLoaded = 0x80111103u;
constexpr std::uint32_t kLibraryNotFound = 0x8002013Cu;

} // namespace

// ---- System ----------------------------------------------------------------

System::System(psprecomp::Runtime &runtime, const SystemConfig &config)
    : runtime_(runtime), config_(config), memory_(runtime.memory()),
      rtc_(config.rtc_from_host ? Rtc::from_host()
                                : Rtc(config.rtc_base_seconds, config.rtc_utc_offset_seconds)),
      bridge_(runtime), modules_(*this), overlays_(*this) {
    kernel::KernelConfig kernel_config;
    kernel_config.memory = &memory_;
    kernel_config.clock = &clock_;
    kernel_config.hooks = &bridge_;
    kernel_config.fiber_stack_size = config.fiber_stack_size;
    kernel_ = std::make_unique<kernel::Kernel>(kernel_config);
    hle_ = std::make_unique<HleTable>(runtime_, *kernel_);
    io_ = std::make_unique<IoManager>(*this);
    interrupts_ = std::make_unique<Interrupts>(*this);
    display_ = std::make_unique<Display>(*this, *interrupts_);
    ctrl_ = std::make_unique<Ctrl>(*this, *display_);
    ge_ = std::make_unique<GeManager>(*this, *interrupts_);
    mpeg_ = std::make_unique<Mpeg>(*this);
    audio_ = std::make_unique<Audio>(*this);
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
    // Interrupt handlers run on a stack of their own (PPSSPP: the idle
    // thread's 4 KiB), with the boot module's $gp once it is loaded.
    constexpr std::uint32_t kInterruptStack = 0x1000u;
    const std::uint32_t stack = alloc_kernel_scratch(kInterruptStack, "interrupt-stack");
    interrupts_->set_context(0u, stack + kInterruptStack - 0x100u);
    clock_.set_realtime(config.realtime);
    register_hle();
    if (config.vblank) display_->start();
}

System::~System() {
    // The kernel abandons the fibers; nothing of the guest runs past here.
    audio_.reset();
    mpeg_.reset();
    ge_.reset();
    ctrl_.reset();
    display_.reset();
    interrupts_.reset();
    io_.reset();
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
    register_time_hle(*this);
    register_system_hle(*this);
    io_->register_hle();
    interrupts_->register_hle();
    display_->register_hle();
    ctrl_->register_hle();
    ge_->register_hle();
    mpeg_->register_hle();
    audio_->register_hle();
}

bool System::mount(std::string *error) {
    if (mounted_) return true;
    if (!config_.disc_iso.empty() && !io_->mount_disc(config_.disc_iso, error)) return false;
    if (!config_.ms0_root.empty() &&
        !io_->mount_memory_stick(config_.ms0_root, rtc_.utc_offset(), error))
        return false;
    mounted_ = true;
    return true;
}

bool System::boot(std::span<const std::uint8_t> elf, const std::string &boot_path,
                  std::string *error) {
    if (!mount(error)) return false;
    const std::uint32_t uid = modules_.boot(elf, boot_path, error);
    if (static_cast<std::int32_t>(uid) < 0) return false;
    // __KernelStartIdleThreads: the interrupt context gets the boot module's $gp.
    if (const Module *module = modules_.find(static_cast<kernel::Uid>(uid)))
        interrupts_->set_gp(module->gp);
    return true;
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
    // NotifyLoadStatusAtrac.
    if (module == kAvModuleAtrac3plus) audio_->notify_atrac_module(true, address);
    return 0u;
}

std::uint32_t System::unload_utility_module(std::uint32_t module, bool av) {
    const auto found = utility_modules_.find(module);
    if (found == utility_modules_.end()) return av ? kAvModuleNotLoaded : kModuleNotLoaded;
    if (found->second != 0u) (void)kernel_->user_memory().free(found->second);
    utility_modules_.erase(found);
    if (module == kAvModuleAtrac3plus) audio_->notify_atrac_module(false, 0u);
    return 0u;
}

} // namespace mpo::hle
