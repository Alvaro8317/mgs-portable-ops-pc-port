// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PSP system the recompiled game runs on (F08b): a psprecomp::Runtime, the
// kernel (mpo_kernel on fibers), the CPU bridge between them, the HLE import
// table and the module manager.
//
//   psprecomp::Runtime runtime(32 MiB);
//   psprecomp::register_generated_functions(runtime);   // + module corpora
//   mpo::hle::System system(runtime, config);
//   system.boot(eboot_elf_bytes, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &error);
//   system.run(until_us);
//
// Time (F09) is GameClock: virtual time that the visibility points of the
// recompiled code advance by `us_per_preemption` every `preemption_interval`
// dispatches, that HLE calls advance by the cycles they eat, and that jumps to
// the next event when every thread waits (game_clock.hpp). The vblank runs on
// it from the start (Display), and IO reads the user's ISO (IoManager).
// The controller (F10b) is sampled at the end of each vblank (Ctrl). The GE
// (F11b) runs the display lists of sceGe on mpo_ge (GeManager), and sceMpeg
// plays the videos on the media engine of F17a (Mpeg).
// The stage overlays (F13) install their recompiled corpus when the game loads
// them (OverlayManager). sceAudio, sceSasCore and sceAtrac3plus (F16b) mix on
// mpo_audio and hand every 64-sample block to Audio::set_output (Audio).
// sceMpeg shows the videos and plays their sound, and sceLibFont draws text
// with the firmware fonts of SystemConfig::font_dir (F17b, Fonts). The
// sceUtility dialogs save to ms0_root and draw with PPGe (F18b, Utility).

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hle/audio.hpp"
#include "hle/cpu_bridge.hpp"
#include "hle/ctrl.hpp"
#include "hle/display.hpp"
#include "hle/font.hpp"
#include "hle/game_clock.hpp"
#include "hle/ge_manager.hpp"
#include "hle/hle_table.hpp"
#include "hle/io_manager.hpp"
#include "hle/modules.hpp"
#include "hle/mpeg.hpp"
#include "hle/overlays.hpp"
#include "hle/runtime_memory.hpp"
#include "hle/utility.hpp"
#include "hle/volatile_memory.hpp"
#include "kernel/kernel.hpp"
#include "psprecomp/runtime.hpp"

namespace mpo::hle {

struct SystemConfig {
    // The user's ISO for disc0:/umd0: (empty: no disc, every open fails).
    std::filesystem::path disc_iso;
    // Host directory for ms0: (empty: no memory stick), also the savedata
    // root. mpo_boot passes the per-OS save folder unless --ms0 is given;
    // tests and traces use analysis/.
    std::filesystem::path ms0_root;
    // Host directory with the firmware fonts, flash0:/font/*.pgf (F17b; empty:
    // none, sceFontOpen fails). The user supplies them.
    std::filesystem::path font_dir;
    std::size_t fiber_stack_size = 64u * 1024u * 1024u;
    std::uint64_t preemption_interval = 4096u;
    std::uint64_t us_per_preemption = 100u;
    UnimplementedPolicy unimplemented = UnimplementedPolicy::kStop;
    // Run the 59.94 Hz vblank (off only for kernel tests that expect idle).
    bool vblank = true;
    // Throttle virtual time to the host's (GameClock::set_realtime).
    bool realtime = false;
    // The RTC base: the host's time and zone, unless fixed (traces, tests).
    bool rtc_from_host = true;
    std::int64_t rtc_base_seconds = 0;
    std::int64_t rtc_utc_offset_seconds = 0;
};

class System {
public:
    System(psprecomp::Runtime &runtime, const SystemConfig &config);
    ~System();
    System(const System &) = delete;
    System &operator=(const System &) = delete;

    [[nodiscard]] psprecomp::Runtime &runtime() noexcept { return runtime_; }
    [[nodiscard]] kernel::Kernel &kernel() noexcept { return *kernel_; }
    [[nodiscard]] GameClock &clock() noexcept { return clock_; }
    [[nodiscard]] const Rtc &rtc() const noexcept { return rtc_; }
    [[nodiscard]] CpuBridge &cpu() noexcept { return bridge_; }
    [[nodiscard]] HleTable &hle() noexcept { return *hle_; }
    [[nodiscard]] ModuleManager &modules() noexcept { return modules_; }
    [[nodiscard]] OverlayManager &overlays() noexcept { return overlays_; }
    [[nodiscard]] IoManager &io() noexcept { return *io_; }
    [[nodiscard]] Display &display() noexcept { return *display_; }
    [[nodiscard]] Interrupts &interrupts() noexcept { return *interrupts_; }
    [[nodiscard]] Ctrl &ctrl() noexcept { return *ctrl_; }
    [[nodiscard]] GeManager &ge() noexcept { return *ge_; }
    [[nodiscard]] Mpeg &mpeg() noexcept { return *mpeg_; }
    [[nodiscard]] Audio &audio() noexcept { return *audio_; }
    [[nodiscard]] Fonts &fonts() noexcept { return *fonts_; }
    [[nodiscard]] VolatileMemory &volatile_memory() noexcept { return volatile_memory_; }
    [[nodiscard]] Utility &utility() noexcept { return *utility_; }
    [[nodiscard]] const SystemConfig &config() const noexcept { return config_; }

    // Mounts the ISO and the memory stick of the config (boot() does it too).
    bool mount(std::string *error);
    // Loads the boot module and creates its root thread (ModuleManager::boot).
    bool boot(std::span<const std::uint8_t> elf, const std::string &boot_path, std::string *error);
    // Runs PSP threads until `until_us` of virtual time, a stop, or nothing can
    // run. A runtime stop (unimplemented import, guest fault, exit) is final.
    kernel::RunResult run(std::uint64_t until_us);

    // Guest memory scratch the host owns (kernel partition), for arguments the
    // HLE passes by address.
    std::uint32_t alloc_kernel_scratch(std::uint32_t size, const char *tag);

    // sceUtilityLoadAvModule & co: the user memory PPSSPP reserves per module.
    std::uint32_t load_utility_module(std::uint32_t module, bool av);
    std::uint32_t unload_utility_module(std::uint32_t module, bool av);

private:
    void register_hle();

    psprecomp::Runtime &runtime_;
    SystemConfig config_;
    RuntimeMemory memory_;
    GameClock clock_;
    Rtc rtc_;
    CpuBridge bridge_;
    std::unique_ptr<kernel::Kernel> kernel_;
    std::unique_ptr<HleTable> hle_;
    ModuleManager modules_;
    OverlayManager overlays_;
    std::unique_ptr<IoManager> io_;
    std::unique_ptr<Interrupts> interrupts_;
    std::unique_ptr<Display> display_;
    std::unique_ptr<Ctrl> ctrl_;
    std::unique_ptr<GeManager> ge_;
    std::unique_ptr<Mpeg> mpeg_;
    std::unique_ptr<Audio> audio_;
    std::unique_ptr<Fonts> fonts_;
    std::unique_ptr<Utility> utility_;
    VolatileMemory volatile_memory_;
    bool mounted_ = false;
    std::map<std::uint32_t, std::uint32_t> utility_modules_;
};

// The HLE groups (hle_kernel.cpp, hle_time.cpp, hle_system.cpp; IoManager,
// Display, Interrupts, Ctrl, GeManager, Mpeg, Audio, Fonts and Utility register
// their own).
void register_kernel_hle(System &system);
void register_module_hle(System &system);
void register_time_hle(System &system);
void register_system_hle(System &system);

} // namespace mpo::hle
