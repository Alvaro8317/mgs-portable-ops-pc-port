// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_font: a synthetic PGF (bit-packed tables with odd widths, H and V row
// glyphs, indexed and explicit metrics, a shadow glyph, revision 2 and 3) is
// parsed and rasterized, and the results are checked against values worked
// out by hand from the format. Truncated and corrupt fonts must not crash.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "font_test_util.h"
#include "pgf.h"

using namespace mpo::font;
using namespace mpo::font::test;

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

void TestGetBits() {
    const uint8_t buf[8] = {0xF0, 0x0F, 0xAA, 0x55, 0x01, 0x02, 0x03, 0x84};
    CHECK_EQ(PgfGetBits(buf, 8, 4, 0), 0x0);
    CHECK_EQ(PgfGetBits(buf, 8, 4, 4), 0xF);
    CHECK_EQ(PgfGetBits(buf, 8, 8, 4), 0xFF);
    CHECK_EQ(PgfGetBits(buf, 8, 32, 0), 0x55AA0FF0u);
    CHECK_EQ(PgfGetBits(buf, 8, 16, 24), 0x0155u); // crosses the word boundary
    CHECK_EQ(PgfGetBits(buf, 8, 1, 63), 1);
    CHECK_EQ(PgfGetBits(buf, 8, 8, 60), 0x08); // runs past the end: zeros
    CHECK_EQ(PgfGetBits(buf, 8, 8, 1000), 0);
    CHECK_EQ(PgfGetBits(buf, 8, 33, 0), 0);
    CHECK_EQ(PgfGetBits(buf, 3, 32, 0), 0xAA0FF0u); // only 3 bytes are valid
}

