// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/MIPS/MIPSVFPUUtils.cpp and Common/Math/math_util.h
// @ v1.20.4 (fa50bb19, GPL-2.0-or-later).
//
// Changes from upstream, none of which alter results:
// - The tables are typed vectors filled with memcpy (no pointer punning) and
//   are loaded explicitly by load_tables() instead of lazily through the VFS.
// - Missing tables abort instead of falling back to the approximate
//   MIPSVFPUFallbacks.cpp code (CLAUDE.md §1.6: never fail silently).
// - vfpu_dot is the portable vfpu_dot_cpp reference; upstream's SSE2 variant
//   is documented as equivalent and the golden tests check it.
// - Integer expressions keep upstream's widths and wrap-arounds on purpose.
// - Constant NaN results go through exact_float(), so MSVC cannot quiet them
//   (see vfpu_math.hpp).

#include "vfpu/vfpu_math.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "vfpu/vfpu.hpp"

#ifdef _MSC_VER
// Unary minus on unsigned values (x & -q) is intentional, as upstream.
#pragma warning(disable : 4146)
#endif

static_assert(std::endian::native == std::endian::little,
              "The VFPU tables are little-endian; add a byteswap to load them on this host");

namespace mpo::vfpu {
namespace {

std::uint32_t clz32_nonzero(std::uint32_t value) {
    return static_cast<std::uint32_t>(std::countl_zero(value));
}

struct Tables {
    std::vector<std::uint32_t> sin_lut8192;             // 1025
    std::vector<std::int8_t> sin_lut_delta;             // [131072][2]
    std::vector<std::int16_t> sin_lut_interval_delta;   // 65537
    std::vector<std::uint8_t> sin_lut_exceptions;       // 86938
    std::vector<std::int8_t> sqrt_lut;                  // [131072][2]
    std::vector<std::int8_t> rsqrt_lut;                 // [131072][2]
    std::vector<std::uint32_t> exp2_lut65536;           // 128
    std::vector<std::uint8_t> exp2_lut;                 // [131072][2]
    std::vector<std::uint32_t> log2_lut65536;           // 129
    std::vector<std::uint32_t> log2_lut65536_quadratic; // 128
    std::vector<std::uint8_t> log2_lut;                 // [8][131072][2]
    std::vector<std::int32_t> asin_lut65536;            // [128][3]
    std::vector<std::uint64_t> asin_lut_deltas;         // 64681
    std::vector<std::uint16_t> asin_lut_indices;        // 399458
    std::vector<std::int8_t> rcp_lut;                   // [131072][2]
    bool loaded = false;
};

Tables g_tables;

template <typename T>
bool load_table(const std::filesystem::path &directory, const char *name, std::size_t expected_size,
                std::vector<T> &out, std::string *error) {
    const std::filesystem::path path = directory / (std::string(name) + ".dat");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != expected_size || expected_size % sizeof(T) != 0) {
        if (error) {
            *error = path.string() + ": " + std::to_string(bytes.size()) + " bytes, expected " +
                     std::to_string(expected_size);
        }
        return false;
    }
    out.resize(expected_size / sizeof(T));
    std::memcpy(out.data(), bytes.data(), expected_size);
    return true;
}

[[noreturn]] void missing_tables(const char *function) {
    std::fprintf(stderr,
                 "mpo_vfpu: %s needs the PPSSPP VFPU tables, but load_tables() was not called "
                 "or failed\n",
                 function);
    std::abort();
}

inline const Tables &tables(const char *function) {
    if (!g_tables.loaded) missing_tables(function);
    return g_tables;
}

inline float bits_to_float(std::uint32_t bits) {
    return std::bit_cast<float>(bits);
}
inline std::uint32_t float_to_bits(float f) {
    return std::bit_cast<std::uint32_t>(f);
}

// Note: PSP sin/cos output only has 22 significant binary digits.
inline std::uint32_t vfpu_sin_quantum(std::uint32_t x) {
    return x < 1u << 22 ? 1u : 1u << (32 - 22 - clz32_nonzero(x));
}

inline std::uint32_t vfpu_sin_truncate_bits(std::uint32_t x) {
    return x & -vfpu_sin_quantum(x);
}

std::uint32_t vfpu_sin_fixed(const Tables &t, std::uint32_t arg) {
    // Handle endpoints.
    if (arg == 0u) return 0u;
    if (arg == 0x00800000) return 0x10000000;
    // Get endpoints for 8192-wide interval.
    const std::uint32_t L = t.sin_lut8192[(arg >> 13) + 0];
    const std::uint32_t H = t.sin_lut8192[(arg >> 13) + 1];
    // Approximate endpoints for 64-wide interval via lerp.
    const std::uint32_t A = L + (((H - L) * (((arg >> 6) & 127) + 0)) >> 7);
    const std::uint32_t B = L + (((H - L) * (((arg >> 6) & 127) + 1)) >> 7);
    // Adjust endpoints from deltas, and increase working precision.
    const std::uint64_t a =
        (std::uint64_t(A) << 5) +
        std::uint64_t(t.sin_lut_delta[(arg >> 6) * 2 + 0]) * vfpu_sin_quantum(A);
    const std::uint64_t b =
        (std::uint64_t(B) << 5) +
        std::uint64_t(t.sin_lut_delta[(arg >> 6) * 2 + 1]) * vfpu_sin_quantum(B);
    // Compute approximation via lerp. Is off by at most 1 quantum.
    std::uint32_t v = std::uint32_t(((a * (64 - (arg & 63)) + b * (arg & 63)) >> 6) >> 5);
    v = vfpu_sin_truncate_bits(v);
    // Look up exceptions via binary search. vfpu_sin_lut_interval_delta stores
    // deltas from interval estimation.
    std::uint32_t lo = ((169u * ((arg >> 7) + 0)) >> 7) +
                       std::uint32_t(t.sin_lut_interval_delta[(arg >> 7) + 0]) + 16384u;
    std::uint32_t hi = ((169u * ((arg >> 7) + 1)) >> 7) +
                       std::uint32_t(t.sin_lut_interval_delta[(arg >> 7) + 1]) + 16384u;
    while (lo < hi) {
        const std::uint32_t m = (lo + hi) / 2;
        // vfpu_sin_lut_exceptions stores index&127 (the upper bits are those
        // of arg&-128), plus direction (0 for +1, and 128 for -1).
        const std::uint32_t b2 = t.sin_lut_exceptions[m];
        const std::uint32_t e = (arg & -128u) + (b2 & 127u);
        if (e == arg) {
            v += vfpu_sin_quantum(v) * (b2 >> 7 ? -1u : +1u);
            break;
        } else if (e < arg) {
            lo = m + 1;
        } else {
            hi = m;
        }
    }
    return v;
}

// Integer square root of 2^23*x (rounded to zero). Input is in
// 2^23 <= x < 2^25, and representable in float.
inline std::uint32_t isqrt23(std::uint32_t x) {
    return std::uint32_t(std::int32_t(std::sqrt(double(x) * 8388608.0)));
}

// Returns floating-point bitpattern.
std::uint32_t vfpu_sqrt_fixed(const Tables &t, std::uint32_t x) {
    // Endpoints of input.
    std::uint32_t lo = (x + 0u) & -64u;
    std::uint32_t hi = (x + 64u) & -64u;
    // Convert input to 9.23 fixed-point.
    lo = (lo >= 0x00400000u ? 4u * lo : 0x00800000u + 2u * lo);
    hi = (hi >= 0x00400000u ? 4u * hi : 0x00800000u + 2u * hi);
    // Estimate endpoints of output.
    const std::uint32_t A = 0x3F000000u + isqrt23(lo);
    const std::uint32_t B = 0x3F000000u + isqrt23(hi);
    // Apply deltas, and increase the working precision.
    const std::uint64_t a = (std::uint64_t(A) << 4) + std::uint64_t(t.sqrt_lut[(x >> 6) * 2 + 0]);
    const std::uint64_t b = (std::uint64_t(B) << 4) + std::uint64_t(t.sqrt_lut[(x >> 6) * 2 + 1]);
    std::uint32_t ret = std::uint32_t((a + (((b - a) * (x & 63)) >> 6)) >> 4);
    // Truncate lower 2 bits.
    ret &= -4u;
    return ret;
}

// Returns floor(2^33/sqrt(x)), for 2^22 <= x < 2^24.
inline std::uint32_t rsqrt_floor22(std::uint32_t x) {
    // Verified correct in all rounding directions, by exhaustive search.
    return std::uint32_t(8589934592.0 / std::sqrt(double(x))); // 0x1p33
}

// Returns floating-point bitpattern.
std::uint32_t vfpu_rsqrt_fixed(const Tables &t, std::uint32_t x) {
    // Endpoints of input.
    std::uint32_t lo = (x + 0u) & -64u;
    std::uint32_t hi = (x + 64u) & -64u;
    // Convert input to 10.22 fixed-point.
    lo = (lo >= 0x00400000u ? 2u * lo : 0x00400000u + lo);
    hi = (hi >= 0x00400000u ? 2u * hi : 0x00400000u + hi);
    // Estimate endpoints of output.
    const std::uint32_t A = 0x3E800000u + 4u * rsqrt_floor22(lo);
    const std::uint32_t B = 0x3E800000u + 4u * rsqrt_floor22(hi);
    // Apply deltas, and increase the working precision.
    const std::uint64_t a = (std::uint64_t(A) << 4) + std::uint64_t(t.rsqrt_lut[(x >> 6) * 2 + 0]);
    const std::uint64_t b = (std::uint64_t(B) << 4) + std::uint64_t(t.rsqrt_lut[(x >> 6) * 2 + 1]);
    // Evaluate via lerp.
    std::uint32_t ret = std::uint32_t((a + (((b - a) * (x & 63)) >> 6)) >> 4);
    // Truncate lower 2 bits.
    ret &= -4u;
    return ret;
}

inline std::uint32_t vfpu_asin_quantum(std::uint32_t x) {
    return x < 1u << 23 ? 1u : 1u << (32 - 23 - clz32_nonzero(x));
}

inline std::uint32_t vfpu_asin_truncate_bits(std::uint32_t x) {
    return x & -vfpu_asin_quantum(x);
}

// Input is fixed 9.23, output is fixed 2.30.
inline std::uint32_t vfpu_asin_approx(const Tables &t, std::uint32_t x) {
    const std::int32_t *C = &t.asin_lut65536[(x >> 16) * 3];
    x &= 0xFFFFu;
    return vfpu_asin_truncate_bits(std::uint32_t(
        ((((((std::int64_t(C[2]) * x) >> 16) + std::int64_t(C[1])) * x) >> 16) + C[0])));
}

// Input is fixed 9.23, output is fixed 2.30.
std::uint32_t vfpu_asin_fixed(const Tables &t, std::uint32_t x) {
    if (x == 0u) return 0u;
    if (x == 1u << 23) return 1u << 30;
    const std::uint32_t ret = vfpu_asin_approx(t, x);
    const std::uint32_t index = t.asin_lut_indices[x / 21u];
    const std::uint64_t deltas = t.asin_lut_deltas[index];
    return ret + (3u - std::uint32_t((deltas >> (3u * (x % 21u))) & 7u)) * vfpu_asin_quantum(ret);
}

inline std::uint32_t vfpu_exp2_approx(const Tables &t, std::uint32_t x) {
    if (x == 0x00800000u) return 0x00800000u;
    const std::uint32_t a = t.exp2_lut65536[x >> 16];
    x &= 0x0000FFFFu;
    const std::uint32_t b = std::uint32_t(((2977151143ull * x) >> 23) +
                                          ((1032119999ull * (std::uint64_t(x) * x)) >> 46));
    return (a + std::uint32_t((std::uint64_t(a + (1u << 23)) * std::uint64_t(b)) >> 32)) & -4u;
}

std::uint32_t vfpu_exp2_fixed(const Tables &t, std::uint32_t x) {
    if (x == 0u) return 0u;
    if (x == 0x00800000u) return 0x00800000u;
    const std::uint32_t A = vfpu_exp2_approx(t, (x) & -64u);
    const std::uint32_t B = vfpu_exp2_approx(t, (x + 64) & -64u);
    // Upstream computes these sums in 32 bits (A is u32, the table is u8);
    // keep the same wrap-around before widening.
    const std::uint64_t a = std::uint32_t((A << 4) + t.exp2_lut[(x >> 6) * 2 + 0] - 64u);
    const std::uint64_t b = std::uint32_t((B << 4) + t.exp2_lut[(x >> 6) * 2 + 1] - 64u);
    std::uint32_t y = std::uint32_t((a + (((b - a) * (x & 63)) >> 6)) >> 4);
    y &= -4u;
    return y;
}

// Input fixed 9.23, output fixed 10.22. Returns log2(1+x).
inline std::uint32_t vfpu_log2_approx(const Tables &t, std::uint32_t x) {
    // x can be exactly 0x00800000 (the "+ 64" endpoint in vfpu_log2), where
    // upstream reads one entry past both tables. Those entries are then
    // multiplied by x & 0xFFFF == 0, so reading 0 instead gives the same
    // result without the out-of-bounds access.
    const std::size_t index = x >> 16;
    const std::uint32_t a = t.log2_lut65536[index];
    const std::uint32_t b = index + 1 < t.log2_lut65536.size() ? t.log2_lut65536[index + 1] : 0u;
    const std::uint32_t c =
        index < t.log2_lut65536_quadratic.size() ? t.log2_lut65536_quadratic[index] : 0u;
    x &= 0xFFFFu;
    std::uint64_t ret = std::uint64_t(a) * (0x10000u - x) + std::uint64_t(b) * x;
    const std::uint64_t d = (std::uint64_t(c) * x * (0x10000u - x)) >> 40;
    ret += d;
    return std::uint32_t(ret >> 16);
}

inline std::uint32_t vfpu_rcp_approx(std::uint32_t i) {
    return 0x3E800000u + (std::uint32_t((1ull << 47) / ((1ull << 23) + i)) & -4u);
}

int is_even(float d) {
    float int_part;
    std::modf(d / 2.0f, &int_part);
    return 2.0f * int_part == d;
}

} // namespace

