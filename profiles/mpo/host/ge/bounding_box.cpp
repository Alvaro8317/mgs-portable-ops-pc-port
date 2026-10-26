// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/GPUCommon.cpp and GPU/Common/DrawEngineCommon.cpp
// @ v1.20.4 (GPL-2.0-or-later).

#include "bounding_box.hpp"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <vector>

#include "ge_constants.hpp"
#include "math3d.hpp"
#include "vertex_decoder.hpp"

namespace mpo::ge {
namespace {

struct Planes {
    float x[6] = {};
    float y[6] = {};
    float z[6] = {};
    float w[6] = {};
    float min_offset_x = 0;
    float min_offset_y = 0;
    float max_offset_x = 0;
    float max_offset_y = 0;

    void Set(int i, float a, float b, float c, float d) {
        x[i] = a;
        y[i] = b;
        z[i] = c;
        w[i] = d;
    }
    [[nodiscard]] float Test(int i, const Vec3f &f) const {
        return x[i] * f.x + y[i] * f.y + z[i] * f.z + w[i];
    }
};

Planes UpdatePlanes(const GeState &gs) {
    Planes planes;
    float view[16];
    float viewproj[16];
    ConvertMatrix4x3To4x4(view, gs.view_matrix.data());
    Matrix4ByMatrix4(viewproj, view, gs.proj_matrix.data());

    // Viewport, scissor, region and offset, for X/Y only. The PSP does not
    // clip against the viewport. Region1 (rate) acts as X1/Y1.
    const float base_x = gs.OffsetX();
    const float base_y = gs.OffsetY();
    planes.min_offset_x =
        (base_x + static_cast<float>(std::max(gs.RegionRateX() - 0x100, gs.ScissorX1()))) - 1.0f;
    planes.min_offset_y =
        (base_y + static_cast<float>(std::max(gs.RegionRateY() - 0x100, gs.ScissorY1()))) - 1.0f;
    planes.max_offset_x =
        (base_x + static_cast<float>(std::min(gs.RegionX2(), gs.ScissorX2()))) + 1.0f;
    planes.max_offset_y =
        (base_y + static_cast<float>(std::min(gs.RegionY2(), gs.ScissorY2()))) + 1.0f;

    const float inv_scale_x = 1.0f / gs.ViewportXScale();
    const float inv_scale_y = 1.0f / gs.ViewportYScale();
    const float min_vp_x = (planes.min_offset_x - gs.ViewportXCenter()) * inv_scale_x;
    const float min_vp_y = (planes.min_offset_y - gs.ViewportYCenter()) * inv_scale_y;
    const float max_vp_x = (planes.max_offset_x - gs.ViewportXCenter()) * inv_scale_x;
    const float max_vp_y = (planes.max_offset_y - gs.ViewportYCenter()) * inv_scale_y;
    const float inv_size_x = 1.0f / (max_vp_x - min_vp_x);
    const float inv_size_y = 1.0f / (max_vp_y - min_vp_y);

    float apply_viewport[16] = {};
    apply_viewport[0] = 2.0f * inv_size_x;                    // xx
    apply_viewport[5] = 2.0f * inv_size_y;                    // yy
    apply_viewport[10] = 1.0f;                                // zz
    apply_viewport[15] = 1.0f;                                // ww
    apply_viewport[12] = -(max_vp_x + min_vp_x) * inv_size_x; // wx
    apply_viewport[13] = -(max_vp_y + min_vp_y) * inv_size_y; // wy

    float m[16];
    Matrix4ByMatrix4(m, viewproj, apply_viewport);
    planes.Set(0, m[3] - m[0], m[7] - m[4], m[11] - m[8], m[15] - m[12]);  // Right
    planes.Set(1, m[3] + m[0], m[7] + m[4], m[11] + m[8], m[15] + m[12]);  // Left
    planes.Set(2, m[3] + m[1], m[7] + m[5], m[11] + m[9], m[15] + m[13]);  // Bottom
    planes.Set(3, m[3] - m[1], m[7] - m[5], m[11] - m[9], m[15] - m[13]);  // Top
    planes.Set(4, m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14]); // Near
    planes.Set(5, m[3] - m[2], m[7] - m[6], m[11] - m[10], m[15] - m[14]); // Far
    return planes;
}

// The positions as TestBoundingBox reads them: raw (scaled) positions, or,
// with indices, weights or morphing, through NormalizeVertices, which skins in
// software one bone at a time.
bool ReadPositions(const GeState &gs, const MemoryView &mem, const VertexDecoder &dec,
                   std::uint32_t vdata, std::uint32_t inds, int count, std::vector<Vec3f> *out) {
    const std::uint32_t vtype = dec.vtype();
    const bool use_inds = inds != 0;
    out->resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        std::uint32_t index = static_cast<std::uint32_t>(i);
        if (use_inds && !ReadIndex(mem, inds, vtype, i, &index)) return false;
        DecodedVertex dv;
        // No skinning in decode, no UV prescale: only the position matters.
        if (!dec.Decode(mem, vdata + index * dec.stride(), DecodeParams{}, &dv)) return false;
        Vec3f pos{dv.pos[0], dv.pos[1], dv.pos[2]};
        if ((vtype & kVtypeWeightMask) != 0) {
            // NormalizeVertices, skinning loop.
            const int num_weights = static_cast<int>((vtype & kVtypeWeightCountMask) >> 14) + 1;
            Vec3f psum;
            for (int w = 0; w < num_weights; ++w) {
                if (dv.weights[w] == 0.0f) continue;
                const Vec3f bpos = Vec3ByMatrix43(pos, gs.bone_matrix.data() + w * 12);
                psum.x = psum.x + bpos.x * dv.weights[w];
                psum.y = psum.y + bpos.y * dv.weights[w];
                psum.z = psum.z + bpos.z * dv.weights[w];
            }
            pos = psum;
        }
        (*out)[static_cast<std::size_t>(i)] = pos;
    }
    return true;
}

} // namespace

