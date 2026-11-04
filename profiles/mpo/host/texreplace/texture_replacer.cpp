// SPDX-License-Identifier: GPL-3.0-or-later
// ReplacementSet::Load and Lookup: ported from PPSSPP
// GPU/Common/TextureReplacer.cpp @ v1.20.4 (GPL-2.0-or-later).
// F26a: the mip levels (ReplacedTexture::Prepare/LoadLevelData's size
// check), FindFiltering and the asynchronous load (ReplacedTexture::Poll on
// PPSSPP's thread manager) follow GPU/Common/ReplacedTexture.cpp and
// TextureReplacer.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "texture_replacer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "png_io.hpp"
#include "sampler.hpp"

namespace mpo::texreplace {
namespace {

std::atomic<std::uint64_t> g_next_image_id{1};

bool IsHashFileName(const std::string &stem) {
    // ScanForHashNamedFiles: 24 characters, or 26-27 with "_<level>".
    return stem.size() == 24 || (stem.size() >= 26 && stem.size() <= 27 && stem[24] == '_');
}

// F25c: one address per byte: VRAM mirrors fold onto 0x04000000.
std::uint32_t NormalizeAddress(std::uint32_t addr) {
    const std::uint32_t c = ge::MemoryView::Canonical(addr);
    return ge::MemoryView::IsVramAddress(c)
               ? ge::MemoryView::kVramBase | (c & (ge::MemoryView::kVramSize - 1))
               : c;
}

std::uint32_t VramOffset(std::uint32_t addr) {
    return addr & (ge::MemoryView::kVramSize - 1);
}

std::int64_t FloorDiv(std::int64_t n, std::int64_t d) {
    std::int64_t q = n / d;
    if (n % d != 0 && ((n < 0) != (d < 0))) --q;
    return q;
}

std::int64_t CeilDiv(std::int64_t n, std::int64_t d) {
    return -FloorDiv(-n, d);
}

// LookupWildcard: the full key, then with parts of it zeroed (see Lookup).
template <typename Map>
typename Map::const_iterator LookupWildcard(const Map &map, const ReplacementKey &full,
                                            bool ignore) {
    // FindReplacement drops the address before the lookup with ignoreAddress.
    const std::uint64_t cachekey = ignore ? full.cachekey & 0xFFFFFFFFull : full.cachekey;
    const std::uint32_t hash = full.hash;
    const ReplacementKey order[] = {
        {cachekey, hash},
        {cachekey & 0xFFFFFFFFull, 0},     // only the CLUT/size half
        {cachekey, 0},                     // no data hash (not with ignoreAddress)
        {cachekey & 0xFFFFFFFFull, hash},  // no address
        {cachekey & ~0xFFFFFFFFull, hash}, // address, no CLUT (not with ignoreAddress)
        {0, hash},                         // any texture with these texels
    };
    for (std::size_t i = 0; i < std::size(order); ++i) {
        if (ignore && (i == 2 || i == 4)) continue;
        auto it = map.find(order[i]);
        if (it != map.end()) return it;
    }
    return map.end();
}

// F26a: one level of the chain as RGBA8 bytes (ScaleToNative's texels).
std::vector<std::uint8_t> ToBytes(const std::vector<std::uint32_t> &texels) {
    std::vector<std::uint8_t> out(texels.size() * 4);
    for (std::size_t i = 0; i < texels.size(); ++i) {
        const std::uint32_t c = texels[i];
        out[i * 4] = static_cast<std::uint8_t>(c);
        out[i * 4 + 1] = static_cast<std::uint8_t>(c >> 8);
        out[i * 4 + 2] = static_cast<std::uint8_t>(c >> 16);
        out[i * 4 + 3] = static_cast<std::uint8_t>(c >> 24);
    }
    return out;
}

std::vector<std::uint32_t> ToTexels(const std::vector<std::uint8_t> &rgba) {
    std::vector<std::uint32_t> out(rgba.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint32_t>(rgba[i * 4]) |
                 (static_cast<std::uint32_t>(rgba[i * 4 + 1]) << 8) |
                 (static_cast<std::uint32_t>(rgba[i * 4 + 2]) << 16) |
                 (static_cast<std::uint32_t>(rgba[i * 4 + 3]) << 24);
    }
    return out;
}

bool ValidImage(const DecodedImage &img) {
    return img.width > 0 && img.height > 0 && img.width <= kMaxImageSide &&
           img.height <= kMaxImageSide &&
           img.rgba.size() ==
               static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4;
}

// F26a: what WatchFolders compares: every file under the folders, with its
// size and time (FNV-1a).
std::uint64_t FolderSignature(const std::vector<std::filesystem::path> &dirs) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](const void *data, std::size_t size) {
        const auto *p = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
    };
    for (const std::filesystem::path &dir : dirs) {
        std::error_code ec;
        std::vector<std::pair<std::string, std::pair<std::uintmax_t, std::int64_t>>> files;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const std::uintmax_t size = it->file_size(fec);
            const auto time = it->last_write_time(fec).time_since_epoch().count();
            files.push_back({it->path().generic_string(), {size, static_cast<std::int64_t>(time)}});
        }
        std::sort(files.begin(), files.end());
        for (const auto &[name, info] : files) {
            mix(name.data(), name.size());
            mix(&info.first, sizeof(info.first));
            mix(&info.second, sizeof(info.second));
        }
    }
    return h;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

} // namespace