void State::reset() {
    for (float &value : v) value = 0.0f;
    // MIPSState::Reset() in PPSSPP Core/MIPS/MIPS.cpp.
    ctrl[kCtrlSPrefix] = 0xE4;
    ctrl[kCtrlTPrefix] = 0xE4;
    ctrl[kCtrlDPrefix] = 0;
    ctrl[kCtrlCc] = 0x3F;
    ctrl[kCtrlInf4] = 0;
    ctrl[kCtrlRsv5] = 0;
    ctrl[kCtrlRsv6] = 0;
    ctrl[kCtrlRev] = 0x7772CEABu;
    ctrl[kCtrlRcx0 + 0] = 0x3F800001u;
    ctrl[kCtrlRcx0 + 1] = 0x3F800002u;
    ctrl[kCtrlRcx0 + 2] = 0x3F800004u;
    ctrl[kCtrlRcx0 + 3] = 0x3F800008u;
    ctrl[kCtrlRcx0 + 4] = 0x3F800000u;
    ctrl[kCtrlRcx0 + 5] = 0x3F800000u;
    ctrl[kCtrlRcx0 + 6] = 0x3F800000u;
    ctrl[kCtrlRcx0 + 7] = 0x3F800000u;
}

bool load_tables(const std::filesystem::path &directory, std::string *error) {
    Tables t;
    const bool ok =
        load_table(directory, "vfpu_sin_lut8192", 4100, t.sin_lut8192, error) &&
        load_table(directory, "vfpu_sin_lut_delta", 262144, t.sin_lut_delta, error) &&
        load_table(directory, "vfpu_sin_lut_interval_delta", 131074, t.sin_lut_interval_delta,
                   error) &&
        load_table(directory, "vfpu_sin_lut_exceptions", 86938, t.sin_lut_exceptions, error) &&
        load_table(directory, "vfpu_sqrt_lut", 262144, t.sqrt_lut, error) &&
        load_table(directory, "vfpu_rsqrt_lut", 262144, t.rsqrt_lut, error) &&
        load_table(directory, "vfpu_exp2_lut65536", 512, t.exp2_lut65536, error) &&
        load_table(directory, "vfpu_exp2_lut", 262144, t.exp2_lut, error) &&
        load_table(directory, "vfpu_log2_lut65536", 516, t.log2_lut65536, error) &&
        load_table(directory, "vfpu_log2_lut65536_quadratic", 512, t.log2_lut65536_quadratic,
                   error) &&
        load_table(directory, "vfpu_log2_lut", 2097152, t.log2_lut, error) &&
        load_table(directory, "vfpu_asin_lut65536", 1536, t.asin_lut65536, error) &&
        load_table(directory, "vfpu_asin_lut_deltas", 517448, t.asin_lut_deltas, error) &&
        load_table(directory, "vfpu_asin_lut_indices", 798916, t.asin_lut_indices, error) &&
        load_table(directory, "vfpu_rcp_lut", 262144, t.rcp_lut, error);
    if (!ok) return false;
    t.loaded = true;
    g_tables = std::move(t);
    return true;
}

