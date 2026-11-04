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
// F25c: textures in VRAM are replaced and dumped too, except framebuffers and
// video frames, as in PPSSPP v1.20.4 (GPU/Common/TextureCacheCommon.cpp: a
// texture that falls on a framebuffer is taken from it and never reaches the
// replacer; one that sceMpeg wrote is STATUS_VIDEO and is not replaced):
// - a framebuffer is VRAM that a draw wrote (its colour buffer, and its
//   depth buffer when it writes depth), that the display shows, or that a
//   block transfer copied from a framebuffer. It is kept as a rectangle
//   (the scissor's rows and columns at the buffer's stride, one per buffer,
//   grown to cover every draw) and forgotten kTargetAge flips after it was
//   last drawn, shown or sampled (PPSSPP's FBO_OLD_AGE decimation);
// - a video frame is what sceMpeg wrote (VideoWritten), for kVideoAge flips
//   (VIDEO_DECIMATE_AGE), in RAM or VRAM.
// A texture any of whose level-0 bytes falls on one is drawn from memory as
// always, and not dumped. The decision is taken in mpo_ge, so softgpu and
// Vulkan always agree.
//
// The texel hash of a texture in RAM is memoized until the next GE run (the
// CPU may have written it in between); of one in VRAM, until the next flip
// or a block transfer or video frame that writes it (PPSSPP checks a texture
// once per frame too: a CPU write to VRAM is seen at the next frame).
//
// F26a, HD packs that look right:
// - mip levels: the pack's own (a.png|b.png, <key>_1.png...) or, without
//   them, a chain generated from a larger image with a 2 x 2 box filter
//   down to the GE texture's size (the level of that size is `native`
//   itself); see texture_provider.hpp for how each renderer samples them;
// - loading off the GE's thread (SetAsync): a draw whose replacement is not
//   decoded yet draws the game's texture, as PPSSPP does, and the PNG is
//   read on a DecodePool thread; a preload (StartPreload) decodes the pack
//   ahead, up to a memory budget. Without SetAsync (the default, and what
//   replays and captures use) the first draw waits for its PNG;
// - reloading a pack while the game runs (RequestReload, from any thread;
//   applied at the next GE run): its folder is read again and replaces the
//   set in place, so the active index and the hot switch stay as they were.
//   WatchFolders requests it when a pack's files change;
// - [filtering] of textures.ini (Filtering()).
// F26b: the dump also keeps dump.csv (dump_index.hpp): per key, its size,
// format, RAM or VRAM, the first stage it was drawn in, the stages, the
// draws and the frames. It adds up over sessions in the same folder. The
// stage comes from SetStageSource (mpo_boot: the stage/<s>/<s>.prx in the
// module list), asked at each GE run while dumping.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dump_index.hpp"
#include "decode_pool.hpp"
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

// F26a: an image in memory, RGBA8 bytes, row-major.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// F26a: the replacement of a GE texture of w x h from its level 0 and the
// pack's own levels 1, 2... (`levels`, each one half the size of the one
// before, at least 1: the first one that is not stops them, with a line in
// `notes`, as PPSSPP's "Replacement mipmap invalid"). Without levels of the
// pack and with `generate`, the chain is generated with a 2 x 2 box filter
// (ScaleToNative) while a level is at least twice the GE size, and the level
// of exactly the GE size is `native`. `native` is the level of the GE size
// when the pack has one, otherwise ScaleToNative of level 0. `id` is set.
std::unique_ptr<ge::ReplacedTexture> BuildReplacement(DecodedImage level0,
                                                      std::vector<DecodedImage> levels, int w,
                                                      int h, bool generate,
                                                      std::vector<std::string> *notes);

class ReplacementSet {
public:
    // Reads <dir>/textures.ini and scans the folder's root for hash-named
    // PNGs. Fails without a textures.ini or with one this port cannot use.
    static std::unique_ptr<ReplacementSet> Load(const std::filesystem::path &dir,
                                                std::string *error);
    // A set from a parsed ini (tests); files are relative to `dir`.
    ReplacementSet(std::filesystem::path dir, TexturesIni ini);
    // Cancels the loads still queued (the running ones finish on their own).
    ~ReplacementSet();
    ReplacementSet(const ReplacementSet &) = delete;
    ReplacementSet &operator=(const ReplacementSet &) = delete;
    // F26a: whether it was read from a folder (Load), so that a reload can
    // read it again.
    [[nodiscard]] bool from_folder() const { return from_folder_; }

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
    // The same, telling a key listed as "do not replace" (`*ignored`) from
    // one not listed.
    [[nodiscard]] const std::string *Lookup(const ReplacementKey &key, bool *ignored) const;

