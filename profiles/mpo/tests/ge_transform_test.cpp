// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the 3D half of mpo_ge (F12): transform mode, viewport, depth
// range and clamp, -Z clipping, splines and beziers, lines, lighting, fog,
// skinning, REVERSE_SUBTRACT blending and the BOUNDINGBOX test. Every list,
// vertex and expected value is synthetic and computed here (CLAUDE.md §1.1).

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "color.hpp"
#include "ge.hpp"
#include "memory_view.hpp"
#include "sampler.hpp"
#include "spline.hpp"
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

void putf(std::vector<std::uint8_t> &out, float f) {
    put32(out, std::bit_cast<std::uint32_t>(f));
}

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
    [[nodiscard]] std::uint16_t fb16(int x, int y) const {
        return mem.VramRead16(static_cast<std::uint32_t>((y * 512 + x) * 2));
    }
    [[nodiscard]] std::uint32_t fb32(int x, int y) const {
        return mem.VramRead32(static_cast<std::uint32_t>((y * 512 + x) * 4));
    }
    [[nodiscard]] std::uint16_t depth(int x, int y) const {
        return mem.VramRead16(0x88000 + static_cast<std::uint32_t>((y * 512 + x) * 2));
    }
};

constexpr std::uint32_t kListAddr = 0x08100000;
constexpr std::uint32_t kVertexAddr = 0x08200000;
constexpr std::uint32_t kVertexAddr2 = 0x08280000;

// vtypes.
constexpr std::uint32_t kPosFloat = 3u << 7;
constexpr std::uint32_t kCol8888 = 7u << 2;
constexpr std::uint32_t kNrmFloat = 3u << 5;

// Viewport of the PSP's usual 480x272 setup: x = 240 * X + 2048,
// y = -136 * Y + 2048, z = 20000 * Z + 30000, offset (1808, 1912). With
// identity matrices, X/Y in [-1, 1] covers the screen.
constexpr int kOffsetX16 = 1808 * 16;
constexpr int kOffsetY16 = 1912 * 16;

std::vector<std::uint32_t> identity_matrices() {
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

// 480x272 target (5551 or 8888) at VRAM 0, depth at 0x88000, transform
// viewport, depth test GEQUAL with writes, full depth range, Gouraud.
std::vector<std::uint32_t> setup_3d(int fb_format) {
    std::vector<std::uint32_t> w = {
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
        op(kCmdViewportXScale, float24(240.0f)),
        op(kCmdViewportYScale, float24(-136.0f)),
        op(kCmdViewportZScale, float24(20000.0f)),
        op(kCmdViewportXCenter, float24(2048.0f)),
        op(kCmdViewportYCenter, float24(2048.0f)),
        op(kCmdViewportZCenter, float24(30000.0f)),
        op(kCmdOffsetX, static_cast<std::uint32_t>(kOffsetX16)),
        op(kCmdOffsetY, static_cast<std::uint32_t>(kOffsetY16)),
        op(kCmdMinZ, 0),
        op(kCmdMaxZ, 0xFFFF),
        op(kCmdZTestEnable, 1),
        op(kCmdZTest, 7), // GEQUAL
        op(kCmdZWriteDisable, 0),
        op(kCmdMaterialAmbient, 0xFFFFFF),
        op(kCmdMaterialAlpha, 0xFF),
        op(kCmdBase, 0x080000),
    };
    const auto m = identity_matrices();
    w.insert(w.end(), m.begin(), m.end());
    return w;
}

RunResult run_list(Ge &ge, Machine &m, std::vector<std::uint32_t> words,
                   DisplayList *out = nullptr) {
    words.push_back(op(kCmdFinish, 0));
    words.push_back(op(kCmdEnd, 0));
    m.write_words(kListAddr, words);
    DisplayList list;
    list.pc = kListAddr;
    const RunResult r = ge.Run(list);
    if (out) *out = list;
    return r;
}

// Float position + 8888 colour (vtype kPosFloat | kCol8888, stride 16).
void vertex_cp(std::vector<std::uint8_t> &out, std::uint32_t color, float x, float y, float z) {
    put32(out, color);
    putf(out, x);
    putf(out, y);
    putf(out, z);
}

// A strip quad from (x0, y0) to (x1, y1) at depth z.
std::vector<std::uint8_t> quad(float x0, float y0, float x1, float y1, float z,
                               const std::uint32_t colors[4]) {
    std::vector<std::uint8_t> v;
    vertex_cp(v, colors[0], x0, y0, z);
    vertex_cp(v, colors[1], x1, y0, z);
    vertex_cp(v, colors[2], x0, y1, z);
    vertex_cp(v, colors[3], x1, y1, z);
    return v;
}

int count_drawn16(const Machine &m, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) n += (m.fb16(x, y) & 0x7FFF) != 0 ? 1 : 0;
    }
    return n;
}

