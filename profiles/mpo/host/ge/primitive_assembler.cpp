// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/TransformUnit.cpp, GPU/Software/Clipper.cpp
// and GPU/Software/BinManager.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "primitive_assembler.hpp"

#include <algorithm>
#include <utility>

namespace mpo::ge {
namespace {

constexpr int kCullCW = 0;
constexpr int kCullCCW = 1;
constexpr int kCullOff = 2;

constexpr int kClipNegZBit = 0x20;

int Wrap32(std::int64_t v) {
    return static_cast<int>(static_cast<std::uint32_t>(v));
}

// Clipper CalcClipMask: -1 (every bit) when z/w < -1.
int CalcClipMask(const ClipCoords &v) {
    if (v.z < -v.w) return -1;
    return 0;
}

bool DifferentSigns(float x, float y) {
    return (x <= 0 && y > 0) || (x > 0 && y <= 0);
}

// clip_dotprod with the -Z plane (A, B, C, D) = (0, 0, 1, 1), evaluated as
// PPSSPP does (left to right, the zero terms included).
float ClipDotNegZ(const ClipVertexData &v) {
    return v.clippos.x * 0.0f + v.clippos.y * 0.0f + v.clippos.z * 1.0f + v.clippos.w * 1.0f;
}

bool CheckOutsideZ(const ClipCoords &p, int *pos, int *neg) {
    constexpr float kOutsideValue = 1.000030517578125f;
    const float z = p.z / p.w;
    if (z >= kOutsideValue) {
        ++*pos;
        return true;
    }
    if (-z >= kOutsideValue) {
        ++*neg;
        return true;
    }
    return false;
}

float Lerp(float a, float b, float t) {
    return a * (1.f - t) + b * t;
}

} // namespace

DecodeParams ComputeDecodeParams(const GeState &gs, std::uint32_t vtype) {
    DecodeParams params;
    const bool through = (vtype & kVtypeThrough) != 0;
    const TexMapMode mode = gs.UVGenMode();
    params.prescale_uv =
        !through && (mode == TexMapMode::kTextureCoords || mode == TexMapMode::kUnknown);
    params.uv_scale = UVScale{gs.TexScaleU(), gs.TexScaleV(), gs.TexOffsetU(), gs.TexOffsetV()};
    if (!through && (vtype & kVtypeWeightMask) != 0) params.bones = gs.bone_matrix.data();
    return params;
}

bool MemoryVertexSource::Read(int i, DecodedVertex *out) {
    std::uint32_t index = static_cast<std::uint32_t>(i);
    if (dec_.index_size() != 0) {
        if (!ReadIndex(mem_, index_addr_, dec_.vtype(), i, &index)) return false;
    }
    return dec_.Decode(mem_, vertex_addr_ + index * dec_.stride(), params_, out);
}

bool ArrayVertexSource::Read(int i, DecodedVertex *out) {
    if (i < 0 || static_cast<std::size_t>(i) >= indices_.size()) return false;
    const std::uint16_t index = indices_[static_cast<std::size_t>(i)];
    if (index >= vertices_.size()) return false;
    *out = vertices_[index];
    return true;
}

BinCoords PrimitiveAssembler::Scissor(BinCoords range) const {
    BinCoords out;
    out.x1 = std::max(range.x1, scissor_.x1);
    out.y1 = std::max(range.y1, scissor_.y1);
    out.x2 = std::min(range.x2, scissor_.x2);
    out.y2 = std::min(range.y2, scissor_.y2);
    return out;
}

BinCoords PrimitiveAssembler::Range(const VertexData &v0, const VertexData &v1) const {
    BinCoords range;
    range.x1 = std::min(v0.screenpos.x, v1.screenpos.x) & ~(kScreenScale - 1);
    range.y1 = std::min(v0.screenpos.y, v1.screenpos.y) & ~(kScreenScale - 1);
    range.x2 = std::max(v0.screenpos.x, v1.screenpos.x) | (kScreenScale - 1);
    range.y2 = std::max(v0.screenpos.y, v1.screenpos.y) | (kScreenScale - 1);
    return Scissor(range);
}

BinCoords PrimitiveAssembler::Range(const VertexData &v0, const VertexData &v1,
                                    const VertexData &v2) const {
    BinCoords range;
    range.x1 = std::min({v0.screenpos.x, v1.screenpos.x, v2.screenpos.x}) & ~(kScreenScale - 1);
    range.y1 = std::min({v0.screenpos.y, v1.screenpos.y, v2.screenpos.y}) & ~(kScreenScale - 1);
    range.x2 = std::max({v0.screenpos.x, v1.screenpos.x, v2.screenpos.x}) | (kScreenScale - 1);
    range.y2 = std::max({v0.screenpos.y, v1.screenpos.y, v2.screenpos.y}) | (kScreenScale - 1);
    return Scissor(range);
}

ClipVertexData PrimitiveAssembler::ReadVertex(int i) {
    DecodedVertex dv;
    if (!source_->Read(i, &dv)) {
        stats_.Unsupported("vertex or index read outside memory");
        dv = DecodedVertex{};
    }
    return TransformVertex(*gs_, transform_, format_, dv, &history_);
}

// BinManager::AddTriangle: drops clockwise and degenerate triangles. The
// cross product is computed in 32-bit ints, which wrap on x86-64.
void PrimitiveAssembler::AddTriangle(const VertexData &a, const VertexData &b,
                                     const VertexData &c) {
    const int d01x = Wrap32(static_cast<std::int64_t>(a.screenpos.x) - b.screenpos.x);
    const int d01y = Wrap32(static_cast<std::int64_t>(a.screenpos.y) - b.screenpos.y);
    const int d02x = Wrap32(static_cast<std::int64_t>(a.screenpos.x) - c.screenpos.x);
    const int d02y = Wrap32(static_cast<std::int64_t>(a.screenpos.y) - c.screenpos.y);
    const int cross =
        Wrap32(static_cast<std::int64_t>(Wrap32(static_cast<std::int64_t>(d01x) * d02y)) -
               Wrap32(static_cast<std::int64_t>(d01y) * d02x));
    if (cross < 0) {
        ++stats_.culled;
        return;
    }
    if ((d01x == 0 && d02x == 0) || (d01y == 0 && d02y == 0)) return;
    const BinCoords range = Range(a, b, c);
    if (range.Invalid()) return;
    ++stats_.triangles;
    raster_.DrawTriangle(a, b, c, range, *state_);
}

void PrimitiveAssembler::AddRect(const VertexData &v0, const VertexData &v1) {
    const BinCoords range = Range(v0, v1);
    if (range.Invalid()) return;
    ++stats_.rectangles;
    raster_.DrawRectangle(v0, v1, range, *state_);
}

void PrimitiveAssembler::AddLine(const VertexData &v0, const VertexData &v1) {
    const BinCoords range = Range(v0, v1);
    if (range.Invalid()) return;
    ++stats_.lines;
    raster_.DrawLine(v0, v1, range, *state_);
}

void PrimitiveAssembler::SendTriangle(int cull, const ClipVertexData *verts, int provoking) {
    if (cull == kCullOff) {
        ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking]);
        ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking]);
    } else if (cull == kCullCW) {
        ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking]);
    } else {
        ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking]);
    }
}