void ScaleToNative(const std::uint8_t *rgba, int iw, int ih, int w, int h,
                   std::vector<std::uint32_t> *out) {
    out->assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);
    auto pixel = [&](int x, int y) {
        return rgba + (static_cast<std::size_t>(y) * static_cast<std::size_t>(iw) +
                       static_cast<std::size_t>(x)) *
                          4;
    };
    for (int y = 0; y < h; ++y) {
        // The image rows this texel covers: [y0, y1), at least one.
        int y0 = static_cast<int>(static_cast<std::int64_t>(y) * ih / h);
        int y1 = static_cast<int>(static_cast<std::int64_t>(y + 1) * ih / h);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; ++x) {
            int x0 = static_cast<int>(static_cast<std::int64_t>(x) * iw / w);
            int x1 = static_cast<int>(static_cast<std::int64_t>(x + 1) * iw / w);
            if (x1 <= x0) x1 = x0 + 1;
            std::uint64_t sum_a = 0;
            std::uint64_t sum_c[3] = {0, 0, 0};     // weighted by alpha
            std::uint64_t sum_plain[3] = {0, 0, 0}; // for a fully transparent block
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const std::uint8_t *p = pixel(sx, sy);
                    sum_a += p[3];
                    for (int c = 0; c < 3; ++c) {
                        sum_c[c] += static_cast<std::uint64_t>(p[c]) * p[3];
                        sum_plain[c] += p[c];
                    }
                }
            }
            const auto n =
                static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0);
            std::uint32_t color[4];
            for (int c = 0; c < 3; ++c) {
                color[c] = sum_a != 0 ? static_cast<std::uint32_t>((sum_c[c] + sum_a / 2) / sum_a)
                                      : static_cast<std::uint32_t>((sum_plain[c] + n / 2) / n);
            }
            color[3] = static_cast<std::uint32_t>((sum_a + n / 2) / n);
            (*out)[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                   static_cast<std::size_t>(x)] =
                color[0] | (color[1] << 8) | (color[2] << 16) | (color[3] << 24);
        }
    }
}

std::unique_ptr<ge::ReplacedTexture> BuildReplacement(DecodedImage level0,
                                                      std::vector<DecodedImage> levels, int w,
                                                      int h, bool generate,
                                                      std::vector<std::string> *notes) {
    auto tex = std::make_unique<ge::ReplacedTexture>();
    tex->width = w;
    tex->height = h;
    tex->image_width = level0.width;
    tex->image_height = level0.height;
    // The pack's levels: each half the one before (LoadLevelData's check).
    int lw = level0.width;
    int lh = level0.height;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        DecodedImage &l = levels[i];
        const int ew = std::max(1, lw / 2);
        const int eh = std::max(1, lh / 2);
        if (l.width != ew || l.height != eh) {
            if (notes != nullptr)
                notes->push_back("mip level " + std::to_string(i + 1) + " is " +
                                 std::to_string(l.width) + "x" + std::to_string(l.height) +
                                 ", expected " + std::to_string(ew) + "x" + std::to_string(eh) +
                                 ": it and the levels after it are not used");
            break;
        }
        tex->mips.push_back({l.width, l.height, std::move(l.rgba)});
        lw = ew;
        lh = eh;
    }
    // The level of the GE texture's size, if the chain has one.
    int native_level = level0.width == w && level0.height == h ? 0 : -1;
    for (std::size_t i = 0; native_level < 0 && i < tex->mips.size(); ++i) {
        if (tex->mips[i].width == w && tex->mips[i].height == h)
            native_level = static_cast<int>(i) + 1;
    }
    if (native_level > 0) {
        tex->native = ToTexels(tex->mips[static_cast<std::size_t>(native_level - 1)].rgba);
    } else {
        ScaleToNative(level0.rgba.data(), level0.width, level0.height, w, h, &tex->native);
    }
    if (tex->mips.empty() && generate) {
        // A 2 x 2 box down to the GE size; that level is `native` itself, so
        // the GPU and softgpu sample the same texels there.
        lw = level0.width;
        lh = level0.height;
        const std::vector<std::uint8_t> *prev = &level0.rgba;
        while (lw % 2 == 0 && lh % 2 == 0 && lw / 2 >= w && lh / 2 >= h) {
            const int nw = lw / 2;
            const int nh = lh / 2;
            ge::ReplacedTexture::Level next{nw, nh, {}};
            if (nw == w && nh == h) {
                next.rgba = ToBytes(tex->native);
            } else {
                std::vector<std::uint32_t> texels;
                ScaleToNative(prev->data(), lw, lh, nw, nh, &texels);
                next.rgba = ToBytes(texels);
            }
            tex->mips.push_back(std::move(next));
            prev = &tex->mips.back().rgba;
            lw = nw;
            lh = nh;
        }
    }
    // softgpu's levels after `native`: the pack's levels below the GE size.
    if (native_level >= 0) {
        for (std::size_t i = static_cast<std::size_t>(native_level); i < tex->mips.size(); ++i) {
            const ge::ReplacedTexture::Level &l = tex->mips[i];
            tex->native_mips.push_back({l.width, l.height, ToTexels(l.rgba)});
        }
    }
    tex->image = std::move(level0.rgba);
    tex->id = g_next_image_id.fetch_add(1, std::memory_order_relaxed);
    return tex;
}

// F26a: what a set and its loads share (the loads may outlive the set).
struct ReplacementSet::Shared {
    mutable std::mutex mutex;
    mutable std::condition_variable idle;
    int running = 0; // loads and preloads queued or running
    std::atomic<bool> cancelled{false};
    // The preload.
    bool preload_started = false;
    std::size_t budget = 0;
    std::unordered_map<std::string, DecodedImage> preloaded;
    std::unordered_set<std::string> requested; // a load took or decoded it
    PreloadStats stats;

    void Done() {
        std::lock_guard<std::mutex> lock(mutex);
        --running;
        idle.notify_all();
    }
};

struct ReplacementSet::Job {
    std::shared_ptr<Shared> shared;
    std::string file;
    std::vector<std::string> names;                 // level 0, then the pack's levels
    std::vector<std::filesystem::path> paths;       // their files
    std::vector<std::optional<DecodedImage>> given; // AddImage's, when there is one
    int w = 0;
    int h = 0;
    bool generate = true;
    std::atomic<bool> done{false};
    std::unique_ptr<ge::ReplacedTexture> result;
    std::string error;
    std::vector<std::string> notes;
};

