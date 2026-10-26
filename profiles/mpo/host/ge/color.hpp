// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Common/Data/Convert/ColorConv.h and GPU/Math3D.h
// @ v1.20.4 (GPL-2.0-or-later): pixel format conversions and the small
// integer vectors the software renderer works with.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mpo::ge {

inline std::uint8_t Convert4To8(std::uint32_t v) {
    return static_cast<std::uint8_t>(((v << 4) | v) & 0xFF);
}

inline std::uint32_t RGBA4444ToRGBA8888(std::uint16_t src) {
    const std::uint32_t r = (src & 0x000Fu) << 0;
    const std::uint32_t g = (src & 0x00F0u) << 4;
    const std::uint32_t b = (src & 0x0F00u) << 8;
    const std::uint32_t a = (src & 0xF000u) << 12;
    const std::uint32_t c = r | g | b | a;
    return c | (c << 4);
}

inline std::uint32_t RGBA5551ToRGBA8888(std::uint16_t src) {
    const std::uint32_t dark =
        ((src & 0x1Fu) << 3) | ((src & 0x3E0u) << 6) | ((src & 0x7C00u) << 9);
    std::uint32_t full = dark | ((dark >> 5) & 0x070707u);
    if (src >> 15) full |= 0xFF000000u;
    return full;
}

inline std::uint32_t RGB565ToRGBA8888(std::uint16_t src) {
    const std::uint32_t dark_rb = ((src & 0x1Fu) << 3) | ((src & 0xF800u) << 8);
    const std::uint32_t full_rb = dark_rb | ((dark_rb >> 5) & 0x070007u);
    const std::uint32_t dark_g = (src & 0x7E0u) << 5;
    const std::uint32_t full_g = dark_g | ((dark_g >> 6) & 0x300u);
    return full_rb | full_g | 0xFF000000u;
}

inline std::uint16_t RGBA8888ToRGB565(std::uint32_t value) {
    const std::uint32_t r = (value >> 3) & 0x1F;
    const std::uint32_t g = (value >> 5) & (0x3Fu << 5);
    const std::uint32_t b = (value >> 8) & (0x1Fu << 11);
    return static_cast<std::uint16_t>(r | g | b);
}

inline std::uint16_t RGBA8888ToRGBA5551(std::uint32_t value) {
    const std::uint32_t r = (value >> 3) & 0x1F;
    const std::uint32_t g = (value >> 6) & (0x1Fu << 5);
    const std::uint32_t b = (value >> 9) & (0x1Fu << 10);
    const std::uint32_t a = (value >> 16) & 0x8000;
    return static_cast<std::uint16_t>(r | g | b | a);
}

inline std::uint16_t RGBA8888ToRGBA555X(std::uint32_t value) {
    const std::uint32_t r = (value >> 3) & 0x1F;
    const std::uint32_t g = (value >> 6) & (0x1Fu << 5);
    const std::uint32_t b = (value >> 9) & (0x1Fu << 10);
    return static_cast<std::uint16_t>(r | g | b);
}

inline std::uint16_t RGBA8888ToRGBA4444(std::uint32_t value) {
    const std::uint32_t c = value >> 4;
    const std::uint32_t r = (c >> 0) & 0x000F;
    const std::uint32_t g = (c >> 4) & 0x00F0;
    const std::uint32_t b = (c >> 8) & 0x0F00;
    const std::uint32_t a = (c >> 12) & 0xF000;
    return static_cast<std::uint16_t>(r | g | b | a);
}

inline std::uint16_t RGBA8888ToRGBA444X(std::uint32_t value) {
    return static_cast<std::uint16_t>(RGBA8888ToRGBA4444(value) & 0x0FFF);
}

inline int ClampU8(int v) {
    return std::clamp(v, 0, 255);
}

// float -> int32 as the x86 conversions PPSSPP's SSE paths and JIT use:
// CVTTPS2DQ truncates, CVTPS2DQ rounds to nearest even; both give
// 0x80000000 for NaN and out-of-range values (no undefined behaviour here).
inline int TruncToInt(float f) {
    if (!(f > -2147483904.0f && f < 2147483648.0f)) return static_cast<int>(0x80000000u);
    return static_cast<int>(f);
}
inline int RoundToInt(float f) {
    const float r = std::nearbyint(f);
    if (!(r >= -2147483648.0f && r < 2147483648.0f)) return static_cast<int>(0x80000000u);
    return static_cast<int>(r);
}

// A 4-lane integer colour (r, g, b, a) or a 2x2 quad of scalars.
struct Vec4i {
    int v[4] = {0, 0, 0, 0};

    static Vec4i All(int x) { return Vec4i{{x, x, x, x}}; }
    static Vec4i FromRGBA(std::uint32_t rgba) {
        return Vec4i{{static_cast<int>(rgba & 0xFF), static_cast<int>((rgba >> 8) & 0xFF),
                      static_cast<int>((rgba >> 16) & 0xFF),
                      static_cast<int>((rgba >> 24) & 0xFF)}};
    }
    // Saturating pack, like _mm_packus_epi16 on x86-64 (Vec4<int>::ToRGBA).
    [[nodiscard]] std::uint32_t ToRGBA() const {
        return static_cast<std::uint32_t>(ClampU8(v[0])) |
               (static_cast<std::uint32_t>(ClampU8(v[1])) << 8) |
               (static_cast<std::uint32_t>(ClampU8(v[2])) << 16) |
               (static_cast<std::uint32_t>(ClampU8(v[3])) << 24);
    }
    // Clamped RGB only (Vec3<int>::ToRGB with SSE clamps; alpha left 0).
    [[nodiscard]] std::uint32_t ToRGB() const { return ToRGBA() & 0x00FFFFFF; }

    int &operator[](int i) { return v[i]; }
    int operator[](int i) const { return v[i]; }
    [[nodiscard]] int r() const { return v[0]; }
    [[nodiscard]] int g() const { return v[1]; }
    [[nodiscard]] int b() const { return v[2]; }
    [[nodiscard]] int a() const { return v[3]; }
};

inline Vec4i operator+(const Vec4i &x, const Vec4i &y) {
    return Vec4i{{x.v[0] + y.v[0], x.v[1] + y.v[1], x.v[2] + y.v[2], x.v[3] + y.v[3]}};
}

} // namespace mpo::ge
