// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/Font/PGF.h and Core/Font/PGF.cpp @ v1.20.4.
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later). Upstream notes that
// parts of PGF.cpp were copied from JPCSP (GPL-3.0), "so I guess this really
// makes this file GPL3"; this project is GPL-3.0-or-later, so that is fine.
//
// Reader and glyph rasterizer for the PSP's PGF fonts (sceFont). The structs
// with a guest layout (PgfCharInfo, PgfFontInfo, PgfFontStyle, GlyphImage)
// match the PSP's and are checked with static_assert.
//
// Changes from upstream:
//   - every bit read is bounds-checked against the font data (upstream reads
//     past the end on corrupt or truncated files) and the header tables are
//     checked against the file size before they are read;
//   - DrawCharacter writes into a span (F17b builds it over guest memory)
//     instead of PSP addresses; out-of-range pixels are skipped;
//   - RasterizeGlyph() (not upstream) returns a glyph as an 8-bit bitmap;
//   - no savestate code, no logging.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mpo::font {

enum {
    FONT_FILETYPE_PGF = 0x00,
    FONT_FILETYPE_BWFON = 0x01,
};

enum {
    FONT_PGF_BMP_H_ROWS = 0x01,
    FONT_PGF_BMP_V_ROWS = 0x02,
    FONT_PGF_BMP_OVERLAY = 0x03,
    // Metric names according to JPCSP findings
    FONT_PGF_METRIC_DIMENSION_INDEX = 0x04,
    FONT_PGF_METRIC_BEARING_X_INDEX = 0x08,
    FONT_PGF_METRIC_BEARING_Y_INDEX = 0x10,
    FONT_PGF_METRIC_ADVANCE_INDEX = 0x20,
    FONT_PGF_CHARGLYPH = 0x20,
    FONT_PGF_SHADOWGLYPH = 0x40,
};

enum Family {
    FONT_FAMILY_SANS_SERIF = 1,
    FONT_FAMILY_SERIF = 2,
};

enum Style {
    FONT_STYLE_REGULAR = 1,
    FONT_STYLE_ITALIC = 2,
    FONT_STYLE_BOLD = 5,
    FONT_STYLE_BOLD_ITALIC = 6,
    FONT_STYLE_DB = 103, // Demi-Bold / semi-bold
};

enum Language {
    FONT_LANGUAGE_JAPANESE = 1,
    FONT_LANGUAGE_LATIN = 2,
    FONT_LANGUAGE_KOREAN = 3,
    FONT_LANGUAGE_CHINESE = 4,
};

enum FontPixelFormat : uint32_t {
    PSP_FONT_PIXELFORMAT_4 = 0,     // 2 pixels packed in 1 byte (natural order)
    PSP_FONT_PIXELFORMAT_4_REV = 1, // 2 pixels packed in 1 byte (reversed order)
    PSP_FONT_PIXELFORMAT_8 = 2,     // 1 pixel in 1 byte
    PSP_FONT_PIXELFORMAT_24 = 3,    // 1 pixel in 3 bytes (RGB)
    PSP_FONT_PIXELFORMAT_32 = 4,    // 1 pixel in 4 bytes (RGBA)
};

#pragma pack(push, 1)

struct PgfFontStyle {
    float fontH;
    float fontV;
    float fontHRes;
    float fontVRes;
    float fontWeight;
    uint16_t fontFamily;
    uint16_t fontStyle;
    // Check.
    uint16_t fontStyleSub;
    uint16_t fontLanguage;
    uint16_t fontRegion;
    uint16_t fontCountry;
    char fontName[64];
    char fontFileName[64];
    uint32_t fontAttributes;
    uint32_t fontExpire;
};

// SceFontGlyphImage as passed to sceFontGetCharGlyphImage(_Clip). F17b reads
// it from guest memory and gives the buffer at bufferPtr as a span.
struct GlyphImage {
    uint32_t pixelFormat; // FontPixelFormat
    int32_t xPos64;
    int32_t yPos64;
    uint16_t bufWidth;
    uint16_t bufHeight;
    uint16_t bytesPerLine;
    uint16_t pad;
    uint32_t bufferPtr;
};