ReplacementSet::ReplacementSet(std::filesystem::path dir, TexturesIni ini)
    : shared_(std::make_shared<Shared>()), dir_(std::move(dir)), ini_(std::move(ini)) {
    for (const TexturesIni::Entry &e : ini_.hashes) {
        aliases_[e.key] = e.file;
        if (!e.levels.empty() && !e.file.empty()) levels_[e.file] = e.levels;
    }
    for (const TexturesIni::Filter &f : ini_.filtering) filtering_[f.key] = f.filter;
}

ReplacementSet::~ReplacementSet() {
    shared_->cancelled.store(true, std::memory_order_relaxed);
}

std::filesystem::path ReplacementSet::PathOf(const std::string &file) const {
    // The names in textures.ini are UTF-8.
    return dir_ / std::filesystem::path(std::u8string(file.begin(), file.end()));
}

void ReplacementSet::SetLevels(const std::string &file, std::vector<std::string> levels) {
    if (levels.empty()) {
        levels_.erase(file);
    } else {
        levels_[file] = std::move(levels);
    }
}

const std::vector<std::string> &ReplacementSet::Levels(const std::string &file) const {
    static const std::vector<std::string> kNone;
    auto it = levels_.find(file);
    return it == levels_.end() ? kNone : it->second;
}

void ReplacementSet::AddFiltering(const ReplacementKey &key, ge::TextureFilter filter) {
    filtering_[key] = filter;
}

bool ReplacementSet::FindFiltering(const ReplacementKey &full, ge::TextureFilter *filter) const {
    if (filtering_.empty()) return false;
    auto it = LookupWildcard(filtering_, full, ini_.ignore_address);
    // A global wildcard (a key of zeros).
    if (it == filtering_.end()) it = filtering_.find(ReplacementKey{0, 0});
    if (it == filtering_.end()) return false;
    *filter = it->second;
    return true;
}

std::vector<std::string> ReplacementSet::TakeNotes() {
    return std::exchange(notes_, {});
}

ReplacementSet::PreloadStats ReplacementSet::preload_stats() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->stats;
}

bool ReplacementSet::WaitIdle(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(shared_->mutex);
    return shared_->idle.wait_for(lock, timeout, [this] { return shared_->running == 0; });
}

bool ReplacementSet::Decode(const std::filesystem::path &path, const std::string &file,
                            Shared &shared, DecodedImage *out, std::string *error) {
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.requested.insert(file);
        auto it = shared.preloaded.find(file);
        if (it != shared.preloaded.end()) {
            *out = std::move(it->second);
            shared.stats.bytes -= out->rgba.size();
            ++shared.stats.used;
            shared.preloaded.erase(it);
            return true;
        }
    }
    return LoadPng(path, &out->width, &out->height, &out->rgba, error);
}

void ReplacementSet::RunJob(Job &job) {
    DecodedImage level0;
    std::vector<DecodedImage> levels;
    for (std::size_t i = 0; i < job.names.size(); ++i) {
        if (job.shared->cancelled.load(std::memory_order_relaxed)) {
            job.error.clear();
            return;
        }
        DecodedImage img;
        std::string err;
        bool ok = true;
        if (job.given[i]) {
            img = std::move(*job.given[i]);
        } else {
            ok = Decode(job.paths[i], job.names[i], *job.shared, &img, &err);
        }
        if (ok && !ValidImage(img)) {
            ok = false;
            err = job.paths[i].string() + ": " + std::to_string(img.width) + "x" +
                  std::to_string(img.height) +
                  (img.width > 0 && img.height > 0 && img.width <= kMaxImageSide &&
                           img.height <= kMaxImageSide
                       ? std::string(": bad image data")
                       : " is outside 1x1 to " + std::to_string(kMaxImageSide) + "x" +
                             std::to_string(kMaxImageSide));
        }
        if (!ok) {
            if (i == 0) {
                job.error = err;
                return;
            }
            job.notes.push_back(err + ": mip level " + std::to_string(i) +
                                " and the levels after it are not used");
            break;
        }
        if (i == 0) {
            level0 = std::move(img);
        } else {
            levels.push_back(std::move(img));
        }
    }
    job.result = BuildReplacement(std::move(level0), std::move(levels), job.w, job.h, job.generate,
                                  &job.notes);
}

void ReplacementSet::StartPreload(const std::shared_ptr<DecodePool> &pool,
                                  std::size_t budget_bytes) {
    if (!pool || budget_bytes == 0) return;
    std::vector<std::string> files;
    {
        std::set<std::string> seen;
        for (const auto &[key, file] : aliases_) {
            (void)key;
            if (file.empty() || in_memory_.count(file) != 0) continue;
            if (seen.insert(file).second) files.push_back(file);
            for (const std::string &l : Levels(file))
                if (in_memory_.count(l) == 0 && seen.insert(l).second) files.push_back(l);
        }
        std::sort(files.begin(), files.end());
    }
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->preload_started) return;
        shared_->preload_started = true;
        shared_->budget = budget_bytes;
        shared_->stats.files = files.size();
        shared_->running += static_cast<int>(files.size());
    }
    for (const std::string &file : files) {
        pool->Submit(
            [shared = shared_, file, path = PathOf(file)] {
                bool skip = shared->cancelled.load(std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    if (shared->stats.bytes >= shared->budget) shared->stats.full = true;
                    skip = skip || shared->stats.full || shared->requested.count(file) != 0;
                }
                if (!skip) {
                    DecodedImage img;
                    std::string error;
                    if (LoadPng(path, &img.width, &img.height, &img.rgba, &error) &&
                        ValidImage(img)) {
                        std::lock_guard<std::mutex> lock(shared->mutex);
                        if (shared->requested.count(file) != 0) {
                            // A draw got there first.
                        } else if (shared->stats.bytes + img.rgba.size() > shared->budget) {
                            shared->stats.full = true;
                        } else {
                            shared->stats.bytes += img.rgba.size();
                            ++shared->stats.decoded;
                            shared->preloaded.emplace(file, std::move(img));
                        }
                    }
                }
                shared->Done();
            },
            false);
    }
}