void TestFont(int revision) {
    const Font f = BuildFont(revision);
    Pgf pgf;
    CHECK(pgf.ReadPtr(f.bytes.data(), f.bytes.size()));
    CHECK(pgf.fileName() == "Synthetic Sans");
    CHECK_EQ(pgf.glyphCount(), 3);
    CHECK_EQ(pgf.firstGlyph(), 0x20);

    PgfFontInfo fi{};
    pgf.GetFontInfo(&fi);
    CHECK_EQ(fi.maxGlyphWidthI, 8 * 64);
    CHECK_EQ(fi.maxGlyphAscenderI, 11 * 64);
    CHECK(fi.maxGlyphDescenderF == -3.0f);
    CHECK(fi.maxGlyphAdvanceYF == 14.0f);
    CHECK_EQ(fi.maxGlyphWidth, 8);
    CHECK_EQ(fi.numGlyphs, 3);
    CHECK_EQ(fi.shadowMapLength, 0);
    CHECK_EQ(fi.BPP, 4);

    PgfCharInfo ci{};
    CHECK(pgf.GetCharInfo('A', &ci, '?'));
    CHECK_EQ(ci.bitmapWidth, 4);
    CHECK_EQ(ci.bitmapHeight, 3);
    CHECK_EQ(ci.bitmapLeft, 1);
    CHECK_EQ(ci.bitmapTop, 10);
    CHECK_EQ(ci.sfp26Width, 4 * 64);
    CHECK_EQ(ci.sfp26Height, 3 * 64);
    CHECK_EQ(ci.sfp26BearingHX, 64);
    CHECK_EQ(ci.sfp26BearingVX, 32);
    CHECK_EQ(ci.sfp26BearingHY, 10 * 64);
    CHECK_EQ(ci.sfp26BearingVY, 5 * 64);
    CHECK_EQ(ci.sfp26Ascender, 10 * 64);
    CHECK_EQ(ci.sfp26Descender, 10 * 64 - 3 * 64);
    CHECK_EQ(ci.sfp26AdvanceH, 6 * 64);
    CHECK_EQ(ci.sfp26AdvanceV, 12 * 64);
    CHECK_EQ(ci.shadowFlags, 0x2A);
    CHECK_EQ(ci.shadowId, 0);

    CHECK(pgf.GetCharInfo('B', &ci, '?'));
    CHECK_EQ(ci.bitmapWidth, 2);
    CHECK_EQ(static_cast<int32_t>(ci.bitmapLeft), -2);
    CHECK_EQ(static_cast<int32_t>(ci.bitmapTop), -3);
    CHECK_EQ(ci.sfp26BearingHX, -128);
    CHECK_EQ(ci.sfp26BearingVX, 7);
    CHECK_EQ(ci.sfp26BearingHY, -192);
    CHECK_EQ(ci.sfp26AdvanceH, 5 * 64);
    CHECK_EQ(ci.sfp26AdvanceV, 9 * 64);
    CHECK_EQ(ci.shadowId, 5);

    // Below firstGlyph: zeroed, false, no alternate.
    std::memset(&ci, 0x55, sizeof(ci));
    CHECK(!pgf.GetCharInfo(0x10, &ci, '?'));
    CHECK_EQ(ci.bitmapWidth, 0);
    // Unmapped: the alternate character is used.
    CHECK(pgf.GetCharInfo('C', &ci, '?'));
    CHECK_EQ(ci.bitmapWidth, 1);
    CHECK_EQ(ci.sfp26AdvanceH, 7 * 64);
    CHECK(!pgf.GetCharInfo('C', &ci, 'D'));
    CHECK(!pgf.HasGlyph('C'));
    CHECK(pgf.HasGlyph('A'));
    // Past the char map the code is used as the glyph index directly.
    CHECK(!pgf.HasGlyph(0x4000));

    GlyphBitmap bmp;
    CHECK(pgf.RasterizeGlyph('A', &bmp));
    CHECK_EQ(bmp.width, 4);
    CHECK_EQ(bmp.height, 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            CHECK_EQ(bmp.pixels[static_cast<size_t>(y * 4 + x)], kGlyphA[y][x] * 0x11);
        }
    }
    CHECK(pgf.RasterizeGlyph('B', &bmp));
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 2; ++x) {
            CHECK_EQ(bmp.pixels[static_cast<size_t>(y * 2 + x)], kGlyphB[y][x] * 0x11);
        }
    }
    CHECK(!pgf.RasterizeGlyph('C', &bmp));

    // Shadow glyph of 'A' (shadow map entry 0 -> glyph 0).
    CHECK(pgf.GetCharInfo('A', &ci, 0, FONT_PGF_SHADOWGLYPH));
    CHECK_EQ(ci.bitmapWidth, 2);
    CHECK_EQ(ci.bitmapHeight, 2);
    CHECK_EQ(static_cast<int32_t>(ci.bitmapLeft), -1);
    CHECK_EQ(ci.bitmapTop, 9);
    CHECK(pgf.RasterizeGlyph('A', &bmp, FONT_PGF_SHADOWGLYPH));
    for (uint8_t p : bmp.pixels) {
        CHECK_EQ(p, 0xCC);
    }
}