bool tables_loaded() {
    return g_tables.loaded;
}

float vfpu_sin(float x) {
    const Tables &t = tables("vfpu_sin");
    std::uint32_t bits = float_to_bits(x);
    std::uint32_t sign = bits & 0x80000000u;
    const std::uint32_t exponent = (bits >> 23) & 0xFFu;
    std::uint32_t significand = (bits & 0x007FFFFFu) | 0x00800000u;
    if (exponent == 0xFFu) {
        // NOTE: this bitpattern is a signaling NaN on x86.
        return exact_float(sign ^ 0x7F800001u);
    }
    if (exponent < 0x7Fu) {
        if (exponent < 0x7Fu - 23u)
            significand = 0u;
        else
            significand >>= (0x7F - exponent);
    } else if (exponent > 0x7Fu) {
        // There is weirdness for large exponents.
        if (exponent - 0x7Fu >= 25u && exponent - 0x7Fu < 32u)
            significand = 0u;
        else if ((exponent & 0x9Fu) == 0x9Fu)
            significand = 0u;
        else
            significand <<= ((exponent - 0x7Fu) & 31);
    }
    sign ^= ((significand << 7) & 0x80000000u);
    significand &= 0x00FFFFFFu;
    if (significand > 0x00800000u) significand = 0x01000000u - significand;
    const std::uint32_t ret = vfpu_sin_fixed(t, significand);
    return (sign ? -1.0f : +1.0f) * float(std::int32_t(ret)) * 3.7252903e-09f; // 0x1p-28f
}

