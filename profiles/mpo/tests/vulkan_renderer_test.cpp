// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vulkan (F14a): the same synthetic GE lists drawn by softgpu (mpo_ge)
// and by the Vulkan renderer, compared pixel by pixel after the readback into
// VRAM. Every list, texture and expected value is made up here (CLAUDE.md
// §1.1).
//
// - Needs a Vulkan 1.2 device. Without a loader or an ICD (the CI today) it
//   exits 77, which ctest reports as skipped.
// - The validation layers are on (when installed); any validation error
//   fails the test. MPO_VK_REQUIRE_VALIDATION=1 also fails when the layer is
//   missing.
// - Every case runs twice: with Vulkan 1.3 dynamic rendering (when the device
//   has it) and with the 1.2 render pass path.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "color.hpp"
#include "context.hpp"
#include "env.hpp"
#include "ge.hpp"
#include "ge_renderer.hpp"
#include "memory_view.hpp"

namespace {

using namespace mpo::ge;

int g_failures = 0;
constexpr int kSkip = 77;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
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

void putf(std::vector<std::uint8_t> &out, float f) {
    put32(out, std::bit_cast<std::uint32_t>(f));
}

constexpr std::uint32_t kListAddr = 0x08100000;
constexpr std::uint32_t kVertexAddr = 0x08200000;
constexpr std::uint32_t kTextureAddr = 0x08300000;
constexpr std::uint32_t kClutAddr = 0x08380000;

struct Machine {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(8u << 20, std::uint8_t{0});
    std::vector<std::uint8_t> vram =
        std::vector<std::uint8_t>(MemoryView::kVramSize, std::uint8_t{0});
    MemoryView mem{ram, 0x08000000, vram};
};

// The memory a case writes before its list runs.
struct Scene {
    std::string name;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> data;
    std::vector<std::uint32_t> list;
    int fb_format = 1; // 5551
    int max_differing = 0;
    int max_delta = 0; // largest per-channel difference allowed (8-bit)
    // The scene must change at least this many pixels of the first one
    // (softgpu), so a case cannot pass by drawing nothing.
    int min_drawn = 1;
};

struct FrameDiff {
    int differing = 0;
    int max_delta = 0;
    int first_x = -1;
    int first_y = -1;
    std::uint32_t soft = 0;
    std::uint32_t gpu = 0;
};

std::uint32_t PixelRgb(const Machine &m, int fmt, int x, int y) {
    if (fmt == 3) return m.mem.VramRead32(static_cast<std::uint32_t>((y * 512 + x) * 4)) & 0xFFFFFF;
    const std::uint16_t v = m.mem.VramRead16(static_cast<std::uint32_t>((y * 512 + x) * 2));
    switch (fmt) {
    case 0:
        return RGB565ToRGBA8888(v) & 0xFFFFFF;
    case 1:
        return RGBA5551ToRGBA8888(v) & 0xFFFFFF;
    default:
        return RGBA4444ToRGBA8888(v) & 0xFFFFFF;
    }
}

int CountChanged(const Machine &m, int fmt) {
    const std::uint32_t first = PixelRgb(m, fmt, 0, 0);
    int n = 0;
    for (int y = 0; y < 272; ++y) {
        for (int x = 0; x < 480; ++x) n += PixelRgb(m, fmt, x, y) != first ? 1 : 0;
    }
    return n;
}

void Load(Machine &m, const Scene &s) {
    for (const auto &[addr, bytes] : s.data) expect(m.mem.CopyIn(addr, bytes), s.name + ": data");
    std::vector<std::uint32_t> words = s.list;
    words.push_back(op(kCmdFinish, 0));
    words.push_back(op(kCmdEnd, 0));
    for (std::size_t i = 0; i < words.size(); ++i)
        m.mem.Write32(kListAddr + static_cast<std::uint32_t>(i * 4), words[i]);
}

bool RunSoft(const Scene &s, Machine &m) {
    Load(m, s);
    Ge ge(m.mem);
    DisplayList list;
    list.pc = kListAddr;
    return ge.Run(list).status == RunStatus::kFinished;
}

bool RunVulkan(mpo::vk::VulkanContext *ctx, const Scene &s, Machine &m) {
    Load(m, s);
    Ge ge(m.mem);
    std::string err;
    auto gpu = mpo::vk::GeRenderer::Create(ctx, m.mem, &err);
    if (!gpu) {
        std::fprintf(stderr, "FAIL: %s: renderer: %s\n", s.name.c_str(), err.c_str());
        ++g_failures;
        return false;
    }
    ge.SetDrawBackend(gpu.get());
    DisplayList list;
    list.pc = kListAddr;
    const bool ok = ge.Run(list).status == RunStatus::kFinished;
    if (!gpu->EndFrame(true, &err)) {
        std::fprintf(stderr, "FAIL: %s: EndFrame: %s\n", s.name.c_str(), err.c_str());
        ++g_failures;
        return false;
    }
    ge.SetDrawBackend(nullptr);
    return ok;
}

FrameDiff Compare(const Machine &a, const Machine &b, int fmt) {
    FrameDiff c;
    for (int y = 0; y < 272; ++y) {
        for (int x = 0; x < 480; ++x) {
            const std::uint32_t pa = PixelRgb(a, fmt, x, y);
            const std::uint32_t pb = PixelRgb(b, fmt, x, y);
            if (pa == pb) continue;
            if (c.differing++ == 0) {
                c.first_x = x;
                c.first_y = y;
                c.soft = pa;
                c.gpu = pb;
            }
            for (int k = 0; k < 3; ++k) {
                const int d = std::abs(static_cast<int>((pa >> (8 * k)) & 0xFF) -
                                       static_cast<int>((pb >> (8 * k)) & 0xFF));
                if (d > c.max_delta) c.max_delta = d;
            }
        }
    }
    return c;
}

// --- Scene building blocks ---------------------------------------------------

std::vector<std::uint32_t> Setup2d(int fb_format) {
    return {
        op(kCmdFrameBufPtr, 0x000000),
        op(kCmdFrameBufWidth, 512),
        op(kCmdFrameBufPixFormat, static_cast<std::uint32_t>(fb_format)),
        op(kCmdZBufPtr, 0x088000),
        op(kCmdZBufWidth, 512),
        op(kCmdScissor1, 0),
        op(kCmdScissor2, (271u << 10) | 479u),
        op(kCmdRegion1, 0),
        op(kCmdRegion2, (271u << 10) | 479u),
        op(kCmdShadeMode, 1),
        op(kCmdBase, 0x080000),
    };
}

std::vector<std::uint32_t> IdentityMatrices() {
    std::vector<std::uint32_t> w;
    const float m43[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const float m44[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    w.push_back(op(kCmdWorldMatrixNumber, 0));
    for (float f : m43) w.push_back(op(kCmdWorldMatrixData, float24(f)));
    w.push_back(op(kCmdViewMatrixNumber, 0));
    for (float f : m43) w.push_back(op(kCmdViewMatrixData, float24(f)));
    w.push_back(op(kCmdProjMatrixNumber, 0));
    for (float f : m44) w.push_back(op(kCmdProjMatrixData, float24(f)));
    return w;
}

// The PSP's usual 480x272 viewport (x = 240 X + 2048, y = -136 Y + 2048,
// z = 20000 Z + 30000, offset 1808/1912), depth GEQUAL with writes.
std::vector<std::uint32_t> Setup3d(int fb_format) {
    std::vector<std::uint32_t> w = Setup2d(fb_format);
    const std::vector<std::uint32_t> more = {
        op(kCmdViewportXScale, float24(240.0f)),
        op(kCmdViewportYScale, float24(-136.0f)),
        op(kCmdViewportZScale, float24(20000.0f)),
        op(kCmdViewportXCenter, float24(2048.0f)),
        op(kCmdViewportYCenter, float24(2048.0f)),
        op(kCmdViewportZCenter, float24(30000.0f)),
        op(kCmdOffsetX, 1808u * 16u),
        op(kCmdOffsetY, 1912u * 16u),
        op(kCmdMinZ, 0),
        op(kCmdMaxZ, 0xFFFF),
        op(kCmdZTestEnable, 1),
        op(kCmdZTest, 7), // GEQUAL
        op(kCmdZWriteDisable, 0),
        op(kCmdMaterialAmbient, 0xFFFFFF),
        op(kCmdMaterialAlpha, 0xFF),
    };
    w.insert(w.end(), more.begin(), more.end());
    const auto m = IdentityMatrices();
    w.insert(w.end(), m.begin(), m.end());
    return w;
}

// Clear of the whole screen to `color` (and depth to `z`) through a
// through-mode RECTANGLES pair (vtype 0x80011C: colour, s16 position).
std::vector<std::uint32_t> ClearCmds(Scene *s, std::uint32_t addr, std::uint32_t color, int z,
                                     std::uint32_t flags = 0x701) {
    std::vector<std::uint8_t> v;
    for (int i = 0; i < 2; ++i) {
        put32(v, color);
        put16(v, static_cast<std::uint16_t>(i ? 480 : 0));
        put16(v, static_cast<std::uint16_t>(i ? 272 : 0));
        put16(v, static_cast<std::uint16_t>(z));
        put16(v, 0);
    }
    s->data.emplace_back(addr, v);
    return {op(kCmdVaddr, addr & 0xFFFFFF), op(kCmdVertexType, 0x80011C), op(kCmdClearMode, flags),
            op(kCmdPrim, (6u << 16) | 2), op(kCmdClearMode, 0)};
}

// Swizzles `linear` (pitch_bytes x rows) into 16-byte x 8-row blocks.
std::vector<std::uint8_t> Swizzle(const std::vector<std::uint8_t> &linear, int pitch_bytes,
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

// A 256-entry 8888 palette with varied colours and alphas.
std::vector<std::uint8_t> Palette8888() {
    std::vector<std::uint8_t> clut;
    for (int i = 0; i < 256; ++i) {
        const std::uint32_t a = i == 0 ? 0 : static_cast<std::uint32_t>((i * 37) & 0xFF) | 0x10;
        put32(clut, (a << 24) | (static_cast<std::uint32_t>(i) << 16) |
                        (static_cast<std::uint32_t>(255 - i) << 8) |
                        static_cast<std::uint32_t>((i * 5) & 0xFF));
    }
    return clut;
}

// CLUT8 texture (w x h), swizzled, with its palette loaded.
std::vector<std::uint32_t> Clut8Texture(Scene *s, int w, int h, int wshift, int hshift,
                                        bool swizzle) {
    std::vector<std::uint8_t> linear(static_cast<std::size_t>(w * h));
    for (int t = 0; t < h; ++t) {
        for (int u = 0; u < w; ++u) {
            linear[static_cast<std::size_t>(t * w + u)] =
                static_cast<std::uint8_t>((u * 7 + t * 13 + (u ^ t)) & 0xFF);
        }
    }
    s->data.emplace_back(kTextureAddr, swizzle ? Swizzle(linear, w, h) : linear);
    s->data.emplace_back(kClutAddr, Palette8888());
    return {
        op(kCmdTextureMapEnable, 1),
        op(kCmdTexFormat, 5),
        op(kCmdTexMode, swizzle ? 1u : 0u),
        op(kCmdTexSize0,
           (static_cast<std::uint32_t>(hshift) << 8) | static_cast<std::uint32_t>(wshift)),
        op(kCmdTexAddr0, kTextureAddr & 0xFFFFFF),
        op(kCmdTexBufWidth0, (0x08u << 16) | static_cast<std::uint32_t>(w)),
        op(kCmdClutAddr, kClutAddr & 0xFFFFFF),
        op(kCmdClutAddrUpper, 0x080000),
        op(kCmdClutFormat, 0xFF03),
        op(kCmdLoadClut, 32),
    };
}

// Through-mode textured vertex: u16 uv, 8888 colour, s16 position
// (vtype 0x80011E, stride 16).
void VertexTc(std::vector<std::uint8_t> &out, int u, int v, std::uint32_t color, int x, int y,
              int z = 0) {
    put16(out, static_cast<std::uint16_t>(u));
    put16(out, static_cast<std::uint16_t>(v));
    put32(out, color);
    put16(out, static_cast<std::uint16_t>(x));
    put16(out, static_cast<std::uint16_t>(y));
    put16(out, static_cast<std::uint16_t>(z));
    put16(out, 0);
}

// Transform-mode vertex: float uv, 8888 colour, float position
// (vtype 0x00019F: uv float, colour 8888, position float; stride 24).
void VertexTcf(std::vector<std::uint8_t> &out, float u, float v, std::uint32_t color, float x,
               float y, float z) {
    putf(out, u);
    putf(out, v);
    put32(out, color);
    putf(out, x);
    putf(out, y);
    putf(out, z);
}
constexpr std::uint32_t kVtypeTcf = 3u | (7u << 2) | (3u << 7);

// --- Scenes ------------------------------------------------------------------

Scene ClearAndFlatSprites() {
    Scene s;
    s.name = "clear and flat sprites";
    s.list = Setup2d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF203040, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    std::vector<std::uint8_t> v;
    for (int i = 0; i < 4; ++i) {
        put32(v, 0xFF000000u | (0x40u * static_cast<std::uint32_t>(i) + 0x30u));
        put16(v, static_cast<std::uint16_t>(10 + i * 50));
        put16(v, static_cast<std::uint16_t>(20 + i * 30));
        put16(v, 0);
        put16(v, 0);
        put32(v, 0xFF000000u | (0x40u * static_cast<std::uint32_t>(i) + 0x30u));
        put16(v, static_cast<std::uint16_t>(70 + i * 50));
        put16(v, static_cast<std::uint16_t>(90 + i * 30));
        put16(v, 0);
        put16(v, 0);
    }
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                 op(kCmdVertexType, 0x80011C), op(kCmdPrim, (6u << 16) | 8)});
    return s;
}

Scene DitheredClear() {
    Scene s;
    s.name = "dithered clear";
    s.list = Setup2d(1);
    s.list.insert(s.list.end(),
                  {op(kCmdDitherEnable, 1), op(kCmdDith0, 0x1F3A), op(kCmdDith1, 0x2B4C),
                   op(kCmdDith2, 0x0D6E), op(kCmdDith3, 0x7F81)});
    // Dither is honoured in clear mode only through the generic rectangle
    // path (softgpu's ClearRectangle needs dither off).
    auto c = ClearCmds(&s, kVertexAddr, 0xFF5A5A5A, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    return s;
}

Scene Clut8SpriteModulate(bool white) {
    Scene s;
    s.name = white ? "CLUT8 sprite, white vertex (no doubling quirk)" : "CLUT8 sprite MODULATE x2";
    s.list = Setup2d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF0000FF, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    auto t = Clut8Texture(&s, 32, 16, 5, 4, true);
    s.list.insert(s.list.end(), t.begin(), t.end());
    std::vector<std::uint8_t> v;
    const std::uint32_t col = white ? 0xFFFFFFFF : 0xFF808080;
    VertexTc(v, 0, 0, col, 10, 20);
    VertexTc(v, 32, 16, col, 42, 36);
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {
                                    op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                    op(kCmdVertexType, 0x80011E),
                                    op(kCmdTexFunc, 0x10100),
                                    op(kCmdTexFilter, 0x101),
                                    op(kCmdTexWrap, 0x101),
                                    op(kCmdAlphaTestEnable, 1),
                                    op(kCmdAlphaTest, 0xFF0006),
                                    op(kCmdAlphaBlendEnable, 1),
                                    op(kCmdBlendMode, 0x0032), // ADD(SRCALPHA, INVSRCALPHA)
                                    op(kCmdPrim, (6u << 16) | 2),
                                });
    // Fixed-function blending rounds differently from softgpu's integer
    // blend: at most one 5-bit step (8 or 9 in 8 bits) on some pixels (F15).
    s.max_differing = 32 * 16;
    s.max_delta = 9;
    return s;
}

Scene ScaledBilinearRect() {
    Scene s;
    s.name = "scaled bilinear CLUT8 rectangle, opaque";
    s.list = Setup2d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF000000, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    auto t = Clut8Texture(&s, 32, 32, 5, 5, false);
    s.list.insert(s.list.end(), t.begin(), t.end());
    std::vector<std::uint8_t> v;
    VertexTc(v, 0, 0, 0xFF808080, 100, 50);
    VertexTc(v, 32, 32, 0xFF808080, 100 + 77, 50 + 61);
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {
                                    op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                    op(kCmdVertexType, 0x80011E),
                                    op(kCmdTexFunc, 0x10000), // MODULATE x2, rgb only
                                    op(kCmdTexFilter, 0x101),
                                    op(kCmdTexWrap, 0x000),
                                    op(kCmdPrim, (6u << 16) | 2),
                                });
    return s;
}

Scene GouraudTriangles() {
    Scene s;
    s.name = "Gouraud triangles (through)";
    s.list = Setup2d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF000000, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    std::vector<std::uint8_t> v;
    auto vert = [&](std::uint32_t col, int x, int y) {
        put32(v, col);
        put16(v, static_cast<std::uint16_t>(x));
        put16(v, static_cast<std::uint16_t>(y));
        put16(v, 0);
        put16(v, 0);
    };
    vert(0xFF0000FF, 20, 20);
    vert(0xFF00FF00, 300, 40);
    vert(0xFFFF0000, 60, 250);
    vert(0xFFFFFFFF, 400, 30);
    vert(0xFF808080, 470, 260);
    vert(0xFF102030, 200, 200);
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                 op(kCmdVertexType, 0x80011C), op(kCmdPrim, (3u << 16) | 6)});
    // Interpolated colours may round at slightly different points on another
    // GPU (lavapipe gives 0).
    s.max_differing = 2000;
    s.max_delta = 9;
    return s;
}

