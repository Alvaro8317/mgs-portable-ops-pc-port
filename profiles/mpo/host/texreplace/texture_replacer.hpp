// SPDX-License-Identifier: GPL-3.0-or-later
// The lookup order of ReplacementSet::Lookup is ported from PPSSPP
// GPU/Common/TextureReplacer.cpp (LookupWildcard, ScanForHashNamedFiles,
// ComputeAliasMap) @ v1.20.4 (GPL-2.0-or-later).
//
// F24c: texture replacement packs, for both renderers.
//
// - A ReplacementSet is one pack: a folder with a textures.ini (PPSSPP's
//   format, textures_ini.hpp) and PNG files, named by their key in the
//   folder's root (as --dump-textures writes them) or listed under
//   [hashes]. PNGs load on first use and stay loaded with the set.
// - TextureReplacer is the GE's TextureProvider (Ge::SetTextureProvider).
//   For each textured draw it computes the texture's key (texture_key.hpp)
//   and looks it up in the active set. It also dumps the textures in use as
//   PNGs named by key (SetDumpDir), to make packs from.
// - Several sets can be loaded and the active one switched while the game
//   runs (SetActiveSet: one pack per controller type for the button prompts,
//   F24b). The switch takes effect at the next GE run, so a frame never
//   mixes two packs inside one display list run.
//
// Only textures in RAM are replaced or dumped: in VRAM the game keeps its
// framebuffers and video frames (PPSSPP does not replace framebuffers either;
// docs/sessions/F24c.md).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "texture_key.hpp"
#include "texture_provider.hpp"
#include "textures_ini.hpp"

namespace mpo::texreplace {

// Largest replacement image side accepted (Vulkan guarantees 4096; 8 x a
// 512-texel texture).
inline constexpr int kMaxImageSide = 4096;

// Scales an RGBA8 image (iw x ih) to the GE texture's w x h for softgpu: a
// box filter where the image is larger (each texel the alpha-weighted mean
// of the image pixels it covers, so transparent pixels add no colour), the
// nearest pixel where it is smaller. The same size is a copy.
void ScaleToNative(const std::uint8_t *rgba, int iw, int ih, int w, int h,
                   std::vector<std::uint32_t> *out);

class ReplacementSet {
public:
    // Reads <dir>/textures.ini and scans the folder's root for hash-named
    // PNGs. Fails without a textures.ini or with one this port cannot use.
    static std::unique_ptr<ReplacementSet> Load(const std::filesystem::path &dir,
                                                std::string *error);
    // A set from a parsed ini (tests); files are relative to `dir`.
    ReplacementSet(std::filesystem::path dir, TexturesIni ini);

    [[nodiscard]] const std::filesystem::path &dir() const { return dir_; }
    [[nodiscard]] const TexturesIni &ini() const { return ini_; }
    // Keys with a file (or marked as not replaced).
    [[nodiscard]] std::size_t size() const { return aliases_.size(); }
    // Adds (or replaces) the file of a key; an empty file marks it as not
    // replaced.
    void AddAlias(const ReplacementKey &key, std::string file);

    // The file for a texture whose full key (address in the cachekey) is
    // `key`, following PPSSPP's LookupWildcard: the exact key (without the
    // address with ignoreAddress), then the CLUT/size half alone, then
    // without the address, then the texel hash alone. nullptr when nothing
    // matches or the match is marked as not replaced.
    [[nodiscard]] const std::string *Lookup(const ReplacementKey &key) const;

    // Gives `file` an image from memory (RGBA8, iw x ih) instead of reading
    // the PNG: a pack built into the program, or a test.
    void AddImage(const std::string &file, int iw, int ih, std::vector<std::uint8_t> rgba);

