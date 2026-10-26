// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/SplineCommon.cpp and
// GPU/Common/SoftwareTransformCommon.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "spline.hpp"

#include <array>

#include "color.hpp"
#include "math3d.hpp"
#include "primitive_assembler.hpp"

namespace mpo::ge {
namespace {

// Spline::Weight.
struct Weight {
    float basis[4] = {};
    float deriv[4] = {};
};

// SimpleVertex as a set of floats.
struct ControlPoint {
    float uv[2] = {0, 0};
    std::uint32_t color = 0;
    Vec3f nrm;
    Vec3f pos;
};

void BezierCalcWeights(float t, Weight *w) {
    // Bernstein 3D basis polynomial.
    w->basis[0] = (1 - t) * (1 - t) * (1 - t);
    w->basis[1] = 3 * t * (1 - t) * (1 - t);
    w->basis[2] = 3 * t * t * (1 - t);
    w->basis[3] = t * t * t;
    // Derivative.
    w->deriv[0] = -3 * (1 - t) * (1 - t);
    w->deriv[1] = 9 * t * t - 12 * t + 3;
    w->deriv[2] = 3 * (2 - 3 * t) * t;
    w->deriv[3] = 3 * t * t;
}

std::vector<Weight> BezierWeights(int tess) {
    std::vector<Weight> weights(static_cast<std::size_t>(tess + 1));
    const float inv_tess = 1.0f / static_cast<float>(tess);
    for (int i = 0; i < tess + 1; ++i) {
        const float t = static_cast<float>(i) * inv_tess;
        BezierCalcWeights(t, &weights[static_cast<std::size_t>(i)]);
    }
    return weights;
}

struct KnotDiv {
    float d3_0 = 1.0f / 3.0f;
    float d4_1 = 1.0f / 3.0f;
    float d5_2 = 1.0f / 3.0f;
    float d3_1 = 1.0f / 2.0f;
    float d4_2 = 1.0f / 2.0f;
    float d3_2 = 1.0f; // Always 1.
};

// Spline3DWeight::CalcWeights. The SSE path computes b and d as
// (0 - f) * (0 - g); written the same way here.
void SplineCalcWeights(float t, const float *knots, const KnotDiv &div, Weight *w) {
    const float t0 = t - knots[0];
    const float t1 = t - knots[1];
    const float t2 = t - knots[2];

    const float f30 = t0 * div.d3_0;
    const float f41 = t1 * div.d4_1;
    const float f52 = t2 * div.d5_2;
    const float f31 = t1 * div.d3_1;
    const float f42 = t2 * div.d4_2;
    const float f32 = t2 * div.d3_2;

    const float a = (1 - f30) * (1 - f31);
    const float b = (0 - f31) * (0 - f41);
    const float c = (1 - f41) * (1 - f42);
    const float d = (0 - f42) * (0 - f52);

    w->basis[0] = a * (1 - f32);
    w->basis[1] = 1 - a - b + ((a + b + c - 1) * f32);
    w->basis[2] = b + ((1 - b - c - d) * f32);
    w->basis[3] = d * f32;

    const float i1 = (1 - f31) * (1 - f32);
    const float i2 = f31 * (1 - f32) + (1 - f42) * f32;
    const float i3 = f42 * f32;
    const float f130 = i1 * div.d3_0;
    const float f241 = i2 * div.d4_1;
    const float f352 = i3 * div.d5_2;
    w->deriv[0] = 3 * (0 - f130);
    w->deriv[1] = 3 * (f130 - f241);
    w->deriv[2] = 3 * (f241 - f352);
    w->deriv[3] = 3 * (f352 - 0);
}

std::vector<Weight> SplineWeights(int tess, int count, int type) {
    const int num_patches = count - 3;
    std::vector<Weight> weights(static_cast<std::size_t>(tess * num_patches + 1));
    // n + 2 knots plus one spare: the SSE path loads four of them.
    std::vector<float> knots(static_cast<std::size_t>(num_patches + 3), 0.0f);
    std::vector<KnotDiv> divs(static_cast<std::size_t>(num_patches));
    for (int i = 0; i < num_patches + 2; ++i)
        knots[static_cast<std::size_t>(i)] = static_cast<float>(i) - 2;
    const auto n = static_cast<std::size_t>(num_patches);
    if ((type & 1) != 0) { // The first edge is open.
        knots[0] = 0;
        knots[1] = 0;
        divs[0].d3_0 = 1.0f;
        divs[0].d4_1 = 1.0f / 2.0f;
        divs[0].d3_1 = 1.0f;
        if (n > 1) divs[1].d3_0 = 1.0f / 2.0f;
    }
    if ((type & 2) != 0) { // The last edge is open.
        divs[n - 1].d4_1 = 1.0f / 2.0f;
        divs[n - 1].d5_2 = 1.0f;
        divs[n - 1].d4_2 = 1.0f;
        if (n > 1) divs[n - 2].d5_2 = 1.0f / 2.0f;
    }
    const float inv_tess = 1.0f / static_cast<float>(tess);
    for (int i = 0; i < num_patches; ++i) {
        const int start = (i == 0) ? 0 : 1;
        for (int j = start; j <= tess; ++j) {
            const int index = i * tess + j;
            const float t = static_cast<float>(index) * inv_tess;
            SplineCalcWeights(t, knots.data() + i, divs[static_cast<std::size_t>(i)],
                              &weights[static_cast<std::size_t>(index)]);
        }
    }
    return weights;
}

// Linear combination ((p0 w0 + p1 w1) + p2 w2) + p3 w3, per component.
template <int N>
using Vecf = std::array<float, N>;

template <int N>
Vecf<N> Sample(const Vecf<N> p[4], const float w[4]) {
    Vecf<N> out;
    for (int i = 0; i < N; ++i) {
        out[static_cast<std::size_t>(i)] =
            p[0][static_cast<std::size_t>(i)] * w[0] + p[1][static_cast<std::size_t>(i)] * w[1] +
            p[2][static_cast<std::size_t>(i)] * w[2] + p[3][static_cast<std::size_t>(i)] * w[3];
    }
    return out;
}

// Tessellator: one 4x4 patch, p[v][u].
template <int N>
class Tessellator {
public:
    Tessellator(const std::vector<Vecf<N>> &points, const int idx[4]) {
        for (int k = 0; k < 4; ++k) p_[k] = points.data() + idx[k];
    }

