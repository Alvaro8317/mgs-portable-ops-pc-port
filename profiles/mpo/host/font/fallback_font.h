// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fallback font of the PPGe dialogs (F23c): GNU FreeFont's FreeSans,
// rasterized ahead of time at the em of the firmware's ltn0.pgf (18 px) by
// make_fallback_font.py into fallback_font_glyphs.inc, which is committed.
// PPGe uses it only when no firmware PGF font is there; the font and its
// license (GPL-3.0-or-later) are in third_party/freefont/NOTICE.md.
//
// It covers what the dialogs draw: ASCII, Latin-1, the typographic
// punctuation of the European languages and the four button symbols
// (○ × △ □). Bitmaps are 8-bit coverage with the GlyphBitmap convention of
// pgf.h (left and top from the pen position, top upwards).

#pragma once

#include <cstdint>
#include <span>

#include "pgf.h"

namespace mpo::font {

struct FallbackGlyph {
    std::uint32_t code;
    std::int32_t advance; // 26.6 pixels
    std::int16_t left;
    std::int16_t top;
    std::uint16_t width;
    std::uint16_t height;
    std::uint32_t offset; // Into the pixel table.
};

class FallbackFont {
public:
    // For reports: what the dialogs draw with.
    static constexpr const char *kName = "FreeSans (GNU FreeFont 20120503)";

    [[nodiscard]] static int em_pixels();
    // Line metrics in 26.6 pixels: the ascender (positive, above the
    // baseline) and the descender (negative).
    [[nodiscard]] static std::int32_t ascender();
    [[nodiscard]] static std::int32_t descender();
    // Sorted by code.
    [[nodiscard]] static std::span<const FallbackGlyph> glyphs();
    [[nodiscard]] static const FallbackGlyph *find(char32_t code);
    // The glyph's coverage (empty for a space). False if the font lacks it.
    static bool rasterize(char32_t code, GlyphBitmap *out);
};

} // namespace mpo::font
