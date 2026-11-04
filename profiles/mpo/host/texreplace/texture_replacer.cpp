// SPDX-License-Identifier: GPL-3.0-or-later
// ReplacementSet::Load and Lookup: ported from PPSSPP
// GPU/Common/TextureReplacer.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "texture_replacer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

TextureReplacer::~TextureReplacer() {
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
        if (const std::string *file = set.Lookup(key)) {
            std::string error;
            if (const ge::ReplacedTexture *tex = set.Get(*file, w, h, &error)) {
                ++stats_.replaced;
                if (vram) ++stats_.vram_replaced;
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
