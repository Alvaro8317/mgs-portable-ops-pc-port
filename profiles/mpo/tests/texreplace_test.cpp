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
// - the dump: a PNG per key and a textures.ini, which then work as a pack;
// - F26b: the dump's dump.csv (parse, write, add up over sessions, the stage,
//   RAM and VRAM, framebuffers left out, the periodic save).
// PNG files are written and read through SDL3 (png_io.hpp); without it,
// those checks are skipped (and said so).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "dump_index.hpp"
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
        expect(ini.hashes[1].file == "a/b.png", "entry 2: backslashes, first mip level");
        expect(ini.hashes[2].file.empty(), "entry 3: listed, not replaced");
        expect(ini.hashes[3].file == "with#hash.png", "entry 4: escaped #");
    }
    expect(ini.warnings.size() == 3,
           "warnings for [games], [hashranges] and levels: " + std::to_string(ini.warnings.size()));

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

// ---- F26b: dump.csv ----

std::string ReadText(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void TestDumpIndex() {
    // The stage of a module's path.
    expect(tr::DumpIndex::StageFromModulePath("disc0:/PSP_GAME/USRDIR/stage/st04vo/st04vo.prx") ==
               "st04vo",
           "stage from a disc0 path");
    expect(tr::DumpIndex::StageFromModulePath("umd0:stage/init/init.prx") == "init",
           "stage after a device name");
    expect(tr::DumpIndex::StageFromModulePath("C:\\game\\stage\\r_main01\\r_main01.prx") ==
               "r_main01",
           "stage from a Windows path");
    expect(tr::DumpIndex::StageFromModulePath("disc0:/PSP_GAME/USRDIR/modules/kjfs.prx").empty(),
           "a module that is not a stage");
    expect(tr::DumpIndex::StageFromModulePath("disc0:/backstage/x/y.prx").empty(),
           "\"stage/\" must be a whole folder name");
    expect(tr::DumpIndex::StageFromModulePath("disc0:/stage/st04vo").empty(),
           "a stage folder without a file");
    expect(tr::DumpIndex::StageFromModulePath("disc0:/stage/a b,c/x.prx").empty(),
           "a name with other characters is not a stage name");
    expect(tr::DumpIndex::StageFromModulePath("disc0:/stage/old/stage/st02/st02.prx") == "st02",
           "the last stage/ folder");

    // Note: sizes, format, memory, first stage, stages, uses and frames.
    const tr::ReplacementKey a{0x0000000009f357c0ull, 0x0cdcc7e2u};
    const tr::ReplacementKey b{0x00000000000a0104ull, 0x12345678u};
    tr::DumpIndex index;
    index.Note(a, 32, 16, mpo::ge::TexFormat::kClut4, tr::DumpIndex::kRam, "", 0);
    index.Note(a, 32, 16, mpo::ge::TexFormat::kClut4, tr::DumpIndex::kRam, "st04vo", 0);
    index.Note(a, 32, 16, mpo::ge::TexFormat::kClut4, tr::DumpIndex::kVram, "st04vo", 1);
    index.Note(a, 32, 16, mpo::ge::TexFormat::kClut4, tr::DumpIndex::kRam, "briefing", 1);
    index.Note(a, 32, 16, mpo::ge::TexFormat::kClut4, tr::DumpIndex::kRam, "st04vo", 2);
    index.Note(b, 128, 64, mpo::ge::TexFormat::kDxt5, tr::DumpIndex::kVram, "st02", 7);
    expect(index.size() == 2 && index.dirty(), "two keys, not written yet");
    const tr::DumpIndex::Entry *ea = index.Find(a);
    expect(ea != nullptr && ea->uses == 5 && ea->frames == 3, "uses count draws, frames flips");
    expect(ea != nullptr && ea->first_stage.empty(), "first drawn before any stage");
    expect(ea != nullptr && ea->stages == std::vector<std::string>{"st04vo", "briefing"},
           "the stages in the order they were met");
    expect(ea != nullptr && ea->memory == (tr::DumpIndex::kRam | tr::DumpIndex::kVram),
           "RAM and VRAM");
    const std::string text = index.Serialize();
    const std::string expected =
        "key,width,height,format,memory,first_stage,stages,uses,frames\n"
        "00000000000a010412345678,128,64,DXT5,vram,st02,st02,1,1\n"
        "0000000009f357c00cdcc7e2,32,16,CLUT4,ram+vram,,st04vo;briefing,5,3\n";
    expect(text == expected, "the file text:\n" + text);

    // Parse gives the same file back; Save and Load too.
    tr::DumpIndex back;
    std::string error;
    expect(back.Parse(text, &error), "the file parses: " + error);
    expect(back.Serialize() == text && !back.dirty(), "and serializes the same");
    expect(back.Parse("\xEF\xBB\xBF" + text, &error) == false, "a BOM is not the header");
    expect(back.Parse(text, &error), "parses again");
    std::string crlf;
    for (char c : text) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    tr::DumpIndex from_crlf;
    expect(from_crlf.Parse(crlf, &error) && from_crlf.Serialize() == text, "CRLF lines");
    const std::filesystem::path dir = FreshDir("mpo_texreplace_test_index");
    tr::DumpIndex loaded;
    expect(loaded.Load(dir, &error) && loaded.size() == 0, "no dump.csv: an empty index");
    expect(index.Save(dir, &error) && !index.dirty(), "saved: " + error);
    expect(!std::filesystem::exists(dir / "dump.csv.tmp"), "no temporary file left");
    expect(ReadText(dir / "dump.csv") == text, "the file on disk");
    expect(loaded.Load(dir, &error) && loaded.Serialize() == text, "loaded back: " + error);
    // Adding up: another session over the loaded rows.
    loaded.Note(a, 32, 16, mpo::ge::TexFormat::kClut4, tr::DumpIndex::kRam, "st02", 0);
    loaded.Note(a, 32, 16, mpo::ge::TexFormat::kClut4, tr::DumpIndex::kRam, "st02", 0);
    const tr::DumpIndex::Entry *la = loaded.Find(a);
    expect(la != nullptr && la->uses == 7 && la->frames == 4 && la->first_stage.empty() &&
               la->stages.size() == 3 && la->stages[2] == "st02",
           "a later session adds its uses, frames and stages");

    // Untrusted input: every malformed file is refused.
    const std::string header = "key,width,height,format,memory,first_stage,stages,uses,frames\n";
    const char *bad_rows[] = {
        "0000000009f357c00cdcc7e2,32,16,CLUT4,ram,,,1\n",             // 8 fields
        "0000000009F357C00CDCC7E2,32,16,CLUT4,ram,,,1,1\n",           // upper case
        "0000000009f357c00cdcc7e,32,16,CLUT4,ram,,,1,1\n",            // 23 digits
        "0000000009f357c00cdcc7e2,0,16,CLUT4,ram,,,1,1\n",            // width 0
        "0000000009f357c00cdcc7e2,32,4097,CLUT4,ram,,,1,1\n",         // too tall
        "0000000009f357c00cdcc7e2,32,16,CLUT2,ram,,,1,1\n",           // format
        "0000000009f357c00cdcc7e2,32,16,CLUT4,edram,,,1,1\n",         // memory
        "0000000009f357c00cdcc7e2,32,16,CLUT4,ram,a b,,1,1\n",        // stage
        "0000000009f357c00cdcc7e2,32,16,CLUT4,ram,,st02;;st03,1,1\n", // empty stage
        "0000000009f357c00cdcc7e2,32,16,CLUT4,ram,,st02;st02,1,1\n",  // twice
        "0000000009f357c00cdcc7e2,32,16,CLUT4,ram,,,-1,1\n",          // negative
        "0000000009f357c00cdcc7e2,32,16,CLUT4,ram,,,99999999999999999999,1\n",
        ("0000000009f357c00cdcc7e2,32,16,CLUT4,ram,,,1,1\n"
         "0000000009f357c00cdcc7e2,32,16,CLUT4,ram,,,1,1\n"), // the same key twice
    };
    for (const char *row : bad_rows) {
        tr::DumpIndex bad;
        bad.Note(a, 1, 1, mpo::ge::TexFormat::k8888, tr::DumpIndex::kRam, "", 0);
        expect(!bad.Parse(header + row, &error), std::string("refused: ") + row);
        expect(bad.size() == 1, "a refused file leaves the index as it was");
    }
    tr::DumpIndex bad;
    expect(!bad.Parse("key,width\n", &error), "another header is refused");
    expect(bad.Parse("", &error) && bad.size() == 0, "an empty file is an empty index");
    std::ofstream(dir / "dump.csv", std::ios::binary) << "garbage\n";
    expect(!bad.Load(dir, &error) && error.find("dump.csv") != std::string::npos,
           "Load names the file it refused: " + error);
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
        dumper.SetStageSource([] { return std::string("st04vo"); });
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
        // F26b: dump.csv, written on FlushDump.
        expect(!std::filesystem::exists(dump / "dump.csv"), "dump.csv waits for a flush");
        expect(dumper.FlushDump(&error), "flush: " + error);
        tr::DumpIndex first;
        expect(first.Load(dump, &error) && first.size() == 2, "dump.csv has the two keys");
        const tr::DumpIndex::Entry *ie = first.Find(icon);
        expect(ie != nullptr && ie->width == 32 && ie->height == 16 && ie->format == "CLUT4" &&
                   ie->memory == tr::DumpIndex::kRam && ie->first_stage == "st04vo" &&
                   ie->stages == std::vector<std::string>{"st04vo"} && ie->uses == 2 &&
                   ie->frames == 1,
               "the icon's row: 32x16 CLUT4 in RAM, st04vo, 2 draws in 1 frame:\n" +
                   ReadText(dump / "dump.csv"));
        const tr::DumpIndex::Entry *re = first.Find(rgba_key);
        expect(re != nullptr && re->width == 16 && re->height == 16 && re->format == "8888" &&
                   re->uses == 1,
               "the 8888 texture's row");
        Machine again;
        {
            tr::TextureReplacer dumper2;
            expect(dumper2.SetDumpDir(dump, &error), "dump dir again");
            expect(dumper2.dump_index().size() == 2, "the second session starts from dump.csv");
            // A stage source with a name that is not a stage name: none.
            std::string stage = "bad name";
            dumper2.SetStageSource([&stage] { return stage; });
            again.ge.SetTextureProvider(&dumper2);
            again.Run(lb);
            dumper2.Flip(nullptr);
            stage = "briefing";
            again.Run(lb);
            expect(dumper2.stats().dumped == 0, "keys already dumped are not written again");
            again.ge.SetTextureProvider(nullptr);
        } // written when the replacer goes away
        tr::DumpIndex second;
        expect(second.Load(dump, &error) && second.size() == 2, "still two keys");
        ie = second.Find(icon);
        expect(ie != nullptr && ie->uses == 6 && ie->frames == 3 && ie->first_stage == "st04vo" &&
                   ie->stages == std::vector<std::string>{"st04vo", "briefing"},
               "the sessions add up:\n" + ReadText(dump / "dump.csv"));
        // The periodic save, every kDumpSaveFlips flips while it changes.
        const std::filesystem::path periodic = FreshDir("mpo_texreplace_test_periodic");
        tr::TextureReplacer dumper3;
        expect(dumper3.SetDumpDir(periodic, &error), "periodic dump dir");
        Machine pm;
        pm.ge.SetTextureProvider(&dumper3);
        pm.Run(lb);
        for (std::uint64_t i = 1; i < tr::TextureReplacer::kDumpSaveFlips; ++i)
            dumper3.Flip(nullptr);
        expect(!std::filesystem::exists(periodic / "dump.csv"), "not before kDumpSaveFlips");
        dumper3.Flip(nullptr);
        expect(std::filesystem::exists(periodic / "dump.csv"), "written after kDumpSaveFlips");
        // A dump.csv it cannot read: the dump does not start.
        const std::filesystem::path broken_csv = FreshDir("mpo_texreplace_test_badcsv");
        std::ofstream(broken_csv / "dump.csv", std::ios::binary) << "not,a,dump\n";
        tr::TextureReplacer dumper4;
        expect(!dumper4.SetDumpDir(broken_csv, &error), "a bad dump.csv stops the dump");
        expect(ReadText(broken_csv / "dump.csv") == "not,a,dump\n", "and is left as it was");
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
    // F26b: dump.csv says where the texels were: VRAM, then RAM too.
    if (tr::HavePng()) {
        const std::filesystem::path dir = FreshDir("mpo_texreplace_test_vram_dump");
        std::string error;
        {
            tr::TextureReplacer dumper;
            expect(dumper.SetDumpDir(dir, &error), "VRAM dump dir: " + error);
            Machine m;
            m.CopyToVram(kVramTex, kRgbaAddr);
            m.ge.SetTextureProvider(&dumper);
            expect(m.Run(vram_list), "VRAM dump list runs");
            expect(dumper.dump_index().Find(kv) != nullptr &&
                       dumper.dump_index().Find(kv)->memory == tr::DumpIndex::kVram,
                   "dumped from VRAM");
            Machine r;
            r.ge.SetTextureProvider(&dumper);
            expect(r.Run(ram_list), "RAM dump list runs");
            const tr::DumpIndex::Entry *e = dumper.dump_index().Find(kv);
            expect(e != nullptr && e->memory == (tr::DumpIndex::kRam | tr::DumpIndex::kVram) &&
                       e->uses == 2,
                   "the same key from RAM too");
        }
        tr::DumpIndex back;
        expect(back.Load(dir, &error) && back.size() == 1 &&
                   ReadText(dir / "dump.csv").find(",ram+vram,") != std::string::npos,
               "ram+vram in the file:\n" + ReadText(dir / "dump.csv"));
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

} // namespace

int main() {
    TestKey();
    TestIni();
    TestLookup();
    TestScale();
    TestDumpIndex();
    TestSoftgpu();
    TestVram();
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_texreplace_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_texreplace_test: all checks passed\n");
    return 0;
}