// A quad from (-0.5, -0.5) to (0.5, 0.5) covers x in [120, 360) and y in
// [68, 204): 240 x 136 pixels.
bool covers_exactly_center_quad(const Machine &m) {
    for (int y = 0; y < 272; ++y) {
        for (int x = 0; x < 480; ++x) {
            const bool drawn = (m.fb16(x, y) & 0x7FFF) != 0;
            const bool inside = x >= 120 && x < 360 && y >= 68 && y < 204;
            if (drawn != inside) return false;
        }
    }
    return true;
}

void test_transform_and_depth() {
    // Gouraud colours (triangles) and a flat colour (detected as a rectangle).
    const std::uint32_t gradient[4] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};
    const std::uint32_t flat[4] = {0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00};
    for (const std::uint32_t *colors : {gradient, flat}) {
        Machine m;
        Ge ge(m.mem);
        m.write(kVertexAddr, quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, colors));
        auto w = setup_3d(1);
        w.insert(w.end(), {op(kCmdVertexType, kPosFloat | kCol8888),
                           op(kCmdVaddr, kVertexAddr & 0xFFFFFF), op(kCmdPrim, (4u << 16) | 4)});
        expect(run_list(ge, m, w).status == RunStatus::kFinished, "3D quad list finishes");
        const bool is_flat = colors == flat;
        expect(covers_exactly_center_quad(m),
               std::string("3D quad covers [120,360)x[68,204) exactly (") +
                   (is_flat ? "rectangle" : "triangles") + ")");
        expect(m.depth(200, 100) == 30000 && m.depth(120, 68) == 30000 &&
                   m.depth(359, 203) == 30000,
               "depth 0.0 -> 30000: " + std::to_string(m.depth(200, 100)));
        expect(m.depth(119, 100) == 0 && m.depth(360, 100) == 0, "depth outside the quad");
        if (is_flat) {
            expect(m.fb16(200, 100) == RGBA8888ToRGBA5551(0xFF00FF00 & 0xFFFFFF),
                   "flat green: " + hex(m.fb16(200, 100)));
            expect(ge.stats().rectangles == 1, "flat 3D strip drawn as a rectangle");
        } else {
            // Top-left on screen is v2 (x0, y1): blue.
            const Vec4i tl = Vec4i::FromRGBA(RGBA5551ToRGBA8888(m.fb16(121, 69)));
            expect(tl.b() > 230 && tl.r() < 30, "Gouraud corner colour");
        }
    }

    // Depth test GEQUAL: nearer (smaller z) fails, farther passes.
    Machine m;
    Ge ge(m.mem);
    const std::uint32_t red[4] = {0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF};
    const std::uint32_t blue[4] = {0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000};
    const std::uint32_t white[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    auto v = quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, red);
    const auto v2 = quad(-0.25f, -0.25f, 0.25f, 0.25f, -0.25f, blue);
    const auto v3 = quad(0.0f, 0.0f, 0.25f, 0.25f, 0.25f, white);
    v.insert(v.end(), v2.begin(), v2.end());
    v.insert(v.end(), v3.begin(), v3.end());
    m.write(kVertexAddr, v);
    auto w = setup_3d(1);
    w.insert(w.end(), {op(kCmdVertexType, kPosFloat | kCol8888),
                       op(kCmdVaddr, kVertexAddr & 0xFFFFFF), op(kCmdPrim, (4u << 16) | 4),
                       op(kCmdPrim, (4u << 16) | 4), op(kCmdPrim, (4u << 16) | 4)});
    run_list(ge, m, w);
    expect(m.fb16(200, 130) == RGBA8888ToRGBA5551(0x0000FF), "z=-0.25 fails GEQUAL (red kept)");
    expect(m.fb16(250, 120) == RGBA8888ToRGBA5551(0xFFFFFF), "z=+0.25 passes GEQUAL (white)");
    expect(m.depth(250, 120) == 35000, "depth 0.25 -> 35000: " + std::to_string(m.depth(250, 120)));

    // Depth range: MAXZ below the fragment depth rejects it.
    for (int max_z : {0xC350, 40000}) {
        Machine mr;
        Ge ger(mr.mem);
        mr.write(kVertexAddr, quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.9f, white));
        auto wr = setup_3d(1);
        wr.insert(wr.end(), {op(kCmdMinZ, 0x2710), op(kCmdMaxZ, static_cast<std::uint32_t>(max_z)),
                             op(kCmdVertexType, kPosFloat | kCol8888),
                             op(kCmdVaddr, kVertexAddr & 0xFFFFFF), op(kCmdPrim, (4u << 16) | 4)});
        run_list(ger, mr, wr);
        const int drawn = count_drawn16(mr, 0, 0, 480, 272);
        expect(max_z == 0xC350 ? drawn == 240 * 136 : drawn == 0,
               "depth range 0x2710-" + hex(static_cast<std::uint32_t>(max_z)) +
                   " at 48000: " + std::to_string(drawn));
    }
}

