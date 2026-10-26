// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for mpo_ge (GE registers, list interpreter, software renderer) and
// mpo_ge_replay (.ppdmp reader and replayer, PNG writer). Everything is
// synthetic: display lists, textures, palettes and frame dumps are built
// here. No game data is used (CLAUDE.md §1.1).

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "color.hpp"
#include "dump_replayer.hpp"
#include "ge.hpp"
#include "image.hpp"
#include "memory_view.hpp"
#include "ppdmp.hpp"
#include "vertex_decoder.hpp"

namespace {

using namespace mpo::ge;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

std::uint32_t op(std::uint8_t cmd, std::uint32_t data) {
    return (static_cast<std::uint32_t>(cmd) << 24) | (data & 0xFFFFFF);
}

std::uint32_t float24(float f) {
    return std::bit_cast<std::uint32_t>(f) >> 8;
}

void put16(std::vector<std::uint8_t> &out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// A PSP-like memory: 8 MiB of RAM at 0x08000000 and the 2 MiB of VRAM.
struct Machine {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(8u << 20, 0);
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(MemoryView::kVramSize, 0);
    MemoryView mem{ram, 0x08000000, vram};

    void write(std::uint32_t addr, const std::vector<std::uint8_t> &bytes) {
        expect(mem.CopyIn(addr, bytes), "machine write at " + hex(addr));
    }
    void write_words(std::uint32_t addr, const std::vector<std::uint32_t> &words) {
        for (std::size_t i = 0; i < words.size(); ++i) {
            mem.Write32(addr + static_cast<std::uint32_t>(i * 4), words[i]);
        }
    }
    std::uint16_t fb16(int x, int y, std::uint32_t base = 0, int stride = 512) const {
        return mem.VramRead16(base + static_cast<std::uint32_t>((y * stride + x) * 2));
    }
};

constexpr std::uint32_t kListAddr = 0x08100000;
constexpr std::uint32_t kVertexAddr = 0x08200000;
constexpr std::uint32_t kTextureAddr = 0x08300000;
constexpr std::uint32_t kClutAddr = 0x08380000;

// Through-mode 480x272 5551 target at VRAM 0, depth at 0x88000.
std::vector<std::uint32_t> screen_setup() {
    return {
        op(kCmdFrameBufPtr, 0x000000),
        op(kCmdFrameBufWidth, 512),
        op(kCmdFrameBufPixFormat, 1),
        op(kCmdZBufPtr, 0x088000),
        op(kCmdZBufWidth, 512),
        op(kCmdScissor1, 0),
        op(kCmdScissor2, (271u << 10) | 479u),
        op(kCmdRegion1, 0),
        op(kCmdRegion2, (271u << 10) | 479u),
        op(kCmdShadeMode, 1),
    };
}

RunResult run_list(Ge &ge, Machine &m, std::vector<std::uint32_t> words) {
    words.push_back(op(kCmdFinish, 0));
    words.push_back(op(kCmdEnd, 0));
    m.write_words(kListAddr, words);
    DisplayList list;
    list.pc = kListAddr;
    return ge.Run(list);
}

// vtype 0x80011C: color 8888 at 0, s16 position at 4, stride 12.
void vertex_c(std::vector<std::uint8_t> &out, std::uint32_t color, int x, int y, int z) {
    put32(out, color);
    put16(out, static_cast<std::uint16_t>(x));
    put16(out, static_cast<std::uint16_t>(y));
    put16(out, static_cast<std::uint16_t>(z));
    put16(out, 0);
}

// vtype 0x80011E: u16 uv at 0, color at 4, s16 position at 8, stride 16.
void vertex_tc(std::vector<std::uint8_t> &out, int u, int v, std::uint32_t color, int x, int y) {
    put16(out, static_cast<std::uint16_t>(u));
    put16(out, static_cast<std::uint16_t>(v));
    put32(out, color);
    put16(out, static_cast<std::uint16_t>(x));
    put16(out, static_cast<std::uint16_t>(y));
    put16(out, 0);
    put16(out, 0);
}

// Swizzles a texture of `pitch_bytes` x `rows` into the PSP layout: 16-byte x
// 8-row blocks, stored block after block, row-major (independent of the
// renderer's own offset formula).
std::vector<std::uint8_t> swizzle(const std::vector<std::uint8_t> &linear, int pitch_bytes,
                                  int rows) {
    std::vector<std::uint8_t> out(linear.size());
    const int blocks_per_row = pitch_bytes / 16;
    for (int y = 0; y < rows; ++y) {
        for (int bx = 0; bx < pitch_bytes; ++bx) {
            const int block = (y / 8) * blocks_per_row + bx / 16;
            const int within = (y % 8) * 16 + bx % 16;
            out[static_cast<std::size_t>(block * 128 + within)] =
                linear[static_cast<std::size_t>(y * pitch_bytes + bx)];
        }
    }
    return out;
}

std::uint16_t to5551(std::uint32_t rgba) {
    return RGBA8888ToRGBA5551(rgba);
}

void test_vertex_sizes() {
    // Expected values from profiles/mpo/tools/ge_usage.py vertex_size(),
    // which matched every draw of real PPSSPP dumps (F05).
    const std::uint32_t cases[][2] = {
        {0x80011E, 16}, {0x80011C, 12}, {0x00011E, 16}, {0x00015E, 20}, {0x004342, 18},
        {0x00019E, 20}, {0x00C342, 20}, {0x00115E, 20}, {0x01C342, 24}, {0x005342, 18},
        {0x800180, 12}, {0x000081, 5},  {0x0000A5, 8},  {0x000106, 10}, {0x1C0182, 128},
        {0x007E01, 16}, {0x00023F, 24}, {0x00065D, 24}, {0x000380, 16}, {0x02C100, 6},
    };
    for (const auto &c : cases) {
        expect(VertexSize(c[0]) == c[1], "vertex size of " + hex(c[0]) + " is " +
                                             std::to_string(VertexSize(c[0])) + ", expected " +
                                             std::to_string(c[1]));
    }
    // Decoding: through-mode s16 position keeps z unsigned; 5551 colour.
    Machine m;
    std::vector<std::uint8_t> v;
    put16(v, 0xFC1F); // color 5551 at 0 (r=31, g=0, b=31, a=1)
    put16(v, static_cast<std::uint16_t>(-5));
    put16(v, 7);
    put16(v, 0xFFFE);
    m.write(kVertexAddr, v);
    const VertexDecoder dec(0x800000 | (5u << 2) | (2u << 7));
    expect(dec.stride() == 8, "5551+s16 stride");
    DecodedVertex out;
    expect(dec.Decode(m.mem, kVertexAddr, DecodeParams{}, &out), "decode");
    expect(out.pos[0] == -5.0f && out.pos[1] == 7.0f && out.pos[2] == 65534.0f,
           "through s16 position");
    expect(out.color0 == 0xFFFF00FFu, "5551 vertex colour " + hex(out.color0));
}

void test_memory_view() {
    Machine m;
    m.mem.Write32(0x04000010, 0xA1B2C3D4);
    expect(m.mem.Read32(0x44000010) == 0xA1B2C3D4, "uncached VRAM alias");
    expect(m.mem.Read32(0x04200010) == 0xA1B2C3D4, "VRAM mirror at +2 MiB");
    expect(m.mem.Read32(0x04600010) == 0xA1B2C3D4, "VRAM mirror at +6 MiB");
    expect(!m.mem.IsValidRange(0x04800000, 4), "VRAM ends at 0x04800000");
    expect(!m.mem.IsValidRange(0x041FFFFE, 4), "range crossing a VRAM mirror");
    m.mem.Write32(0x08001000, 0x11223344);
    expect(m.mem.Read32(0x48001000) == 0x11223344, "uncached RAM alias");
    expect(m.mem.Read32(0x88001000) == 0x11223344, "kernel RAM alias");
    expect(!m.mem.IsValidRange(0x08800000, 1), "beyond RAM");
    expect(m.mem.Read32(0x00000000) == 0, "invalid read returns 0");
    expect(m.mem.ClampValidSize(0x087FFFF0, 64) == 16, "clamp valid size");
    expect(m.mem.VramRead16(MemoryView::kVramSize + 4) == m.mem.VramRead16(4), "VRAM wrap");
}

void test_clear() {
    Machine m;
    Ge ge(m.mem);
    std::vector<std::uint8_t> v;
    vertex_c(v, 0xFF0000FF, 0, 0, 0x1234);
    vertex_c(v, 0xFF0000FF, 480, 272, 0x1234);
    m.write(kVertexAddr, v);
    auto words = screen_setup();
    words.insert(words.end(), {
                                  op(kCmdBase, 0x080000),
                                  op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
                                  op(kCmdVertexType, 0x80011C),
                                  op(kCmdClearMode, 0x701),
                                  op(kCmdPrim, (6u << 16) | 2),
                                  op(kCmdClearMode, 0),
                              });
    const RunResult r = run_list(ge, m, words);
    expect(r.status == RunStatus::kFinished, "clear list finishes");
    expect(m.fb16(0, 0) == 0x801F && m.fb16(479, 271) == 0x801F && m.fb16(200, 100) == 0x801F,
           "clear writes 5551 red with the stencil bit: " + hex(m.fb16(0, 0)));
    expect(m.fb16(480, 0) == 0, "clear stops at the scissor");
    expect(m.mem.VramRead16(0x088000 + (10 * 512 + 10) * 2) == 0x1234, "clear writes depth");
    expect(ge.stats().clear_rects == 1, "one clear rectangle");
    expect(ge.vertex_addr() == kVertexAddr + 24, "VADDR advances after the draw");
}

// A 32x16 CLUT8 swizzled texture drawn 1:1 with MODULATE x2 and alpha test.
void test_clut8_swizzled_sprite() {
    Machine m;
    Ge ge(m.mem);
    constexpr int kW = 32;
    constexpr int kH = 16;
    auto index_at = [](int u, int t) { return static_cast<std::uint8_t>((u * 7 + t * 13) & 0xFF); };
    std::vector<std::uint8_t> linear(kW * kH);
    for (int t = 0; t < kH; ++t) {
        for (int u = 0; u < kW; ++u) linear[static_cast<std::size_t>(t * kW + u)] = index_at(u, t);
    }
    m.write(kTextureAddr, swizzle(linear, kW, kH));
    auto clut_color = [](int i) -> std::uint32_t {
        if (i == 0) return 0;
        return 0xFF000000u | (static_cast<std::uint32_t>(i) << 16) |
               (static_cast<std::uint32_t>(255 - i) << 8) | static_cast<std::uint32_t>(i);
    };
    std::vector<std::uint8_t> clut;
    for (int i = 0; i < 256; ++i) put32(clut, clut_color(i));
    m.write(kClutAddr, clut);

    std::vector<std::uint8_t> v;
    vertex_c(v, 0xFF0000FF, 0, 0, 0);
    vertex_c(v, 0xFF0000FF, 480, 272, 0);
    vertex_tc(v, 0, 0, 0xFF808080, 10, 20);
    vertex_tc(v, kW, kH, 0xFF808080, 10 + kW, 20 + kH);
    m.write(kVertexAddr, v);

    auto words = screen_setup();
    words.insert(words.end(), {
                                  op(kCmdBase, 0x080000),
                                  op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
                                  op(kCmdVertexType, 0x80011C),
                                  op(kCmdClearMode, 0x701),
                                  op(kCmdPrim, (6u << 16) | 2),
                                  op(kCmdClearMode, 0),
                                  op(kCmdVertexType, 0x80011E),
                                  op(kCmdTextureMapEnable, 1),
                                  op(kCmdTexFormat, 5),
                                  op(kCmdTexMode, 1),
                                  op(kCmdTexSize0, (4u << 8) | 5u),
                                  op(kCmdTexAddr0, kTextureAddr & 0xFFFFFF),
                                  op(kCmdTexBufWidth0, (0x08u << 16) | kW),
                                  op(kCmdClutAddr, kClutAddr & 0xFFFFFF),
                                  op(kCmdClutAddrUpper, 0x080000),
                                  op(kCmdClutFormat, 0xFF03),
                                  op(kCmdLoadClut, 32),
                                  op(kCmdTexFunc, 0x10100),
                                  op(kCmdTexFilter, 0),
                                  op(kCmdTexWrap, 0x101),
                                  op(kCmdAlphaTestEnable, 1),
                                  op(kCmdAlphaTest, 0xFF0006),
                                  op(kCmdPrim, (6u << 16) | 2),
                              });
    const RunResult r = run_list(ge, m, words);
    expect(r.status == RunStatus::kFinished, "sprite list finishes");
    expect(ge.stats().sprites == 1, "drawn through the sprite path");
    int bad = 0;
    for (int t = 0; t < kH; ++t) {
        for (int u = 0; u < kW; ++u) {
            const int idx = index_at(u, t);
            std::uint16_t expected = 0x801F; // alpha 0 texels fail the alpha test
            if (idx != 0) {
                const Vec4i tex = Vec4i::FromRGBA(clut_color(idx));
                Vec4i c;
                for (int k = 0; k < 3; ++k) c[k] = ((128 + 1) * tex[k] * 2) >> 8;
                c[3] = 255;
                expected = static_cast<std::uint16_t>(RGBA8888ToRGBA555X(c.ToRGBA()) | 0x8000);
            }
            if (m.fb16(10 + u, 20 + t) != expected) ++bad;
        }
    }
    expect(bad == 0, "CLUT8 swizzled MODULATE x2 sprite: " + std::to_string(bad) + " bad pixels");
    expect(m.fb16(9, 20) == 0x801F && m.fb16(10 + kW, 20) == 0x801F, "sprite edges");
}

// Scaled CLUT4 texture through the generic rectangle path with blending and
// dither.
void test_clut4_rectangle_blend_dither() {
    Machine m;
    Ge ge(m.mem);
    constexpr int kW = 64; // 32 bytes per row: two swizzle blocks wide
    constexpr int kH = 8;
    auto index_at = [](int u, int t) { return (u + t * 3) & 0xF; };
    std::vector<std::uint8_t> linear(kW / 2 * kH);
    for (int t = 0; t < kH; ++t) {
        for (int u = 0; u < kW; u += 2) {
            linear[static_cast<std::size_t>(t * kW / 2 + u / 2)] =
                static_cast<std::uint8_t>(index_at(u, t) | (index_at(u + 1, t) << 4));
        }
    }
    m.write(kTextureAddr, swizzle(linear, kW / 2, kH));
    auto clut_color = [](int i) -> std::uint32_t {
        return 0x80000000u | (static_cast<std::uint32_t>(i * 16) << 8) |
               static_cast<std::uint32_t>(255 - i * 16);
    };
    std::vector<std::uint8_t> clut;
    for (int i = 0; i < 16; ++i) put32(clut, clut_color(i));
    m.write(kClutAddr, clut);

    std::vector<std::uint8_t> v;
    vertex_c(v, 0xFF204060, 0, 0, 0);
    vertex_c(v, 0xFF204060, 480, 272, 0);
    vertex_tc(v, 0, 0, 0xFFFFFFFF, 100, 50);
    vertex_tc(v, kW, kH, 0xFFFFFFFF, 100 + 2 * kW, 50 + 2 * kH);
    m.write(kVertexAddr, v);

    // PSP default dither matrix rows (signed nibbles, x in the low bits).
    const int dither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    std::uint32_t dith_reg[4];
    for (int y = 0; y < 4; ++y) {
        dith_reg[y] = 0;
        for (int x = 0; x < 4; ++x) {
            dith_reg[y] |= (static_cast<std::uint32_t>(dither[y][x]) & 0xF) << (4 * x);
        }
    }

    auto words = screen_setup();
    words.insert(words.end(), {
                                  op(kCmdBase, 0x080000),
                                  op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
                                  op(kCmdVertexType, 0x80011C),
                                  op(kCmdClearMode, 0x701),
                                  op(kCmdPrim, (6u << 16) | 2),
                                  op(kCmdClearMode, 0),
                                  op(kCmdVertexType, 0x80011E),
                                  op(kCmdTextureMapEnable, 1),
                                  op(kCmdTexFormat, 4),
                                  op(kCmdTexMode, 1),
                                  op(kCmdTexSize0, (3u << 8) | 6u),
                                  op(kCmdTexAddr0, kTextureAddr & 0xFFFFFF),
                                  op(kCmdTexBufWidth0, (0x08u << 16) | kW),
                                  op(kCmdClutAddr, kClutAddr & 0xFFFFFF),
                                  op(kCmdClutAddrUpper, 0x080000),
                                  op(kCmdClutFormat, 0xFF03),
                                  op(kCmdLoadClut, 2),
                                  op(kCmdTexFunc, 0x100),
                                  op(kCmdTexFilter, 0),
                                  op(kCmdTexWrap, 0),
                                  op(kCmdAlphaBlendEnable, 1),
                                  op(kCmdBlendMode, (0u << 8) | (3u << 4) | 2u),
                                  op(kCmdDitherEnable, 1),
                                  op(kCmdDith0, dith_reg[0]),
                                  op(kCmdDith1, dith_reg[1]),
                                  op(kCmdDith2, dith_reg[2]),
                                  op(kCmdDith3, dith_reg[3]),
                                  op(kCmdPrim, (6u << 16) | 2),
                              });
    const RunResult r = run_list(ge, m, words);
    expect(r.status == RunStatus::kFinished, "rectangle list finishes");
    expect(ge.stats().rectangles == 1, "scaled rectangle uses the generic path");
    const Vec4i dst = Vec4i::FromRGBA(RGBA5551ToRGBA8888(to5551(0xFF204060)));
    int bad = 0;
    for (int y = 0; y < 2 * kH; ++y) {
        for (int x = 0; x < 2 * kW; ++x) {
            // Nearest, 2x: pixel x samples texel x / 2.
            const Vec4i src = Vec4i::FromRGBA(clut_color(index_at(x / 2, y / 2)));
            const int px = 100 + x;
            const int py = 50 + y;
            Vec4i out;
            for (int k = 0; k < 3; ++k) {
                const int a = src.a();
                out[k] = (((src[k] * 2 + 1) * (a * 2 + 1)) >> 10) +
                         (((dst[k] * 2 + 1) * ((255 - a) * 2 + 1)) >> 10) + dither[py & 3][px & 3];
            }
            const std::uint16_t expected =
                static_cast<std::uint16_t>(RGBA8888ToRGBA555X(out.ToRGB()) | 0x8000);
            if (m.fb16(px, py) != expected) ++bad;
        }
    }
    expect(bad == 0,
           "CLUT4 x2 rectangle with blend and dither: " + std::to_string(bad) + " bad pixels");
}

void test_triangle_fill_and_shading() {
    for (int gouraud = 0; gouraud < 2; ++gouraud) {
        Machine m;
        Ge ge(m.mem);
        std::vector<std::uint8_t> v;
        vertex_c(v, 0xFF0000FF, 0, 0, 0);
        vertex_c(v, 0xFF00FF00, 64, 64, 0);
        vertex_c(v, 0xFFFF0000, 0, 64, 0);
        m.write(kVertexAddr, v);
        auto words = screen_setup();
        words.insert(words.end(), {
                                      op(kCmdShadeMode, static_cast<std::uint32_t>(gouraud)),
                                      op(kCmdBase, 0x080000),
                                      op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
                                      op(kCmdVertexType, 0x80011C),
                                      op(kCmdPrim, (3u << 16) | 3),
                                  });
        const RunResult r = run_list(ge, m, words);
        expect(r.status == RunStatus::kFinished, "triangle list finishes");
        int covered = 0;
        bool exact = true;
        for (int y = 0; y < 70; ++y) {
            for (int x = 0; x < 70; ++x) {
                const bool drawn = (m.fb16(x, y) & 0x7FFF) != 0;
                covered += drawn ? 1 : 0;
                // Samples sit at +7/16 of each pixel. The diagonal is a right
                // edge, so the samples exactly on it (x == y) are excluded;
                // the bottom edge (y = 64) is flat-bottom and excluded too.
                if (drawn != (x < y && y < 64)) exact = false;
            }
        }
        expect(exact && covered == 2016,
               "triangle coverage follows the fill rule: " + std::to_string(covered));
        if (gouraud) {
            const Vec4i near_red = Vec4i::FromRGBA(RGBA5551ToRGBA8888(m.fb16(0, 1)));
            const Vec4i near_blue = Vec4i::FromRGBA(RGBA5551ToRGBA8888(m.fb16(0, 60)));
            expect(near_red.r() > 230 && near_red.g() < 30 && near_red.b() < 30,
                   "Gouraud: red near v0");
            expect(near_blue.b() > 200 && near_blue.r() < 40, "Gouraud: blue near v2");
        } else {
            // Blue (ABGR 0xFFFF0000) with the old, clear stencil bit.
            expect(m.fb16(0, 1) == 0x7C00 && m.fb16(10, 30) == 0x7C00,
                   "flat shading uses the last vertex: " + hex(m.fb16(0, 1)));
        }
    }
}

void test_list_flow() {
    Machine m;
    Ge ge(m.mem);
    // Main list: BASE/VADDR/OFFSETADDR/IADDR, CALL a subroutine, JUMP to a
    // tail with a world matrix upload, FINISH.
    const std::uint32_t sub = kListAddr + 0x100;
    const std::uint32_t tail = kListAddr + 0x200;
    m.write_words(kListAddr, {
                                 op(kCmdBase, 0x080000),
                                 op(kCmdVaddr, 0x200000),
                                 op(kCmdOffsetAddr, 0x000010),
                                 op(kCmdIaddr, 0x000000),
                                 op(kCmdOffsetAddr, 0),
                                 op(kCmdCall, sub & 0xFFFFFF),
                                 op(kCmdJump, tail & 0xFFFFFF),
                             });
    m.write_words(sub, {op(kCmdMinZ, 0x1234), op(kCmdRet, 0)});
    std::vector<std::uint32_t> t = {op(kCmdWorldMatrixNumber, 0)};
    for (int i = 0; i < 13; ++i)
        t.push_back(op(kCmdWorldMatrixData, float24(static_cast<float>(i))));
    t.push_back(op(kCmdFinish, 0x77));
    t.push_back(op(kCmdEnd, 0));
    m.write_words(tail, t);
    DisplayList list;
    list.pc = kListAddr;
    const RunResult r = ge.Run(list);
    expect(r.status == RunStatus::kFinished && r.value == 0x77, "FINISH token");
    expect(ge.vertex_addr() == 0x08200000, "VADDR with BASE: " + hex(ge.vertex_addr()));
    expect(ge.index_addr() == 0x08001000, "IADDR with OFFSETADDR: " + hex(ge.index_addr()));
    expect(ge.state().Reg(kCmdMinZ) == op(kCmdMinZ, 0x1234), "CALL ran the subroutine");
    expect(ge.state().world_matrix[11] == 11.0f && ge.state().world_matrix[0] == 0.0f,
           "world matrix upload");
    expect((ge.state().Reg(kCmdWorldMatrixNumber) & 0xFFFFFF) == 13, "matrix counter");
    expect(list.stack_ptr == 0, "stack balanced");

    // Stall, then signal with the suspend behaviour, then a signal jump.
    Ge ge2(m.mem);
    const std::uint32_t after_jump = kListAddr + 0x400;
    m.write_words(kListAddr, {
                                 op(kCmdMinZ, 1),
                                 op(kCmdMinZ, 2),
                                 op(kCmdSignal, (0x01u << 16) | 0x55),
                                 op(kCmdEnd, 0xABCD),
                                 op(kCmdSignal, (0x10u << 16) | (after_jump >> 16)),
                                 op(kCmdEnd, after_jump & 0xFFFF),
                                 op(kCmdMinZ, 0xBAD),
                             });
    m.write_words(after_jump, {op(kCmdMaxZ, 0x99), op(kCmdFinish, 0), op(kCmdEnd, 0)});
    DisplayList l2;
    l2.pc = kListAddr;
    l2.stall = kListAddr + 4;
    RunResult r2 = ge2.Run(l2);
    expect(r2.status == RunStatus::kStalled && r2.commands == 1, "stall stops the list");
    l2.stall = 0;
    r2 = ge2.Run(l2);
    expect(r2.status == RunStatus::kSignal && r2.value == 0x55 && r2.end_data == 0xABCD &&
               r2.behavior == SignalBehavior::kHandlerSuspend,
           "signal handler status");
    r2 = ge2.Run(l2);
    expect(r2.status == RunStatus::kFinished, "list finishes after the signal jump");
    expect(ge2.state().Reg(kCmdMaxZ) == op(kCmdMaxZ, 0x99) &&
               ge2.state().Reg(kCmdMinZ) == op(kCmdMinZ, 2),
           "signal jump skipped the following commands");

    // Invalid jump target reports an error.
    Ge ge3(m.mem);
    m.write_words(kListAddr, {op(kCmdBase, 0x000000), op(kCmdJump, 0x000100)});
    DisplayList l3;
    l3.pc = kListAddr;
    expect(ge3.Run(l3).status == RunStatus::kError, "jump outside memory is an error");
    expect(!ge3.stats().unsupported.empty(), "the error is reported");

    // A JUMP to itself never ends: the command budget bounds it.
    Ge ge4(m.mem);
    m.write_words(kListAddr, {op(kCmdBase, 0x080000), op(kCmdJump, kListAddr & 0xFFFFFF)});
    DisplayList l4;
    l4.pc = kListAddr;
    const RunResult r4 = ge4.Run(l4, 1000);
    expect(r4.status == RunStatus::kBudget && r4.commands == 1000, "command budget");
}

void test_block_transfer_and_context() {
    Machine m;
    Ge ge(m.mem);
    std::vector<std::uint8_t> src;
    for (int i = 0; i < 16; ++i) put16(src, static_cast<std::uint16_t>(0x1000 + i));
    m.write(0x08400000, src);
    std::vector<std::uint32_t> words = {
        op(kCmdTransferSrc, 0x400000),
        op(kCmdTransferSrcW, (0x08u << 16) | 8),
        op(kCmdTransferSrcPos, 0),
        op(kCmdTransferDst, 0x010000),
        op(kCmdTransferDstW, (0x04u << 16) | 512),
        op(kCmdTransferDstPos, (2u << 10) | 3u),
        op(kCmdTransferSize, (1u << 10) | 3u),
        op(kCmdTransferStart, 0),
    };
    const RunResult r = run_list(ge, m, words);
    expect(r.status == RunStatus::kFinished, "transfer list finishes");
    bool ok = true;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (m.fb16(3 + x, 2 + y, 0x010000) != 0x1000 + y * 8 + x) ok = false;
        }
    }
    expect(ok, "block transfer copies 4x2 16-bit pixels into VRAM");

