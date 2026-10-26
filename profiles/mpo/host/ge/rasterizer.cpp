// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Rasterizer.cpp and
// GPU/Software/RasterizerRectangle.cpp @ v1.20.4 (GPL-2.0-or-later).
//
// Arithmetic follows the x86-64 build of PPSSPP (the one the reference
// captures come from): colour and depth interpolation round to nearest
// (CVTPS2DQ), edge functions wrap at 32 bits (PMULLD/PADDD).

#include "rasterizer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace mpo::ge {
namespace {

int Wrap32(std::int64_t v) {
    return static_cast<int>(static_cast<std::uint32_t>(v));
}

int ScreenToDrawing(int v) {
    return v / kScreenScale;
}

std::uint8_t ClampFogDepth(float fogdepth) {
    const auto u = std::bit_cast<std::uint32_t>(fogdepth);
    const std::uint32_t exp = u >> 23;
    if ((u & 0x80000000u) != 0 || exp <= 126 - 8) return 0;
    if (exp > 126) return 255;
    const std::uint32_t mantissa = (u & 0x007FFFFFu) | 0x00800000u;
    return static_cast<std::uint8_t>(mantissa >> (16 + 126 - exp));
}

// Produces a signed 1.27.4 value.
int TexLog2(float delta) {
    const auto u = std::bit_cast<std::uint32_t>(delta);
    const int useful = static_cast<int>((u >> 19) & 0x0FFF);
    return useful - 127 * 16;
}

void CalculateSamplingParams(float ds, float dt, float w, const RasterizerState &state, int *level,
                             int *level_frac, bool *filt) {
    const auto width = static_cast<float>(1 << state.sampler.width0_shift);
    const auto height = static_cast<float>(1 << state.sampler.height0_shift);
    int detail;
    switch (state.tex_level_mode) {
    case TexLevelMode::kAuto:
        detail = TexLog2(std::max(std::abs(ds * width), std::abs(dt * height)));
        break;
    case TexLevelMode::kSlope:
        detail = TexLog2(2.0f * w * state.texture_lod_slope);
        break;
    case TexLevelMode::kConst:
    default:
        detail = 0;
        break;
    }
    detail += state.tex_level_offset;
    if (detail > 0 && state.max_tex_level > 0) {
        int level8 = std::min(detail, state.max_tex_level * 16);
        if (!state.mip_filt) level8 += 8;
        *level = level8 >> 4;
        *level_frac = state.mip_filt ? level8 & 0xF : 0;
    } else {
        *level = 0;
        *level_frac = 0;
    }
    *filt = detail > 0 ? state.min_filt : state.mag_filt;
}

bool IsRightSideOrFlatBottomLine(const ScreenCoords &vertex, const ScreenCoords &line1,
                                 const ScreenCoords &line2) {
    if (line1.y == line2.y) return vertex.y < line1.y;
    const std::int64_t x =
        static_cast<std::int64_t>(line1.x) +
        static_cast<std::int64_t>(line2.x - line1.x) * (vertex.y - line1.y) / (line2.y - line1.y);
    return vertex.x < x;
}

struct TriangleEdge {
    int step_x = 0;
    int step_y = 0;

    void Start(const ScreenCoords &v0, const ScreenCoords &v1, int origin_x, int origin_y,
               int out[4]) {
        constexpr int kCenterOff = (kScreenScale / 2) - 1;
        constexpr int kCenterPlus1 = kScreenScale + kCenterOff;
        const int init_x[4] = {origin_x + kCenterOff, origin_x + kCenterPlus1,
                               origin_x + kCenterOff, origin_x + kCenterPlus1};
        const int init_y[4] = {origin_y + kCenterOff, origin_y + kCenterOff,
                               origin_y + kCenterPlus1, origin_y + kCenterPlus1};
        const std::int64_t xf = static_cast<std::int64_t>(v0.y) - v1.y;
        const std::int64_t yf = static_cast<std::int64_t>(v1.x) - v0.x;
        const std::int64_t c =
            static_cast<std::int64_t>(v1.y) * v0.x - static_cast<std::int64_t>(v1.x) * v0.y;
        step_x = Wrap32(xf * kScreenScale * 2);
        step_y = Wrap32(yf * kScreenScale * 2);
        for (int i = 0; i < 4; ++i) out[i] = Wrap32(xf * init_x[i] + yf * init_y[i] + c);
    }

    void NarrowMinMaxX(const int w[4], std::int64_t min_x, std::int64_t *row_min_x,
                       std::int64_t *row_max_x) const {
        const std::int64_t wmax = std::max(std::max(w[0], w[1]), std::max(w[2], w[3]));
        if (wmax < 0) {
            if (step_x > 0) {
                const std::int64_t steps = -wmax / step_x;
                *row_min_x = std::max(*row_min_x, min_x + steps * kScreenScale * 2);
            } else {
                *row_min_x = *row_max_x + 1;
            }
        }
        if (wmax >= 0 && step_x < 0) {
            const std::int64_t steps = (-wmax / step_x) + 1;
            *row_max_x = std::min(*row_max_x, min_x + steps * kScreenScale * 2);
        }
    }
};

bool AnyMask(const int mask[4]) {
    return mask[0] >= 0 || mask[1] >= 0 || mask[2] >= 0 || mask[3] >= 0;
}

// Interpolate (SSE): ((c0 * w0 + c1 * w1) + c2 * w2) * wsum, as floats.
float InterpolateF(float c0, float c1, float c2, int w0, int w1, int w2, float wsum) {
    float v = c0 * static_cast<float>(w0);
    v = v + c1 * static_cast<float>(w1);
    v = v + c2 * static_cast<float>(w2);
    return v * wsum;
}

Vec4i InterpolateColor(const Vec4i &c0, const Vec4i &c1, const Vec4i &c2, int w0, int w1, int w2,
                       float wsum, int lanes) {
    Vec4i out;
    for (int i = 0; i < lanes; ++i) {
        out[i] = RoundToInt(InterpolateF(static_cast<float>(c0[i]), static_cast<float>(c1[i]),
                                         static_cast<float>(c2[i]), w0, w1, w2, wsum));
    }
    return out;
}

// Through-mode fast pixel write used by DrawSprite (RasterizerRectangle.cpp):
// only SRCALPHA/INVSRCALPHA blending, skipped when alpha is 255.
std::uint32_t StandardAlphaBlend(std::uint32_t source, std::uint32_t dst) {
    const int a = static_cast<int>(source >> 24);
    const Vec4i s = Vec4i::FromRGBA(source);
    const Vec4i d = Vec4i::FromRGBA(dst);
    Vec4i out;
    for (int i = 0; i < 3; ++i) {
        out[i] =
            (((s[i] * 2 + 1) * (a * 2 + 1)) >> 10) + (((d[i] * 2 + 1) * ((255 - a) * 2 + 1)) >> 10);
    }
    return out.ToRGB();
}

void DrawSinglePixelFast(const RenderTarget &rt, BufferFormat fmt, bool alpha_blend, int x, int y,
                         std::uint32_t color_in) {
    const bool opaque = !alpha_blend || (color_in >> 24) == 255;
    switch (fmt) {
    case BufferFormat::k8888: {
        const std::uint32_t old = rt.Get32(x, y);
        std::uint32_t c = opaque ? (color_in & 0xFFFFFF) : StandardAlphaBlend(color_in, old);
        rt.Set32(x, y, c | (old & 0xFF000000));
        return;
    }
    case BufferFormat::k565: {
        const std::uint16_t old = rt.Get16(x, y);
        const std::uint32_t c =
            opaque ? (color_in & 0xFFFFFF) : StandardAlphaBlend(color_in, RGB565ToRGBA8888(old));
        rt.Set16(x, y, RGBA8888ToRGB565(c));
        return;
    }
    case BufferFormat::k5551: {
        const std::uint16_t old = rt.Get16(x, y);
        const std::uint32_t c =
            opaque ? (color_in & 0xFFFFFF) : StandardAlphaBlend(color_in, RGBA5551ToRGBA8888(old));
        rt.Set16(x, y, static_cast<std::uint16_t>(RGBA8888ToRGBA555X(c) | (old & 0x8000)));
        return;
    }
    case BufferFormat::k4444: {
        const std::uint16_t old = rt.Get16(x, y);
        const std::uint32_t c =
            opaque ? (color_in & 0xFFFFFF) : StandardAlphaBlend(color_in, RGBA4444ToRGBA8888(old));
        rt.Set16(x, y, static_cast<std::uint16_t>(RGBA8888ToRGBA444X(c) | (old & 0xF000)));
        return;
    }
    }
}

bool AlphaTestIsNeedless(const PixelState &ps) {
    switch (ps.alpha_test_func) {
    case Comparison::kNever:
    case Comparison::kEqual:
    case Comparison::kLess:
    case Comparison::kLequal:
        return false;
    case Comparison::kAlways:
        return true;
    case Comparison::kNotEqual:
    case Comparison::kGreater:
    case Comparison::kGequal:
        return !(ps.alpha_test_ref != 0 || ps.has_alpha_test_mask);
    }
    return false;
}

bool UseDrawSinglePixel(const PixelState &ps) {
    if (ps.clear_mode || ps.color_test || ps.stencil_test) return false;
    if (!AlphaTestIsNeedless(ps) || ps.depth_test_func != Comparison::kAlways) return false;
    if (ps.alpha_blend) {
        if (ps.blend_eq != BlendMode::kAdd || ps.blend_src != PixelBlendFactor::kSrcAlpha)
            return false;
        if (ps.blend_dst != PixelBlendFactor::kInvSrcAlpha) return false;
    }
    if (ps.depth_write || ps.apply_fog) return false;
    if (ps.dithering || ps.apply_logic_op || ps.apply_color_write_mask) return false;
    return true;
}

// ModulateRGBA (x86-64): ((prim + 1) * tex) >> 8 with RGB doubling.
Vec4i ModulateRGBA(const Vec4i &prim, const Vec4i &tex, const SamplerState &st) {
    Vec4i out;
    for (int i = 0; i < 4; ++i) {
        const int t = (st.use_color_doubling && i < 3) ? tex[i] * 2 : tex[i];
        out[i] = ((prim[i] + 1) * t) >> 8;
    }
    return out;
}

bool NoClampOrWrap(const RasterizerState &state, float s, float t) {
    if (s < 0 || t < 0) return false;
    const auto &size = state.sampler.sizes[0];
    if (size.w > 512 || size.h > 512) return false;
    return s <= static_cast<float>(size.w) && t <= static_cast<float>(size.h);
}

} // namespace

