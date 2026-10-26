// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/TransformUnit.cpp (ComputeTransformState,
// ReadVertex, ClipToScreenInternal) @ v1.20.4 (GPL-2.0-or-later).
//
// Turns a decoded vertex into a ClipVertexData: in through mode the position
// is already in screen space; in transform mode it goes through the
// world/view/projection matrices, the viewport, the screen offset and the
// depth clamp, and gets fog depth, generated texture coordinates and
// lighting.

#pragma once

#include <cstdint>

#include "ge_state.hpp"
#include "lighting.hpp"
#include "math3d.hpp"
#include "rasterizer.hpp"
#include "vertex_decoder.hpp"

namespace mpo::ge {

struct TransformState {
    LightingState lighting;
    float matrix[16] = {};
    Vec4f pos_to_fog;
    Vec3f screen_scale;
    Vec3f screen_add;
    bool enable_transform = false;
    bool enable_lighting = false;
    bool enable_fog = false;
    bool read_uv = false;
    bool negate_normals = false;
    bool uses_world_normal = false;
    // MatrixMode::WORLD_TO_CLIP (lighting needs the world position) instead
    // of POS_TO_CLIP.
    bool world_to_clip = false;
    bool depth_clamp = false;
    TexMapMode uv_gen_mode = TexMapMode::kTextureCoords;
};

// What the vertex format provides, as VertexReader::has*().
struct VertexFormat {
    bool through = false;
    bool has_uv = false;
    bool has_color0 = false;
    bool has_normal = false;
};

TransformState ComputeTransformState(const GeState &gs, const VertexFormat &fmt);

// The static lastTC and lastnormal of ReadVertex: a vertex without UV or
// normal reuses the last one read, even from an earlier draw.
struct VertexHistory {
    float s = 0;
    float t = 0;
    float q = 0;
    Vec3f normal;
};

ClipVertexData TransformVertex(const GeState &gs, const TransformState &ts, const VertexFormat &fmt,
                               const DecodedVertex &dv, VertexHistory *history);

// TransformUnit::ClipToScreen (always checks the range), used by the clipper
// for the new vertices.
ScreenCoords ClipToScreen(const GeState &gs, const ClipCoords &coords, bool *outside_range);

// PPSSPP's "outside range" marker on a vertex.
inline constexpr int kOutsideRange = 0x7FFFFFFF;

} // namespace mpo::ge