float vfpu_cos(float x) {
    const Tables &t = tables("vfpu_cos");
    std::uint32_t bits = float_to_bits(x);
    bits &= 0x7FFFFFFFu;
    std::uint32_t sign = 0u;
    const std::uint32_t exponent = (bits >> 23) & 0xFFu;
    std::uint32_t significand = (bits & 0x007FFFFFu) | 0x00800000u;
    if (exponent == 0xFFu) {
        // sign is always 0 here, so this is a constant signaling NaN.
        return exact_float(0x7F800001u);
    }
    if (exponent < 0x7Fu) {
        if (exponent < 0x7Fu - 23u)
            significand = 0u;
        else
            significand >>= (0x7F - exponent);
    } else if (exponent > 0x7Fu) {
        if (exponent - 0x7Fu >= 25u && exponent - 0x7Fu < 32u)
            significand = 0u;
        else if ((exponent & 0x9Fu) == 0x9Fu)
            significand = 0u;
        else
            significand <<= ((exponent - 0x7Fu) & 31);
    }
    sign ^= ((significand << 7) & 0x80000000u);
    significand &= 0x00FFFFFFu;
    if (significand >= 0x00800000u) {
        significand = 0x01000000u - significand;
        sign ^= 0x80000000u;
    }
    const std::uint32_t ret = vfpu_sin_fixed(t, 0x00800000u - significand);
    return (sign ? -1.0f : +1.0f) * float(std::int32_t(ret)) * 3.7252903e-09f; // 0x1p-28f
}

