// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/GPUCommon.cpp (Execute_BoundingBox) and
// GPU/Common/DrawEngineCommon.cpp (UpdatePlanes, TestBoundingBox)
// @ v1.20.4 (GPL-2.0-or-later).

#pragma once

#include <cstdint>

#include "ge_state.hpp"
#include "memory_view.hpp"

namespace mpo::ge {

enum class BoundingBoxResult {
    kVisible,
    kCulled,
    // The vertex or index data is outside memory; like PPSSPP, the box is
    // then taken as visible.
    kBadMemory,
};

// The BOUNDINGBOX test for `count` (> 0) vertices at `vertex_addr`: the box
// is culled when every vertex is outside one of the frustum planes built from
// the view/projection matrices, the viewport, the scissor and the region.
BoundingBoxResult TestBoundingBox(const GeState &gs, const MemoryView &mem,
                                  std::uint32_t vertex_addr, std::uint32_t index_addr, int count);

} // namespace mpo::ge
