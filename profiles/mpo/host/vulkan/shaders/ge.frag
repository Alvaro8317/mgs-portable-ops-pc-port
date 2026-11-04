// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Sampler.cpp (SampleNearest/SampleLinear,
// TransformClutIndex, LookupColor), GPU/Software/DrawPixel.cpp
// (DrawSinglePixel: alpha test, fog, colour test, dither, AlphaBlendingResult,
// ApplyLogicOp, SetPixelColor's write mask) and
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
// The depth test and the depth write are fixed-function. Blending is either
// fixed-function (only where that is exact, or on request) or done here
// (F15a): with kReadDst the shader reads the destination pixel and does
// softgpu's integer blend, the dither after it, the logic op and the per-bit
// write mask, then writes the final pixel. This source is compiled three
// times (compile_shaders.py): without destination reads, with GE_DST_INPUT
// (an input attachment: the colour target itself) and with GE_DST_COPY (a
// sampled copy of the target).
//
// GE_EXACT (F15b, triangles and rectangles): the values are not the GPU's
// interpolation but softgpu's (Rasterizer::DrawTriangle and DrawRectangle).
// For a triangle the shader evaluates the three integer edge functions at
// softgpu's sample point (16x + 7, 16y + 7 in 12.4), discards what softgpu
// would not cover, and interpolates the colours, z, fog and UV with the same
// float operations in the same order (precise: no fused multiply-add), from
// the per-triangle data in u_tris (GpuTriangle in pipelines.hpp). A rectangle
// steps its UV by repeated additions, as softgpu does. Vulkan's add, multiply
// and int-to-float conversion are correctly rounded, so they give the CPU's
// bits; the per-pixel reciprocal of the perspective UV is not (2.5 ulp), so
// ExactRecip rounds it exactly. The texture detail (filter choice) comes from
// the UV of softgpu's 2x2 quads, which start at the primitive's first pixel
// (not at even pixels like the GPU's). z goes to gl_FragDepth, so the
// fixed-function depth test sees softgpu's z.

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
// Destination reads (F15a): only true in the GE_DST_INPUT and GE_DST_COPY
// builds.
layout(constant_id = 20) const bool kShaderBlend = false;
layout(constant_id = 21) const int kBlendEq = 0;  // GE: add sub rsub min max absdiff
layout(constant_id = 22) const int kBlendSrc = 2; // PixelBlendFactor (pixel.hpp)
layout(constant_id = 23) const int kBlendDst = 3;
layout(constant_id = 24) const int kLogicOp = 3;  // GE logic op; 3 (COPY) = none
layout(constant_id = 25) const bool kWriteMask = false;
layout(constant_id = 26) const bool kReadDst = false;

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
    uint blend_fix_src; // 0x00BBGGRR
    uint blend_fix_dst;
    uint write_mask;   // 8888 bits that keep the old value (GE colour mask)
    uint render_scale; // F23b: image pixels per GE pixel (1 to 8)
    uint rect_sample;  // F23b: 1 + the readback's sample of a rectangle, or 0
    uint samp_size;    // F24c: replaced texture's width | height << 16, or 0
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
// The triangle's index in the batch (ge.vert); only GE_EXACT reads it.
layout(location = 4) flat in uint v_tri;

