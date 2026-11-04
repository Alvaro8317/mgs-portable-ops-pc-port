// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_texreplace (F24c), without the game and without a GPU:
// - the key: PPSSPP's formula (checked against xxHash directly and a fixed
//   name), stable, address-independent with ignoreAddress, sensitive to the
//   texels, the palette and the CLUT format, blind to the bytes between rows
//   and to the palette for direct-colour textures;
// - textures.ini parsing and the hash file names;
// - the lookup order (PPSSPP's LookupWildcard) and "do not replace" entries;
// - the box filter that brings a larger PNG to the native size;
// - on softgpu (mpo_ge): no provider and a provider with nothing to replace
//   draw the same bytes as before; a replacement is drawn texel for texel
//   (CLUT4 swizzled icon, at its size and from a 4x PNG); the hot switch
//   between two sets takes effect at the next list run;
// - the dump: a PNG per key and a textures.ini, which then work as a pack.
// F26a:
// - mip levels in textures.ini (a.png|b.png, <key>_N) and on disk
//   (<key>_N.png), [filtering];
// - BuildReplacement: the generated chain (2 x 2 box down to the native
//   size, whose level is `native` itself), the pack's own levels and their
//   size check, the levels softgpu gets;
// - on softgpu: a native-size pack with its own levels picks them by the
//   detail, with mip filtering (and without them, level 0 as before);
//   [filtering] forces the filters, also on a texture without a
//   replacement, not on one listed as "do not replace";
// - the background load: the game's texture until the PNG is decoded (not
//   dumped meanwhile), the synchronous mode, the preload and its budget;
// - the reload of a pack (in place, keeping the active set) and the folder
//   watch.
// PNG files are written and read through SDL3 (png_io.hpp); without it,
// those checks are skipped (and said so).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "png_io.hpp"
#include "texreplace_test_util.hpp"
#include "texture_key.hpp"
#include "texture_replacer.hpp"
#include "textures_ini.hpp"
#include "xxhash.h"

