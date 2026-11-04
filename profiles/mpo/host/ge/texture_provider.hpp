// SPDX-License-Identifier: GPL-3.0-or-later
//
// F24c: texture replacement, as the GE sees it. A TextureProvider (the
// replacement packs of mpo_texreplace) may hand the GE, for level 0 of the
// texture of a draw, an image of its own. mpo_ge only defines the interface,
// so it still depends on nothing but the C++ library; without a provider
// (Ge::SetTextureProvider(nullptr), the default) nothing changes.
//
// Both renderers sample the same replacement: softgpu reads `native` (the
// image at the GE texture's size, RGBA8888 as Sampler::Fetch returns texels)
// instead of the texels in memory; the Vulkan renderer uploads `image` at its
// own size and samples it with the same UV mapping. With a PNG of the
// texture's size, both are the same pixels.

#pragma once

#include <cstdint>
#include <vector>

#include "ge_state.hpp"
#include "memory_view.hpp"

namespace mpo::ge {

struct SamplerState;

struct ReplacedTexture {
    // At the GE texture's size (level 0: w x h), RGBA8888 (0xAABBGGRR).
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> native;
    // The replacement at its own size, RGBA8 bytes, row-major.
    int image_width = 0;
    int image_height = 0;
    std::vector<std::uint8_t> image;
    // Unique for the process (a GPU cache keys its images with it).
    std::uint64_t id = 0;
};

// What PPSSPP's texture cache knows about the CLUT besides its bytes, which
// its replacement key hashes: the size of the last LOADCLUT
// (clutTotalBytes_) and the largest one so far (clutMaxBytes_).
struct ClutLoadInfo {
    std::uint32_t last_bytes = 0;
    std::uint32_t max_bytes = 0;
};

class TextureProvider {
public:
    virtual ~TextureProvider() = default;
    // Ge::Run starts: the CPU may have written memory since the last run.
    virtual void BeginRun() {}
    // A block transfer wrote [addr, addr + size).
    virtual void MemoryWritten(std::uint32_t addr, std::uint32_t size) {
        (void)addr;
        (void)size;
    }
    // The replacement for level 0 of the texture `st` samples (`clut`: the
    // GE's 1 KiB palette), or nullptr. The pointer stays valid at least
    // until the next BeginRun.
    virtual const ReplacedTexture *Find(const GeState &gs, const MemoryView &mem,
                                        const SamplerState &st, const ClutLoadInfo &clut) = 0;
};

} // namespace mpo::ge