BoundingBoxResult TestBoundingBox(const GeState &gs, const MemoryView &mem,
                                  std::uint32_t vertex_addr, std::uint32_t index_addr, int count) {
    const std::uint32_t vtype = gs.VertType();
    const VertexDecoder dec(vtype);
    const bool use_inds = (vtype & kVtypeIdxMask) != 0;
    const std::uint32_t bytes = (use_inds ? 1u : dec.stride()) * static_cast<std::uint32_t>(count);
    if (!mem.IsValidRange(vertex_addr, bytes)) return BoundingBoxResult::kBadMemory;
    if (use_inds) {
        const int shift = dec.index_size() == 4 ? 2 : dec.index_size() - 1;
        if (!mem.IsValidRange(index_addr, static_cast<std::uint32_t>(count) << shift)) {
            return BoundingBoxResult::kBadMemory;
        }
    }

    // The PSP only seems to vary the result based on a single range of 0x100.
    std::uint32_t vdata = vertex_addr;
    int check = count;
    if (count > 0x200) {
        // The second to last set of 0x100 is checked (even for odd counts).
        vdata += static_cast<std::uint32_t>(count - 0x200) * dec.stride();
        check = 0x100;
    } else if (count > 0x100) {
        check = count - 0x100;
    }
    if (check > 1024) return BoundingBoxResult::kVisible;

    // Without indices, weights or morphing the positions are read directly and
    // scaled as in transform mode, even in through mode; the layout does not
    // depend on the through bit.
    const bool simple = !use_inds && (vtype & (kVtypeWeightMask | kVtypeMorphCountMask)) == 0;
    const VertexDecoder simple_dec(vtype & ~kVtypeThrough);
    std::vector<Vec3f> verts;
    if (!ReadPositions(gs, mem, simple ? simple_dec : dec, vdata, use_inds ? index_addr : 0, check,
                       &verts)) {
        return BoundingBoxResult::kBadMemory;
    }
    for (Vec3f &v : verts) v = Vec3ByMatrix43(v, gs.world_matrix.data());

    const Planes planes = UpdatePlanes(gs);
    // Near and far are not checked without depth clamp.
    const int total_planes = gs.IsDepthClampEnabled() ? 6 : 4;
    for (int plane = 0; plane < total_planes; ++plane) {
        int inside = 0;
        for (const Vec3f &v : verts) {
            if (!(planes.Test(plane, v) <= -FLT_EPSILON)) ++inside;
        }
        if (inside == 0) {
            // All out, unless the offset was near the cull box edge.
            bool outside_edge = false;
            switch (plane) {
            case 0:
                outside_edge = planes.max_offset_x >= 4096.0f;
                break;
            case 1:
                outside_edge = planes.min_offset_x < 1.0f;
                break;
            case 2:
                outside_edge = planes.min_offset_y < 1.0f;
                break;
            case 3:
                outside_edge = planes.max_offset_y >= 4096.0f;
                break;
            default:
                break;
            }
            if (!outside_edge) return BoundingBoxResult::kCulled;
        }
    }
    return BoundingBoxResult::kVisible;
}

} // namespace mpo::ge
