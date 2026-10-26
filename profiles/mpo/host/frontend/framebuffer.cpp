// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Common/Data/Convert/ColorConv.{h,cpp} @ v1.20.4
// (GPL-2.0-or-later): the scalar paths of ConvertRGB565ToRGBA8888,
// ConvertRGBA5551ToRGBA8888 and ConvertRGBA4444ToRGBA8888 and the
// Convert4To8/5To8/6To8 bit replication. The SSE paths give the same result.

#include "framebuffer.h"

#include <algorithm>
#include <cstring>

namespace mpo::frontend {
namespace {

constexpr std::uint8_t Convert4To8(std::uint32_t v) {
    // 00001234 -> 12341234
    return static_cast<std::uint8_t>((v << 4) | v);
}
constexpr std::uint8_t Convert5To8(std::uint32_t v) {
    // 00012345 -> 12345123
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}
constexpr std::uint8_t Convert6To8(std::uint32_t v) {
    // 00123456 -> 12345612
    return static_cast<std::uint8_t>((v << 2) | (v >> 4));
}

std::uint16_t Load16(const std::uint8_t *p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

constexpr int kMaxDimension = 4096;

} // namespace

std::string_view PixelFormatName(PixelFormat format) {
    switch (format) {
    case PixelFormat::kRgb565:
        return "565";
    case PixelFormat::kRgba5551:
        return "5551";
    case PixelFormat::kRgba4444:
        return "4444";
    case PixelFormat::kRgba8888:
        return "8888";
    }
    return "?";
}

bool PixelFormatFromValue(std::uint32_t value, PixelFormat *out) {
    if (value > 3) return false;
    *out = static_cast<PixelFormat>(value);
    return true;
}

std::string_view ConvertStatusName(ConvertStatus status) {
    switch (status) {
    case ConvertStatus::kOk:
        return "ok";
    case ConvertStatus::kBlank:
        return "blank";
    case ConvertStatus::kPartial:
        return "partial";
    case ConvertStatus::kInvalid:
        return "invalid";
    }
    return "?";
}

const std::uint8_t *GuestMemory::Resolve(std::uint32_t address, std::size_t size) const {
    const std::uint32_t a = Canonical(address);
    for (const MemoryRegion &region : regions_) {
        if (a < region.base) continue;
        const std::size_t offset = a - region.base;
        if (offset > region.bytes.size() || size > region.bytes.size() - offset) continue;
        return region.bytes.data() + offset;
    }
    return nullptr;
}

void ConvertRow(PixelFormat format, const std::uint8_t *src, int count, std::uint8_t *dst,
                bool keep_alpha) {
    switch (format) {
    case PixelFormat::kRgb565:
        // PSP 565: R in bits 0-4, G in 5-10, B in 11-15. Always opaque.
        for (int i = 0; i < count; ++i, src += 2, dst += 4) {
            const std::uint32_t c = Load16(src);
            dst[0] = Convert5To8(c & 0x1F);
            dst[1] = Convert6To8((c >> 5) & 0x3F);
            dst[2] = Convert5To8((c >> 11) & 0x1F);
            dst[3] = 0xFF;
        }
        break;
    case PixelFormat::kRgba5551:
        for (int i = 0; i < count; ++i, src += 2, dst += 4) {
            const std::uint32_t c = Load16(src);
            dst[0] = Convert5To8(c & 0x1F);
            dst[1] = Convert5To8((c >> 5) & 0x1F);
            dst[2] = Convert5To8((c >> 10) & 0x1F);
            dst[3] = static_cast<std::uint8_t>(!keep_alpha || (c >> 15) != 0 ? 0xFF : 0x00);
        }
        break;
    case PixelFormat::kRgba4444:
        for (int i = 0; i < count; ++i, src += 2, dst += 4) {
            const std::uint32_t c = Load16(src);
            dst[0] = Convert4To8(c & 0xF);
            dst[1] = Convert4To8((c >> 4) & 0xF);
            dst[2] = Convert4To8((c >> 8) & 0xF);
            dst[3] = keep_alpha ? Convert4To8((c >> 12) & 0xF) : std::uint8_t{0xFF};
        }
        break;
    case PixelFormat::kRgba8888:
        // Already R, G, B, A in memory order.
        std::memcpy(dst, src, static_cast<std::size_t>(count) * 4);
        if (!keep_alpha) {
            for (int i = 0; i < count; ++i) dst[static_cast<std::size_t>(i) * 4 + 3] = 0xFF;
        }
        break;
    }
}

ConvertResult ConvertFramebuffer(const GuestMemory &memory, const FramebufferDesc &desc,
                                 std::span<std::uint8_t> dst_rgba, const ConvertOptions &options) {
    ConvertResult result;
    const bool size_ok = desc.width > 0 && desc.height > 0 && desc.width <= kMaxDimension &&
                         desc.height <= kMaxDimension &&
                         dst_rgba.size() >= static_cast<std::size_t>(desc.width) *
                                                static_cast<std::size_t>(desc.height) * 4;
    if (!size_ok || desc.stride < static_cast<std::uint32_t>(desc.width) ||
        desc.stride > kMaxDimension || static_cast<std::uint32_t>(desc.format) > 3) {
        if (size_ok) {
            std::fill_n(dst_rgba.begin(),
                        static_cast<std::size_t>(desc.width) *
                            static_cast<std::size_t>(desc.height) * 4,
                        std::uint8_t{0});
        }
        result.status = ConvertStatus::kInvalid;
        return result;
    }

    const std::size_t row_out = static_cast<std::size_t>(desc.width) * 4;
    const std::uint32_t bpp = BytesPerPixel(desc.format);
    const std::size_t row_in = static_cast<std::size_t>(desc.width) * bpp;
    const std::uint32_t pitch = desc.stride * bpp;
    if (desc.address == 0) {
        std::fill_n(dst_rgba.begin(), row_out * static_cast<std::size_t>(desc.height),
                    std::uint8_t{0});
        for (std::size_t i = 3; i < row_out * static_cast<std::size_t>(desc.height); i += 4) {
            dst_rgba[i] = 0xFF;
        }
        result.status = ConvertStatus::kBlank;
        return result;
    }

    for (int y = 0; y < desc.height; ++y) {
        std::uint8_t *out = dst_rgba.data() + static_cast<std::size_t>(y) * row_out;
        // Each row is resolved on its own: a framebuffer near the end of VRAM
        // continues in the next mirror, which folds back to the start.
        const std::uint32_t row_addr = desc.address + static_cast<std::uint32_t>(y) * pitch;
        const std::uint8_t *src = memory.Resolve(row_addr, row_in);
        if (src == nullptr) {
            for (std::size_t i = 0; i < row_out; i += 4) {
                out[i] = 0;
                out[i + 1] = 0;
                out[i + 2] = 0;
                out[i + 3] = 0xFF;
            }
            ++result.rows_missing;
            continue;
        }
        ConvertRow(desc.format, src, desc.width, out, options.keep_alpha);
    }
    if (result.rows_missing > 0) result.status = ConvertStatus::kPartial;
    return result;
}

} // namespace mpo::frontend
