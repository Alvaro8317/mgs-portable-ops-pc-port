// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Sampler.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "texture_decode.hpp"

#include <cstring>

#include "color.hpp"

namespace mpo::vk {
namespace {

std::uint32_t BitsPerTexel(ge::TexFormat format) {
    switch (format) {
    case ge::TexFormat::k5650:
    case ge::TexFormat::k5551:
    case ge::TexFormat::k4444:
    case ge::TexFormat::kClut16:
        return 16;
    case ge::TexFormat::k8888:
    case ge::TexFormat::kClut32:
        return 32;
    case ge::TexFormat::kClut8:
        return 8;
    case ge::TexFormat::kClut4:
        return 4;
    default:
        return 0;
    }
}

void Put32(std::uint8_t *dst, std::uint32_t v) {
    std::memcpy(dst, &v, 4);
}

} // namespace

TexelKind KindForFormat(ge::TexFormat format) {
    switch (format) {
    case ge::TexFormat::kClut4:
    case ge::TexFormat::kClut8:
        return TexelKind::kIndex8;
    case ge::TexFormat::kClut16:
        return TexelKind::kIndex16;
    case ge::TexFormat::kClut32:
        return TexelKind::kIndex32;
    default:
        return TexelKind::kRgba8;
    }
}

int TexelBytes(TexelKind kind) {
    switch (kind) {
    case TexelKind::kIndex8:
        return 1;
    case TexelKind::kIndex16:
        return 2;
    case TexelKind::kIndex32:
    case TexelKind::kRgba8:
    default:
        return 4;
    }
}

bool IsClutFormat(ge::TexFormat format) {
    return format >= ge::TexFormat::kClut4 && format <= ge::TexFormat::kClut32;
}

std::uint32_t TexelByteOffset(std::uint32_t bits, std::uint32_t row_pitch, std::uint32_t u,
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

std::uint32_t TextureSourceBytes(const ge::SamplerState &st) {
    const std::uint32_t bits = BitsPerTexel(st.texfmt);
    const int w = st.sizes[0].w;
    const int h = st.sizes[0].h;
    if (bits == 0 || w <= 0 || h <= 0 || st.texbufw[0] <= 0) return 0;
    // The offset grows with u and with v (linear and swizzled), so the last
    // texel is the farthest one.
    const std::uint32_t last = TexelByteOffset(bits, static_cast<std::uint32_t>(st.texbufw[0]),
                                               static_cast<std::uint32_t>(w - 1),
                                               static_cast<std::uint32_t>(h - 1), st.swizzle);
    return last + (bits >= 8 ? bits / 8 : 1);
}

bool DecodeTexture(const ge::MemoryView &mem, const ge::SamplerState &st, DecodedTexture *out) {
    out->width = st.sizes[0].w;
    out->height = st.sizes[0].h;
    out->kind = KindForFormat(st.texfmt);
    const auto texel_bytes = static_cast<std::size_t>(TexelBytes(out->kind));
    out->data.assign(static_cast<std::size_t>(out->width) * static_cast<std::size_t>(out->height) *
                         texel_bytes,
                     std::uint8_t{0});
    const std::uint32_t bits = BitsPerTexel(st.texfmt);
    if (bits == 0 || out->width <= 0 || out->height <= 0) return false;

    const std::uint32_t base = st.texaddr[0];
    const auto bufw = static_cast<std::uint32_t>(st.texbufw[0]);
    const bool sw = st.swizzle;
    std::uint8_t *dst = out->data.data();
    for (int y = 0; y < out->height; ++y) {
        for (int x = 0; x < out->width; ++x, dst += texel_bytes) {
            const auto u = static_cast<std::uint32_t>(x);
            const auto v = static_cast<std::uint32_t>(y);
            const std::uint32_t addr = base + TexelByteOffset(bits, bufw, u, v, sw);
            switch (st.texfmt) {
            case ge::TexFormat::k4444:
                Put32(dst, ge::RGBA4444ToRGBA8888(mem.Read16(addr)));
                break;
            case ge::TexFormat::k5551:
                Put32(dst, ge::RGBA5551ToRGBA8888(mem.Read16(addr)));
                break;
            case ge::TexFormat::k5650:
                Put32(dst, ge::RGB565ToRGBA8888(mem.Read16(addr)));
                break;
            case ge::TexFormat::k8888:
                Put32(dst, mem.Read32(addr));
                break;
            case ge::TexFormat::kClut32:
                Put32(dst, mem.Read32(addr));
                break;
            case ge::TexFormat::kClut16: {
                const std::uint16_t i = mem.Read16(addr);
                std::memcpy(dst, &i, 2);
                break;
            }
            case ge::TexFormat::kClut8:
                *dst = mem.Read8(addr);
                break;
            case ge::TexFormat::kClut4: {
                const std::uint8_t byte = mem.Read8(addr);
                *dst = static_cast<std::uint8_t>((u & 1) ? (byte >> 4) : (byte & 0xF));
                break;
            }
            default:
                break;
            }
        }
    }
    return true;
}

} // namespace mpo::vk