std::unique_ptr<ReplacementSet> ReplacementSet::Load(const std::filesystem::path &dir,
                                                     std::string *error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        *error = dir.string() + ": not a folder";
        return nullptr;
    }
    const std::filesystem::path ini_path = dir / "textures.ini";
    std::ifstream in(ini_path, std::ios::binary);
    if (!in) {
        *error = dir.string() + ": no textures.ini";
        return nullptr;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TexturesIni ini;
    std::string ini_error;
    if (!ParseTexturesIni(text, &ini, &ini_error)) {
        *error = ini_path.string() + ": " + ini_error;
        return nullptr;
    }
    // Hash-named PNGs in the root first; [hashes] overrides them
    // (LoadIniValues fills the same map after ScanForHashNamedFiles).
    // F26a: with their mip levels (<key>_1.png, <key>_2.png...), joined as
    // ComputeAliasMap does: from level 0 while they follow each other.
    std::map<ReplacementKey, std::map<int, std::string>> by_key;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::filesystem::path name = entry.path().filename();
        const std::string file = name.string();
        if (file.empty() || file[0] == '.') continue;
        if (!EqualsNoCase(name.extension().string(), ".png")) continue;
        const std::string stem = name.stem().string();
        if (!IsHashFileName(stem)) continue;
        ReplacementKey key;
        int level = 0;
        if (!ParseHashName(stem, &key, &level)) continue;
        by_key[key][level] = file;
    }
    if (ec) {
        *error = dir.string() + ": " + ec.message();
        return nullptr;
    }
    std::vector<TexturesIni::Entry> scanned;
    for (auto &[key, levels] : by_key) {
        TexturesIni::Entry e;
        e.key = key;
        int next = 0;
        for (auto &[level, file] : levels) {
            if (level != next) {
                if (next > 0)
                    ini.warnings.push_back(file + ": mip level " + std::to_string(level) +
                                           " without level " + std::to_string(next) + ", ignored");
                break;
            }
            if (level == 0) {
                e.file = std::move(file);
            } else {
                e.levels.push_back(std::move(file));
            }
            ++next;
        }
        if (next > 0) scanned.push_back(std::move(e));
    }
    std::vector<TexturesIni::Entry> listed = std::move(ini.hashes);
    ini.hashes = std::move(scanned);
    ini.hashes.insert(ini.hashes.end(), std::make_move_iterator(listed.begin()),
                      std::make_move_iterator(listed.end()));
    auto set = std::make_unique<ReplacementSet>(dir, std::move(ini));
    set->from_folder_ = true;
    return set;
}

void ReplacementSet::AddAlias(const ReplacementKey &key, std::string file) {
    aliases_[key] = std::move(file);
}

void ReplacementSet::AddImage(const std::string &file, int iw, int ih,
                              std::vector<std::uint8_t> rgba) {
    in_memory_[file] = Decoded{iw, ih, std::move(rgba)};
    failed_.erase(file);
    for (auto it = images_.begin(); it != images_.end();) {
        it = std::get<0>(it->first) == file ? images_.erase(it) : std::next(it);
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        it = std::get<0>(it->first) == file ? jobs_.erase(it) : std::next(it);
    }
}

const std::string *ReplacementSet::Lookup(const ReplacementKey &full) const {
    bool ignored = false;
    return Lookup(full, &ignored);
}

const std::string *ReplacementSet::Lookup(const ReplacementKey &full, bool *ignored) const {
    *ignored = false;
    auto it = LookupWildcard(aliases_, full, ini_.ignore_address);
    if (it == aliases_.end()) return nullptr;
    if (it->second.empty()) {
        *ignored = true; // listed as "do not replace"
        return nullptr;
    }
    return &it->second;
}

const ge::ReplacedTexture *ReplacementSet::Get(const std::string &file, int w, int h,
                                               std::string *error, bool *pending) {
    if (pending != nullptr) *pending = false;
    const auto key = std::make_tuple(file, w, h);
    if (auto it = images_.find(key); it != images_.end()) return it->second.get();
    if (failed_.count(file) != 0) return nullptr;
    std::shared_ptr<Job> job;
    if (auto it = jobs_.find(key); it != jobs_.end()) {
        if (!it->second->done.load(std::memory_order_acquire)) {
            if (pending != nullptr) *pending = true;
            return nullptr;
        }
        job = std::move(it->second);
        jobs_.erase(it);
    } else {
        job = std::make_shared<Job>();
        job->shared = shared_;
        job->file = file;
        job->w = w;
        job->h = h;
        job->generate = generate_mipmaps_;
        job->names.push_back(file);
        for (const std::string &l : Levels(file)) job->names.push_back(l);
        bool all_given = true;
        for (const std::string &name : job->names) {
            job->paths.push_back(PathOf(name));
            if (auto mem = in_memory_.find(name); mem != in_memory_.end()) {
                job->given.emplace_back(
                    DecodedImage{mem->second.width, mem->second.height, mem->second.rgba});
            } else {
                job->given.emplace_back(std::nullopt);
                all_given = false;
            }
        }
        if (pool_ && !all_given) {
            // F26a: on a pool thread; the game's texture meanwhile.
            {
                std::lock_guard<std::mutex> lock(shared_->mutex);
                ++shared_->running;
            }
            pool_->Submit(
                [job] {
                    RunJob(*job);
                    job->done.store(true, std::memory_order_release);
                    job->shared->Done();
                },
                true);
            jobs_.emplace(key, job);
            if (pending != nullptr) *pending = true;
            return nullptr;
        }
        RunJob(*job);
    }
    for (std::string &note : job->notes) notes_.push_back(std::move(note));
    if (!job->result) {
        failed_.insert(file);
        *error = job->error;
        return nullptr;
    }
    const ge::ReplacedTexture *out = job->result.get();
    images_.emplace(key, std::move(job->result));
    return out;
}

int TextureReplacer::AddSet(std::unique_ptr<ReplacementSet> set) {
    ConfigureSet(*set);
    sets_.push_back(std::move(set));
    return static_cast<int>(sets_.size()) - 1;
}