// ClipVertexData::Lerp + clip_interpolate.
void PrimitiveAssembler::ClipInterpolate(ClipVertexData *dest, float t, const ClipVertexData &a,
                                         const ClipVertexData &b) const {
    ClipVertexData out;
    out.clippos.x = Lerp(a.clippos.x, b.clippos.x, t);
    out.clippos.y = Lerp(a.clippos.y, b.clippos.y, t);
    out.clippos.z = Lerp(a.clippos.z, b.clippos.z, t);
    out.clippos.w = Lerp(a.clippos.w, b.clippos.w, t);
    out.v.s = Lerp(a.v.s, b.v.s, t);
    out.v.t = Lerp(a.v.t, b.v.t, t);
    out.v.q = Lerp(a.v.q, b.v.q, t);
    out.v.fogdepth = Lerp(a.v.fogdepth, b.v.fogdepth, t);
    // (u16)(t * 256), then LerpInt<..., 256>.
    const int t_int = static_cast<std::uint16_t>(TruncToInt(t * 256));
    const Vec4i a0 = Vec4i::FromRGBA(a.v.color0);
    const Vec4i b0 = Vec4i::FromRGBA(b.v.color0);
    const Vec4i a1 = Vec4i::FromRGBA(a.v.color1 & 0xFFFFFF);
    const Vec4i b1 = Vec4i::FromRGBA(b.v.color1 & 0xFFFFFF);
    Vec4i c0;
    Vec4i c1;
    for (int i = 0; i < 4; ++i) c0[i] = (a0[i] * (256 - t_int) + b0[i] * t_int) / 256;
    for (int i = 0; i < 3; ++i) c1[i] = (a1[i] * (256 - t_int) + b1[i] * t_int) / 256;
    out.v.color0 = c0.ToRGBA();
    out.v.color1 = c1.ToRGB();

    bool outside = false;
    out.v.screenpos = ClipToScreen(*gs_, out.clippos, &outside);
    out.v.clipw = out.clippos.w;
    if (outside) out.v.screenpos.x = kOutsideRange;
    *dest = out;
}