void test_depth_clamp_and_clipping() {
    // Two vertices beyond the far plane (z = 2 -> 70000), one at z = 0.
    for (int clamp = 0; clamp < 2; ++clamp) {
        Machine m;
        Ge ge(m.mem);
        std::vector<std::uint8_t> v;
        vertex_cp(v, 0xFFFFFFFF, -0.5f, -0.5f, 2.0f);
        vertex_cp(v, 0xFFFFFFFF, 0.5f, -0.5f, 2.0f);
        vertex_cp(v, 0xFFFFFFFF, 0.0f, 0.5f, 0.0f);
        m.write(kVertexAddr, v);
        auto w = setup_3d(1);
        w.insert(w.end(), {op(kCmdDepthClampEnable, static_cast<std::uint32_t>(clamp)),
                           op(kCmdVertexType, kPosFloat | kCol8888),
                           op(kCmdVaddr, kVertexAddr & 0xFFFFFF), op(kCmdPrim, (3u << 16) | 3)});
        run_list(ge, m, w);
        const int drawn = count_drawn16(m, 0, 0, 480, 272);
        if (clamp) {
            expect(drawn > 10000, "depth clamp keeps the triangle: " + std::to_string(drawn));
            // Bottom edge (y = -0.5, row 203) is at the clamped far depth.
            // Row 203 samples just above the bottom edge (65535 there).
            expect(m.depth(240, 203) > 65000 && m.depth(240, 203) < 65535,
                   "clamped depth near the far edge: " + std::to_string(m.depth(240, 203)));
        } else {
            expect(drawn == 0, "without depth clamp the triangle is discarded");
        }
    }

    // A vertex behind the near plane: z = -2 < -w. Clipped at z = -1, the
    // triangle keeps only y <= 0 (rows 136 to 203).
    Machine m;
    Ge ge(m.mem);
    std::vector<std::uint8_t> v;
    vertex_cp(v, 0xFFFFFFFF, -0.5f, -0.5f, 0.0f);
    vertex_cp(v, 0xFFFFFFFF, 0.5f, -0.5f, 0.0f);
    vertex_cp(v, 0xFFFFFFFF, 0.0f, 0.5f, -2.0f);
    m.write(kVertexAddr, v);
    auto w = setup_3d(1);
    w.insert(w.end(), {op(kCmdDepthClampEnable, 1), op(kCmdVertexType, kPosFloat | kCol8888),
                       op(kCmdVaddr, kVertexAddr & 0xFFFFFF), op(kCmdPrim, (3u << 16) | 3)});
    run_list(ge, m, w);
    expect(ge.stats().clipped_triangles >= 1, "the triangle goes through the clipper");
    expect(count_drawn16(m, 0, 0, 480, 136) == 0, "nothing above the clip line (y > 0)");
    // Row 140 (Y = -0.0294): the trapezoid spans X in [-0.2647, 0.2647],
    // pixels [176.5, 303.5].
    expect(m.fb16(180, 140) != 0 && m.fb16(300, 140) != 0, "inside the clipped trapezoid");
    expect(m.fb16(172, 140) == 0 && m.fb16(308, 140) == 0, "outside the clipped trapezoid");
    expect(m.fb16(240, 200) != 0, "bottom of the clipped trapezoid");
}

