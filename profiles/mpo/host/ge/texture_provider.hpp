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
//
// F26a: a replacement may have mip levels (the pack's own, or generated from
// a larger image down to the native size), and a textures.ini may force a
// texture's filtering ([filtering]). Both renderers choose the level with
// softgpu's rule (CalculateSamplingParams), in the replacement's own texels:
// the GPU over the whole chain (`mips`), softgpu over the levels at or below
// the native size (`native` and `native_mips`). So the levels finer than
// native are only seen on the GPU, and a native-size PNG without levels of
// its own draws exactly as before.

#pragma once

#include <cstdint>
#include <vector>

#include "ge_state.hpp"
#include "memory_view.hpp"

namespace mpo::ge {

struct SamplerState;

// F26a: [filtering] of textures.ini (PPSSPP's TextureFiltering for a
// replacement): kAuto keeps the game's filters.
enum class TextureFilter : std::uint8_t { kAuto, kNearest, kLinear };

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

    // F26a: mip levels 1, 2... of `image` (each half the size of the one
    // before, at least 1), RGBA8 bytes. Empty: one level.
    struct Level {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
    };
    std::vector<Level> mips;
    // The levels of the chain coarser than the GE texture's size, at w >> i
    // x h >> i (i = 1, 2...), RGBA8888 as `native`: what softgpu samples
    // after `native`. Empty: softgpu has level 0 only.
    struct NativeLevel {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> texels;
    };
    std::vector<NativeLevel> native_mips;
};

// What PPSSPP's texture cache knows about the CLUT besides its bytes, which
// its replacement key hashes: the size of the last LOADCLUT
// (clutTotalBytes_) and the largest one so far (clutMaxBytes_).
struct ClutLoadInfo {
    std::uint32_t last_bytes = 0;
    std::uint32_t max_bytes = 0;
};

// F25c: `rows` rows of `row_bytes` bytes each, `stride` bytes apart, from
// `addr` (a PSP address, with or without the VRAM base).
struct MemoryRect {
    std::uint32_t addr = 0;
    std::uint32_t stride = 0;
    std::uint32_t row_bytes = 0;
    std::uint32_t rows = 0;
    // From the first byte to the last, inclusive of the gaps between rows.
    [[nodiscard]] std::uint32_t span() const {
        return rows == 0 || row_bytes == 0 ? 0u : (rows - 1) * stride + row_bytes;
    }
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

    // F25c: what lets a provider tell framebuffers and video frames from
    // textures in VRAM (PPSSPP never replaces either). All of them are
    // called on the GE's thread (the emulated CPU's), in the guest's order.
    //
    // Every draw command (PRIM, BEZIER, SPLINE), textured or not, before its
    // texture is looked up: `gs` says where it draws (FBP/FBW/PSM, ZBP/ZBW,
    // scissor, clear mode and depth writes).
    virtual void BeginDraw(const GeState &gs) { (void)gs; }
    // A block transfer copied `src` to `dst` (both rectangles as the GE
    // walks them). The default reports the write as MemoryWritten.
    virtual void BlockTransfer(const MemoryRect &src, const MemoryRect &dst) {
        (void)src;
        MemoryWritten(dst.addr, dst.span());
    }
    // sceMpeg wrote a decoded video frame to [addr, addr + size)
    // (what fills PPSSPP's TextureCacheCommon::videos_).
    virtual void VideoWritten(std::uint32_t addr, std::uint32_t size) {
        (void)addr;
        (void)size;
    }
    // A display flip (once per frame shown): `shown` is the framebuffer the
    // display shows from now on (its visible 480 x 272 rectangle), or
    // nullptr when there is none.
    virtual void Flip(const MemoryRect *shown) { (void)shown; }
    // The replacement for level 0 of the texture `st` samples (`clut`: the
    // GE's 1 KiB palette), or nullptr. The pointer stays valid at least
    // until the next BeginRun.
    virtual const ReplacedTexture *Find(const GeState &gs, const MemoryView &mem,
                                        const SamplerState &st, const ClutLoadInfo &clut) = 0;
    // F26a: the filtering [filtering] forces on the texture of the last
    // Find (replaced or not), as PPSSPP's ReplacedTexture::ForceFiltering.
    // Ge applies it to both renderers' state.
    [[nodiscard]] virtual TextureFilter Filtering() const { return TextureFilter::kAuto; }
};

} // namespace mpo::ge
