// SPDX-License-Identifier: GPL-3.0-or-later

#include "fallback_font.h"

#include <algorithm>
#include <cstddef>

namespace mpo::font {

namespace {

#include "fallback_font_glyphs.inc"

static_assert(std::size(kFallbackGlyphs) > 0);

} // namespace

int FallbackFont::em_pixels() {
    return kFallbackEmPixels;
}

std::int32_t FallbackFont::ascender() {
    return kFallbackAscender;
}

std::int32_t FallbackFont::descender() {
    return kFallbackDescender;
}

std::span<const FallbackGlyph> FallbackFont::glyphs() {
    return kFallbackGlyphs;
}

const FallbackGlyph *FallbackFont::find(char32_t code) {
    const auto *end = std::end(kFallbackGlyphs);
    const auto *it =
        std::lower_bound(std::begin(kFallbackGlyphs), end, code,
                         [](const FallbackGlyph &g, char32_t c) { return g.code < c; });
    return it != end && it->code == code ? it : nullptr;
}

bool FallbackFont::rasterize(char32_t code, GlyphBitmap *out) {
    const FallbackGlyph *g = find(code);
    if (g == nullptr || out == nullptr) return false;
    out->width = g->width;
    out->height = g->height;
    out->left = g->left;
    out->top = g->top;
    const std::size_t size = static_cast<std::size_t>(g->width) * g->height;
    if (g->offset + size > std::size(kFallbackPixels)) {
        out->pixels.clear();
        return false;
    }
    out->pixels.assign(kFallbackPixels + g->offset, kFallbackPixels + g->offset + size);
    return true;
}

} // namespace mpo::font