std::vector<std::uint8_t> patch_grid(int n) {
    std::vector<std::uint8_t> v;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            vertex_cp(v, 0xFF00FF00, -0.5f + static_cast<float>(i) / static_cast<float>(n - 1),
                      -0.5f + static_cast<float>(j) / static_cast<float>(n - 1), 0.0f);
        }
    }
    return v;
}

void test_spline_and_bezier() {
    // A flat 4x4 spline, open on both sides (type 3x3), division 6x6: the
    // same quad as above, as 6 x 6 x 2 triangles.
    for (int spline = 1; spline >= 0; --spline) {
        Machine m;
        Ge ge(m.mem);
        m.write(kVertexAddr, patch_grid(4));
        auto w = setup_3d(1);
        const std::uint32_t patch_op =
            spline ? op(kCmdSpline, (3u << 18) | (3u << 16) | (4u << 8) | 4u)
                   : op(kCmdBezier, (4u << 8) | 4u);
        w.insert(w.end(), {op(kCmdPatchDivision, (6u << 8) | 6u), op(kCmdPatchPrimitive, 0),
                           op(kCmdVertexType, kPosFloat | kCol8888),
                           op(kCmdVaddr, kVertexAddr & 0xFFFFFF), patch_op});
        run_list(ge, m, w);
        const std::string name = spline ? "spline" : "bezier";
        expect(ge.stats().patches == 1, name + " tessellated");
        expect(ge.stats().triangles == 72,
               name + " 6x6x2 triangles: " + std::to_string(ge.stats().triangles));
        expect(covers_exactly_center_quad(m), name + " covers the quad exactly");
        expect(m.fb16(200, 100) == RGBA8888ToRGBA5551(0x00FF00), name + " colour");
        expect(ge.vertex_addr() == kVertexAddr + 16 * 16, name + " advances VADDR by 16 points");
        expect(ge.stats().unsupported.empty(), name + " leaves nothing unsupported");
    }

    // The tessellated vertices of the flat spline. With a single patch open
    // on both sides, PPSSPP's knot divisors (Spline3DWeight::CalcKnots) give
    // f30 = t, f41 = t/2, f52 = f31 = f42 = f32 = t: an asymmetric basis that
    // is not the clamped B-spline (the Bezier basis). Reproduced as it is;
    // the expected positions use that basis, written out independently.
    auto ppsspp_basis = [](double t, double out[4]) {
        const double a = (1 - t) * (1 - t);
        const double b = t * t / 2;
        const double c = (1 - t / 2) * (1 - t);
        const double d = t * t;
        out[0] = (1 - t) * (1 - t) * (1 - t);
        out[1] = 1 - a - b + (a + b + c - 1) * t;
        out[2] = b + (1 - b - c - d) * t;
        out[3] = t * t * t;
    };
    Machine m;
    Ge ge(m.mem);
    m.write(kVertexAddr, patch_grid(4));
    run_list(ge, m,
             {op(kCmdPatchDivision, (6u << 8) | 6u), op(kCmdVertexType, kPosFloat | kCol8888)});
    PatchMesh mesh;
    bool bad = false;
    const bool ok = TessellatePatch(ge.state(), m.mem, (3u << 18) | (3u << 16) | (4u << 8) | 4u,
                                    true, kVertexAddr, 0, &mesh, &bad);
    expect(ok && !bad && mesh.vertices.size() == 49 && mesh.indices.size() == 216,
           "spline mesh 7x7 vertices, 216 indices");
    double max_err = 0;
    for (int j = 0; j < 7 && ok; ++j) {
        for (int i = 0; i < 7; ++i) {
            const DecodedVertex &dv = mesh.vertices[static_cast<std::size_t>(j * 7 + i)];
            double bu[4];
            double bv[4];
            ppsspp_basis(i / 6.0, bu);
            ppsspp_basis(j / 6.0, bv);
            double ex = -0.5;
            double ey = -0.5;
            for (int k = 0; k < 4; ++k) {
                ex += bu[k] * k / 3.0;
                ey += bv[k] * k / 3.0;
            }
            max_err = std::max(max_err, std::abs(dv.pos[0] - ex));
            max_err = std::max(max_err, std::abs(dv.pos[1] - ey));
        }
    }
    expect(max_err < 1e-6,
           "spline vertices follow PPSSPP's basis, max error " + std::to_string(max_err));
    // The asymmetry: the middle vertex is not at the centre.
    const DecodedVertex &mid = mesh.vertices[3 * 7 + 3];
    expect(std::abs(mid.pos[0] - (-0.5f + (0.5f / 3 + 0.25f * 2 / 3 + 0.125f))) < 1e-6f,
           "single-patch open spline: middle vertex at x = -1/24");
    // Corners are exact (the first/last weight is exactly 1).
    expect(mesh.vertices[0].pos[0] == -0.5f && mesh.vertices[48].pos[0] == 0.5f &&
               mesh.vertices[48].pos[1] == 0.5f,
           "spline corners exact");

    // Fewer than 4 points: nothing is drawn and VADDR does not move.
    Machine m2;
    Ge ge2(m2.mem);
    m2.write(kVertexAddr, patch_grid(4));
    auto w = setup_3d(1);
    w.insert(w.end(),
             {op(kCmdVertexType, kPosFloat | kCol8888), op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
              op(kCmdSpline, (3u << 18) | (3u << 16) | (3u << 8) | 4u)});
    run_list(ge2, m2, w);
    expect(ge2.stats().patches == 0 && count_drawn16(m2, 0, 0, 480, 272) == 0,
           "3x4 spline draws nothing");
    expect(ge2.vertex_addr() == kVertexAddr, "3x4 spline does not advance VADDR");
}

