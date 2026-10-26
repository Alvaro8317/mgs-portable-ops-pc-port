// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/TransformUnit.cpp, GPU/Software/Clipper.cpp
// and GPU/Software/BinManager.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "primitive_assembler.hpp"

#include <algorithm>

namespace mpo::ge {
namespace {

constexpr int kCullCW = 0;
constexpr int kCullCCW = 1;
constexpr int kCullOff = 2;

} // namespace

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

bool PrimitiveAssembler::ReadVertex(const GeState &gs, const RasterizerState &state,
                                    const VertexDecoder &dec, std::uint32_t vertex_addr,
                                    std::uint32_t index_addr, int i, ClipVertexData *out) {
    std::uint32_t index = static_cast<std::uint32_t>(i);
    if (dec.index_size() != 0) {
        if (!ReadIndex(mem_, index_addr, dec.vtype(), i, &index)) return false;
    }
    DecodedVertex dv;
    if (!dec.Decode(mem_, vertex_addr + index * dec.stride(), UVScale{}, false, &dv)) return false;

    *out = ClipVertexData{};
    const bool read_uv = !gs.IsModeClear() && gs.IsTextureMapEnabled() && dec.has_uv();
    if (read_uv) {
        out->v.s = dv.uv[0];
        out->v.t = dv.uv[1];
        out->v.q = 0.0f;
        last_s_ = out->v.s;
        last_t_ = out->v.t;
    } else {
        out->v.s = last_s_;
        out->v.t = last_t_;
    }
    out->v.color0 = dec.has_color() ? dv.color0 : gs.MaterialAmbientRGBA();
    out->v.color1 = 0;

    // Through mode: positions are already screen coordinates.
    out->v.screenpos.x = TruncToInt(dv.pos[0] * static_cast<float>(kScreenScale));
    out->v.screenpos.y = TruncToInt(dv.pos[1] * static_cast<float>(kScreenScale));
    out->v.screenpos.z = static_cast<int>(static_cast<std::uint16_t>(TruncToInt(dv.pos[2])));
    out->v.clipw = 1.0f;
    out->v.fogdepth = 1.0f;
    (void)state;
    return true;
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

void PrimitiveAssembler::ProcessTriangle(const ClipVertexData &v0, const ClipVertexData &v1,
                                         const ClipVertexData &v2,
                                         const ClipVertexData &provoking) {
    // Through mode needs no clipping (Clipper::ProcessTriangle, mask == 0).
    VertexData c2 = v2.v;
    if (!gs_->IsShadeGouraud()) {
        c2.color0 = provoking.v.color0;
        c2.color1 = provoking.v.color1;
    }
    const VertexData &a = v0.v;
    const VertexData &b = v1.v;
    // BinManager::AddTriangle: drop clockwise and degenerate triangles.
    const std::int64_t d01x = static_cast<std::int64_t>(a.screenpos.x) - b.screenpos.x;
    const std::int64_t d01y = static_cast<std::int64_t>(a.screenpos.y) - b.screenpos.y;
    const std::int64_t d02x = static_cast<std::int64_t>(a.screenpos.x) - c2.screenpos.x;
    const std::int64_t d02y = static_cast<std::int64_t>(a.screenpos.y) - c2.screenpos.y;
    if (d01x * d02y - d01y * d02x < 0) {
        ++stats_.culled;
        return;
    }
    if ((d01x == 0 && d02x == 0) || (d01y == 0 && d02y == 0)) return;
    const BinCoords range = Range(a, b, c2);
    if (range.Invalid()) return;
    ++stats_.triangles;
    raster_.DrawTriangle(a, b, c2, range, *state_);
}

void PrimitiveAssembler::ProcessRect(const ClipVertexData &v0, const ClipVertexData &v1) {
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
        if (range.Invalid()) return;
        ++stats_.rectangles;
        raster_.DrawRectangle(v0.v, v1.v, range, *state_);
    }
}

void PrimitiveAssembler::ProcessPoint(const ClipVertexData &v0) {
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

void PrimitiveAssembler::ProcessLine(const ClipVertexData & /*v0*/, const ClipVertexData & /*v1*/) {
    stats_.Unsupported("line segments (F12)");
}

std::uint32_t PrimitiveAssembler::Submit(const GeState &gs, const RasterizerState &state,
                                         PrimType prim_type, std::uint32_t vertex_addr,
                                         std::uint32_t index_addr, int vertex_count) {
    const VertexDecoder dec(gs.VertType());
    const std::uint32_t bytes_read = static_cast<std::uint32_t>(vertex_count) * dec.stride();
    // Vertices without position are entirely culled.
    if (!dec.has_position()) return bytes_read;

    gs_ = &gs;
    state_ = &state;
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
    if (!dec.through()) {
        // 3D: transform, clipping, lighting and fog are F12.
        stats_.Unsupported("transform-mode primitives (F12)");
        data_index_ = 0;
        return bytes_read;
    }

    const bool skip_cull = !gs.IsCullEnabled() || gs.IsModeClear();
    const int cull = skip_cull ? kCullOff : (gs.CullMode() ? kCullCCW : kCullCW);

    auto read = [&](int i, ClipVertexData *out) {
        if (!ReadVertex(gs, state, dec, vertex_addr, index_addr, i, out)) {
            stats_.Unsupported("vertex or index read outside memory");
            *out = ClipVertexData{};
        }
    };

    if (cull == kCullOff && prim_type == PrimType::kTriangles && data_index_ == 0 &&
        vertex_count >= 6 && (vertex_count % 6) == 0) {
        // Rectangles sent as pairs of triangles (through mode only).
        ClipVertexData buf[6];
        int buf_index = 0;
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            read(vtx, &buf[buf_index++]);
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
        return bytes_read;
    }

    switch (prim_type) {
    case PrimType::kPoints:
        for (int i = 0; i < data_index_; ++i) ProcessPoint(data_[i]);
        data_index_ = 0;
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            read(vtx, &data_[0]);
            ProcessPoint(data_[0]);
        }
        break;

    case PrimType::kLines:
        for (int i = 0; i < data_index_ - 1; i += 2) ProcessLine(data_[i], data_[i + 1]);
        data_index_ &= 1;
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            read(vtx, &data_[data_index_++]);
            if (data_index_ == 2) {
                ProcessLine(data_[0], data_[1]);
                data_index_ = 0;
            }
        }
        break;

    case PrimType::kTriangles:
        for (int vtx = 0; vtx < vertex_count; ++vtx) {
            read(vtx, &data_[data_index_++]);
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
            read(vtx, &data_[data_index_++]);
            if (data_index_ == 4 && cull == kCullOff) {
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
            read(vtx, &data_[(data_index_++) & 1]);
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
                for (int vtx = base == 0 ? 0 : 2; vtx < 4; ++vtx) read(base + vtx, &data_[vtx]);
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
            read(vtx, &data_[provoking]);
            --skip_count;
            ++start_vtx;
        }
        for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
            const int provoking = (data_index_++) % 3;
            read(vtx, &data_[provoking]);
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
            read(0, &data_[0]);
            data_index_++;
            start_vtx = 1;
        }
        if (data_index_ == 1 && vertex_count == 4 && cull == kCullOff) {
            for (int vtx = start_vtx; vtx < vertex_count; ++vtx) read(vtx, &data_[vtx]);
            int tl = -1;
            int br = -1;
            if (DetectRectangleFromFan(state, data_, &tl, &br)) {
                ProcessRect(data_[tl], data_[br]);
                break;
            }
        }
        for (int vtx = start_vtx; vtx < vertex_count && skip_count > 0; ++vtx) {
            const int provoking = 2 - ((data_index_++) % 2);
            read(vtx, &data_[provoking]);
            --skip_count;
            ++start_vtx;
        }
        for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
            const int provoking = 2 - ((data_index_++) % 2);
            read(vtx, &data_[provoking]);
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
    return bytes_read;
}

} // namespace mpo::ge