    void SampleU(const float weights[4]) {
        if (weights[0] == 1.0f) { // {1,0,0,0}: the first edge is open.
            SampleEdgeU(0);
            return;
        }
        if (weights[3] == 1.0f) { // {0,0,0,1}: the last edge is open.
            SampleEdgeU(3);
            return;
        }
        for (int k = 0; k < 4; ++k) u_[k] = Sample<N>(p_[k], weights);
    }

    Vecf<N> SampleV(const float weights[4]) const {
        if (weights[0] == 1.0f) return u_[0];
        if (weights[3] == 1.0f) return u_[3];
        return Sample<N>(u_, weights);
    }

private:
    void SampleEdgeU(int idx) {
        for (int k = 0; k < 4; ++k) u_[k] = p_[k][idx];
    }

    const Vecf<N> *p_[4] = {};
    Vecf<N> u_[4] = {};
};

// Vec4<float>::ToRGBA (SSE): CVTPS2DQ of c * 255, then saturating packs.
std::uint32_t ColorToRGBA(const Vecf<4> &c) {
    std::uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        const int v = RoundToInt(c[static_cast<std::size_t>(i)] * 255.0f);
        const int clamped = v < 0 ? 0 : (v > 255 ? 255 : v);
        out |= static_cast<std::uint32_t>(clamped) << (8 * i);
    }
    return out;
}

struct Surface {
    bool spline = true;
    int tess_u = 1;
    int tess_v = 1;
    int num_points_u = 0;
    int num_points_v = 0;
    int num_patches_u = 0;
    int num_patches_v = 0;
    int type_u = 0;
    int type_v = 0;
    PatchPrimType prim_type = PatchPrimType::kTriangles;
    bool patch_facing = false;
    // Bezier.
    int num_verts_per_patch = 0;
    // Spline.
    int num_vertices_u = 0;