Scene DepthFogQuads() {
    Scene s;
    s.name = "3D quads: depth GEQUAL, depth range, fog";
    s.list = Setup3d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF000000, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    std::vector<std::uint8_t> v;
    auto quad = [&](float x0, float y0, float x1, float y1, float z, std::uint32_t col) {
        for (int i = 0; i < 4; ++i) {
            put32(v, col);
            putf(v, (i & 1) ? x1 : x0);
            putf(v, (i & 2) ? y1 : y0);
            putf(v, z);
        }
    };
    quad(-0.8f, -0.8f, 0.4f, 0.4f, 0.1f, 0xFF3060C0);  // z 32000
    quad(-0.4f, -0.4f, 0.8f, 0.8f, -0.2f, 0xFFC06030); // z 26000: behind, fails GEQUAL
    quad(-0.2f, -0.9f, 0.2f, 0.9f, 0.5f, 0xFF20E020);  // z 40000: in front
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {
                                    op(kCmdFogEnable, 1),
                                    op(kCmdFog1, float24(1.0f)),
                                    op(kCmdFog2, float24(0.5f)),
                                    op(kCmdFogColor, 0x804020),
                                    op(kCmdMinZ, 0x2710),
                                    op(kCmdMaxZ, 0xC350),
                                    op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                    op(kCmdVertexType, (7u << 2) | (3u << 7)),
                                    op(kCmdPrim, (4u << 16) | 4),
                                    op(kCmdPrim, (4u << 16) | 4),
                                    op(kCmdPrim, (4u << 16) | 4),
                                });
    s.max_differing = 600;
    s.max_delta = 9;
    return s;
}