struct PgfHeader {
    uint16_t headerOffset;
    uint16_t headerSize;

    char PGFMagic[4];
    int32_t revision;
    int32_t version;

    int32_t charMapLength;
    int32_t charPointerLength;
    int32_t charMapBpe;
    int32_t charPointerBpe;

    // TODO (upstream): This has values in it (0404)...
    uint8_t pad1[2];
    uint8_t bpp;
    uint8_t pad2[1];

    int32_t hSize;
    int32_t vSize;
    int32_t hResolution;
    int32_t vResolution;

    uint8_t pad3[1];
    char fontName[64];
    char fontType[64];
    uint8_t pad4[1];

    uint16_t firstGlyph;
    uint16_t lastGlyph;

    // TODO (upstream): This has a few 01s in it in the official fonts.
    uint8_t pad5[26];

    int32_t maxAscender;
    int32_t maxDescender;
    int32_t maxLeftXAdjust;
    int32_t maxBaseYAdjust;
    int32_t minCenterXAdjust;
    int32_t maxTopYAdjust;

    int32_t maxAdvance[2];
    int32_t maxSize[2];
    uint16_t maxGlyphWidth;
    uint16_t maxGlyphHeight;
    uint8_t pad6[2];

    uint8_t dimTableLength;
    uint8_t xAdjustTableLength;
    uint8_t yAdjustTableLength;
    uint8_t advanceTableLength;
    uint8_t pad7[102];

    int32_t shadowMapLength;
    int32_t shadowMapBpe;
    float unknown1;
    int32_t shadowScale[2];
    uint8_t pad8[8];
};

struct PgfHeaderRev3Extra {
    int32_t compCharMapBpe1;
    int32_t compCharMapLength1;
    int32_t compCharMapBpe2;
    int32_t compCharMapLength2;
    uint32_t unknown;
};

// SceFontCharInfo.
struct PgfCharInfo {
    uint32_t bitmapWidth;
    uint32_t bitmapHeight;
    uint32_t bitmapLeft;
    uint32_t bitmapTop;
    // Glyph metrics (in 26.6 signed fixed-point).
    uint32_t sfp26Width;
    uint32_t sfp26Height;
    int32_t sfp26Ascender;
    int32_t sfp26Descender;
    int32_t sfp26BearingHX;
    int32_t sfp26BearingHY;
    int32_t sfp26BearingVX;
    int32_t sfp26BearingVY;
    int32_t sfp26AdvanceH;
    int32_t sfp26AdvanceV;
    int16_t shadowFlags;
    int16_t shadowId;
};

// SceFontInfo.
struct PgfFontInfo {
    // Glyph metrics (in 26.6 signed fixed-point).
    int32_t maxGlyphWidthI;
    int32_t maxGlyphHeightI;
    int32_t maxGlyphAscenderI;
    int32_t maxGlyphDescenderI;
    int32_t maxGlyphLeftXI;
    int32_t maxGlyphBaseYI;
    int32_t minGlyphCenterXI;
    int32_t maxGlyphTopYI;
    int32_t maxGlyphAdvanceXI;
    int32_t maxGlyphAdvanceYI;

    // Glyph metrics (replicated as float).
    float maxGlyphWidthF;
    float maxGlyphHeightF;
    float maxGlyphAscenderF;
    float maxGlyphDescenderF;
    float maxGlyphLeftXF;
    float maxGlyphBaseYF;
    float minGlyphCenterXF;
    float maxGlyphTopYF;
    float maxGlyphAdvanceXF;
    float maxGlyphAdvanceYF;

    // Bitmap dimensions.
    int16_t maxGlyphWidth;
    int16_t maxGlyphHeight;
    int32_t numGlyphs;
    int32_t shadowMapLength; // Number of elements in the font's shadow charmap.

    // Font style (used by font comparison functions).
    PgfFontStyle fontStyle;

    uint8_t BPP; // Font's BPP.
    uint8_t pad[3];
};

#pragma pack(pop)