    std::vector<std::uint32_t> saved(kContextWords);
    ge.SaveContext(saved);
    Ge other(m.mem);
    other.RestoreContext(saved);
    expect(other.state().Reg(kCmdTransferDstW) == ge.state().Reg(kCmdTransferDstW) &&
               other.state().Reg(kCmdTransferSize) == ge.state().Reg(kCmdTransferSize),
           "context save/restore round trip");
}

// ---- Frame dumps -----------------------------------------------------------

std::vector<std::uint8_t> zstd_raw(const std::vector<std::uint8_t> &data) {
    std::vector<std::uint8_t> out;
    put32(out, 0xFD2FB528);
    out.push_back(0xA0); // single segment, 4-byte content size
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(0x20000, data.size() - pos);
        const bool last = pos + n == data.size();
        const std::uint32_t header = (last ? 1u : 0u) | static_cast<std::uint32_t>(n << 3);
        out.push_back(static_cast<std::uint8_t>(header));
        out.push_back(static_cast<std::uint8_t>(header >> 8));
        out.push_back(static_cast<std::uint8_t>(header >> 16));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
                   data.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < data.size());
    return out;
}

std::vector<std::uint8_t> snappy_literals(const std::vector<std::uint8_t> &data) {
    std::vector<std::uint8_t> out;
    std::size_t n = data.size();
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(n & 0x7F);
        n >>= 7;
        out.push_back(static_cast<std::uint8_t>(byte | (n ? 0x80 : 0)));
    } while (n);
    for (std::size_t i = 0; i < data.size(); i += 60) {
        const std::size_t len = std::min<std::size_t>(60, data.size() - i);
        out.push_back(static_cast<std::uint8_t>((len - 1) << 2));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i),
                   data.begin() + static_cast<std::ptrdiff_t>(i + len));
    }
    return out;
}