namespace {

using namespace texreplace_test;
namespace tr = mpo::texreplace;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

// The state of a CLUT4 icon draw, as Ge::ExecutePrim hands it over.
struct IconDraw {
    Machine m;
    ListBuilder lb;
    explicit IconDraw(std::uint32_t icon = kIconAddr, std::uint32_t clut = kClutAddr) {
        lb.LoadClut(clut);
        lb.Texture(icon, 32, 5, 4, 4, true);
        lb.Sprites(true, {{0, 0, 32, 16, 0, 0, 32, 16, 0xFFFFFFFF}});
    }
};

// A provider that records the key of every texture it is asked about.
class KeyProbe final : public mpo::ge::TextureProvider {
public:
    std::vector<tr::ReplacementKey> keys;
    const mpo::ge::ReplacedTexture *Find(const mpo::ge::GeState &gs, const mpo::ge::MemoryView &mem,
                                         const mpo::ge::SamplerState &st,
                                         const mpo::ge::ClutLoadInfo &clut) override {
        keys.push_back(tr::ComputeKey(gs, mem, st, clut, tr::HashType::kXxh64));
        return nullptr;
    }
};

tr::ReplacementKey ProbeIcon(std::uint32_t icon, std::uint32_t clut,
                             void (*tweak)(Machine &) = nullptr) {
    IconDraw d(icon, clut);
    if (tweak) tweak(d.m);
    KeyProbe probe;
    d.m.ge.SetTextureProvider(&probe);
    expect(d.m.Run(d.lb), "icon list runs");
    expect(probe.keys.size() == 1, "one textured draw");
    return probe.keys.empty() ? tr::ReplacementKey{} : probe.keys[0];
}

void TestKey() {
    const tr::ReplacementKey a = ProbeIcon(kIconAddr, kClutAddr);
    // PPSSPP's formula, by hand: dim = TEXSIZE0 & 0xF0F = 0x405; the CLUT
    // hash covers min(64 + 0, 64) bytes; the texels bufw * h / 2 bytes.
    Machine m;
    const std::uint32_t clut_reg = op(kCmdClutFormat, 3 | (0xFFu << 8));
    const std::uint32_t cluthash = XXH32(&m.ram[kClutAddr - kRamBase], 64, 0xC0108888) ^ clut_reg;
    const std::uint64_t cachekey =
        ((static_cast<std::uint64_t>(kIconAddr) & 0x3FFFFFFF) << 32 | 0x405u) ^ cluthash;
    const auto hash =
        static_cast<std::uint32_t>(XXH64(&m.ram[kIconAddr - kRamBase], 32 * 16 / 2, 0xBACD7814));
    expect(a.cachekey == cachekey, "cachekey follows TexCacheEntry::CacheKey");
    expect(a.hash == hash, "hash is XXH64 of the texels, truncated");
    std::printf("  icon key %s\n", tr::HashName(a).c_str());
    // Stable: a fixed value (the formula above, frozen), so a pack's names
    // never change from one build to another.
    expect(tr::HashName(tr::WithoutAddress(a)) == "0000000009f357c00cdcc7e2",
           "stable name of the synthetic icon: " + tr::HashName(tr::WithoutAddress(a)));

    const tr::ReplacementKey moved = ProbeIcon(kIconAddr2, kClutAddr);
    expect(moved.cachekey != a.cachekey, "the full key has the address");
    expect(tr::WithoutAddress(moved) == tr::WithoutAddress(a),
           "without the address, the same texels give the same key");
    const tr::ReplacementKey other_clut = ProbeIcon(kIconAddr, kClutAddr2);
    expect(tr::WithoutAddress(other_clut).cachekey != tr::WithoutAddress(a).cachekey,
           "another palette changes the key");
    expect(other_clut.hash == a.hash, "the palette is not in the texel hash");
    const tr::ReplacementKey texel = ProbeIcon(
        kIconAddr, kClutAddr, [](Machine &mm) { mm.ram[kIconAddr - kRamBase + 37] ^= 0x10; });
    expect(texel.hash != a.hash, "a texel changes the hash");
    const tr::ReplacementKey beyond = ProbeIcon(kIconAddr, kClutAddr, [](Machine &mm) {
        // Palette entry 20, past the 64 bytes loaded.
        mm.ram[kClutAddr - kRamBase + 80] ^= 0xFF;
        // And a byte right after the texture.
        mm.ram[kIconAddr - kRamBase + 256] ^= 0xFF;
    });
    expect(beyond == a, "bytes outside the texture and the loaded palette do not count");

    // Direct colour: the palette does not count; bufw > w hashes row by row,
    // blind to the gap.
    auto probe_rgba = [](int bufw, void (*tweak)(Machine &)) {
        Machine mm;
        if (tweak) tweak(mm);
        ListBuilder lb;
        lb.LoadClut(kClutAddr);
        lb.Texture(kRgbaAddr, bufw, 3, 3, 3, false);
        lb.Sprites(true, {{0, 0, 8, 8, 0, 0, 8, 8, 0xFFFFFFFF}});
        KeyProbe probe;
        mm.ge.SetTextureProvider(&probe);
        mm.Run(lb);
        return probe.keys.empty() ? tr::ReplacementKey{} : probe.keys[0];
    };
    const tr::ReplacementKey r8 = probe_rgba(8, nullptr);
    const tr::ReplacementKey r8_clut =
        probe_rgba(8, [](Machine &mm) { mm.ram[kClutAddr - kRamBase] ^= 0xFF; });
    expect(r8 == r8_clut, "8888: the palette is not in the key");
    expect((r8.cachekey & 0xFFFFFFFFu) == 0x303u, "8888: the low half is the dim alone");
    const tr::ReplacementKey r16 = probe_rgba(16, nullptr);
    const tr::ReplacementKey r16_gap = probe_rgba(16, [](Machine &mm) {
        mm.ram[kRgbaAddr - kRamBase + 8 * 4 + 1] ^= 0xFF; // texel (8, 0): in the gap
    });
    expect(r16 == r16_gap, "bufw > w: the bytes between rows do not count");
    std::uint32_t rows = 0;
    {
        Machine mm;
        for (int y = 0; y < 8; ++y)
            rows = (rows * 11) ^
                   static_cast<std::uint32_t>(
                       XXH64(&mm.ram[kRgbaAddr - kRamBase + static_cast<std::uint32_t>(y) * 64], 32,
                             0xBACD7814));
    }
    expect(r16.hash == rows, "bufw > w: (result * 11) ^ row hash, as PPSSPP");

    // Names.
    tr::ReplacementKey k;
    int level = -1;
    expect(tr::ParseHashName("00000000bb2bdcb33dc35a44", &k, &level) && k.cachekey == 0xbb2bdcb3u &&
               k.hash == 0x3dc35a44u && level == 0,
           "ParseHashName");
    expect(tr::ParseHashName("08300000000004053dc35a44_2", &k, &level) &&
               k.cachekey == 0x0830000000000405ull && level == 2,
           "ParseHashName with a level");
    expect(tr::ParseHashName("0830000000000405", &k, &level) && k.hash == 0,
           "ParseHashName: the cachekey alone");
    expect(!tr::ParseHashName("xyz", &k, &level), "ParseHashName rejects non-hex");
    expect(tr::HashName({0x12, 0xAB}) == "0000000000000012000000ab", "HashName");
}

void TestIni() {
    tr::TexturesIni ini;
    std::string error;
    const std::string text = "\xEF\xBB\xBF# a pack\r\n"
                             "[options]\r\n"
                             "version = 1\r\n"
                             "hash = XXH64   # the default\r\n"
                             "ignoreAddress = true\r\n"
                             "ignoreMipmap = true\r\n"
                             "[games]\r\n"
                             "ULUS10202 = textures.ini\r\n"
                             "[hashes]\r\n"
                             "00000000bb2bdcb33dc35a44 = icons/y.png\r\n"
                             "00000000bb2bdcb33dc35a44_1 = icons/y_1.png\r\n"
                             "0000000000000303deadbeef = a\\b.png|a\\b1.png\r\n"
                             "000000000000040500000000 =\r\n"
                             "00000000000004051234abcd = with\\#hash.png\r\n"
                             "[hashranges]\r\n"
                             "08b31020,512,512 = 480,272\r\n";
    expect(tr::ParseTexturesIni(text, &ini, &error), "ini parses: " + error);
    expect(ini.hash == tr::HashType::kXxh64 && ini.ignore_address && ini.version == 1, "options");
    expect(ini.hashes.size() == 4, "four level-0 entries");
    if (ini.hashes.size() == 4) {
        expect(ini.hashes[0].file == "icons/y.png" && ini.hashes[0].key.hash == 0x3dc35a44u,
               "entry 1");
        // F26a: its _1 line is its level 1.
        expect(ini.hashes[0].levels == std::vector<std::string>{"icons/y_1.png"},
               "entry 1: level 1 from the _1 line");
        expect(ini.hashes[1].file == "a/b.png", "entry 2: backslashes, first mip level");
        expect(ini.hashes[1].levels == std::vector<std::string>{"a/b1.png"},
               "entry 2: level 1 from a|b");
        expect(ini.hashes[2].file.empty(), "entry 3: listed, not replaced");
        expect(ini.hashes[3].file == "with#hash.png", "entry 4: escaped #");
    }
    expect(ini.warnings.size() == 2,
           "warnings for [games] and [hashranges]: " + std::to_string(ini.warnings.size()));

    // F26a: levels must follow each other; [filtering].
    tr::TexturesIni lv;
    expect(tr::ParseTexturesIni("[options]\nhash = xxh64\n[hashes]\n"
                                "00000000bb2bdcb33dc35a44_1 = c.png\n"
                                "00000000bb2bdcb33dc35a44 = a.png|b.png\n"
                                "00000000bb2bdcb33dc35a44_2 = d.png\n"
                                "00000000bb2bdcb33dc35a44_4 = e.png\n"
                                "0000000011111111aaaaaaaa_1 = only1.png\n"
                                "[filtering]\n"
                                "00000000bb2bdcb33dc35a44 = Linear\n"
                                "0000000022222222 = nearest\n"
                                "0000000000000000 = auto\n"
                                "0000000033333333 = bilinear\n",
                                &lv, &error),
           "levels and filtering parse: " + error);
    expect(lv.hashes.size() == 1 && lv.hashes[0].file == "a.png" &&
               lv.hashes[0].levels == std::vector<std::string>{"b.png", "c.png", "d.png"},
           "a|b, then _1 and _2, joined as ComputeAliasMap does (and _4 after a gap is not)");
    expect(lv.filtering.size() == 3, "three [filtering] entries");
    if (lv.filtering.size() == 3) {
        expect(lv.filtering[0].filter == mpo::ge::TextureFilter::kLinear &&
                   lv.filtering[0].key.hash == 0x3dc35a44u,
               "linear, any case");
        expect(lv.filtering[1].filter == mpo::ge::TextureFilter::kNearest &&
                   lv.filtering[1].key.cachekey == 0x22222222u && lv.filtering[1].key.hash == 0,
               "a CLUT/size half alone");
        expect(lv.filtering[2].filter == mpo::ge::TextureFilter::kAuto &&
                   lv.filtering[2].key == tr::ReplacementKey{},
               "the global entry");
    }
    expect(lv.warnings.size() == 3, "warnings: level 4 without 3, level 1 without 0, a bad mode: " +
                                        std::to_string(lv.warnings.size()));

    tr::TexturesIni other;
    expect(tr::ParseTexturesIni("[options]\nhash = xxh32\n", &other, &error) &&
               other.hash == tr::HashType::kXxh32 && !other.ignore_address,
           "xxh32, ignoreAddress defaults to false (PPSSPP)");
    expect(!tr::ParseTexturesIni("[options]\nhash = quick\n", &other, &error), "quick is refused");
    expect(!tr::ParseTexturesIni("[options]\nignoreAddress = true\n", &other, &error),
           "a hash type is required");
    expect(!tr::ParseTexturesIni("[options]\nhash = xxh64\nreduceHash = true\n", &other, &error),
           "reduceHash is refused");
    expect(tr::ParseTexturesIni(tr::DefaultTexturesIni(), &other, &error) &&
               other.hash == tr::HashType::kXxh64 && other.ignore_address && other.hashes.empty() &&
               other.warnings.empty(),
           "the dump's textures.ini");
}

void TestLookup() {
    tr::TexturesIni ini;
    ini.ignore_address = false;
    tr::ReplacementSet set("unused", ini);
    const tr::ReplacementKey full{0x08300000BB2BDCB3ull, 0x3DC35A44u};
    expect(set.Lookup(full) == nullptr, "empty set");
    set.AddAlias({0, 0x3DC35A44u}, "any.png");
    expect(set.Lookup(full) && *set.Lookup(full) == "any.png", "texel hash alone");
    set.AddAlias({0xBB2BDCB3ull, 0x3DC35A44u}, "noaddr.png");
    expect(*set.Lookup(full) == "noaddr.png", "without the address before the texel hash alone");
    set.AddAlias({0xBB2BDCB3ull, 0}, "clut.png");
    expect(*set.Lookup(full) == "clut.png", "CLUT/size half before that");
    set.AddAlias(full, "exact.png");
    expect(*set.Lookup(full) == "exact.png", "the exact key first");
    set.AddAlias(full, "");
    expect(set.Lookup(full) == nullptr, "an empty file stops the lookup");

    tr::TexturesIni ignore;
    ignore.ignore_address = true;
    tr::ReplacementSet set2("unused", ignore);
    set2.AddAlias({0x08300000BB2BDCB3ull, 0x3DC35A44u}, "with_address.png");
    expect(set2.Lookup(full) == nullptr, "ignoreAddress: names with an address never match");
    set2.AddAlias({0xBB2BDCB3ull, 0x3DC35A44u}, "y.png");
    expect(set2.Lookup(full) && *set2.Lookup(full) == "y.png", "ignoreAddress: content key");
    expect(set2.Lookup({0x08310000BB2BDCB3ull, 0x3DC35A44u}) != nullptr,
           "ignoreAddress: anywhere in memory");
}

void TestFilteringLookup() {
    tr::TexturesIni ini;
    ini.ignore_address = true;
    tr::ReplacementSet set("unused", ini);
    const tr::ReplacementKey full{0x08300000BB2BDCB3ull, 0x3DC35A44u};
    mpo::ge::TextureFilter f = mpo::ge::TextureFilter::kAuto;
    expect(!set.FindFiltering(full, &f) && !set.has_filtering(), "no [filtering]");
    set.AddFiltering({0, 0}, mpo::ge::TextureFilter::kNearest);
    expect(set.FindFiltering(full, &f) && f == mpo::ge::TextureFilter::kNearest,
           "the global entry");
    set.AddFiltering({0xBB2BDCB3ull, 0}, mpo::ge::TextureFilter::kLinear);
    expect(set.FindFiltering(full, &f) && f == mpo::ge::TextureFilter::kLinear,
           "the CLUT/size half before the global entry");
    set.AddFiltering({0xBB2BDCB3ull, 0x3DC35A44u}, mpo::ge::TextureFilter::kAuto);
    expect(set.FindFiltering(full, &f) && f == mpo::ge::TextureFilter::kAuto,
           "the exact key (without the address) first");
}

void TestBuild() {
    using Img = tr::DecodedImage;
    std::vector<std::string> notes;
    // A native-size image: one level, as in F24c.
    auto same = tr::BuildReplacement(Img{16, 16, Pattern(16, 16, 0)}, {}, 16, 16, true, &notes);
    expect(same->mips.empty() && same->native_mips.empty() && same->native.size() == 256,
           "native size: no levels");
    // A 4x image: levels of 32 and 16; the 16 is `native` itself.
    const std::vector<std::uint8_t> big = Pattern(64, 64, 1);
    auto hd = tr::BuildReplacement(Img{64, 64, big}, {}, 16, 16, true, &notes);
    expect(hd->mips.size() == 2 && hd->mips[0].width == 32 && hd->mips[1].width == 16 &&
               hd->native_mips.empty(),
           "4x: two generated levels, none for softgpu");
    if (hd->mips.size() == 2) {
        std::vector<std::uint32_t> half;
        tr::ScaleToNative(big.data(), 64, 64, 32, 32, &half);
        std::uint32_t c = 0;
        std::memcpy(&c, &hd->mips[0].rgba[(5 * 32 + 7) * 4], 4);
        expect(c == half[5 * 32 + 7], "level 1 is the 2 x 2 box of level 0");
        bool native = true;
        for (std::size_t i = 0; i < 256; ++i) {
            std::memcpy(&c, &hd->mips[1].rgba[i * 4], 4);
            native = native && c == hd->native[i];
        }
        expect(native, "the level of the native size is `native`");
    }
    expect(tr::BuildReplacement(Img{64, 64, big}, {}, 16, 16, false, &notes)->mips.empty(),
           "no generation when it is off");
    // 3x: one level of 1.5x (not a power of two), then it stops.
    auto three = tr::BuildReplacement(Img{48, 48, Pattern(48, 48, 2)}, {}, 16, 16, true, &notes);
    expect(three->mips.size() == 1 && three->mips[0].width == 24, "3x: one level of 24");
    // The pack's own levels of a native-size image: softgpu gets them.
    auto own = tr::BuildReplacement(
        Img{16, 16, SolidImage(16, 16, 0xFF0000FFu)},
        {Img{8, 8, SolidImage(8, 8, 0xFF00FF00u)}, Img{4, 4, SolidImage(4, 4, 0xFFFF0000u)}}, 16,
        16, true, &notes);
    expect(own->mips.size() == 2 && own->native_mips.size() == 2 &&
               own->native_mips[0].width == 8 && own->native_mips[1].texels[0] == 0xFFFF0000u,
           "the pack's levels below the native size go to softgpu too");
    expect(notes.empty(), "no notes so far");
    // A level of the wrong size stops the chain there.
    auto bad =
        tr::BuildReplacement(Img{16, 16, SolidImage(16, 16, 0xFF0000FFu)},
                             {Img{7, 7, SolidImage(7, 7, 0xFF00FF00u)}}, 16, 16, true, &notes);
    expect(bad->mips.empty() && notes.size() == 1, "a level of the wrong size: noted, not used");
    // A 4x pack with its own levels: `native` is its level of the native size.
    auto hd_own = tr::BuildReplacement(Img{64, 64, big},
                                       {Img{32, 32, SolidImage(32, 32, 0xFF101010u)},
                                        Img{16, 16, SolidImage(16, 16, 0xFF202020u)},
                                        Img{8, 8, SolidImage(8, 8, 0xFF303030u)}},
                                       16, 16, true, &notes);
    expect(hd_own->mips.size() == 3 && hd_own->native[0] == 0xFF202020u &&
               hd_own->native_mips.size() == 1 && hd_own->native_mips[0].texels[0] == 0xFF303030u,
           "4x with its own levels: native and softgpu's level 1 are the pack's");
    expect(hd->id != hd_own->id && hd->id != 0, "ids");
}

void TestScale() {
    std::vector<std::uint32_t> out;
    const std::vector<std::uint8_t> img = {1, 2, 3, 4, 5, 6, 7, 8};
    tr::ScaleToNative(img.data(), 2, 1, 2, 1, &out);
    expect(out.size() == 2 && out[0] == 0x04030201u && out[1] == 0x08070605u, "same size: copy");
    // 2x2 -> 1x1: alpha-weighted colour, mean alpha.
    const std::vector<std::uint8_t> quad = {200, 0, 0, 255, 0, 0, 0, 0, 0, 100, 0, 255, 0, 0, 0, 0};
    tr::ScaleToNative(quad.data(), 2, 2, 1, 1, &out);
    expect(out.size() == 1 && out[0] == ((128u << 24) | (50u << 8) | 100u),
           "box filter ignores the colour of transparent pixels");
    // Smaller: nearest.
    tr::ScaleToNative(img.data(), 2, 1, 4, 1, &out);
    expect(out.size() == 4 && out[0] == out[1] && out[2] == out[3] && out[2] == 0x08070605u,
           "smaller image: nearest");
    // A 4x nearest upscale comes back exactly.
    const std::vector<std::uint8_t> y16 = LetterY(32, 16);
    const std::vector<std::uint8_t> y64 = UpscaleNearest(y16, 32, 16, 4);
    tr::ScaleToNative(y64.data(), 128, 64, 32, 16, &out);
    bool same = out.size() == 32 * 16;
    for (std::size_t i = 0; same && i < out.size(); ++i) {
        std::uint32_t c;
        std::memcpy(&c, &y16[i * 4], 4);
        same = out[i] == c;
    }
    expect(same, "4x nearest upscale scales back to the same image");
}

// The 32x16 icon area of the framebuffer, as RGBA words.
std::vector<std::uint32_t> IconPixels(const Machine &m, int x0, int y0) {
    std::vector<std::uint32_t> px;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x) px.push_back(m.Pixel(x0 + x, y0 + y));
    return px;
}

