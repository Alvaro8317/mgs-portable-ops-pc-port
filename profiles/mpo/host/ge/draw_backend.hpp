// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the primitive assembler sends what it produced: screen-space
// triangles, rectangles, lines and points, already transformed, lit, clipped
// against -Z and culled by mpo_ge (the "software transform" split that
// PPSSPP's hardware renderers also use). The default backend is the software
// rasterizer (softgpu); the Vulkan renderer (F14a, profiles/mpo/host/vulkan)
// implements the same interface and rasterizes on the GPU instead.

#pragma once

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