    // F26a: the files of the mip levels 1, 2... of `file` (empty: none).
    void SetLevels(const std::string &file, std::vector<std::string> levels);
    [[nodiscard]] const std::vector<std::string> &Levels(const std::string &file) const;
    // F26a: [filtering]: the filter for a texture's full key, with
    // LookupWildcard's order and then the global entry (a zero key), as
    // PPSSPP's FindFiltering. False when none applies.
    void AddFiltering(const ReplacementKey &key, ge::TextureFilter filter);
    [[nodiscard]] bool FindFiltering(const ReplacementKey &key, ge::TextureFilter *filter) const;
    [[nodiscard]] bool has_filtering() const { return !filtering_.empty(); }
    // F26a: generate the mip levels of an image larger than its texture
    // (on by default). Applies to the images loaded from then on.
    void SetGenerateMipmaps(bool on) { generate_mipmaps_ = on; }
    [[nodiscard]] bool generate_mipmaps() const { return generate_mipmaps_; }

    // Gives `file` an image from memory (RGBA8, iw x ih) instead of reading
    // the PNG: a pack built into the program, or a test.
    void AddImage(const std::string &file, int iw, int ih, std::vector<std::uint8_t> rgba);

    // The image of `file` for a texture of w x h, loaded on first use.
    // nullptr (and `error`, once) when it cannot be read. F26a: with a pool
    // (SetPool), the first calls return nullptr with `*pending` set while a
    // pool thread reads it; without one, the call reads it.
    const ge::ReplacedTexture *Get(const std::string &file, int w, int h, std::string *error,
                                   bool *pending = nullptr);

    // F26a: loads on `pool`'s threads from now on (nullptr: on the caller's).
    void SetPool(std::shared_ptr<DecodePool> pool) { pool_ = std::move(pool); }
    // F26a: decodes every PNG of the set on `pool`'s background queue, while
    // the decoded images take less than `budget_bytes`; Get then takes them
    // from there. A second call does nothing.
    void StartPreload(const std::shared_ptr<DecodePool> &pool, std::size_t budget_bytes);
    struct PreloadStats {
        std::size_t files = 0;   // PNGs in the set
        std::size_t decoded = 0; // decoded ahead
        std::size_t bytes = 0;   // what the ones not used yet take now
        std::size_t used = 0;    // taken by Get
        bool full = false;       // stopped at the budget
    };
    [[nodiscard]] PreloadStats preload_stats() const;
    // Loads started and not collected by Get yet.
    [[nodiscard]] std::size_t pending() const { return jobs_.size(); }
    // Waits until no load or preload of this set is running or queued
    // (tests). False on timeout.
    bool WaitIdle(std::chrono::milliseconds timeout) const;
    // Lines about the images loaded since the last call (bad mip levels).
    [[nodiscard]] std::vector<std::string> TakeNotes();

private:
    struct Shared;
    struct Job;
    // Gets the decoded `file` (from the preload or the PNG).
    static bool Decode(const std::filesystem::path &path, const std::string &file, Shared &shared,
                       DecodedImage *out, std::string *error);
    static void RunJob(Job &job);
    // The full path of a file of the pack (names are UTF-8).
    [[nodiscard]] std::filesystem::path PathOf(const std::string &file) const;

    bool from_folder_ = false;
    bool generate_mipmaps_ = true;
    std::shared_ptr<DecodePool> pool_;
    std::shared_ptr<Shared> shared_;
    std::map<std::tuple<std::string, int, int>, std::shared_ptr<Job>> jobs_;
    std::unordered_map<std::string, std::vector<std::string>> levels_;
    std::unordered_map<ReplacementKey, ge::TextureFilter, ReplacementKeyHash> filtering_;

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
    std::vector<std::string> notes_;
};

class TextureReplacer final : public ge::TextureProvider {
public:
    struct Stats {
        std::uint64_t lookups = 0;       // textured draws seen while a set or the dump is on
        std::uint64_t replaced = 0;      // draws that got a replacement
        std::uint64_t vram = 0;          // lookups of a texture in VRAM
        std::uint64_t vram_replaced = 0; // ... replaced (F25c)
        std::uint64_t framebuffer = 0;   // lookups skipped: a framebuffer (F25c)
        std::uint64_t video = 0;         // lookups skipped: a video frame (F25c)
        std::uint64_t targets = 0;       // framebuffer rectangles seen (F25c)
        std::uint64_t targets_aged = 0;  // ... forgotten after kTargetAge flips
        std::uint64_t flips = 0;
        std::uint64_t hashes = 0; // texel hashes computed (the rest came from the memo)
        std::uint64_t hashed_bytes = 0;
        std::uint64_t dumped = 0; // PNGs written by the dump
        std::uint64_t dump_errors = 0;
        std::uint64_t load_errors = 0;    // PNGs that could not be loaded
        std::uint64_t switches = 0;       // active set changes applied
        std::set<std::uint64_t> distinct; // ids of the replacements used
        // F26a.
        std::uint64_t pending = 0;       // draws given the game's texture while it loads
        std::uint64_t filtered = 0;      // draws whose filtering [filtering] forced
        std::uint64_t mipmapped = 0;     // draws given a replacement with mip levels
        std::uint64_t reloads = 0;       // packs read again (RequestReload)
        std::uint64_t reload_errors = 0; // ... that could not be, and were kept
    };

