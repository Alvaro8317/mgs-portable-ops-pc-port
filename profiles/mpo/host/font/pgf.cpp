// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/Font/PGF.cpp @ v1.20.4 ("Thanks to the JPCSP
// project! This sceFont implementation is basically a C++ take on JPCSP's
// font code."). Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later; parts
// from JPCSP, GPL-3.0). See pgf.h for the changes from upstream.

#include "pgf.h"

#include <algorithm>
#include <cstring>

namespace mpo::font {
namespace {

// These fonts, created by ttf2pgf, don't have complete glyph info and need
// to be identified.
bool isJPCSPFont(const char *fontName) {
    return !std::strcmp(fontName, "Liberation Sans") || !std::strcmp(fontName, "Liberation Serif") ||
           !std::strcmp(fontName, "Sazanami") || !std::strcmp(fontName, "UnDotum") ||
           !std::strcmp(fontName, "Microsoft YaHei");
}

uint32_t readWord(const uint8_t *buf, size_t size, size_t wordpos) {
    uint32_t v = 0;
    const size_t byte = wordpos * 4;
    for (size_t i = 0; i < 4; ++i) {
        if (byte + i < size) {
            v |= static_cast<uint32_t>(buf[byte + i]) << (8 * i);
        }
    }
    return v;
}

std::vector<int> getTable(const uint8_t *buf, size_t size, int bpe, size_t length) {
    std::vector<int> vec(length);
    for (size_t i = 0; i < length; i++) {
        vec[i] = static_cast<int>(PgfGetBits(buf, size, bpe, static_cast<size_t>(bpe) * i));
    }
    return vec;
}

// Size in bytes of a bit-packed table, padded to 32 bits as in the file.
size_t packedTableBytes(int32_t length, int32_t bpe) {
    const uint64_t bits = static_cast<uint64_t>(length) * static_cast<uint64_t>(bpe);
    return static_cast<size_t>(((bits + 31) & ~static_cast<uint64_t>(31)) / 8);
}

} // namespace

// Gets a number of bits from an offset.
uint32_t PgfGetBits(const uint8_t *buf, size_t size, int numBits, size_t pos) {
    if (numBits <= 0 || numBits > 32) {
        return 0;
    }
    const size_t wordpos = pos >> 5;
    const unsigned bitoff = static_cast<unsigned>(pos & 31);

    // Might just be in one, has to be within two.
    if (bitoff + static_cast<unsigned>(numBits) < 32) {
        const uint32_t mask = (1u << numBits) - 1;
        return (readWord(buf, size, wordpos) >> bitoff) & mask;
    }
    uint32_t v = readWord(buf, size, wordpos) >> bitoff;
    const unsigned done = 32 - bitoff;
    const unsigned remaining = static_cast<unsigned>(numBits) - done;
    if (remaining > 0) {
        const uint32_t mask = (1u << remaining) - 1;
        v |= (readWord(buf, size, wordpos + 1) & mask) << done;
    }
    return v;
}

uint32_t Pgf::getBits(int numBits, size_t pos) const {
    return PgfGetBits(fontData_.data(), fontData_.size(), numBits, pos);
}

uint32_t Pgf::consumeBits(int numBits, size_t &pos) const {
    const uint32_t v = getBits(numBits, pos);
    pos += static_cast<size_t>(numBits);
    return v;
}

bool Pgf::ReadPtr(const uint8_t *ptr, size_t dataSize) {
    *this = Pgf{};
    const uint8_t *const startPtr = ptr;
    const uint8_t *const endPtr = ptr + dataSize;
    auto fits = [&](const uint8_t *p, size_t bytes) {
        return p >= startPtr && p <= endPtr && static_cast<size_t>(endPtr - p) >= bytes;
    };

    if (ptr == nullptr || dataSize < sizeof(header_)) {
        return false;
    }
    std::memcpy(&header_, ptr, sizeof(header_));
    ptr += sizeof(header_);
    // Terminate the names, whatever the file holds.
    header_.fontName[sizeof(header_.fontName) - 1] = '\0';
    header_.fontType[sizeof(header_.fontType) - 1] = '\0';
    fileName_ = header_.fontName;

    if (header_.charMapLength < 0 || header_.charPointerLength < 0 || header_.shadowMapLength < 0 ||
        header_.charMapBpe < 0 || header_.charMapBpe > 32 || header_.charPointerBpe < 0 ||
        header_.charPointerBpe > 32 || header_.shadowMapBpe < 0 || header_.shadowMapBpe > 32) {
        *this = Pgf{};
        return false;
    }

    if (header_.revision == 3) {
        if (!fits(ptr, sizeof(rev3extra_))) {
            *this = Pgf{};
            return false;
        }
        std::memcpy(&rev3extra_, ptr, sizeof(rev3extra_));
        rev3extra_.compCharMapLength1 &= 0xFFFF;
        rev3extra_.compCharMapLength2 &= 0xFFFF;
        ptr += sizeof(rev3extra_);
    }

    // Four tables of (u32, u32) pairs.
    const size_t pairTables = static_cast<size_t>(header_.dimTableLength) + header_.xAdjustTableLength +
                              header_.yAdjustTableLength + header_.advanceTableLength;
    if (!fits(ptr, pairTables * 8)) {
        *this = Pgf{};
        return false;
    }
    auto readPairs = [&ptr](std::vector<int> (&table)[2], int length) {
        table[0].resize(static_cast<size_t>(length));
        table[1].resize(static_cast<size_t>(length));
        for (int i = 0; i < length; i++) {
            uint32_t a;
            uint32_t b;
            std::memcpy(&a, ptr, 4);
            std::memcpy(&b, ptr + 4, 4);
            ptr += 8;
            table[0][static_cast<size_t>(i)] = static_cast<int>(a);
            table[1][static_cast<size_t>(i)] = static_cast<int>(b);
        }
    };
    readPairs(dimensionTable_, header_.dimTableLength);
    readPairs(xAdjustTable_, header_.xAdjustTableLength);
    readPairs(yAdjustTable_, header_.yAdjustTableLength);
    readPairs(advanceTable_, header_.advanceTableLength);

    const size_t shadowCharMapSize = packedTableBytes(header_.shadowMapLength, header_.shadowMapBpe);
    if (!fits(ptr, shadowCharMapSize)) {
        *this = Pgf{};
        return false;
    }
    const uint8_t *shadowCharMap = ptr;
    ptr += shadowCharMapSize;
    if (ptr >= endPtr) {
        *this = Pgf{};
        return false;
    }

    if (header_.revision == 3) {
        const size_t compBytes =
            (static_cast<size_t>(rev3extra_.compCharMapLength1) +
             static_cast<size_t>(rev3extra_.compCharMapLength2)) * 4;
        if (!fits(ptr, compBytes)) {
            *this = Pgf{};
            return false;
        }
        auto readU16Pairs = [&ptr](std::vector<int> (&table)[2], int length) {
            table[0].resize(static_cast<size_t>(length));
            table[1].resize(static_cast<size_t>(length));
            for (int i = 0; i < length; i++) {
                uint16_t a;
                uint16_t b;
                std::memcpy(&a, ptr, 2);
                std::memcpy(&b, ptr + 2, 2);
                ptr += 4;
                table[0][static_cast<size_t>(i)] = a;
                table[1][static_cast<size_t>(i)] = b;
            }
        };
        readU16Pairs(charmapCompressionTable1_, rev3extra_.compCharMapLength1);
        readU16Pairs(charmapCompressionTable2_, rev3extra_.compCharMapLength2);
    }

    const size_t charMapSize = packedTableBytes(header_.charMapLength, header_.charMapBpe);
    if (!fits(ptr, charMapSize)) {
        *this = Pgf{};
        return false;
    }
    const uint8_t *charMap = ptr;
    ptr += charMapSize;

    const size_t charPointerSize = packedTableBytes(header_.charPointerLength, header_.charPointerBpe);
    if (!fits(ptr, charPointerSize)) {
        *this = Pgf{};
        return false;
    }
    const uint8_t *charPointerTable = ptr;
    ptr += charPointerSize;

    if (ptr >= endPtr) {
        *this = Pgf{};
        return false;
    }

    // PGF Fontdata.
    fontData_.assign(ptr, endPtr);

    charmap_.resize(static_cast<size_t>(header_.charMapLength));
    glyphs_.resize(static_cast<size_t>(header_.charPointerLength));
    shadowGlyphs_.resize(static_cast<size_t>(header_.charPointerLength));
    firstGlyph_ = header_.firstGlyph;

    // Parse out the char map (array where each entry is an irregular number
    // of bits). BPE = bits per entry, I think.
    for (int i = 0; i < header_.charMapLength; i++) {
        int v = static_cast<int>(PgfGetBits(charMap, charMapSize, header_.charMapBpe,
                                            static_cast<size_t>(i) * static_cast<size_t>(header_.charMapBpe)));
        // This check seems a little odd.
        if (v < 0 || static_cast<size_t>(v) >= glyphs_.size()) {
            v = 65535;
        }
        charmap_[static_cast<size_t>(i)] = v;
    }

    const std::vector<int> charPointers =
        getTable(charPointerTable, charPointerSize, header_.charPointerBpe, glyphs_.size());
    const std::vector<int> shadowMap =
        getTable(shadowCharMap, shadowCharMapSize, header_.shadowMapBpe,
                 static_cast<size_t>(header_.shadowMapLength));

    // Pregenerate glyphs.
    for (size_t i = 0; i < glyphs_.size(); i++) {
        ReadCharGlyph(static_cast<size_t>(static_cast<uint32_t>(charPointers[i])) * 4 * 8 /* ??? */,
                      glyphs_[i]);
    }

    // And shadow glyphs.
    for (size_t i = 0; i < glyphs_.size(); i++) {
        const size_t shadowId = static_cast<size_t>(glyphs_[i].shadowID);
        if (shadowId < shadowMap.size()) {
            const size_t charId = static_cast<size_t>(shadowMap[shadowId]);
            if (charId < shadowGlyphs_.size()) {
                // TODO (upstream): check for pre existing shadow glyph
                ReadShadowGlyph(static_cast<size_t>(static_cast<uint32_t>(charPointers[charId])) * 4 * 8,
                                shadowGlyphs_[charId]);
            }
        }
    }
    return true;
}

bool Pgf::GetCharInfo(int charCode, PgfCharInfo *charInfo, int altCharCode, int glyphType) const {
    Glyph glyph;
    std::memset(charInfo, 0, sizeof(*charInfo));

    if (!GetCharGlyph(charCode, glyphType, glyph)) {
        if (charCode < firstGlyph_) {
            // Character not in font, return zeroed charInfo as on real PSP.
            return false;
        }
        if (!GetCharGlyph(altCharCode, glyphType, glyph)) {
            return false;
        }
    }

    charInfo->bitmapWidth = static_cast<uint32_t>(glyph.w);
    charInfo->bitmapHeight = static_cast<uint32_t>(glyph.h);
    charInfo->bitmapLeft = static_cast<uint32_t>(glyph.left);
    charInfo->bitmapTop = static_cast<uint32_t>(glyph.top);
    charInfo->sfp26Width = static_cast<uint32_t>(glyph.dimensionWidth);
    charInfo->sfp26Height = static_cast<uint32_t>(glyph.dimensionHeight);
    charInfo->sfp26Ascender = glyph.yAdjustH;
    // Font y goes upwards.  If top is 10 and height is 11, the descender is
    // approx. -1 (below 0.)
    charInfo->sfp26Descender = charInfo->sfp26Ascender - static_cast<int32_t>(charInfo->sfp26Height);
    charInfo->sfp26BearingHX = glyph.xAdjustH;
    charInfo->sfp26BearingHY = glyph.yAdjustH;
    charInfo->sfp26BearingVX = glyph.xAdjustV;
    charInfo->sfp26BearingVY = glyph.yAdjustV;
    charInfo->sfp26AdvanceH = glyph.advanceH;
    charInfo->sfp26AdvanceV = glyph.advanceV;
    charInfo->shadowFlags = static_cast<int16_t>(glyph.shadowFlags);
    charInfo->shadowId = static_cast<int16_t>(glyph.shadowID);
    return true;
}

void Pgf::GetFontInfo(PgfFontInfo *fi) const {
    fi->maxGlyphWidthI = header_.maxSize[0];
    fi->maxGlyphHeightI = header_.maxSize[1];
    fi->maxGlyphAscenderI = header_.maxAscender;
    fi->maxGlyphDescenderI = header_.maxDescender;
    fi->maxGlyphLeftXI = header_.maxLeftXAdjust;
    fi->maxGlyphBaseYI = header_.maxBaseYAdjust;
    fi->minGlyphCenterXI = header_.minCenterXAdjust;
    fi->maxGlyphTopYI = header_.maxTopYAdjust;
    fi->maxGlyphAdvanceXI = header_.maxAdvance[0];
    fi->maxGlyphAdvanceYI = header_.maxAdvance[1];
    fi->maxGlyphWidthF = static_cast<float>(header_.maxSize[0]) / 64.0f;
    fi->maxGlyphHeightF = static_cast<float>(header_.maxSize[1]) / 64.0f;
    fi->maxGlyphAscenderF = static_cast<float>(header_.maxAscender) / 64.0f;
    fi->maxGlyphDescenderF = static_cast<float>(header_.maxDescender) / 64.0f;
    fi->maxGlyphLeftXF = static_cast<float>(header_.maxLeftXAdjust) / 64.0f;
    fi->maxGlyphBaseYF = static_cast<float>(header_.maxBaseYAdjust) / 64.0f;
    fi->minGlyphCenterXF = static_cast<float>(header_.minCenterXAdjust) / 64.0f;
    fi->maxGlyphTopYF = static_cast<float>(header_.maxTopYAdjust) / 64.0f;
    fi->maxGlyphAdvanceXF = static_cast<float>(header_.maxAdvance[0]) / 64.0f;
    fi->maxGlyphAdvanceYF = static_cast<float>(header_.maxAdvance[1]) / 64.0f;

    fi->maxGlyphWidth = static_cast<int16_t>(header_.maxGlyphWidth);
    fi->maxGlyphHeight = static_cast<int16_t>(header_.maxGlyphHeight);
    fi->numGlyphs = header_.charPointerLength;
    fi->shadowMapLength = 0; // header.shadowMapLength; TODO (upstream)

    fi->BPP = header_.bpp;
}

bool Pgf::ReadShadowGlyph(size_t charPtr, Glyph &glyph) const {
    // Most of the glyph info is from the char data.
    if (!ReadCharGlyph(charPtr, glyph)) {
        return false;
    }

    // Skip over the char data.
    if (charPtr + 96 > fontData_.size() * 8) {
        return false;
    }
    charPtr += static_cast<size_t>(getBits(14, charPtr)) * 8;
    if (charPtr + 96 > fontData_.size() * 8) {
        return false;
    }

    // Skip size.
    charPtr += 14;

    glyph.w = static_cast<int>(consumeBits(7, charPtr));
    glyph.h = static_cast<int>(consumeBits(7, charPtr));

    glyph.left = static_cast<int>(consumeBits(7, charPtr));
    if (glyph.left >= 64) {
        glyph.left -= 128;
    }

    glyph.top = static_cast<int>(consumeBits(7, charPtr));
    if (glyph.top >= 64) {
        glyph.top -= 128;
    }

    glyph.ptr = static_cast<uint32_t>(charPtr / 8);
    return true;
}

bool Pgf::ReadCharGlyph(size_t charPtr, Glyph &glyph) const {
    // Skip size.
    charPtr += 14;

    glyph.w = static_cast<int>(consumeBits(7, charPtr));
    glyph.h = static_cast<int>(consumeBits(7, charPtr));

    glyph.left = static_cast<int>(consumeBits(7, charPtr));
    if (glyph.left >= 64) {
        glyph.left -= 128;
    }

    glyph.top = static_cast<int>(consumeBits(7, charPtr));
    if (glyph.top >= 64) {
        glyph.top -= 128;
    }

    glyph.flags = static_cast<int>(consumeBits(6, charPtr));

    glyph.shadowFlags = static_cast<int>(consumeBits(2, charPtr) << (2 + 3));
    glyph.shadowFlags |= static_cast<int>(consumeBits(2, charPtr) << 3);
    glyph.shadowFlags |= static_cast<int>(consumeBits(3, charPtr));

    glyph.shadowID = static_cast<int>(consumeBits(9, charPtr));

    const bool jpcsp = isJPCSPFont(fileName_.c_str());
    if ((glyph.flags & FONT_PGF_METRIC_DIMENSION_INDEX) == FONT_PGF_METRIC_DIMENSION_INDEX) {
        const int dimensionIndex = static_cast<int>(consumeBits(8, charPtr));

        if (dimensionIndex < header_.dimTableLength) {
            glyph.dimensionWidth = dimensionTable_[0][static_cast<size_t>(dimensionIndex)];
            glyph.dimensionHeight = dimensionTable_[1][static_cast<size_t>(dimensionIndex)];
        }

        if (dimensionIndex == 0 && jpcsp) {
            // Fonts created by ttf2pgf do not contain complete Glyph
            // information. Provide default values.
            glyph.dimensionWidth = glyph.w << 6;
            glyph.dimensionHeight = glyph.h << 6;
        }
    } else {
        glyph.dimensionWidth = static_cast<int>(consumeBits(32, charPtr));
        glyph.dimensionHeight = static_cast<int>(consumeBits(32, charPtr));
    }

    if ((glyph.flags & FONT_PGF_METRIC_BEARING_X_INDEX) == FONT_PGF_METRIC_BEARING_X_INDEX) {
        const int xAdjustIndex = static_cast<int>(consumeBits(8, charPtr));

        if (xAdjustIndex < header_.xAdjustTableLength) {
            glyph.xAdjustH = xAdjustTable_[0][static_cast<size_t>(xAdjustIndex)];
            glyph.xAdjustV = xAdjustTable_[1][static_cast<size_t>(xAdjustIndex)];
        }

        if (xAdjustIndex == 0 && jpcsp) {
            glyph.xAdjustH = glyph.left * 64;
            glyph.xAdjustV = glyph.left * 64;
        }
    } else {
        glyph.xAdjustH = static_cast<int>(consumeBits(32, charPtr));
        glyph.xAdjustV = static_cast<int>(consumeBits(32, charPtr));
    }

    if ((glyph.flags & FONT_PGF_METRIC_BEARING_Y_INDEX) == FONT_PGF_METRIC_BEARING_Y_INDEX) {
        const int yAdjustIndex = static_cast<int>(consumeBits(8, charPtr));

        if (yAdjustIndex < header_.yAdjustTableLength) {
            glyph.yAdjustH = yAdjustTable_[0][static_cast<size_t>(yAdjustIndex)];
            glyph.yAdjustV = yAdjustTable_[1][static_cast<size_t>(yAdjustIndex)];
        }

        if (yAdjustIndex == 0 && jpcsp) {
            glyph.yAdjustH = glyph.top * 64;
            glyph.yAdjustV = glyph.top * 64;
        }
    } else {
        glyph.yAdjustH = static_cast<int>(consumeBits(32, charPtr));
        glyph.yAdjustV = static_cast<int>(consumeBits(32, charPtr));
    }

    if ((glyph.flags & FONT_PGF_METRIC_ADVANCE_INDEX) == FONT_PGF_METRIC_ADVANCE_INDEX) {
        const int advanceIndex = static_cast<int>(consumeBits(8, charPtr));

        if (advanceIndex < header_.advanceTableLength) {
            glyph.advanceH = advanceTable_[0][static_cast<size_t>(advanceIndex)];
            glyph.advanceV = advanceTable_[1][static_cast<size_t>(advanceIndex)];
        }
    } else {
        glyph.advanceH = static_cast<int>(consumeBits(32, charPtr));
        glyph.advanceV = static_cast<int>(consumeBits(32, charPtr));
    }

    glyph.ptr = static_cast<uint32_t>(charPtr / 8);
    return true;
}

bool Pgf::GetCharGlyph(int charCode, int glyphType, Glyph &glyph) const {
    if (charCode < firstGlyph_) {
        return false;
    }
    charCode -= firstGlyph_;
    if (charCode < static_cast<int>(charmap_.size())) {
        charCode = charmap_[static_cast<size_t>(charCode)];
    }
    if (glyphType == FONT_PGF_CHARGLYPH) {
        if (charCode >= static_cast<int>(glyphs_.size())) {
            return false;
        }
        glyph = glyphs_[static_cast<size_t>(charCode)];
    } else {
        if (charCode >= static_cast<int>(shadowGlyphs_.size())) {
            return false;
        }
        glyph = shadowGlyphs_[static_cast<size_t>(charCode)];
    }
    return true;
}

bool Pgf::HasGlyph(int charCode, int glyphType) const {
    Glyph glyph;
    return GetCharGlyph(charCode, glyphType, glyph);
}

bool Pgf::DecodeGlyphPixels(const Glyph &glyph, std::vector<uint8_t> *decodedPixels) const {
    if (glyph.w <= 0 || glyph.h <= 0) {
        return false;
    }
    if (((glyph.flags & FONT_PGF_BMP_OVERLAY) != FONT_PGF_BMP_H_ROWS) &&
        ((glyph.flags & FONT_PGF_BMP_OVERLAY) != FONT_PGF_BMP_V_ROWS)) {
        // Nonsense glyph bitmap direction flag
        return false;
    }
    size_t bitPtr = static_cast<size_t>(glyph.ptr) * 8;
    const int numberPixels = glyph.w * glyph.h;
    int pixelIndex = 0;
    decodedPixels->assign(static_cast<size_t>(numberPixels), 0);

    while (pixelIndex < numberPixels && bitPtr + 8 < fontData_.size() * 8) {
        // This is some kind of nibble based RLE compression.
        const int nibble = static_cast<int>(consumeBits(4, bitPtr));

        int count;
        int value = 0;
        if (nibble < 8) {
            value = static_cast<int>(consumeBits(4, bitPtr));
            count = nibble + 1;
        } else {
            count = 16 - nibble;
        }

        for (int i = 0; i < count && pixelIndex < numberPixels; i++) {
            if (nibble >= 8) {
                value = static_cast<int>(consumeBits(4, bitPtr));
            }
            (*decodedPixels)[static_cast<size_t>(pixelIndex++)] = static_cast<uint8_t>(value | (value << 4));
        }
    }
    return true;
}

bool Pgf::RasterizeGlyph(int charCode, GlyphBitmap *out, int glyphType) const {
    *out = GlyphBitmap{};
    Glyph glyph;
    if (!GetCharGlyph(charCode, glyphType, glyph)) {
        return false;
    }
    std::vector<uint8_t> decoded;
    if (!DecodeGlyphPixels(glyph, &decoded)) {
        return false;
    }
    out->width = glyph.w;
    out->height = glyph.h;
    out->left = glyph.left;
    out->top = glyph.top;
    out->pixels.resize(decoded.size());
    const bool hRows = (glyph.flags & FONT_PGF_BMP_OVERLAY) == FONT_PGF_BMP_H_ROWS;
    for (int y = 0; y < glyph.h; ++y) {
        for (int x = 0; x < glyph.w; ++x) {
            const int index = hRows ? y * glyph.w + x : x * glyph.h + y;
            out->pixels[static_cast<size_t>(y * glyph.w + x)] = decoded[static_cast<size_t>(index)];
        }
    }
    return true;
}

void Pgf::DrawCharacter(const GlyphImage &image, std::span<uint8_t> buffer, int clipX, int clipY,
                        int clipWidth, int clipHeight, int charCode, int altCharCode,
                        int glyphType) const {
    Glyph glyph;
    if (!GetCharGlyph(charCode, glyphType, glyph)) {
        if (charCode < firstGlyph_) {
            // Don't draw anything if the character is before the first
            // available glyph.
            return;
        }
        // No Glyph available for this charCode, try to use the alternate char.
        charCode = altCharCode;
        if (!GetCharGlyph(charCode, glyphType, glyph)) {
            return;
        }
    }

    // Glyph with negative size or nonsense bitmap direction: not rendered.
    std::vector<uint8_t> decodedPixels;
    if (!DecodeGlyphPixels(glyph, &decodedPixels)) {
        return;
    }

    const int x = image.xPos64 >> 6;
    const int y = image.yPos64 >> 6;
    const int xFrac = image.xPos64 & 0x3F;
    const int yFrac = image.yPos64 & 0x3F;

    // Negative means don't clip on that side.
    if (clipX < 0) {
        clipX = 0;
    }
    if (clipY < 0) {
        clipY = 0;
    }
    if (clipWidth < 0) {
        clipWidth = 8192;
    }
    if (clipHeight < 0) {
        clipHeight = 8192;
    }

    auto samplePixel = [&](int xx, int yy) -> uint32_t {
        if (xx < 0 || yy < 0 || xx >= glyph.w || yy >= glyph.h) {
            return 0;
        }
        int index;
        if ((glyph.flags & FONT_PGF_BMP_OVERLAY) == FONT_PGF_BMP_H_ROWS) {
            index = yy * glyph.w + xx;
        } else {
            index = xx * glyph.h + yy;
        }
        return decodedPixels[static_cast<size_t>(index)];
    };

    const int renderX1 = std::max(clipX, x) - x;
    const int renderY1 = std::max(clipY, y) - y;
    // We can render up to frac beyond the glyph w/h, so add 1px if necessary.
    const int renderX2 = std::min(clipX + clipWidth - x, glyph.w + (xFrac > 0 ? 1 : 0));
    const int renderY2 = std::min(clipY + clipHeight - y, glyph.h + (yFrac > 0 ? 1 : 0));

    if (xFrac == 0 && yFrac == 0) {
        for (int yy = renderY1; yy < renderY2; ++yy) {
            for (int xx = renderX1; xx < renderX2; ++xx) {
                const uint8_t pixelColor = static_cast<uint8_t>(samplePixel(xx, yy));
                SetFontPixel(buffer, image.bytesPerLine, image.bufWidth, image.bufHeight, x + xx,
                             y + yy, pixelColor, image.pixelFormat);
            }
        }
    } else {
        const uint32_t xf = static_cast<uint32_t>(xFrac);
        const uint32_t yf = static_cast<uint32_t>(yFrac);
        for (int yy = renderY1; yy < renderY2; ++yy) {
            for (int xx = renderX1; xx < renderX2; ++xx) {
                // First, blend horizontally.  Tests show we blend swizzled to 8 bit.
                const uint32_t horiz1 = samplePixel(xx - 1, yy - 1) * xf + samplePixel(xx, yy - 1) * (64 - xf);
                const uint32_t horiz2 = samplePixel(xx - 1, yy + 0) * xf + samplePixel(xx, yy + 0) * (64 - xf);
                // Now blend those together vertically.
                const uint32_t blended = horiz1 * yf + horiz2 * (64 - yf);

                // We multiplied an 8 bit value by 64 twice, so now we have a 20 bit value.
                const uint8_t pixelColor = static_cast<uint8_t>(blended >> 12);
                SetFontPixel(buffer, image.bytesPerLine, image.bufWidth, image.bufHeight, x + xx,
                             y + yy, pixelColor, image.pixelFormat);
            }
        }
    }
}

void Pgf::SetFontPixel(std::span<uint8_t> buffer, int bpl, int bufWidth, int bufHeight, int x, int y,
                       uint8_t pixelColor, uint32_t pixelformat) const {
    if (x < 0 || x >= bufWidth || y < 0 || y >= bufHeight) {
        return;
    }

    static const uint8_t fontPixelSizeInBytes[] = {0, 0, 1, 3, 4}; // 0 means 2 pixels per byte
    if (pixelformat > PSP_FONT_PIXELFORMAT_32) {
        return;
    }
    const int pixelBytes = fontPixelSizeInBytes[pixelformat];
    const int bufMaxWidth = (pixelBytes == 0 ? bpl * 2 : bpl / pixelBytes);
    if (x >= bufMaxWidth) {
        return;
    }

    const size_t framebufferAddr = static_cast<size_t>(y) * static_cast<size_t>(bpl) +
                                   static_cast<size_t>(pixelBytes == 0 ? x / 2 : x * pixelBytes);
    const size_t bytes = pixelBytes == 0 ? 1 : static_cast<size_t>(pixelBytes);
    if (framebufferAddr + bytes > buffer.size()) {
        return;
    }
    uint8_t *p = buffer.data() + framebufferAddr;

    switch (pixelformat) {
    case PSP_FONT_PIXELFORMAT_4:
    case PSP_FONT_PIXELFORMAT_4_REV: {
        // We always get a 8-bit value, so take only the top 4 bits.
        const int pix4 = pixelColor >> 4;
        const int oldColor = *p;
        int newColor;
        if (static_cast<uint32_t>(x & 1) != pixelformat) {
            newColor = (pix4 << 4) | (oldColor & 0xF);
        } else {
            newColor = (oldColor & 0xF0) | pix4;
        }
        *p = static_cast<uint8_t>(newColor);
        break;
    }
    case PSP_FONT_PIXELFORMAT_8:
        *p = pixelColor;
        break;
    case PSP_FONT_PIXELFORMAT_24:
        // Each channel has the same value.
        p[0] = pixelColor;
        p[1] = pixelColor;
        p[2] = pixelColor;
        break;
    case PSP_FONT_PIXELFORMAT_32: {
        // Spread the 8 bits out into one write of 32 bits.
        uint32_t pix32 = pixelColor;
        pix32 |= pix32 << 8;
        pix32 |= pix32 << 16;
        std::memcpy(p, &pix32, 4);
        break;
    }
    default:
        break;
    }
}

} // namespace mpo::font