TextureReplacer::~TextureReplacer() {
    StopWatching();
    std::string error;
    if (!FlushDump(&error)) Log("textures: dump: " + error);
}

bool TextureReplacer::FlushDump(std::string *error) {
    if (dump_dir_.empty() || !index_.dirty()) return true;
    index_saved_flip_ = flips_;
    return index_.Save(dump_dir_, error);
}

void TextureReplacer::SetStageSource(std::function<std::string()> source) {
    stage_source_ = std::move(source);
    RefreshStage();
}

void TextureReplacer::RefreshStage() {
    if (!stage_source_) return;
    std::string name = stage_source_();
    if (!name.empty() && !DumpIndex::ValidStageName(name)) name.clear();
    if (name != stage_) stage_ = std::move(name);
}

void TextureReplacer::EnsurePool(int threads) {
    if (!pool_) pool_ = std::make_shared<DecodePool>(threads);
}

void TextureReplacer::ConfigureSet(ReplacementSet &set) {
    set.SetGenerateMipmaps(generate_mipmaps_);
    set.SetPool(async_ ? pool_ : nullptr);
    if (preload_budget_ != 0) set.StartPreload(pool_, preload_budget_);
}

void TextureReplacer::SetAsync(bool on, int threads) {
    async_ = on;
    if (on) EnsurePool(threads);
    for (auto &set : sets_) set->SetPool(on ? pool_ : nullptr);
}

void TextureReplacer::StartPreload(std::size_t budget_bytes, int threads) {
    preload_budget_ = budget_bytes;
    if (budget_bytes == 0) return;
    EnsurePool(threads);
    for (auto &set : sets_) set->StartPreload(pool_, budget_bytes);
}

void TextureReplacer::ReloadSets() {
    for (auto &set : sets_) {
        if (!set->from_folder()) continue;
        const std::filesystem::path dir = set->dir();
        std::string error;
        auto fresh = ReplacementSet::Load(dir, &error);
        if (!fresh) {
            ++stats_.reload_errors;
            Log("textures: reload: " + error + " (the pack loaded before stays)");
            continue;
        }
        for (const std::string &w : fresh->ini().warnings)
            Log("textures: " + dir.string() + ": " + w);
        ConfigureSet(*fresh);
        Log("textures: reloaded " + dir.string() + ": " + std::to_string(fresh->size()) +
            " replacements");
        set = std::move(fresh);
        ++stats_.reloads;
    }
}

void TextureReplacer::StopWatching() {
    if (!watcher_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        watch_stop_ = true;
    }
    watch_cv_.notify_all();
    watcher_.join();
    watch_stop_ = false;
}

void TextureReplacer::WatchFolders(std::chrono::milliseconds period) {
    StopWatching();
    if (period.count() <= 0) return;
    std::vector<std::filesystem::path> dirs;
    for (const auto &set : sets_)
        if (set->from_folder()) dirs.push_back(set->dir());
    if (dirs.empty()) return;
    // The state to compare with, from before this returns.
    const std::uint64_t first = FolderSignature(dirs);
    watcher_ = std::thread([this, dirs = std::move(dirs), period, first] {
        std::uint64_t seen = first;
        std::unique_lock<std::mutex> lock(watch_mutex_);
        while (!watch_cv_.wait_for(lock, period, [this] { return watch_stop_; })) {
            lock.unlock();
            const std::uint64_t now = FolderSignature(dirs);
            lock.lock();
            if (now != seen) {
                seen = now;
                RequestReload();
            }
        }
    });
}

bool TextureReplacer::SetDumpDir(const std::filesystem::path &dir, std::string *error) {
    if (!FlushDump(error)) return false;
    dump_dir_.clear();
    dumped_.clear();
    index_ = DumpIndex{};
    if (dir.empty()) return true;
    if (!HavePng()) {
        *error = "--dump-textures: this build has no PNG support (it needs SDL3)";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        *error = dir.string() + ": " + ec.message();
        return false;
    }
    const std::filesystem::path ini_path = dir / "textures.ini";
    std::string text;
    if (std::ifstream in(ini_path, std::ios::binary); in) {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        text = DefaultTexturesIni();
        std::ofstream out(ini_path, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            *error = "cannot write " + ini_path.string();
            return false;
        }
    }
    std::string ini_error;
    if (!ParseTexturesIni(text, &dump_ini_, &ini_error)) {
        *error = ini_path.string() + ": " + ini_error;
        return false;
    }
    // Keys already in the folder are not written again.
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string stem = entry.path().stem().string();
        ReplacementKey key;
        int level = 0;
        if (IsHashFileName(stem) && ParseHashName(stem, &key, &level) && level == 0)
            dumped_.insert(key);
    }
    // F26b: the rows of earlier sessions, to add to.
    if (!index_.Load(dir, error)) {
        index_ = DumpIndex{};
        return false;
    }
    dump_dir_ = dir;
    index_saved_flip_ = flips_;
    RefreshStage();
    return true;
}

void TextureReplacer::Log(const std::string &line) const {
    if (log_) log_(line);
}

void TextureReplacer::BeginRun() {
    memo_.clear(); // RAM only: VRAM hashes live until the next flip (F25c)
    // F26a: a reload asked for since the last run.
    if (const std::uint32_t req = reload_requests_.load(std::memory_order_relaxed);
        req != reloads_applied_) {
        reloads_applied_ = req;
        ReloadSets();
    }
    // F26b: the stage loaded now, for dump.csv.
    if (!dump_dir_.empty()) RefreshStage();
    const int want = requested_.load(std::memory_order_relaxed);
    const int next = want >= 0 && want < set_count() ? want : -1;
    if (next != active_) {
        active_ = next;
        ++stats_.switches;
    }
}

void TextureReplacer::MemoryWritten(std::uint32_t addr, std::uint32_t size) {
    memo_.clear();
    if (ge::MemoryView::IsVramAddress(addr) && size != 0) {
        const std::uint32_t begin = VramOffset(addr);
        ForgetVramHashes(begin, begin + size);
    }
}