RasterizerState ComputeRasterizerState(const GeState &gs, const MemoryView &mem,
                                       const std::uint8_t *clut) {
    RasterizerState state;
    state.pixel = ComputePixelState(gs);
    state.enable_textures = gs.IsTextureMapEnabled() && !state.pixel.clear_mode;
    if (state.enable_textures) {
        state.sampler = ComputeSamplerState(gs, mem, clut);
        state.max_tex_level = state.sampler.has_any_mips ? gs.TextureMaxLevel() : 0;
        state.texture_lod_slope = gs.TextureLodSlope();
        state.tex_level_mode = gs.GetTexLevelMode();
        state.tex_level_offset = gs.TexLevelOffset16();
        state.mip_filt = gs.IsMipmapFilteringEnabled();
        state.min_filt = gs.IsMinifyFilteringEnabled();
        state.mag_filt = gs.IsMagnifyFilteringEnabled();
        state.texture_proj = gs.UVGenMode() == TexMapMode::kTextureMatrix;
        if (state.texture_proj) {
            const auto &m = gs.tgen_matrix;
            const bool q_zero_st = m[2] == 0.0f && m[5] == 0.0f;
            const bool q_zero_q = m[8] == 0.0f;
            const bool q_factor_zero = ((gs.Reg(kCmdTexMapMode) >> 8) & 3) == 1; // GE_PROJMAP_UV
            if (q_zero_st && (q_zero_q || q_factor_zero) && m[11] == 1.0f)
                state.texture_proj = false;
        }
    }
    state.shade_gouraud = !gs.IsModeClear() && gs.IsShadeGouraud();
    state.through_mode = gs.IsModeThrough();
    state.antialias_lines = gs.IsAntiAliasEnabled();
    return state;
}

void Rasterizer::ApplyTexturing(const RasterizerState &state, const Sampler &sampler,
                                Vec4i prim_color[4], const int mask[4], const float s[4],
                                const float t[4], float w) const {
    const float ds = s[1] - s[0];
    const float dt = t[2] - t[0];
    int level;
    int level_frac;
    bool bilinear;
    CalculateSamplingParams(ds, dt, w, state, &level, &level_frac, &bilinear);
    for (int i = 0; i < 4; ++i) {
        if (mask[i] < 0) continue;
        prim_color[i] = bilinear ? sampler.Linear(s[i], t[i], prim_color[i], level, level_frac)
                                 : sampler.Nearest(s[i], t[i], prim_color[i], level, level_frac);
    }
}