void TestSoftgpu() {
    ListBuilder lb;
    lb.Clear(0xFF402010);
    lb.LoadClut(kClutAddr);
    lb.Texture(kIconAddr, 32, 5, 4, 4, true);
    lb.Sprites(true, {{10, 10, 42, 26, 0, 0, 32, 16, 0xFFFFFFFF}});
    lb.Blend(true);
    lb.Texture(kIconAddr2, 32, 5, 4, 4, true, 0x100, true, false);
    lb.Sprites(true, {{100, 40, 164, 72, 0, 0, 32, 16, 0xFF80FFFF}});
    lb.Blend(false);
    lb.Texture(kRgbaAddr, 16, 4, 4, 3, false);
    lb.Sprites(true, {{200, 10, 232, 42, 0, 0, 16, 16, 0xFFFFFFFF}});

    Machine plain;
    expect(plain.Run(lb), "plain list runs");

    // A provider without sets (and no dump): the same bytes.
    Machine idle;
    tr::TextureReplacer idle_replacer;
    idle.ge.SetTextureProvider(&idle_replacer);
    expect(idle.Run(lb), "idle list runs");
    expect(idle.vram == plain.vram, "a replacer with nothing to do changes nothing");
    expect(idle_replacer.stats().lookups == 0, "and does not even hash");

    // A set whose key matches nothing: the same bytes.
    tr::TexturesIni ini;
    ini.ignore_address = true;
    Machine miss;
    tr::TextureReplacer miss_replacer;
    auto miss_set = std::make_unique<tr::ReplacementSet>("unused", ini);
    miss_set->AddAlias({0x1234, 0x5678}, "nothing.png");
    miss_replacer.SetActiveSet(miss_replacer.AddSet(std::move(miss_set)));
    miss.ge.SetTextureProvider(&miss_replacer);
    expect(miss.Run(lb), "miss list runs");
    expect(miss.vram == plain.vram, "a set that covers nothing changes nothing");
    expect(miss_replacer.stats().lookups == 3 && miss_replacer.stats().replaced == 0,
           "three lookups, nothing replaced");

    if (!tr::HavePng()) {
        std::printf("  SKIP: no PNG support in this build: replacement and dump checks\n");
        return;
    }

    // The pack: the icon's content key -> our "Y", native size; and the 8888
    // texture -> a 4x PNG.
    const std::filesystem::path pack = FreshDir("mpo_texreplace_test_pack");
    const tr::ReplacementKey icon = tr::WithoutAddress(ProbeIcon(kIconAddr, kClutAddr));
    const std::vector<std::uint8_t> y = LetterY(32, 16);
    std::string error;
    expect(tr::SavePng(pack / "y.png", 32, 16, y, &error), "write y.png: " + error);
    std::vector<std::uint8_t> big(64 * 64 * 4);
    for (std::size_t i = 0; i < big.size(); i += 4) {
        big[i] = 0x10;
        big[i + 1] = static_cast<std::uint8_t>(i / 4 % 64 * 4);
        big[i + 2] = 0xC0;
        big[i + 3] = 0xFF;
    }
    expect(tr::SavePng(pack / "big.png", 64, 64, big, &error), "write big.png: " + error);
    tr::ReplacementKey rgba_key;
    {
        Machine mm;
        ListBuilder l2;
        l2.Texture(kRgbaAddr, 16, 4, 4, 3, false);
        l2.Sprites(true, {{0, 0, 16, 16, 0, 0, 16, 16, 0xFFFFFFFF}});
        KeyProbe probe;
        mm.ge.SetTextureProvider(&probe);
        mm.Run(l2);
        if (!probe.keys.empty()) rgba_key = tr::WithoutAddress(probe.keys[0]);
    }
    {
        std::ofstream out(pack / "textures.ini", std::ios::binary);
        out << "[options]\nversion = 1\nhash = xxh64\nignoreAddress = true\n\n[hashes]\n"
            << tr::HashName(rgba_key) << " = big.png\n";
    }
    // The icon by its file name, as a dump names it.
    std::filesystem::copy_file(pack / "y.png", pack / (tr::HashName(icon) + ".png"));

    auto set = tr::ReplacementSet::Load(pack, &error);
    expect(set != nullptr, "the pack loads: " + error);
    if (!set) return;
    expect(set->size() == 2, "two replacements (one listed, one by file name)");
    Machine rep;
    tr::TextureReplacer replacer;
    replacer.SetActiveSet(replacer.AddSet(std::move(set)));
    rep.ge.SetTextureProvider(&replacer);
    expect(rep.Run(lb), "replaced list runs");
    expect(replacer.stats().replaced == 3,
           "three draws replaced: " + std::to_string(replacer.stats().replaced));
    expect(replacer.stats().distinct.size() == 2, "two images");
    // The first sprite is 1:1 with REPLACE: the PNG's pixels, where opaque
    // (alpha 0 keeps the clear colour with the alpha test of the sprite path).
    const std::vector<std::uint32_t> px = IconPixels(rep, 10, 10);
    bool exact = true;
    for (int i = 0; i < 32 * 16; ++i) {
        std::uint32_t c;
        std::memcpy(&c, &y[static_cast<std::size_t>(i) * 4], 4);
        if ((c >> 24) != 0 && px[static_cast<std::size_t>(i)] != c) exact = false;
    }
    expect(exact, "softgpu draws the replacement texel for texel");
    expect(IconPixels(rep, 10, 10) != IconPixels(plain, 10, 10), "and it is not the game's icon");
    // The 4x PNG is box-filtered to 16x16: the pixel at the sprite's (0, 0)
    // is the mean of the PNG's top-left 4x4.
    expect(rep.Pixel(200, 10) == 0xFFC00610u, "the larger PNG is filtered to the texture's size: " +
                                                  std::to_string(rep.Pixel(200, 10)));
    // Everything outside the sprites is the same as without the pack.
    bool outside = true;
    for (int yy = 100; yy < 272; ++yy)
        for (int xx = 0; xx < 480; ++xx)
            outside = outside && rep.Pixel(xx, yy) == plain.Pixel(xx, yy);
    expect(outside, "nothing else changes");

    // The hot switch: set 1 is the plain icon (the game's own texels, from
    // a dump), set 0 the "Y". The switch applies at the next run.
    const std::filesystem::path dump = FreshDir("mpo_texreplace_test_dump");
    {
        Machine dm;
        tr::TextureReplacer dumper;
        expect(dumper.SetDumpDir(dump, &error), "dump dir: " + error);
        dm.ge.SetTextureProvider(&dumper);
        expect(dm.Run(lb), "dump list runs");
        expect(dm.vram == plain.vram, "dumping changes nothing on screen");
        // Icon (one key: the two copies share it), its other palette state is
        // the same, and the 8888 texture.
        expect(dumper.stats().dumped == 2,
               "two PNGs dumped: " + std::to_string(dumper.stats().dumped));
        expect(std::filesystem::exists(dump / "textures.ini"), "the dump writes a textures.ini");
        expect(std::filesystem::exists(dump / (tr::HashName(icon) + ".png")),
               "the icon's PNG has its key as name");
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> back;
        expect(tr::LoadPng(dump / (tr::HashName(icon) + ".png"), &w, &h, &back, &error) &&
                   w == 32 && h == 16,
               "the dumped icon reads back");
        Machine again;
        tr::TextureReplacer dumper2;
        expect(dumper2.SetDumpDir(dump, &error), "dump dir again");
        again.ge.SetTextureProvider(&dumper2);
        again.Run(lb);
        expect(dumper2.stats().dumped == 0, "keys already dumped are not written again");
    }
    auto dumped_set = tr::ReplacementSet::Load(dump, &error);
    expect(dumped_set != nullptr && dumped_set->size() == 2, "the dump loads as a pack");
    if (!dumped_set) return;
    const int dumped_index = replacer.AddSet(std::move(dumped_set));
    replacer.SetActiveSet(dumped_index);
    expect(replacer.active_set() == 0, "the switch waits for the next run");
    Machine sw;
    sw.ge.SetTextureProvider(&replacer);
    expect(sw.Run(lb), "switched list runs");
    expect(replacer.active_set() == dumped_index, "the next run uses the new set");
    expect(sw.vram == plain.vram, "the dumped textures as a pack draw exactly the game's frame");
    replacer.SetActiveSet(0);
    Machine back_to_y;
    back_to_y.ge.SetTextureProvider(&replacer);
    expect(back_to_y.Run(lb), "switched back");
    expect(back_to_y.vram == rep.vram, "and back to the Y");
    replacer.SetActiveSet(-1);
    Machine off;
    off.ge.SetTextureProvider(&replacer);
    expect(off.Run(lb), "set -1");
    expect(off.vram == plain.vram, "no active set: the game's frame");
    expect(replacer.stats().switches == 4,
           "four switches applied: " + std::to_string(replacer.stats().switches));

    // A PNG that cannot be read: reported once, the game's texture drawn.
    {
        const std::filesystem::path broken = FreshDir("mpo_texreplace_test_broken");
        std::ofstream(broken / "textures.ini", std::ios::binary)
            << "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n"
            << tr::HashName(icon) << " = missing.png\n";
        auto bset = tr::ReplacementSet::Load(broken, &error);
        expect(bset != nullptr, "broken pack loads");
        if (bset) {
            tr::TextureReplacer br;
            int logged = 0;
            br.SetLog([&](std::string_view) { ++logged; });
            br.SetActiveSet(br.AddSet(std::move(bset)));
            Machine bm;
            bm.ge.SetTextureProvider(&br);
            bm.Run(lb);
            expect(bm.vram == plain.vram, "an unreadable PNG leaves the game's texture");
            expect(logged == 1 && br.stats().load_errors == 1, "and is reported once");
        }
    }
    expect(tr::ReplacementSet::Load(FreshDir("mpo_texreplace_test_empty"), &error) == nullptr,
           "a folder without textures.ini is not a pack");
}