// Mirrors DumpBuilder in ge_usage_test.py (PPSSPP GPU/Debugger/Record.cpp).
struct DumpBuilder {
    struct Rec {
        std::uint8_t type;
        std::uint32_t size;
        std::uint32_t ptr;
    };
    std::vector<Rec> records;
    std::vector<std::uint8_t> pushbuf;

    DumpBuilder &add(std::uint8_t type, const std::vector<std::uint8_t> &payload) {
        // Align like the recorder does for RAM data.
        while (pushbuf.size() % 16) pushbuf.push_back(0);
        records.push_back({type, static_cast<std::uint32_t>(payload.size()),
                           static_cast<std::uint32_t>(pushbuf.size())});
        pushbuf.insert(pushbuf.end(), payload.begin(), payload.end());
        return *this;
    }
    DumpBuilder &init(const std::vector<std::uint32_t> &context) {
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t w : context) put32(bytes, w);
        return add(0x00, bytes);
    }
    DumpBuilder &regs(const std::vector<std::uint32_t> &ops) {
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t w : ops) put32(bytes, w);
        return add(0x01, bytes);
    }
    std::vector<std::uint8_t> build(std::uint32_t version = 6) const {
        std::vector<std::uint8_t> table;
        for (const Rec &r : records) {
            table.push_back(r.type);
            put32(table, r.size);
            put32(table, r.ptr);
        }
        std::vector<std::uint8_t> out = {'P', 'P', 'S', 'S', 'P', 'P', 'G', 'E'};
        put32(out, version);
        if (version >= 4) {
            const char id[12] = "ULUS10202";
            out.insert(out.end(), id, id + 12);
        }
        put32(out, static_cast<std::uint32_t>(records.size()));
        put32(out, static_cast<std::uint32_t>(pushbuf.size()));
        const std::vector<std::uint8_t> *blobs[2] = {&table, &pushbuf};
        for (const auto *blob : blobs) {
            const auto packed = version >= 5 ? zstd_raw(*blob) : snappy_literals(*blob);
            put32(out, static_cast<std::uint32_t>(packed.size()));
            out.insert(out.end(), packed.begin(), packed.end());
        }
        return out;
    }
};