void Rasterizer::DrawTriangle(const VertexData &v0, const VertexData &v1, const VertexData &v2,
                              const BinCoords &range, const RasterizerState &state) const {
    const PixelState &ps = state.pixel;
    const bool clear_mode = ps.clear_mode;
    const RenderTarget rt(mem_, ps);
    const Sampler sampler(mem_, state.sampler);

    const int bias0 =
        IsRightSideOrFlatBottomLine(v0.screenpos, v1.screenpos, v2.screenpos) ? -1 : 0;
    const int bias1 =
        IsRightSideOrFlatBottomLine(v1.screenpos, v2.screenpos, v0.screenpos) ? -1 : 0;
    const int bias2 =
        IsRightSideOrFlatBottomLine(v2.screenpos, v0.screenpos, v1.screenpos) ? -1 : 0;

    TriangleEdge e0;
    TriangleEdge e1;
    TriangleEdge e2;
    const std::int64_t min_x = range.x1;
    const std::int64_t max_x = range.x2;
    const std::int64_t min_y = range.y1;
    const std::int64_t max_y = range.y2;

    int w0_base[4];
    int w1_base[4];
    int w2_base[4];
    e0.Start(v1.screenpos, v2.screenpos, range.x1, range.y1, w0_base);
    e1.Start(v2.screenpos, v0.screenpos, range.x1, range.y1, w1_base);
    e2.Start(v0.screenpos, v1.screenpos, range.x1, range.y1, w2_base);

    // The sum of weights stays constant across the triangle.
    float wsum_recip[4];
    for (int i = 0; i < 4; ++i) {
        const int sum = Wrap32(static_cast<std::int64_t>(w0_base[i]) + w1_base[i] + w2_base[i]);
        wsum_recip[i] = 1.0f / static_cast<float>(sum);
    }

    const bool flat_z = v0.screenpos.z == v1.screenpos.z && v0.screenpos.z == v2.screenpos.z;
    const bool flat_color_all = !state.shade_gouraud;
    const bool flat_color0 = flat_color_all || (v0.color0 == v1.color0 && v0.color0 == v2.color0);
    const bool flat_color1 = flat_color_all || (v0.color1 == v1.color1 && v0.color1 == v2.color1);
    const bool no_fog = clear_mode || !ps.apply_fog ||
                        (v0.fogdepth >= 1.0f && v1.fogdepth >= 1.0f && v2.fogdepth >= 1.0f);

    if (ps.apply_depth_range && flat_z) {
        if (v0.screenpos.z < ps.minz || v0.screenpos.z > ps.maxz) return;
    }

    const Vec4i v0_c0 = Vec4i::FromRGBA(v0.color0);
    const Vec4i v1_c0 = Vec4i::FromRGBA(v1.color0);
    const Vec4i v2_c0 = Vec4i::FromRGBA(v2.color0);
    const Vec4i v0_c1 = Vec4i::FromRGBA(v0.color1 & 0xFFFFFF);
    const Vec4i v1_c1 = Vec4i::FromRGBA(v1.color1 & 0xFFFFFF);
    const Vec4i v2_c1 = Vec4i::FromRGBA(v2.color1 & 0xFFFFFF);

    for (std::int64_t cur_y = min_y; cur_y <= max_y; cur_y += kScreenScale * 2) {
        int w0[4];
        int w1[4];
        int w2[4];
        std::memcpy(w0, w0_base, sizeof(w0));
        std::memcpy(w1, w1_base, sizeof(w1));
        std::memcpy(w2, w2_base, sizeof(w2));

        int px = ScreenToDrawing(static_cast<int>(min_x));
        const int py = ScreenToDrawing(static_cast<int>(cur_y));

        std::int64_t row_min_x = min_x;
        std::int64_t row_max_x = max_x;
        e0.NarrowMinMaxX(w0, min_x, &row_min_x, &row_max_x);
        e1.NarrowMinMaxX(w1, min_x, &row_min_x, &row_max_x);
        e2.NarrowMinMaxX(w2, min_x, &row_min_x, &row_max_x);

        const auto skip_x = static_cast<int>((row_min_x - min_x) / (kScreenScale * 2));
        for (int i = 0; i < 4; ++i) {
            w0[i] = Wrap32(w0[i] + static_cast<std::int64_t>(e0.step_x) * skip_x);
            w1[i] = Wrap32(w1[i] + static_cast<std::int64_t>(e1.step_x) * skip_x);
            w2[i] = Wrap32(w2[i] + static_cast<std::int64_t>(e2.step_x) * skip_x);
        }
        px = (px + 2 * skip_x) & 0x3FF;

        const int scissor_y_plus1 = cur_y + kScreenScale > max_y ? -1 : 0;
        const int row_span = Wrap32(row_max_x - row_min_x - kScreenScale);
        int scissor_mask[4] = {0, row_span, scissor_y_plus1, row_span | scissor_y_plus1};

        for (std::int64_t cur_x = row_min_x; cur_x <= row_max_x; cur_x += kScreenScale * 2) {
            int mask[4];
            for (int i = 0; i < 4; ++i) {
                mask[i] = Wrap32(static_cast<std::int64_t>(w0[i]) + bias0) |
                          Wrap32(static_cast<std::int64_t>(w1[i]) + bias1) |
                          Wrap32(static_cast<std::int64_t>(w2[i]) + bias2) | scissor_mask[i];
            }
            if (AnyMask(mask)) {
                int z[4];
                if (flat_z) {
                    for (int &zi : z) zi = v2.screenpos.z;
                } else {
                    for (int i = 0; i < 4; ++i) {
                        float zf = static_cast<float>(w0[i]) * static_cast<float>(v0.screenpos.z);
                        zf = zf + static_cast<float>(w1[i]) * static_cast<float>(v1.screenpos.z);
                        zf = zf + static_cast<float>(w2[i]) * static_cast<float>(v2.screenpos.z);
                        z[i] = RoundToInt(zf * wsum_recip[i]);
                    }
                }

                bool any = true;
                if (ps.early_z_checks) {
                    if (ps.apply_depth_range) {
                        for (int i = 0; i < 4; ++i) {
                            if (z[i] < ps.minz || z[i] > ps.maxz) mask[i] = -1;
                        }
                    }
                    if (AnyMask(mask)) {
                        for (int i = 0; i < 4; ++i) {
                            const int x = px + (i & 1);
                            const int y = py + (i / 2);
                            // CheckDepthTestPassed4 compares the raw u16 depth.
                            if (!DepthTestPassed(rt, ps.depth_test_func, x, y, z[i])) mask[i] = -1;
                        }
                    }
                    any = AnyMask(mask);
                }

                if (any) {
                    Vec4i prim_color[4];
                    if (!flat_color0) {
                        for (int i = 0; i < 4; ++i) {
                            if (mask[i] >= 0) {
                                prim_color[i] = InterpolateColor(v0_c0, v1_c0, v2_c0, w0[i], w1[i],
                                                                 w2[i], wsum_recip[i], 4);
                            }
                        }
                    } else {
                        for (Vec4i &c : prim_color) c = v2_c0;
                    }
                    Vec4i sec_color[4];
                    if (!flat_color1) {
                        for (int i = 0; i < 4; ++i) {
                            if (mask[i] >= 0) {
                                sec_color[i] = InterpolateColor(v0_c1, v1_c1, v2_c1, w0[i], w1[i],
                                                                w2[i], wsum_recip[i], 3);
                            }
                        }
                    } else {
                        for (Vec4i &c : sec_color) c = v2_c1;
                    }

                    if (state.enable_textures && !clear_mode) {
                        float s[4];
                        float t[4];
                        if (state.through_mode) {
                            const float sw =
                                1.0f / static_cast<float>(1 << state.sampler.width0_shift);
                            const float th =
                                1.0f / static_cast<float>(1 << state.sampler.height0_shift);
                            for (int i = 0; i < 4; ++i) {
                                s[i] = InterpolateF(v0.s, v1.s, v2.s, w0[i], w1[i], w2[i],
                                                    wsum_recip[i]) *
                                       sw;
                                t[i] = InterpolateF(v0.t, v1.t, v2.t, w0[i], w1[i], w2[i],
                                                    wsum_recip[i]) *
                                       th;
                            }
                        } else {
                            // Perspective-correct (GetTextureCoordinates / ...Proj).
                            const float q0 = 1.0f / v0.clipw;
                            const float q1 = 1.0f / v1.clipw;
                            const float q2 = 1.0f / v2.clipw;
                            for (int i = 0; i < 4; ++i) {
                                const float wq0 = static_cast<float>(w0[i]) * q0;
                                const float wq1 = static_cast<float>(w1[i]) * q1;
                                const float wq2 = static_cast<float>(w2[i]) * q2;
                                float q_recip;
                                if (state.texture_proj) {
                                    float qv = wq0 * v0.q;
                                    qv = qv + wq1 * v1.q;
                                    qv = qv + wq2 * v2.q;
                                    q_recip = 1.0f / qv;
                                } else {
                                    q_recip = 1.0f / (wq0 + wq1 + wq2);
                                }
                                float sv = wq0 * v0.s;
                                sv = sv + wq1 * v1.s;
                                sv = sv + wq2 * v2.s;
                                float tv = wq0 * v0.t;
                                tv = tv + wq1 * v1.t;
                                tv = tv + wq2 * v2.t;
                                s[i] = sv * q_recip;
                                t[i] = tv * q_recip;
                            }
                        }
                        float clipw = 0.0f;
                        if (state.tex_level_mode == TexLevelMode::kSlope) {
                            clipw = (v0.clipw * static_cast<float>(w0[0]) +
                                     v1.clipw * static_cast<float>(w1[0]) +
                                     v2.clipw * static_cast<float>(w2[0])) *
                                    wsum_recip[0];
                        }
                        ApplyTexturing(state, sampler, prim_color, mask, s, t, clipw);
                    }

                    if (!clear_mode) {
                        // PADDD: wraps (both can be INT_MIN on zero-area triangles).
                        for (int i = 0; i < 4; ++i) {
                            for (int c = 0; c < 3; ++c) {
                                prim_color[i][c] = Wrap32(
                                    static_cast<std::int64_t>(prim_color[i][c]) + sec_color[i][c]);
                            }
                        }
                    }

                    int fog[4] = {255, 255, 255, 255};
                    if (!no_fog) {
                        for (int i = 0; i < 4; ++i) {
                            float f = static_cast<float>(w0[i]) * v0.fogdepth;
                            f = f + static_cast<float>(w1[i]) * v1.fogdepth;
                            f = f + static_cast<float>(w2[i]) * v2.fogdepth;
                            fog[i] = ClampFogDepth(f * wsum_recip[i]);
                        }
                    }

                    for (int i = 0; i < 4; ++i) {
                        if (mask[i] < 0) continue;
                        DrawPixel(rt, px + (i & 1), py + (i / 2), z[i], fog[i], prim_color[i], ps);
                    }
                }
            }

            for (int i = 0; i < 4; ++i) {
                w0[i] = Wrap32(static_cast<std::int64_t>(w0[i]) + e0.step_x);
                w1[i] = Wrap32(static_cast<std::int64_t>(w1[i]) + e1.step_x);
                w2[i] = Wrap32(static_cast<std::int64_t>(w2[i]) + e2.step_x);
            }
            scissor_mask[1] -= kScreenScale * 2;
            scissor_mask[3] -= kScreenScale * 2;
            px = (px + 2) & 0x3FF;
        }

        for (int i = 0; i < 4; ++i) {
            w0_base[i] = Wrap32(static_cast<std::int64_t>(w0_base[i]) + e0.step_y);
            w1_base[i] = Wrap32(static_cast<std::int64_t>(w1_base[i]) + e1.step_y);
            w2_base[i] = Wrap32(static_cast<std::int64_t>(w2_base[i]) + e2.step_y);
        }
    }
}

