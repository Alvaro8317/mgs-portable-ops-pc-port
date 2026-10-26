// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/TransformUnit.cpp (SubmitPrimitive,
// SoftwareVertexReader, SendTriangle), GPU/Software/Clipper.cpp
// (ProcessRect, ProcessTriangle, ProcessLine, ProcessPoint, the -Z clip) and
// the range/cull logic of GPU/Software/BinManager.cpp @ v1.20.4
// (GPL-2.0-or-later).
//
// Through mode (2D) and transform mode (3D: matrices, viewport, depth clamp,
// clipping against -Z, lighting and fog), every primitive type, lines
// included.

#pragma once

#include <cstdint>
#include <span>

#include "draw_backend.hpp"
#include "ge_constants.hpp"
#include "ge_state.hpp"
#include "memory_view.hpp"
#include "rasterizer.hpp"
#include "stats.hpp"
#include "transform.hpp"
#include "vertex_decoder.hpp"

namespace mpo::ge {

// Supplies the decoded vertices of one draw, index lookup included.
class VertexSource {
public:
    virtual ~VertexSource() = default;
    // Decodes vertex number `i` of the draw. Returns false on bad memory.
    virtual bool Read(int i, DecodedVertex *out) = 0;
};

// Vertices and indices in PSP memory (PRIM, BOUNDINGBOX).
class MemoryVertexSource final : public VertexSource {
public:
    MemoryVertexSource(const MemoryView &mem, const VertexDecoder &dec, const DecodeParams &params,
                       std::uint32_t vertex_addr, std::uint32_t index_addr)
        : mem_(mem), dec_(dec), params_(params), vertex_addr_(vertex_addr),
          index_addr_(index_addr) {}

    bool Read(int i, DecodedVertex *out) override;

private:
    const MemoryView &mem_;
    const VertexDecoder &dec_;
    DecodeParams params_;
    std::uint32_t vertex_addr_;
    std::uint32_t index_addr_;
};

// Already decoded vertices with 16-bit indices (tessellated patches).
class ArrayVertexSource final : public VertexSource {
public:
    ArrayVertexSource(std::span<const DecodedVertex> vertices,
                      std::span<const std::uint16_t> indices)
        : vertices_(vertices), indices_(indices) {}

    bool Read(int i, DecodedVertex *out) override;

private:
    std::span<const DecodedVertex> vertices_;
    std::span<const std::uint16_t> indices_;
};

// The decode parameters PPSSPP's software renderer uses for `vtype` with the
// current state (UV prescale, skinning in decode).
DecodeParams ComputeDecodeParams(const GeState &gs, std::uint32_t vtype);

class PrimitiveAssembler {
public:
    PrimitiveAssembler(const MemoryView &mem, Stats &stats)
        : mem_(mem), soft_(mem), stats_(stats) {}

    // Sends the primitives to `backend` instead of the software rasterizer
    // (nullptr restores it). The backend must outlive its use here.
    void SetBackend(DrawBackend *backend) { backend_ = backend ? backend : &soft_; }

    // Submits `count` vertices of `prim` (PRIM command). `vertex_addr` and
    // `index_addr` are absolute PSP addresses. Returns the bytes of vertex data
    // consumed (for VADDR advance when not indexed).
    std::uint32_t Submit(const GeState &gs, const RasterizerState &state, PrimType prim,
                         std::uint32_t vertex_addr, std::uint32_t index_addr, int count);

    // Submits `count` vertices from `source`, decoded with the layout of
    // `vtype` (only its through, UV, colour, normal and position bits
    // matter here).
    void SubmitVertices(const GeState &gs, const RasterizerState &state, PrimType prim,
                        VertexSource &source, std::uint32_t vtype, int count);

    // Forgets a pending strip/fan (new list or context restore).
    void Reset() {
        data_index_ = 0;
        prev_prim_ = PrimType::kPoints;
    }

private:
    ClipVertexData ReadVertex(int i);
    void SendTriangle(int cull, const ClipVertexData *verts, int provoking);
    // Clipper.
    void ProcessTriangle(const ClipVertexData &v0, const ClipVertexData &v1,
                         const ClipVertexData &v2, const ClipVertexData &provoking);
    void ProcessRect(const ClipVertexData &v0, const ClipVertexData &v1);
    void ProcessPoint(const ClipVertexData &v0);
    void ProcessLine(const ClipVertexData &v0, const ClipVertexData &v1);
    void ClipInterpolate(ClipVertexData *dest, float t, const ClipVertexData &a,
                         const ClipVertexData &b) const;
    void AddTriangleRect(const VertexData &v0, const VertexData &v1);
    // BinManager.
    void AddTriangle(const VertexData &v0, const VertexData &v1, const VertexData &v2);
    void AddRect(const VertexData &v0, const VertexData &v1);
    void AddLine(const VertexData &v0, const VertexData &v1);
    [[nodiscard]] BinCoords Range(const VertexData &v0, const VertexData &v1) const;
    [[nodiscard]] BinCoords Range(const VertexData &v0, const VertexData &v1,
                                  const VertexData &v2) const;
    [[nodiscard]] BinCoords Scissor(BinCoords range) const;

    const MemoryView &mem_;
    SoftwareBackend soft_;
    DrawBackend *backend_ = &soft_;
    Stats &stats_;

    // Per-draw context, valid during SubmitVertices().
    const GeState *gs_ = nullptr;
    const RasterizerState *state_ = nullptr;
    VertexSource *source_ = nullptr;
    VertexFormat format_;
    TransformState transform_;
    BinCoords scissor_;

    ClipVertexData data_[4];
    int data_index_ = 0;
    PrimType prev_prim_ = PrimType::kPoints;
    // ReadVertex keeps the last UV and normal across draws (statics in PPSSPP).
    VertexHistory history_;
};

} // namespace mpo::ge