void TextureReplacer::ForgetVramHashes(std::uint32_t begin, std::uint32_t end) {
    for (auto it = vram_memo_.begin(); it != vram_memo_.end();) {
        it =
            it->second.begin < end && it->second.end > begin ? vram_memo_.erase(it) : std::next(it);
    }
}

void TextureReplacer::AddTarget(std::uint32_t base, std::uint32_t stride, std::uint32_t x0,
                                std::uint32_t x1, std::uint32_t y0, std::uint32_t y1) {
    if (x1 <= x0 || y1 <= y0) return;
    for (Target &t : targets_) {
        if (t.base != base || t.stride != stride) continue;
        // The same buffer: grow it to cover this use too.
        const Target grown{base,
                           stride,
                           std::min(t.x0, x0),
                           std::max(t.x1, x1),
                           std::min(t.y0, y0),
                           std::max(t.y1, y1),
                           flips_};
        if (grown.x0 != t.x0 || grown.x1 != t.x1 || grown.y0 != t.y0 || grown.y1 != t.y1)
            ++targets_gen_;
        t = grown;
        return;
    }
    if (targets_.size() >= kMaxTargets) {
        auto oldest = std::min_element(
            targets_.begin(), targets_.end(),
            [](const Target &a, const Target &b) { return a.last_flip < b.last_flip; });
        targets_.erase(oldest);
    }
    targets_.push_back(Target{base, stride, x0, x1, y0, y1, flips_});
    ++targets_gen_;
    ++stats_.targets;
}

void TextureReplacer::AddTargetRect(const ge::MemoryRect &rect) {
    if (!ge::MemoryView::IsVramAddress(rect.addr) || rect.rows == 0 || rect.row_bytes == 0) return;
    AddTarget(VramOffset(rect.addr), rect.stride, 0, rect.row_bytes, 0, rect.rows);
}

std::uint64_t TextureReplacer::TargetsHit(const ge::MemoryRect &rect) const {
    std::uint64_t hit = 0;
    if (rect.rows == 0 || rect.row_bytes == 0) return 0;
    const std::int64_t first = rect.addr;
    const std::int64_t last = first + rect.span(); // exclusive
    // Row r of `rect` is [a, b) = [first + r * stride, + row_bytes); when its
    // rows touch each other it is one run of bytes.
    const bool contiguous = rect.rows == 1 || rect.row_bytes >= rect.stride;
    const std::uint32_t rows = contiguous ? 1u : rect.rows;
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        const Target &t = targets_[i];
        const std::int64_t base = t.base;
        const std::int64_t s = t.stride;
        const std::int64_t tx0 = t.x0;
        const std::int64_t tx1 = t.x1;
        const std::int64_t ty0 = t.y0;
        const std::int64_t ty1 = t.y1;
        // The target's bytes, first to last.
        const std::int64_t t_first = base + ty0 * s + tx0;
        const std::int64_t t_last = base + (ty1 - 1) * s + tx1;
        if (t_first >= last || t_last <= first) continue;
        for (std::uint32_t r = 0; r < rows; ++r) {
            const std::int64_t a = first + static_cast<std::int64_t>(r) * rect.stride;
            const std::int64_t b = contiguous ? last : a + rect.row_bytes;
            // Row y of the target, [base + y*s + x0, base + y*s + x1), meets
            // [a, b) when a - base - x1 < y*s < b - base - x0.
            const std::int64_t lo = a - base;
            const std::int64_t hi = b - base;
            bool meets = false;
            if (s == 0) {
                meets = tx0 < hi && tx1 > lo;
            } else {
                const std::int64_t ymin = std::max<std::int64_t>(FloorDiv(lo - tx1, s) + 1, ty0);
                const std::int64_t ymax = std::min<std::int64_t>(CeilDiv(hi - tx0, s) - 1, ty1 - 1);
                meets = ymin <= ymax;
            }
            if (meets) {
                hit |= std::uint64_t{1} << i;
                break;
            }
        }
    }
    return hit;
}

ge::MemoryRect TextureReplacer::TextureRect(const ge::SamplerState &st) {
    const auto fmt = static_cast<std::size_t>(st.texfmt) & 15u;
    std::uint32_t bits = ge::kTextureBitsPerPixel[fmt];
    if (bits == 0) bits = 16; // as HashTexels: an invalid format reads as 5650
    const auto w = static_cast<std::uint32_t>(std::max(st.sizes[0].w, 0));
    const auto h = static_cast<std::uint32_t>(std::max(st.sizes[0].h, 0));
    const auto bufw = static_cast<std::uint32_t>(std::max(st.texbufw[0], 0));
    const std::uint32_t pitch = bufw * bits / 8;
    ge::MemoryRect rect;
    rect.addr = NormalizeAddress(st.texaddr[0]);
    if (fmt >= 8 || st.swizzle) {
        // DXT blocks and swizzled 16-byte x 8-row tiles cover whole rows.
        const std::uint32_t rows = st.swizzle ? (h + 7u) / 8u * 8u : h;
        rect.stride = 0;
        rect.row_bytes = std::max(pitch, (w * bits + 7u) / 8u) * rows;
        rect.rows = 1;
    } else {
        rect.stride = pitch;
        rect.row_bytes = (w * bits + 7u) / 8u;
        rect.rows = h;
    }
    return rect;
}

bool TextureReplacer::IsFramebuffer(const ge::SamplerState &st) {
    const ge::MemoryRect rect = TextureRect(st);
    if (targets_.empty() || !ge::MemoryView::IsVramAddress(rect.addr)) return false;
    if (target_memo_gen_ != targets_gen_) {
        target_memo_.clear();
        target_memo_gen_ = targets_gen_;
    }
    const MemoKey key{rect.addr,
                      static_cast<std::uint32_t>(st.sizes[0].w) |
                          (static_cast<std::uint32_t>(st.sizes[0].h) << 16),
                      static_cast<std::uint32_t>(st.texbufw[0]),
                      static_cast<std::uint32_t>(st.texfmt) | (st.swizzle ? 0x100u : 0u)};
    std::uint64_t hit = 0;
    if (auto it = target_memo_.find(key); it != target_memo_.end()) {
        hit = it->second;
    } else {
        ge::MemoryRect offsets = rect;
        offsets.addr = VramOffset(rect.addr);
        hit = TargetsHit(offsets);
        target_memo_.emplace(key, hit);
    }
    if (hit == 0) return false;
    // Sampling a framebuffer keeps it alive (PPSSPP's last_frame_used).
    for (std::size_t i = 0; i < targets_.size(); ++i)
        if ((hit >> i) & 1u) targets_[i].last_flip = flips_;
    return true;
}