void Rasterizer::DrawRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                               const RasterizerState &state) const {
    const PixelState &ps = state.pixel;
    const RenderTarget rt(mem_, ps);
    const Sampler sampler(mem_, state.sampler);

    const int entire_x1 = std::min(v0.screenpos.x, v1.screenpos.x);
    const int entire_y1 = std::min(v0.screenpos.y, v1.screenpos.y);
    const int entire_x2 = std::max(v0.screenpos.x, v1.screenpos.x) - 1;
    const int entire_y2 = std::max(v0.screenpos.y, v1.screenpos.y) - 1;
    int min_x = std::max(entire_x1 & ~(kScreenScale - 1), range.x1) | (kScreenScale / 2 - 1);
    int min_y = std::max(entire_y1 & ~(kScreenScale - 1), range.y1) | (kScreenScale / 2 - 1);
    const int max_x = std::min(entire_x2, range.x2);
    const int max_y = std::min(entire_y2, range.y2);
    if (min_x < entire_x1 - 1) min_x += kScreenScale;
    if (min_y < entire_y1 - 1) min_y += kScreenScale;

    float row_s = 0;
    float row_t = 0;
    float stx_s = 0;
    float stx_t = 0;
    float sty_s = 0;
    float sty_t = 0;
    if (state.enable_textures) {
        float tc0s = v0.s;
        float tc0t = v0.t;
        float tc1s = v1.s;
        float tc1t = v1.t;
        if (state.through_mode) {
            const float sw = 1.0f / static_cast<float>(1 << state.sampler.width0_shift);
            const float th = 1.0f / static_cast<float>(1 << state.sampler.height0_shift);
            tc0s *= sw;
            tc1s *= sw;
            tc0t *= th;
            tc1t *= th;
        }
        const float diff_x =
            static_cast<float>(entire_x2 - entire_x1 + 1) / static_cast<float>(kScreenScale);
        const float diff_y =
            static_cast<float>(entire_y2 - entire_y1 + 1) / static_cast<float>(kScreenScale);
        const float diff_s = tc1s - tc0s;
        const float diff_t = tc1t - tc0t;
        if (v0.screenpos.x < v1.screenpos.x) {
            if (v0.screenpos.y < v1.screenpos.y) {
                row_s = tc0s;
                row_t = tc0t;
                stx_s = 2.0f * diff_s / diff_x;
                sty_t = 2.0f * diff_t / diff_y;
            } else {
                row_s = tc1s;
                row_t = tc0t;
                stx_t = 2.0f * diff_t / diff_x;
                sty_s = 2.0f * -diff_s / diff_y;
            }
        } else {
            if (v0.screenpos.y < v1.screenpos.y) {
                row_s = tc0s;
                row_t = tc1t;
                stx_t = 2.0f * -diff_t / diff_x;
                sty_s = 2.0f * diff_s / diff_y;
            } else {
                row_s = tc1s;
                row_t = tc1t;
                stx_s = 2.0f * -diff_s / diff_x;
                sty_t = 2.0f * -diff_t / diff_y;
            }
        }
        const float kx = static_cast<float>(min_x - entire_x1 + 1);
        const float ky = static_cast<float>(min_y - entire_y1 + 1);
        const float inv = 1.0f / static_cast<float>(kScreenScale * 2);
        row_s += (stx_s * inv) * kx;
        row_t += (stx_t * inv) * kx;
        row_s += (sty_s * inv) * ky;
        row_t += (sty_t * inv) * ky;
    }

    const float sto4[4] = {0.0f, 0.5f * stx_s, 0.5f * sty_s, 0.5f * stx_s + 0.5f * sty_s};
    const float tto4[4] = {0.0f, 0.5f * stx_t, 0.5f * sty_t, 0.5f * stx_t + 0.5f * sty_t};
    const int fog = ClampFogDepth(v1.fogdepth);
    const int z = v1.screenpos.z;
    const Vec4i c0 = Vec4i::FromRGBA(v1.color0);
    const Vec4i sec_color = Vec4i::FromRGBA(v1.color1 & 0xFFFFFF);

    if (ps.apply_depth_range && (z < ps.minz || z > ps.maxz)) return;

    for (std::int64_t cur_y = min_y; cur_y < max_y; cur_y += kScreenScale * 2) {
        int px = ScreenToDrawing(min_x);
        const int py = ScreenToDrawing(static_cast<int>(cur_y));
        const int scissor_y2 = cur_y + kScreenScale > max_y ? -1 : 0;
        int scissor_mask[4] = {0, max_x - min_x - kScreenScale, scissor_y2,
                               (max_x - min_x - kScreenScale) | scissor_y2};
        float st_s = row_s;
        float st_t = row_t;
        for (std::int64_t cur_x = min_x; cur_x < max_x; cur_x += kScreenScale * 2) {
            int mask[4];
            std::memcpy(mask, scissor_mask, sizeof(mask));
            Vec4i prim_color[4] = {c0, c0, c0, c0};
            if (ps.early_z_checks) {
                for (int i = 0; i < 4; ++i) {
                    if (mask[i] < 0) continue;
                    if (!DepthTestPassed(rt, ps.depth_test_func, px + (i & 1), py + (i / 2), z))
                        mask[i] = -1;
                }
            }
            if (state.enable_textures) {
                float s[4];
                float t[4];
                for (int i = 0; i < 4; ++i) {
                    s[i] = st_s + sto4[i];
                    t[i] = st_t + tto4[i];
                }
                ApplyTexturing(state, sampler, prim_color, mask, s, t, v1.clipw);
            }
            if (!ps.clear_mode) {
                for (Vec4i &c : prim_color) {
                    for (int k = 0; k < 3; ++k) c[k] += sec_color[k];
                }
            }
            for (int i = 0; i < 4; ++i) {
                if (mask[i] < 0) continue;
                DrawPixel(rt, px + (i & 1), py + (i / 2), z, fog, prim_color[i], ps);
            }
            st_s += stx_s;
            st_t += stx_t;
            scissor_mask[1] -= kScreenScale * 2;
            scissor_mask[3] -= kScreenScale * 2;
            px = (px + 2) & 0x3FF;
        }
        row_s += sty_s;
        row_t += sty_t;
    }
}

