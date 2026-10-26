// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scalar VFPU math that reproduces the PSP bit for bit (sin, cos, sqrt, rsqrt,
// asin, exp2, log2, rcp, the accurate dot product, vrnd and half floats).
//
// Ported from PPSSPP Core/MIPS/MIPSVFPUUtils.{h,cpp} and Common/Math/math_util.h
// @ v1.20.4 (fa50bb19, GPL-2.0-or-later).

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace mpo::vfpu {

// Table-driven, exact PSP results. They abort if load_tables() was not called.
float vfpu_sin(float x);
float vfpu_cos(float x);
void vfpu_sincos(float a, float &s, float &c);
float vfpu_asin(float x);
float vfpu_sqrt(float x);
float vfpu_rsqrt(float x);
float vfpu_exp2(float x);
float vfpu_rexp2(float x);
float vfpu_log2(float x);
float vfpu_rcp(float x);

// PSP-accurate 4-element dot product (pure integer emulation, no tables).
float vfpu_dot(const float a[4], const float b[4]);

// vrnds/vrndi/vrndf* generator over the eight RCX control registers.
void vrnd_init_default(std::uint32_t *rcx);
void vrnd_init(std::uint32_t seed, std::uint32_t *rcx);
std::uint32_t vrnd_generate(std::uint32_t *rcx);

// vfim (Float16ToFloat32) and vh2f/vf2h (ExpandHalf/ShrinkToHalf).
float float16_to_float32(std::uint16_t half);
float expand_half(std::uint16_t half);
std::uint16_t shrink_to_half(float full);

// Saturation used by the D prefix: NaN is preserved, -0.0 becomes +0.0 when
// min is +0.0.
inline float vfpu_clamp(float v, float min, float max) {
    return v >= max ? max : (v <= min ? min : v);
}

inline bool my_isinf(float f) {
    const std::uint32_t u = std::bit_cast<std::uint32_t>(f);
    return u == 0x7F800000u || u == 0xFF800000u;
}

inline bool my_isnan(float f) {
    const std::uint32_t u = std::bit_cast<std::uint32_t>(f);
    return (u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu) != 0;
}

inline bool my_isnanorinf(float f) {
    return (std::bit_cast<std::uint32_t>(f) & 0x7F800000u) == 0x7F800000u;
}

// The NaN that an x86 SSE addss/mulss returns, with the operands in source
// order: the first NaN operand, made quiet; or the default NaN 0xFFC00000 when
// the operation itself is invalid (inf - inf, 0 * inf).
inline float sse_nan(float a, float b) {
    if (my_isnan(a)) return std::bit_cast<float>(std::bit_cast<std::uint32_t>(a) | 0x00400000u);
    if (my_isnan(b)) return std::bit_cast<float>(std::bit_cast<std::uint32_t>(b) | 0x00400000u);
    return std::bit_cast<float>(0xFFC00000u);
}

// a + b and a * b with the NaN pinned as above. The compiler may commute a
// plain + or *, which changes which NaN comes out; PPSSPP's interpreter has
// that problem too, so its NaN payloads depend on how it was built (see
// difftest::Policy). Generated code that wants the same NaNs as mpo_vfpu must
// use these as well.
inline float fadd(float a, float b) {
    const float r = a + b;
    return my_isnan(r) ? sse_nan(a, b) : r;
}

inline float fmul(float a, float b) {
    const float r = a * b;
    return my_isnan(r) ? sse_nan(a, b) : r;
}

// --- What PPSSPP's interpreter actually computes (hook targets for F07b) ---
//
// In PPSSPP v1.20.4 USE_VFPU_SQRT and USE_VFPU_DOT are false, so vrsq/vsqrt
// use the host's correctly rounded sqrt (not vfpu_rsqrt/vfpu_sqrt), and
// vdot/vhdp/vfad/vavg/vtfm/vmmul sum in plain float, left to right (vfpu_dot
// is only used by vmmul under the MoreAccurateVMMUL compat flag, which is off
// for this game). vsin/vcos/vasin/vexp2/vrexp2/vlog2/vrcp and vnrcp/vnsin
// (negated) do use the table functions above.
inline float ppsspp_vrsq(float x) {
    return 1.0f / std::sqrt(x);
}
inline float ppsspp_vsqrt(float x) {
    return std::fabs(std::sqrt(x));
}

// vdot.q as PPSSPP's interpreter does it: 0 + s0*t0 + s1*t1 + s2*t2 + s3*t3
// (unused lanes are zero after the prefixes), with pinned NaNs.
inline float ppsspp_dot4(const float *s, const float *t) {
    float d = 0.0f;
    for (int i = 0; i < 4; i++) d = fadd(d, fmul(s[i], t[i]));
    return d;
}

// Rounds *.5 to the closest even number (PPSSPP round_ieee_754).
double round_ieee_754(double d);

} // namespace mpo::vfpu