// The shape of MPO's cutscene overlays: big perspective triangles, partly
// off screen, a CLUT8 texture with bilinear filtering and clamp, MODULATE x2
// and a very low alpha, blended over a lit background.
Scene LowAlphaPerspectiveOverlay() {
    Scene s;
    s.name = "low-alpha perspective overlay (CLUT8, bilinear, clamp)";
    s.list = Setup3d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF8090A0, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    auto t = Clut8Texture(&s, 64, 64, 6, 6, true);
    s.list.insert(s.list.end(), t.begin(), t.end());
    // A projection with w = z (simple perspective): x' = x, y' = y, w = -z.
    const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.1f, -1, 0, 0, 0, 0};
    s.list.push_back(op(kCmdProjMatrixNumber, 0));
    for (float f : proj) s.list.push_back(op(kCmdProjMatrixData, float24(f)));
    std::vector<std::uint8_t> v;
    // w around 960 and vertices hundreds of pixels off screen, the shape of
    // the cutscene_1 draw where lavapipe's clipper produced NaN colours
    // before the viewport covered the whole GE screen space (shaders/ge.vert;
    // these exact vertices do not trigger it, F14a.md).
    VertexTcf(v, 0, 1, 0x22287864, -9.0f * 320.0f, 6.0f * 320.0f, -3.0f * 320.0f);
    VertexTcf(v, 1, 0, 0x22287864, 7.0f * 320.0f, 2.0f * 320.0f, -2.0f * 320.0f);
    VertexTcf(v, 0, 0, 0x22287864, 1.0f * 320.0f, -8.0f * 320.0f, -2.5f * 320.0f);
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {
                                    op(kCmdZTestEnable, 0),
                                    op(kCmdDepthClampEnable, 1),
                                    op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                    op(kCmdVertexType, kVtypeTcf),
                                    op(kCmdTexFunc, 0x10100),
                                    op(kCmdTexFilter, 0x101),
                                    op(kCmdTexWrap, 0x101),
                                    op(kCmdTexScaleU, float24(1.0f)),
                                    op(kCmdTexScaleV, float24(1.0f)),
                                    op(kCmdAlphaTestEnable, 1),
                                    op(kCmdAlphaTest, 0xFF0006),
                                    op(kCmdAlphaBlendEnable, 1),
                                    op(kCmdBlendMode, 0x0032),
                                    op(kCmdPrim, (3u << 16) | 3),
                                });
    // Blend rounding (F15) differs by one step on many pixels.
    s.max_differing = 130560;
    s.max_delta = 9;
    return s;
}

