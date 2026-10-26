// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/FuncId.cpp (SamplerID, ComputeSamplerID)
// and GPU/Software/Sampler.cpp (portable sampler, no JIT) @ v1.20.4
// (GPL-2.0-or-later).

#pragma once

#include <cstdint>

#include "color.hpp"
#include "ge_constants.hpp"
#include "ge_state.hpp"
#include "memory_view.hpp"

namespace mpo::ge {

struct SamplerState {
    TexFormat texfmt = TexFormat::k5650;
    ClutFormat clutfmt = ClutFormat::k565;
    bool clamp_s = false;
    bool clamp_t = false;
    bool swizzle = false;
    bool use_shared_clut = true;
    bool has_clut_mask = false;
    bool has_clut_shift = false;
    bool has_clut_offset = false;
    bool has_invalid_ptr = false;
    int width0_shift = 0;
    int height0_shift = 0;
    TexFunc tex_func = TexFunc::kModulate;
    bool use_texture_alpha = false;
    bool use_color_doubling = false;
    bool has_any_mips = false;

    struct Size {
        int w = 0;
        int h = 0;
    };
    Size sizes[8];
    std::uint32_t tex_blend_color = 0;
    std::uint32_t clut_format = 0;
    // The GE's internal palette (1024 bytes), loaded by LOADCLUT.
    const std::uint8_t *clut = nullptr;

    std::uint32_t texaddr[8] = {};
    int texbufw[8] = {};
};

// Texture buffer width in texels for `level` (GetTextureBufw).
int TextureBufw(const GeState &gs, int level, TexFormat format);

SamplerState ComputeSamplerState(const GeState &gs, const MemoryView &mem,
                                 const std::uint8_t *clut);

class Sampler {
public:
    Sampler(const MemoryView &mem, const SamplerState &state) : mem_(mem), st_(state) {}

    // Raw texel at integer coordinates (no wrap), RGBA8888 (SampleFetch).
    [[nodiscard]] std::uint32_t Fetch(int u, int v, int level) const;
    // Nearest and bilinear sampling with the texture function applied
    // (SampleNearest / SampleLinear). `s` and `t` are normalised.
    [[nodiscard]] Vec4i Nearest(float s, float t, const Vec4i &prim_color, int level,
                                int level_frac) const;
    [[nodiscard]] Vec4i Linear(float s, float t, const Vec4i &prim_color, int level,
                               int level_frac) const;

    [[nodiscard]] const SamplerState &state() const { return st_; }

private:
    [[nodiscard]] std::uint32_t LookupColor(std::uint32_t index, int level) const;
    [[nodiscard]] std::uint32_t TransformClutIndex(std::uint32_t index) const;
    [[nodiscard]] Vec4i LinearLevel(float s, float t, int level) const;

    const MemoryView &mem_;
    const SamplerState &st_;
};

Vec4i TextureFunctionOutput(const Vec4i &prim_color, const Vec4i &texcolor, const SamplerState &st);

} // namespace mpo::ge
