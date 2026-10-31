// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Rasterizer.{h,cpp},
// GPU/Software/RasterizerRectangle.cpp and GPU/Software/TransformUnit.h
// (vertex and screen types) @ v1.20.4 (GPL-2.0-or-later).
//
// Single-threaded, without the binner: every primitive is drawn as soon as it
// is submitted, with the scissor range PPSSPP's BinManager would give it.

#pragma once

#include <cstdint>

#include "ge_state.hpp"
#include "memory_view.hpp"
#include "pixel.hpp"
#include "sampler.hpp"

namespace mpo::ge {

// Screen coordinates are 12.4 fixed point.
inline constexpr int kScreenScale = 16;

struct ScreenCoords {
    int x = 0;
    int y = 0;
    int z = 0; // u16 in hardware.
};

struct VertexData {
    float s = 0; // texture coordinates (s, t, q)
    float t = 0;
    float q = 0;
    float clipw = 1.0f;
    std::uint32_t color0 = 0;
    std::uint32_t color1 = 0;
    ScreenCoords screenpos;
    float fogdepth = 1.0f;
};

struct ClipCoords {
    float x = 0;
    float y = 0;
    float z = 0;
    float w = 0;
};

struct ClipVertexData {
    ClipCoords clippos;
    VertexData v;
    [[nodiscard]] bool OutsideRange() const { return v.screenpos.x == 0x7FFFFFFF; }
};

struct BinCoords {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    [[nodiscard]] bool Invalid() const { return x2 < x1 || y2 < y1; }
};

struct RasterizerState {
    PixelState pixel;
    SamplerState sampler;
    bool enable_textures = false;
    int max_tex_level = 0;
    float texture_lod_slope = 0;
    TexLevelMode tex_level_mode = TexLevelMode::kAuto;
    int tex_level_offset = 0;
    bool mip_filt = false;
    bool min_filt = false;
    bool mag_filt = false;
    bool shade_gouraud = false;
    bool through_mode = false;
    bool antialias_lines = false;
    bool texture_proj = false;
};

RasterizerState ComputeRasterizerState(const GeState &gs, const MemoryView &mem,
                                       const std::uint8_t *clut);

// F15b: what Rasterizer::DrawTriangle derives from the three vertices before
// its pixel loop, for a renderer that interpolates the same way elsewhere
// (the Vulkan renderer's exact path). At the sample point (X, Y) = (16x + 7,
// 16y + 7) of pixel (x, y), in 12.4 drawing units, edge i weighs
// w_i = Wrap32(xf[i] * X + yf[i] * Y + c[i]); the pixel is covered when every
// w_i + bias[i] is >= 0, and the values are interpolated with the w_i and
// wsum_recip (InterpolateF).
struct TriangleSetup {
    int xf[3] = {0, 0, 0};
    int yf[3] = {0, 0, 0};
    int c[3] = {0, 0, 0};
    int bias[3] = {0, 0, 0}; // 0 or -1
    float wsum_recip = 0.0f;
    // w0 + w1 + w2 is 0: softgpu still draws the pixels whose sample point
    // lies on the segment (every w_i is 0 there), and everything it
    // interpolates for them is 0 * inf = NaN (x86's default NaN, negative).
    bool zero_area = false;
};
TriangleSetup SetUpTriangle(const VertexData &v0, const VertexData &v1, const VertexData &v2);

// F15b: the same for Rasterizer::DrawRectangle. Its first pixel is
// (min_x / 16, min_y / 16) (12.4 units); it walks 2x2 quads from there, the
// texture coordinates of quad (qx, qy) being row_* plus sty_* added qy times,
// plus stx_* added qx times (float additions, in that order), plus the
// quad's lane offset (0, stx / 2, sty / 2 or stx / 2 + sty / 2). Through mode
// coordinates are already divided by the texture size.
struct RectangleSetup {
    int min_x = 0;
    int min_y = 0;
    float row_s = 0.0f;
    float row_t = 0.0f;
    float stx_s = 0.0f;
    float stx_t = 0.0f;
    float sty_s = 0.0f;
    float sty_t = 0.0f;
};
RectangleSetup SetUpRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                              const RasterizerState &state);

// Draws into the colour/depth buffers described by `state.pixel`.
class Rasterizer {
public:
    explicit Rasterizer(const MemoryView &mem) : mem_(mem) {}

    // Triangle with vertices in counter-clockwise order.
    void DrawTriangle(const VertexData &v0, const VertexData &v1, const VertexData &v2,
                      const BinCoords &range, const RasterizerState &state) const;
    void DrawRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                       const RasterizerState &state) const;
    // Through-mode 1:1 textured or untextured rectangle.
    void DrawSprite(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                    const RasterizerState &state) const;
    void ClearRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                        const RasterizerState &state) const;
    void DrawPoint(const VertexData &v0, const BinCoords &range,
                   const RasterizerState &state) const;
    // Rasterizer.cpp DrawLine: a DDA in double precision over the 12.4
    // coordinates (PPSSPP's own TODO notes it does not handle fractional
    // endpoints exactly; this reproduces it as it is).
    void DrawLine(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                  const RasterizerState &state) const;

private:
    void ApplyTexturing(const RasterizerState &state, const Sampler &sampler, Vec4i prim_color[4],
                        const int mask[4], const float s[4], const float t[4], float w) const;

    const MemoryView &mem_;
};

// Rectangle detection used by the primitive assembler (RasterizerRectangle).
bool RectangleCanUseSprite(const VertexData &v0, const VertexData &v1,
                           const RasterizerState &state);
bool DetectRectangleFromStrip(const RasterizerState &state, const ClipVertexData data[4],
                              int *tl_index, int *br_index);
bool DetectRectangleFromFan(const RasterizerState &state, const ClipVertexData *data, int *tl_index,
                            int *br_index);
bool DetectRectangleFromPair(const RasterizerState &state, const ClipVertexData data[6],
                             int *tl_index, int *br_index);
bool DetectRectangleThroughModeSlices(const RasterizerState &state, const ClipVertexData data[4]);

} // namespace mpo::ge
