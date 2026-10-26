// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/FuncId.{h,cpp} (PixelFuncID,
// ComputePixelFuncID) and GPU/Software/DrawPixel.cpp (DrawSinglePixel)
// @ v1.20.4 (GPL-2.0-or-later). Only the portable path; the x86 JIT is not
// ported, but where the JIT and the SSE path differ from the plain C++ path
// (saturating blend subtract before dither) this follows the x86-64 build,
// which is what the reference captures come from.

#pragma once

#include <cstdint>

#include "color.hpp"
#include "ge_constants.hpp"
#include "ge_state.hpp"
#include "memory_view.hpp"

namespace mpo::ge {

// 0-10 match the GE blend factors; ZERO and ONE are fixed colours folded.
enum class PixelBlendFactor : std::uint8_t {
    kOtherColor,
    kInvOtherColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDstAlpha,
    kInvDstAlpha,
    kDoubleSrcAlpha,
    kDoubleInvSrcAlpha,
    kDoubleDstAlpha,
    kDoubleInvDstAlpha,
    kFix,
    kZero,
    kOne,
};

// Everything the per-pixel stage needs for one draw (PixelFuncID plus its
// cached values and the buffer addresses).
struct PixelState {
    bool clear_mode = false;
    // In clear mode these three mean "clear colour", "clear stencil" and
    // "clear depth", as in PPSSPP.
    bool color_test = false;
    bool stencil_test = false;
    bool depth_write = false;
    bool apply_depth_range = false;
    Comparison alpha_test_func = Comparison::kAlways;
    Comparison depth_test_func = Comparison::kAlways;
    Comparison stencil_test_func = Comparison::kNever;
    BufferFormat fb_format = BufferFormat::k565;
    std::uint8_t alpha_test_ref = 0;
    std::uint8_t stencil_test_ref = 0;
    bool alpha_blend = false;
    BlendMode blend_eq = BlendMode::kAdd;
    PixelBlendFactor blend_src = PixelBlendFactor::kOtherColor;
    PixelBlendFactor blend_dst = PixelBlendFactor::kOtherColor;
    bool has_alpha_test_mask = false;
    bool has_stencil_test_mask = false;
    bool dithering = false;
    bool apply_logic_op = false;
    bool apply_fog = false;
    bool apply_color_write_mask = false;
    StencilOp sfail = StencilOp::kKeep;
    StencilOp zfail = StencilOp::kKeep;
    StencilOp zpass = StencilOp::kKeep;
    bool early_z_checks = false;

    // Cached values.
    std::uint32_t color_write_mask = 0;
    std::int8_t dither_matrix[16] = {};
    std::uint32_t fog_color = 0;
    int minz = 0;
    int maxz = 0;
    int fb_stride = 0;
    int depth_stride = 0;
    LogicOp logic_op = LogicOp::kCopy;
    std::uint8_t stencil_ref = 0;
    std::uint8_t stencil_test_mask = 0;
    std::uint8_t alpha_test_mask = 0;
    Comparison color_test_func = Comparison::kNever;
    std::uint32_t color_test_mask = 0;
    std::uint32_t color_test_ref = 0;
    std::uint32_t blend_fix_src = 0;
    std::uint32_t blend_fix_dst = 0;

    // VRAM offsets of the colour and depth buffers.
    std::uint32_t fb_offset = 0;
    std::uint32_t depth_offset = 0;
};

PixelState ComputePixelState(const GeState &gs);

// The colour and depth targets in VRAM.
class RenderTarget {
public:
    RenderTarget(const MemoryView &mem, const PixelState &ps) : mem_(mem), ps_(ps) {}

    [[nodiscard]] std::uint32_t ColorOffset16(int x, int y) const {
        return ps_.fb_offset + static_cast<std::uint32_t>((y * ps_.fb_stride + x) * 2);
    }
    [[nodiscard]] std::uint32_t ColorOffset32(int x, int y) const {
        return ps_.fb_offset + static_cast<std::uint32_t>((y * ps_.fb_stride + x) * 4);
    }
    [[nodiscard]] std::uint16_t Get16(int x, int y) const {
        return mem_.VramRead16(ColorOffset16(x, y));
    }
    [[nodiscard]] std::uint32_t Get32(int x, int y) const {
        return mem_.VramRead32(ColorOffset32(x, y));
    }
    void Set16(int x, int y, std::uint16_t v) const { mem_.VramWrite16(ColorOffset16(x, y), v); }
    void Set32(int x, int y, std::uint32_t v) const { mem_.VramWrite32(ColorOffset32(x, y), v); }
    [[nodiscard]] std::uint16_t GetDepth(int x, int y) const {
        return mem_.VramRead16(ps_.depth_offset +
                               static_cast<std::uint32_t>((y * ps_.depth_stride + x) * 2));
    }
    void SetDepth(int x, int y, std::uint16_t v) const {
        mem_.VramWrite16(
            ps_.depth_offset + static_cast<std::uint32_t>((y * ps_.depth_stride + x) * 2), v);
    }

private:
    const MemoryView &mem_;
    const PixelState &ps_;
};

bool DepthTestPassed(const RenderTarget &rt, Comparison func, int x, int y, int z);

// DrawSinglePixel<clearMode, fbFormat>: tests, fog, blending, dither, logic
// op and the write to the framebuffer.
void DrawPixel(const RenderTarget &rt, int x, int y, int z, int fog, const Vec4i &color_in,
               const PixelState &ps);

} // namespace mpo::ge
