// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage overlays at run time (F13). The library loading follows Yakumo's
// profiles/mhp3rd/host/overlays.cpp (MIT); see overlays.hpp.

#include "hle/overlays.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <system_error>
#include <utility>

#include "hle/system.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/sha256.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mpo::hle {

namespace {

std::string short_hash(const std::string &sha256) {
    return sha256.substr(0, 12);
}

bool is_overlay_library(const std::filesystem::path &path) {
    const std::filesystem::path extension = path.extension();
    return extension == ".so" || extension == ".dll" || extension == ".dylib";
}

void *open_library(const std::filesystem::path &path) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(LoadLibraryW(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void *library_symbol(void *handle, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

std::string library_error() {
#if defined(_WIN32)
    return "error " + std::to_string(GetLastError());
#else
    const char *message = dlerror();
    return message != nullptr ? message : "unknown error";
#endif
}

// Process-global: a library is mapped once, whatever the number of Systems.
struct LoadedLibrary {
    const OverlayModuleInfo *info{};
    OverlayInstallFunction install{};
};

std::map<std::string, LoadedLibrary> &loaded_libraries() {
    static std::map<std::string, LoadedLibrary> libraries;
    return libraries;
}

} // namespace

void OverlayManager::add_corpus(const OverlayModuleInfo &info, OverlayInstallFunction install,
                                std::string origin) {
    OverlayCorpus corpus;
    corpus.name = info.name != nullptr ? info.name : "";
    corpus.origin = std::move(origin);
    corpus.base = info.base;
    corpus.size = info.size;
    corpus.prx_sha256 = info.prx_sha256 != nullptr ? info.prx_sha256 : "";
    corpus.text_offset = info.text_offset;
    corpus.text_size = info.text_size;
    corpus.text_sha256 = info.text_sha256 != nullptr ? info.text_sha256 : "";
    corpus.install = install;
    corpora_.push_back(std::move(corpus));
}

std::size_t OverlayManager::load_libraries(const std::filesystem::path &directory) {
    std::error_code ec;
    std::filesystem::directory_iterator entries(directory, ec);
    if (ec) {
        std::fprintf(stderr, "mpo: no overlay libraries in %s (%s)\n", directory.string().c_str(),
                     ec.message().c_str());
        return 0u;
    }
    std::vector<std::filesystem::path> paths;
    for (const std::filesystem::directory_entry &entry : entries) {
        if (entry.is_regular_file(ec) && is_overlay_library(entry.path()))
            paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::size_t added = 0;
    for (const std::filesystem::path &path : paths) {
        const std::string key = std::filesystem::absolute(path, ec).string();
        auto found = loaded_libraries().find(key);
        if (found == loaded_libraries().end()) {
            void *handle = open_library(path);
            if (handle == nullptr) {
                std::fprintf(stderr, "mpo: cannot load the overlay library %s: %s\n",
                             path.string().c_str(), library_error().c_str());
                continue;
            }
            const auto info_of = reinterpret_cast<decltype(&mpo_overlay_info)>(
                library_symbol(handle, "mpo_overlay_info"));
            const auto install = reinterpret_cast<decltype(&mpo_register_overlay)>(
                library_symbol(handle, "mpo_register_overlay"));
            if (info_of == nullptr || install == nullptr) {
                std::fprintf(stderr, "mpo: %s is not an overlay library\n", path.string().c_str());
                continue;
            }
            found = loaded_libraries().emplace(key, LoadedLibrary{info_of(), install}).first;
        }
        const OverlayModuleInfo *info = found->second.info;
        if (info == nullptr || info->abi_version != kOverlayAbiVersion ||
            info->runtime_size != sizeof(psprecomp::Runtime) ||
            info->context_size != sizeof(psprecomp::AllegrexContext)) {
            std::fprintf(stderr, "mpo: %s was built for another build of the runtime; rebuild it\n",
                         path.string().c_str());
            continue;
        }
        add_corpus(*info, found->second.install, path.filename().string());
        ++added;
    }
    return added;
}

std::string OverlayManager::text_sha256(const OverlayCorpus &corpus) const {
    const std::uint32_t address = corpus.base + corpus.text_offset;
    const std::uint8_t *text = system_.runtime().memory().raw_pointer(address, corpus.text_size);
    if (text == nullptr) return {};
    return psprecomp::sha256_bytes(std::span<const std::uint8_t>(text, corpus.text_size));
}

OverlayStats &OverlayManager::stats_for(const std::string &name, std::uint32_t base,
                                        std::uint32_t size) {
    for (OverlayStats &entry : stats_)
        if (entry.name == name && entry.base == base) return entry;
    OverlayStats entry;
    entry.name = name;
    entry.base = base;
    entry.size = size;
    stats_.push_back(entry);
    return stats_.back();
}

bool OverlayManager::on_module_loaded(kernel::Uid uid, const std::string &module_name,
                                      std::span<const std::uint8_t> file, std::uint32_t base,
                                      std::uint32_t size, std::uint32_t entry) {
    // mgp_main and the resident modules have their corpus linked in.
    if (system_.runtime().has_function(entry)) return false;
    const std::string sha256 = psprecomp::sha256_bytes(file);
    const OverlayCorpus *other_base = nullptr;
    for (std::size_t i = 0; i < corpora_.size(); ++i) {
        const OverlayCorpus &corpus = corpora_[i];
        if (corpus.prx_sha256 != sha256) continue;
        if (corpus.base != base || corpus.size != size) {
            other_base = &corpus;
            continue;
        }
        psprecomp::Runtime &runtime = system_.runtime();
        // Whatever an earlier module left in the window goes first.
        runtime.unregister_functions(base, base + size);
        Installed installed;
        installed.corpus = i;
        installed.loaded_text_sha256 = text_sha256(corpus);
        corpus.install(runtime);
        OverlayStats &stats = stats_for(corpus.name, base, size);
        ++stats.loads;
        ++stats.installs;
        installed_[uid] = std::move(installed);
        ++unverified_;
        system_.hle().trace_event("overlay %s (%s) installed at %08X..%08X for module %d",
                                  corpus.name.c_str(), corpus.origin.c_str(), base, base + size,
                                  uid);
        return true;
    }
    const std::string name = module_name + "@" + short_hash(sha256);
    OverlayStats &stats = stats_for(name, base, size);
    ++stats.loads;
    ++stats.interpreted;
    if (other_base != nullptr) {
        std::fprintf(
            stderr,
            "mpo: overlay %s has a corpus for %08X (size %X), but the game loaded it at "
            "%08X (size %X): it runs on the interpreter (profiles/mpo/config/stages.toml)\n",
            other_base->name.c_str(), other_base->base, other_base->size, base, size);
    } else {
        std::fprintf(stderr,
                     "mpo: no overlay corpus for module '%s' (PRX SHA-256 %s) at %08X: it runs on "
                     "the interpreter (build_overlays.py)\n",
                     module_name.c_str(), sha256.c_str(), base);
    }
    system_.hle().trace_event("module %d '%s' at %08X: no overlay corpus (%s)", uid,
                              module_name.c_str(), base, name.c_str());
    return false;
}

void OverlayManager::remove(const Installed &installed) {
    const OverlayCorpus &corpus = corpora_[installed.corpus];
    system_.runtime().unregister_functions(corpus.base, corpus.base + corpus.size);
    if (!installed.verified) --unverified_;
}

void OverlayManager::on_module_unloaded(kernel::Uid uid) {
    const auto found = installed_.find(uid);
    if (found == installed_.end()) return;
    remove(found->second);
    system_.hle().trace_event("overlay %s removed (module %d unloaded)",
                              corpora_[found->second.corpus].name.c_str(), uid);
    installed_.erase(found);
}

void OverlayManager::verify() {
    if (unverified_ == 0u) return;
    std::vector<kernel::Uid> mismatched;
    for (auto &[uid, installed] : installed_) {
        if (installed.verified) continue;
        const OverlayCorpus &corpus = corpora_[installed.corpus];
        const std::string actual = text_sha256(corpus);
        if (actual == corpus.text_sha256) {
            installed.verified = true;
            --unverified_;
            ++stats_for(corpus.name, corpus.base, corpus.size).verified;
            system_.hle().trace_event("overlay %s verified: the linked .text matches the corpus",
                                      corpus.name.c_str());
            continue;
        }
        if (actual == installed.loaded_text_sha256) continue; // Not linked yet.
        std::fprintf(stderr,
                     "mpo: overlay %s at %08X: the .text the game linked (SHA-256 %s) is not the "
                     "one the corpus was generated from (%s); the stage runs on the interpreter\n",
                     corpus.name.c_str(), corpus.base, actual.c_str(), corpus.text_sha256.c_str());
        system_.hle().trace_event("overlay %s: .text mismatch (%s), removed", corpus.name.c_str(),
                                  short_hash(actual).c_str());
        ++stats_for(corpus.name, corpus.base, corpus.size).mismatches;
        mismatched.push_back(uid);
    }
    for (const kernel::Uid uid : mismatched) {
        const auto found = installed_.find(uid);
        remove(found->second);
        installed_.erase(found);
        system_.modules().bind_import_stubs(uid);
    }
}

} // namespace mpo::hle