    void Init(int max_vertices) {
        // If specified as 0, uses 1. Spline quality "high" (PPSSPP's default)
        // keeps the division as it is.
        if (tess_u < 1) tess_u = 1;
        if (tess_v < 1) tess_v = 1;
        if (spline) {
            while ((num_patches_u * tess_u + 1) * (num_patches_v * tess_v + 1) > max_vertices) {
                tess_u--;
                tess_v--;
            }
            num_vertices_u = num_patches_u * tess_u + 1;
        } else {
            while ((tess_u + 1) * (tess_v + 1) * num_patches_u * num_patches_v > max_vertices) {
                tess_u--;
                tess_v--;
            }
            num_verts_per_patch = (tess_u + 1) * (tess_v + 1);
        }
    }

    [[nodiscard]] int TessStart(int patch) const { return spline && patch != 0 ? 1 : 0; }
    [[nodiscard]] int PointIndex(int patch_u, int patch_v) const {
        return spline ? patch_v * num_points_u + patch_u : patch_v * 3 * num_points_u + patch_u * 3;
    }
    [[nodiscard]] int IndexU(int patch_u, int tile_u) const {
        return spline ? patch_u * tess_u + tile_u : tile_u;
    }
    [[nodiscard]] int IndexV(int patch_v, int tile_v) const {
        return spline ? patch_v * tess_v + tile_v : tile_v;
    }
    [[nodiscard]] int Index(int index_u, int index_v, int patch_u, int patch_v) const {
        if (spline) return index_v * num_vertices_u + index_u;
        const int patch_index = patch_v * num_patches_u + patch_u;
        return index_v * (tess_u + 1) + index_u + num_verts_per_patch * patch_index;
    }
    [[nodiscard]] int VertexCount() const {
        return spline ? (num_patches_u * tess_u + 1) * (num_patches_v * tess_v + 1)
                      : num_verts_per_patch * num_patches_u * num_patches_v;
    }
};

void CopyQuadIndex(std::vector<std::uint16_t> *out, PatchPrimType type, int idx0, int idx1,
                   int idx2, int idx3) {
    const int order_lines[6] = {idx0, idx2, idx1, idx3, idx1, idx2};
    const int order_tris[6] = {idx0, idx2, idx1, idx1, idx2, idx3};
    const int *order = type == PatchPrimType::kLines ? order_lines : order_tris;
    for (int i = 0; i < 6; ++i) out->push_back(static_cast<std::uint16_t>(order[i]));
}

void BuildIndex(std::vector<std::uint16_t> *out, int num_u, int num_v, PatchPrimType prim_type,
                int total) {
    for (int v = 0; v < num_v; ++v) {
        for (int u = 0; u < num_u; ++u) {
            const int idx0 = v * (num_u + 1) + u + total;       // top left
            const int idx2 = (v + 1) * (num_u + 1) + u + total; // bottom left
            CopyQuadIndex(out, prim_type, idx0, idx0 + 1, idx2, idx2 + 1);
        }
    }
}

PrimType PatchPrimToPrim(PatchPrimType type) {
    switch (type) {
    case PatchPrimType::kTriangles:
        return PrimType::kTriangles;
    case PatchPrimType::kLines:
        return PrimType::kLines;
    case PatchPrimType::kPoints:
    case PatchPrimType::kUnknown:
    default:
        return PrimType::kPoints;
    }
}

} // namespace