void test_lines() {
    Machine m;
    Ge ge(m.mem);
    // Through mode, float positions, no colour: the material ambient colour.
    std::vector<std::uint8_t> v;
    auto p = [&](float x, float y) {
        putf(v, x);
        putf(v, y);
        putf(v, 0.0f);
    };
    p(10, 20);
    p(50, 20);
    p(10, 30);
    p(20, 40);
    p(50, 60);
    p(10, 60);
    m.write(kVertexAddr, v);
    std::vector<std::uint32_t> w = {
        op(kCmdFrameBufPtr, 0),
        op(kCmdFrameBufWidth, 512),
        op(kCmdFrameBufPixFormat, 1),
        op(kCmdScissor1, 0),
        op(kCmdScissor2, (271u << 10) | 479u),
        op(kCmdRegion2, (271u << 10) | 479u),
        op(kCmdMaterialAmbient, 0xFFFFFF),
        op(kCmdMaterialAlpha, 0xFF),
        op(kCmdBase, 0x080000),
        op(kCmdVertexType, 0x800000 | kPosFloat),
        op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
        op(kCmdPrim, (1u << 16) | 6), // LINES: three segments
    };
    run_list(ge, m, w);
    expect(ge.stats().lines == 3, "three line segments");
    bool horizontal = true;
    for (int x = 0; x < 64; ++x) {
        const bool want = x >= 10 && x < 50;
        if (((m.fb16(x, 20) & 0x7FFF) == 0x7FFF) != want) horizontal = false;
        if ((m.fb16(x, 60) == 0x7FFF) != want) horizontal = false;
    }
    expect(horizontal, "horizontal lines cover x in [10, 50) in both directions");
    expect(count_drawn16(m, 0, 19, 64, 20) == 0 && count_drawn16(m, 0, 21, 64, 22) == 0,
           "horizontal line is one pixel thick");
    bool diagonal = true;
    for (int i = 0; i < 10; ++i) diagonal = diagonal && m.fb16(10 + i, 30 + i) == 0x7FFF;
    expect(diagonal && m.fb16(20, 40) == 0 && count_drawn16(m, 0, 25, 64, 45) == 10,
           "diagonal line: 10 pixels, end point excluded");

    // A transform-mode line across the centre row.
    Machine m3;
    Ge ge3(m3.mem);
    std::vector<std::uint8_t> v3;
    vertex_cp(v3, 0xFFFFFFFF, -0.5f, 0.0f, 0.0f);
    vertex_cp(v3, 0xFFFFFFFF, 0.5f, 0.0f, 0.0f);
    m3.write(kVertexAddr, v3);
    auto w3 = setup_3d(1);
    w3.insert(w3.end(), {op(kCmdVertexType, kPosFloat | kCol8888),
                         op(kCmdVaddr, kVertexAddr & 0xFFFFFF), op(kCmdPrim, (2u << 16) | 2)});
    run_list(ge3, m3, w3);
    expect(count_drawn16(m3, 0, 136, 480, 137) == 240 && count_drawn16(m3, 0, 0, 480, 272) == 240,
           "3D line: 240 pixels on row 136, " +
               std::to_string(count_drawn16(m3, 0, 136, 480, 137)));
    expect(m3.fb16(120, 136) != 0 && m3.fb16(359, 136) != 0 && m3.depth(200, 136) == 30000,
           "3D line ends and depth");
}