void TestDraw() {
    const Font f = BuildFont(2);
    Pgf pgf;
    CHECK(pgf.ReadPtr(f.bytes.data(), f.bytes.size()));

    // 8 bpp at an integer position.
    GlyphImage img{};
    img.pixelFormat = PSP_FONT_PIXELFORMAT_8;
    img.bufWidth = 8;
    img.bufHeight = 6;
    img.bytesPerLine = 8;
    img.xPos64 = 2 * 64;
    img.yPos64 = 1 * 64;
    std::vector<uint8_t> buf(8 * 6, 0x11);
    pgf.DrawCharacter(img, buf, -1, -1, -1, -1, 'A', '?', FONT_PGF_CHARGLYPH);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            CHECK_EQ(buf[static_cast<size_t>((y + 1) * 8 + x + 2)], kGlyphA[y][x] * 0x11);
        }
    }
    CHECK_EQ(buf[0], 0x11); // untouched outside the glyph
    CHECK_EQ(buf[1 * 8 + 6], 0x11);

    // Clip to a 2x2 window.
    std::fill(buf.begin(), buf.end(), uint8_t{0x11});
    pgf.DrawCharacter(img, buf, 3, 2, 2, 2, 'A', '?', FONT_PGF_CHARGLYPH);
    CHECK_EQ(buf[2 * 8 + 3], kGlyphA[1][1] * 0x11);
    CHECK_EQ(buf[3 * 8 + 4], kGlyphA[2][2] * 0x11);
    CHECK_EQ(buf[1 * 8 + 3], 0x11);
    CHECK_EQ(buf[2 * 8 + 2], 0x11);

    // Subpixel position: blend of the four neighbours, as upstream.
    img.xPos64 = 2 * 64 + 16;
    img.yPos64 = 1 * 64 + 32;
    std::fill(buf.begin(), buf.end(), uint8_t{0});
    pgf.DrawCharacter(img, buf, -1, -1, -1, -1, 'A', '?', FONT_PGF_CHARGLYPH);
    auto sample = [](int x, int y) -> uint32_t {
        if (x < 0 || y < 0 || x >= 4 || y >= 3) {
            return 0;
        }
        return kGlyphA[y][x] * 0x11u;
    };
    for (int yy = 0; yy < 4; ++yy) {
        for (int xx = 0; xx < 5; ++xx) {
            const uint32_t h1 = sample(xx - 1, yy - 1) * 16 + sample(xx, yy - 1) * 48;
            const uint32_t h2 = sample(xx - 1, yy) * 16 + sample(xx, yy) * 48;
            const uint32_t want = (h1 * 32 + h2 * 32) >> 12;
            CHECK_EQ(buf[static_cast<size_t>((yy + 1) * 8 + xx + 2)], want);
        }
    }

    // 4 bpp, natural and reversed nibble order, 24 and 32 bpp.
    img.xPos64 = 1 * 64;
    img.yPos64 = 0;
    for (uint32_t fmt : {PSP_FONT_PIXELFORMAT_4, PSP_FONT_PIXELFORMAT_4_REV}) {
        img.pixelFormat = fmt;
        img.bytesPerLine = 4;
        std::vector<uint8_t> b4(4 * 6, 0);
        pgf.DrawCharacter(img, b4, -1, -1, -1, -1, 'A', 0, FONT_PGF_CHARGLYPH);
        // Row 0 of 'A' at x = 1..4: 0 F F 0 (x = 2 and 3 share byte 1).
        CHECK_EQ(b4[0], 0x00);
        CHECK_EQ(b4[1], 0xFF);
        CHECK_EQ(b4[2], 0x00);
        // Row 1: F 0 0 F at x = 1..4. Natural order puts even x in the low
        // nibble; the reversed order in the high one.
        if (fmt == PSP_FONT_PIXELFORMAT_4) {
            CHECK_EQ(b4[4 + 0], 0xF0);
            CHECK_EQ(b4[4 + 2], 0x0F);
        } else {
            CHECK_EQ(b4[4 + 0], 0x0F);
            CHECK_EQ(b4[4 + 2], 0xF0);
        }
    }
    img.pixelFormat = PSP_FONT_PIXELFORMAT_24;
    img.bytesPerLine = 8 * 3;
    std::vector<uint8_t> b24(8 * 3 * 6, 0);
    pgf.DrawCharacter(img, b24, -1, -1, -1, -1, 'A', 0, FONT_PGF_CHARGLYPH);
    CHECK_EQ(b24[2 * 3 + 0], 0xFF);
    CHECK_EQ(b24[2 * 3 + 2], 0xFF);
    CHECK_EQ(b24[2 * 24 + 1 * 3 + 1], 0x88);
    img.pixelFormat = PSP_FONT_PIXELFORMAT_32;
    img.bytesPerLine = 8 * 4;
    std::vector<uint8_t> b32(8 * 4 * 6, 0);
    pgf.DrawCharacter(img, b32, -1, -1, -1, -1, 'A', 0, FONT_PGF_CHARGLYPH);
    uint32_t px;
    std::memcpy(&px, b32.data() + 2 * 4, 4);
    CHECK_EQ(px, 0xFFFFFFFFu);
    std::memcpy(&px, b32.data() + 2 * 32 + 1 * 4, 4);
    CHECK_EQ(px, 0x88888888u);

    // Out of range: buffer smaller than the image claims, bad format, glyph
    // half outside the image. Nothing may be written out of bounds.
    img.pixelFormat = PSP_FONT_PIXELFORMAT_32;
    img.bufWidth = 100;
    img.bufHeight = 100;
    img.bytesPerLine = 400;
    img.xPos64 = -2 * 64;
    img.yPos64 = -1 * 64;
    std::vector<uint8_t> tiny(10, 0);
    pgf.DrawCharacter(img, tiny, -1, -1, -1, -1, 'A', 0, FONT_PGF_CHARGLYPH);
    img.pixelFormat = 9;
    pgf.DrawCharacter(img, tiny, -1, -1, -1, -1, 'A', 0, FONT_PGF_CHARGLYPH);
    // Below firstGlyph nothing is drawn, not even the alternate.
    img.pixelFormat = PSP_FONT_PIXELFORMAT_8;
    img.xPos64 = 0;
    img.yPos64 = 0;
    img.bufWidth = 8;
    img.bufHeight = 6;
    img.bytesPerLine = 8;
    std::fill(buf.begin(), buf.end(), uint8_t{0x11});
    pgf.DrawCharacter(img, buf, -1, -1, -1, -1, 0x10, 'A', FONT_PGF_CHARGLYPH);
    CHECK_EQ(buf[1], 0x11);
    // Unmapped: the alternate is drawn.
    pgf.DrawCharacter(img, buf, -1, -1, -1, -1, 'C', '?', FONT_PGF_CHARGLYPH);
    CHECK_EQ(buf[0], 0x99);
}