Scene RadarLines() {
    Scene s;
    s.name = "through-mode line strip (softgpu DDA on the CPU)";
    s.list = Setup2d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF000000, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    std::vector<std::uint8_t> v;
    for (int i = 0; i <= 16; ++i) {
        const float a = static_cast<float>(i) * 0.392699f;
        put32(v, 0xFF00FF00u | static_cast<std::uint32_t>(i * 15));
        putf(v, 300.0f + 60.0f * std::cos(a));
        putf(v, 130.0f + 60.0f * std::sin(a));
        putf(v, 0.0f);
    }
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                 op(kCmdVertexType, 0x800000u | (7u << 2) | (3u << 7)),
                                 op(kCmdPrim, (2u << 16) | 17)});
    return s;
}

Scene Spline() {
    Scene s;
    s.name = "spline 4x4 (tessellated by mpo_ge)";
    s.list = Setup3d(1);
    auto c = ClearCmds(&s, kVertexAddr, 0xFF000000, 0);
    s.list.insert(s.list.end(), c.begin(), c.end());
    std::vector<std::uint8_t> v;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            put32(v, 0xFF000000u | static_cast<std::uint32_t>(i * 60) |
                         (static_cast<std::uint32_t>(j * 60) << 8));
            putf(v, -0.7f + 0.45f * static_cast<float>(i));
            putf(v, -0.7f + 0.45f * static_cast<float>(j));
            putf(v, 0.1f * static_cast<float>((i + j) % 3));
        }
    }
    s.data.emplace_back(kVertexAddr + 0x100, v);
    s.list.insert(s.list.end(), {op(kCmdPatchDivision, (6u << 8) | 6u), op(kCmdPatchPrimitive, 0),
                                 op(kCmdVaddr, (kVertexAddr + 0x100) & 0xFFFFFF),
                                 op(kCmdVertexType, (7u << 2) | (3u << 7)),
                                 op(kCmdSpline, (3u << 18) | (3u << 16) | (4u << 8) | 4u)});
    s.max_differing = 1500;
    s.max_delta = 9;
    return s;
}

