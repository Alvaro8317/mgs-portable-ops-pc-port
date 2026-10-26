// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/TransformUnit.cpp @ v1.20.4
// (GPL-2.0-or-later).

#include "transform.hpp"

#include <bit>
#include <cmath>

#include "color.hpp"

namespace mpo::ge {
namespace {

bool IsNanOrInf(float f) {
    return (std::bit_cast<std::uint32_t>(f) & 0x7F800000u) == 0x7F800000u;
}

// ClipToScreenInternal<depthClamp, alwaysCheckRange>.
ScreenCoords RoundToScreen(const GeState &gs, bool depth_clamp, bool always_check_range, float sx,
                           float sy, float sz, const ClipCoords &coords, bool *outside_range) {
    // Account for rounding for X and Y.
    constexpr float kScreenBound = 4095.0f + (15.5f / 16.0f);
    if (depth_clamp) {
        // If the depth is clipped (z/w <= -1), the flag is not set, even for
        // x and y.
        if ((always_check_range || coords.z > -coords.w) &&
            (sx >= kScreenBound || sy >= kScreenBound || sx < 0 || sy < 0)) {
            *outside_range = true;
        }
        if (sz < 0.f) {
            sz = 0.f;
        } else if (sz > 65535.0f) {
            sz = 65535.0f;
        }
    } else if (sx > kScreenBound || sy >= kScreenBound || sx < 0 || sy < 0 || sz < 0.0f ||
               sz >= 65536.0f) {
        *outside_range = true;
    }
    // Round up at 0.625 to the nearest subpixel.
    ScreenCoords ret;
    ret.x = TruncToInt(sx * 16.0f + 0.375f - static_cast<float>(gs.OffsetX16()));
    ret.y = TruncToInt(sy * 16.0f + 0.375f - static_cast<float>(gs.OffsetY16()));
    // float -> u16 as the x86-64 build does it: CVTTSS2SI, low 16 bits.
    ret.z = static_cast<std::uint16_t>(TruncToInt(sz));
    return ret;
}

} // namespace

TransformState ComputeTransformState(const GeState &gs, const VertexFormat &fmt) {
    TransformState state;
    state.enable_transform = !fmt.through;
    state.enable_lighting = gs.IsLightingEnabled();
    state.enable_fog = gs.IsFogEnabled();
    state.read_uv = !gs.IsModeClear() && gs.IsTextureMapEnabled() && fmt.has_uv;
    state.negate_normals = gs.AreNormalsReversed();
    state.uv_gen_mode = gs.UVGenMode();
    if (state.uv_gen_mode == TexMapMode::kUnknown) state.uv_gen_mode = TexMapMode::kTextureCoords;
    state.depth_clamp = gs.IsDepthClampEnabled();

    if (state.enable_transform) {
        bool can_skip_world_pos = true;
        if (state.enable_lighting) {
            state.lighting = ComputeLightingState(gs, fmt.has_color0);
            can_skip_world_pos = !state.lighting.uses_world_pos;
            state.uses_world_normal = state.lighting.uses_world_normal;
        } else {
            state.uses_world_normal = state.uv_gen_mode == TexMapMode::kEnvironmentMap;
        }

        float world[16];
        float view[16];
        float worldview[16];
        ConvertMatrix4x3To4x4(view, gs.view_matrix.data());
        if (state.enable_fog || can_skip_world_pos) {
            ConvertMatrix4x3To4x4(world, gs.world_matrix.data());
            Matrix4ByMatrix4(worldview, world, view);
        }
        if (can_skip_world_pos) {
            state.world_to_clip = false;
            Matrix4ByMatrix4(state.matrix, worldview, gs.proj_matrix.data());
        } else {
            state.world_to_clip = true;
            Matrix4ByMatrix4(state.matrix, view, gs.proj_matrix.data());
        }

        if (state.enable_fog) {
            const float fog_end = gs.Fog1();
            float fog_slope = gs.Fog2();
            // Fog end and slope are baked into the dot product.
            state.pos_to_fog =
                Vec4f{worldview[2], worldview[6], worldview[10], worldview[14] + fog_end};
            if (IsNanOrInf(fog_end)) {
                bool sign = std::signbit(fog_end);
                if (std::signbit(fog_slope)) sign = !sign;
                if (fog_slope == 0.0f) sign = true;
                const float forced = sign ? 0.0f : 1.0f;
                state.pos_to_fog = Vec4f{0.0f, 0.0f, 0.0f, forced};
            } else {
                if (IsNanOrInf(fog_slope))
                    fog_slope = std::signbit(fog_slope) ? -262144.0f : 262144.0f;
                state.pos_to_fog.x *= fog_slope;
                state.pos_to_fog.y *= fog_slope;
                state.pos_to_fog.z *= fog_slope;
                state.pos_to_fog.w *= fog_slope;
            }
        }

        state.screen_scale = Vec3f{gs.ViewportXScale(), gs.ViewportYScale(), gs.ViewportZScale()};
        state.screen_add = Vec3f{gs.ViewportXCenter(), gs.ViewportYCenter(), gs.ViewportZCenter()};
    }
    return state;
}

ClipVertexData TransformVertex(const GeState &gs, const TransformState &ts, const VertexFormat &fmt,
                               const DecodedVertex &dv, VertexHistory *history) {
    ClipVertexData vertex;
    const Vec3f pos{dv.pos[0], dv.pos[1], dv.pos[2]};

    if (ts.read_uv) {
        vertex.v.s = dv.uv[0];
        vertex.v.t = dv.uv[1];
        vertex.v.q = 0.0f;
        history->s = vertex.v.s;
        history->t = vertex.v.t;
        history->q = vertex.v.q;
    } else {
        vertex.v.s = history->s;
        vertex.v.t = history->t;
        vertex.v.q = history->q;
    }

    if (fmt.has_normal) history->normal = Vec3f{dv.normal[0], dv.normal[1], dv.normal[2]};
    Vec3f normal = history->normal;
    if (ts.negate_normals) normal = Vec3f{-normal.x, -normal.y, -normal.z};

    vertex.v.color0 = fmt.has_color0 ? dv.color0 : gs.MaterialAmbientRGBA();
    vertex.v.color1 = 0;

    if (!ts.enable_transform) {
        vertex.v.screenpos.x = TruncToInt(pos.x * static_cast<float>(kScreenScale));
        vertex.v.screenpos.y = TruncToInt(pos.y * static_cast<float>(kScreenScale));
        vertex.v.screenpos.z = static_cast<std::uint16_t>(TruncToInt(pos.z));
        vertex.v.clipw = 1.0f;
        vertex.v.fogdepth = 1.0f;
        return vertex;
    }

    Vec3f worldpos;
    Vec4f clip;
    if (ts.world_to_clip) {
        worldpos = Vec3ByMatrix43(pos, gs.world_matrix.data());
        clip = Vec3ByMatrix44(worldpos, ts.matrix);
    } else {
        clip = Vec3ByMatrix44(pos, ts.matrix);
    }
    vertex.clippos = ClipCoords{clip.x, clip.y, clip.z, clip.w};

    // (clippos * screenScale) / w + screenAdd, lane by lane.
    const float sx = clip.x * ts.screen_scale.x / clip.w + ts.screen_add.x;
    const float sy = clip.y * ts.screen_scale.y / clip.w + ts.screen_add.y;
    const float sz = clip.z * ts.screen_scale.z / clip.w + ts.screen_add.z;
    bool outside = false;
    vertex.v.screenpos =
        RoundToScreen(gs, ts.depth_clamp, false, sx, sy, sz, vertex.clippos, &outside);
    if (outside) {
        vertex.v.screenpos.x = kOutsideRange;
        return vertex;
    }

    vertex.v.fogdepth = ts.enable_fog ? Dot43(ts.pos_to_fog, pos) : 1.0f;
    vertex.v.clipw = clip.w;

    Vec3f worldnormal;
    if (ts.uses_world_normal) {
        worldnormal = Norm3ByMatrix43(normal, gs.world_matrix.data());
        NormalizeOr001(&worldnormal);
    }

    if (ts.uv_gen_mode == TexMapMode::kTextureMatrix) {
        Vec3f source;
        switch (gs.UVProjMode()) {
        case TexProjMapMode::kPosition:
            source = pos;
            break;
        case TexProjMapMode::kUv:
            source = Vec3f{vertex.v.s, vertex.v.t, 0.0f};
            break;
        case TexProjMapMode::kNormalizedNormal:
            // Does not use 0, 0, 1 if the length is zero.
            source = NormalizedApprox(normal);
            break;
        case TexProjMapMode::kNormal:
            source = normal;
            break;
        }
        // UV scale/offset are not used in this mode.
        const Vec3f stq = Vec3ByMatrix43(source, gs.tgen_matrix.data());
        vertex.v.s = stq.x;
        vertex.v.t = stq.y;
        vertex.v.q = stq.z;
    } else if (ts.uv_gen_mode == TexMapMode::kEnvironmentMap) {
        GenerateLightST(gs, vertex.v, worldnormal);
    }

    if (ts.enable_lighting) ProcessLighting(gs, vertex.v, worldpos, worldnormal, ts.lighting);
    return vertex;
}

ScreenCoords ClipToScreen(const GeState &gs, const ClipCoords &coords, bool *outside_range) {
    const float sx = coords.x * gs.ViewportXScale() / coords.w + gs.ViewportXCenter();
    const float sy = coords.y * gs.ViewportYScale() / coords.w + gs.ViewportYCenter();
    const float sz = coords.z * gs.ViewportZScale() / coords.w + gs.ViewportZCenter();
    return RoundToScreen(gs, gs.IsDepthClampEnabled(), true, sx, sy, sz, coords, outside_range);
}

} // namespace mpo::ge
