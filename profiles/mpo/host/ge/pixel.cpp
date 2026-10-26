// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/FuncId.cpp and GPU/Software/DrawPixel.cpp
// @ v1.20.4 (GPL-2.0-or-later).

#include "pixel.hpp"

#include <algorithm>
#include <cstdlib>

namespace mpo::ge {
namespace {

Comparison OptimizeRefByteCompare(Comparison func, std::uint8_t ref) {
    if (ref == 0 && func == Comparison::kGreater) return Comparison::kNotEqual;
    if (ref == 0xFF && func == Comparison::kLess) return Comparison::kNotEqual;
    if (ref == 0 && func == Comparison::kGequal) return Comparison::kAlways;
    if (ref == 0xFF && func == Comparison::kLequal) return Comparison::kAlways;
    return func;
}

PixelBlendFactor OptimizeAlphaFactor(std::uint32_t color) {
    if (color == 0x00000000) return PixelBlendFactor::kZero;
    if (color == 0x00FFFFFF) return PixelBlendFactor::kOne;
    return PixelBlendFactor::kFix;
}

std::uint32_t ConvertWriteMask(BufferFormat fmt, std::uint32_t mask) {
    switch (fmt) {
    case BufferFormat::k565:
        return RGBA8888ToRGB565(mask);
    case BufferFormat::k5551:
        return RGBA8888ToRGBA5551(mask);
    case BufferFormat::k4444:
        return RGBA8888ToRGBA4444(mask);
    case BufferFormat::k8888:
        return mask;
    }
    return mask;
}

std::uint8_t GetPixelStencil(const RenderTarget &rt, BufferFormat fmt, int x, int y) {
    switch (fmt) {
    case BufferFormat::k565:
        return 0;
    case BufferFormat::k5551:
        return (rt.Get16(x, y) & 0x8000) != 0 ? 0xFF : 0;
    case BufferFormat::k4444:
        return Convert4To8(static_cast<std::uint32_t>(rt.Get16(x, y) >> 12));
    case BufferFormat::k8888:
        return static_cast<std::uint8_t>(rt.Get32(x, y) >> 24);
    }
    return 0;
}

void SetPixelStencil(const RenderTarget &rt, BufferFormat fmt, std::uint32_t write_mask, int x,
                     int y, std::uint8_t value) {
    switch (fmt) {
    case BufferFormat::k565:
        break;
    case BufferFormat::k5551:
        if ((write_mask & 0x8000) == 0) {
            auto pixel = static_cast<std::uint16_t>(rt.Get16(x, y) & ~0x8000u);
            pixel = static_cast<std::uint16_t>(pixel | ((value & 0x80u) << 8));
            rt.Set16(x, y, pixel);
        }
        break;
    case BufferFormat::k4444: {
        const std::uint32_t mask = write_mask | 0x0FFF;
        std::uint32_t pixel = rt.Get16(x, y) & mask;
        pixel |= (static_cast<std::uint32_t>(value) << 8) & ~mask;
        rt.Set16(x, y, static_cast<std::uint16_t>(pixel));
        break;
    }
    case BufferFormat::k8888: {
        const std::uint32_t mask = write_mask | 0x00FFFFFF;
        std::uint32_t pixel = rt.Get32(x, y) & mask;
        pixel |= (static_cast<std::uint32_t>(value) << 24) & ~mask;
        rt.Set32(x, y, pixel);
        break;
    }
    }
}

std::uint32_t GetPixelColor(const RenderTarget &rt, BufferFormat fmt, int x, int y) {
    switch (fmt) {
    case BufferFormat::k565:
        return RGB565ToRGBA8888(rt.Get16(x, y)) & 0x00FFFFFF;
    case BufferFormat::k5551:
        return RGBA5551ToRGBA8888(rt.Get16(x, y));
    case BufferFormat::k4444:
        return RGBA4444ToRGBA8888(rt.Get16(x, y));
    case BufferFormat::k8888:
        return rt.Get32(x, y);
    }
    return 0;
}

void SetPixelColor(const RenderTarget &rt, BufferFormat fmt, int x, int y, std::uint32_t value,
                   std::uint32_t old_value, std::uint32_t write_mask) {
    switch (fmt) {
    case BufferFormat::k565: {
        std::uint32_t v = RGBA8888ToRGB565(value);
        if (write_mask != 0) {
            v = (v & ~write_mask) | (RGBA8888ToRGB565(old_value) & write_mask);
        }
        rt.Set16(x, y, static_cast<std::uint16_t>(v));
        break;
    }
    case BufferFormat::k5551: {
        std::uint32_t v = RGBA8888ToRGBA5551(value);
        if (write_mask != 0) {
            v = (v & ~write_mask) | (RGBA8888ToRGBA5551(old_value) & write_mask);
        }
        rt.Set16(x, y, static_cast<std::uint16_t>(v));
        break;
    }
    case BufferFormat::k4444: {
        std::uint32_t v = RGBA8888ToRGBA4444(value);
        if (write_mask != 0) {
            v = (v & ~write_mask) | (RGBA8888ToRGBA4444(old_value) & write_mask);
        }
        rt.Set16(x, y, static_cast<std::uint16_t>(v));
        break;
    }
    case BufferFormat::k8888:
        rt.Set32(x, y, (value & ~write_mask) | (old_value & write_mask));
        break;
    }
}

bool AlphaTestPassed(const PixelState &ps, int alpha) {
    const int ref = ps.alpha_test_ref;
    if (ps.has_alpha_test_mask) alpha &= ps.alpha_test_mask;
    switch (ps.alpha_test_func) {
    case Comparison::kNever:
        return false;
    case Comparison::kAlways:
        return true;
    case Comparison::kEqual:
        return alpha == ref;
    case Comparison::kNotEqual:
        return alpha != ref;
    case Comparison::kLess:
        return alpha < ref;
    case Comparison::kLequal:
        return alpha <= ref;
    case Comparison::kGreater:
        return alpha > ref;
    case Comparison::kGequal:
        return alpha >= ref;
    }
    return true;
}

bool ColorTestPassed(const PixelState &ps, const Vec4i &color) {
    const std::uint32_t c = color.ToRGB() & ps.color_test_mask;
    switch (ps.color_test_func) {
    case Comparison::kNever:
        return false;
    case Comparison::kAlways:
        return true;
    case Comparison::kEqual:
        return c == ps.color_test_ref;
    case Comparison::kNotEqual:
        return c != ps.color_test_ref;
    default:
        return true;
    }
}

bool StencilTestPassed(const PixelState &ps, std::uint8_t stencil) {
    if (ps.has_stencil_test_mask)
        stencil = static_cast<std::uint8_t>(stencil & ps.stencil_test_mask);
    const std::uint8_t ref = ps.stencil_test_ref;
    switch (ps.stencil_test_func) {
    case Comparison::kNever:
        return false;
    case Comparison::kAlways:
        return true;
    case Comparison::kEqual:
        return ref == stencil;
    case Comparison::kNotEqual:
        return ref != stencil;
    case Comparison::kLess:
        return ref < stencil;
    case Comparison::kLequal:
        return ref <= stencil;
    case Comparison::kGreater:
        return ref > stencil;
    case Comparison::kGequal:
        return ref >= stencil;
    }
    return true;
}

std::uint8_t ApplyStencilOp(BufferFormat fmt, std::uint8_t replace, StencilOp op,
                            std::uint8_t old_stencil) {
    switch (op) {
    case StencilOp::kKeep:
        return old_stencil;
    case StencilOp::kZero:
        return 0;
    case StencilOp::kReplace:
        return replace;
    case StencilOp::kInvert:
        return static_cast<std::uint8_t>(~old_stencil);
    case StencilOp::kIncr:
        switch (fmt) {
        case BufferFormat::k8888:
            return old_stencil != 0xFF ? static_cast<std::uint8_t>(old_stencil + 1) : old_stencil;
        case BufferFormat::k5551:
            return 0xFF;
        case BufferFormat::k4444:
            return old_stencil < 0xF0 ? static_cast<std::uint8_t>(old_stencil + 0x10) : old_stencil;
        default:
            return old_stencil;
        }
    case StencilOp::kDecr:
        switch (fmt) {
        case BufferFormat::k4444:
            if (old_stencil >= 0x10) return static_cast<std::uint8_t>(old_stencil - 0x10);
            break;
        case BufferFormat::k5551:
            return 0;
        default:
            return old_stencil != 0 ? static_cast<std::uint8_t>(old_stencil - 1) : old_stencil;
        }
        break;
    }
    return old_stencil;
}

std::uint32_t ApplyLogicOp(LogicOp op, std::uint32_t old_color, std::uint32_t new_color) {
    switch (op) {
    case LogicOp::kClear:
        new_color &= 0xFF000000;
        break;
    case LogicOp::kAnd:
        new_color = new_color & (old_color | 0xFF000000);
        break;
    case LogicOp::kAndReverse:
        new_color = new_color & (~old_color | 0xFF000000);
        break;
    case LogicOp::kCopy:
        break;
    case LogicOp::kAndInverted:
        new_color = (~new_color & (old_color & 0x00FFFFFF)) | (new_color & 0xFF000000);
        break;
    case LogicOp::kNoop:
        new_color = (old_color & 0x00FFFFFF) | (new_color & 0xFF000000);
        break;
    case LogicOp::kXor:
        new_color = new_color ^ (old_color & 0x00FFFFFF);
        break;
    case LogicOp::kOr:
        new_color = new_color | (old_color & 0x00FFFFFF);
        break;
    case LogicOp::kNor:
        new_color = (~(new_color | old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
        break;
    case LogicOp::kEquiv:
        new_color = (~(new_color ^ old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
        break;
    case LogicOp::kInverted:
        new_color = (~old_color & 0x00FFFFFF) | (new_color & 0xFF000000);
        break;
    case LogicOp::kOrReverse:
        new_color = new_color | (~old_color & 0x00FFFFFF);
        break;
    case LogicOp::kCopyInverted:
        new_color = (~new_color & 0x00FFFFFF) | (new_color & 0xFF000000);
        break;
    case LogicOp::kOrInverted:
        new_color = ((~new_color | old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
        break;
    case LogicOp::kNand:
        new_color = (~(new_color & old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
        break;
    case LogicOp::kSet:
        new_color |= 0x00FFFFFF;
        break;
    }
    return new_color;
}

// Returns the factor as one value for r, g and b (all GE factors are either
// a scalar or a colour).
void BlendFactor(PixelBlendFactor factor, bool is_src, const Vec4i &source, const Vec4i &dst,
                 std::uint32_t fix, int out[3]) {
    switch (factor) {
    case PixelBlendFactor::kOtherColor: {
        const Vec4i &other = is_src ? dst : source;
        for (int i = 0; i < 3; ++i) out[i] = other[i];
        return;
    }
    case PixelBlendFactor::kInvOtherColor: {
        const Vec4i &other = is_src ? dst : source;
        for (int i = 0; i < 3; ++i) out[i] = 255 - other[i];
        return;
    }
    case PixelBlendFactor::kSrcAlpha:
        out[0] = out[1] = out[2] = source.a();
        return;
    case PixelBlendFactor::kInvSrcAlpha:
        out[0] = out[1] = out[2] = 255 - source.a();
        return;
    case PixelBlendFactor::kDstAlpha:
        out[0] = out[1] = out[2] = dst.a();
        return;
    case PixelBlendFactor::kInvDstAlpha:
        out[0] = out[1] = out[2] = 255 - dst.a();
        return;
    case PixelBlendFactor::kDoubleSrcAlpha:
        out[0] = out[1] = out[2] = 2 * source.a();
        return;
    case PixelBlendFactor::kDoubleInvSrcAlpha:
        out[0] = out[1] = out[2] = 255 - std::min(2 * source.a(), 255);
        return;
    case PixelBlendFactor::kDoubleDstAlpha:
        out[0] = out[1] = out[2] = 2 * dst.a();
        return;
    case PixelBlendFactor::kDoubleInvDstAlpha:
        out[0] = out[1] = out[2] = 255 - std::min(2 * dst.a(), 255);
        return;
    case PixelBlendFactor::kZero:
        out[0] = out[1] = out[2] = 0;
        return;
    case PixelBlendFactor::kOne:
        out[0] = out[1] = out[2] = 255;
        return;
    case PixelBlendFactor::kFix:
    default: {
        const Vec4i f = Vec4i::FromRGBA(fix);
        for (int i = 0; i < 3; ++i) out[i] = f[i];
        return;
    }
    }
}

// AlphaBlendingResult, x86-64 SSE semantics: products are
// ((2c + 1) * (2f + 1)) >> 10 and subtractions saturate at zero.
Vec4i AlphaBlendingResult(const PixelState &ps, const Vec4i &source, const Vec4i &dst) {
    int srcf[3];
    int dstf[3];
    BlendFactor(ps.blend_src, true, source, dst, ps.blend_fix_src, srcf);
    BlendFactor(ps.blend_dst, false, source, dst, ps.blend_fix_dst, dstf);
    Vec4i out;
    for (int i = 0; i < 3; ++i) {
        const int s = ((source[i] * 2 + 1) * (srcf[i] * 2 + 1)) >> 10;
        const int d = ((dst[i] * 2 + 1) * (dstf[i] * 2 + 1)) >> 10;
        switch (ps.blend_eq) {
        case BlendMode::kAdd:
            out[i] = s + d;
            break;
        case BlendMode::kSubtract:
            out[i] = std::max(s - d, 0);
            break;
        case BlendMode::kReverseSubtract:
            out[i] = std::max(d - s, 0);
            break;
        case BlendMode::kMin:
            out[i] = std::min(source[i], dst[i]);
            break;
        case BlendMode::kMax:
            out[i] = std::max(source[i], dst[i]);
            break;
        case BlendMode::kAbsDiff:
            out[i] = std::abs(source[i] - dst[i]);
            break;
        }
    }
    return out;
}

} // namespace

PixelState ComputePixelState(const GeState &gs) {
    PixelState id;
    id.apply_depth_range = !gs.IsModeThrough();
    id.dithering = gs.IsDitherEnabled();
    id.fb_format = gs.FrameBufFormat();
    id.apply_color_write_mask = gs.ColorMask() != 0;

    id.clear_mode = gs.IsModeClear();
    if (id.clear_mode) {
        id.color_test = gs.IsClearModeColorMask();
        id.stencil_test = gs.IsClearModeAlphaMask() && gs.FrameBufFormat() != BufferFormat::k565;
        id.depth_write = gs.IsClearModeDepthMask();
        id.depth_test_func = Comparison::kAlways;
        id.alpha_test_func = Comparison::kAlways;
    } else {
        id.color_test = gs.IsColorTestEnabled() && gs.ColorTestFunction() != Comparison::kAlways;
        if (gs.IsStencilTestEnabled() && gs.StencilTestFunction() == Comparison::kAlways) {
            const bool stencil_write =
                (gs.Reg(kCmdMaskAlpha) & 0xFF) != 0xFF && gs.FrameBufFormat() != BufferFormat::k565;
            if (gs.IsDepthTestEnabled() && gs.DepthTestFunction() != Comparison::kAlways) {
                id.stencil_test = stencil_write && (gs.StencilOpZPass() != StencilOp::kKeep ||
                                                    gs.StencilOpZFail() != StencilOp::kKeep);
            } else {
                id.stencil_test = stencil_write && gs.StencilOpZPass() != StencilOp::kKeep;
            }
        } else {
            id.stencil_test = gs.IsStencilTestEnabled();
        }
        id.depth_write = gs.IsDepthTestEnabled() && gs.IsDepthWriteEnabled();
        id.depth_test_func = gs.IsDepthTestEnabled() ? gs.DepthTestFunction() : Comparison::kAlways;

        if (id.stencil_test) {
            id.stencil_test_ref =
                static_cast<std::uint8_t>(gs.StencilTestRef() & gs.StencilTestMask());
            id.stencil_test_func =
                OptimizeRefByteCompare(gs.StencilTestFunction(), id.stencil_test_ref);
            id.has_stencil_test_mask =
                gs.StencilTestMask() != 0xFF && gs.FrameBufFormat() != BufferFormat::k565;
            const bool can_write = gs.FrameBufFormat() != BufferFormat::k565;
            if (can_write && gs.StencilOpSFail() <= StencilOp::kDecr)
                id.sfail = gs.StencilOpSFail();
            if (can_write && gs.StencilOpZFail() <= StencilOp::kDecr) {
                id.zfail = gs.IsDepthTestEnabled() ? gs.StencilOpZFail() : StencilOp::kKeep;
            }
            if (can_write && gs.StencilOpZPass() <= StencilOp::kDecr)
                id.zpass = gs.StencilOpZPass();

            if (gs.StencilTestRef() == 0) {
                if (id.sfail == StencilOp::kReplace) id.sfail = StencilOp::kZero;
                if (id.zfail == StencilOp::kReplace) id.zfail = StencilOp::kZero;
                if (id.zpass == StencilOp::kReplace) id.zpass = StencilOp::kZero;
            }
            if (id.fb_format == BufferFormat::k5551) {
                if (id.sfail == StencilOp::kDecr) id.sfail = StencilOp::kZero;
                if (id.zfail == StencilOp::kDecr) id.zfail = StencilOp::kZero;
                if (id.zpass == StencilOp::kDecr) id.zpass = StencilOp::kZero;
            }
            if (id.stencil_test_func == Comparison::kAlways) {
                if (id.depth_test_func == Comparison::kAlways) id.zfail = StencilOp::kKeep;
                id.sfail = StencilOp::kKeep;
                id.stencil_test_ref = static_cast<std::uint8_t>(gs.StencilTestRef());
                id.has_stencil_test_mask = false;
                if (id.sfail == StencilOp::kKeep && id.zfail == StencilOp::kKeep &&
                    id.zpass == StencilOp::kKeep) {
                    id.stencil_test = false;
                    id.stencil_test_func = Comparison::kNever;
                    id.stencil_test_ref = 0;
                }
            } else if (id.depth_test_func == Comparison::kAlways) {
                id.zfail = id.zpass;
            }
        }

        id.alpha_test_func = gs.IsAlphaTestEnabled() ? gs.AlphaTestFunction() : Comparison::kAlways;
        if (id.alpha_test_func != Comparison::kAlways) {
            id.alpha_test_ref = static_cast<std::uint8_t>(gs.AlphaTestRef() & gs.AlphaTestMask());
            id.has_alpha_test_mask = gs.AlphaTestMask() != 0xFF;
            id.alpha_test_func = OptimizeRefByteCompare(id.alpha_test_func, id.alpha_test_ref);
            if (id.alpha_test_func == Comparison::kAlways) {
                id.alpha_test_ref = 0;
                id.has_alpha_test_mask = false;
            }
        }

        // Invalid equations (6, 7) do not blend.
        id.alpha_blend = gs.IsAlphaBlendEnabled() && gs.BlendEq() <= 5;
        if (id.alpha_blend && gs.BlendEq() == static_cast<int>(BlendMode::kAdd)) {
            const bool src_fixed_one = gs.BlendFuncA() == 10 && gs.FixA() == 0x00FFFFFF;
            const bool dst_fixed_zero = gs.BlendFuncB() == 10 && gs.FixB() == 0x00000000;
            if (src_fixed_one && dst_fixed_zero) id.alpha_blend = false;
        }
        if (id.alpha_blend) id.blend_eq = static_cast<BlendMode>(gs.BlendEq());
        if (id.alpha_blend && id.blend_eq <= BlendMode::kReverseSubtract) {
            int src = gs.BlendFuncA();
            int dst = gs.BlendFuncB();
            if (src >= 10) src = static_cast<int>(OptimizeAlphaFactor(gs.FixA()));
            if (dst >= 10) dst = static_cast<int>(OptimizeAlphaFactor(gs.FixB()));
            id.blend_src = static_cast<PixelBlendFactor>(src);
            id.blend_dst = static_cast<PixelBlendFactor>(dst);
        }

        if (id.color_test && gs.ColorTestFunction() == Comparison::kNotEqual &&
            gs.ColorTestRef() == 0 && gs.ColorTestMask() == 0xFFFFFF) {
            if (!id.depth_write && !id.stencil_test && id.alpha_blend &&
                id.blend_eq == BlendMode::kAdd && id.blend_dst == PixelBlendFactor::kOne) {
                id.color_test = false;
            }
        }

        id.apply_logic_op = gs.IsLogicOpEnabled() && gs.GetLogicOp() != LogicOp::kCopy;
        id.apply_fog = gs.IsFogEnabled() && !gs.IsModeThrough();

        id.early_z_checks = id.depth_test_func != Comparison::kAlways;
        if (id.stencil_test && id.early_z_checks) {
            if (id.sfail != StencilOp::kKeep || id.zfail != StencilOp::kKeep)
                id.early_z_checks = false;
        }
    }

    if (id.dithering) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                id.dither_matrix[y * 4 + x] = static_cast<std::int8_t>(gs.DitherValue(x, y));
            }
        }
    }
    if (id.apply_color_write_mask) {
        std::uint32_t mask = gs.ColorMask();
        if (!id.stencil_test) mask |= 0xFF000000;
        id.color_write_mask = ConvertWriteMask(id.fb_format, mask);
    }
    if (id.apply_fog) id.fog_color = gs.Reg(kCmdFogColor) & 0x00FFFFFF;
    if (id.apply_logic_op) id.logic_op = gs.GetLogicOp();
    id.minz = gs.DepthRangeMin();
    id.maxz = gs.DepthRangeMax();
    id.fb_stride = gs.FrameBufStride();
    id.depth_stride = gs.DepthBufStride();
    if (id.has_stencil_test_mask) {
        id.stencil_ref = static_cast<std::uint8_t>(gs.StencilTestRef());
        id.stencil_test_mask = static_cast<std::uint8_t>(gs.StencilTestMask());
    }
    if (id.has_alpha_test_mask) id.alpha_test_mask = static_cast<std::uint8_t>(gs.AlphaTestMask());
    if (!id.clear_mode && id.color_test) {
        id.color_test_func = gs.ColorTestFunction();
        id.color_test_mask = gs.ColorTestMask();
        id.color_test_ref = gs.ColorTestRef() & id.color_test_mask;
    }
    if (id.blend_src == PixelBlendFactor::kFix) id.blend_fix_src = gs.FixA();
    if (id.blend_dst == PixelBlendFactor::kFix) id.blend_fix_dst = gs.FixB();

    id.fb_offset = gs.FrameBufRawAddress();
    id.depth_offset = gs.DepthBufRawAddress();
    return id;
}

bool DepthTestPassed(const RenderTarget &rt, Comparison func, int x, int y, int z) {
    const int reference_z = rt.GetDepth(x, y);
    switch (func) {
    case Comparison::kNever:
        return false;
    case Comparison::kAlways:
        return true;
    case Comparison::kEqual:
        return z == reference_z;
    case Comparison::kNotEqual:
        return z != reference_z;
    case Comparison::kLess:
        return z < reference_z;
    case Comparison::kLequal:
        return z <= reference_z;
    case Comparison::kGreater:
        return z > reference_z;
    case Comparison::kGequal:
        return z >= reference_z;
    }
    return false;
}

void DrawPixel(const RenderTarget &rt, int x, int y, int z, int fog, const Vec4i &color_in,
               const PixelState &ps) {
    const bool clear_mode = ps.clear_mode;
    Vec4i prim_color;
    for (int i = 0; i < 4; ++i) prim_color[i] = ClampU8(color_in[i]);

    if (ps.apply_depth_range && !ps.early_z_checks) {
        if (z < ps.minz || z > ps.maxz) return;
    }

    if (ps.alpha_test_func != Comparison::kAlways && !clear_mode) {
        if (!AlphaTestPassed(ps, prim_color.a())) return;
    }

    if (ps.apply_fog && !clear_mode) {
        const Vec4i fog_color = Vec4i::FromRGBA(ps.fog_color);
        for (int i = 0; i < 3; ++i) {
            prim_color[i] = (prim_color[i] * fog + fog_color[i] * (255 - fog) + 255) / 256;
        }
    }

    if (ps.color_test && !clear_mode) {
        if (!ColorTestPassed(ps, prim_color)) return;
    }

    const std::uint32_t write_mask = ps.apply_color_write_mask ? ps.color_write_mask : 0;
    std::uint8_t stencil = clear_mode ? static_cast<std::uint8_t>(prim_color.a())
                                      : GetPixelStencil(rt, ps.fb_format, x, y);
    if (clear_mode) {
        if (ps.depth_write) rt.SetDepth(x, y, static_cast<std::uint16_t>(z));
    } else if (ps.stencil_test) {
        const std::uint8_t replace =
            ps.has_stencil_test_mask ? ps.stencil_ref : ps.stencil_test_ref;
        if (!StencilTestPassed(ps, stencil)) {
            stencil = ApplyStencilOp(ps.fb_format, replace, ps.sfail, stencil);
            SetPixelStencil(rt, ps.fb_format, write_mask, x, y, stencil);
            return;
        }
        if (!ps.early_z_checks && ps.depth_test_func != Comparison::kAlways &&
            !DepthTestPassed(rt, ps.depth_test_func, x, y, z)) {
            stencil = ApplyStencilOp(ps.fb_format, replace, ps.zfail, stencil);
            SetPixelStencil(rt, ps.fb_format, write_mask, x, y, stencil);
            return;
        }
        stencil = ApplyStencilOp(ps.fb_format, replace, ps.zpass, stencil);
    } else if (!ps.early_z_checks) {
        if (ps.depth_test_func != Comparison::kAlways &&
            !DepthTestPassed(rt, ps.depth_test_func, x, y, z)) {
            return;
        }
    }

    if (ps.depth_write && !clear_mode) rt.SetDepth(x, y, static_cast<std::uint16_t>(z));

    const std::uint32_t old_color = GetPixelColor(rt, ps.fb_format, x, y);
    std::uint32_t new_color;
    const int dither = ps.dithering ? ps.dither_matrix[(y & 3) * 4 + (x & 3)] : 0;
    if (ps.alpha_blend && !clear_mode) {
        const Vec4i dst = Vec4i::FromRGBA(old_color);
        Vec4i blended = AlphaBlendingResult(ps, prim_color, dst);
        for (int i = 0; i < 3; ++i) blended[i] += dither;
        new_color = blended.ToRGB() | (static_cast<std::uint32_t>(stencil) << 24);
    } else {
        for (int i = 0; i < 3; ++i) prim_color[i] += dither;
        new_color = prim_color.ToRGB() | (static_cast<std::uint32_t>(stencil) << 24);
    }

    if (ps.apply_logic_op && !clear_mode) {
        new_color = ApplyLogicOp(ps.logic_op, old_color, new_color);
    }

    if (clear_mode) {
        if (!ps.color_test) new_color = (new_color & 0xFF000000) | (old_color & 0x00FFFFFF);
        if (!ps.stencil_test) new_color = (new_color & 0x00FFFFFF) | (old_color & 0xFF000000);
    }

    SetPixelColor(rt, ps.fb_format, x, y, new_color, old_color, write_mask);
}

} // namespace mpo::ge