std::vector<std::string> TextureReplacer::DescribeTargets() const {
    std::vector<std::string> out;
    for (const Target &t : targets_) {
        char line[128];
        std::snprintf(line, sizeof(line), "0x%06x stride %u: rows %u-%u, bytes %u-%u", t.base,
                      t.stride, t.y0, t.y1 - 1, t.x0, t.x1 - 1);
        out.emplace_back(line);
    }
    return out;
}

bool TextureReplacer::IsVideo(const ge::MemoryRect &rect) const {
    const std::uint32_t begin = rect.addr;
    const std::uint32_t end = begin + rect.span();
    for (const Video &v : videos_)
        if (v.begin < end && v.end > begin) return true;
    return false;
}

void TextureReplacer::BeginDraw(const ge::GeState &gs) {
    const bool depth_written = gs.IsModeClear()
                                   ? gs.IsClearModeDepthMask()
                                   : gs.IsDepthTestEnabled() && gs.IsDepthWriteEnabled();
    const auto x1 = static_cast<std::uint32_t>(gs.ScissorX1());
    const auto y1 = static_cast<std::uint32_t>(gs.ScissorY1());
    const auto x2 = static_cast<std::uint32_t>(gs.ScissorX2());
    const auto y2 = static_cast<std::uint32_t>(gs.ScissorY2());
    DrawRegs regs;
    regs.valid = true;
    regs.regs[0] = gs.FrameBufRawAddress();
    regs.regs[1] = static_cast<std::uint32_t>(gs.FrameBufStride());
    regs.regs[2] = static_cast<std::uint32_t>(gs.FrameBufFormat());
    regs.regs[3] = gs.DepthBufRawAddress();
    regs.regs[4] = static_cast<std::uint32_t>(gs.DepthBufStride());
    regs.regs[5] = x1 | (y1 << 16);
    regs.regs[6] = x2 | (y2 << 16);
    regs.regs[7] = depth_written ? 1u : 0u;
    // The same buffers as the last draw, in the same flip, with no target
    // added, grown or forgotten in between: already marked.
    if (regs == last_draw_ && last_draw_gen_ == targets_gen_ && last_draw_flip_ == flips_) return;
    if (x2 >= x1 && y2 >= y1) {
        const std::uint32_t bpp = gs.FrameBufFormat() == ge::BufferFormat::k8888 ? 4u : 2u;
        AddTarget(VramOffset(gs.FrameBufRawAddress()),
                  static_cast<std::uint32_t>(gs.FrameBufStride()) * bpp, x1 * bpp, (x2 + 1) * bpp,
                  y1, y2 + 1);
        if (depth_written)
            AddTarget(VramOffset(gs.DepthBufRawAddress()),
                      static_cast<std::uint32_t>(gs.DepthBufStride()) * 2u, x1 * 2u, (x2 + 1) * 2u,
                      y1, y2 + 1);
    }
    last_draw_ = regs;
    last_draw_gen_ = targets_gen_;
    last_draw_flip_ = flips_;
}

void TextureReplacer::BlockTransfer(const ge::MemoryRect &src, const ge::MemoryRect &dst) {
    MemoryWritten(dst.addr, dst.span());
    // A copy of a framebuffer into VRAM is a framebuffer too.
    if (targets_.empty() || !ge::MemoryView::IsVramAddress(src.addr) ||
        !ge::MemoryView::IsVramAddress(dst.addr))
        return;
    ge::MemoryRect offsets = src;
    offsets.addr = VramOffset(src.addr);
    if (TargetsHit(offsets) != 0) AddTargetRect(dst);
}

void TextureReplacer::VideoWritten(std::uint32_t addr, std::uint32_t size) {
    if (size == 0) return;
    const std::uint32_t begin = NormalizeAddress(addr);
    const std::uint32_t end = begin + size;
    bool known = false;
    for (Video &v : videos_) {
        if (v.begin == begin && v.end == end) {
            v.flip = flips_;
            known = true;
        }
    }
    if (!known) videos_.push_back(Video{begin, end, flips_});
    MemoryWritten(addr, size);
}

void TextureReplacer::Flip(const ge::MemoryRect *shown) {
    ++flips_;
    ++stats_.flips;
    // The CPU may have rewritten any texture in VRAM during the frame.
    vram_memo_.clear();
    const std::size_t before = targets_.size();
    std::erase_if(targets_, [&](const Target &t) { return t.last_flip + kTargetAge < flips_; });
    if (targets_.size() != before) {
        stats_.targets_aged += before - targets_.size();
        ++targets_gen_;
    }
    std::erase_if(videos_, [&](const Video &v) { return v.flip + kVideoAge < flips_; });
    if (shown != nullptr) AddTargetRect(*shown);
    // F26b: dump.csv, every kDumpSaveFlips flips while it changes.
    if (!dump_dir_.empty() && index_.dirty() && flips_ - index_saved_flip_ >= kDumpSaveFlips) {
        std::string error;
        if (!FlushDump(&error)) Log("textures: dump: " + error);
    }
}