std::vector<std::uint32_t> default_context() {
    std::vector<std::uint32_t> ctx(kContextWords, 0);
    GeState gs;
    for (int c = 0; c < 256; ++c)
        gs.cmdmem[static_cast<std::size_t>(c)] = op(static_cast<std::uint8_t>(c), 0);
    gs.SaveContext(ctx, 0, 0, 0);
    return ctx;
}

void test_ppdmp_container() {
    DumpBuilder b;
    b.init(default_context()).regs({op(kCmdMinZ, 5)}).add(0x09, std::vector<std::uint8_t>(12, 0));
    for (std::uint32_t version : {2u, 3u, 4u, 5u, 6u}) {
        const auto file = b.build(version);
        replay::FrameDump dump;
        const std::string err = replay::ParseFrameDump(file, &dump);
        expect(err.empty(), "parse v" + std::to_string(version) + ": " + err);
        expect(dump.records.size() == 3 && dump.version == version, "records parsed");
        expect(dump.game_id == (version >= 4 ? "ULUS10202" : ""), "game id");
    }
    const auto good = b.build(6);
    replay::FrameDump dump;
    auto bad = good;
    bad[0] = 'X';
    expect(!replay::ParseFrameDump(bad, &dump).empty(), "bad magic rejected");
    bad = good;
    bad[8] = 7;
    expect(!replay::ParseFrameDump(bad, &dump).empty(), "version 7 rejected");
    bad = good;
    bad.push_back(0);
    expect(!replay::ParseFrameDump(bad, &dump).empty(), "trailing bytes rejected");
    int accepted = 0;
    for (std::size_t n = 0; n < good.size(); ++n) {
        const std::vector<std::uint8_t> cut(good.begin(),
                                            good.begin() + static_cast<std::ptrdiff_t>(n));
        if (replay::ParseFrameDump(cut, &dump).empty()) ++accepted;
    }
    expect(accepted == 0, "every truncation is rejected");
    DumpBuilder outside;
    outside.records.push_back({0x02, 64, 0});
    outside.pushbuf.resize(16);
    expect(!replay::ParseFrameDump(outside.build(), &dump).empty(), "record outside the pushbuf");
    DumpBuilder unknown;
    unknown.add(0x0C, {1, 2, 3, 4});
    expect(!replay::ParseFrameDump(unknown.build(), &dump).empty(), "unknown record type");

    // Snappy copies (kind 1 and 2) and their bounds.
    std::vector<std::uint8_t> out;
    const std::vector<std::uint8_t> s = {10, 0x0C, 'a', 'b', 'c', 'd', 0x09 | (0 << 5), 4};
    // literal "abcd", then copy length 6 offset 4 (kind 1: len=((tag>>2)&7)+4)
    expect(replay::SnappyDecompress(s, 10, &out).empty() &&
               std::string(out.begin(), out.end()) == "abcdabcdab",
           "snappy copy");
    const std::vector<std::uint8_t> s_bad = {4, 0x01 | (0 << 2), 9};
    expect(!replay::SnappyDecompress(s_bad, 4, &out).empty(), "snappy copy before data");
}

