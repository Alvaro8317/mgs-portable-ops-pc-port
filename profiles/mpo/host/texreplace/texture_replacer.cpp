// SPDX-License-Identifier: GPL-3.0-or-later
// ReplacementSet::Load and Lookup: ported from PPSSPP
// GPU/Common/TextureReplacer.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "texture_replacer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

#include "png_io.hpp"
#include "sampler.hpp"

namespace mpo::texreplace {
namespace {

std::atomic<std::uint64_t> g_next_image_id{1};

bool IsHashFileName(const std::string &stem) {
    // ScanForHashNamedFiles: 24 characters, or 26-27 with "_<level>".
    return stem.size() == 24 || (stem.size() >= 26 && stem.size() <= 27 && stem[24] == '_');
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

ReplacementSet::ReplacementSet(std::filesystem::path dir, TexturesIni ini)
    : dir_(std::move(dir)), ini_(std::move(ini)) {
    for (const TexturesIni::Entry &e : ini_.hashes) aliases_[e.key] = e.file;
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
    std::vector<TexturesIni::Entry> scanned;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::filesystem::path name = entry.path().filename();
        const std::string file = name.string();
        if (file.empty() || file[0] == '.') continue;
        if (!EqualsNoCase(name.extension().string(), ".png")) continue;
        const std::string stem = name.stem().string();
        if (!IsHashFileName(stem)) continue;
        TexturesIni::Entry e;
        int level = 0;
        if (!ParseHashName(stem, &e.key, &level) || level != 0) continue;
        e.file = file;
        scanned.push_back(std::move(e));
    }
    if (ec) {
        *error = dir.string() + ": " + ec.message();
        return nullptr;
    }
    std::sort(
        scanned.begin(), scanned.end(),
        [](const TexturesIni::Entry &a, const TexturesIni::Entry &b) { return a.key < b.key; });
    std::vector<TexturesIni::Entry> listed = std::move(ini.hashes);
    ini.hashes = std::move(scanned);
    ini.hashes.insert(ini.hashes.end(), std::make_move_iterator(listed.begin()),
                      std::make_move_iterator(listed.end()));
    return std::make_unique<ReplacementSet>(dir, std::move(ini));
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
}

const std::string *ReplacementSet::Lookup(const ReplacementKey &full) const {
    const bool ignore = ini_.ignore_address;
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
        auto it = aliases_.find(order[i]);
        if (it == aliases_.end()) continue;
        if (it->second.empty()) return nullptr; // listed as "do not replace"
        return &it->second;
    }
    return nullptr;
}

const ge::ReplacedTexture *ReplacementSet::Get(const std::string &file, int w, int h,
                                               std::string *error) {
    const auto key = std::make_tuple(file, w, h);
    if (auto it = images_.find(key); it != images_.end()) return it->second.get();
    if (failed_.count(file) != 0) return nullptr;
    // The names in textures.ini are UTF-8.
    const std::filesystem::path path =
        dir_ / std::filesystem::path(std::u8string(file.begin(), file.end()));
    int iw = 0;
    int ih = 0;
    std::vector<std::uint8_t> rgba;
    if (auto mem = in_memory_.find(file); mem != in_memory_.end()) {
        iw = mem->second.width;
        ih = mem->second.height;
        rgba = mem->second.rgba;
    } else {
        std::string err;
        if (!LoadPng(path, &iw, &ih, &rgba, &err)) {
            failed_.insert(file);
            *error = err;
            return nullptr;
        }
    }
    if (rgba.size() !=
        static_cast<std::size_t>(std::max(iw, 0)) * static_cast<std::size_t>(std::max(ih, 0)) * 4) {
        failed_.insert(file);
        *error = path.string() + ": bad image data";
        return nullptr;
    }
    if (iw <= 0 || ih <= 0 || iw > kMaxImageSide || ih > kMaxImageSide) {
        failed_.insert(file);
        *error = path.string() + ": " + std::to_string(iw) + "x" + std::to_string(ih) +
                 " is outside 1x1 to " + std::to_string(kMaxImageSide) + "x" +
                 std::to_string(kMaxImageSide);
        return nullptr;
    }
    auto tex = std::make_unique<ge::ReplacedTexture>();
    tex->width = w;
    tex->height = h;
    ScaleToNative(rgba.data(), iw, ih, w, h, &tex->native);
    tex->image_width = iw;
    tex->image_height = ih;
    tex->image = std::move(rgba);
    tex->id = g_next_image_id.fetch_add(1, std::memory_order_relaxed);
    const ge::ReplacedTexture *out = tex.get();
    images_.emplace(key, std::move(tex));
    return out;
}

int TextureReplacer::AddSet(std::unique_ptr<ReplacementSet> set) {
    sets_.push_back(std::move(set));
    return static_cast<int>(sets_.size()) - 1;
}

bool TextureReplacer::SetDumpDir(const std::filesystem::path &dir, std::string *error) {
    dump_dir_.clear();
    dumped_.clear();
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
    dump_dir_ = dir;
    return true;
}

void TextureReplacer::Log(const std::string &line) const {
    if (log_) log_(line);
}

void TextureReplacer::BeginRun() {
    memo_.clear();
    const int want = requested_.load(std::memory_order_relaxed);
    const int next = want >= 0 && want < set_count() ? want : -1;
    if (next != active_) {
        active_ = next;
        ++stats_.switches;
    }
}

void TextureReplacer::MemoryWritten(std::uint32_t addr, std::uint32_t size) {
    (void)addr;
    (void)size;
    memo_.clear();
}

std::uint32_t TextureReplacer::HashTexels(const ge::MemoryView &mem, const ge::SamplerState &st,
                                          HashType type) {
    const MemoKey key{ge::MemoryView::Canonical(st.texaddr[0]),
                      static_cast<std::uint32_t>(st.sizes[0].w) |
                          (static_cast<std::uint32_t>(st.sizes[0].h) << 16),
                      static_cast<std::uint32_t>(st.texbufw[0]),
                      static_cast<std::uint32_t>(st.texfmt) |
                          (static_cast<std::uint32_t>(type) << 8)};
    if (auto it = memo_.find(key); it != memo_.end()) return it->second;
    const ge::TexFormat format =
        static_cast<int>(st.texfmt) >= 11 ? ge::TexFormat::k5650 : st.texfmt;
    const std::uint32_t hash =
        TexelHash(mem, st.texaddr[0], st.texbufw[0], st.sizes[0].w, st.sizes[0].h, format, type);
    ++stats_.hashes;
    stats_.hashed_bytes += static_cast<std::uint64_t>(st.texbufw[0]) *
                           static_cast<std::uint64_t>(st.sizes[0].h) *
                           ge::kTextureBitsPerPixel[static_cast<std::size_t>(format)] / 8;
    memo_.emplace(key, hash);
    return hash;
}

const ge::ReplacedTexture *TextureReplacer::Find(const ge::GeState &gs, const ge::MemoryView &mem,
                                                 const ge::SamplerState &st,
                                                 const ge::ClutLoadInfo &clut) {
    const bool dumping = !dump_dir_.empty();
    if (active_ < 0 && !dumping) return nullptr;
    const int w = st.sizes[0].w;
    const int h = st.sizes[0].h;
    if (w <= 0 || h <= 0 || st.has_invalid_ptr) return nullptr;
    ++stats_.lookups;
    if (ge::MemoryView::IsVramAddress(st.texaddr[0])) {
        ++stats_.vram;
        return nullptr;
    }
    const std::uint64_t cachekey = CacheKeyFor(gs, st, clut);
    if (active_ >= 0) {
        ReplacementSet &set = *sets_[static_cast<std::size_t>(active_)];
        const ReplacementKey key{cachekey, HashTexels(mem, st, set.ini().hash)};
        if (const std::string *file = set.Lookup(key)) {
            std::string error;
            if (const ge::ReplacedTexture *tex = set.Get(*file, w, h, &error)) {
                ++stats_.replaced;
                stats_.distinct.insert(tex->id);
                return tex;
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
        if (dumped_.insert(key).second) Dump(key, mem, st);
    }
    return nullptr;
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
