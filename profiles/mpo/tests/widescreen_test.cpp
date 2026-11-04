// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_widescreen (F25b), without the game:
// - --aspect: the forms it takes, native up to 16:9, the limits;
// - the x scale's float bits, against tools/widescreen.py value;
// - the profile: the built-in widescreen.toml is the file in config/, its
//   module is the decrypted mgp_main of supported.toml, and broken files are
//   refused;
// - the patch on a synthetic PRX: only the constant changes, and a wrong
//   SHA-256, a constant that is not the original or an address outside the
//   code segment leave the module as it was;
// - which flips are video frames.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "util/sha256.hpp"
#include "widescreen.hpp"

namespace {

namespace ws = mpo::widescreen;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void test_parse() {
    struct Case {
        const char *text;
        std::uint32_t w;
        std::uint32_t h;
    };
    const Case good[] = {
        {"native", 0, 0},      {"16:9", 0, 0},        {"480:272", 0, 0},     {"30:17", 0, 0},
        {"21:9", 7, 3},        {"2560:1080", 64, 27}, {"2560x1080", 64, 27}, {"2560/1080", 64, 27},
        {"3440x1440", 43, 18}, {"32:9", 32, 9},       {"4:1", 4, 1},         {"2.37", 237, 100},
        {"2.5", 5, 2},         {"3", 3, 1},           {"1.7777", 0, 0},      {"1.7778", 8889, 5000},
    };
    for (const Case &c : good) {
        std::string error;
        const auto aspect = ws::ParseAspect(c.text, &error);
        expect(aspect && aspect->width == c.w && aspect->height == c.h,
               std::string("parse ") + c.text + " " + error);
    }
    const char *bad[] = {"",      "wide",  "21:",       ":9",     "0:9",   "21:0", "-21:9",
                         "21:9x", "16:10", "4:3",       "1",      "41:10", "5:1",  "2.",
                         ".5",    "2.3.4", "1234567:1", "21 : 9", "Native"};
    for (const char *text : bad) {
        std::string error;
        expect(!ws::ParseAspect(text, &error) && !error.empty(),
               std::string("refused: '") + text + "'");
    }
    expect(ws::Aspect{}.native() && ws::Aspect{}.text() == "native", "native text");
    expect(ws::Aspect{64, 27}.text() == "64:27", "w:h text");
    expect(ws::Aspect{}.ratio() > 1.76 && ws::Aspect{}.ratio() < 1.77, "native ratio");
}

void test_bits() {
    // profiles/mpo/tools/widescreen.py value (docs/analysis/widescreen.md §2.1).
    expect(ws::XScaleBits({}) == 0x3F800000u, "native is 1.0");
    expect(ws::XScaleBits({64, 27}) == 0x3F3E9697u, "2560x1080 is 0x3F3E9697");
    expect(ws::XScaleBits({32, 9}) == 0x3EFE1E1Eu, "32:9 is 0x3EFE1E1E");
    expect(ws::XScaleBits({7, 3}) == 0x3F419D0Bu, "21:9 is 0x3F419D0B");
    expect(ws::XScaleBits({43, 18}) == 0x3F3D1C5Eu, "3440x1440 is 0x3F3D1C5E");
    expect(ws::XScaleBits({4, 1}) == 0x3EE1E1E2u, "4:1 is 0x3EE1E1E2");
    expect(ws::BitsToFloat(0x3F800000u) == 1.0f, "bits to float");
}

void test_profile() {
    const std::string file = read_file(MPO_WIDESCREEN_CONFIG);
    expect(!file.empty() && ws::Profile::BuiltinText() == file,
           "the built-in profile is config/widescreen.toml");
    const ws::Profile &profile = ws::Profile::Builtin();
    expect(profile.modules.size() == 1u, "one module");
    if (profile.modules.size() == 1u) {
        const ws::ModuleProfile &m = profile.modules[0];
        expect(m.name == "mgp_main" && m.disc_id == "ULUS10202" && m.disc_version == "1.02",
               "mgp_main of ULUS-10202 1.02");
        expect(m.image_base == 0x08804000u && m.x_scale_address == 0x089E9728u &&
                   m.original == 0x3F800000u,
               "the spike's address and value");
        // The same module the installer decrypts and checks.
        const std::string supported = read_file(MPO_SUPPORTED_CONFIG);
        const std::size_t line = supported.find("module = \"mgp_main\"");
        const std::size_t end = supported.find('\n', line);
        expect(line != std::string::npos &&
                   supported.substr(line, end - line).find("sha256 = \"" + m.sha256 + "\"") !=
                       std::string::npos,
               "the SHA-256 is supported.toml's decrypted mgp_main");
    }
    const std::string bad[] = {
        "",
        "x = 1",
        "module = 1",
        "[[module]]\nname = \"m\"\n",
        "[[module]]\nname = \"m\"\nsha256 = \"AB\"\nimage_base = 0\nx_scale_address = 4\n"
        "original = 0\n",
        "[[module]]\nname = \"m\"\nsha256 = \"" + std::string(64, 'a') +
            "\"\nimage_base = 0x100\nx_scale_address = 0x10\noriginal = 0\n",
        "[[module]]\nname = \"m\"\nsha256 = \"" + std::string(64, 'a') +
            "\"\nimage_base = 0\nx_scale_address = 0x11\noriginal = 0\n",
        "[[module]]\nname = \"m\"\nsha256 = \"" + std::string(64, 'a') +
            "\"\nimage_base = 0\nx_scale_address = 0x10\noriginal = -1\n",
        "[[module]\n",
    };
    for (const std::string &text : bad) {
        std::string error;
        expect(!ws::Profile::Parse(text, &error) && !error.empty(), "bad profile: " + text);
    }
}

void put32(std::vector<std::uint8_t> &d, std::size_t at, std::uint32_t v) {
    std::memcpy(d.data() + at, &v, 4u);
}
void put16(std::vector<std::uint8_t> &d, std::size_t at, std::uint16_t v) {
    std::memcpy(d.data() + at, &v, 2u);
}
std::uint32_t get32(const std::vector<std::uint8_t> &d, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + at, 4u);
    return v;
}