// ---- F25c: textures in VRAM ----

// The content key of the first texture `lb` samples on `m`.
tr::ReplacementKey ProbeFirst(Machine &m, const ListBuilder &lb) {
    KeyProbe probe;
    m.ge.SetTextureProvider(&probe);
    expect(m.Run(lb), "probe list runs");
    m.ge.SetTextureProvider(nullptr);
    expect(!probe.keys.empty(), "probe saw a texture");
    return probe.keys.empty() ? tr::ReplacementKey{} : tr::WithoutAddress(probe.keys[0]);
}

// A replacer whose only set gives `keys` the solid magenta image.
std::unique_ptr<tr::TextureReplacer> SolidReplacer(const std::vector<tr::ReplacementKey> &keys) {
    tr::TexturesIni ini;
    ini.ignore_address = true;
    auto set = std::make_unique<tr::ReplacementSet>("solid", ini);
    for (const tr::ReplacementKey &k : keys) set->AddAlias(k, "solid.png");
    set->AddImage("solid.png", 16, 16, Solid(kSolid));
    auto r = std::make_unique<tr::TextureReplacer>();
    r->SetActiveSet(r->AddSet(std::move(set)));
    return r;
}

void TestVram() {
    // A: an 8888 texture the CPU copied into VRAM is replaced.
    ListBuilder vram_list;
    vram_list.Clear(0xFF402010);
    SampleRgba(vram_list, kVramTex, 300, 100);
    Machine plain;
    plain.CopyToVram(kVramTex, kRgbaAddr);
    const tr::ReplacementKey kv = ProbeFirst(plain, vram_list);
    {
        Machine again;
        again.CopyToVram(kVramTex, kRgbaAddr);
        expect(again.Run(vram_list) && again.vram == plain.vram, "VRAM list is deterministic");
    }
    const std::vector<std::uint8_t> plain_vram = plain.vram;
    {
        Machine idle;
        idle.CopyToVram(kVramTex, kRgbaAddr);
        tr::TextureReplacer none;
        idle.ge.SetTextureProvider(&none);
        expect(idle.Run(vram_list) && idle.vram == plain_vram,
               "VRAM: a replacer without sets changes nothing");
        Machine miss;
        miss.CopyToVram(kVramTex, kRgbaAddr);
        auto other = SolidReplacer({{0x1234, 0x5678}});
        miss.ge.SetTextureProvider(other.get());
        expect(miss.Run(vram_list) && miss.vram == plain_vram,
               "VRAM: a set that covers nothing changes nothing");
        expect(other->stats().vram == 1 && other->stats().replaced == 0,
               "VRAM: one lookup in VRAM, nothing replaced");
    }
    {
        Machine rep;
        rep.CopyToVram(kVramTex, kRgbaAddr);
        auto r = SolidReplacer({kv});
        rep.ge.SetTextureProvider(r.get());
        expect(rep.Run(vram_list), "VRAM replaced list runs");
        expect(rep.Pixel(300, 100) == kSolid && rep.Pixel(331, 131) == kSolid,
               "a texture in VRAM is replaced");
        expect(r->stats().vram_replaced == 1 && r->stats().framebuffer == 0,
               "counted as replaced in VRAM");
        bool outside = true;
        for (int y = 0; y < 272; ++y)
            for (int x = 0; x < 480; ++x)
                if (x < 300 || x >= 332 || y < 100 || y >= 132)
                    outside = outside && rep.Pixel(x, y) == plain.Pixel(x, y);
        expect(outside, "nothing else changes");
    }

    // The same content as a RAM texture has the same key (ignoreAddress).
    ListBuilder ram_list;
    ram_list.Clear(0xFF402010);
    SampleRgba(ram_list, kRgbaAddr, 300, 100);
    {
        Machine m;
        expect(ProbeFirst(m, ram_list) == kv, "RAM and VRAM copies share the content key");
    }

    // B: a texture taken from a render target is never replaced, even when
    // its content has a replacement.
    ListBuilder rtt_list;
    rtt_list.Clear(0xFF402010);
    DrawTarget(rtt_list);
    SampleRgba(rtt_list, 0x04000000u | kVramTarget, 10, 150);
    Machine rtt_plain;
    const tr::ReplacementKey kt = ProbeFirst(rtt_plain, rtt_list);
    expect(rtt_plain.Pixel(10, 150) == 0xFF0000FFu && rtt_plain.Pixel(41, 181) == 0xFFFFFFFFu,
           "the render target is drawn and sampled");
    auto rtt_rep = SolidReplacer({kt, kv});
    Machine rtt;
    rtt.ge.SetTextureProvider(rtt_rep.get());
    expect(rtt.Run(rtt_list), "render-to-texture list runs");
    expect(rtt.vram == rtt_plain.vram, "a render target sampled as a texture is not replaced");
    expect(rtt_rep->stats().framebuffer == 1 && rtt_rep->stats().replaced == 0,
           "counted as a framebuffer: " + std::to_string(rtt_rep->stats().framebuffer));
    // The control: the same bytes copied there by the CPU are replaced.
    ListBuilder sample_target;
    sample_target.Clear(0xFF402010);
    SampleRgba(sample_target, 0x04000000u | kVramTarget, 10, 150);
    {
        Machine cpu;
        std::memcpy(&cpu.vram[kVramTarget], &rtt_plain.vram[kVramTarget], 16 * 16 * 4);
        auto r = SolidReplacer({kt});
        cpu.ge.SetTextureProvider(r.get());
        expect(cpu.Run(sample_target) && cpu.Pixel(10, 150) == kSolid,
               "the same content written by the CPU is replaced (the key matches)");
    }

    // C: the target is forgotten kTargetAge flips after its last use, and
    // sampling it keeps it alive.
    {
        tr::TextureReplacer &r = *rtt_rep;
        expect(r.target_count() == 3,
               "three targets (screen, its depth, the 16x16): " + std::to_string(r.target_count()));
        const std::vector<std::string> lines = r.DescribeTargets();
        expect(lines.size() == 3 && lines[0] == "0x000000 stride 2048: rows 0-271, bytes 0-1919" &&
                   lines[2] == "0x190000 stride 64: rows 0-15, bytes 0-63",
               "the targets as mpo_boot reports them: " + (lines.empty() ? "" : lines[0]));
        for (std::uint64_t i = 0; i < tr::TextureReplacer::kTargetAge; ++i) r.Flip(nullptr);
        expect(rtt.Run(sample_target) && r.stats().framebuffer == 2 && r.stats().replaced == 0,
               "still a framebuffer kTargetAge flips later (and sampling refreshes it)");
        for (std::uint64_t i = 0; i < tr::TextureReplacer::kTargetAge; ++i) r.Flip(nullptr);
        expect(rtt.Run(sample_target) && r.stats().framebuffer == 3,
               "kTargetAge flips after sampling, still one");
        for (std::uint64_t i = 0; i <= tr::TextureReplacer::kTargetAge; ++i) r.Flip(nullptr);
        expect(r.stats().targets_aged >= 1, "unused for longer: forgotten");
        expect(rtt.Run(sample_target) && rtt.Pixel(10, 150) == kSolid && r.stats().replaced == 1,
               "and then it is a texture like any other");
    }

    // D: what the display shows is a framebuffer.
    {
        Machine m;
        m.CopyToVram(kVramTex, kRgbaAddr);
        auto r = SolidReplacer({kv});
        m.ge.SetTextureProvider(r.get());
        const mpo::ge::MemoryRect shown{kVramTex, 64, 64, 16};
        r->Flip(&shown);
        expect(m.Run(vram_list) && m.vram == plain_vram && r->stats().framebuffer == 1,
               "the displayed framebuffer is not replaced");
    }

    // E: a video frame is not replaced (in VRAM or in RAM) for kVideoAge
    // flips after sceMpeg wrote it.
    {
        Machine m;
        m.CopyToVram(kVramTex, kRgbaAddr);
        auto r = SolidReplacer({kv});
        m.ge.SetTextureProvider(r.get());
        r->VideoWritten(kVramTex, 16 * 16 * 4);
        expect(m.Run(vram_list) && m.vram == plain_vram && r->stats().video == 1,
               "a video frame in VRAM is not replaced");
        for (std::uint64_t i = 0; i < tr::TextureReplacer::kVideoAge; ++i) r->Flip(nullptr);
        expect(m.Run(vram_list) && m.Pixel(300, 100) != kSolid && r->stats().video == 2,
               "nor kVideoAge flips later");
        r->Flip(nullptr);
        expect(m.Run(vram_list) && m.Pixel(300, 100) == kSolid,
               "once the video stops, the texture is replaced");
        Machine ram;
        auto rr = SolidReplacer({kv});
        ram.ge.SetTextureProvider(rr.get());
        expect(ram.Run(ram_list) && ram.Pixel(300, 100) == kSolid, "the RAM texture is replaced");
        rr->VideoWritten(kRgbaAddr, 16 * 16 * 4);
        expect(ram.Run(ram_list) && ram.Pixel(300, 100) != kSolid && rr->stats().video == 1,
               "a video frame in RAM is not");
    }

    // F: a block transfer over a VRAM texture makes it hash again inside
    // the same list: first sprite with content B (no replacement), then A
    // copied over it by the GE, second sprite replaced.
    {
        ListBuilder lb;
        lb.Clear(0xFF402010);
        SampleRgba(lb, kVramTex, 300, 100);
        lb.Transfer(kRgbaAddr, 16, kVramTex, 16, 16, 16);
        SampleRgba(lb, kVramTex, 350, 100);
        Machine soft_plain;
        soft_plain.CopyToVram(kVramTex, kRgbaAddr2);
        expect(soft_plain.Run(lb), "transfer list runs");
        Machine m;
        m.CopyToVram(kVramTex, kRgbaAddr2);
        auto r = SolidReplacer({kv});
        m.ge.SetTextureProvider(r.get());
        expect(m.Run(lb), "transfer list runs with the pack");
        expect(m.Pixel(300, 100) == soft_plain.Pixel(300, 100) && m.Pixel(300, 100) != kSolid,
               "before the transfer: the game's texels");
        expect(m.Pixel(350, 100) == kSolid, "after the transfer: rehashed and replaced");
        expect(r->stats().hashes == 2, "two hashes: " + std::to_string(r->stats().hashes));
    }

    // G: a CPU write to VRAM is seen at the next flip; until then the hash is
    // cached (PPSSPP checks once per frame too).
    {
        Machine m;
        m.CopyToVram(kVramTex, kRgbaAddr);
        auto r = SolidReplacer({kv});
        m.ge.SetTextureProvider(r.get());
        expect(m.Run(vram_list) && m.Pixel(300, 100) == kSolid, "A: replaced");
        expect(m.Run(vram_list) && r->stats().hashes == 1, "the next run reuses the hash");
        m.CopyToVram(kVramTex, kRgbaAddr2);
        r->Flip(nullptr);
        Machine b_plain;
        b_plain.CopyToVram(kVramTex, kRgbaAddr2);
        b_plain.Run(vram_list);
        expect(m.Run(vram_list) && m.vram == b_plain.vram && r->stats().hashes == 2,
               "after the flip the CPU's new texels are hashed: not replaced");
        m.CopyToVram(kVramTex, kRgbaAddr);
        r->Flip(nullptr);
        expect(m.Run(vram_list) && m.Pixel(300, 100) == kSolid, "and A again: replaced again");
    }

    // H: a block transfer from a render target makes a framebuffer.
    {
        ListBuilder lb;
        lb.Clear(0xFF402010);
        DrawTarget(lb);
        lb.Transfer(0x04000000u | kVramTarget, 16, 0x041A0000u, 16, 16, 16);
        SampleRgba(lb, 0x041A0000u, 10, 150);
        Machine p;
        p.Run(lb);
        Machine m;
        auto r = SolidReplacer({kt});
        m.ge.SetTextureProvider(r.get());
        expect(m.Run(lb) && m.vram == p.vram && r->stats().framebuffer == 1,
               "a copy of a render target is a framebuffer too");
    }

    // I: a texture in the stride gap of the screen (columns 480 to 511,
    // never inside the scissor) is not a framebuffer.
    {
        constexpr std::uint32_t gap = 0x04000000u + 480u * 4u;
        ListBuilder lb;
        lb.Clear(0xFF402010);
        SampleRgba(lb, gap, 300, 100, 512);
        Machine p;
        p.CopyToVram(gap, kRgbaAddr, 512 * 4);
        const tr::ReplacementKey kg = ProbeFirst(p, lb);
        Machine m;
        m.CopyToVram(gap, kRgbaAddr, 512 * 4);
        auto r = SolidReplacer({kg});
        m.ge.SetTextureProvider(r.get());
        expect(m.Run(lb) && m.Pixel(300, 100) == kSolid && r->stats().framebuffer == 0,
               "a texture beside the screen, in its stride, is replaced");
        // One column to the left, it overlaps the screen.
        ListBuilder lb2;
        lb2.Clear(0xFF402010);
        SampleRgba(lb2, gap - 16, 300, 100, 512);
        expect(m.Run(lb2) && r->stats().framebuffer == 1, "four texels into the screen: not");
    }
}

