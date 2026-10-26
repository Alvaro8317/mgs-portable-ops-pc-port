// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/SplineCommon.{h,cpp} (software tessellation
// of BEZIER and SPLINE, DrawEngineCommon::SubmitCurve) and
// GPU/Common/SoftwareTransformCommon.cpp (NormalizeVertices) @ v1.20.4
// (GPL-2.0-or-later).
//
// The patch is tessellated on the CPU into an indexed mesh, which then goes
// through the primitive assembler like any PRIM, as PPSSPP's software
// renderer does (it never uses hardware tessellation).

#pragma once

#include <cstdint>
#include <vector>

#include "ge_constants.hpp"
#include "ge_state.hpp"
#include "memory_view.hpp"
#include "vertex_decoder.hpp"

namespace mpo::ge {

struct PatchMesh {
    // Vertices as the vertex decoder delivers them for the generated format
    // TC_FLOAT | COL_8888 | NRM_FLOAT | POS_FLOAT (+ THROUGH of the original).
    std::vector<DecodedVertex> vertices;
    std::vector<std::uint16_t> indices;
    PrimType prim = PrimType::kTriangles;
    std::uint32_t vtype = 0;
};

// Tessellates the BEZIER or SPLINE `op` with the control points at
// `vertex_addr` (and `index_addr` when indexed). Returns false when nothing
// is drawn: fewer than 4 points in U or V (the hardware draws nothing) or a
// control point outside memory (`*bad_memory` is then set).
bool TessellatePatch(const GeState &gs, const MemoryView &mem, std::uint32_t op, bool spline,
                     std::uint32_t vertex_addr, std::uint32_t index_addr, PatchMesh *mesh,
                     bool *bad_memory);

} // namespace mpo::ge