void test_lighting_and_fog() {
    // One directional light, diffuse only, white; material diffuse 0x80.
    // Normal (0, 0.6, 0.8), light direction (0, 0, 1): factor 0.8 ->
    // ceil(512 * 0.8 + 1) = 411; (511 * 257 * 411) >> 19 = 102.
    struct Case {
        float ny, nz;
        int attspot;
    };
    for (const Case c : {Case{0.0f, 1.0f, 512}, Case{0.6f, 0.8f, 411}}) {
        Machine m;
        Ge ge(m.mem);
        std::vector<std::uint8_t> v;
        auto vert = [&](float x, float y) {
            putf(v, 0.0f);
            putf(v, c.ny);
            putf(v, c.nz);
            putf(v, x);
            putf(v, y);
            putf(v, 0.0f);
        };
        vert(-0.5f, -0.5f);
        vert(0.5f, -0.5f);
        vert(0.0f, 0.5f);
        m.write(kVertexAddr, v);
        auto w = setup_3d(3);
        w.insert(w.end(), {
                              op(kCmdLightingEnable, 1),
                              op(kCmdLightEnable0, 1),
                              op(kCmdLightType0, 0), // directional, diffuse only
                              op(kCmdLx0, float24(0.0f)),
                              op(static_cast<std::uint8_t>(kCmdLx0 + 1), float24(0.0f)),
                              op(static_cast<std::uint8_t>(kCmdLx0 + 2), float24(2.0f)),
                              op(kCmdLac0, 0x000000),
                              op(static_cast<std::uint8_t>(kCmdLac0 + 1), 0xFFFFFF),
                              op(kCmdMaterialUpdate, 0),
                              op(kCmdMaterialEmissive, 0),
                              op(kCmdMaterialAmbient, 0),
                              op(kCmdMaterialAlpha, 0xFF),
                              op(kCmdMaterialDiffuse, 0x808080),
                              op(kCmdAmbientColor, 0),
                              op(kCmdAmbientAlpha, 0xFF),
                              op(kCmdVertexType, kNrmFloat | kPosFloat),
                              op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
                              op(kCmdPrim, (3u << 16) | 3),
                          });
        run_list(ge, m, w);
        const int expected = (511 * 257 * c.attspot) >> 19;
        // The alpha byte of an 8888 target is the stencil (0 here).
        const std::uint32_t want = static_cast<std::uint32_t>(expected) * 0x010101u;
        expect(m.fb32(240, 150) == want, "diffuse lighting " + std::to_string(c.attspot) + ": " +
                                             hex(m.fb32(240, 150)) + " want " + hex(want));
    }

    // Fog: identity matrices, fog end 0.501953125, slope 1 -> fog depth
    // 128.5/256 -> 128; white over black fog: (255*128 + 255) / 256 = 128.
    Machine m;
    Ge ge(m.mem);
    std::vector<std::uint8_t> v;
    vertex_cp(v, 0xFFFFFFFF, -0.5f, -0.5f, 0.0f);
    vertex_cp(v, 0xFFFFFFFF, 0.5f, -0.5f, 0.0f);
    vertex_cp(v, 0xFFFFFFFF, 0.0f, 0.5f, 0.0f);
    m.write(kVertexAddr, v);
    auto w = setup_3d(3);
    w.insert(w.end(), {op(kCmdFogEnable, 1), op(kCmdFog1, float24(0.501953125f)),
                       op(kCmdFog2, float24(1.0f)), op(kCmdFogColor, 0x000000),
                       op(kCmdVertexType, kPosFloat | kCol8888),
                       op(kCmdVaddr, kVertexAddr & 0xFFFFFF), op(kCmdPrim, (3u << 16) | 3)});
    run_list(ge, m, w);
    expect(m.fb32(240, 150) == 0x00808080u, "fog 128 halves white: " + hex(m.fb32(240, 150)));
}