// ---- F26a ----

// The content key of the 8888 texture at kRgbaAddr.
tr::ReplacementKey RgbaKey() {
    Machine m;
    ListBuilder lb;
    DrawRgba(lb, 0, 0, 16);
    return ProbeFirst(m, lb);
}

// A replacer with one set, made by `fill`.
template <typename Fill>
std::unique_ptr<tr::TextureReplacer> OneSet(Fill fill) {
    tr::TexturesIni ini;
    ini.ignore_address = true;
    auto set = std::make_unique<tr::ReplacementSet>("f26a", ini);
    fill(*set);
    auto r = std::make_unique<tr::TextureReplacer>();
    r->SetActiveSet(r->AddSet(std::move(set)));
    return r;
}

void TestSoftgpuMips() {
    const tr::ReplacementKey key = RgbaKey();
    constexpr std::uint32_t kRed = 0xFF0000FFu;
    constexpr std::uint32_t kGreen = 0xFF00FF00u;
    constexpr std::uint32_t kBlue = 0xFFFF0000u;
    auto with_levels = [&](bool levels) {
        return OneSet([&](tr::ReplacementSet &set) {
            set.AddAlias(key, "a.png");
            set.AddImage("a.png", 16, 16, SolidImage(16, 16, kRed));
            if (levels) {
                set.SetLevels("a.png", {"a_1.png", "a_2.png"});
                set.AddImage("a_1.png", 8, 8, SolidImage(8, 8, kGreen));
                set.AddImage("a_2.png", 4, 4, SolidImage(4, 4, kBlue));
            }
        });
    };
    ListBuilder lb;
    lb.Clear(0xFF402010);
    DrawRgba(lb, 0, 0, 32);  // magnified: level 0
    DrawRgba(lb, 40, 0, 16); // 1:1: level 0
    DrawRgba(lb, 60, 0, 8);  // detail 16: level 1
    DrawRgba(lb, 70, 0, 4);  // detail 32: level 2
    DrawRgba(lb, 80, 0, 6);  // detail 22: level 1 and 6/16 of level 2
    auto r = with_levels(true);
    Machine m;
    m.ge.SetTextureProvider(r.get());
    expect(m.Run(lb), "mips list runs");
    expect(m.Pixel(5, 5) == kRed && m.Pixel(45, 5) == kRed, "magnified and 1:1: level 0");
    expect(m.Pixel(62, 2) == kGreen, "half size: the pack's level 1");
    expect(m.Pixel(71, 1) == kBlue, "quarter size: level 2");
    // softgpu's level_frac: (c1 * frac + c0 * (16 - frac)) >> 4 per channel.
    const std::uint32_t mixed = m.Pixel(82, 2);
    expect((mixed & 0xFF) == 0 && ((mixed >> 8) & 0xFF) > 0x80 && ((mixed >> 16) & 0xFF) > 0x40 &&
               ((mixed >> 8) & 0xFF) + ((mixed >> 16) & 0xFF) >= 0xF0,
           "between two levels: mip filtering, " + std::to_string(mixed));
    expect(r->stats().mipmapped == 5, "five draws with mip levels");
    // Without the pack's levels: level 0 everywhere, as before F26a.
    auto flat = with_levels(false);
    Machine f;
    f.ge.SetTextureProvider(flat.get());
    expect(f.Run(lb), "flat list runs");
    expect(f.Pixel(62, 2) == kRed && f.Pixel(71, 1) == kRed && f.Pixel(82, 2) == kRed,
           "no levels of its own: level 0 at every size");
    expect(flat->stats().mipmapped == 0, "no mip levels");
}

