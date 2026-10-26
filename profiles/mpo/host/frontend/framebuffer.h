// SPDX-License-Identifier: GPL-3.0-or-later
//
// PSP display framebuffer -> RGBA8 conversion, the CPU half of the
// presentation path. No SDL here: the runtime (F10b) and the tests use it
// directly, and SdlPresenter (sdl_presenter.h) uploads its output.
//
// The framebuffer is described the way sceDisplaySetFrameBuf receives it: a
// PSP address (VRAM or RAM), a stride in pixels (bufferwidth) and a pixel
// format. Guest memory is passed in as spans at their PSP addresses, so this
// code never owns or allocates PSP memory.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mpo::frontend {

// PSP_DISPLAY_PIXEL_FORMAT_* (the same values as GE_FORMAT_*).
enum class PixelFormat : std::uint32_t {
    kRgb565 = 0,
    kRgba5551 = 1,
    kRgba4444 = 2,
    kRgba8888 = 3,
};

inline constexpr int kPspScreenWidth = 480;
inline constexpr int kPspScreenHeight = 272;

[[nodiscard]] constexpr std::uint32_t BytesPerPixel(PixelFormat format) {
    return format == PixelFormat::kRgba8888 ? 4u : 2u;
}
[[nodiscard]] std::string_view PixelFormatName(PixelFormat format);
// Accepts 0-3 only.
[[nodiscard]] bool PixelFormatFromValue(std::uint32_t value, PixelFormat *out);

// One piece of host memory that backs PSP memory starting at `base` (a
// canonical address, i.e. already masked with 0x1FFFFFFF).
struct MemoryRegion {
    std::uint32_t base = 0;
    std::span<const std::uint8_t> bytes;
};

// Read-only view of PSP memory made of regions (typically VRAM at 0x04000000
// and RAM at 0x08000000). Addresses are folded like the hardware does: the
// cached/uncached/kernel aliases with `addr & 0x1FFFFFFF`, and the four VRAM
// mirrors (0x04000000-0x047FFFFF) onto the 2 MiB of VRAM.
class GuestMemory {
public:
    static constexpr std::uint32_t kVramBase = 0x04000000;
    static constexpr std::uint32_t kVramSize = 0x00200000;
    static constexpr std::uint32_t kVramMirrorEnd = 0x04800000;
    static constexpr std::uint32_t kRamBase = 0x08000000;

    GuestMemory() = default;
    explicit GuestMemory(std::span<const MemoryRegion> regions) {
        for (const MemoryRegion &region : regions) AddRegion(region.base, region.bytes);
    }

    void AddRegion(std::uint32_t base, std::span<const std::uint8_t> bytes) {
        regions_.push_back({Canonical(base), bytes});
    }

    [[nodiscard]] static constexpr std::uint32_t Canonical(std::uint32_t address) {
        const std::uint32_t a = address & 0x1FFFFFFF;
        if (a >= kVramBase && a < kVramMirrorEnd) return kVramBase + (a & (kVramSize - 1));
        return a;
    }

    // Host pointer to `size` contiguous bytes at `address`, or nullptr when the
    // range is not entirely inside one region.
    [[nodiscard]] const std::uint8_t *Resolve(std::uint32_t address, std::size_t size) const;

private:
    std::vector<MemoryRegion> regions_;
};

// What sceDisplaySetFrameBuf stores: `stride` is bufferwidth, in pixels.
struct FramebufferDesc {
    std::uint32_t address = 0;
    std::uint32_t stride = 512;
    PixelFormat format = PixelFormat::kRgba8888;
    int width = kPspScreenWidth;
    int height = kPspScreenHeight;
};

struct ConvertOptions {
    // The alpha bits of a PSP framebuffer hold stencil, not transparency, and
    // the display ignores them. Keep them only for debugging.
    bool keep_alpha = false;
};

enum class ConvertStatus {
    kOk,
    // Address 0: the PSP shows black (sceDisplaySetFrameBuf(0, ...)).
    kBlank,
    // Some rows were outside guest memory; they were painted black.
    kPartial,
    // Bad size or stride; nothing was converted (the output is black).
    kInvalid,
};

struct ConvertResult {
    ConvertStatus status = ConvertStatus::kOk;
    int rows_missing = 0;
};

[[nodiscard]] std::string_view ConvertStatusName(ConvertStatus status);

// Converts `count` pixels of `format` to RGBA8 (bytes R, G, B, A in memory,
// i.e. SDL_PIXELFORMAT_RGBA32). `src` needs count * BytesPerPixel bytes and
// may be unaligned.
void ConvertRow(PixelFormat format, const std::uint8_t *src, int count, std::uint8_t *dst_rgba,
                bool keep_alpha);

// Converts a whole framebuffer into `dst_rgba` (width * height * 4 bytes,
// tightly packed). Rows that cannot be resolved come out black.
ConvertResult ConvertFramebuffer(const GuestMemory &memory, const FramebufferDesc &desc,
                                 std::span<std::uint8_t> dst_rgba,
                                 const ConvertOptions &options = {});

} // namespace mpo::frontend