void PrimitiveAssembler::ProcessTriangle(const ClipVertexData &v0, const ClipVertexData &v1,
                                         const ClipVertexData &v2,
                                         const ClipVertexData &provoking) {
    int mask = 0;
    if (!state_->through_mode) {
        // If any verts were outside range, throw the entire prim away.
        if (v0.OutsideRange() || v1.OutsideRange() || v2.OutsideRange()) return;
        // If all verts have negative W, we also cull.
        if (v0.clippos.w < 0.0f && v1.clippos.w < 0.0f && v2.clippos.w < 0.0f) return;

        mask |= CalcClipMask(v0.clippos);
        mask |= CalcClipMask(v1.clippos);
        mask |= CalcClipMask(v2.clippos);

        int outside_pos = 0;
        int outside_neg = 0;
        CheckOutsideZ(v0.clippos, &outside_pos, &outside_neg);
        CheckOutsideZ(v1.clippos, &outside_pos, &outside_neg);
        CheckOutsideZ(v2.clippos, &outside_pos, &outside_neg);
        // With depth clamp off, discard the triangle if even one vert is
        // outside; with it on, all three must be outside on the same side.
        if (outside_pos + outside_neg > 0 && !gs_->IsDepthClampEnabled()) return;
        if (outside_pos >= 3 || outside_neg >= 3) return;
    }

    if ((mask & kClipNegZBit) == 0) {
        if (!gs_->IsShadeGouraud()) {
            VertexData corrected2 = v2.v;
            corrected2.color0 = provoking.v.color0;
            corrected2.color1 = provoking.v.color1;
            AddTriangle(v0.v, v1.v, corrected2);
        } else {
            AddTriangle(v0.v, v1.v, v2.v);
        }
        return;
    }

    ++stats_.clipped_triangles;
    constexpr int kNumIndices = 6;
    constexpr int kSkip = -1;
    ClipVertexData clipped[kNumIndices];
    clipped[0] = v0;
    clipped[1] = v1;
    clipped[2] = v2;
    int indices[kNumIndices] = {kSkip, kSkip, kSkip, kSkip, kSkip, kSkip};
    int num_indices = 3;

    int vlist[2][2 * 6 + 1] = {};
    int *inlist = vlist[0];
    int *outlist = vlist[1];
    int n = 3;
    int num_vertices = 3;
    inlist[0] = 0;
    inlist[1] = 1;
    inlist[2] = 2;

    // CLIP_POLY(CLIP_NEG_Z_BIT, 0, 0, 1, 1): the PSP only clips on -Z.
    bool keep = true;
    {
        int idx_prev = inlist[0];
        float dp_prev = ClipDotNegZ(clipped[idx_prev]);
        int outcount = 0;
        inlist[n] = inlist[0];
        for (int j = 1; j <= n; ++j) {
            const int idx = inlist[j];
            const float dp = ClipDotNegZ(clipped[idx]);
            if (dp_prev >= 0) outlist[outcount++] = idx_prev;
            // Skipping w sign mismatches avoids inversions (PPSSPP #16131).
            if (DifferentSigns(dp, dp_prev) && num_vertices < kNumIndices) {
                ClipVertexData *vert = &clipped[num_vertices++];
                if (dp < 0) {
                    const float t = dp / (dp - dp_prev);
                    ClipInterpolate(vert, t, clipped[idx], clipped[idx_prev]);
                } else {
                    const float t = dp_prev / (dp_prev - dp);
                    ClipInterpolate(vert, t, clipped[idx_prev], clipped[idx]);
                }
                outlist[outcount++] = num_vertices - 1;
            }
            idx_prev = idx;
            dp_prev = dp;
        }
        if (outcount < 3) {
            keep = false;
        } else {
            std::swap(inlist, outlist);
            n = outcount;
        }
    }

    if (keep) {
        indices[0] = inlist[0];
        indices[1] = inlist[1];
        indices[2] = inlist[2];
        for (int j = 3; j < n && num_indices + 3 <= kNumIndices; ++j) {
            indices[num_indices++] = inlist[0];
            indices[num_indices++] = inlist[j - 1];
            indices[num_indices++] = inlist[j];
        }
    }

    for (int i = 0; i + 3 <= num_indices; i += 3) {
        if (indices[i] == kSkip) continue;
        ClipVertexData &sub0 = clipped[indices[i + 0]];
        ClipVertexData &sub1 = clipped[indices[i + 1]];
        ClipVertexData &sub2 = clipped[indices[i + 2]];
        if (sub0.OutsideRange() || sub1.OutsideRange() || sub2.OutsideRange()) continue;
        if (!gs_->IsShadeGouraud()) {
            sub2.v.color0 = provoking.v.color0;
            sub2.v.color1 = provoking.v.color1;
        }
        AddTriangle(sub0.v, sub1.v, sub2.v);
    }
}

