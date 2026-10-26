// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/TransformUnit.cpp (SubmitPrimitive,
// ReadVertex, SendTriangle), GPU/Software/Clipper.cpp (ProcessRect,
// ProcessTriangle, ProcessPoint) and the range/cull logic of
// GPU/Software/BinManager.cpp @ v1.20.4 (GPL-2.0-or-later).
//
// F11a covers through mode (2D). Transform-mode primitives (3D: matrices,
// viewport, clipping, lighting, fog) are F12: they are read, counted as
// skipped and not drawn. Lines are F12 as well.

#pragma once

#include <cstdint>

#include "ge_constants.hpp"
#include "ge_state.hpp"
#include "memory_view.hpp"
#include "rasterizer.hpp"
#include "stats.hpp"
#include "vertex_decoder.hpp"

namespace mpo::ge {

class PrimitiveAssembler {
public:
    PrimitiveAssembler(const MemoryView &mem, Stats &stats)
        : mem_(mem), raster_(mem), stats_(stats) {}

    // Submits `count` vertices of `prim` (PRIM command). `vertex_addr` and
    // `index_addr` are absolute PSP addresses. Returns the bytes of vertex data
    // consumed (for VADDR advance when not indexed).
    std::uint32_t Submit(const GeState &gs, const RasterizerState &state, PrimType prim,
                         std::uint32_t vertex_addr, std::uint32_t index_addr, int count);

    // Forgets a pending strip/fan (new list or context restore).
    void Reset() {
        data_index_ = 0;
        prev_prim_ = PrimType::kPoints;
    }

private:
    bool ReadVertex(const GeState &gs, const RasterizerState &state, const VertexDecoder &dec,
                    std::uint32_t vertex_addr, std::uint32_t index_addr, int i,
                    ClipVertexData *out);
    void SendTriangle(int cull, const ClipVertexData *verts, int provoking);
    void ProcessTriangle(const ClipVertexData &v0, const ClipVertexData &v1,
                         const ClipVertexData &v2, const ClipVertexData &provoking);
    void ProcessRect(const ClipVertexData &v0, const ClipVertexData &v1);
    void ProcessPoint(const ClipVertexData &v0);
    void ProcessLine(const ClipVertexData &v0, const ClipVertexData &v1);
    [[nodiscard]] BinCoords Range(const VertexData &v0, const VertexData &v1) const;
    [[nodiscard]] BinCoords Range(const VertexData &v0, const VertexData &v1,
                                  const VertexData &v2) const;
    [[nodiscard]] BinCoords Scissor(BinCoords range) const;

    const MemoryView &mem_;
    Rasterizer raster_;
    Stats &stats_;

    // Per-draw context, valid during Submit().
    const GeState *gs_ = nullptr;
    const RasterizerState *state_ = nullptr;
    BinCoords scissor_;

    ClipVertexData data_[4];
    int data_index_ = 0;
    PrimType prev_prim_ = PrimType::kPoints;
    // ReadVertex keeps the last UV and normal across draws (statics in PPSSPP).
    float last_s_ = 0;
    float last_t_ = 0;
};

} // namespace mpo::ge