void test_replay_end_to_end() {
    // A dump shaped like the recorder's output: state, VERTICES, then the
    // PRIM in the next register block; a RAM CLUT and a RAM texture.
    constexpr int kW = 32;
    constexpr int kH = 16;
    auto index_at = [](int u, int t) { return static_cast<std::uint8_t>((u * 5 + t * 11) & 0xFF); };
    std::vector<std::uint8_t> linear(kW * kH);
    for (int t = 0; t < kH; ++t) {
        for (int u = 0; u < kW; ++u) linear[static_cast<std::size_t>(t * kW + u)] = index_at(u, t);
    }
    std::vector<std::uint8_t> clut;
    for (int i = 0; i < 256; ++i) {
        put32(clut, i == 0 ? 0u : 0xFF000000u | static_cast<std::uint32_t>(i) * 0x010101u);
    }
    std::vector<std::uint8_t> clear_verts;
    vertex_c(clear_verts, 0xFF00FF00, 0, 0, 0);
    vertex_c(clear_verts, 0xFF00FF00, 480, 272, 0);
    std::vector<std::uint8_t> sprite_verts;
    vertex_tc(sprite_verts, 0, 0, 0xFF808080, 200, 100);
    vertex_tc(sprite_verts, kW, kH, 0xFF808080, 200 + kW, 100 + kH);

    auto setup = screen_setup();
    setup.push_back(op(kCmdVertexType, 0x80011C));
    setup.push_back(op(kCmdClearMode, 0x701));
    DumpBuilder b;
    b.init(default_context());
    b.regs(setup);
    b.add(0x02, clear_verts);
    b.regs({
        op(kCmdPrim, (6u << 16) | 2),
        op(kCmdClearMode, 0),
        op(kCmdVertexType, 0x80011E),
        op(kCmdTextureMapEnable, 1),
        op(kCmdTexFormat, 5),
        op(kCmdTexMode, 1),
        op(kCmdTexSize0, (4u << 8) | 5u),
        // Recorded with the game's own addresses; the replayer rewrites them.
        op(kCmdTexAddr0, 0x123450),
        op(kCmdTexBufWidth0, (0x09u << 16) | kW),
        op(kCmdClutFormat, 0xFF03),
        op(kCmdTexFunc, 0x10100),
        op(kCmdAlphaTestEnable, 1),
        op(kCmdAlphaTest, 0xFF0006),
    });
    b.add(0x04, clut);
    b.regs({op(kCmdLoadClut, 32)});
    b.add(0x10, swizzle(linear, kW, kH));
    b.add(0x02, sprite_verts);
    b.regs({op(kCmdPrim, (6u << 16) | 2)});
    std::vector<std::uint8_t> display;
    put32(display, 0x04000000);
    put32(display, 512);
    put32(display, 1);
    b.add(0x09, display);

    for (std::uint32_t version : {4u, 6u}) {
        replay::FrameDump dump;
        const std::string perr = replay::ParseFrameDump(b.build(version), &dump);
        expect(perr.empty(), "synthetic dump parses: " + perr);
        replay::DumpReplayer replayer;
        replay::ReplaySummary summary;
        const std::string err = replayer.Run(dump, &summary);
        expect(err.empty(), "synthetic dump replays: " + err);
        expect(summary.finished, "replayed list reaches FINISH");
        expect(summary.displays.size() == 1 && summary.displays[0].format == 1, "DISPLAY record");
        expect(replayer.ge().stats().unsupported.empty(), "nothing unsupported in the 2D dump");
        const MemoryView &mem = replayer.memory();
        auto fb = [&](int x, int y) {
            return mem.VramRead16(static_cast<std::uint32_t>((y * 512 + x) * 2));
        };
        const std::uint16_t green = to5551(0xFF00FF00);
        int bad = 0;
        for (int t = 0; t < kH; ++t) {
            for (int u = 0; u < kW; ++u) {
                const int idx = index_at(u, t);
                std::uint16_t expected = green;
                if (idx != 0) {
                    const int c = std::min(((128 + 1) * idx * 2) >> 8, 255);
                    expected = static_cast<std::uint16_t>(
                        RGBA8888ToRGBA555X(static_cast<std::uint32_t>(c) * 0x010101u) | 0x8000);
                }
                if (fb(200 + u, 100 + t) != expected) ++bad;
            }
        }
        expect(bad == 0, "replayed sprite pixels: " + std::to_string(bad) + " bad");
        expect(fb(0, 0) == green && fb(479, 271) == green, "replayed clear");

        // The PNG writer round trip on the replayed framebuffer.
        const replay::Image img = replay::ReadFramebuffer(mem, 0x04000000, 512, 1, 480, 272, true);
        const auto png = replay::EncodePng(img);
        expect(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G', "PNG signature");
        // IEND chunk: length 0, "IEND", CRC 0xAE426082.
        const std::uint8_t iend[] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
        expect(png.size() >= 12 && std::memcmp(png.data() + png.size() - 12, iend, 12) == 0,
               "PNG IEND chunk");
        // Decode the stored deflate blocks back and compare a pixel.
        const std::size_t idat = 8 + 25; // after the signature and IHDR
        const std::uint32_t idat_len = (std::uint32_t{png[idat]} << 24) |
                                       (std::uint32_t{png[idat + 1]} << 16) |
                                       (std::uint32_t{png[idat + 2]} << 8) | png[idat + 3];
        std::vector<std::uint8_t> raw;
        std::size_t p = idat + 8 + 2;
        const std::size_t end = idat + 8 + idat_len - 4;
        while (p < end) {
            const bool last = png[p] & 1;
            const std::size_t n = png[p + 1] | (static_cast<std::size_t>(png[p + 2]) << 8);
            raw.insert(raw.end(), png.begin() + static_cast<std::ptrdiff_t>(p + 5),
                       png.begin() + static_cast<std::ptrdiff_t>(p + 5 + n));
            p += 5 + n;
            if (last) break;
        }
        const std::size_t row = 1 + 480 * 4;
        expect(raw.size() == row * 272, "PNG scanlines");
        if (raw.size() == row * 272) {
            const std::size_t at = 100 * row + 1 + 210 * 4;
            const std::uint32_t c = RGBA5551ToRGBA8888(fb(210, 100)) | 0xFF000000u;
            expect(raw[at] == (c & 0xFF) && raw[at + 1] == ((c >> 8) & 0xFF) &&
                       raw[at + 2] == ((c >> 16) & 0xFF) && raw[at + 3] == 0xFF,
                   "PNG pixel matches VRAM");
        }
    }
}

} // namespace

int main() {
    try {
        test_vertex_sizes();
        test_memory_view();
        test_clear();
        test_clut8_swizzled_sprite();
        test_clut4_rectangle_blend_dither();
        test_triangle_fill_and_shading();
        test_list_flow();
        test_block_transfer_and_context();
        test_ppdmp_container();
        test_replay_end_to_end();
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "FAIL: uncaught exception: %s\n", exception.what());
        return EXIT_FAILURE;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("mpo_ge_test: all checks passed");
    return EXIT_SUCCESS;
}