void Rasterizer::DrawSprite(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                            const RasterizerState &state) const {
    const PixelState &ps = state.pixel;
    const SamplerState &st = state.sampler;
    const RenderTarget rt(mem_, ps);
    const Sampler sampler(mem_, st);

    int pos0x = ScreenToDrawing(v0.screenpos.x);
    int pos0y = ScreenToDrawing(v0.screenpos.y);
    // Include the ending pixel based on its centre, not its start.
    int pos1x = ScreenToDrawing(v1.screenpos.x + 7);
    int pos1y = ScreenToDrawing(v1.screenpos.y + 7);
    const int scissor_tlx = ScreenToDrawing(range.x1);
    const int scissor_tly = ScreenToDrawing(range.y1);
    const int scissor_brx = ScreenToDrawing(range.x2);
    const int scissor_bry = ScreenToDrawing(range.y2);
    const int z = v1.screenpos.z;
    constexpr int kFog = 255;

    if (ps.apply_depth_range && (z < ps.minz || z > ps.maxz)) return;
    const bool is_white = v1.color0 == 0xFFFFFFFF;

    if (state.enable_textures) {
        int s_start = TruncToInt(v0.s);
        int t_start = TruncToInt(v0.t);
        const int ds = v1.s > v0.s ? 1 : -1;
        const int dt = v1.t > v0.t ? 1 : -1;
        if (ds < 0) s_start += ds;
        if (dt < 0) t_start += dt;
        if (pos1x > scissor_brx) pos1x = scissor_brx + 1;
        if (pos1y > scissor_bry) pos1y = scissor_bry + 1;
        if (pos0x < scissor_tlx) {
            s_start += (scissor_tlx - pos0x) * ds;
            pos0x = scissor_tlx;
        }
        if (pos0y < scissor_tly) {
            t_start += (scissor_tly - pos0y) * dt;
            pos0y = scissor_tly;
        }

        if (UseDrawSinglePixel(ps) &&
            (st.tex_func == TexFunc::kModulate || st.tex_func == TexFunc::kReplace) &&
            st.use_texture_alpha) {
            // DrawSpriteTex<isWhite, alphaBlend, alphaTestZero>.
            const bool white_path = is_white || st.tex_func == TexFunc::kReplace;
            const bool alpha_blend = ps.alpha_blend;
            const bool alpha_test_zero =
                ps.alpha_blend || ps.alpha_test_func != Comparison::kAlways;
            const Vec4i c0 = Vec4i::FromRGBA(v1.color0);
            int t = t_start;
            for (int y = pos0y; y < pos1y; ++y) {
                int s = s_start;
                for (int x = pos0x; x < pos1x; ++x) {
                    const Vec4i tex = Vec4i::FromRGBA(sampler.Fetch(s, t, 0));
                    if (white_path) {
                        if (!alpha_test_zero || tex.a() != 0) {
                            DrawSinglePixelFast(rt, ps.fb_format, alpha_blend, x, y, tex.ToRGBA());
                        }
                    } else {
                        const Vec4i prim = ModulateRGBA(c0, tex, st);
                        if (!alpha_test_zero || prim.a() > 0) {
                            DrawSinglePixelFast(rt, ps.fb_format, alpha_blend, x, y, prim.ToRGBA());
                        }
                    }
                    s += ds;
                }
                t += dt;
            }
        } else {
            const float dsf =
                static_cast<float>(ds) * (1.0f / static_cast<float>(1 << st.width0_shift));
            const float dtf =
                static_cast<float>(dt) * (1.0f / static_cast<float>(1 << st.height0_shift));
            const float sf_start =
                static_cast<float>(s_start) * (1.0f / static_cast<float>(1 << st.width0_shift));
            const float tf_start =
                static_cast<float>(t_start) * (1.0f / static_cast<float>(1 << st.height0_shift));
            const Vec4i c0 = Vec4i::FromRGBA(v1.color0);
            float t = tf_start;
            for (int y = pos0y; y < pos1y; ++y) {
                float s = sf_start;
                for (int x = pos0x; x < pos1x; ++x) {
                    if (!ps.early_z_checks || DepthTestPassed(rt, ps.depth_test_func, x, y, z)) {
                        const Vec4i prim = sampler.Nearest(s, t, c0, 0, 0);
                        DrawPixel(rt, x, y, z, kFog, prim, ps);
                    }
                    s += dsf;
                }
                t += dtf;
            }
        }
        return;
    }

    if (pos1x > scissor_brx) pos1x = scissor_brx + 1;
    if (pos1y > scissor_bry) pos1y = scissor_bry + 1;
    if (pos0x < scissor_tlx) pos0x = scissor_tlx;
    if (pos0y < scissor_tly) pos0y = scissor_tly;
    if (UseDrawSinglePixel(ps)) {
        if (ps.alpha_blend && (v1.color0 >> 24) == 0) return;
        for (int y = pos0y; y < pos1y; ++y) {
            for (int x = pos0x; x < pos1x; ++x) {
                DrawSinglePixelFast(rt, ps.fb_format, ps.alpha_blend, x, y, v1.color0);
            }
        }
        return;
    }
    const Vec4i prim = Vec4i::FromRGBA(v1.color0);
    for (int y = pos0y; y < pos1y; ++y) {
        for (int x = pos0x; x < pos1x; ++x) {
            if (ps.early_z_checks && !DepthTestPassed(rt, ps.depth_test_func, x, y, z)) continue;
            DrawPixel(rt, x, y, z, kFog, prim, ps);
        }
    }
}