// Clipper RotateUV + AddTriangleRect: rectangles with texture projection are
// drawn as triangles.
void PrimitiveAssembler::AddTriangleRect(const VertexData &v0, const VertexData &v1) {
    VertexData buf[4];
    buf[0] = v1;
    buf[0].screenpos = ScreenCoords{v0.screenpos.x, v0.screenpos.y, v1.screenpos.z};
    buf[0].s = v0.s;
    buf[0].t = v0.t;
    buf[0].q = v0.q;

    buf[1] = v1;
    buf[1].screenpos = ScreenCoords{v0.screenpos.x, v1.screenpos.y, v1.screenpos.z};
    buf[1].s = v0.s;
    buf[1].t = v1.t;
    buf[1].q = v0.q;

    buf[2] = v1;
    buf[2].screenpos = ScreenCoords{v1.screenpos.x, v0.screenpos.y, v1.screenpos.z};
    buf[2].s = v1.s;
    buf[2].t = v0.t;
    buf[2].q = v1.q;

    buf[3] = v1;

    VertexData *topleft = &buf[0];
    VertexData *topright = &buf[1];
    VertexData *bottomleft = &buf[2];
    VertexData *bottomright = &buf[3];
    for (auto &b : buf) {
        if (b.screenpos.x < topleft->screenpos.x && b.screenpos.y < topleft->screenpos.y)
            topleft = &b;
        if (b.screenpos.x > topright->screenpos.x && b.screenpos.y < topright->screenpos.y)
            topright = &b;
        if (b.screenpos.x < bottomleft->screenpos.x && b.screenpos.y > bottomleft->screenpos.y)
            bottomleft = &b;
        if (b.screenpos.x > bottomright->screenpos.x && b.screenpos.y > bottomright->screenpos.y)
            bottomright = &b;
    }

    // RotateUV(v0, v1, *topright, *bottomleft).
    const int x1 = v0.screenpos.x;
    const int x2 = v1.screenpos.x;
    const int y1 = v0.screenpos.y;
    const int y2 = v1.screenpos.y;
    if ((x1 < x2 && y1 > y2) || (x1 > x2 && y1 < y2)) {
        std::swap(bottomleft->s, topright->s);
        std::swap(bottomleft->t, topright->t);
        std::swap(bottomleft->q, topright->q);
    }

    AddTriangle(*topleft, *topright, *bottomleft);
    AddTriangle(*bottomleft, *topright, *topleft);
    AddTriangle(*topright, *bottomright, *bottomleft);
    AddTriangle(*bottomleft, *bottomright, *topright);
}