void test_skinning_decode() {
    // 8-bit weights, 2 bones: bone 0 translates +X, bone 1 translates +Y.
    // Weights 0.5/0.5 on the origin give (0.5, 0.5, 0).
    Machine m;
    std::vector<std::uint8_t> v = {64, 64, 0, 0}; // two u8 weights, padding to float
    putf(v, 0.0f);
    putf(v, 0.0f);
    putf(v, 0.0f);
    m.write(kVertexAddr, v);
    const std::uint32_t vtype = (1u << 9) | (1u << 14) | kPosFloat; // 8-bit weights x2
    const VertexDecoder dec(vtype);
    expect(dec.stride() == 16, "weights + float position stride " + std::to_string(dec.stride()));
    float bones[96] = {};
    const float b0[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0};
    const float b1[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0};
    for (int i = 0; i < 12; ++i) {
        bones[i] = b0[i];
        bones[12 + i] = b1[i];
    }
    DecodeParams params;
    params.bones = bones;
    DecodedVertex out;
    expect(dec.Decode(m.mem, kVertexAddr, params, &out), "skinned decode");
    expect(out.pos[0] == 0.5f && out.pos[1] == 0.5f && out.pos[2] == 0.0f,
           "skinned position " + std::to_string(out.pos[0]) + ", " + std::to_string(out.pos[1]));
    DecodedVertex raw;
    expect(dec.Decode(m.mem, kVertexAddr, DecodeParams{}, &raw) && raw.pos[0] == 0.0f &&
               raw.weights[0] == 0.5f,
           "decode without bones keeps the model position");
}

void test_reverse_subtract_blend() {
    Machine m;
    Ge ge(m.mem);
    std::vector<std::uint8_t> v;
    // Through-mode RECTANGLES, colour + s16 position (vtype 0x80011C).
    auto vert = [&](std::uint32_t color, int x, int y) {
        put32(v, color);
        put16(v, static_cast<std::uint16_t>(x));
        put16(v, static_cast<std::uint16_t>(y));
        put16(v, 0);
        put16(v, 0);
    };
    vert(0xFF808080, 0, 0);
    vert(0xFF808080, 32, 32);
    vert(0x80606060, 8, 8);
    vert(0x80606060, 24, 24);
    m.write(kVertexAddr, v);
    std::vector<std::uint32_t> w = {
        op(kCmdFrameBufPtr, 0),
        op(kCmdFrameBufWidth, 512),
        op(kCmdFrameBufPixFormat, 3),
        op(kCmdScissor1, 0),
        op(kCmdScissor2, (271u << 10) | 479u),
        op(kCmdRegion2, (271u << 10) | 479u),
        op(kCmdBase, 0x080000),
        op(kCmdVertexType, 0x80011C),
        op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
        op(kCmdPrim, (6u << 16) | 2), // destination colour, no blending
        op(kCmdAlphaBlendEnable, 1),
        op(kCmdBlendMode, (2u << 8) | (10u << 4) | 2u), // REVERSE_SUBTRACT(SRCALPHA, FIX)
        op(kCmdBlendFixedB, 0xFFFFFF),
        op(kCmdPrim, (6u << 16) | 2),
    };
    run_list(ge, m, w);
    // s = ((2*96+1) * (2*128+1)) >> 10 = 48; d = ((2*128+1) * 511) >> 10 = 128.
    const int s = ((2 * 0x60 + 1) * (2 * 0x80 + 1)) >> 10;
    const int d = ((2 * 0x80 + 1) * (2 * 255 + 1)) >> 10;
    const auto c = static_cast<std::uint32_t>(d - s);
    // The alpha byte keeps the stencil (0), it is not blended.
    expect(m.fb32(16, 16) == c * 0x010101u,
           "REVERSE_SUBTRACT: " + hex(m.fb32(16, 16)) + " want " + hex(c * 0x010101u));
    expect(m.fb32(4, 4) == 0x00808080u, "outside the blended rectangle");
}

