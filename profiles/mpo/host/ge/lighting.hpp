// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Lighting.{h,cpp} @ v1.20.4
// (GPL-2.0-or-later).
//
// Per-vertex lighting of the transform unit. Follows the x86-64 SSE4.1 path
// (ProcessSIMD<true>), the one the reference captures come from.

#pragma once

#include <cstdint>

#include "color.hpp"
#include "ge_state.hpp"
#include "math3d.hpp"

namespace mpo::ge {

struct VertexData;

struct LightingState {
    struct Light {
        Vec3f pos; // pre-normalised when directional
        Vec3f att;
        Vec3f spot_dir;
        float spot_cutoff = 0;
        float spot_exp = 0;
        Vec4i ambient_color_factor;
        Vec4i diffuse_color_factor;
        Vec4i specular_color_factor;
        bool enabled = false;
        bool spot = false;
        bool directional = false;
        bool powered_diffuse = false;
        bool ambient = false;
        bool diffuse = false;
        bool specular = false;
    };
    Light lights[4];
    Vec4i material_ambient_color_factor;
    Vec4i material_diffuse_color_factor;
    Vec4i material_specular_color_factor;
    Vec4i base_ambient_color_factor;
    float specular_exp = 0;
    bool color_for_ambient = false;
    bool color_for_diffuse = false;
    bool color_for_specular = false;
    bool set_color1 = false;
    bool add_color1 = false;
    bool uses_world_pos = false;
    bool uses_world_normal = false;
};

LightingState ComputeLightingState(const GeState &gs, bool has_color0);

// Lighting::GenerateLightST: environment-map texture coordinates.
void GenerateLightST(const GeState &gs, VertexData &vertex, const Vec3f &worldnormal);

// Lighting::Process: replaces color0 (and color1) of `vertex`.
void ProcessLighting(const GeState &gs, VertexData &vertex, const Vec3f &worldpos,
                     const Vec3f &worldnormal, const LightingState &state);

} // namespace mpo::ge