void vfpu_sincos(float a, float &s, float &c) {
    s = vfpu_sin(a);
    c = vfpu_cos(a);
}

float vfpu_sqrt(float x) {
    const Tables &t = tables("vfpu_sqrt");
    std::uint32_t bits = float_to_bits(x);
    if ((bits & 0x7FFFFFFFu) <= 0x007FFFFFu) {
        // Denormals (and zeroes) get +0, regardless of sign.
        return +0.0f;
    }
    if (bits >> 31) {
        // Other negatives get NaN.
        return exact_float(0x7F800001u);
    }
    if ((bits >> 23) == 255u) {
        // Inf/NaN gets Inf/NaN.
        return exact_float(0x7F800000u + ((bits & 0x007FFFFFu) != 0u));
    }
    const std::int32_t exponent = std::int32_t(bits >> 23) - 127;
    // Bottom bit of exponent (inverted) + significand (except bottom bit).
    const std::uint32_t index = ((bits + 0x00800000u) >> 1) & 0x007FFFFFu;
    bits = vfpu_sqrt_fixed(t, index);
    bits += std::uint32_t(exponent >> 1) << 23;
    return bits_to_float(bits);
}

float vfpu_rsqrt(float x) {
    const Tables &t = tables("vfpu_rsqrt");
    std::uint32_t bits = float_to_bits(x);
    if ((bits & 0x7FFFFFFFu) <= 0x007FFFFFu) {
        // Denormals (and zeroes) get inf of the same sign.
        return bits_to_float(0x7F800000u | (bits & 0x80000000u));
    }
    if (bits >> 31) {
        // Other negatives get negative NaN.
        return exact_float(0xFF800001u);
    }
    if ((bits >> 23) == 255u) {
        // inf gets 0, NaN gets NaN.
        return bits_to_float((bits & 0x007FFFFFu) ? 0x7F800001u : 0u);
    }
    const std::int32_t exponent = std::int32_t(bits >> 23) - 127;
    // Bottom bit of exponent (inverted) + significand (except bottom bit).
    const std::uint32_t index = ((bits + 0x00800000u) >> 1) & 0x007FFFFFu;
    bits = vfpu_rsqrt_fixed(t, index);
    bits -= std::uint32_t(exponent >> 1) << 23;
    return bits_to_float(bits);
}

float vfpu_asin(float x) {
    const Tables &t = tables("vfpu_asin");
    std::uint32_t bits = float_to_bits(x);
    const std::uint32_t sign = bits & 0x80000000u;
    bits = bits & 0x7FFFFFFFu;
    if (bits > 0x3F800000u) {
        return exact_float(0x7F800001u ^ sign);
    }
    bits = vfpu_asin_fixed(t, std::uint32_t(std::int32_t(std::fabs(x) * 8388608.0f))); // 0x1p23
    x = float(std::int32_t(bits)) * 9.31322574615478515625e-10f;                       // 0x1p-30
    if (sign) x = -x;
    return x;
}

float vfpu_exp2(float x) {
    const Tables &t = tables("vfpu_exp2");
    std::uint32_t bits = float_to_bits(x);
    if ((bits & 0x7FFFFFFFu) <= 0x007FFFFFu) {
        // Denormals are treated as 0.
        return 1.0f;
    }
    if (x != x) {
        // NaN gets NaN.
        return exact_float(0x7F800001u);
    }
    if (x <= -126.0f) {
        // Small numbers get 0 (exp2(-126) is smallest positive non-denormal).
        // But yes, -126.0f produces +0.0f.
        return 0.0f;
    }
    if (x >= +128.0f) {
        // Large numbers get infinity.
        return bits_to_float(0x7F800000u);
    }
    std::int32_t ibits = std::int32_t(x * 0x1p23f);
    if (x < 0.0f) --ibits; // Yes, really.
    // Upstream adds these as int32; the sum never overflows for |x| < 128,
    // and unsigned arithmetic gives the same bits.
    bits = 0x3F800000u + (std::uint32_t(ibits) & 0xFF800000u) +
           vfpu_exp2_fixed(t, std::uint32_t(ibits) & 0x007FFFFFu);
    return bits_to_float(bits);
}