void test_bounding_box() {
    // Eight corners of a box: inside the view, then far to the right.
    for (int culled = 0; culled < 2; ++culled) {
        Machine m;
        Ge ge(m.mem);
        std::vector<std::uint8_t> v;
        const float x0 = culled ? 2.0f : -0.5f;
        for (int i = 0; i < 8; ++i) {
            putf(v, x0 + static_cast<float>(i & 1));
            putf(v, (i & 2) ? 0.5f : -0.5f);
            putf(v, (i & 4) ? 0.5f : -0.5f);
        }
        m.write(kVertexAddr2, v);
        auto w = setup_3d(1);
        w.insert(w.end(), {op(kCmdVertexType, kPosFloat), op(kCmdVaddr, kVertexAddr2 & 0xFFFFFF),
                           op(kCmdBoundingBox, 8)});
        DisplayList list;
        run_list(ge, m, w, &list);
        expect(list.bbox_result == (culled == 0),
               std::string("bounding box ") + (culled ? "outside is culled" : "inside is visible"));
        expect(ge.vertex_addr() == kVertexAddr2 + 8 * 12, "bounding box advances VADDR");
        expect(ge.stats().bbox_tests == 1 && ge.stats().unsupported.empty(),
               "bounding box tested, nothing unsupported");
    }
}

void test_texture_function_saturation() {
    // Zero-area triangles interpolate NaN colours (INT_MIN). The x86-64 JIT
    // saturates them to 16 bits and multiplies in 16-bit lanes:
    // (u16)(-32768 + 1) = 32769; (32769 * 255) & 0xFFFF = 33023; >> 7 = 257.
    SamplerState st;
    st.tex_func = TexFunc::kModulate;
    st.use_texture_alpha = true;
    st.use_color_doubling = true;
    const int kMin = static_cast<int>(0x80000000u);
    const Vec4i out = TextureFunctionOutput(Vec4i{{kMin, kMin, kMin, kMin}}, Vec4i::All(255), st);
    expect(out[0] == 257 && out[1] == 257 && out[2] == 257 && out[3] == 128,
           "MODULATE x2 with a saturated colour: " + std::to_string(out[0]) + ", " +
               std::to_string(out[3]));
    // In range it is the usual ((prim + 1) * tex * 2) >> 8.
    const Vec4i ok =
        TextureFunctionOutput(Vec4i{{128, 0, 255, 255}}, Vec4i{{200, 77, 255, 255}}, st);
    expect(ok[0] == (129 * 400) / 256 && ok[1] == (1 * 154) / 256 && ok[2] == 510 &&
               ok[3] == (256 * 255) / 256,
           "MODULATE x2 in range");
    st.use_color_doubling = false;
    st.use_texture_alpha = false;
    const Vec4i plain = TextureFunctionOutput(Vec4i{{100, 50, 25, kMin}}, Vec4i::All(128), st);
    expect(plain[0] == (101 * 128) / 256 && plain[3] == 0x8000,
           "MODULATE without texture alpha keeps the (zero-extended) primary alpha");
}

} // namespace

int main() {
    try {
        test_transform_and_depth();
        test_depth_clamp_and_clipping();
        test_spline_and_bezier();
        test_lines();
        test_lighting_and_fog();
        test_skinning_decode();
        test_reverse_subtract_blend();
        test_bounding_box();
        test_texture_function_saturation();
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "FAIL: uncaught exception: %s\n", exception.what());
        return EXIT_FAILURE;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("mpo_ge_transform_test: all checks passed");
    return EXIT_SUCCESS;
}