#if defined(GE_EXACT)
// GpuTriangle (pipelines.hpp).
struct Tri {
    ivec4 xf;   // edge x factors, .w flags (rectangle: first pixel in .xy)
    ivec4 yf;   // edge y factors, .w first pixel x of the quads
    ivec4 c;    // edge constants, .w first pixel y of the quads
    vec4 k;     // 1 / (w0 + w1 + w2), then 1 / clip w of each vertex
    vec4 s;     // rectangle: row, x step, y step; .w clip w 0
    vec4 t;     // .w clip w 1
    vec4 q;     // .w clip w 2
    vec4 fog;
    ivec4 z;    // .w last pixel x of the range
    uvec4 col0; // 0xAABBGGRR; .w last pixel y of the range
    uvec4 col1;
};
layout(set = 0, binding = 3, std430) readonly buffer Tris {
    Tri tris[];
} u_tris;
const int kTriFlatZ = 1 << 3;
const int kTriFlatColor0 = 1 << 4;
const int kTriFlatColor1 = 1 << 5;
const int kTriNoFog = 1 << 6;
const int kTriThrough = 1 << 7;
const int kTriRect = 1 << 8;
const int kTriFlatFog = 1 << 9;
// ge::TriangleSetup::zero_area: every w_i is 0 on the pixels softgpu covers,
// so each interpolated value is 0 * inf = NaN (x86's default NaN, sign bit
// set). Its effects: RoundToInt and TruncToInt give INT_MIN, ClampFogDepth 0
// and TexLog2 2056.
const int kTriZeroArea = 1 << 10;
const int kIntMin = int(0x80000000u);
const float kNanCoordinate = 1e30; // what makes the samplers see INT_MIN
const int kNanDetail = 2056;
#endif

layout(location = 0) out vec4 out_color;

// The destination pixel as softgpu's GetPixelColor sees it: the framebuffer
// value expanded to 8 bits (the target always holds expanded values).
#if defined(GE_DST_INPUT)
layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInput u_dst;
ivec4 LoadDst() {
    return ivec4(roundEven(subpassLoad(u_dst) * 255.0));
}
#elif defined(GE_DST_COPY)
layout(set = 1, binding = 0) uniform sampler2D u_dst;
ivec4 LoadDst() {
    return ivec4(roundEven(texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0) * 255.0));
}
#else
ivec4 LoadDst() {
    return ivec4(0);
}
#endif

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