    TextureReplacer() = default;
    // Writes dump.csv if it has rows not written yet (errors go to the log).
    ~TextureReplacer() override;
    TextureReplacer(const TextureReplacer &) = delete;
    TextureReplacer &operator=(const TextureReplacer &) = delete;

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

    // Writes every texture drawn while no replacement covers it (F25c: in
    // VRAM too, but never a framebuffer or a video frame) into
    // `dir` as <key>.png (once per key), with a textures.ini (xxh64,
    // ignoreAddress) unless the folder already has one, whose options it
    // then follows. Empty: no dump.
    // F26b: also reads the folder's dump.csv (fails when it is not one) and
    // keeps adding to it; it is written every kDumpSaveFlips flips while it
    // changes, on FlushDump and when the replacer is destroyed.
    bool SetDumpDir(const std::filesystem::path &dir, std::string *error);
    // F26b: the name of the stage loaded now ("" for none), for dump.csv.
    // Asked at the start of each GE run while dumping; a name that is not a
    // valid stage name (DumpIndex::ValidStageName) counts as none.
    void SetStageSource(std::function<std::string()> source);
    // Writes dump.csv now if it changed. False, with `error`, on a failure.
    bool FlushDump(std::string *error);
    [[nodiscard]] const DumpIndex &dump_index() const { return index_; }
    [[nodiscard]] const std::string &stage() const { return stage_; }
    static constexpr std::uint64_t kDumpSaveFlips = 600;

    // Where load and dump errors go (one line each); nothing by default.
    void SetLog(std::function<void(std::string_view)> log) { log_ = std::move(log); }

    // F26a: load the replacements on pool threads (true) or on the GE's
    // thread at their first draw (false, the default: what replays,
    // recordings and captures need, so that no frame depends on time).
    // `threads` <= 0: DecodePool's default. Before the game starts.
    void SetAsync(bool on, int threads = 0);
    [[nodiscard]] bool async() const { return async_; }
    // F26a: decode the packs (those added already and later) ahead, in the
    // background, up to `budget_bytes` each (0: no preload). Before the game
    // starts. It never changes what is drawn, so it also runs in the
    // synchronous mode.
    void StartPreload(std::size_t budget_bytes, int threads = 0);
    // F26a: read every pack that came from a folder again, at the next GE
    // run. Safe from any thread (the F5 key, WatchFolders).
    void RequestReload() { reload_requests_.fetch_add(1, std::memory_order_relaxed); }
    // F26a: checks the folders of the packs every `period` on a thread of
    // its own and requests a reload when a file in one of them changed
    // (added, removed, resized or rewritten). Zero stops it.
    void WatchFolders(std::chrono::milliseconds period);
    // F26a: generate mip levels for the larger images (on by default), in
    // the sets added from now on and those reloaded.
    void SetGenerateMipmaps(bool on) { generate_mipmaps_ = on; }

    [[nodiscard]] const Stats &stats() const { return stats_; }

    // ge::TextureProvider.
    void BeginRun() override;
    void MemoryWritten(std::uint32_t addr, std::uint32_t size) override;
    void BeginDraw(const ge::GeState &gs) override;
    void BlockTransfer(const ge::MemoryRect &src, const ge::MemoryRect &dst) override;
    void VideoWritten(std::uint32_t addr, std::uint32_t size) override;
    void Flip(const ge::MemoryRect *shown) override;
    const ge::ReplacedTexture *Find(const ge::GeState &gs, const ge::MemoryView &mem,
                                    const ge::SamplerState &st,
                                    const ge::ClutLoadInfo &clut) override;
    [[nodiscard]] ge::TextureFilter Filtering() const override { return last_filter_; }

