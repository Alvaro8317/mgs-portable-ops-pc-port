// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Sampler.cpp (the texel addressing of
// SampleFetch, linear and swizzled) @ v1.20.4 (GPL-2.0-or-later), through
// profiles/mpo/host/ge/sampler.cpp.
//
// Level 0 of a GE texture as the GPU image the fragment shader reads with
// texelFetch: unswizzled, w x h texels (the texture size, not the buffer
// width), each texel what softgpu's Fetch reads at (u, v):
// - direct colour formats (5650, 5551, 4444, 8888): RGBA8888, converted with
//   the same functions as mpo_ge;
// - CLUT formats: the raw index (before shift/mask/offset, which the shader
//   applies with the palette): 8 bits for CLUT4 and CLUT8, 16 for CLUT16, 32
//   for CLUT32.
// No Vulkan here, so it is tested without a GPU.

#pragma once

#include <cstdint>
#include <vector>

#include "memory_view.hpp"
#include "sampler.hpp"

namespace mpo::vk {

enum class TexelKind : std::uint8_t {
    kRgba8,   // VK_FORMAT_R8G8B8A8_UNORM
    kIndex8,  // VK_FORMAT_R8_UINT
    kIndex16, // VK_FORMAT_R16_UINT
    kIndex32, // VK_FORMAT_R32_UINT
};

struct DecodedTexture {
    int width = 0;
    int height = 0;
    TexelKind kind = TexelKind::kRgba8;
    std::vector<std::uint8_t> data; // width * height * TexelBytes(kind), row-major
};

[[nodiscard]] TexelKind KindForFormat(ge::TexFormat format);
[[nodiscard]] int TexelBytes(TexelKind kind);
[[nodiscard]] bool IsClutFormat(ge::TexFormat format);

// Byte offset of texel (u, v) from the texture address (PixelDataOffset).
[[nodiscard]] std::uint32_t TexelByteOffset(std::uint32_t bits, std::uint32_t row_pitch,
                                            std::uint32_t u, std::uint32_t v, bool swizzled);

// Bytes from the texture address up to and including the last texel of level
// 0 (the source range a cache hashes). 0 for DXT and invalid sizes.
[[nodiscard]] std::uint32_t TextureSourceBytes(const ge::SamplerState &st);

// Decodes level 0. Reads outside memory give 0, as in softgpu. Returns false
// for formats without a decoder (DXT), leaving `out` zero-filled.
bool DecodeTexture(const ge::MemoryView &mem, const ge::SamplerState &st, DecodedTexture *out);

} // namespace mpo::vk