void Rasterizer::ClearRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                                const RasterizerState &state) const {
    const PixelState &ps = state.pixel;
    const RenderTarget rt(mem_, ps);
    const int entire_x1 = std::min(v0.screenpos.x, v1.screenpos.x);
    const int entire_y1 = std::min(v0.screenpos.y, v1.screenpos.y);
    const int entire_x2 = std::max(v0.screenpos.x, v1.screenpos.x) - 1;
    const int entire_y2 = std::max(v0.screenpos.y, v1.screenpos.y) - 1;
    int min_x = std::max(entire_x1 & ~(kScreenScale - 1), range.x1) | (kScreenScale / 2 - 1);
    int min_y = std::max(entire_y1 & ~(kScreenScale - 1), range.y1) | (kScreenScale / 2 - 1);
    const int max_x = std::min(entire_x2, range.x2);
    const int max_y = std::min(entire_y2, range.y2);
    if (min_x < entire_x1 - 1) min_x += kScreenScale;
    if (min_y < entire_y1 - 1) min_y += kScreenScale;

    const int x0 = ScreenToDrawing(min_x);
    const int y0 = ScreenToDrawing(min_y);
    // Only include the end pixel when it is >= 0.5.
    const int x_end = ScreenToDrawing(max_x - kScreenScale / 2);
    const int y_end = ScreenToDrawing(max_y - kScreenScale / 2);
    const int w = x_end - x0 + 1;
    if (w <= 0) return;

    if (ps.depth_write) { // DepthClear()
        const auto z = static_cast<std::uint16_t>(v1.screenpos.z);
        for (int y = y0; y <= y_end; ++y) {
            for (int x = 0; x < w; ++x) rt.SetDepth(x0 + x, y, z);
        }
    }

    // Stays 0xFFFFFFFF when keeping colour and alpha, even for 16-bit.
    std::uint32_t keep_old_mask = 0xFFFFFFFF;
    const bool color_clear = ps.color_test;
    const bool stencil_clear = ps.stencil_test;
    if (color_clear && stencil_clear) {
        keep_old_mask = 0;
    } else {
        switch (ps.fb_format) {
        case BufferFormat::k565:
            if (color_clear) keep_old_mask = 0;
            break;
        case BufferFormat::k5551:
            if (color_clear) {
                keep_old_mask = 0xFFFF8000;
            } else if (stencil_clear) {
                keep_old_mask = 0xFFFF7FFF;
            }
            break;
        case BufferFormat::k4444:
            if (color_clear) {
                keep_old_mask = 0xFFFFF000;
            } else if (stencil_clear) {
                keep_old_mask = 0xFFFF0FFF;
            }
            break;
        case BufferFormat::k8888:
            if (color_clear) {
                keep_old_mask = 0xFF000000;
            } else if (stencil_clear) {
                keep_old_mask = 0x00FFFFFF;
            }
            break;
        }
    }
    if (ps.apply_color_write_mask) keep_old_mask |= ps.color_write_mask;

    const std::uint32_t new_color = v1.color0;
    std::uint16_t new_color16 = 0;
    switch (ps.fb_format) {
    case BufferFormat::k565:
        new_color16 = RGBA8888ToRGB565(new_color);
        break;
    case BufferFormat::k5551:
        new_color16 = RGBA8888ToRGBA5551(new_color);
        break;
    case BufferFormat::k4444:
        new_color16 = RGBA8888ToRGBA4444(new_color);
        break;
    case BufferFormat::k8888:
        break;
    }
    if (keep_old_mask == 0xFFFFFFFF) return;
    for (int y = y0; y <= y_end; ++y) {
        for (int x = 0; x < w; ++x) {
            if (ps.fb_format == BufferFormat::k8888) {
                const std::uint32_t old_color = keep_old_mask ? rt.Get32(x0 + x, y) : 0;
                rt.Set32(x0 + x, y, (old_color & keep_old_mask) | (new_color & ~keep_old_mask));
            } else {
                const std::uint32_t old_color = keep_old_mask ? rt.Get16(x0 + x, y) : 0;
                rt.Set16(x0 + x, y,
                         static_cast<std::uint16_t>((old_color & keep_old_mask) |
                                                    (new_color16 & ~keep_old_mask)));
            }
        }
    }
}

void Rasterizer::DrawPoint(const VertexData &v0, const BinCoords & /*range*/,
                           const RasterizerState &state) const {
    const PixelState &ps = state.pixel;
    const RenderTarget rt(mem_, ps);
    const Sampler sampler(mem_, state.sampler);
    Vec4i prim_color = Vec4i::FromRGBA(v0.color0);
    const int x = ScreenToDrawing(v0.screenpos.x);
    const int y = ScreenToDrawing(v0.screenpos.y);
    const int z = v0.screenpos.z;
    if (ps.early_z_checks) {
        if (ps.apply_depth_range && (z < ps.minz || z > ps.maxz)) return;
        if (!DepthTestPassed(rt, ps.depth_test_func, x, y, z)) return;
    }
    if (state.enable_textures) {
        float s = v0.s;
        float t = v0.t;
        if (state.through_mode) {
            s *= 1.0f / static_cast<float>(1 << state.sampler.width0_shift);
            t *= 1.0f / static_cast<float>(1 << state.sampler.height0_shift);
        } else {
            // GetTextureCoordinates(v0, v0, 0.0f): p = 0, so only v1 (= v0) counts.
            const float q = 1.0f / v0.clipw;
            const float wq1 = 1.0f * q;
            const float q_recip = state.texture_proj ? 1.0f / (v0.q * wq1) : 1.0f / wq1;
            s = (v0.s * wq1) * q_recip;
            t = (v0.t * wq1) * q_recip;
        }
        int level;
        int level_frac;
        bool bilinear;
        CalculateSamplingParams(0.0f, 0.0f, v0.clipw, state, &level, &level_frac, &bilinear);
        prim_color = bilinear ? sampler.Linear(s, t, prim_color, level, level_frac)
                              : sampler.Nearest(s, t, prim_color, level, level_frac);
    }
    if (!ps.clear_mode) {
        const Vec4i sec = Vec4i::FromRGBA(v0.color1 & 0xFFFFFF);
        for (int k = 0; k < 3; ++k) prim_color[k] += sec[k];
    }
    int fog = 255;
    if (ps.apply_fog) fog = ClampFogDepth(v0.fogdepth);
    DrawPixel(rt, x, y, z, fog, prim_color, ps);
}

namespace {

// GetTextureCoordinates / GetTextureCoordinatesProj for two vertices.
void LineTextureCoordinates(const VertexData &v0, const VertexData &v1, float p, bool proj,
                            float *s, float *t) {
    const float q0 = 1.f / v0.clipw;
    const float q1 = 1.f / v1.clipw;
    const float wq0 = p * q0;
    const float wq1 = (1.0f - p) * q1;
    const float q_recip = proj ? 1.0f / (v0.q * wq0 + v1.q * wq1) : 1.0f / (wq0 + wq1);
    *s = (v0.s * wq0 + v1.s * wq1) * q_recip;
    *t = (v0.t * wq0 + v1.t * wq1) * q_recip;
}

// double -> int as CVTTSD2SI (out of range and NaN give INT_MIN).
int TruncDoubleToInt(double d) {
    if (!(d > -2147483649.0 && d < 2147483648.0)) return static_cast<int>(0x80000000u);
    return static_cast<int>(d);
}

} // namespace