float vfpu_rexp2(float x) {
    return vfpu_exp2(-x);
}

// Matches PSP output on all known values.
float vfpu_log2(float x) {
    const Tables &t = tables("vfpu_log2");
    std::uint32_t bits = float_to_bits(x);
    if ((bits & 0x7FFFFFFFu) <= 0x007FFFFFu) {
        // Denormals (and zeroes) get -inf.
        return bits_to_float(0xFF800000u);
    }
    if (bits & 0x80000000u) {
        // Other negatives get NaN.
        return exact_float(0x7F800001u);
    }
    if ((bits >> 23) == 255u) {
        // NaN gets NaN, +inf gets +inf.
        return exact_float(0x7F800000u + ((bits & 0x007FFFFFu) != 0));
    }
    const std::uint32_t e = (bits & 0x7F800000u) - 0x3F800000u;
    const std::uint32_t i = bits & 0x007FFFFFu;
    if (e >> 31 && i >= 0x007FFE00u) {
        // Process 1-2^{-14}<=x*2^n<1 (for n>0) separately, since the table
        // doesn't give the right answer.
        const float c = float(std::int32_t(~e) >> 23);
        // Note: if c is 0 the sign of -0 output is correct.
        return i < 0x007FFEF7u ? // 1-265*2^{-24}
                   -3.05175781e-05f - c
                               : -0.0f - c;
    }
    const int d = (e < 0x01000000u ? 0 : 8 - int(clz32_nonzero(e)) - int(e >> 31));
    const std::uint32_t q = 1u << d;
    const std::uint32_t A = vfpu_log2_approx(t, (i) & -64u) & -q;
    const std::uint32_t B = vfpu_log2_approx(t, (i + 64) & -64u) & -q;
    const std::size_t lut_index = (std::size_t(d) * 131072u + (i >> 6)) * 2u;
    const std::uint64_t a = (A << 6) + (std::uint64_t(t.log2_lut[lut_index + 0]) - 80ull) * q;
    const std::uint64_t b = (B << 6) + (std::uint64_t(t.log2_lut[lut_index + 1]) - 80ull) * q;
    std::uint32_t v = std::uint32_t((a + (((b - a) * (i & 63)) >> 6)) >> 6);
    v &= -q;
    bits = e ^ (2u * v);
    return float(std::int32_t(bits)) * 1.1920928955e-7f; // 0x1p-23f
}

float vfpu_rcp(float x) {
    const Tables &t = tables("vfpu_rcp");
    std::uint32_t bits = float_to_bits(x);
    const std::uint32_t s = bits & 0x80000000u;
    const std::uint32_t e = bits & 0x7F800000u;
    const std::uint32_t i = bits & 0x007FFFFFu;
    if ((bits & 0x7FFFFFFFu) > 0x7E800000u) {
        return bits_to_float(e == 0x7F800000u && i ? s ^ 0x7F800001u : s);
    }
    if (e == 0u) {
        return bits_to_float(s ^ 0x7F800000u);
    }
    const std::uint32_t A = vfpu_rcp_approx((i) & -64u);
    const std::uint32_t B = vfpu_rcp_approx((i + 64) & -64u);
    const std::uint64_t a =
        (std::uint64_t(A) << 6) + std::uint64_t(t.rcp_lut[(i >> 6) * 2 + 0]) * 4u;
    const std::uint64_t b =
        (std::uint64_t(B) << 6) + std::uint64_t(t.rcp_lut[(i >> 6) * 2 + 1]) * 4u;
    std::uint32_t v = std::uint32_t((a + (((b - a) * (i & 63)) >> 6)) >> 6);
    v &= -4u;
    bits = s + (0x3F800000u - e) + v;
    return bits_to_float(bits);
}

