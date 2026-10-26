// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Sampler.cpp (SampleNearest/SampleLinear,
// TransformClutIndex, LookupColor), GPU/Software/DrawPixel.cpp
// (DrawSinglePixel: alpha test, fog, colour test, dither) and
// GPU/Software/Rasterizer.cpp (CalculateSamplingParams, ClampFogDepth)
// @ v1.20.4 (GPL-2.0-or-later), through the C++ port in
// profiles/mpo/host/ge (sampler.cpp, pixel.cpp, rasterizer.cpp).
//
// The GE pixel pipeline as one "ubershader": every decision that depends on
// the GE state is a specialization constant, so each pipeline gets a shader
// specialised for its state when it is created (dead branches folded away by
// the driver). The arithmetic is integer, as in softgpu, so a texel, a
// texture function, the fog or the dither give the same values as mpo_ge.
//
// Blending, the depth test and the depth write are fixed-function.

#version 450

// --- Specialization constants (PipelineKey in pipelines.cpp) ---------------
// Texturing: 0 off, 1 direct colour (RGBA8 image), 2 CLUT index (uint image).
layout(constant_id = 0) const int kTexMode = 0;
layout(constant_id = 1) const int kClutFormat = 3; // GE: 0 565, 1 5551, 2 4444, 3 8888
layout(constant_id = 2) const int kTexFunc = 0;    // 0 modulate 1 decal 2 blend 3 replace 4 add
layout(constant_id = 3) const bool kTexAlpha = false;
layout(constant_id = 4) const bool kDoubling = false;
layout(constant_id = 5) const bool kMagLinear = false;
layout(constant_id = 6) const bool kMinLinear = false;
layout(constant_id = 7) const bool kClampS = false;
layout(constant_id = 8) const bool kClampT = false;
layout(constant_id = 9) const bool kTexProj = false;
layout(constant_id = 10) const int kLodMode = 0; // 0 auto, 1 const, 2 slope
layout(constant_id = 11) const int kAlphaFunc = 1; // GE comparison, 1 = always
layout(constant_id = 12) const int kColorFunc = 1; // GE comparison, 1 = always
layout(constant_id = 13) const bool kFog = false;
layout(constant_id = 14) const bool kClear = false;
layout(constant_id = 15) const int kFbFormat = 3; // 0 565, 1 5551, 2 4444, 3 8888
layout(constant_id = 16) const bool kDither = false;
// Write values exactly representable in the target format (no fixed-function
// blending follows, so the shader output is the final pixel).
layout(constant_id = 17) const bool kQuantize = false;
layout(constant_id = 18) const bool kDepthRange = false;
layout(constant_id = 19) const bool kSecondary = false;

layout(push_constant) uniform Push {
    vec2 ndc_scale;
    vec2 uv_scale;     // through mode: 1 / texture size; else 1
    uint tex_size;     // width | height << 16 (level 0)
    uint clut_params;  // shift | mask << 8 | offset << 16 | transform << 31
    uint alpha_test;   // ref | mask << 8
    uint color_ref;
    uint color_mask;
    uint fog_color;    // 0x00BBGGRR
    uint env_color;    // texture environment colour 0x00BBGGRR
    uint depth_range;  // minz | maxz << 16
    int lod_offset;    // 1.27.4
    float lod_slope;
    vec2 ndc_origin;
    uvec4 dither;      // 16 signed bytes, row-major 4x4
} pc;

layout(set = 0, binding = 0) uniform sampler2D u_rgba;
layout(set = 0, binding = 1) uniform usampler2D u_index;
// The GE's internal palette (1 KiB, as loaded by LOADCLUT).
layout(set = 0, binding = 2, std140) uniform Clut {
    uvec4 words[64];
} u_clut;

layout(location = 0) noperspective in vec4 v_color0;
layout(location = 1) noperspective in vec3 v_color1;
layout(location = 2) in vec3 v_uvq;
layout(location = 3) noperspective in float v_fog;

layout(location = 0) out vec4 out_color;

const int kCompNever = 0;
const int kCompAlways = 1;
const int kCompEqual = 2;
const int kCompNotEqual = 3;
const int kCompLess = 4;
const int kCompLequal = 5;
const int kCompGreater = 6;
const int kCompGequal = 7;