void Rasterizer::DrawLine(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                          const RasterizerState &state) const {
    const PixelState &ps = state.pixel;
    const RenderTarget rt(mem_, ps);
    const Sampler sampler(mem_, state.sampler);

    const int ax = v0.screenpos.x;
    const int ay = v0.screenpos.y;
    const int az = v0.screenpos.z;
    const int bx = v1.screenpos.x;
    const int by = v1.screenpos.y;
    const int bz = v1.screenpos.z;

    int dx = bx - ax;
    int dy = by - ay;
    const int dz = bz - az;

    int steps;
    if (std::abs(dx) < std::abs(dy)) {
        steps = std::abs(dy) / kScreenScale;
    } else {
        steps = std::abs(dx) / kScreenScale;
    }
    if (steps <= 0) return;

    // Avoid going too far since we typically don't start at the pixel centre.
    if (dx < 0 && dx >= -kScreenScale) dx++;
    if (dy < 0 && dy >= -kScreenScale) dy++;

    const double xinc = static_cast<double>(dx) / steps;
    const double yinc = static_cast<double>(dy) / steps;
    const double zinc = static_cast<double>(dz) / steps;

    const bool interpolate_color =
        state.shade_gouraud && !(v0.color0 == v1.color0 && v0.color1 == v1.color1);
    const Vec4i v0_c0 = Vec4i::FromRGBA(v0.color0);
    const Vec4i v1_c0 = Vec4i::FromRGBA(v1.color0);
    const Vec4i v0_c1 = Vec4i::FromRGBA(v0.color1 & 0xFFFFFF);
    const Vec4i v1_c1 = Vec4i::FromRGBA(v1.color1 & 0xFFFFFF);

    double x = ax > bx ? ax - 1 : ax;
    double y = ay > by ? ay - 1 : ay;
    double z = az;
    const int steps1 = steps;
    for (int i = 0; i < steps; ++i) {
        // TransformUnit::ScreenToDrawing(int, int): truncate, then / 16.
        const int px = ScreenToDrawing(TruncDoubleToInt(x));
        const int py = ScreenToDrawing(TruncDoubleToInt(y));
        const int zi = TruncDoubleToInt(z);

        bool mask_ok = x >= range.x1 && y >= range.y1 && x <= range.x2 && y <= range.y2;
        if (mask_ok && ps.early_z_checks) {
            if (ps.apply_depth_range && (z < ps.minz || z > ps.maxz)) mask_ok = false;
            // CheckDepthTestPassed takes the depth as u16.
            if (!DepthTestPassed(rt, ps.depth_test_func, px, py, static_cast<std::uint16_t>(zi))) {
                mask_ok = false;
            }
        }

        if (mask_ok) {
            Vec4i prim_color;
            Vec4i sec_color;
            if (interpolate_color) {
                for (int k = 0; k < 4; ++k) {
                    prim_color[k] = (v0_c0[k] * (steps - i) + v1_c0[k] * i) / steps1;
                }
                for (int k = 0; k < 3; ++k) {
                    sec_color[k] = (v0_c1[k] * (steps - i) + v1_c1[k] * i) / steps1;
                }
            } else {
                prim_color = v1_c0;
                sec_color = v1_c1;
            }

            int fog = 255;
            if (ps.apply_fog) {
                fog = ClampFogDepth((v0.fogdepth * static_cast<float>(steps - i) +
                                     v1.fogdepth * static_cast<float>(i)) /
                                    static_cast<float>(steps1));
            }

            if (state.antialias_lines) prim_color[3] = 0x7F;

            if (state.enable_textures) {
                float s;
                float s1;
                float t;
                float t1;
                if (state.through_mode) {
                    const float fa = static_cast<float>(steps - i);
                    const float fb = static_cast<float>(i);
                    const float fa1 = static_cast<float>(steps - i - 1);
                    const float fb1 = static_cast<float>(i + 1);
                    const auto fs1 = static_cast<float>(steps1);
                    const float tcs = (v0.s * fa + v1.s * fb) / fs1;
                    const float tct = (v0.t * fa + v1.t * fb) / fs1;
                    const float tc1s = (v0.s * fa1 + v1.s * fb1) / fs1;
                    const float tc1t = (v0.t * fa1 + v1.t * fb1) / fs1;
                    const float sw = 1.0f / static_cast<float>(1 << state.sampler.width0_shift);
                    const float th = 1.0f / static_cast<float>(1 << state.sampler.height0_shift);
                    s = tcs * sw;
                    s1 = tc1s * sw;
                    t = tct * th;
                    t1 = tc1t * th;
                } else {
                    const auto fs1 = static_cast<float>(steps1);
                    LineTextureCoordinates(v0, v1, static_cast<float>(steps - i) / fs1,
                                           state.texture_proj, &s, &t);
                    LineTextureCoordinates(v0, v1, static_cast<float>(steps - i - 1) / fs1,
                                           state.texture_proj, &s1, &t1);
                }

                // If inc is 0, force the delta to zero. (float * double).
                const float ds =
                    xinc == 0.0
                        ? 0.0f
                        : static_cast<float>(
                              static_cast<double>((s1 - s) * static_cast<float>(kScreenScale)) *
                              (1.0f / xinc));
                const float dt =
                    yinc == 0.0
                        ? 0.0f
                        : static_cast<float>(
                              static_cast<double>((t1 - t) * static_cast<float>(kScreenScale)) *
                              (1.0f / yinc));
                const float w =
                    (v0.clipw * static_cast<float>(steps - i) + v1.clipw * static_cast<float>(i)) /
                    static_cast<float>(steps1);

                int level;
                int level_frac;
                bool bilinear;
                CalculateSamplingParams(ds, dt, w, state, &level, &level_frac, &bilinear);
                if (state.antialias_lines) {
                    // PPSSPP: "a naive and wrong implementation".
                    s = static_cast<float>((static_cast<float>(px) + xinc / 32.0f) / 512.0f);
                    t = static_cast<float>((static_cast<float>(py) + yinc / 32.0f) / 512.0f);
                    bilinear = true;
                }
                prim_color = bilinear ? sampler.Linear(s, t, prim_color, level, level_frac)
                                      : sampler.Nearest(s, t, prim_color, level, level_frac);
            }

            if (!ps.clear_mode) {
                for (int k = 0; k < 3; ++k) prim_color[k] += sec_color[k];
            }
            DrawPixel(rt, px, py, zi, fog, prim_color, ps);
        }

        x += xinc;
        y += yinc;
        z += zinc;
    }
}

bool RectangleCanUseSprite(const VertexData &v0, const VertexData &v1,
                           const RasterizerState &state) {
    const int xdiff = v1.screenpos.x - v0.screenpos.x;
    const int ydiff = v1.screenpos.y - v0.screenpos.y;
    const int udiff = TruncToInt((v1.s - v0.s) * static_cast<float>(kScreenScale));
    const int vdiff = TruncToInt((v1.t - v0.t) * static_cast<float>(kScreenScale));
    const bool orient_check = xdiff >= 0 && ydiff >= 0;
    bool state_check = state.through_mode && !state.pixel.clear_mode &&
                       !state.sampler.has_any_mips && !state.texture_proj;
    bool coord_check = true;
    if (state.enable_textures) {
        state_check =
            state_check && NoClampOrWrap(state, v0.s, v0.t) && NoClampOrWrap(state, v1.s, v1.t);
        coord_check = (xdiff == udiff || xdiff == -udiff) && (ydiff == vdiff || ydiff == -vdiff);
    }
    const bool subpixel_check =
        ((v0.screenpos.x | v0.screenpos.y | v1.screenpos.x | v1.screenpos.y) & 0xF) == 0;
    return coord_check && orient_check && state_check && subpixel_check;
}

