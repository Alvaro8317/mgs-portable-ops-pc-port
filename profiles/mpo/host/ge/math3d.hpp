// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Math3D.{h,cpp} and Common/Math/fast/fast_matrix.h
// @ v1.20.4 (GPL-2.0-or-later).
//
// The vector and matrix helpers the transform unit, the lighting and the
// tessellator need, written as plain scalar code but with the evaluation
// order of PPSSPP's SSE paths (the x86-64 build the reference captures come
// from). Every sum is spelled out with parentheses so the result does not
// depend on the compiler; build with -ffp-contract=off (MSVC /fp:precise does
// not contract either).

#pragma once

#include <cmath>
#include <cstdint>

namespace mpo::ge {

struct Vec3f {
    float x = 0;
    float y = 0;
    float z = 0;
};

struct Vec4f {
    float x = 0;
    float y = 0;
    float z = 0;
    float w = 0;
};

// Vec3ByMatrix43 (SSE): (col0 * x + col1 * y) + (col2 * z + col3).
inline Vec3f Vec3ByMatrix43(const Vec3f &v, const float m[12]) {
    Vec3f out;
    out.x = (m[0] * v.x + m[3] * v.y) + (m[6] * v.z + m[9]);
    out.y = (m[1] * v.x + m[4] * v.y) + (m[7] * v.z + m[10]);
    out.z = (m[2] * v.x + m[5] * v.y) + (m[8] * v.z + m[11]);
    return out;
}

// Norm3ByMatrix43 (SSE): (col0 * x + col1 * y) + col2 * z.
inline Vec3f Norm3ByMatrix43(const Vec3f &v, const float m[12]) {
    Vec3f out;
    out.x = (m[0] * v.x + m[3] * v.y) + m[6] * v.z;
    out.y = (m[1] * v.x + m[4] * v.y) + m[7] * v.z;
    out.z = (m[2] * v.x + m[5] * v.y) + m[8] * v.z;
    return out;
}

// Vec3ByMatrix44 (SSE): (col0 * x + col1 * y) + (col2 * z + col3).
inline Vec4f Vec3ByMatrix44(const Vec3f &v, const float m[16]) {
    Vec4f out;
    out.x = (m[0] * v.x + m[4] * v.y) + (m[8] * v.z + m[12]);
    out.y = (m[1] * v.x + m[5] * v.y) + (m[9] * v.z + m[13]);
    out.z = (m[2] * v.x + m[6] * v.y) + (m[10] * v.z + m[14]);
    out.w = (m[3] * v.x + m[7] * v.y) + (m[11] * v.z + m[15]);
    return out;
}

// Matrix4ByMatrix4(out, a, b) = fast_matrix_mul_4x4(out, b, a): row i of the
// result is row i of `a` times `b`, accumulated left to right.
inline void Matrix4ByMatrix4(float out[16], const float a[16], const float b[16]) {
    for (int i = 0; i < 16; i += 4) {
        for (int r = 0; r < 4; ++r) {
            float sum = b[r] * a[i];
            sum = sum + b[4 + r] * a[i + 1];
            sum = sum + b[8 + r] * a[i + 2];
            sum = sum + b[12 + r] * a[i + 3];
            out[i + r] = sum;
        }
    }
}

inline void ConvertMatrix4x3To4x4(float out[16], const float m[12]) {
    out[0] = m[0];
    out[1] = m[1];
    out[2] = m[2];
    out[3] = 0.0f;
    out[4] = m[3];
    out[5] = m[4];
    out[6] = m[5];
    out[7] = 0.0f;
    out[8] = m[6];
    out[9] = m[7];
    out[10] = m[8];
    out[11] = 0.0f;
    out[12] = m[9];
    out[13] = m[10];
    out[14] = m[11];
    out[15] = 1.0f;
}

// Vec3<float>::Length (SSE): sqrt(x*x + (y*y + z*z)).
inline float Length(const Vec3f &v) {
    return std::sqrt(v.x * v.x + (v.y * v.y + v.z * v.z));
}

// Vec3<float>::NormalizeOr001 (in place, exact division). Returns the length.
inline float NormalizeOr001(Vec3f *v) {
    const float len = Length(*v);
    if (len == 0.0f) {
        v->z = 1.0f;
    } else {
        v->x = v->x / len;
        v->y = v->y / len;
        v->z = v->z / len;
    }
    return len;
}

// Dot33 (Lighting.cpp, SSE): (x + y) + z.
inline float Dot33(const Vec3f &a, const Vec3f &b) {
    return (a.x * b.x + a.y * b.y) + a.z * b.z;
}

// Dot43 (TransformUnit.cpp, SSE4.1): (ax + by) + (cz + d).
inline float Dot43(const Vec4f &a, const Vec3f &b) {
    return (a.x * b.x + a.y * b.y) + (a.z * b.z + a.w * 1.0f);
}

// Vec3<float>::Normalized / NormalizedOr001 with SSE use RSQRTPS, an
// approximation whose exact bits depend on the CPU vendor. Here the exact
// reciprocal square root is used instead; see docs/sessions/F12.md.
inline Vec3f NormalizedApprox(const Vec3f &v) {
    const float len2 = (v.x * v.x + v.y * v.y) + v.z * v.z;
    const float k = 1.0f / std::sqrt(len2);
    return Vec3f{v.x * k, v.y * k, v.z * k};
}

inline Vec3f NormalizedOr001Approx(const Vec3f &v) {
    Vec3f r = NormalizedApprox(v);
    // _mm_cmpunord_ps(result, vec): NaN lanes become (0, 0, 1).
    if (std::isnan(r.x) || std::isnan(v.x)) r.x = 0.0f;
    if (std::isnan(r.y) || std::isnan(v.y)) r.y = 0.0f;
    if (std::isnan(r.z) || std::isnan(v.z)) r.z = 1.0f;
    return r;
}

inline Vec3f Cross(const Vec3f &a, const Vec3f &b) {
    return Vec3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

} // namespace mpo::ge