// Reference C++ version (vfpu_dot_cpp upstream).
float vfpu_dot(const float a[4], const float b[4]) {
    constexpr int kExtraBits = 2;
    std::int32_t exps[4];
    std::int32_t mants[4];
    std::int32_t signs[4];
    std::int32_t max_exp = 0;
    std::int32_t last_inf = -1;

    const auto get_uexp = [](std::uint32_t x) { return std::int32_t((x >> 23) & 0xFF); };
    // Note: this returns the hidden 1.
    const auto get_mant = [](std::uint32_t x) {
        return std::int32_t((x & 0x007FFFFF) | 0x00800000);
    };
    const auto get_sign = [](std::uint32_t x) { return std::int32_t(x & 0x80000000u); };

    for (int i = 0; i < 4; i++) {
        const std::uint32_t src0 = float_to_bits(a[i]);
        const std::uint32_t src1 = float_to_bits(b[i]);

        const std::int32_t aexp = get_uexp(src0);
        const std::int32_t bexp = get_uexp(src1);
        const std::int32_t amant = get_mant(src0) << kExtraBits;
        const std::int32_t bmant = get_mant(src1) << kExtraBits;

        exps[i] = aexp + bexp - 127;
        if (aexp == 255) {
            // INF * 0 = NAN
            if ((src0 & 0x007FFFFF) != 0 || bexp == 0) return exact_float(0x7F800001u);
            mants[i] = get_mant(0) << kExtraBits;
            exps[i] = 255;
        } else if (bexp == 255) {
            if ((src1 & 0x007FFFFF) != 0 || aexp == 0) return exact_float(0x7F800001u);
            mants[i] = get_mant(0) << kExtraBits;
            exps[i] = 255;
        } else {
            const std::uint64_t adjust = std::uint64_t(amant) * std::uint64_t(bmant);
            mants[i] = std::int32_t((adjust >> (23 + kExtraBits)) & 0x7FFFFFFF);
        }
        signs[i] = get_sign(src0) ^ get_sign(src1);

        if (exps[i] > max_exp) max_exp = exps[i];
        if (exps[i] >= 255) {
            // Infinity minus infinity is not a real number.
            if (last_inf != -1 && signs[i] != last_inf) return exact_float(0x7F800001u);
            last_inf = signs[i];
        }
    }

    std::int32_t mant_sum = 0;
    for (int i = 0; i < 4; i++) {
        const int exp = max_exp - exps[i];
        if (exp >= 32) {
            mants[i] = 0;
        } else {
            mants[i] >>= exp;
        }
        if (signs[i]) mants[i] = -mants[i];
        mant_sum += mants[i];
    }

    std::uint32_t sign_sum = 0;
    if (mant_sum < 0) {
        sign_sum = 0x80000000;
        mant_sum = -mant_sum;
    }

    // Truncate off the extra bits now. We want to zero them for rounding purposes.
    mant_sum >>= kExtraBits;

    if (mant_sum == 0 || max_exp <= 0) return 0.0f;

    std::int8_t shift = static_cast<std::int8_t>(clz32_nonzero(std::uint32_t(mant_sum)) - 8);
    if (shift < 0) {
        // Round to even if we'd shift away a 0.5.
        const std::uint32_t round_bit = 1u << (-shift - 1);
        if ((std::uint32_t(mant_sum) & round_bit) && (std::uint32_t(mant_sum) & (round_bit << 1))) {
            mant_sum += std::int32_t(round_bit);
            shift = static_cast<std::int8_t>(clz32_nonzero(std::uint32_t(mant_sum)) - 8);
        } else if ((std::uint32_t(mant_sum) & round_bit) &&
                   (std::uint32_t(mant_sum) & (round_bit - 1))) {
            mant_sum += std::int32_t(round_bit);
            shift = static_cast<std::int8_t>(clz32_nonzero(std::uint32_t(mant_sum)) - 8);
        }
        mant_sum >>= -shift;
        max_exp += -shift;
    } else {
        mant_sum <<= shift;
        max_exp -= shift;
    }

    if (max_exp >= 255) {
        max_exp = 255;
        mant_sum = 0;
    } else if (max_exp <= 0) {
        return 0.0f;
    }

    return bits_to_float(sign_sum | (std::uint32_t(max_exp) << 23) |
                         (std::uint32_t(mant_sum) & 0x007FFFFFu));
}

// The code below attempts to exactly match behaviour of PSP's vrnd
// instructions. See https://github.com/hrydgard/ppsspp/issues/16946.
void vrnd_init_default(std::uint32_t *rcx) {
    rcx[0] = 0x00000001;
    rcx[1] = 0x00000002;
    rcx[2] = 0x00000004;
    rcx[3] = 0x00000008;
    rcx[4] = 0x00000000;
    rcx[5] = 0x00000000;
    rcx[6] = 0x00000000;
    rcx[7] = 0x00000000;
}

void vrnd_init(std::uint32_t seed, std::uint32_t *rcx) {
    for (int i = 0; i < 8; ++i) {
        rcx[i] = 0x3F800000u |                          // 1.0f mask.
                 ((seed >> ((i / 4) * 16)) & 0xFFFFu) | // lower or upper half of the seed.
                 (((seed >> (4 * i)) & 0xF) << 16);     // remaining nibble.
    }
}