void PrimitiveAssembler::ProcessRect(const ClipVertexData &v0, const ClipVertexData &v1) {
    if (!state_->through_mode) {
        if (v0.OutsideRange() || v1.OutsideRange()) return;
        int outside_pos = 0;
        int outside_neg = 0;
        CheckOutsideZ(v0.clippos, &outside_pos, &outside_neg);
        CheckOutsideZ(v1.clippos, &outside_pos, &outside_neg);
        if (outside_pos + outside_neg > 0 && !gs_->IsDepthClampEnabled()) return;
        if (outside_pos >= 2 || outside_neg >= 2) return;

        bool split_fog = v0.v.fogdepth != v1.v.fogdepth;
        if (split_fog) {
            // Within the same 1/255 the fog can be considered flat.
            constexpr float kFogHalfStep = 0.5f / 255.0f;
            if (v1.v.fogdepth - kFogHalfStep <= v0.v.fogdepth &&
                v1.v.fogdepth + kFogHalfStep >= v0.v.fogdepth) {
                split_fog = false;
            }
        }
        if (split_fog) {
            // Rectangles use nearest along X for fog depth, but reversed.
            VertexData vhalf0 = v1.v;
            vhalf0.screenpos.x = v0.v.screenpos.x + (v1.v.screenpos.x - v0.v.screenpos.x) / 2;
            vhalf0.s = v0.v.s + (v1.v.s - v0.v.s) / 2;

            VertexData vhalf1 = v1.v;
            vhalf1.screenpos.x = v0.v.screenpos.x + (v1.v.screenpos.x - v0.v.screenpos.x) / 2;
            vhalf1.screenpos.y = v0.v.screenpos.y;
            vhalf1.s = v0.v.s + (v1.v.s - v0.v.s) / 2;
            vhalf1.t = v0.v.t;

            VertexData vrev1 = v1.v;
            vrev1.fogdepth = v0.v.fogdepth;

            if (state_->texture_proj) {
                AddTriangleRect(v0.v, vhalf0);
                AddTriangleRect(vhalf1, vrev1);
            } else {
                AddRect(v0.v, vhalf0);
                AddRect(vhalf1, vrev1);
            }
        } else if (state_->texture_proj) {
            AddTriangleRect(v0.v, v1.v);
        } else {
            AddRect(v0.v, v1.v);
        }
        return;
    }

    // Through mode.
    const BinCoords range = Range(v0.v, v1.v);
    if (RectangleCanUseSprite(v0.v, v1.v, *state_)) {
        if (range.Invalid()) return;
        ++stats_.sprites;
        raster_.DrawSprite(v0.v, v1.v, range, *state_);
    } else if (gs_->IsModeClear() && !gs_->IsDitherEnabled()) {
        if (range.Invalid()) return;
        ++stats_.clear_rects;
        raster_.ClearRectangle(v0.v, v1.v, range, *state_);
    } else {
        AddRect(v0.v, v1.v);
    }
}

void PrimitiveAssembler::ProcessPoint(const ClipVertexData &v0) {
    if (!state_->through_mode && v0.OutsideRange()) return;
    BinCoords range;
    range.x1 = v0.v.screenpos.x & ~(kScreenScale - 1);
    range.y1 = v0.v.screenpos.y & ~(kScreenScale - 1);
    range.x2 = v0.v.screenpos.x | (kScreenScale - 1);
    range.y2 = v0.v.screenpos.y | (kScreenScale - 1);
    range = Scissor(range);
    if (range.Invalid()) return;
    ++stats_.points;
    raster_.DrawPoint(v0.v, range, *state_);
}

void PrimitiveAssembler::ProcessLine(const ClipVertexData &v0, const ClipVertexData &v1) {
    if (state_->through_mode) {
        AddLine(v0.v, v1.v);
        return;
    }
    if (v0.OutsideRange() || v1.OutsideRange()) return;

    int outside_pos = 0;
    int outside_neg = 0;
    CheckOutsideZ(v0.clippos, &outside_pos, &outside_neg);
    CheckOutsideZ(v1.clippos, &outside_pos, &outside_neg);
    if (outside_pos + outside_neg > 0 && !gs_->IsDepthClampEnabled()) return;
    if (outside_pos >= 2 || outside_neg >= 2) return;

    const int mask0 = CalcClipMask(v0.clippos);
    const int mask1 = CalcClipMask(v1.clippos);
    if (((mask0 | mask1) & kClipNegZBit) == 0) {
        AddLine(v0.v, v1.v);
        return;
    }

    // CLIP_LINE(CLIP_NEG_Z_BIT, 0, 0, 1, 1).
    ClipVertexData clipped[2] = {v0, v1};
    float dp0 = ClipDotNegZ(clipped[0]);
    const float dp1 = ClipDotNegZ(clipped[1]);
    if ((mask0 & kClipNegZBit) && dp0 < 0) {
        const float t = dp1 / (dp1 - dp0);
        ClipInterpolate(&clipped[0], t, clipped[1], clipped[0]);
    }
    dp0 = ClipDotNegZ(clipped[0]);
    if ((mask1 & kClipNegZBit) && dp1 < 0) {
        const float t = dp1 / (dp1 - dp0);
        ClipInterpolate(&clipped[1], t, clipped[1], clipped[0]);
    }
    if (!clipped[0].OutsideRange() && !clipped[1].OutsideRange()) {
        AddLine(clipped[0].v, clipped[1].v);
    }
}