    // The image of `file` for a texture of w x h, loaded on first use.
    // nullptr (and `error`, once) when it cannot be read.
    const ge::ReplacedTexture *Get(const std::string &file, int w, int h, std::string *error);

private:
    std::filesystem::path dir_;
    TexturesIni ini_;
    std::unordered_map<ReplacementKey, std::string, ReplacementKeyHash> aliases_;
    std::map<std::tuple<std::string, int, int>, std::unique_ptr<ge::ReplacedTexture>> images_;
    std::set<std::string> failed_;
    struct Decoded {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
    };
    std::map<std::string, Decoded> in_memory_;
};

class TextureReplacer final : public ge::TextureProvider {
public:
    struct Stats {
        std::uint64_t lookups = 0;  // textured draws seen while a set or the dump is on
        std::uint64_t replaced = 0; // draws that got a replacement
        std::uint64_t vram = 0;     // draws with a texture in VRAM (never replaced)
        std::uint64_t hashes = 0;   // texel hashes computed (the rest came from the memo)
        std::uint64_t hashed_bytes = 0;
        std::uint64_t dumped = 0; // PNGs written by the dump
        std::uint64_t dump_errors = 0;
        std::uint64_t load_errors = 0;    // PNGs that could not be loaded
        std::uint64_t switches = 0;       // active set changes applied
        std::set<std::uint64_t> distinct; // ids of the replacements used
    };

    TextureReplacer() = default;

    // Adds a loaded pack and returns its index. Call it from the GE's thread
    // (or before the game starts). Not active until SetActiveSet.
    int AddSet(std::unique_ptr<ReplacementSet> set);
    [[nodiscard]] int set_count() const { return static_cast<int>(sets_.size()); }
    [[nodiscard]] ReplacementSet &set(int index) { return *sets_[static_cast<std::size_t>(index)]; }

    // The hot switch: the set the GE uses from its next run on (-1: none).
    // Safe from any thread.
    void SetActiveSet(int index) { requested_.store(index, std::memory_order_relaxed); }
    // The set asked for last, and the one the current run uses.
    [[nodiscard]] int requested_set() const { return requested_.load(std::memory_order_relaxed); }
    [[nodiscard]] int active_set() const { return active_; }

    // Writes every RAM texture drawn while no replacement covers it into
    // `dir` as <key>.png (once per key), with a textures.ini (xxh64,
    // ignoreAddress) unless the folder already has one, whose options it
    // then follows. Empty: no dump.
    bool SetDumpDir(const std::filesystem::path &dir, std::string *error);

    // Where load and dump errors go (one line each); nothing by default.
    void SetLog(std::function<void(std::string_view)> log) { log_ = std::move(log); }

    [[nodiscard]] const Stats &stats() const { return stats_; }

    // ge::TextureProvider.
    void BeginRun() override;
    void MemoryWritten(std::uint32_t addr, std::uint32_t size) override;
    const ge::ReplacedTexture *Find(const ge::GeState &gs, const ge::MemoryView &mem,
                                    const ge::SamplerState &st,
                                    const ge::ClutLoadInfo &clut) override;

private:
    struct MemoKey {
        std::uint32_t addr;
        std::uint32_t size; // w | h << 16
        std::uint32_t bufw;
        std::uint32_t format; // TexFormat | HashType << 8
        bool operator==(const MemoKey &) const = default;
    };
    struct MemoKeyHash {
        std::size_t operator()(const MemoKey &k) const {
            std::uint64_t h = (static_cast<std::uint64_t>(k.addr) << 32) ^ k.size;
            h *= 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<std::uint64_t>(k.bufw) << 16) ^ k.format;
            return static_cast<std::size_t>(h ^ (h >> 31));
        }
    };

    // The texel hash of level 0 of `st`, from the memo when this run already
    // hashed it (cleared at every run and every block transfer).
    std::uint32_t HashTexels(const ge::MemoryView &mem, const ge::SamplerState &st, HashType type);
    void Dump(const ReplacementKey &key, const ge::MemoryView &mem, const ge::SamplerState &st);
    void Log(const std::string &line) const;

    std::vector<std::unique_ptr<ReplacementSet>> sets_;
    std::atomic<int> requested_{-1};
    int active_ = -1;
    std::filesystem::path dump_dir_;
    TexturesIni dump_ini_;
    std::unordered_set<ReplacementKey, ReplacementKeyHash> dumped_;
    std::unordered_map<MemoKey, std::uint32_t, MemoKeyHash> memo_;
    std::function<void(std::string_view)> log_;
    Stats stats_;
};

} // namespace mpo::texreplace
