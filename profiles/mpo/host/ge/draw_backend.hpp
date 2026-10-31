// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the primitive assembler sends what it produced: screen-space
// triangles, rectangles, lines and points, already transformed, lit, clipped
// against -Z and culled by mpo_ge (the "software transform" split that
// PPSSPP's hardware renderers also use). The default backend is the software
// rasterizer (softgpu); the Vulkan renderer (F14a, profiles/mpo/host/vulkan)
// implements the same interface and rasterizes on the GPU instead.

#pragma once

#include <cstdint>

#include "ge_state.hpp"
#include "rasterizer.hpp"

namespace mpo::ge {

class DrawBackend {
public:
    virtual ~DrawBackend() = default;

    // Called once per draw command (PRIM, or the mesh of a BEZIER/SPLINE)
    // before its primitives, with the state they are drawn with. `state` is
    // the object later passed to the Draw* calls of this draw.
    virtual void BeginDraw(const GeState &gs, const RasterizerState &state) {
        (void)gs;
        (void)state;
    }

    // Same contracts as the Rasterizer methods of the same name.
    virtual void DrawTriangle(const VertexData &v0, const VertexData &v1, const VertexData &v2,
                              const BinCoords &range, const RasterizerState &state) = 0;
    virtual void DrawRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                               const RasterizerState &state) = 0;
    virtual void DrawSprite(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                            const RasterizerState &state) = 0;
    virtual void ClearRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                                const RasterizerState &state) = 0;
    virtual void DrawPoint(const VertexData &v0, const BinCoords &range,
                           const RasterizerState &state) = 0;
    virtual void DrawLine(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                          const RasterizerState &state) = 0;

    // F15b: called before the GE reads (`write` false) or writes (`write`
    // true) memory outside a draw: a CLUT load reads the palette, a block
    // transfer reads its source and writes its destination. `addr` is a PSP
    // address (with or without the VRAM base) and `size` may span several
    // rows. A backend that keeps framebuffers away from memory (the GPU)
    // brings memory up to date first, and after a write takes it again from
    // memory. softgpu draws in memory, so it has nothing to do.
    virtual void SyncMemory(std::uint32_t addr, std::uint32_t size, bool write) {
        (void)addr;
        (void)size;
        (void)write;
    }
};

// softgpu: draws into the VRAM framebuffer through Rasterizer.
class SoftwareBackend final : public DrawBackend {
public:
    explicit SoftwareBackend(const MemoryView &mem) : raster_(mem) {}

    void DrawTriangle(const VertexData &v0, const VertexData &v1, const VertexData &v2,
                      const BinCoords &range, const RasterizerState &state) override {
        raster_.DrawTriangle(v0, v1, v2, range, state);
    }
    void DrawRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                       const RasterizerState &state) override {
        raster_.DrawRectangle(v0, v1, range, state);
    }
    void DrawSprite(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                    const RasterizerState &state) override {
        raster_.DrawSprite(v0, v1, range, state);
    }
    void ClearRectangle(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                        const RasterizerState &state) override {
        raster_.ClearRectangle(v0, v1, range, state);
    }
    void DrawPoint(const VertexData &v0, const BinCoords &range,
                   const RasterizerState &state) override {
        raster_.DrawPoint(v0, range, state);
    }
    void DrawLine(const VertexData &v0, const VertexData &v1, const BinCoords &range,
                  const RasterizerState &state) override {
        raster_.DrawLine(v0, v1, range, state);
    }

private:
    Rasterizer raster_;
};

} // namespace mpo::ge
