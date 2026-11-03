// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_font (F23c): the fallback font of the PPGe dialogs has a glyph for
// every character the dialogs draw (ASCII, Latin-1, the button symbols), its
// table is well formed, and the glyphs that sit on a line fit inside the
// line metrics PPGe lays the text out with.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "fallback_font.h"

using mpo::font::FallbackFont;
using mpo::font::FallbackGlyph;
using mpo::font::GlyphBitmap;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const long long ca = static_cast<long long>(a);                                            \
        const long long cb = static_cast<long long>(b);                                            \
        if (ca != cb) {                                                                            \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__,    \
                         __LINE__, #a, #b, ca, cb);                                                \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// What the dialogs draw: printable ASCII (the OSK of MPO asks for digits and
// uppercase letters, F18b), Latin-1 and the four button symbols.
std::vector<char32_t> needed() {
    std::vector<char32_t> codes;
    for (char32_t c = 0x20; c < 0x7F; ++c) codes.push_back(c);
    for (char32_t c = 0xA0; c < 0x100; ++c) codes.push_back(c);
    for (char32_t c : {U'\u25CB', U'\u00D7', U'\u25B3', U'\u25A1', U'\u2026', U'\u201C', U'\u201D',
                       U'\u2018', U'\u2019', U'\u2122', U'\u20AC', U'\u0152'})
        codes.push_back(c);
    return codes;
}

void TestTable() {
    const auto glyphs = FallbackFont::glyphs();
    CHECK(glyphs.size() >= 200u);
    for (std::size_t i = 1; i < glyphs.size(); ++i) CHECK(glyphs[i - 1].code < glyphs[i].code);
    CHECK_EQ(FallbackFont::em_pixels(), 18);
    // A line of 18 px at the em of ltn0.pgf (18 px), as PPGe expects.
    CHECK(FallbackFont::ascender() > 0);
    CHECK(FallbackFont::descender() < 0);
    CHECK_EQ(FallbackFont::ascender() - FallbackFont::descender(), 18 * 64);
    for (const FallbackGlyph &g : glyphs) {
        GlyphBitmap b;
        CHECK(FallbackFont::rasterize(g.code, &b));
        CHECK_EQ(b.pixels.size(),
                 static_cast<std::size_t>(b.width) * static_cast<std::size_t>(b.height));
        CHECK(g.advance > 0);
    }
    GlyphBitmap none;
    CHECK(!FallbackFont::rasterize(U'\u3042', &none));
    CHECK(FallbackFont::find(0x1F600) == nullptr);
}

void TestCoverage() {
    for (const char32_t c : needed()) {
        const FallbackGlyph *g = FallbackFont::find(c);
        CHECK(g != nullptr);
        if (g == nullptr) {
            std::fprintf(stderr, "  missing U+%04X\n", static_cast<unsigned>(c));
            continue;
        }
        if (c == U' ' || c == 0xA0 || c == 0xAD) continue; // Blank (0xAD: soft hyphen).
        GlyphBitmap b;
        CHECK(FallbackFont::rasterize(c, &b));
        CHECK(b.width > 0 && b.height > 0);
        std::uint8_t peak = 0;
        for (const std::uint8_t p : b.pixels) peak = p > peak ? p : peak;
        // Ink that survives PPGe's 4-bit palette.
        CHECK(peak >= 0x80u);
    }
    // The OSK's characters and the button symbols sit inside the line.
    const int ascender = FallbackFont::ascender() / 64;
    const int descender = FallbackFont::descender() / 64;
    std::vector<char32_t> line;
    for (char32_t c = U'0'; c <= U'9'; ++c) line.push_back(c);
    for (char32_t c = U'A'; c <= U'Z'; ++c) line.push_back(c);
    for (char32_t c = U'a'; c <= U'z'; ++c) line.push_back(c);
    for (char32_t c : {U'\u25CB', U'\u00D7', U'\u25B3', U'\u25A1', U'_', U'(', U')'})
        line.push_back(c);
    for (const char32_t c : line) {
        const FallbackGlyph *g = FallbackFont::find(c);
        if (g == nullptr) continue;
        CHECK(g->top <= ascender);
        CHECK(g->top - g->height >= descender);
    }
    // Capitals and digits are about as tall as in ltn0 (13 px of 18).
    const FallbackGlyph *h = FallbackFont::find(U'H');
    CHECK(h != nullptr && h->height >= 12 && h->height <= 14);
}

} // namespace

int main() {
    TestTable();
    TestCoverage();
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_font_fallback_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_font_fallback_test: all checks passed\n");
    return 0;
}