    // F25c: a framebuffer is forgotten this many flips after its last use
    // (PPSSPP: FBO_OLD_AGE).
    static constexpr std::uint64_t kTargetAge = 5;
    // A video frame stops being one this many flips after it was written
    // (PPSSPP: VIDEO_DECIMATE_AGE).
    static constexpr std::uint64_t kVideoAge = 4;
    // At most this many framebuffer rectangles (the least recently used one
    // goes first).
    static constexpr std::size_t kMaxTargets = 64;
    // The framebuffer rectangles known now (tests), and one line per
    // rectangle ("0x000000 stride 1024: rows 0-271, bytes 0-959"), for the
    // report of mpo_boot.
    [[nodiscard]] std::size_t target_count() const { return targets_.size(); }
    [[nodiscard]] std::vector<std::string> DescribeTargets() const;

private:
    // F25c: a framebuffer, in VRAM offsets: rows [y0, y1) of `stride` bytes
    // from `base`, bytes [x0, x1) of each row.
    struct Target {
        std::uint32_t base = 0;
        std::uint32_t stride = 0;
        std::uint32_t x0 = 0;
        std::uint32_t x1 = 0;
        std::uint32_t y0 = 0;
        std::uint32_t y1 = 0;
        std::uint64_t last_flip = 0;
    };
    struct Video {
        std::uint32_t begin = 0; // Normalized addresses (NormalizeAddress).
        std::uint32_t end = 0;
        std::uint64_t flip = 0;
    };
    struct VramMemo {
        std::uint32_t hash = 0;
        std::uint32_t begin = 0; // VRAM offsets of the bytes hashed
        std::uint32_t end = 0;
    };
    // What a draw's state said last time (BeginDraw's fast path).
    struct DrawRegs {
        std::uint32_t regs[9] = {};
        bool valid = false;
        bool operator==(const DrawRegs &) const = default;
    };

    // Marks a framebuffer rectangle (VRAM offsets) as used now.
    void AddTarget(std::uint32_t base, std::uint32_t stride, std::uint32_t x0, std::uint32_t x1,
                   std::uint32_t y0, std::uint32_t y1);
    void AddTargetRect(const ge::MemoryRect &rect);
    // The targets (bit i: targets_[i]) that some byte of `rect` (VRAM
    // offsets) falls on.
    [[nodiscard]] std::uint64_t TargetsHit(const ge::MemoryRect &rect) const;
    // The level-0 bytes of `st`, as rows (in VRAM offsets for a texture in
    // VRAM, a normalized address otherwise).
    [[nodiscard]] static ge::MemoryRect TextureRect(const ge::SamplerState &st);
    // Whether `st` is a framebuffer or a video frame now (refreshing the
    // framebuffers it falls on).
    [[nodiscard]] bool IsFramebuffer(const ge::SamplerState &st);
    [[nodiscard]] bool IsVideo(const ge::MemoryRect &rect) const;
    void ForgetVramHashes(std::uint32_t begin, std::uint32_t end);

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
    // F26b: counts a draw of `key` in dump.csv and dumps its PNG once.
    void NoteDump(const ReplacementKey &key, const ge::MemoryView &mem, const ge::SamplerState &st,
                  bool vram);
    void RefreshStage();
    void Log(const std::string &line) const;
    // F26a.
    void ConfigureSet(ReplacementSet &set);
    void ReloadSets();
    void StopWatching();
    void EnsurePool(int threads);

    // F26a: before sets_, so that the sets (and their queued loads) go first.
    std::shared_ptr<DecodePool> pool_;
    bool async_ = false;
    bool generate_mipmaps_ = true;
    std::size_t preload_budget_ = 0;
    std::atomic<std::uint32_t> reload_requests_{0};
    std::uint32_t reloads_applied_ = 0;
    ge::TextureFilter last_filter_ = ge::TextureFilter::kAuto;
    std::thread watcher_;
    std::mutex watch_mutex_;
    std::condition_variable watch_cv_;
    bool watch_stop_ = false;

    std::vector<std::unique_ptr<ReplacementSet>> sets_;
    std::atomic<int> requested_{-1};
    int active_ = -1;
    std::filesystem::path dump_dir_;
    TexturesIni dump_ini_;
    std::unordered_set<ReplacementKey, ReplacementKeyHash> dumped_;
    std::unordered_map<MemoKey, std::uint32_t, MemoKeyHash> memo_;
    // F25c.
    std::unordered_map<MemoKey, VramMemo, MemoKeyHash> vram_memo_;
    std::vector<Target> targets_;
    std::uint64_t targets_gen_ = 0; // changes when targets_ grows, shrinks or moves
    std::unordered_map<MemoKey, std::uint64_t, MemoKeyHash> target_memo_; // TargetsHit
    std::uint64_t target_memo_gen_ = 0;
    std::vector<Video> videos_;
    std::uint64_t flips_ = 0;
    DrawRegs last_draw_;
    std::uint64_t last_draw_gen_ = 0;
    std::uint64_t last_draw_flip_ = 0;
    std::function<void(std::string_view)> log_;
    Stats stats_;
    // F26b.
    DumpIndex index_;
    std::function<std::string()> stage_source_;
    std::string stage_;
    std::uint64_t index_saved_flip_ = 0;
};

} // namespace mpo::texreplace
