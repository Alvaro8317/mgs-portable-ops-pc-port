// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/VertexDecoderCommon.cpp @ v1.20.4
// (GPL-2.0-or-later).

#include "vertex_decoder.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include "color.hpp"
#include "ge_constants.hpp"

namespace mpo::ge {
namespace {

constexpr std::uint32_t kTcSize[4] = {0, 2, 4, 8};
constexpr std::uint32_t kTcAlign[4] = {0, 1, 2, 4};
constexpr std::uint32_t kColSize[8] = {0, 0, 0, 0, 2, 2, 2, 4};
constexpr std::uint32_t kColAlign[8] = {0, 0, 0, 0, 2, 2, 2, 4};
constexpr std::uint32_t kNrmSize[4] = {0, 3, 6, 12};
constexpr std::uint32_t kNrmAlign[4] = {0, 1, 2, 4};
constexpr std::uint32_t kPosSize[4] = {3, 3, 6, 12};
constexpr std::uint32_t kPosAlign[4] = {1, 1, 2, 4};
constexpr std::uint32_t kWtSize[4] = {0, 1, 2, 4};
constexpr std::uint32_t kWtAlign[4] = {0, 1, 2, 4};

std::uint32_t Align(std::uint32_t n, std::uint32_t align) {
    if (align <= 1) return n;
    return (n + (align - 1)) & ~(align - 1);
}

template <typename T>
T Load(const std::uint8_t *p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

} // namespace

VertexDecoder::VertexDecoder(std::uint32_t vtype)
    : vtype_(vtype & 0xFFFFFF), through_((vtype & kVtypeThrough) != 0),
      tc_(static_cast<int>(vtype & 3)), col_(static_cast<int>((vtype >> 2) & 7)),
      nrm_(static_cast<int>((vtype >> 5) & 3)), pos_(static_cast<int>((vtype >> 7) & 3)),
      weight_(static_cast<int>((vtype >> 9) & 3)),
      nweights_(static_cast<int>((vtype >> 14) & 7) + 1),
      morphcount_(static_cast<int>((vtype >> 18) & 7) + 1) {
    std::uint32_t size = 0;
    std::uint32_t biggest = 0;
    if (weight_) {
        size += kWtSize[weight_] * static_cast<std::uint32_t>(nweights_);
        biggest = std::max(biggest, kWtAlign[weight_]);
    }
    if (tc_) {
        size = Align(size, kTcAlign[tc_]);
        tcoff_ = size;
        size += kTcSize[tc_];
        biggest = std::max(biggest, kTcAlign[tc_]);
    }
    if (col_) {
        size = Align(size, kColAlign[col_]);
        coloff_ = size;
        size += kColSize[col_];
        biggest = std::max(biggest, kColAlign[col_]);
    }
    if (nrm_) {
        size = Align(size, kNrmAlign[nrm_]);
        nrmoff_ = size;
        size += kNrmSize[nrm_];
        biggest = std::max(biggest, kNrmAlign[nrm_]);
    }
    size = Align(size, kPosAlign[pos_]);
    posoff_ = size;
    size += kPosSize[pos_];
    biggest = std::max(biggest, kPosAlign[pos_]);
    onesize_ = Align(size, biggest);
    size_ = onesize_ * static_cast<std::uint32_t>(morphcount_);
}

std::uint32_t VertexSize(std::uint32_t vtype) {
    return VertexDecoder(vtype).stride();
}

int VertexDecoder::index_size() const {
    switch ((vtype_ & kVtypeIdxMask) >> kVtypeIdxShift) {
    case 1:
        return 1;
    case 2:
        return 2;
    case 3:
        return 4;
    default:
        return 0;
    }
}

bool VertexDecoder::Decode(const MemoryView &mem, std::uint32_t address, const UVScale &uv_scale,
                           bool prescale_uv, DecodedVertex *out) const {
    const std::uint8_t *ptr = mem.Translate(address, onesize_);
    if (!ptr) return false;
    *out = DecodedVertex{};

    if (weight_) {
        const std::uint8_t *w = ptr;
        for (int i = 0; i < nweights_; ++i) {
            switch (weight_) {
            case 1:
                out->weights[i] = static_cast<float>(w[i]) * (1.0f / 128.0f);
                break;
            case 2:
                out->weights[i] =
                    static_cast<float>(Load<std::uint16_t>(w + i * 2)) * (1.0f / 32768.0f);
                break;
            default:
                out->weights[i] = Load<float>(w + i * 4);
                break;
            }
        }
    }

    if (tc_) {
        const std::uint8_t *t = ptr + tcoff_;
        float u = 0;
        float v = 0;
        switch (tc_) {
        case 1: // Step_TcU8ToFloat (also in through mode).
            u = static_cast<float>(t[0]) * (1.0f / 128.0f);
            v = static_cast<float>(t[1]) * (1.0f / 128.0f);
            break;
        case 2:
            if (through_) { // Step_TcU16ThroughToFloat.
                u = static_cast<float>(Load<std::uint16_t>(t));
                v = static_cast<float>(Load<std::uint16_t>(t + 2));
            } else { // Step_TcU16ToFloat / Step_TcU16Prescale.
                u = static_cast<float>(Load<std::uint16_t>(t)) * (1.0f / 32768.0f);
                v = static_cast<float>(Load<std::uint16_t>(t + 2)) * (1.0f / 32768.0f);
            }
            break;
        default:
            u = Load<float>(t);
            v = Load<float>(t + 4);
            break;
        }
        if (!through_ && prescale_uv) {
            u = u * uv_scale.u_scale + uv_scale.u_off;
            v = v * uv_scale.v_scale + uv_scale.v_off;
        }
        out->uv[0] = u;
        out->uv[1] = v;
    }

    if (col_) {
        const std::uint8_t *c = ptr + coloff_;
        switch (col_) {
        case 4:
            out->color0 = RGB565ToRGBA8888(Load<std::uint16_t>(c));
            break;
        case 5:
            out->color0 = RGBA5551ToRGBA8888(Load<std::uint16_t>(c));
            break;
        case 6:
            out->color0 = RGBA4444ToRGBA8888(Load<std::uint16_t>(c));
            break;
        case 7:
            out->color0 = Load<std::uint32_t>(c);
            break;
        default: // Step_ColorInvalid leaves the decoded colour untouched.
            break;
        }
    }

    if (nrm_) {
        const std::uint8_t *n = ptr + nrmoff_;
        for (int i = 0; i < 3; ++i) {
            switch (nrm_) {
            case 1:
                out->normal[i] =
                    static_cast<float>(static_cast<std::int8_t>(n[i])) * (1.0f / 128.0f);
                break;
            case 2:
                out->normal[i] =
                    static_cast<float>(Load<std::int16_t>(n + i * 2)) * (1.0f / 32768.0f);
                break;
            default:
                out->normal[i] = Load<float>(n + i * 4);
                break;
            }
        }
    }

    const std::uint8_t *p = ptr + posoff_;
    if (through_) {
        switch (pos_) {
        case 1: // 8-bit positions in through mode always decode to 0.
            break;
        case 2: // Step_PosS16Through: z is unsigned.
            out->pos[0] = static_cast<float>(Load<std::int16_t>(p));
            out->pos[1] = static_cast<float>(Load<std::int16_t>(p + 2));
            out->pos[2] = static_cast<float>(Load<std::uint16_t>(p + 4));
            break;
        case 3: { // Step_PosFloatThrough.
            out->pos[0] = Load<float>(p);
            out->pos[1] = Load<float>(p + 4);
            const float z = Load<float>(p + 8);
            out->pos[2] = z > 65535.0f ? 65535.0f : (z < 0.0f ? 0.0f : z);
            break;
        }
        default: // Step_PosInvalid.
            out->pos[0] = out->pos[1] = out->pos[2] = std::numeric_limits<float>::infinity();
            break;
        }
    } else {
        switch (pos_) {
        case 1:
            for (int i = 0; i < 3; ++i) {
                out->pos[i] = static_cast<float>(static_cast<std::int8_t>(p[i])) * (1.0f / 128.0f);
            }
            break;
        case 2:
            for (int i = 0; i < 3; ++i) {
                out->pos[i] = static_cast<float>(Load<std::int16_t>(p + i * 2)) * (1.0f / 32768.0f);
            }
            break;
        case 3:
            for (int i = 0; i < 3; ++i) out->pos[i] = Load<float>(p + i * 4);
            break;
        default:
            out->pos[0] = out->pos[1] = out->pos[2] = std::numeric_limits<float>::infinity();
            break;
        }
    }
    return true;
}

bool ReadIndex(const MemoryView &mem, std::uint32_t address, std::uint32_t vtype, int i,
               std::uint32_t *out) {
    const auto index = static_cast<std::uint32_t>(i);
    switch ((vtype & kVtypeIdxMask) >> kVtypeIdxShift) {
    case 1: {
        const std::uint8_t *p = mem.Translate(address + index, 1);
        if (!p) return false;
        *out = *p;
        return true;
    }
    case 2: {
        const std::uint8_t *p = mem.Translate(address + index * 2, 2);
        if (!p) return false;
        *out = Load<std::uint16_t>(p);
        return true;
    }
    case 3: {
        const std::uint8_t *p = mem.Translate(address + index * 4, 4);
        if (!p) return false;
        *out = Load<std::uint32_t>(p);
        return true;
    }
    default:
        *out = index;
        return true;
    }
}

} // namespace mpo::ge