// A PRX with one LOAD segment of `size` bytes at file offset 0x100.
std::vector<std::uint8_t> synthetic_prx(std::uint32_t size) {
    std::vector<std::uint8_t> d(0x100u + size, 0u);
    const std::uint8_t ident[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(d.data(), ident, sizeof ident);
    put16(d, 16, 0xFFA0u); // e_type
    put16(d, 18, 8u);      // e_machine: MIPS
    put32(d, 28, 52u);     // e_phoff
    put16(d, 42, 32u);     // e_phentsize
    put16(d, 44, 1u);      // e_phnum
    put32(d, 52, 1u);      // p_type: LOAD
    put32(d, 56, 0x100u);  // p_offset
    put32(d, 60, 0u);      // p_vaddr
    put32(d, 68, size);    // p_filesz
    for (std::uint32_t i = 0; i < size; i += 4u) put32(d, 0x100u + i, 0x01000000u + i);
    return d;
}

ws::Profile profile_for(const std::vector<std::uint8_t> &elf, std::uint32_t address,
                        std::uint32_t original) {
    ws::Profile p;
    ws::ModuleProfile m;
    m.name = "test";
    m.sha256 = mpo::sha256_hex(elf);
    m.image_base = 0x08804000u;
    m.x_scale_address = address;
    m.original = original;
    p.modules.push_back(m);
    return p;
}

void test_patch() {
    std::vector<std::uint8_t> elf = synthetic_prx(0x400u);
    put32(elf, 0x100u + 0x328u, 0x3F800000u);
    const std::vector<std::uint8_t> before = elf;
    const ws::Profile profile = profile_for(elf, 0x08804328u, 0x3F800000u);
    std::string error;
    const auto patched = ws::PatchModule(elf, profile, 0x3F3E9697u, &error);
    expect(patched && patched->file_offset == 0x428u && patched->module->name == "test",
           "patched at file offset 0x428: " + error);
    expect(get32(elf, 0x428u) == 0x3F3E9697u, "the new value");
    std::size_t changed = 0;
    for (std::size_t i = 0; i < elf.size(); ++i) changed += elf[i] != before[i] ? 1u : 0u;
    expect(changed <= 4u, "only the constant changed");

    // The module patched once is no longer the profile's (another SHA-256).
    std::vector<std::uint8_t> again = elf;
    expect(!ws::PatchModule(again, profile, 0x3F3E9697u, &error) && again == elf &&
               error.find("SHA-256") != std::string::npos,
           "a module of another SHA-256 is refused: " + error);
    // The right SHA-256 but not the original value there.
    std::vector<std::uint8_t> other = before;
    const ws::Profile wrong_value = profile_for(other, 0x08804328u, 0x40000000u);
    expect(!ws::PatchModule(other, wrong_value, 0x3F3E9697u, &error) && other == before &&
               error.find("not 0x40000000") != std::string::npos,
           "not the original value: " + error);
    // Outside the code segment.
    const ws::Profile outside = profile_for(other, 0x08804400u, 0x3F800000u);
    expect(!ws::PatchModule(other, outside, 0x3F3E9697u, &error) && other == before &&
               error.find("outside") != std::string::npos,
           "outside the segment: " + error);
    // Not a PRX (a plain executable).
    std::vector<std::uint8_t> exec = before;
    put16(exec, 16, 2u);
    const std::vector<std::uint8_t> exec_before = exec;
    const ws::Profile exec_profile = profile_for(exec, 0x08804328u, 0x3F800000u);
    expect(!ws::PatchModule(exec, exec_profile, 0x3F3E9697u, &error) && exec == exec_before &&
               error.find("PRX") != std::string::npos,
           "not a PRX: " + error);
    // Truncated: the segment goes past the end of the file.
    std::vector<std::uint8_t> cut(before.begin(), before.begin() + 0x200);
    const std::vector<std::uint8_t> cut_before = cut;
    const ws::Profile cut_profile = profile_for(cut, 0x08804028u, 0x01000028u);
    expect(!ws::PatchModule(cut, cut_profile, 0x3F3E9697u, &error) && cut == cut_before,
           "a truncated module is refused: " + error);
    // Not an ELF at all.
    std::vector<std::uint8_t> text(64, 'x');
    const ws::Profile text_profile = profile_for(text, 0x08804000u, 0u);
    expect(!ws::PatchModule(text, text_profile, 0u, &error) &&
               error.find("ELF") != std::string::npos,
           "not an ELF: " + error);
    // The built-in profile refuses a module it does not know.
    std::vector<std::uint8_t> unknown = before;
    expect(!ws::PatchModule(unknown, ws::Profile::Builtin(), 0x3F3E9697u, &error) &&
               unknown == before,
           "the built-in profile refuses an unknown module");
}

void test_video_rule() {
    ws::VideoFrameRule rule(3);
    expect(!rule.OnFlip(0) && !rule.OnFlip(0), "no video yet");
    expect(rule.OnFlip(1), "a decoded frame");
    expect(rule.OnFlip(1) && rule.OnFlip(1), "held for 3 flips");
    expect(!rule.OnFlip(1), "then the GE's again");
    // A 30 fps video at 60 flips per second: every other flip.
    ws::VideoFrameRule playing(3);
    bool all = true;
    for (std::uint64_t flip = 0; flip < 20; ++flip) all = playing.OnFlip(2 + flip / 2) && all;
    expect(all, "a playing video stays video");
    ws::VideoFrameRule defaults;
    std::uint64_t video = 0;
    (void)defaults.OnFlip(5);
    for (int i = 0; i < 20; ++i) video += defaults.OnFlip(5) ? 1u : 0u;
    expect(video == ws::VideoFrameRule::kDefaultHold - 1u, "the default hold");
}

} // namespace

int main() {
    test_parse();
    test_bits();
    test_profile();
    test_patch();
    test_video_rule();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_widescreen_test: all passed\n");
    return 0;
}