std::uint32_t vrnd_generate(std::uint32_t *rcx) {
    // The actual RNG state appears to be 5 parts (32-bit each) stored into
    // the registers as follows:
    std::uint32_t A = (rcx[0] & 0xFFFFu) | (rcx[4] << 16);
    std::uint32_t B = (rcx[1] & 0xFFFFu) | (rcx[5] << 16);
    std::uint32_t C = (rcx[2] & 0xFFFFu) | (rcx[6] << 16);
    std::uint32_t D = (rcx[3] & 0xFFFFu) | (rcx[7] << 16);
    std::uint32_t E = (((rcx[0] >> 16) & 0xF) << 0) | (((rcx[1] >> 16) & 0xF) << 4) |
                      (((rcx[2] >> 16) & 0xF) << 8) | (((rcx[3] >> 16) & 0xF) << 12) |
                      (((rcx[4] >> 16) & 0xF) << 16) | (((rcx[5] >> 16) & 0xF) << 20) |
                      (((rcx[6] >> 16) & 0xF) << 24) | (((rcx[7] >> 16) & 0xF) << 28);
    // LCG with classic parameters.
    A = 69069u * A + 1u;
    // Xorshift, with classic parameters (Marsaglia, "Xorshift RNGs", "xor").
    B ^= B << 13;
    B ^= B >> 17;
    B ^= B << 5;
    // Sequence similar to Pell numbers, with different starting values and an
    // occasional increment (E).
    const std::uint32_t t = 2u * D + C + E;
    // The details of how E is set are somewhat of a guess upstream.
    E = std::uint32_t((std::uint64_t(C) + std::uint64_t(D >> 1) + std::uint64_t(E)) >> 32);
    C = D;
    D = t;
    rcx[0] = 0x3F800000u | (((E >> 0) & 0xF) << 16) | (A & 0xFFFFu);
    rcx[1] = 0x3F800000u | (((E >> 4) & 0xF) << 16) | (B & 0xFFFFu);
    rcx[2] = 0x3F800000u | (((E >> 8) & 0xF) << 16) | (C & 0xFFFFu);
    rcx[3] = 0x3F800000u | (((E >> 12) & 0xF) << 16) | (D & 0xFFFFu);
    rcx[4] = 0x3F800000u | (((E >> 16) & 0xF) << 16) | (A >> 16);
    rcx[5] = 0x3F800000u | (((E >> 20) & 0xF) << 16) | (B >> 16);
    rcx[6] = 0x3F800000u | (((E >> 24) & 0xF) << 16) | (C >> 16);
    rcx[7] = 0x3F800000u | (((E >> 28) & 0xF) << 16) | (D >> 16);
    return A + B + D;
}

float float16_to_float32(std::uint16_t l) {
    const unsigned int sign = (l >> 15) & 0x1;
    int exponent = (l >> 10) & 0x1F;
    unsigned int fraction = l & 0x3FF;

    if (exponent == 0x1F) {
        return bits_to_float((sign << 31) | (255u << 23) | fraction);
    }
    if (exponent == 0 && fraction == 0) {
        return sign == 1 ? -0.0f : 0.0f;
    }
    if (exponent == 0) {
        do {
            fraction <<= 1;
            exponent--;
        } while (!(fraction & (0x3FF + 1)));
        fraction &= 0x3FF;
    }
    // Convert to 32-bit single-precision IEEE754.
    return bits_to_float((sign << 31) | (std::uint32_t(exponent + 112) << 23) | (fraction << 13));
}

// half_to_float_fast5 (ryg): https://fgiesen.wordpress.com/2012/03/28/half-to-float-done-quic/
float expand_half(std::uint16_t half) {
    const float magic = bits_to_float((127 + (127 - 15)) << 23);
    const float was_infnan = bits_to_float((127 + 16) << 23);
    std::uint32_t o = std::uint32_t(half & 0x7FFF) << 13; // exponent/mantissa bits
    float of = bits_to_float(o) * magic;                  // exponent adjust
    o = float_to_bits(of);
    if (of >= was_infnan) { // make sure Inf/NaN survive (retain the low bits)
        o = (255u << 23) | (half & 0x03FFu);
    }
    o |= std::uint32_t(half & 0x8000) << 16; // sign bit
    return bits_to_float(o);
}

// float_to_half_fast3 (ryg): https://gist.github.com/rygorous/2156668
std::uint16_t shrink_to_half(float full) {
    constexpr std::uint32_t f32infty = 255u << 23;
    constexpr std::uint32_t f16infty = 31u << 23;
    const float magic = bits_to_float(15u << 23);
    constexpr std::uint32_t sign_mask = 0x80000000u;
    constexpr std::uint32_t round_mask = ~0xFFFu;

    std::uint32_t f = float_to_bits(full);
    std::uint16_t o = 0;
    const std::uint32_t sign = f & sign_mask;
    f ^= sign;
    if (f >= f32infty) { // Inf or NaN (all exponent bits set)
        o = static_cast<std::uint16_t>((f > f32infty) ? (0x7E00 | (f & 0x3FF)) : 0x7C00);
    } else { // (De)normalized number or zero
        f &= round_mask;
        f = float_to_bits(bits_to_float(f) * magic);
        f -= round_mask;
        if (f > f16infty) f = f16infty; // Clamp to signed infinity if overflowed
        o = static_cast<std::uint16_t>(f >> 13);
    }
    o = static_cast<std::uint16_t>(o | (sign >> 16));
    return o;
}

double round_ieee_754(double d) {
    const float i = float(std::floor(d));
    d -= i;
    if (d < 0.5f) return i;
    if (d > 0.5f) return i + 1.0f;
    if (is_even(i)) return i;
    return i + 1.0f;
}

} // namespace mpo::vfpu
