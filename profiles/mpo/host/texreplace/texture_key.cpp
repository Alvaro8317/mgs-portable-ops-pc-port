// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/TextureReplacer.cpp, GPU/Common/
// TextureCacheCommon.h/.cpp and GPU/Vulkan/TextureCacheVulkan.cpp
// @ v1.20.4 (GPL-2.0-or-later).

#include "texture_key.hpp"

#include <algorithm>
#include <cstdio>

#include "xxhash.h"

namespace mpo::texreplace {
namespace {

constexpr std::uint32_t kTexelSeed = 0xBACD7814;
constexpr std::uint32_t kClutSeed = 0xC0108888;

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::uint32_t HashBytes(const std::uint8_t *data, std::uint32_t size, HashType type) {
    if (type == HashType::kXxh32) return XXH32(data, size, kTexelSeed);
    return static_cast<std::uint32_t>(XXH64(data, size, kTexelSeed));
}

} // namespace

std::string HashName(const ReplacementKey &key, int level) {
    char name[48];
    if (level > 0) {
        std::snprintf(name, sizeof(name), "%016llx%08x_%d",
                      static_cast<unsigned long long>(key.cachekey), key.hash, level);
    } else {
        std::snprintf(name, sizeof(name), "%016llx%08x",
                      static_cast<unsigned long long>(key.cachekey), key.hash);
    }
    return name;
}

bool ParseHashName(std::string_view text, ReplacementKey *key, int *level) {
    // sscanf("%16llx%8x_%d") skips leading white space and reads up to 16
    // and then up to 8 hex digits.
    std::size_t pos = 0;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    auto read_hex = [&](std::size_t max_digits, std::uint64_t *out) {
        std::size_t n = 0;
        std::uint64_t v = 0;
        while (n < max_digits && pos < text.size() && HexDigit(text[pos]) >= 0) {
            v = (v << 4) | static_cast<std::uint64_t>(HexDigit(text[pos]));
            ++pos;
            ++n;
        }
        *out = v;
        return n != 0;
    };
    std::uint64_t cachekey = 0;
    if (!read_hex(16, &cachekey)) return false;
    std::uint64_t hash = 0;
    (void)read_hex(8, &hash);
    int lvl = 0;
    if (pos + 1 < text.size() && text[pos] == '_' && text[pos + 1] >= '0' && text[pos + 1] <= '9') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && lvl < 1000) {
            lvl = lvl * 10 + (text[pos] - '0');
            ++pos;
        }
    }
    key->cachekey = cachekey;
    key->hash = static_cast<std::uint32_t>(hash);
    if (level != nullptr) *level = lvl;
    return true;
}

std::uint64_t CacheKey(std::uint32_t addr, std::uint32_t format, std::uint32_t dim,
                       std::uint32_t cluthash) {
    std::uint64_t cachekey = (static_cast<std::uint64_t>(addr & 0x3FFFFFFF) << 32) | (dim & 0xFFFF);
    if ((format & 4) != 0) cachekey ^= cluthash;
    return cachekey;
}

std::uint32_t ClutHash(const ge::GeState &gs, std::span<const std::uint8_t> clut,
                       const ge::ClutLoadInfo &info) {
    const bool bits32 = gs.ClutPaletteFormat() == ge::ClutFormat::k8888;
    const auto base_bytes = static_cast<std::uint32_t>(gs.ClutIndexStartPos()) * (bits32 ? 4u : 2u);
    std::uint32_t bytes = std::min(info.last_bytes + base_bytes, info.max_bytes);
    // PPSSPP keeps 2 KiB (loads of 0x40 blocks); the GE's palette is 1 KiB.
    bytes = std::min<std::uint32_t>(bytes, static_cast<std::uint32_t>(clut.size()));
    return XXH32(clut.data(), bytes, kClutSeed) ^ gs.Reg(ge::kCmdClutFormat);
}

std::uint32_t TexelHash(const ge::MemoryView &mem, std::uint32_t addr, int bufw, int w, int h,
                        ge::TexFormat format, HashType type) {
    const std::uint32_t bpp = ge::kTextureBitsPerPixel[static_cast<std::size_t>(format) & 15];
    if (w <= 0 || h <= 0) return 0;
    const auto uw = static_cast<std::uint32_t>(w);
    const auto uh = static_cast<std::uint32_t>(h);
    const auto ubufw = static_cast<std::uint32_t>(std::max(bufw, 0));
    if (ubufw <= uw) {
        // Contiguous: the texels used, from the first to the last.
        const std::uint32_t total_pixels = ubufw * uh + (uw - ubufw);
        const std::uint32_t size = (bpp * total_pixels) / 8;
        const std::uint8_t *p = mem.Translate(addr, size);
        if (p == nullptr) return 0;
        return HashBytes(p, size, type);
    }
    // Gaps between the rows: hash each row and combine.
    const std::uint32_t row_bytes = (bpp * uw) / 8;
    const std::uint32_t stride = (bpp * ubufw) / 8;
    std::uint32_t result = 0;
    for (std::uint32_t y = 0; y < uh; ++y) {
        const std::uint8_t *p = mem.Translate(addr + y * stride, row_bytes);
        // PPSSPP reads past the end here; an unreadable row hashes as empty.
        const std::uint32_t row =
            p != nullptr ? HashBytes(p, row_bytes, type) : HashBytes(nullptr, 0, type);
        result = (result * 11) ^ row;
    }
    return result;
}

std::uint64_t CacheKeyFor(const ge::GeState &gs, const ge::SamplerState &st,
                          const ge::ClutLoadInfo &info) {
    auto format = static_cast<std::uint32_t>(gs.TextureFormat());
    if (format >= 11) format = static_cast<std::uint32_t>(ge::TexFormat::k5650);
    const std::uint32_t dim = gs.Reg(ge::kCmdTexSize0) & 0xF0F;
    std::uint32_t cluthash = 0;
    if ((format & 4) != 0 && st.clut != nullptr)
        cluthash = ClutHash(gs, std::span<const std::uint8_t>(st.clut, 1024), info);
    return CacheKey(st.texaddr[0], format, dim, cluthash);
}

ReplacementKey ComputeKey(const ge::GeState &gs, const ge::MemoryView &mem,
                          const ge::SamplerState &st, const ge::ClutLoadInfo &info, HashType type) {
    ReplacementKey key;
    key.cachekey = CacheKeyFor(gs, st, info);
    // SetTexture takes an invalid format (11 and up) as 5650.
    const ge::TexFormat format =
        static_cast<int>(st.texfmt) >= 11 ? ge::TexFormat::k5650 : st.texfmt;
    key.hash =
        TexelHash(mem, st.texaddr[0], st.texbufw[0], st.sizes[0].w, st.sizes[0].h, format, type);
    return key;
}

} // namespace mpo::texreplace