// float -> int like CVTTSS2SI / CVTSS2SI: INT_MIN for NaN and out of range.
int TruncToInt(float f) {
    if (!(f > -2147483904.0 && f < 2147483648.0)) return int(0x80000000u);
    return int(f);
}
int RoundToInt(float f) {
    const float r = roundEven(f);
    if (!(r >= -2147483648.0 && r < 2147483648.0)) return int(0x80000000u);
    return int(r);
}

ivec4 FromRGBA(uint c) {
    return ivec4(int(c & 0xFFu), int((c >> 8) & 0xFFu), int((c >> 16) & 0xFFu), int(c >> 24));
}

// color.hpp conversions.
ivec4 From565(uint c) {
    const int r = int(c & 0x1Fu);
    const int g = int((c >> 5) & 0x3Fu);
    const int b = int((c >> 11) & 0x1Fu);
    return ivec4((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}
ivec4 From5551(uint c) {
    const int r = int(c & 0x1Fu);
    const int g = int((c >> 5) & 0x1Fu);
    const int b = int((c >> 10) & 0x1Fu);
    return ivec4((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2),
                 (c & 0x8000u) != 0u ? 255 : 0);
}
ivec4 From4444(uint c) {
    const ivec4 n = ivec4(int(c & 0xFu), int((c >> 4) & 0xFu), int((c >> 8) & 0xFu),
                          int((c >> 12) & 0xFu));
    return (n << 4) | n;
}

// Sampler::TransformClutIndex.
uint TransformClutIndex(uint index) {
    if ((pc.clut_params & 0x80000000u) != 0u) {
        const uint shift = pc.clut_params & 0x1Fu;
        const uint mask = (pc.clut_params >> 8) & 0xFFu;
        const uint offset = (pc.clut_params >> 16) & 0x1FFu;
        const uint offset_mask = kClutFormat == 3 ? 0xFFu : 0x1FFu;
        return ((index >> shift) & mask) | (offset & offset_mask);
    }
    return index & 0xFFu;
}

// Sampler::LookupColor (level 0: the palette is always shared).
ivec4 LookupClut(uint entry) {
    if (kClutFormat == 3) {
        if (entry >= 256u) return ivec4(0);
        return FromRGBA(u_clut.words[entry >> 2][entry & 3u]);
    }
    if (entry >= 512u) return ivec4(0);
    const uint word = u_clut.words[entry >> 3][(entry >> 1) & 3u];
    const uint c = (entry & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    if (kClutFormat == 0) return From565(c);
    if (kClutFormat == 1) return From5551(c);
    return From4444(c);
}

ivec4 Fetch(ivec2 uv) {
    if (kTexMode == 1) return ivec4(roundEven(texelFetch(u_rgba, uv, 0) * 255.0));
    return LookupClut(TransformClutIndex(texelFetch(u_index, uv, 0).r));
}

int ClampUV(int v, int size) {
    if (v >= size - 1) return size - 1;
    if (v >= 511) return 511;
    if (v < 0) return 0;
    return v;
}
int WrapUV(int v, int size) {
    return v & (size - 1) & 511;
}
int ApplyClampS(int v, int size) {
    return kClampS ? ClampUV(v, size) : WrapUV(v, size);
}
int ApplyClampT(int v, int size) {
    return kClampT ? ClampUV(v, size) : WrapUV(v, size);
}

ivec4 SampleNearest(float s, float t, int width, int height) {
    const int u = ApplyClampS(TruncToInt(s * float(width) * 256.0) >> 8, width);
    const int v = ApplyClampT(TruncToInt(t * float(height) * 256.0) >> 8, height);
    return Fetch(ivec2(u, v));
}

// The x86-64 JIT rounds the coordinate (CVTPS2DQ), see sampler.cpp.
ivec4 SampleLinear(float s, float t, int width, int height) {
    int base_u = int(uint(RoundToInt(s * float(width) * 256.0)) - 128u);
    int base_v = int(uint(RoundToInt(t * float(height) * 256.0)) - 128u);
    const int frac_u = (base_u >> 4) & 0x0F;
    const int frac_v = (base_v >> 4) & 0x0F;
    base_u >>= 8;
    base_v >>= 8;
    const int u0 = ApplyClampS(base_u, width);
    const int u1 = ApplyClampS(base_u + 1, width);
    const int v0 = ApplyClampT(base_v, height);
    const int v1 = ApplyClampT(base_v + 1, height);
    const ivec4 tl = Fetch(ivec2(u0, v0));
    const ivec4 tr = Fetch(ivec2(u1, v0));
    const ivec4 bl = Fetch(ivec2(u0, v1));
    const ivec4 br = Fetch(ivec2(u1, v1));
    const ivec4 top = tl * (0x10 - frac_u) + tr * frac_u;
    const ivec4 bot = bl * (0x10 - frac_u) + br * frac_u;
    return (top * (0x10 - frac_v) + bot * frac_v) >> 8;
}

// TextureFunctionOutput (x86-64 JIT: 16-bit lanes for MODULATE).
ivec4 TextureFunction(ivec4 prim_in, ivec4 tex) {
    const ivec4 prim = clamp(prim_in, ivec4(-32768), ivec4(32767));
    const int prim_a_word = int(uint(prim.a) & 0xFFFFu);
    ivec4 res;
    if (kTexFunc == 0) {
        const uvec4 p1 = uvec4(prim + 1) & 0xFFFFu;
        uvec4 r = (p1 * uvec4(tex)) & 0xFFFFu;
        r = r >> (kDoubling ? 7u : 8u);
        res.rgb = ivec3(r.rgb);
        if (!kTexAlpha) {
            res.a = prim_a_word;
        } else if (kDoubling) {
            res.a = int(r.a) >> 1;
        } else {
            res.a = int(r.a);
        }
    } else if (kTexFunc == 1) {
        if (kTexAlpha) {
            const int t = tex.a;
            const ivec3 c = (prim.rgb + 1) * (255 - t) + (tex.rgb + 1) * t;
            res.rgb = kDoubling ? c / 128 : c / 256;
        } else {
            res.rgb = kDoubling ? tex.rgb * 2 : tex.rgb;
        }
        res.a = prim_a_word;
    } else if (kTexFunc == 2) {
        const ivec4 env = FromRGBA(pc.env_color);
        const ivec3 c = (255 - tex.rgb) * prim.rgb + tex.rgb * env.rgb + 255;
        res.rgb = kDoubling ? c / 128 : c / 256;
        res.a = kTexAlpha ? ((prim.a + 1) * tex.a) / 256 : prim_a_word;
    } else if (kTexFunc == 3) {
        res.rgb = kDoubling ? tex.rgb * 2 : tex.rgb;
        res.a = kTexAlpha ? tex.a : prim_a_word;
    } else {
        const ivec3 c = prim.rgb + tex.rgb;
        res.rgb = kDoubling ? c * 2 : c;
        res.a = kTexAlpha ? ((prim.a + 1) * tex.a) / 256 : prim_a_word;
    }
    return res;
}

// Rasterizer.cpp TexLog2: a signed 1.27.4 log2.
int TexLog2(float delta) {
    const uint u = floatBitsToUint(delta);
    return int((u >> 19) & 0x0FFFu) - 127 * 16;
}

// Rasterizer.cpp ClampFogDepth.
int ClampFogDepth(float fogdepth) {
    const uint u = floatBitsToUint(fogdepth);
    const uint e = u >> 23;
    if ((u & 0x80000000u) != 0u || e <= 126u - 8u) return 0;
    if (e > 126u) return 255;
    const uint mantissa = (u & 0x007FFFFFu) | 0x00800000u;
    return int(mantissa >> (16u + 126u - e));
}

bool Compare(int func, int a, int b) {
    if (func == kCompNever) return false;
    if (func == kCompEqual) return a == b;
    if (func == kCompNotEqual) return a != b;
    if (func == kCompLess) return a < b;
    if (func == kCompLequal) return a <= b;
    if (func == kCompGreater) return a > b;
    if (func == kCompGequal) return a >= b;
    return true;
}

int DitherValue() {
    const ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    const int i = p.y * 4 + p.x;
    return bitfieldExtract(int(pc.dither[i >> 2]), (i & 3) * 8, 8);
}

// The value the framebuffer will hold, expanded back to 8 bits (RGBA8888To*
// and back, color.hpp).
ivec3 QuantizeRGB(ivec3 c) {
    if (kFbFormat == 0) {
        const ivec3 n = ivec3(c.r >> 3, c.g >> 2, c.b >> 3);
        return ivec3((n.r << 3) | (n.r >> 2), (n.g << 2) | (n.g >> 4), (n.b << 3) | (n.b >> 2));
    }
    if (kFbFormat == 1) {
        const ivec3 n = c >> 3;
        return (n << 3) | (n >> 2);
    }
    if (kFbFormat == 2) {
        const ivec3 n = c >> 4;
        return (n << 4) | n;
    }
    return c;
}
int QuantizeAlpha(int a) {
    if (kFbFormat == 1) return (a & 0x80) != 0 ? 255 : 0;
    if (kFbFormat == 2) return ((a >> 4) << 4) | (a >> 4);
    return a;
}

void main() {
    ivec4 prim = ivec4(RoundToInt(v_color0.r * 255.0), RoundToInt(v_color0.g * 255.0),
                       RoundToInt(v_color0.b * 255.0), RoundToInt(v_color0.a * 255.0));

    if (kClear) {
        // Clear mode: the vertex colour, dithered (softgpu dithers clears too).
        ivec3 rgb = prim.rgb;
        if (kDither) rgb += DitherValue();
        rgb = clamp(rgb, 0, 255);
        const int a = clamp(prim.a, 0, 255);
        out_color = vec4(QuantizeRGB(rgb), QuantizeAlpha(a)) / 255.0;
        return;
    }

    if (kTexMode != 0) {
        const int width = int(pc.tex_size & 0xFFFFu);
        const int height = int(pc.tex_size >> 16);
        float s = v_uvq.x;
        float t = v_uvq.y;
        if (kTexProj) {
            s = v_uvq.x / v_uvq.z;
            t = v_uvq.y / v_uvq.z;
        }
        s *= pc.uv_scale.x;
        t *= pc.uv_scale.y;
        bool linear = kMagLinear;
        if (kMagLinear != kMinLinear) {
            // CalculateSamplingParams without mipmaps: the level is always 0,
            // the filter is the minification one when the detail is > 0.
            int detail;
            if (kLodMode == 0) {
                const float ds = dFdxFine(s) * float(width);
                const float dt = dFdyFine(t) * float(height);
                detail = TexLog2(max(abs(ds), abs(dt)));
            } else if (kLodMode == 2) {
                detail = TexLog2(2.0 * (1.0 / gl_FragCoord.w) * pc.lod_slope);
            } else {
                detail = 0;
            }
            detail += pc.lod_offset;
            linear = detail > 0 ? kMinLinear : kMagLinear;
        }
        const ivec4 tex =
            linear ? SampleLinear(s, t, width, height) : SampleNearest(s, t, width, height);
        prim = TextureFunction(prim, tex);
    }

    if (kSecondary) {
        prim.rgb += ivec3(RoundToInt(v_color1.r * 255.0), RoundToInt(v_color1.g * 255.0),
                          RoundToInt(v_color1.b * 255.0));
    }
    prim = clamp(prim, 0, 255);

    if (kDepthRange) {
        const int z = RoundToInt(gl_FragCoord.z * 65535.0);
        if (z < int(pc.depth_range & 0xFFFFu) || z > int(pc.depth_range >> 16)) discard;
    }

    if (kAlphaFunc != kCompAlways) {
        const int mask = int((pc.alpha_test >> 8) & 0xFFu);
        if (!Compare(kAlphaFunc, prim.a & mask, int(pc.alpha_test & 0xFFu))) discard;
    }

    if (kFog) {
        const int fog = ClampFogDepth(v_fog);
        const ivec3 fc = FromRGBA(pc.fog_color).rgb;
        prim.rgb = (prim.rgb * fog + fc * (255 - fog) + 255) / 256;
    }

    if (kColorFunc != kCompAlways) {
        const uint c = (uint(prim.r) | (uint(prim.g) << 8) | (uint(prim.b) << 16)) & pc.color_mask;
        if (kColorFunc == kCompNever) discard;
        if (kColorFunc == kCompEqual && c != pc.color_ref) discard;
        if (kColorFunc == kCompNotEqual && c == pc.color_ref) discard;
    }

    ivec3 rgb = prim.rgb;
    if (kQuantize) {
        if (kDither) rgb = clamp(rgb + DitherValue(), 0, 255);
        rgb = QuantizeRGB(rgb);
    }
    out_color = vec4(vec3(rgb), float(prim.a)) / 255.0;
}