std::uint32_t TextureReplacer::HashTexels(const ge::MemoryView &mem, const ge::SamplerState &st,
                                          HashType type) {
    const bool vram = ge::MemoryView::IsVramAddress(st.texaddr[0]);
    const MemoKey key{
        vram ? NormalizeAddress(st.texaddr[0]) : ge::MemoryView::Canonical(st.texaddr[0]),
        static_cast<std::uint32_t>(st.sizes[0].w) |
            (static_cast<std::uint32_t>(st.sizes[0].h) << 16),
        static_cast<std::uint32_t>(st.texbufw[0]),
        static_cast<std::uint32_t>(st.texfmt) | (static_cast<std::uint32_t>(type) << 8)};
    if (vram) {
        if (auto it = vram_memo_.find(key); it != vram_memo_.end()) return it->second.hash;
    } else if (auto it = memo_.find(key); it != memo_.end()) {
        return it->second;
    }
    const ge::TexFormat format =
        static_cast<int>(st.texfmt) >= 11 ? ge::TexFormat::k5650 : st.texfmt;
    const std::uint32_t hash =
        TexelHash(mem, st.texaddr[0], st.texbufw[0], st.sizes[0].w, st.sizes[0].h, format, type);
    ++stats_.hashes;
    stats_.hashed_bytes += static_cast<std::uint64_t>(st.texbufw[0]) *
                           static_cast<std::uint64_t>(st.sizes[0].h) *
                           ge::kTextureBitsPerPixel[static_cast<std::size_t>(format)] / 8;
    if (vram) {
        // The bytes it covers, for the writes that make it stale.
        const ge::MemoryRect rect = TextureRect(st);
        const std::uint32_t begin = VramOffset(rect.addr);
        vram_memo_.emplace(key, VramMemo{hash, begin, begin + rect.span()});
    } else {
        memo_.emplace(key, hash);
    }
    return hash;
}

const ge::ReplacedTexture *TextureReplacer::Find(const ge::GeState &gs, const ge::MemoryView &mem,
                                                 const ge::SamplerState &st,
                                                 const ge::ClutLoadInfo &clut) {
    last_filter_ = ge::TextureFilter::kAuto;
    const bool dumping = !dump_dir_.empty();
    if (active_ < 0 && !dumping) return nullptr;
    const int w = st.sizes[0].w;
    const int h = st.sizes[0].h;
    if (w <= 0 || h <= 0 || st.has_invalid_ptr) return nullptr;
    ++stats_.lookups;
    const bool vram = ge::MemoryView::IsVramAddress(st.texaddr[0]);
    if (vram) {
        ++stats_.vram;
        // F25c: never a framebuffer (what the GE drew or the display shows).
        if (IsFramebuffer(st)) {
            ++stats_.framebuffer;
            return nullptr;
        }
    }
    // Nor a video frame (sceMpeg's), in VRAM or RAM.
    if (!videos_.empty() && IsVideo(TextureRect(st))) {
        ++stats_.video;
        return nullptr;
    }
    const std::uint64_t cachekey = CacheKeyFor(gs, st, clut);
    if (active_ >= 0) {
        ReplacementSet &set = *sets_[static_cast<std::size_t>(active_)];
        const ReplacementKey key{cachekey, HashTexels(mem, st, set.ini().hash)};
        bool ignored = false;
        const std::string *file = set.Lookup(key, &ignored);
        // F26a: [filtering], for a texture replaced or not (not for one
        // listed as "do not replace", as FindReplacement returns first).
        if (!ignored && set.has_filtering() && set.FindFiltering(key, &last_filter_) &&
            last_filter_ != ge::TextureFilter::kAuto)
            ++stats_.filtered;
        if (file != nullptr) {
            std::string error;
            bool pending = false;
            const ge::ReplacedTexture *tex = set.Get(*file, w, h, &error, &pending);
            for (const std::string &note : set.TakeNotes()) Log("textures: " + note);
            if (tex != nullptr) {
                ++stats_.replaced;
                if (vram) ++stats_.vram_replaced;
                if (!tex->mips.empty()) ++stats_.mipmapped;
                stats_.distinct.insert(tex->id);
                return tex;
            }
            if (pending) {
                // F26a: the game's texture until the PNG is decoded (and not
                // dumped: it has a replacement).
                ++stats_.pending;
                return nullptr;
            }
            if (!error.empty()) {
                ++stats_.load_errors;
                Log("textures: " + error);
            }
        }
    }
    if (dumping) {
        ReplacementKey key{cachekey, HashTexels(mem, st, dump_ini_.hash)};
        if (dump_ini_.ignore_address) key = WithoutAddress(key);
        NoteDump(key, mem, st, vram);
    }
    return nullptr;
}

void TextureReplacer::NoteDump(const ReplacementKey &key, const ge::MemoryView &mem,
                               const ge::SamplerState &st, bool vram) {
    index_.Note(key, st.sizes[0].w, st.sizes[0].h, st.texfmt,
                vram ? DumpIndex::kVram : DumpIndex::kRam, stage_, flips_);
    if (dumped_.insert(key).second) Dump(key, mem, st);
}

void TextureReplacer::Dump(const ReplacementKey &key, const ge::MemoryView &mem,
                           const ge::SamplerState &st) {
    const int w = st.sizes[0].w;
    const int h = st.sizes[0].h;
    ge::SamplerState plain = st;
    plain.replaced = nullptr;
    const ge::Sampler sampler(mem, plain);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int v = 0; v < h; ++v) {
        for (int u = 0; u < w; ++u) {
            const std::uint32_t c = sampler.Fetch(u, v, 0);
            std::uint8_t *p = &rgba[(static_cast<std::size_t>(v) * static_cast<std::size_t>(w) +
                                     static_cast<std::size_t>(u)) *
                                    4];
            p[0] = static_cast<std::uint8_t>(c);
            p[1] = static_cast<std::uint8_t>(c >> 8);
            p[2] = static_cast<std::uint8_t>(c >> 16);
            p[3] = static_cast<std::uint8_t>(c >> 24);
        }
    }
    const std::filesystem::path path = dump_dir_ / (HashName(key) + ".png");
    std::string error;
    if (SavePng(path, w, h, rgba, &error)) {
        ++stats_.dumped;
    } else {
        ++stats_.dump_errors;
        Log("textures: dump: " + error);
    }
}

} // namespace mpo::texreplace