void TestRobustness() {
    const Font f = BuildFont(3);
    Pgf pgf;
    for (size_t n = 0; n < f.bytes.size(); ++n) {
        std::vector<uint8_t> cut(f.bytes.begin(), f.bytes.begin() + static_cast<std::ptrdiff_t>(n));
        if (pgf.ReadPtr(cut.data(), cut.size())) {
            GlyphBitmap bmp;
            pgf.RasterizeGlyph('A', &bmp);
            pgf.RasterizeGlyph('B', &bmp);
            pgf.RasterizeGlyph('A', &bmp, FONT_PGF_SHADOWGLYPH);
        }
    }
    CHECK(!pgf.ReadPtr(nullptr, 0));
    CHECK(!pgf.ReadPtr(f.bytes.data(), sizeof(PgfHeader) - 1));
    // Bits per entry above 32 is rejected.
    std::vector<uint8_t> bad = f.bytes;
    bad[offsetof(PgfHeader, charMapBpe)] = 40;
    CHECK(!pgf.ReadPtr(bad.data(), bad.size()));
    // Huge table lengths are rejected, not allocated.
    bad = f.bytes;
    bad[offsetof(PgfHeader, charPointerLength) + 3] = 0x7F;
    CHECK(!pgf.ReadPtr(bad.data(), bad.size()));
    // Random corruption anywhere: must not crash.
    std::mt19937 rng(77);
    for (int round = 0; round < 500; ++round) {
        std::vector<uint8_t> c = f.bytes;
        const int flips = 1 + static_cast<int>(rng() % 16);
        for (int i = 0; i < flips; ++i) {
            c[rng() % c.size()] = static_cast<uint8_t>(rng());
        }
        if (pgf.ReadPtr(c.data(), c.size())) {
            PgfCharInfo ci{};
            GlyphBitmap bmp;
            std::vector<uint8_t> canvas(64 * 64);
            GlyphImage img{};
            img.pixelFormat = static_cast<uint32_t>(rng() % 6);
            img.bufWidth = 64;
            img.bufHeight = 64;
            img.bytesPerLine = static_cast<uint16_t>(16 + rng() % 200);
            img.xPos64 = static_cast<int32_t>(rng() % 4096) - 1024;
            img.yPos64 = static_cast<int32_t>(rng() % 4096) - 1024;
            for (int c2 : {int{'A'}, int{'B'}, int{'?'}, 0x30}) {
                pgf.GetCharInfo(c2, &ci, '?');
                pgf.RasterizeGlyph(c2, &bmp);
                pgf.DrawCharacter(img, canvas, -1, -1, -1, -1, c2, '?', FONT_PGF_CHARGLYPH);
                pgf.DrawCharacter(img, canvas, -1, -1, -1, -1, c2, '?', FONT_PGF_SHADOWGLYPH);
            }
        }
    }
}

} // namespace

int main() {
    TestGetBits();
    TestFont(2);
    TestFont(3);
    TestDraw();
    TestRobustness();
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_font_pgf_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_font_pgf_test: all checks passed\n");
    return 0;
}