void TestFiltering() {
    const tr::ReplacementKey key = RgbaKey();
    auto list = [](bool linear) {
        ListBuilder lb;
        lb.Clear(0xFF402010);
        DrawRgba(lb, 100, 100, 48, linear); // magnified 3x: the filter shows
        return lb;
    };
    Machine nearest_plain;
    Machine linear_plain;
    expect(nearest_plain.Run(list(false)) && linear_plain.Run(list(true)), "plain lists run");
    expect(nearest_plain.vram != linear_plain.vram, "the two filters differ");
    auto run = [&](mpo::ge::TextureFilter filter, const tr::ReplacementKey &k, bool linear,
                   bool ignored = false) {
        auto r = OneSet([&](tr::ReplacementSet &set) {
            set.AddFiltering(k, filter);
            if (ignored) set.AddAlias(k, "");
        });
        Machine m;
        m.ge.SetTextureProvider(r.get());
        expect(m.Run(list(linear)), "filtered list runs");
        return std::make_pair(m.vram, r->stats().filtered);
    };
    // Forced on a texture without a replacement (PPSSPP applies it too).
    auto [lin, lin_count] = run(mpo::ge::TextureFilter::kLinear, key, false);
    expect(lin == linear_plain.vram && lin_count == 1, "linear forced on a nearest draw");
    auto [near, near_count] = run(mpo::ge::TextureFilter::kNearest, key, true);
    expect(near == nearest_plain.vram && near_count == 1, "nearest forced on a linear draw");
    auto [aut, aut_count] = run(mpo::ge::TextureFilter::kAuto, key, true);
    expect(aut == linear_plain.vram && aut_count == 0, "auto: the game's filter");
    auto [glob, glob_count] = run(mpo::ge::TextureFilter::kNearest, tr::ReplacementKey{}, true);
    expect(glob == nearest_plain.vram && glob_count == 1, "the global entry");
    auto [ign, ign_count] = run(mpo::ge::TextureFilter::kNearest, key, true, true);
    expect(ign == linear_plain.vram && ign_count == 0,
           "not on a texture listed as \"do not replace\"");
    // From a textures.ini, with a replacement drawn with it.
    const std::filesystem::path dir = FreshDir("mpo_texreplace_test_filtering");
    std::ofstream(dir / "textures.ini", std::ios::binary)
        << "[options]\nhash = xxh64\nignoreAddress = true\n[filtering]\n"
        << tr::HashName(key) << " = linear\n";
    std::string error;
    auto set = tr::ReplacementSet::Load(dir, &error);
    expect(set != nullptr && set->has_filtering(), "[filtering] from a file: " + error);
    if (set) {
        tr::TextureReplacer r;
        r.SetActiveSet(r.AddSet(std::move(set)));
        Machine m;
        m.ge.SetTextureProvider(&r);
        expect(m.Run(list(false)) && m.vram == linear_plain.vram, "and drawn linear");
    }
}

