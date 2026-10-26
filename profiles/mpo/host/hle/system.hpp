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
// Time is a stub until F09: the kernel runs on a virtual clock that jumps to
// the next timer when every thread waits, and that the visibility points of
// the recompiled code advance by `us_per_preemption` every
// `preemption_interval` dispatches, so a guest busy-wait on the clock still
// makes progress.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hle/cpu_bridge.hpp"
#include "hle/hle_table.hpp"
#include "hle/modules.hpp"
#include "hle/runtime_memory.hpp"
#include "kernel/clock.hpp"
#include "kernel/kernel.hpp"
#include "psprecomp/runtime.hpp"

namespace mpo::hle {

struct SystemConfig {
    // Host directory with the disc's files (`PSP_GAME/...`), for the boot IO
    // stub (F09 replaces it with the ISO). Empty: every open fails.
    std::filesystem::path disc_root;
    std::size_t fiber_stack_size = 64u * 1024u * 1024u;
    std::uint64_t preemption_interval = 4096u;
    std::uint64_t us_per_preemption = 100u;
    UnimplementedPolicy unimplemented = UnimplementedPolicy::kStop;
};

// Minimal file access for the module loader until F09: sceIoOpen/Read/Lseek/
// Close of read-only files of the disc (disc0:/, umd0:/, umd1:/), read whole
// from `disc_root`. Registered as `stub`.
class BootIo {
public:
    explicit BootIo(std::filesystem::path disc_root) : disc_root_(std::move(disc_root)) {}

    struct File {
        std::string path;
        std::vector<std::uint8_t> data;
        std::uint64_t position{};
    };
    // Returns an fd (>= 3) or a PSP error code.
    std::uint32_t open(const std::string &psp_path, std::uint32_t flags);
    std::uint32_t close(std::uint32_t fd);
    [[nodiscard]] File *file(std::uint32_t fd);
    [[nodiscard]] std::filesystem::path host_path(const std::string &psp_path) const;

private:
    std::filesystem::path disc_root_;
    std::map<std::uint32_t, File> files_;
    std::uint32_t next_fd_ = 3;
};

class System {
public:
    System(psprecomp::Runtime &runtime, const SystemConfig &config);
    ~System();
    System(const System &) = delete;
    System &operator=(const System &) = delete;

    [[nodiscard]] psprecomp::Runtime &runtime() noexcept { return runtime_; }
    [[nodiscard]] kernel::Kernel &kernel() noexcept { return *kernel_; }
    [[nodiscard]] kernel::VirtualClock &clock() noexcept { return clock_; }
    [[nodiscard]] CpuBridge &cpu() noexcept { return bridge_; }
    [[nodiscard]] HleTable &hle() noexcept { return *hle_; }
    [[nodiscard]] ModuleManager &modules() noexcept { return modules_; }
    [[nodiscard]] BootIo &io() noexcept { return io_; }
    [[nodiscard]] const SystemConfig &config() const noexcept { return config_; }

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
    kernel::VirtualClock clock_;
    CpuBridge bridge_;
    std::unique_ptr<kernel::Kernel> kernel_;
    std::unique_ptr<HleTable> hle_;
    ModuleManager modules_;
    BootIo io_;
    std::map<std::uint32_t, std::uint32_t> utility_modules_;
};

// The HLE groups (hle_kernel.cpp, hle_stubs.cpp).
void register_kernel_hle(System &system);
void register_module_hle(System &system);
void register_boot_stubs(System &system);

} // namespace mpo::hle