namespace {

bool IsCoordRectangleCompatible(const RasterizerState &state, const ClipVertexData &data) {
    if (!state.through_mode) {
        if (data.OutsideRange()) return false;
        if (data.clippos.w < 0.0f) return false;
        if (data.clippos.z < -data.clippos.w) return false;
    }
    return true;
}

bool AreCoordsRectangleCompatible(const RasterizerState &state, const ClipVertexData &data0,
                                  const ClipVertexData &data1) {
    if (data1.v.color0 != data0.v.color0) return false;
    if (data1.v.screenpos.z != data0.v.screenpos.z) {
        if (state.pixel.depth_write || state.pixel.depth_test_func != Comparison::kAlways)
            return false;
    }
    if (!state.through_mode) {
        if (data1.v.color1 != data0.v.color1) return false;
        if (data1.OutsideRange()) return false;
        if (state.enable_textures && data1.clippos.w != data0.clippos.w) {
            constexpr float kHalfTexel = 0.5f / 512.0f;
            if (data1.clippos.w - kHalfTexel > data0.clippos.w ||
                data1.clippos.w + kHalfTexel < data0.clippos.w) {
                return false;
            }
        }
        if (data1.clippos.w < 0.0f) return false;
        if (data1.clippos.z < -data1.clippos.w) return false;
        if (state.enable_textures && data1.v.q != data0.v.q) return false;
        if (state.pixel.apply_fog && data1.v.fogdepth != data0.v.fogdepth) {
            constexpr float kFogHalfStep = 0.5f / 255.0f;
            if (data1.v.fogdepth - kFogHalfStep > data0.v.fogdepth ||
                data1.v.fogdepth + kFogHalfStep < data0.v.fogdepth) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool DetectRectangleFromStrip(const RasterizerState &state, const ClipVertexData data[4],
                              int *tl_index, int *br_index) {
    if (!IsCoordRectangleCompatible(state, data[0])) return false;
    int tl = 0;
    int br = 0;
    for (int i = 1; i < 4; ++i) {
        if (!AreCoordsRectangleCompatible(state, data[0], data[i])) return false;
        if (data[i].v.screenpos.x <= data[tl].v.screenpos.x &&
            data[i].v.screenpos.y <= data[tl].v.screenpos.y) {
            tl = i;
        }
        if (data[i].v.screenpos.x >= data[br].v.screenpos.x &&
            data[i].v.screenpos.y >= data[br].v.screenpos.y) {
            br = i;
        }
    }
    *tl_index = tl;
    *br_index = br;
    const auto &p = data;
    if (p[0].v.screenpos.x == p[1].v.screenpos.x && p[0].v.screenpos.y == p[2].v.screenpos.y &&
        p[2].v.screenpos.x == p[3].v.screenpos.x && p[1].v.screenpos.y == p[3].v.screenpos.y) {
        if (!state.enable_textures) return true;
        return p[0].v.s == p[1].v.s && p[0].v.t == p[2].v.t && p[2].v.s == p[3].v.s &&
               p[1].v.t == p[3].v.t;
    }
    if (p[0].v.screenpos.x == p[2].v.screenpos.x && p[0].v.screenpos.y == p[1].v.screenpos.y &&
        p[1].v.screenpos.x == p[3].v.screenpos.x && p[2].v.screenpos.y == p[3].v.screenpos.y) {
        if (!state.enable_textures) return true;
        return p[0].v.s == p[2].v.s && p[0].v.t == p[1].v.t && p[1].v.s == p[3].v.s &&
               p[2].v.t == p[3].v.t;
    }
    return false;
}

bool DetectRectangleFromFan(const RasterizerState &state, const ClipVertexData *data, int *tl_index,
                            int *br_index) {
    if (!IsCoordRectangleCompatible(state, data[0])) return false;
    int tl = 0;
    int br = 0;
    for (int i = 1; i < 4; ++i) {
        if (!AreCoordsRectangleCompatible(state, data[0], data[i])) return false;
        if (data[i].v.screenpos.x <= data[tl].v.screenpos.x &&
            data[i].v.screenpos.y <= data[tl].v.screenpos.y) {
            tl = i;
        }
        if (data[i].v.screenpos.x >= data[br].v.screenpos.x &&
            data[i].v.screenpos.y >= data[br].v.screenpos.y) {
            br = i;
        }
    }
    *tl_index = tl;
    *br_index = br;
    int tr = 1;
    int bl = 1;
    for (int i = 0; i < 4; ++i) {
        if (i == tl || i == br) continue;
        if (data[i].v.screenpos.x <= data[tl].v.screenpos.x &&
            data[i].v.screenpos.y >= data[tl].v.screenpos.y) {
            bl = i;
        }
        if (data[i].v.screenpos.x >= data[br].v.screenpos.x &&
            data[i].v.screenpos.y <= data[br].v.screenpos.y) {
            tr = i;
        }
    }
    if (tl + tr + bl + br != 6) return false;
    const auto &postl = data[tl].v.screenpos;
    const auto &postr = data[tr].v.screenpos;
    const auto &posbr = data[br].v.screenpos;
    const auto &posbl = data[bl].v.screenpos;
    if (postl.x == posbl.x && postr.x == posbr.x && postl.y == postr.y && posbl.y == posbr.y) {
        if (!state.enable_textures) return true;
        const auto &textl = data[tl].v;
        const auto &textr = data[tr].v;
        const auto &texbl = data[bl].v;
        const auto &texbr = data[br].v;
        if (textl.s == texbl.s && textr.s == texbr.s && textl.t == textr.t && texbl.t == texbr.t) {
            return textl.t < texbr.t && postl.y < posbr.y && textl.s < texbr.s && postl.x < posbr.x;
        }
    }
    return false;
}

bool DetectRectangleFromPair(const RasterizerState &state, const ClipVertexData data[6],
                             int *tl_index, int *br_index) {
    if (!IsCoordRectangleCompatible(state, data[0])) return false;
    int tl = 0;
    int br = 0;
    for (int i = 1; i < 6; ++i) {
        if (!AreCoordsRectangleCompatible(state, data[0], data[i])) return false;
        if (data[i].v.screenpos.x <= data[tl].v.screenpos.x &&
            data[i].v.screenpos.y <= data[tl].v.screenpos.y) {
            tl = i;
        }
        if (data[i].v.screenpos.x >= data[br].v.screenpos.x &&
            data[i].v.screenpos.y >= data[br].v.screenpos.y) {
            br = i;
        }
    }
    *tl_index = tl;
    *br_index = br;
    auto xat = [&](int i) { return data[i].v.screenpos.x; };
    auto yat = [&](int i) { return data[i].v.screenpos.y; };
    auto uat = [&](int i) { return data[i].v.s; };
    auto vat = [&](int i) { return data[i].v.t; };
    if (tl == 3 && br == 4) {
        bool x1_match = xat(0) == xat(3) && xat(0) == xat(5);
        bool x2_match = xat(1) == xat(2) && xat(1) == xat(4);
        bool y1_match = yat(0) == yat(1) && yat(0) == yat(3);
        bool y2_match = yat(2) == yat(4) && yat(2) == yat(5);
        if (x1_match && y1_match && x2_match && y2_match) {
            if (!state.enable_textures) return true;
            x1_match = uat(0) == uat(3) && uat(0) == uat(5);
            x2_match = uat(1) == uat(2) && uat(1) == uat(4);
            y1_match = vat(0) == vat(1) && vat(0) == vat(3);
            y2_match = vat(2) == vat(4) && vat(2) == vat(5);
            if (x1_match && y1_match && x2_match && y2_match) {
                return vat(tl) < vat(br) && yat(tl) < yat(br) && uat(tl) < uat(br) &&
                       xat(tl) < xat(br);
            }
        }
    }
    return false;
}

bool DetectRectangleThroughModeSlices(const RasterizerState &state, const ClipVertexData data[4]) {
    for (int i = 1; i < 4; ++i) {
        if (data[i].v.color0 != data[0].v.color0) return false;
        if (data[i].v.screenpos.z != data[0].v.screenpos.z) {
            if (state.pixel.depth_write || state.pixel.depth_test_func != Comparison::kAlways) {
                return false;
            }
        }
    }
    const auto &tl1 = data[0].v.screenpos;
    const auto &br1 = data[1].v.screenpos;
    const auto &tl2 = data[2].v.screenpos;
    const auto &br2 = data[3].v.screenpos;
    if (tl1.y == tl2.y && br1.y == br2.y && br1.y > tl1.y) {
        if (br1.x == tl2.x && tl1.x < br1.x && tl2.x < br2.x) {
            if (!state.enable_textures) return true;
            const auto &textl1 = data[0].v;
            const auto &texbr1 = data[1].v;
            const auto &textl2 = data[2].v;
            const auto &texbr2 = data[3].v;
            if (textl1.t != textl2.t || texbr1.t != texbr2.t || textl1.t > texbr1.t) return false;
            if (texbr1.s != textl2.s || textl1.s > texbr1.s || textl2.s > texbr2.s) return false;
            const int texdiff1 =
                TruncToInt((texbr1.s - textl1.s) * static_cast<float>(kScreenScale));
            const int texdiff2 =
                TruncToInt((texbr2.s - textl2.s) * static_cast<float>(kScreenScale));
            const int posdiff1 = br1.x - tl1.x;
            const int posdiff2 = br2.x - tl2.x;
            return texdiff1 == posdiff1 && texdiff2 == posdiff2;
        }
    }
    return false;
}

} // namespace mpo::ge