static_assert(sizeof(PgfFontStyle) == 168);
static_assert(sizeof(GlyphImage) == 24);
static_assert(sizeof(PgfHeader) == 0x188);
static_assert(sizeof(PgfHeaderRev3Extra) == 20);
static_assert(sizeof(PgfCharInfo) == 60);
static_assert(sizeof(PgfFontInfo) == 264);

struct Glyph {
    int w = 0;
    int h = 0;
    int left = 0;
    int top = 0;
    int flags = 0;
    int shadowFlags = 0;
    int shadowID = 0;
    int advanceH = 0;
    int advanceV = 0;
    int dimensionWidth = 0, dimensionHeight = 0;
    int xAdjustH = 0, xAdjustV = 0;
    int yAdjustH = 0, yAdjustV = 0;
    uint32_t ptr = 0; // byte offset of the bitmap in the font data
};

// A glyph decoded to one byte per pixel (0..255), rows top to bottom.
struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int left = 0; // bitmap offset from the pen position, in pixels
    int top = 0;
    std::vector<uint8_t> pixels;
};

class Pgf {
public:
    // Parses a whole .pgf file. Returns false (and leaves the font empty) if
    // the header or the tables do not fit in `dataSize` bytes.
    bool ReadPtr(const uint8_t *ptr, size_t dataSize);

    bool GetCharInfo(int charCode, PgfCharInfo *ci, int altCharCode,
                     int glyphType = FONT_PGF_CHARGLYPH) const;
    void GetFontInfo(PgfFontInfo *fi) const;
    // Draws a glyph into `buffer`, which holds the image's
    // bytesPerLine * bufHeight bytes; image.bufferPtr is not used.
    void DrawCharacter(const GlyphImage &image, std::span<uint8_t> buffer, int clipX, int clipY,
                       int clipWidth, int clipHeight, int charCode, int altCharCode,
                       int glyphType) const;
    // Decodes a glyph to 8 bits per pixel. False if the font has no such
    // glyph (no alternate character is tried) or its bitmap is invalid.
    bool RasterizeGlyph(int charCode, GlyphBitmap *out, int glyphType = FONT_PGF_CHARGLYPH) const;
    // Whether the font maps this character to a glyph.
    bool HasGlyph(int charCode, int glyphType = FONT_PGF_CHARGLYPH) const;

    const PgfHeader &header() const { return header_; }
    const std::string &fileName() const { return fileName_; }
    int firstGlyph() const { return firstGlyph_; }
    size_t glyphCount() const { return glyphs_.size(); }

private:
    bool ReadCharGlyph(size_t charPtr, Glyph &glyph) const;
    bool ReadShadowGlyph(size_t charPtr, Glyph &glyph) const;
    bool GetCharGlyph(int charCode, int glyphType, Glyph &glyph) const;
    bool DecodeGlyphPixels(const Glyph &glyph, std::vector<uint8_t> *decodedPixels) const;
    void SetFontPixel(std::span<uint8_t> buffer, int bpl, int bufWidth, int bufHeight, int x, int y,
                      uint8_t pixelColor, uint32_t pixelformat) const;
    // Bits of the font data; reads past the end return 0.
    uint32_t getBits(int numBits, size_t pos) const;
    uint32_t consumeBits(int numBits, size_t &pos) const;

    PgfHeader header_{};
    PgfHeaderRev3Extra rev3extra_{};

    // Font character image data
    std::vector<uint8_t> fontData_;
    std::string fileName_;

    std::vector<int> dimensionTable_[2];
    std::vector<int> xAdjustTable_[2];
    std::vector<int> yAdjustTable_[2];
    std::vector<int> advanceTable_[2];

    // Unused (upstream reads them too)
    std::vector<int> charmapCompressionTable1_[2];
    std::vector<int> charmapCompressionTable2_[2];

    std::vector<int> charmap_;
    std::vector<Glyph> glyphs_;
    std::vector<Glyph> shadowGlyphs_;
    int firstGlyph_ = 0;
};

// Bit reader used by the PGF format: bit i is bit (i % 32) of the i / 32-th
// little-endian 32-bit word. Reads past `size` bytes return 0. Exposed for
// the tests.
uint32_t PgfGetBits(const uint8_t *buf, size_t size, int numBits, size_t pos);

} // namespace mpo::font
