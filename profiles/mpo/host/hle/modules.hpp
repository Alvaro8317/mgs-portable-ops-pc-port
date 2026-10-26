// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module loading and ModuleMgrForUser (F08b).
//
// Loads PSP modules into user memory where PPSSPP puts them (same block
// allocator, same order), so the recompiled corpora of mgp_main and of the
// resident Konami modules (sound2, kjfs, zlibdec, generated for those
// addresses) find their code where it was recompiled:
//
//   - an ELF/PRX is loaded and relocated; its exports become `link` imports
//     (calling one jumps to the export), its own imports without HLE become
//     `unimplemented` ones (visible), and its module_start runs on a thread
//     as in PPSSPP's __KernelStartModule;
//   - an encrypted firmware module that PPSSPP replaces with HLE
//     (libfont.prx: sceFont_Library) becomes a "fake" module: its memory is
//     reserved from the ~PSP header like PPSSPP does, nothing runs.
//
// Semantics from PPSSPP Core/HLE/sceKernelModule.cpp and Core/ELF/ElfReader.cpp
// @ v1.20.4 (__KernelLoadELFFromPtr, ElfReader::LoadInto, __KernelStartModule,
// __KernelReturnFromModuleFunc, sceKernelStartModule/StopModule/...).

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "kernel/kernel.hpp"
#include "psprecomp/elf32.hpp"

namespace mpo::hle {

class System;

enum class ModuleStatus : std::uint8_t { kLoaded, kStarting, kStarted, kStopping, kStopped };

struct ModuleExport {
    std::string library;
    std::uint32_t nid{};
    std::uint32_t address{};
};

struct Module {
    kernel::Uid uid{};
    std::string name;
    std::string path;
    bool fake{};
    std::uint32_t base{};
    std::uint32_t size{};
    std::uint16_t attributes{};
    std::uint32_t gp{};
    std::uint32_t entry{};
    std::uint32_t start_func{};
    std::uint32_t stop_func{};
    std::uint32_t start_thread_priority{};
    std::uint32_t start_thread_stack_size{};
    std::uint32_t start_thread_attr{};
    std::uint32_t stop_thread_priority{};
    std::uint32_t stop_thread_stack_size{};
    std::uint32_t stop_thread_attr{};
    ModuleStatus status{ModuleStatus::kLoaded};
    std::vector<ModuleExport> exports;
    std::vector<psprecomp::PspImport> imports;
    // Threads waiting in sceKernelStart/StopModule and where they want the
    // module function's exit status.
    struct Waiter {
        kernel::Uid thread{};
        std::uint32_t status_addr{};
    };
    std::vector<Waiter> waiters;
    std::uint32_t function_exit_status{};
};

// PPSSPP's HLEModuleMeta names: firmware modules it never runs (HLE.cpp).
[[nodiscard]] bool is_hle_firmware_module(const std::string &name);

class ModuleManager {
public:
    explicit ModuleManager(System &system) : system_(system) {}

    // Loads an image as sceKernelLoadModule* does (load_address 0: first fit
    // from the bottom, or from the top with `from_top`). Returns the module UID
    // or a PSP error code (negative as int32).
    std::uint32_t load(std::span<const std::uint8_t> image, const std::string &path,
                       std::uint32_t load_address, bool from_top, std::string *error);
    // The boot module: loaded at its default address (0x08804000), then run by
    // a "root" thread with argv[0] = boot_path, as PPSSPP's __KernelLoadExec.
    std::uint32_t boot(std::span<const std::uint8_t> image, const std::string &boot_path,
                       std::string *error);

    // ModuleMgrForUser.
    std::uint32_t start_module(kernel::Uid uid, std::uint32_t arg_size, std::uint32_t arg_addr,
                               std::uint32_t status_addr, std::uint32_t option_addr);
    std::uint32_t stop_module(kernel::Uid uid, std::uint32_t arg_size, std::uint32_t arg_addr,
                              std::uint32_t status_addr, std::uint32_t option_addr);
    std::uint32_t unload_module(kernel::Uid uid);
    std::uint32_t get_module_id();
    std::uint32_t get_module_id_by_address(std::uint32_t address) const;

    // The thread of a module function (module_start/stop, the boot root)
    // returned with $v0 (CpuBridge's thread return handler).
    void on_thread_return(kernel::Uid thread, std::uint32_t v0);

    [[nodiscard]] const Module *find(kernel::Uid uid) const;
    [[nodiscard]] const Module *find_by_name(const std::string &name) const;
    [[nodiscard]] std::vector<const Module *> modules() const;

private:
    Module *get(kernel::Uid uid);
    std::uint32_t load_fake(std::span<const std::uint8_t> psp_header, const std::string &path,
                            bool from_top, std::string *error);
    void read_exports(Module &module, std::uint32_t ent_top, std::uint32_t ent_end);
    void link(Module &module);
    void unlink(const Module &module);
    void finish_module_function(Module &module, kernel::Uid thread, std::uint32_t exit_status);

    System &system_;
    std::map<kernel::Uid, std::unique_ptr<Module>> modules_;
    // Thread -> module whose start/stop function (or boot) it runs.
    std::map<kernel::Uid, kernel::Uid> function_threads_;
};

} // namespace mpo::hle
