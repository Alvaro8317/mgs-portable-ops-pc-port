// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/FuncId.cpp, GPU/Software/Sampler.cpp and
// GPU/Common/TextureDecoder.cpp (GetTextureBufw) @ v1.20.4
// (GPL-2.0-or-later).

#include "sampler.hpp"

#include <algorithm>
#include <cstring>

namespace mpo::ge {
namespace {

constexpr std::uint32_t kTextureAlignMask16[16] = {
    0x7FF & ~(((8 * 16) / 16) - 1),
    0x7FF & ~(((8 * 16) / 16) - 1),
    0x7FF & ~(((8 * 16) / 16) - 1),
    0x7FF & ~(((8 * 16) / 32) - 1),
    0x7FF & ~(((8 * 16) / 4) - 1),
    0x7FF & ~(((8 * 16) / 8) - 1),
    0x7FF & ~(((8 * 16) / 16) - 1),
    0x7FF & ~(((8 * 16) / 32) - 1),
    0x7FF & ~(((8 * 16) / 32) - 1),
    0x7FF & ~(((8 * 16) / 32) - 1),
    0x7FF & ~(((8 * 16) / 32) - 1),
    0,
    0,
    0,
    0,
    0,
};

// Byte offset of texel (u, v) in a texture of `row_pitch` texels, linear or
// swizzled in 16-byte x 8-row blocks.
std::uint32_t PixelDataOffset(std::uint32_t bits, std::uint32_t row_pitch, std::uint32_t u,
                              std::uint32_t v, bool swizzled) {
    if (!swizzled) return (v * (row_pitch * bits >> 3)) + (u * bits >> 3);
    constexpr std::uint32_t kTileSizeBits = 32;
    constexpr std::uint32_t kTilesInBlockH = 4;
    constexpr std::uint32_t kTilesInBlockV = 8;
    const std::uint32_t texels_per_tile = kTileSizeBits / bits;
    const std::uint32_t tile_u = u / texels_per_tile;
    const std::uint32_t tile_idx =
        (v % kTilesInBlockV) * kTilesInBlockH +
        (v / kTilesInBlockV) * ((row_pitch * bits / kTileSizeBits) * kTilesInBlockV) +
        (tile_u % kTilesInBlockH) + (tile_u / kTilesInBlockH) * (kTilesInBlockH * kTilesInBlockV);
    return tile_idx * (kTileSizeBits / 8) + ((u % texels_per_tile) * bits) / 8;
}

int ClampUV(int v, int height) {
    if (v >= height - 1) return height - 1;
    if (v >= 511) return 511;
    if (v < 0) return 0;
    return v;
}

int WrapUV(int v, int height) {
    return v & (height - 1) & 511;
}

int ApplyClamp(bool clamp, int v, int size) {
    return clamp ? ClampUV(v, size) : WrapUV(v, size);
}

} // namespace

int TextureBufw(const GeState &gs, int level, TexFormat format) {
    const std::uint32_t addr = MemoryView::Canonical(gs.TextureAddress(level));
    if (addr >= 0x08000000 && addr < 0x08400000) {
        return static_cast<int>(gs.TexBufWidthReg(level) & 0x1FFF);
    }
    const auto fmt = static_cast<std::size_t>(format);
    std::uint32_t bufw = gs.TexBufWidthReg(level) & kTextureAlignMask16[fmt];
    if (bufw == 0 && format <= TexFormat::kDxt5) bufw = (8 * 16) / kTextureBitsPerPixel[fmt];
    return static_cast<int>(bufw);
}

SamplerState ComputeSamplerState(const GeState &gs, const MemoryView &mem,
                                 const std::uint8_t *clut) {
    SamplerState id;
    const int max_level = gs.IsMipmapEnabled() ? gs.TextureMaxLevel() : 0;
    const TexFormat fmt = gs.TextureFormat();
    for (int i = 0; i <= max_level; ++i) {
        const std::uint32_t addr = gs.TextureAddress(i);
        if (!mem.IsValidRange(addr, 1)) id.has_invalid_ptr = true;
        id.sizes[i].w = gs.TextureWidth(i);
        id.sizes[i].h = gs.TextureHeight(i);
        id.texaddr[i] = addr;
        id.texbufw[i] = TextureBufw(gs, i, fmt);
    }
    id.width0_shift = gs.TexSize0WidthShift();
    id.height0_shift = gs.TexSize0HeightShift();
    id.has_any_mips = max_level != 0;
    id.texfmt = fmt;
    id.swizzle = gs.IsTextureSwizzled();
    id.use_shared_clut = fmt != TexFormat::kClut4 || max_level == 0 || !gs.IsMipmapEnabled() ||
                         gs.IsClutSharedForMipmaps();
    if (gs.IsTextureFormatIndexed()) {
        id.clutfmt = gs.ClutPaletteFormat();
        id.has_clut_mask = gs.ClutIndexMask() != 0xFF;
        id.has_clut_shift = gs.ClutIndexShift() != 0;
        id.has_clut_offset = gs.ClutIndexStartPos() != 0;
        id.clut_format = gs.Reg(kCmdClutFormat);
    }
    id.clamp_s = gs.IsTexCoordClampedS();
    id.clamp_t = gs.IsTexCoordClampedT();
    id.use_texture_alpha = gs.IsTextureAlphaUsed();
    id.use_color_doubling = gs.IsColorDoublingEnabled();
    int func = gs.TextureFunction();
    if (func > static_cast<int>(TexFunc::kAdd)) func = static_cast<int>(TexFunc::kAdd);
    id.tex_func = static_cast<TexFunc>(func);
    if (id.tex_func == TexFunc::kBlend) id.tex_blend_color = gs.TextureEnvColor();
    id.clut = clut;
    return id;
}

std::uint32_t Sampler::TransformClutIndex(std::uint32_t index) const {
    if (st_.has_clut_shift || st_.has_clut_mask || st_.has_clut_offset) {
        const std::uint32_t shift = (st_.clut_format >> 2) & 0x1F;
        const std::uint32_t mask = (st_.clut_format >> 8) & 0xFF;
        const std::uint32_t offset = ((st_.clut_format >> 16) & 0x1F) << 4;
        const std::uint32_t offset_mask = st_.clutfmt == ClutFormat::k8888 ? 0xFF : 0x1FF;
        return ((index >> shift) & mask) | (offset & offset_mask);
    }
    return index & 0xFF;
}

std::uint32_t Sampler::LookupColor(std::uint32_t index, int level) const {
    const std::uint32_t sharing = st_.use_shared_clut ? 0 : static_cast<std::uint32_t>(level) * 16;
    const std::uint32_t entry = index + sharing;
    if (!st_.clut) return 0;
    if (st_.clutfmt == ClutFormat::k8888) {
        if (entry >= 256) return 0;
        std::uint32_t c;
        std::memcpy(&c, st_.clut + entry * 4, 4);
        return c;
    }
    if (entry >= 512) return 0;
    std::uint16_t c;
    std::memcpy(&c, st_.clut + entry * 2, 2);
    switch (st_.clutfmt) {
    case ClutFormat::k565:
        return RGB565ToRGBA8888(c);
    case ClutFormat::k5551:
        return RGBA5551ToRGBA8888(c);
    case ClutFormat::k4444:
        return RGBA4444ToRGBA8888(c);
    default:
        return 0;
    }
}

std::uint32_t Sampler::Fetch(int u_in, int v_in, int level) const {
    const std::uint32_t base = st_.texaddr[level];
    const auto bufw = static_cast<std::uint32_t>(st_.texbufw[level]);
    const auto u = static_cast<std::uint32_t>(u_in);
    const auto v = static_cast<std::uint32_t>(v_in);
    const bool sw = st_.swizzle;
    switch (st_.texfmt) {
    case TexFormat::k4444:
        return RGBA4444ToRGBA8888(mem_.Read16(base + PixelDataOffset(16, bufw, u, v, sw)));
    case TexFormat::k5551:
        return RGBA5551ToRGBA8888(mem_.Read16(base + PixelDataOffset(16, bufw, u, v, sw)));
    case TexFormat::k5650:
        return RGB565ToRGBA8888(mem_.Read16(base + PixelDataOffset(16, bufw, u, v, sw)));
    case TexFormat::k8888:
        return mem_.Read32(base + PixelDataOffset(32, bufw, u, v, sw));
    case TexFormat::kClut32:
        return LookupColor(
            TransformClutIndex(mem_.Read32(base + PixelDataOffset(32, bufw, u, v, sw))), 0);
    case TexFormat::kClut16:
        return LookupColor(
            TransformClutIndex(mem_.Read16(base + PixelDataOffset(16, bufw, u, v, sw))), 0);
    case TexFormat::kClut8:
        return LookupColor(
            TransformClutIndex(mem_.Read8(base + PixelDataOffset(8, bufw, u, v, sw))), 0);
    case TexFormat::kClut4: {
        const std::uint8_t byte = mem_.Read8(base + PixelDataOffset(4, bufw, u, v, sw));
        const std::uint32_t val = (u & 1) ? (byte >> 4) : (byte & 0xF);
        return LookupColor(TransformClutIndex(val), level);
    }
    default:
        // DXT is not used by MPO (F05); it is reported by the command
        // processor and samples as transparent black.
        return 0;
    }
}

// The x86-64 JIT (Jit_ApplyTextureFunc) packs the primary colour to 16 bits
// with signed saturation (PACKSSDW), works in 16-bit lanes and widens the
// result with PMOVZXWD. For colours in 0-255 this is the plain formula; it
// matters for the NaN-interpolated colours of zero-area triangles, which
// would otherwise overflow.
Vec4i TextureFunctionOutput(const Vec4i &prim_color_in, const Vec4i &texcolor,
                            const SamplerState &st) {
    Vec4i prim_color;
    for (int i = 0; i < 4; ++i) prim_color[i] = std::clamp(prim_color_in[i], -32768, 32767);
    // A prim lane copied as is: the word, zero-extended.
    auto word = [](int v) { return static_cast<int>(static_cast<std::uint16_t>(v)); };
    Vec4i out;
    const bool rgba = st.use_texture_alpha;
    switch (st.tex_func) {
    case TexFunc::kModulate: {
        // PADDW 1, PMULLW, PSRLW 7 (doubling) or 8.
        int res[4];
        for (int i = 0; i < 4; ++i) {
            const auto p1 = static_cast<std::uint16_t>(prim_color[i] + 1);
            auto r = static_cast<std::uint16_t>(static_cast<std::uint32_t>(p1) *
                                                static_cast<std::uint32_t>(texcolor[i]));
            r = static_cast<std::uint16_t>(r >> (st.use_color_doubling ? 7 : 8));
            res[i] = r;
        }
        for (int i = 0; i < 3; ++i) out[i] = res[i];
        if (!rgba) {
            out[3] = word(prim_color.a());
        } else if (st.use_color_doubling) {
            out[3] = res[3] >> 1; // alpha was doubled by the shift by 7
        } else {
            out[3] = res[3];
        }
        break;
    }
    case TexFunc::kDecal:
        if (rgba) {
            const int t = texcolor.a();
            const int invt = 255 - t;
            for (int i = 0; i < 3; ++i) {
                const int c = (prim_color[i] + 1) * invt + (texcolor[i] + 1) * t;
                out[i] = st.use_color_doubling ? c / 128 : c / 256;
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                out[i] = st.use_color_doubling ? texcolor[i] * 2 : texcolor[i];
            }
        }
        out[3] = word(prim_color.a());
        break;
    case TexFunc::kBlend: {
        const Vec4i env = Vec4i::FromRGBA(st.tex_blend_color);
        for (int i = 0; i < 3; ++i) {
            const int c = (255 - texcolor[i]) * prim_color[i] + texcolor[i] * env[i] + 255;
            out[i] = st.use_color_doubling ? c / 128 : c / 256;
        }
        out[3] = rgba ? ((prim_color.a() + 1) * texcolor.a()) / 256 : word(prim_color.a());
        break;
    }
    case TexFunc::kReplace:
        for (int i = 0; i < 3; ++i) {
            out[i] = st.use_color_doubling ? texcolor[i] * 2 : texcolor[i];
        }
        out[3] = rgba ? texcolor.a() : word(prim_color.a());
        break;
    case TexFunc::kAdd:
    default:
        for (int i = 0; i < 3; ++i) {
            const int c = prim_color[i] + texcolor[i];
            out[i] = st.use_color_doubling ? c * 2 : c;
        }
        out[3] = rgba ? ((prim_color.a() + 1) * texcolor.a()) / 256 : word(prim_color.a());
        break;
    }
    return out;
}

Vec4i Sampler::Nearest(float s, float t, const Vec4i &prim_color, int level, int level_frac) const {
    auto texel = [&](int lvl) {
        const int width = st_.sizes[lvl].w;
        const int height = st_.sizes[lvl].h;
        // Nearest: truncation (C++ path and CVTTPS2DQ in the x86 JIT agree).
        int base_u = TruncToInt(s * static_cast<float>(width) * 256.0f);
        int base_v = TruncToInt(t * static_cast<float>(height) * 256.0f);
        base_u >>= 8;
        base_v >>= 8;
        const int u = ApplyClamp(st_.clamp_s, base_u, width);
        const int v = ApplyClamp(st_.clamp_t, base_v, height);
        return Vec4i::FromRGBA(Fetch(u, v, lvl));
    };
    Vec4i c0 = texel(level);
    if (level_frac) {
        const Vec4i c1 = texel(level + 1);
        for (int i = 0; i < 4; ++i) c0[i] = (c1[i] * level_frac + c0[i] * (16 - level_frac)) >> 4;
    }
    return TextureFunctionOutput(prim_color, c0, st_);
}

Vec4i Sampler::LinearLevel(float s, float t, int level) const {
    const int width = st_.sizes[level].w;
    const int height = st_.sizes[level].h;
    // Bilinear: the x86-64 JIT (Jit_GetTexelCoordsQuad) converts with
    // CVTPS2DQ, i.e. rounds to nearest, while the portable C++ path truncates.
    // The reference captures come from x86-64, so follow the JIT.
    // The subtraction wraps like PADDD.
    int base_u = static_cast<int>(
        static_cast<std::uint32_t>(RoundToInt(s * static_cast<float>(width) * 256.0f)) - 128u);
    int base_v = static_cast<int>(
        static_cast<std::uint32_t>(RoundToInt(t * static_cast<float>(height) * 256.0f)) - 128u);
    const int frac_u = (base_u >> 4) & 0x0F;
    const int frac_v = (base_v >> 4) & 0x0F;
    base_u >>= 8;
    base_v >>= 8;
    const int u0 = ApplyClamp(st_.clamp_s, base_u, width);
    const int u1 = ApplyClamp(st_.clamp_s, base_u + 1, width);
    const int v0 = ApplyClamp(st_.clamp_t, base_v, height);
    const int v1 = ApplyClamp(st_.clamp_t, base_v + 1, height);
    const Vec4i tl = Vec4i::FromRGBA(Fetch(u0, v0, level));
    const Vec4i tr = Vec4i::FromRGBA(Fetch(u1, v0, level));
    const Vec4i bl = Vec4i::FromRGBA(Fetch(u0, v1, level));
    const Vec4i br = Vec4i::FromRGBA(Fetch(u1, v1, level));
    Vec4i out;
    for (int i = 0; i < 4; ++i) {
        const int top = tl[i] * (0x10 - frac_u) + tr[i] * frac_u;
        const int bot = bl[i] * (0x10 - frac_u) + br[i] * frac_u;
        out[i] = (top * (0x10 - frac_v) + bot * frac_v) >> 8;
    }
    return out;
}

Vec4i Sampler::Linear(float s, float t, const Vec4i &prim_color, int level, int level_frac) const {
    Vec4i c0 = LinearLevel(s, t, level);
    if (level_frac) {
        const Vec4i c1 = LinearLevel(s, t, level + 1);
        for (int i = 0; i < 4; ++i) c0[i] = (c1[i] * level_frac + c0[i] * (16 - level_frac)) >> 4;
    }
    return TextureFunctionOutput(prim_color, c0, st_);
}

} // namespace mpo::ge
