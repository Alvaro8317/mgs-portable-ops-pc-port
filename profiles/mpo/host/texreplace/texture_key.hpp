// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/TextureReplacer.cpp (ComputeHash, HashName,
// the key parsing of LoadIniValues), GPU/Common/TextureCacheCommon.h
// (TexCacheEntry::CacheKey) and GPU/Vulkan/TextureCacheVulkan.cpp
// (UpdateCurrentClut, the CLUT hash with the replacer on) @ v1.20.4
// (GPL-2.0-or-later).
//
// F24c: the key of a texture for its replacement, as PPSSPP computes it, so
// that a file name of a PPSSPP texture pack for MPO is the same here:
//
//   <cachekey: 16 hex digits><hash: 8 hex digits>.png
//   cachekey = (texture address & 0x3FFFFFFF) << 32 | dim, and for the CLUT
//              formats ^= XXH32(CLUT bytes, 0xC0108888) ^ the CLUTFORMAT
//              register (dim: TEXSIZE0 & 0xF0F, the log2 of w and h)
//   hash     = the texels in memory with XXH64 (or XXH32), seed 0xBACD7814,
//              truncated to 32 bits; row by row when the buffer is wider
//              than the texture.
//
// With ignoreAddress (textures.ini), the address half of the cachekey is
// left out, so the key is pure content: texels + CLUT + size + CLUT format.
// That is what a pack for MPO wants: its texture files load wherever the
// game puts them, and the same icon is the same file in every stage.
//
// What PPSSPP does differently is listed in docs/sessions/F24c.md (no
// [hashranges], no reduceHash, no 512-row maxSeenV trim, CLUT up to 1 KiB).

#pragma once

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ge_state.hpp"
#include "memory_view.hpp"
#include "sampler.hpp"
#include "texture_provider.hpp"

namespace mpo::texreplace {

enum class HashType : std::uint8_t { kXxh32, kXxh64 };

struct ReplacementKey {
    std::uint64_t cachekey = 0;
    std::uint32_t hash = 0;
    auto operator<=>(const ReplacementKey &) const = default;
};

struct ReplacementKeyHash {
    std::size_t operator()(const ReplacementKey &k) const {
        std::uint64_t h = k.cachekey * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(k.hash) + (h >> 29);
        return static_cast<std::size_t>(h);
    }
};

// "%016llx%08x" (level 0), or with "_<level>" for a mip level.
[[nodiscard]] std::string HashName(const ReplacementKey &key, int level = 0);

// Parses a hash name like PPSSPP's sscanf("%16llx%8x_%d"): up to 16 hex
// digits of cachekey, then up to 8 of hash (0 when missing) and an optional
// "_<level>" (0 when missing). False when it does not start with a hex digit.
bool ParseHashName(std::string_view text, ReplacementKey *key, int *level);

// TexCacheEntry::CacheKey.
[[nodiscard]] std::uint64_t CacheKey(std::uint32_t addr, std::uint32_t format, std::uint32_t dim,
                                     std::uint32_t cluthash);

// clutHash_ ^ gstate.clutformat: XXH32 of min(last load + the start index's
// bytes, the largest load) bytes of the palette (at most its 1 KiB).
[[nodiscard]] std::uint32_t ClutHash(const ge::GeState &gs, std::span<const std::uint8_t> clut,
                                     const ge::ClutLoadInfo &info);

// TextureReplacer::ComputeHash for a texture at `addr` (0 when its bytes run
// past the end of memory, as PPSSPP's sanity check).
[[nodiscard]] std::uint32_t TexelHash(const ge::MemoryView &mem, std::uint32_t addr, int bufw,
                                      int w, int h, ge::TexFormat format, HashType type);

// The cachekey of level 0 of `st` (the texture of the current draw), with
// the address; ReplacementSet masks it out with ignoreAddress.
[[nodiscard]] std::uint64_t CacheKeyFor(const ge::GeState &gs, const ge::SamplerState &st,
                                        const ge::ClutLoadInfo &info);

// Both halves of the key of level 0 of `st`.
[[nodiscard]] ReplacementKey ComputeKey(const ge::GeState &gs, const ge::MemoryView &mem,
                                        const ge::SamplerState &st, const ge::ClutLoadInfo &info,
                                        HashType type);

// The key's address-free form (cachekey & 0xFFFFFFFF).
[[nodiscard]] constexpr ReplacementKey WithoutAddress(ReplacementKey key) {
    key.cachekey &= 0xFFFFFFFFull;
    return key;
}

} // namespace mpo::texreplace
