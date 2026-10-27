// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage overlays at run time (F13).
//
// mgp_main loads one stage at a time (stage/<s>/<s>.prx) with
// sceKernelLoadModuleByID, starts it, then links it to itself by applying the
// stage's .rlc in guest memory (0x08805C0C) and calls
// sceKernelDcacheWritebackAll (docs/formats/rlc.md, "Cómo lo aplica el
// juego"). The corpus of a stage is generated ahead of time from the PRX with
// that .rlc already applied (build_overlays.py), so the recompiled code is
// what the guest memory holds once the game has linked it.
//
// OverlayManager keeps the stage corpora (overlay libraries, or ones linked
// in) and drives them from the module loader:
//
//   - on_module_loaded: the PRX the game loads is identified by its SHA-256
//     and the base the allocator gave it; the matching corpus is installed
//     before module_start runs and before the loader binds the import stubs
//     (the corpus has its own import wrappers). Stages that share a PRX also
//     share their .rlc, so (PRX, base) names one corpus. No match: the stage
//     runs on the interpreter, and the report says why;
//   - verify (from sceKernelDcacheWritebackAll): each installed overlay not
//     yet verified hashes its .text in guest memory. The linked image ->
//     verified. The image as loaded (the game has not applied the .rlc yet)
//     -> still pending. Anything else -> the corpus does not describe that
//     memory: it is removed, the stubs are bound again, the interpreter runs
//     the stage and the mismatch is reported (stderr, trace and report);
//   - on_module_unloaded: the corpus leaves the dispatch table.
//
// The libraries stay mapped for the rest of the process: a thread may still
// have native frames of a corpus that left the dispatch table.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "hle/overlay_module.hpp"
#include "kernel/kernel.hpp"

namespace mpo::hle {

class System;

struct OverlayCorpus {
    std::string name;
    // The library it came from ("" when linked in).
    std::string origin;
    std::uint32_t base{};
    std::uint32_t size{};
    std::string prx_sha256;
    std::uint32_t text_offset{};
    std::uint32_t text_size{};
    std::string text_sha256;
    OverlayInstallFunction install{};
};

// Per stage, for the report.
struct OverlayStats {
    std::string name;
    std::uint32_t base{};
    std::uint32_t size{};
    std::uint32_t loads{};
    std::uint32_t installs{};
    std::uint32_t verified{};
    std::uint32_t mismatches{};
    // Loaded with no corpus for that PRX at that base.
    std::uint32_t interpreted{};
};

class OverlayManager {
public:
    explicit OverlayManager(System &system) : system_(system) {}

    // A corpus linked into the executable (tests) or from a library.
    void add_corpus(const OverlayModuleInfo &info, OverlayInstallFunction install,
                    std::string origin);
    // Loads every overlay library (.so, .dll, .dylib) in `directory`; prints
    // why a file is skipped. Returns the number of corpora added.
    std::size_t load_libraries(const std::filesystem::path &directory);
    [[nodiscard]] const std::vector<OverlayCorpus> &corpora() const noexcept { return corpora_; }

    // ModuleManager::load, after relocating `file` at `base` and before
    // binding the import stubs. Returns true when a corpus was installed.
    // `entry` is the module's start function: a module whose entry no corpus
    // covers counts as interpreted.
    bool on_module_loaded(kernel::Uid uid, const std::string &module_name,
                          std::span<const std::uint8_t> file, std::uint32_t base,
                          std::uint32_t size, std::uint32_t entry);
    void on_module_unloaded(kernel::Uid uid);
    // sceKernelDcacheWritebackAll: checks the installed overlays not yet
    // verified (cheap when there are none).
    void verify();

    [[nodiscard]] bool installed(kernel::Uid uid) const { return installed_.contains(uid); }
    [[nodiscard]] const std::vector<OverlayStats> &stats() const noexcept { return stats_; }

private:
    struct Installed {
        std::size_t corpus{};
        // SHA-256 of the .text right after loading (before the game's .rlc).
        std::string loaded_text_sha256;
        bool verified{};
    };

    std::string text_sha256(const OverlayCorpus &corpus) const;
    OverlayStats &stats_for(const std::string &name, std::uint32_t base, std::uint32_t size);
    void remove(const Installed &installed);

    System &system_;
    std::vector<OverlayCorpus> corpora_;
    std::map<kernel::Uid, Installed> installed_;
    std::size_t unverified_{};
    std::vector<OverlayStats> stats_;
};

} // namespace mpo::hle