bool TessellatePatch(const GeState &gs, const MemoryView &mem, std::uint32_t op, bool spline,
                     std::uint32_t vertex_addr, std::uint32_t index_addr, PatchMesh *mesh,
                     bool *bad_memory) {
    *bad_memory = false;
    Surface surface;
    surface.spline = spline;
    surface.tess_u = gs.PatchDivisionU();
    surface.tess_v = gs.PatchDivisionV();
    surface.num_points_u = static_cast<int>(op & 0xFF);
    surface.num_points_v = static_cast<int>((op >> 8) & 0xFF);
    if (spline) {
        surface.type_u = static_cast<int>((op >> 16) & 0x3);
        surface.type_v = static_cast<int>((op >> 18) & 0x3);
        surface.num_patches_u = surface.num_points_u - 3;
        surface.num_patches_v = surface.num_points_v - 3;
    } else {
        surface.num_patches_u = (surface.num_points_u - 1) / 3;
        surface.num_patches_v = (surface.num_points_v - 1) / 3;
    }
    surface.prim_type = gs.PatchPrimitiveType();
    surface.patch_facing = gs.PatchFacing();

    // Real hardware draws nothing when given < 4 points in U or V.
    if (surface.num_points_u < 4 || surface.num_points_v < 4) return false;

    // NormalizeVertices: decode every control point (skinning in decode, UV
    // prescale), with the material ambient colour when there is none.
    const std::uint32_t vtype = gs.VertType();
    const VertexDecoder dec(vtype);
    MemoryVertexSource source(mem, dec, ComputeDecodeParams(gs, vtype), vertex_addr, index_addr);
    const bool has_tc = (vtype & kVtypeTcMask) != 0;
    const bool has_col = (vtype & kVtypeColMask) != 0;
    const bool has_nrm = (vtype & kVtypeNrmMask) != 0;
    const int num_points = surface.num_points_u * surface.num_points_v;
    std::vector<ControlPoint> points(static_cast<std::size_t>(num_points));
    for (int i = 0; i < num_points; ++i) {
        DecodedVertex dv;
        if (!source.Read(i, &dv)) {
            *bad_memory = true;
            return false;
        }
        ControlPoint &cp = points[static_cast<std::size_t>(i)];
        if (has_tc) {
            cp.uv[0] = dv.uv[0];
            cp.uv[1] = dv.uv[1];
        }
        cp.color = has_col ? dv.color0 : gs.MaterialAmbientRGBA();
        cp.nrm = has_nrm ? Vec3f{dv.normal[0], dv.normal[1], dv.normal[2]} : Vec3f{0, 0, 1.0f};
        cp.pos = Vec3f{dv.pos[0], dv.pos[1], dv.pos[2]};
    }

    // ControlPoints::Convert.
    std::vector<Vecf<3>> pos(points.size());
    std::vector<Vecf<2>> tex(points.size());
    std::vector<Vecf<4>> col(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        pos[i] = {points[i].pos.x, points[i].pos.y, points[i].pos.z};
        tex[i] = {points[i].uv[0], points[i].uv[1]};
        const Vec4i c = Vec4i::FromRGBA(points[i].color);
        for (int k = 0; k < 4; ++k) {
            col[i][static_cast<std::size_t>(k)] = static_cast<float>(c[k]) * (1.0f / 255.0f);
        }
    }
    const std::uint32_t defcolor = points[0].color;

    // DECODED_VERTEX_BUFFER_SIZE / 2 / sizeof(SimpleVertex) = 65536.
    surface.Init(65536);

    const std::vector<Weight> weights_u =
        spline ? SplineWeights(surface.tess_u, surface.num_points_u, surface.type_u)
               : BezierWeights(surface.tess_u);
    const std::vector<Weight> weights_v =
        spline ? SplineWeights(surface.tess_v, surface.num_points_v, surface.type_v)
               : BezierWeights(surface.tess_v);

    const bool sample_nrm = has_nrm || gs.IsLightingEnabled();
    const bool sample_col = has_col;
    const bool sample_tex = has_tc;
    const bool through = (vtype & kVtypeThrough) != 0;

    struct OutVertex {
        float uv[2] = {0, 0};
        std::uint32_t color = 0;
        Vec3f nrm;
        Vec3f pos;
    };
    std::vector<OutVertex> out(static_cast<std::size_t>(surface.VertexCount()));

    const float inv_u = 1.0f / static_cast<float>(surface.tess_u);
    const float inv_v = 1.0f / static_cast<float>(surface.tess_v);
    for (int patch_u = 0; patch_u < surface.num_patches_u; ++patch_u) {
        const int start_u = surface.TessStart(patch_u);
        for (int patch_v = 0; patch_v < surface.num_patches_v; ++patch_v) {
            const int start_v = surface.TessStart(patch_v);
            const int idx = surface.PointIndex(patch_u, patch_v);
            const int idx_v[4] = {idx, idx + surface.num_points_u, idx + surface.num_points_u * 2,
                                  idx + surface.num_points_u * 3};
            Tessellator<3> tess_pos(pos, idx_v);
            Tessellator<4> tess_col(col, idx_v);
            Tessellator<2> tess_tex(tex, idx_v);
            Tessellator<3> tess_nrm(pos, idx_v);

            for (int tile_u = start_u; tile_u <= surface.tess_u; ++tile_u) {
                const int index_u = surface.IndexU(patch_u, tile_u);
                const Weight &wu = weights_u[static_cast<std::size_t>(index_u)];
                tess_pos.SampleU(wu.basis);
                if (sample_col) tess_col.SampleU(wu.basis);
                if (sample_tex) tess_tex.SampleU(wu.basis);
                if (sample_nrm) tess_nrm.SampleU(wu.deriv);

                for (int tile_v = start_v; tile_v <= surface.tess_v; ++tile_v) {
                    const int index_v = surface.IndexV(patch_v, tile_v);
                    const Weight &wv = weights_v[static_cast<std::size_t>(index_v)];
                    OutVertex &vert = out[static_cast<std::size_t>(
                        surface.Index(index_u, index_v, patch_u, patch_v))];

                    const Vecf<3> p = tess_pos.SampleV(wv.basis);
                    vert.pos = Vec3f{p[0], p[1], p[2]};
                    vert.color = sample_col ? ColorToRGBA(tess_col.SampleV(wv.basis)) : defcolor;
                    if (sample_tex) {
                        const Vecf<2> uv = tess_tex.SampleV(wv.basis);
                        vert.uv[0] = uv[0];
                        vert.uv[1] = uv[1];
                    } else {
                        // Generated texture coordinates.
                        vert.uv[0] =
                            static_cast<float>(patch_u) + static_cast<float>(tile_u) * inv_u;
                        vert.uv[1] =
                            static_cast<float>(patch_v) + static_cast<float>(tile_v) * inv_v;
                    }
                    if (sample_nrm) {
                        const Vecf<3> du = tess_nrm.SampleV(wv.basis);
                        const Vecf<3> dvv = tess_pos.SampleV(wv.deriv);
                        Vec3f n = NormalizedApprox(
                            Cross(Vec3f{du[0], du[1], du[2]}, Vec3f{dvv[0], dvv[1], dvv[2]}));
                        if (surface.patch_facing) n = Vec3f{n.x * -1.0f, n.y * -1.0f, n.z * -1.0f};
                        vert.nrm = n;
                    } else {
                        vert.nrm = Vec3f{0.0f, 0.0f, 1.0f};
                    }
                }
            }
        }
    }

    mesh->indices.clear();
    if (spline) {
        BuildIndex(&mesh->indices, surface.num_patches_u * surface.tess_u,
                   surface.num_patches_v * surface.tess_v, surface.prim_type, 0);
    } else {
        for (int patch_u = 0; patch_u < surface.num_patches_u; ++patch_u) {
            for (int patch_v = 0; patch_v < surface.num_patches_v; ++patch_v) {
                const int patch_index = patch_v * surface.num_patches_u + patch_u;
                BuildIndex(&mesh->indices, surface.tess_u, surface.tess_v, surface.prim_type,
                           patch_index * surface.num_verts_per_patch);
            }
        }
    }

    // The generated vertices go through the vertex decoder again with the
    // float format and the UV scale/offset forced to identity (SubmitCurve).
    const TexMapMode mode = gs.UVGenMode();
    const bool prescale =
        !through && (mode == TexMapMode::kTextureCoords || mode == TexMapMode::kUnknown);
    mesh->vertices.resize(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const OutVertex &o = out[i];
        DecodedVertex &dv = mesh->vertices[i];
        dv = DecodedVertex{};
        dv.uv[0] = prescale ? o.uv[0] * 1.0f + 0.0f : o.uv[0];
        dv.uv[1] = prescale ? o.uv[1] * 1.0f + 0.0f : o.uv[1];
        dv.color0 = o.color;
        dv.normal[0] = o.nrm.x;
        dv.normal[1] = o.nrm.y;
        dv.normal[2] = o.nrm.z;
        dv.pos[0] = o.pos.x;
        dv.pos[1] = o.pos.y;
        dv.pos[2] = o.pos.z;
        if (through) { // Step_PosFloatThrough clamps z.
            if (dv.pos[2] > 65535.0f) {
                dv.pos[2] = 65535.0f;
            } else if (dv.pos[2] < 0.0f) {
                dv.pos[2] = 0.0f;
            }
        }
    }
    mesh->prim = PatchPrimToPrim(surface.prim_type);
    // TC_FLOAT | COL_8888 | NRM_FLOAT | POS_FLOAT | IDX_16BIT.
    mesh->vtype = (3u << 0) | (7u << 2) | (3u << 5) | (3u << 7) | (vtype & kVtypeThrough) |
                  (2u << kVtypeIdxShift);
    return !mesh->indices.empty();
}

} // namespace mpo::ge