int RunAll(bool allow_13) {
    mpo::vk::ContextOptions opts;
    opts.app_name = "mpo_vulkan_renderer_test";
    opts.validation = true;
    opts.require_validation = mpo::vk::GetEnv("MPO_VK_REQUIRE_VALIDATION") == "1";
    opts.allow_vulkan_1_3 = allow_13;
    mpo::vk::VulkanContext::Status status;
    std::string err;
    auto ctx = mpo::vk::VulkanContext::Create(opts, &status, &err);
    if (!ctx) {
        if (status == mpo::vk::VulkanContext::Status::kUnavailable) {
            std::printf("SKIP: no usable Vulkan device: %s\n", err.c_str());
            return kSkip;
        }
        std::fprintf(stderr, "FAIL: Vulkan context: %s\n", err.c_str());
        return 1;
    }
    const auto &caps = ctx->caps();
    std::printf("device %s, API %s, %s, validation %s\n", caps.device_name.c_str(),
                mpo::vk::VersionString(caps.api_version).c_str(),
                caps.dynamic_rendering ? "dynamic rendering" : "render passes",
                caps.validation_active ? "on" : "OFF (layer not installed)");

    const std::vector<std::function<Scene()>> scenes = {
        ClearAndFlatSprites,
        DitheredClear,
        [] { return Clut8SpriteModulate(false); },
        [] { return Clut8SpriteModulate(true); },
        ScaledBilinearRect,
        GouraudTriangles,
        DepthFogQuads,
        LowAlphaPerspectiveOverlay,
        RadarLines,
        Spline,
    };
    for (const auto &make : scenes) {
        const Scene s = make();
        auto soft = std::make_unique<Machine>();
        auto gpu = std::make_unique<Machine>();
        expect(RunSoft(s, *soft), s.name + ": soft list finishes");
        expect(RunVulkan(ctx.get(), s, *gpu), s.name + ": vulkan list finishes");
        const FrameDiff c = Compare(*soft, *gpu, s.fb_format);
        const int drawn = CountChanged(*soft, s.fb_format);
        expect(drawn >= s.min_drawn,
               s.name + ": the scene draws (" + std::to_string(drawn) + " pixels)");
        std::printf("  %-55s drawn %6d, differing %6d, max delta %3d", s.name.c_str(), drawn,
                    c.differing, c.max_delta);
        if (c.differing > 0) {
            std::printf(" (first at %d,%d: soft %06X vulkan %06X)", c.first_x, c.first_y, c.soft,
                        c.gpu);
        }
        std::printf("\n");
        expect(c.differing <= s.max_differing && c.max_delta <= s.max_delta,
               s.name + ": " + std::to_string(c.differing) + " pixels differ (max delta " +
                   std::to_string(c.max_delta) + ")");
    }
    const int errors = ctx->validation().errors.load();
    for (const std::string &m : ctx->validation().Messages()) std::printf("  %s\n", m.c_str());
    expect(errors == 0, "validation errors: " + std::to_string(errors));
    return 0;
}

} // namespace

int main() {
    const int first = RunAll(true);
    if (first == kSkip) return kSkip;
    if (first != 0) return first;
    if (g_failures == 0) {
        std::printf("again on the Vulkan 1.2 path (render passes):\n");
        const int second = RunAll(false);
        if (second != 0 && second != kSkip) return second;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_vulkan_renderer_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_vulkan_renderer_test: all checks passed\n");
    return 0;
}