std::uint32_t PrimitiveAssembler::Submit(const GeState &gs, const RasterizerState &state,
                                         PrimType prim_type, std::uint32_t vertex_addr,
                                         std::uint32_t index_addr, int vertex_count) {
    const std::uint32_t vtype = gs.VertType();
    const VertexDecoder dec(vtype);
    const std::uint32_t bytes_read = static_cast<std::uint32_t>(vertex_count) * dec.stride();
    MemoryVertexSource source(mem_, dec, ComputeDecodeParams(gs, vtype), vertex_addr, index_addr);
    SubmitVertices(gs, state, prim_type, source, vtype, vertex_count);
    return bytes_read;
}

void PrimitiveAssembler::SubmitVertices(const GeState &gs, const RasterizerState &state,
                                        PrimType prim_type, VertexSource &source,
                                        std::uint32_t vtype, int vertex_count) {
    // Vertices without position are entirely culled.
    if ((vtype & kVtypePosMask) == 0) return;

    gs_ = &gs;
    state_ = &state;
    source_ = &source;
    format_.through = (vtype & kVtypeThrough) != 0;
    format_.has_uv = (vtype & kVtypeTcMask) != 0;
    format_.has_color0 = (vtype & kVtypeColMask) != 0;
    format_.has_normal = (vtype & kVtypeNrmMask) != 0;
    transform_ = ComputeTransformState(gs, format_);

    // BinManager scissor: the scissor box, limited by the drawing region.
    scissor_.x1 = gs.ScissorX1() * kScreenScale;
    scissor_.y1 = gs.ScissorY1() * kScreenScale;
    scissor_.x2 = std::min(gs.ScissorX2(), gs.RegionX2()) * kScreenScale + kScreenScale - 1;
    scissor_.y2 = std::min(gs.ScissorY2(), gs.RegionY2()) * kScreenScale + kScreenScale - 1;

    if (prim_type != PrimType::kKeepPrevious) {
        data_index_ = 0;
        prev_prim_ = prim_type;
    } else {
        prim_type = prev_prim_;
    }
    stats_.vertices += static_cast<std::uint64_t>(vertex_count);

    const bool through = format_.through;
    const bool skip_cull = !gs.IsCullEnabled() || gs.IsModeClear();
    const int cull = skip_cull ? kCullOff : (gs.CullMode() ? kCullCCW : kCullCW);

    if (through && cull == kCullOff && prim_type == PrimType::kTriangles && data_index_ == 0 &&
        vertex_count >= 6 && (vertex_count % 6) == 0) {
        // Rectangles sent as pairs of triangles (through mode only).
        ClipVertexData buf[6];
        int buf_index = 0;
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            buf[buf_index++] = ReadVertex(vtx);
            if (buf_index < 6) continue;
            int tl = -1;
            int br = -1;
            if (DetectRectangleFromPair(state, buf, &tl, &br)) {
                ProcessRect(buf[tl], buf[br]);
            } else {
                SendTriangle(cull, &buf[0], 2);
                SendTriangle(cull, &buf[3], 2);
            }
            buf_index = 0;
        }
        data_index_ = 0;
        return;
    }

    switch (prim_type) {
    case PrimType::kPoints:
        for (int i = 0; i < data_index_; ++i) ProcessPoint(data_[i]);
        data_index_ = 0;
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            data_[0] = ReadVertex(vtx);
            ProcessPoint(data_[0]);
        }
        break;

    case PrimType::kLines:
        for (int i = 0; i < data_index_ - 1; i += 2) ProcessLine(data_[i], data_[i + 1]);
        data_index_ &= 1;
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            data_[data_index_++] = ReadVertex(vtx);
            if (data_index_ == 2) {
                ProcessLine(data_[0], data_[1]);
                data_index_ = 0;
            }
        }
        break;

    case PrimType::kTriangles:
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            data_[data_index_++] = ReadVertex(vtx);
            if (data_index_ < 3) continue;
            data_index_ = 0;
            SendTriangle(cull, &data_[0], 2);
        }
        if (data_index_ >= 3) {
            SendTriangle(cull, &data_[0], 2);
            data_index_ = 0;
        }
        break;

    case PrimType::kRectangles:
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            data_[data_index_++] = ReadVertex(vtx);
            if (data_index_ == 4 && through && cull == kCullOff) {
                if (DetectRectangleThroughModeSlices(state, data_)) {
                    data_[1] = data_[3];
                    data_index_ = 2;
                }
            }
            if (data_index_ == 4) {
                ProcessRect(data_[0], data_[1]);
                ProcessRect(data_[2], data_[3]);
                data_index_ = 0;
            }
        }
        if (data_index_ >= 2) {
            ProcessRect(data_[0], data_[1]);
            data_index_ -= 2;
        }
        break;

    case PrimType::kLineStrip: {
        int skip_count = data_index_ == 0 ? 1 : 0;
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            data_[(data_index_++) & 1] = ReadVertex(vtx);
            if (skip_count) {
                --skip_count;
            } else {
                ProcessLine(data_[data_index_ & 1], data_[(data_index_ & 1) ^ 1]);
            }
        }
        break;
    }

    case PrimType::kTriangleStrip: {
        int skip_count = data_index_ >= 2 ? 0 : 2 - data_index_;
        int start_vtx = 0;
        if (data_index_ == 0 && vertex_count >= 4 && (vertex_count & 1) == 0 && cull == kCullOff) {
            for (int base = 0; base < vertex_count - 2; base += 2) {
                for (int vtx = base == 0 ? 0 : 2; vtx < 4; ++vtx)
                    data_[vtx] = ReadVertex(base + vtx);
                int tl = -1;
                int br = -1;
                if (DetectRectangleFromStrip(state, data_, &tl, &br)) {
                    ProcessRect(data_[tl], data_[br]);
                    start_vtx += 2;
                    skip_count = 2;
                    if (base + 4 >= vertex_count) {
                        start_vtx = vertex_count;
                        break;
                    }
                    data_[0] = data_[2];
                    data_[1] = data_[3];
                    data_index_ = 2;
                } else {
                    break;
                }
            }
        }
        for (int vtx = start_vtx; vtx < vertex_count && skip_count > 0; ++vtx) {
            const int provoking = (data_index_++) % 3;
            data_[provoking] = ReadVertex(vtx);
            --skip_count;
            ++start_vtx;
        }
        for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
            const int provoking = (data_index_++) % 3;
            data_[provoking] = ReadVertex(vtx);
            const int wind = (data_index_ - 1) % 2;
            const int alt_cull = cull == kCullOff ? cull : (cull ^ wind);
            SendTriangle(alt_cull, &data_[0], provoking);
        }
        break;
    }

    case PrimType::kTriangleFan: {
        int skip_count = data_index_ <= 1 ? 1 : 0;
        int start_vtx = 0;
        if (data_index_ == 0 && vertex_count > 0) {
            data_[0] = ReadVertex(0);
            data_index_++;
            start_vtx = 1;
        }
        if (data_index_ == 1 && vertex_count == 4 && cull == kCullOff) {
            for (int vtx = start_vtx; vtx < vertex_count; ++vtx) data_[vtx] = ReadVertex(vtx);
            int tl = -1;
            int br = -1;
            if (DetectRectangleFromFan(state, data_, &tl, &br)) {
                ProcessRect(data_[tl], data_[br]);
                break;
            }
        }
        for (int vtx = start_vtx; vtx < vertex_count && skip_count > 0; ++vtx) {
            const int provoking = 2 - ((data_index_++) % 2);
            data_[provoking] = ReadVertex(vtx);
            --skip_count;
            ++start_vtx;
        }
        for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
            const int provoking = 2 - ((data_index_++) % 2);
            data_[provoking] = ReadVertex(vtx);
            const int wind = (data_index_ - 1) % 2;
            const int alt_cull = cull == kCullOff ? cull : (cull ^ wind);
            SendTriangle(alt_cull, &data_[0], provoking);
        }
        break;
    }

    default:
        stats_.Unsupported("invalid primitive type");
        break;
    }
}

} // namespace mpo::ge