// Runs `lb` on a fresh machine with `r` until `done` says so (at most ~5 s).
template <typename Done>
bool RunUntil(tr::TextureReplacer &r, const ListBuilder &lb, Machine *out, Done done) {
    for (int i = 0; i < 250; ++i) {
        Machine m;
        m.ge.SetTextureProvider(&r);
        if (!m.Run(lb)) return false;
        if (done(m)) {
            out->vram = m.vram;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void TestAsync() {
    if (!tr::HavePng()) {
        std::printf("  SKIP: no PNG support in this build: background load and reload checks\n");
        return;
    }
    const tr::ReplacementKey key = RgbaKey();
    const std::filesystem::path dir = FreshDir("mpo_texreplace_test_async");
    std::string error;
    constexpr std::uint32_t kRed = 0xFF0000FFu;
    constexpr std::uint32_t kGreen = 0xFF00FF00u;
    expect(tr::SavePng(dir / "a.png", 64, 64, SolidImage(64, 64, kRed), &error),
           "write a.png: " + error);
    std::ofstream(dir / "textures.ini", std::ios::binary)
        << "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n"
        << tr::HashName(key) << " = a.png\n";
    ListBuilder lb;
    lb.Clear(0xFF402010);
    DrawRgba(lb, 100, 100, 16);
    Machine plain;
    expect(plain.Run(lb), "plain list runs");

    // Synchronous (the default): the first draw already has it.
    {
        auto set = tr::ReplacementSet::Load(dir, &error);
        tr::TextureReplacer r;
        r.SetActiveSet(r.AddSet(std::move(set)));
        Machine m;
        m.ge.SetTextureProvider(&r);
        expect(m.Run(lb) && m.Pixel(100, 100) == kRed && r.stats().pending == 0,
               "synchronous: replaced at the first draw");
    }
    // In the background: the game's texture (and no dump of it) until it is
    // decoded, then the replacement.
    {
        auto set = tr::ReplacementSet::Load(dir, &error);
        tr::TextureReplacer r;
        r.SetAsync(true, 1);
        const std::filesystem::path dump = FreshDir("mpo_texreplace_test_async_dump");
        expect(r.SetDumpDir(dump, &error), "dump dir: " + error);
        const int index = r.AddSet(std::move(set));
        r.SetActiveSet(index);
        Machine m;
        m.ge.SetTextureProvider(&r);
        expect(m.Run(lb), "async list runs");
        expect(m.vram == plain.vram && r.stats().pending == 1 && r.stats().replaced == 0,
               "the first run draws the game's texture while the PNG loads");
        expect(r.stats().dumped == 0, "and does not dump it");
        expect(r.set(index).WaitIdle(std::chrono::seconds(10)), "the load finishes");
        Machine after;
        after.ge.SetTextureProvider(&r);
        expect(after.Run(lb) && after.Pixel(100, 100) == kRed && r.stats().replaced == 1,
               "the next run draws the replacement");
    }
    // The preload decodes it ahead; a budget too small keeps nothing.
    {
        auto set = tr::ReplacementSet::Load(dir, &error);
        tr::TextureReplacer r;
        r.SetAsync(true, 1);
        r.StartPreload(std::size_t{64} << 20, 1);
        const int index = r.AddSet(std::move(set));
        r.SetActiveSet(index);
        expect(r.set(index).WaitIdle(std::chrono::seconds(10)), "the preload finishes");
        tr::ReplacementSet::PreloadStats ps = r.set(index).preload_stats();
        expect(ps.files == 1 && ps.decoded == 1 && ps.bytes == 64 * 64 * 4 && !ps.full,
               "one PNG preloaded");
        Machine first;
        RunUntil(r, lb, &first, [&](const Machine &mm) { return mm.Pixel(100, 100) == kRed; });
        ps = r.set(index).preload_stats();
        expect(ps.used == 1 && ps.bytes == 0, "the load took the preloaded image");

        auto small = tr::ReplacementSet::Load(dir, &error);
        tr::TextureReplacer r2;
        r2.StartPreload(16, 1);
        const int i2 = r2.AddSet(std::move(small));
        expect(r2.set(i2).WaitIdle(std::chrono::seconds(10)), "the small preload finishes");
        ps = r2.set(i2).preload_stats();
        expect(ps.decoded == 0 && ps.full && ps.bytes == 0, "over the budget: nothing kept");
    }

    // Mip levels named by key on disk (<key>_1.png), as PPSSPP scans them.
    {
        const std::filesystem::path named = FreshDir("mpo_texreplace_test_named_levels");
        const std::string base = tr::HashName(key);
        expect(tr::SavePng(named / (base + ".png"), 16, 16, SolidImage(16, 16, kRed), &error) &&
                   tr::SavePng(named / (base + "_1.png"), 8, 8, SolidImage(8, 8, kGreen), &error) &&
                   tr::SavePng(named / (base + "_3.png"), 2, 2, SolidImage(2, 2, kGreen), &error),
               "write the levels: " + error);
        std::ofstream(named / "textures.ini", std::ios::binary)
            << "[options]\nhash = xxh64\nignoreAddress = true\n";
        auto set = tr::ReplacementSet::Load(named, &error);
        expect(set != nullptr &&
                   set->Levels(base + ".png") == std::vector<std::string>{base + "_1.png"},
               "<key>_1.png is level 1 (and _3 after a gap is not)");
        expect(set != nullptr && set->ini().warnings.size() == 1, "the gap is reported");
        if (set) {
            tr::TextureReplacer r;
            r.SetActiveSet(r.AddSet(std::move(set)));
            ListBuilder small;
            small.Clear(0xFF402010);
            DrawRgba(small, 100, 100, 8);
            Machine m;
            m.ge.SetTextureProvider(&r);
            expect(m.Run(small) && m.Pixel(101, 101) == kGreen, "and softgpu samples it");
        }
    }

    // The reload: in place, at the next run, keeping the active set.
    {
        auto set = tr::ReplacementSet::Load(dir, &error);
        tr::TextureReplacer r;
        int logged = 0;
        r.SetLog([&](std::string_view) { ++logged; });
        tr::TexturesIni ini;
        auto other = std::make_unique<tr::ReplacementSet>("in-memory", ini);
        r.AddSet(std::move(other)); // index 0: not from a folder, never reloaded
        const int index = r.AddSet(std::move(set));
        r.SetActiveSet(index);
        Machine m;
        m.ge.SetTextureProvider(&r);
        expect(m.Run(lb) && m.Pixel(100, 100) == kRed, "before the reload");
        expect(tr::SavePng(dir / "b.png", 32, 32, SolidImage(32, 32, kGreen), &error),
               "write b.png");
        std::ofstream(dir / "textures.ini", std::ios::binary)
            << "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n"
            << tr::HashName(key) << " = b.png\n";
        Machine stale;
        stale.ge.SetTextureProvider(&r);
        expect(stale.Run(lb) && stale.Pixel(100, 100) == kRed, "nothing changes until asked");
        r.RequestReload();
        Machine fresh;
        fresh.ge.SetTextureProvider(&r);
        expect(fresh.Run(lb) && fresh.Pixel(100, 100) == kGreen, "after the reload: the new ini");
        expect(r.stats().reloads == 1 && r.active_set() == index && r.set_count() == 2,
               "one pack reloaded in place, still active");
        // A broken textures.ini: reported, and the pack before stays.
        std::ofstream(dir / "textures.ini", std::ios::binary) << "[options]\nhash = quick\n";
        r.RequestReload();
        Machine kept;
        kept.ge.SetTextureProvider(&r);
        expect(kept.Run(lb) && kept.Pixel(100, 100) == kGreen && r.stats().reload_errors == 1,
               "a broken reload keeps the pack in use");
        expect(logged >= 2, "the reloads are reported");

        // The folder watch: a change in the folder reloads it.
        std::ofstream(dir / "textures.ini", std::ios::binary)
            << "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n"
            << tr::HashName(key) << " = a.png\n";
        r.RequestReload();
        Machine red_again;
        red_again.ge.SetTextureProvider(&r);
        expect(red_again.Run(lb) && red_again.Pixel(100, 100) == kRed, "reloaded once more");
        r.WatchFolders(std::chrono::milliseconds(20));
        expect(tr::SavePng(dir / "c.png", 16, 16, SolidImage(16, 16, kGreen), &error),
               "write c.png");
        std::ofstream(dir / "textures.ini", std::ios::binary)
            << "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n"
            << tr::HashName(key) << " = c.png\n";
        Machine watched;
        expect(RunUntil(r, lb, &watched,
                        [&](const Machine &mm) { return mm.Pixel(100, 100) == kGreen; }),
               "the watch reloads the changed pack");
        r.WatchFolders(std::chrono::milliseconds(0));
    }
}

} // namespace

int main() {
    TestKey();
    TestIni();
    TestLookup();
    TestFilteringLookup();
    TestBuild();
    TestScale();
    TestSoftgpu();
    TestVram();
    TestSoftgpuMips();
    TestFiltering();
    TestAsync();
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_texreplace_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_texreplace_test: all checks passed\n");
    return 0;
}