// `limit`: the last texel the GE addresses, 511 (F24c: scaled with an HD
// replacement, which can also have a size that is not a power of two).
int ClampUV(int v, int size, int limit) {
    if (v >= size - 1) return size - 1;
    if (v >= limit) return limit;
    if (v < 0) return 0;
    return v;
}
int WrapUV(int v, int size, int limit) {
    if ((size & (size - 1)) == 0) return v & (size - 1) & limit;
    return min(((v % size) + size) % size, limit);
}
// F24c: the image sampled (the GE texture, or its replacement at another
// size) and its texel limits.
ivec2 SampleSize(int width, int height) {
    if (pc.samp_size == 0u) return ivec2(width, height);
    return ivec2(int(pc.samp_size & 0xFFFFu), int(pc.samp_size >> 16));
}
int g_limit_s = 511;
int g_limit_t = 511;
int ApplyClampS(int v, int size) {
    return kClampS ? ClampUV(v, size, g_limit_s) : WrapUV(v, size, g_limit_s);
}
int ApplyClampT(int v, int size) {
    return kClampT ? ClampUV(v, size, g_limit_t) : WrapUV(v, size, g_limit_t);
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
    // The GE pixel's place in the matrix (F23b: N x N image pixels each).
    const ivec2 p = (ivec2(gl_FragCoord.xy) / int(pc.render_scale)) & 3;
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

// Bits per channel of the framebuffer format.
ivec4 FbBits() {
    if (kFbFormat == 0) return ivec4(5, 6, 5, 0);
    if (kFbFormat == 1) return ivec4(5, 5, 5, 1);
    if (kFbFormat == 2) return ivec4(4);
    return ivec4(8);
}

// An n-bit channel value back to 8 bits (color.hpp).
int Expand(int v, int bits) {
    if (bits == 8) return v;
    if (bits == 6) return (v << 2) | (v >> 4);
    if (bits == 5) return (v << 3) | (v >> 2);
    if (bits == 4) return (v << 4) | v;
    if (bits == 1) return v != 0 ? 255 : 0;
    return 0;
}

// SetPixelColor: the value converted to the framebuffer format (truncated),
// with the bits of the write mask taken from the old value, expanded back.
ivec4 StorePixel(ivec4 c, ivec4 old) {
    const ivec4 bits = FbBits();
    ivec4 out_c;
    for (int i = 0; i < 4; ++i) {
        const int shift = 8 - bits[i];
        int v = shift >= 8 ? 0 : (c[i] >> shift);
        if (kWriteMask && shift < 8) {
            const int m = int((pc.write_mask >> (8 * i)) & 0xFFu) >> shift;
            v = (v & ~m) | ((old[i] >> shift) & m);
        }
        out_c[i] = Expand(v, bits[i]);
    }
    return out_c;
}

// pixel.cpp BlendFactor: one value for r, g and b.
ivec3 BlendFactor(int factor, bool is_src, ivec4 src, ivec4 dst) {
    const ivec4 other = is_src ? dst : src;
    if (factor == 0) return other.rgb;
    if (factor == 1) return 255 - other.rgb;
    if (factor == 2) return ivec3(src.a);
    if (factor == 3) return ivec3(255 - src.a);
    if (factor == 4) return ivec3(dst.a);
    if (factor == 5) return ivec3(255 - dst.a);
    if (factor == 6) return ivec3(2 * src.a);
    if (factor == 7) return ivec3(255 - min(2 * src.a, 255));
    if (factor == 8) return ivec3(2 * dst.a);
    if (factor == 9) return ivec3(255 - min(2 * dst.a, 255));
    if (factor == 11) return ivec3(0);
    if (factor == 12) return ivec3(255);
    return FromRGBA(is_src ? pc.blend_fix_src : pc.blend_fix_dst).rgb;
}

// AlphaBlendingResult, x86-64 SSE semantics: products are
// ((2c + 1) * (2f + 1)) >> 10 and subtractions saturate at zero.
ivec3 AlphaBlend(ivec4 src, ivec4 dst) {
    if (kBlendEq == 3) return min(src.rgb, dst.rgb);
    if (kBlendEq == 4) return max(src.rgb, dst.rgb);
    if (kBlendEq == 5) return abs(src.rgb - dst.rgb);
    const ivec3 sf = BlendFactor(kBlendSrc, true, src, dst);
    const ivec3 df = BlendFactor(kBlendDst, false, src, dst);
    const ivec3 s = ((src.rgb * 2 + 1) * (sf * 2 + 1)) >> 10;
    const ivec3 d = ((dst.rgb * 2 + 1) * (df * 2 + 1)) >> 10;
    if (kBlendEq == 1) return max(s - d, 0);
    if (kBlendEq == 2) return max(d - s, 0);
    return s + d;
}

// pixel.cpp ApplyLogicOp, on RGBA8 words (the alpha byte is the stencil).
uint ApplyLogicOp(uint n, uint o) {
    const uint kRgb = 0x00FFFFFFu;
    const uint kA = 0xFF000000u;
    switch (kLogicOp) {
    case 0:
        return n & kA;
    case 1:
        return n & (o | kA);
    case 2:
        return n & (~o | kA);
    case 3:
        return n;
    case 4:
        return (~n & (o & kRgb)) | (n & kA);
    case 5:
        return (o & kRgb) | (n & kA);
    case 6:
        return n ^ (o & kRgb);
    case 7:
        return n | (o & kRgb);
    case 8:
        return (~(n | o) & kRgb) | (n & kA);
    case 9:
        return (~(n ^ o) & kRgb) | (n & kA);
    case 10:
        return (~o & kRgb) | (n & kA);
    case 11:
        return n | (~o & kRgb);
    case 12:
        return (~n & kRgb) | (n & kA);
    case 13:
        return ((~n | o) & kRgb) | (n & kA);
    case 14:
        return (~(n & o) & kRgb) | (n & kA);
    default:
        return n | kRgb;
    }
}

uint PackRGBA(ivec4 c) {
    const uvec4 u = uvec4(clamp(c, 0, 255));
    return u.r | (u.g << 8) | (u.b << 16) | (u.a << 24);
}

// The end of DrawSinglePixel with the destination at hand: blending, the
// dither after it, the logic op and the write to the framebuffer format.
// The stencil (alpha) is the old one outside clear mode (no stencil ops yet).
void WritePixel(ivec4 prim) {
    ivec4 dst = LoadDst();
    if (kFbFormat == 0) dst.a = 0; // GetPixelColor: 565 has no alpha
    ivec3 rgb = prim.rgb;
    int a = kClear ? clamp(prim.a, 0, 255) : dst.a;
    if (kShaderBlend && !kClear) rgb = AlphaBlend(prim, dst);
    if (kDither) rgb += DitherValue();
    uint c = PackRGBA(ivec4(rgb, a));
    if (kLogicOp != 3 && !kClear) c = ApplyLogicOp(c, PackRGBA(dst));
    out_color = vec4(StorePixel(FromRGBA(c), dst)) / 255.0;
}

#if defined(GE_EXACT)
// rasterizer.cpp InterpolateF: ((c0 * w0 + c1 * w1) + c2 * w2) * wsum, each
// operation rounded on its own.
float InterpolateF(vec3 c, vec3 w, float wsum) {
    precise float v = c.x * w.x;
    v = v + c.y * w.y;
    v = v + c.z * w.z;
    precise float r = v * wsum;
    return r;
}

// 1 / x rounded to nearest, as the CPU's division gives it. The GPU's 1 / x
// may be a few ulp off, so the floats around it are checked with the exact
// product of the mantissas (24 x 24 bits): the nearest one makes x * r
// closest to 1. Zero, denormals, infinities and NaN keep the GPU's value.
float ExactRecip(float x) {
    precise float r = 1.0 / x;
    const uint xb = floatBitsToUint(x);
    const uint rb = floatBitsToUint(r);
    const uint ex = (xb >> 23) & 0xFFu;
    const uint er = (rb >> 23) & 0xFFu;
    if (ex == 0u || ex == 255u || er <= 4u || er >= 251u) return r;
    const uint mx = (xb & 0x7FFFFFu) | 0x800000u;
    uint best = rb;
    uint best_hi = 0xFFFFFFFFu;
    uint best_lo = 0xFFFFFFFFu;
    for (int d = -3; d <= 3; ++d) {
        const uint cb = uint(int(rb) + d); // the neighbours in magnitude
        const uint ec = (cb >> 23) & 0xFFu;
        const uint mc = (cb & 0x7FFFFFu) | 0x800000u;
        // x * c = mx * mc * 2^(ex + ec - 300), and 1 is 2^k in those units.
        const int k = 300 - int(ex) - int(ec);
        if (k < 45 || k > 49) continue;
        uint hi;
        uint lo;
        umulExtended(mx, mc, hi, lo);
        const uint thi = 1u << uint(k - 32);
        uint dhi;
        uint dlo;
        if (hi >= thi) {
            dhi = hi - thi;
            dlo = lo;
        } else {
            dhi = thi - hi - (lo != 0u ? 1u : 0u);
            dlo = 0u - lo;
        }
        if (dhi < best_hi || (dhi == best_hi && dlo < best_lo)) {
            best = cb;
            best_hi = dhi;
            best_lo = dlo;
        }
    }
    return uintBitsToFloat(best);
}

// Edge weights of a triangle at pixel p, as floats.
vec3 EdgeWeights(Tri tri, ivec2 p) {
    const ivec2 sp = p * 16 + 7;
    return vec3(tri.xf.xyz * sp.x + tri.yf.xyz * sp.y + tri.c.xyz);
}

// The texture coordinates softgpu computes at pixel p (scaled to the texture
// in through mode).
vec2 ExactUV(Tri tri, ivec2 p) {
    const int flags = tri.xf.w;
    if ((flags & kTriRect) != 0) {
        // DrawRectangle: the row's coordinates step once per two rows, then
        // once per two columns, then the lane's offset within the quad.
        const ivec2 rel = p - tri.xf.xy;
        const ivec2 q = rel >> 1;
        precise float s = tri.s.x;
        precise float t = tri.t.x;
        for (int j = 0; j < q.y; ++j) {
            s = s + tri.s.z;
            t = t + tri.t.z;
        }
        for (int i = 0; i < q.x; ++i) {
            s = s + tri.s.y;
            t = t + tri.t.y;
        }
        const int lane = (rel.x & 1) | ((rel.y & 1) << 1);
        precise float hxs = 0.5 * tri.s.y;
        precise float hys = 0.5 * tri.s.z;
        precise float hxt = 0.5 * tri.t.y;
        precise float hyt = 0.5 * tri.t.z;
        precise float os = lane == 0 ? 0.0 : (lane == 1 ? hxs : (lane == 2 ? hys : hxs + hys));
        precise float ot = lane == 0 ? 0.0 : (lane == 1 ? hxt : (lane == 2 ? hyt : hxt + hyt));
        precise float rs = s + os;
        precise float rt = t + ot;
        return vec2(rs, rt);
    }
    if ((flags & kTriZeroArea) != 0) return vec2(kNanCoordinate);
    const vec3 fw = EdgeWeights(tri, p);
    if ((flags & kTriThrough) != 0) {
        precise float s = InterpolateF(tri.s.xyz, fw, tri.k.x) * pc.uv_scale.x;
        precise float t = InterpolateF(tri.t.xyz, fw, tri.k.x) * pc.uv_scale.y;
        return vec2(s, t);
    }
    // GetTextureCoordinates(Proj): perspective with the clip w.
    precise vec3 wq = fw * tri.k.yzw;
    float q_recip;
    if (kTexProj) {
        precise float qv = wq.x * tri.q.x;
        qv = qv + wq.y * tri.q.y;
        qv = qv + wq.z * tri.q.z;
        q_recip = ExactRecip(qv);
    } else {
        precise float sum = wq.x + wq.y;
        sum = sum + wq.z;
        q_recip = ExactRecip(sum);
    }
    precise float sv = wq.x * tri.s.x;
    sv = sv + wq.y * tri.s.y;
    sv = sv + wq.z * tri.s.z;
    precise float tv = wq.x * tri.t.x;
    tv = tv + wq.y * tri.t.y;
    tv = tv + wq.z * tri.t.z;
    precise float s = sv * q_recip;
    precise float t = tv * q_recip;
    return vec2(s, t);
}

// The top-left pixel of softgpu's 2x2 quad holding p.
ivec2 QuadOrigin(Tri tri, ivec2 p) {
    const ivec2 o = (tri.xf.w & kTriRect) != 0 ? tri.xf.xy : ivec2(tri.yf.w, tri.c.w);
    return o + (((p - o) >> 1) << 1);
}

// The clip w softgpu's slope-based detail uses: the quad's first lane.
float ExactClipW(Tri tri, ivec2 quad) {
    if ((tri.xf.w & kTriRect) != 0) return tri.s.w;
    return InterpolateF(vec3(tri.s.w, tri.t.w, tri.q.w), EdgeWeights(tri, quad), tri.k.x);
}
#endif

void main() {
#if defined(GE_EXACT)
    // softgpu's interpolation (Rasterizer::DrawTriangle), see the top.
    const Tri tri = u_tris.tris[v_tri];
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const ivec2 sp = pixel * 16 + 7;
    const ivec3 w = tri.xf.xyz * sp.x + tri.yf.xyz * sp.y + tri.c.xyz; // wraps
    const int flags = tri.xf.w;
    const ivec3 bias = -((ivec3(flags) >> ivec3(0, 1, 2)) & 1);
    // A rectangle covers what the GPU rasterizes (its coverage is exact). A
    // triangle covers its pixels inside the range (only one without area
    // reaches past it: its weights are 0 along the whole line).
    const bool in_range = all(greaterThanEqual(pixel, ivec2(tri.yf.w, tri.c.w))) &&
                          all(lessThanEqual(pixel, ivec2(tri.z.w, int(tri.col0.w))));
    const bool covered = (flags & kTriRect) != 0 ||
                         (in_range && ((w.x + bias.x) | (w.y + bias.y) | (w.z + bias.z)) >= 0);
    const vec3 fw = vec3(w);
    const float wsum = tri.k.x;
    const bool zero_area = (flags & kTriZeroArea) != 0;
    int ez = tri.z.z;
    if ((flags & kTriFlatZ) == 0)
        ez = zero_area ? kIntMin : RoundToInt(InterpolateF(vec3(tri.z.xyz), fw, wsum));
    // +0.25: the D16 conversion gives ez whether it rounds or truncates.
    gl_FragDepth = (float(clamp(ez, 0, 65535)) + 0.25) / 65535.0;
    ivec4 prim;
    if ((flags & kTriFlatColor0) != 0) {
        prim = FromRGBA(tri.col0.z);
    } else if (zero_area) {
        prim = ivec4(kIntMin);
    } else {
        const ivec4 c0 = FromRGBA(tri.col0.x);
        const ivec4 c1 = FromRGBA(tri.col0.y);
        const ivec4 c2 = FromRGBA(tri.col0.z);
        for (int i = 0; i < 4; ++i)
            prim[i] = RoundToInt(InterpolateF(vec3(c0[i], c1[i], c2[i]), fw, wsum));
    }
#else
    ivec4 prim = ivec4(RoundToInt(v_color0.r * 255.0), RoundToInt(v_color0.g * 255.0),
                       RoundToInt(v_color0.b * 255.0), RoundToInt(v_color0.a * 255.0));
#endif

    if (kClear && kReadDst) {
#if defined(GE_EXACT)
        if (!covered) discard;
#endif
        WritePixel(prim);
        return;
    }
    if (kClear) {
#if defined(GE_EXACT)
        if (!covered) discard;
#endif
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
#if defined(GE_EXACT)
        const vec2 uv = ExactUV(tri, pixel);
        const float s = uv.x;
        const float t = uv.y;
#else
        float s = v_uvq.x;
        float t = v_uvq.y;
        if (kTexProj) {
            s = v_uvq.x / v_uvq.z;
            t = v_uvq.y / v_uvq.z;
        }
        s *= pc.uv_scale.x;
        t *= pc.uv_scale.y;
        // F23b: the derivatives for rect_sample, taken where every pixel of
        // the quad still runs (below).
        const vec2 uv_step = vec2(dFdxFine(s) + dFdyFine(s), dFdxFine(t) + dFdyFine(t));
#endif
        bool linear = kMagLinear;
        if (kMagLinear != kMinLinear) {
            // CalculateSamplingParams without mipmaps: the level is always 0,
            // the filter is the minification one when the detail is > 0.
            int detail;
            if (kLodMode == 0) {
#if defined(GE_EXACT)
                // ApplyTexturing: s[1] - s[0] and t[2] - t[0] of the quad.
                if ((tri.xf.w & kTriZeroArea) != 0) {
                    detail = kNanDetail;
                } else {
                const ivec2 quad = QuadOrigin(tri, pixel);
                const vec2 uv0 = ExactUV(tri, quad);
                precise float ds = ExactUV(tri, quad + ivec2(1, 0)).x - uv0.x;
                precise float dt = ExactUV(tri, quad + ivec2(0, 1)).y - uv0.y;
                precise float ads = abs(ds * float(width));
                precise float adt = abs(dt * float(height));
                detail = TexLog2(max(ads, adt));
                }
#else
                // Per GE pixel (F23b: N image pixels).
                const float ds = dFdxFine(s) * float(width) * float(pc.render_scale);
                const float dt = dFdyFine(t) * float(height) * float(pc.render_scale);
                detail = TexLog2(max(abs(ds), abs(dt)));
#endif
            } else if (kLodMode == 2) {
#if defined(GE_EXACT)
                if ((tri.xf.w & kTriZeroArea) != 0) {
                    detail = kNanDetail;
                } else {
                    precise float x = 2.0 * ExactClipW(tri, QuadOrigin(tri, pixel));
                    x = x * pc.lod_slope;
                    detail = TexLog2(x);
                }
#else
                detail = TexLog2(2.0 * (1.0 / gl_FragCoord.w) * pc.lod_slope);
#endif
            } else {
                detail = 0;
            }
            detail += pc.lod_offset;
            linear = detail > 0 ? kMinLinear : kMagLinear;
        }
#if !defined(GE_EXACT)
        // F23b: the image pixel of a rectangle's N x N block that the
        // readback keeps (rect_sample - 1, at an even N) takes the UV at the
        // GE pixel's centre, half an image pixel down and right, as softgpu
        // samples it (a rectangle's UVs are affine). The others keep theirs.
        if (pc.rect_sample != 0u &&
            all(equal(ivec2(gl_FragCoord.xy) % int(pc.render_scale),
                      ivec2(int(pc.rect_sample) - 1)))) {
            s += 0.5 * uv_step.x;
            t += 0.5 * uv_step.y;
        }
#endif
        // F24c: an HD replacement is sampled at its own size, with the GE's
        // 512-texel limit scaled to it.
        const ivec2 samp = SampleSize(width, height);
        if (pc.samp_size != 0u) {
            g_limit_s = samp.x * 512 / width - 1;
            g_limit_t = samp.y * 512 / height - 1;
        }
        const ivec4 tex =
            linear ? SampleLinear(s, t, samp.x, samp.y) : SampleNearest(s, t, samp.x, samp.y);
        prim = TextureFunction(prim, tex);
    }

    if (kSecondary) {
#if defined(GE_EXACT)
        if ((flags & kTriFlatColor1) != 0) {
            prim.rgb += FromRGBA(tri.col1.z).rgb;
        } else if (zero_area) {
            prim.rgb += ivec3(kIntMin);
        } else {
            const ivec4 c0 = FromRGBA(tri.col1.x);
            const ivec4 c1 = FromRGBA(tri.col1.y);
            const ivec4 c2 = FromRGBA(tri.col1.z);
            for (int i = 0; i < 3; ++i)
                prim[i] += RoundToInt(InterpolateF(vec3(c0[i], c1[i], c2[i]), fw, wsum));
        }
#else
        prim.rgb += ivec3(RoundToInt(v_color1.r * 255.0), RoundToInt(v_color1.g * 255.0),
                          RoundToInt(v_color1.b * 255.0));
#endif
    }
    prim = clamp(prim, 0, 255);

#if defined(GE_EXACT)
    // After the texturing, so that every pixel of a quad computed its
    // derivatives.
    if (!covered) discard;
#endif

    if (kDepthRange) {
#if defined(GE_EXACT)
        const int z = ez;
#else
        const int z = RoundToInt(gl_FragCoord.z * 65535.0);
#endif
        if (z < int(pc.depth_range & 0xFFFFu) || z > int(pc.depth_range >> 16)) discard;
    }

    if (kAlphaFunc != kCompAlways) {
        const int mask = int((pc.alpha_test >> 8) & 0xFFu);
        if (!Compare(kAlphaFunc, prim.a & mask, int(pc.alpha_test & 0xFFu))) discard;
    }

    if (kFog) {
#if defined(GE_EXACT)
        int fog = 255;
        if ((flags & kTriFlatFog) != 0) {
            fog = ClampFogDepth(tri.fog.z);
        } else if ((flags & kTriNoFog) == 0) {
            fog = zero_area ? 0 : ClampFogDepth(InterpolateF(tri.fog.xyz, fw, wsum));
        }
#else
        const int fog = ClampFogDepth(v_fog);
#endif
        const ivec3 fc = FromRGBA(pc.fog_color).rgb;
        prim.rgb = (prim.rgb * fog + fc * (255 - fog) + 255) / 256;
    }

    if (kColorFunc != kCompAlways) {
        const uint c = (uint(prim.r) | (uint(prim.g) << 8) | (uint(prim.b) << 16)) & pc.color_mask;
        if (kColorFunc == kCompNever) discard;
        if (kColorFunc == kCompEqual && c != pc.color_ref) discard;
        if (kColorFunc == kCompNotEqual && c == pc.color_ref) discard;
    }

    if (kReadDst) {
        WritePixel(prim);
        return;
    }
    ivec3 rgb = prim.rgb;
    if (kQuantize) {
        if (kDither) rgb = clamp(rgb + DitherValue(), 0, 255);
        rgb = QuantizeRGB(rgb);
    }
    out_color = vec4(vec3(rgb), float(prim.a)) / 255.0;
}
