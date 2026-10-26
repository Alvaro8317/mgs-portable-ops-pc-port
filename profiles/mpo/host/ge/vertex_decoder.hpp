// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/VertexDecoderCommon.cpp @ v1.20.4
// (GPL-2.0-or-later): vertex layout (VertexDecoder::SetVertexType) and the
// per-component decode steps, without the JIT.

#pragma once

#include <cstdint>

#include "memory_view.hpp"

namespace mpo::ge {

// One vertex expanded to floats, as PPSSPP's decoded vertex format.
struct DecodedVertex {
    float pos[3] = {0, 0, 0};
    float uv[2] = {0, 0};
    std::uint32_t color0 = 0;
    float normal[3] = {0, 0, 0};
    float weights[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

// Texture coordinate prescale for transform mode (TEXSCALE/TEXOFFSET).
struct UVScale {
    float u_scale = 1.0f;
    float v_scale = 1.0f;
    float u_off = 0.0f;
    float v_off = 0.0f;
};

// What a decode applies on top of the raw layout (PPSSPP's choice of steps in
// VertexDecoder::SetVertexType for the software renderer).
struct DecodeParams {
    // Transform mode with UV generation TEXTURE_COORDS (or UNKNOWN): the
    // Step_Tc*Prescale steps, uv * scale + offset.
    bool prescale_uv = false;
    UVScale uv_scale;
    // Transform mode with weights: skinning in decode (applySkinInDecode is
    // always on in softgpu). The 8 bone matrices, 4x3 each.
    const float *bones = nullptr;
};

// Byte stride of one vertex of `vtype`, including every morph target. This
// matches `ge_usage.vertex_size()` (F05), verified on real dumps.
std::uint32_t VertexSize(std::uint32_t vtype);

class VertexDecoder {
public:
    explicit VertexDecoder(std::uint32_t vtype);

    [[nodiscard]] std::uint32_t vtype() const { return vtype_; }
    [[nodiscard]] std::uint32_t stride() const { return size_; }
    [[nodiscard]] bool through() const { return through_; }
    [[nodiscard]] bool has_uv() const { return tc_ != 0; }
    [[nodiscard]] bool has_color() const { return col_ != 0; }
    [[nodiscard]] bool has_normal() const { return nrm_ != 0; }
    [[nodiscard]] bool has_position() const { return pos_ != 0; }
    [[nodiscard]] int num_weights() const { return weight_ ? nweights_ : 0; }
    [[nodiscard]] int morph_count() const { return morphcount_; }
    [[nodiscard]] int index_size() const; // 0, 1, 2 or 4 bytes.

    // Decodes the vertex at `address` (first morph target only: morphing is
    // not implemented and the caller reports it). Returns false when the
    // vertex is outside valid memory.
    bool Decode(const MemoryView &mem, std::uint32_t address, const DecodeParams &params,
                DecodedVertex *out) const;

private:
    std::uint32_t vtype_;
    bool through_;
    int tc_, col_, nrm_, pos_, weight_, nweights_, morphcount_;
    std::uint32_t tcoff_ = 0, coloff_ = 0, nrmoff_ = 0, posoff_ = 0;
    std::uint32_t onesize_ = 0, size_ = 0;
};

// Reads index `i` of the index buffer at `address` for `vtype` (8, 16 or
// 32-bit). Returns false on invalid memory.
bool ReadIndex(const MemoryView &mem, std::uint32_t address, std::uint32_t vtype, int i,
               std::uint32_t *out);

} // namespace mpo::ge
