// SPDX-License-Identifier: GPL-3.0-or-later
//
// A synthetic PGF font for the tests (F17a, shared with mpo_hle_test in
// F17b): bit-packed tables with odd widths (a 9-bit char map, 13-bit
// pointers), glyph 'A' (H rows, indexed metrics, a shadow), 'B' (V rows,
// explicit metrics, negative left/top) and '?' (1x1), revision 2 or 3.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pgf.h"

namespace mpo::font::test {

// LSB-first bit writer: bit i is bit (i % 8) of byte i / 8, which is how
// PgfGetBits reads little-endian 32-bit words.
class Bits {
public:
    void put(uint32_t v, int n) {
        for (int i = 0; i < n; ++i) {
            const size_t byte = pos_ / 8;
            if (byte >= bytes_.size()) {
                bytes_.resize(byte + 1, 0);
            }
            if ((v >> i) & 1) {
                bytes_[byte] = static_cast<uint8_t>(bytes_[byte] | (1u << (pos_ % 8)));
            }
            ++pos_;
        }
    }
    void alignTo(size_t bytes) {
        while (pos_ % (bytes * 8) != 0) {
            put(0, 1);
        }
    }
    size_t pos() const { return pos_; }
    void seek(size_t bit) {
        pos_ = bit;
        if (bytes_.size() < (bit + 7) / 8) {
            bytes_.resize((bit + 7) / 8, 0);
        }
    }
    std::vector<uint8_t> &bytes() { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    size_t pos_ = 0;
};

// Expected glyph pixels (4-bit values, row-major).
inline const uint8_t kGlyphA[3][4] = {{0, 15, 15, 0}, {15, 0, 0, 15}, {8, 8, 8, 8}};
inline const uint8_t kGlyphB[3][2] = {{1, 2}, {3, 4}, {5, 6}}; // stored column-major (V rows)

// 4-bit RLE: n < 8 repeats the next value n + 1 times; n >= 8 is followed
// by 16 - n literal values.
inline void putRepeat(Bits &b, int count, int value) {
    b.put(static_cast<uint32_t>(count - 1), 4);
    b.put(static_cast<uint32_t>(value), 4);
}
inline void putLiterals(Bits &b, const std::vector<int> &values) {
    b.put(static_cast<uint32_t>(16 - values.size()), 4);
    for (int v : values) {
        b.put(static_cast<uint32_t>(v), 4);
    }
}

inline void putGlyphHeader(Bits &b, int sizeField, int w, int h, int left, int top, int flags,
                    int shadowFlags, int shadowId) {
    b.put(static_cast<uint32_t>(sizeField), 14);
    b.put(static_cast<uint32_t>(w), 7);
    b.put(static_cast<uint32_t>(h), 7);
    b.put(static_cast<uint32_t>(left & 0x7F), 7);
    b.put(static_cast<uint32_t>(top & 0x7F), 7);
    b.put(static_cast<uint32_t>(flags), 6);
    b.put(static_cast<uint32_t>((shadowFlags >> 5) & 3), 2);
    b.put(static_cast<uint32_t>((shadowFlags >> 3) & 3), 2);
    b.put(static_cast<uint32_t>(shadowFlags & 7), 3);
    b.put(static_cast<uint32_t>(shadowId), 9);
}

struct Font {
    std::vector<uint8_t> bytes;
};

// Glyph 0 = 'A' (H rows, table metrics, has a shadow), glyph 1 = 'B' (V rows,
// explicit metrics), glyph 2 = '?' (1x1).
inline Font BuildFont(int revision) {
    PgfHeader h{};
    h.headerOffset = 0;
    h.headerSize = sizeof(PgfHeader);
    std::memcpy(h.PGFMagic, "PGF0", 4);
    h.revision = revision;
    h.version = 6;
    h.charMapLength = 0x30; // 0x20..0x4F
    h.charPointerLength = 3;
    h.charMapBpe = 9;
    h.charPointerBpe = 13;
    h.bpp = 4;
    h.hSize = 10 * 64;
    h.vSize = 10 * 64;
    h.hResolution = 128 * 64;
    h.vResolution = 128 * 64;
    std::snprintf(h.fontName, sizeof(h.fontName), "%s", "Synthetic Sans");
    std::snprintf(h.fontType, sizeof(h.fontType), "%s", "Regular");
    h.firstGlyph = 0x20;
    h.lastGlyph = 0x4F;
    h.maxAscender = 11 * 64;
    h.maxDescender = -3 * 64;
    h.maxLeftXAdjust = 64;
    h.maxBaseYAdjust = 2 * 64;
    h.minCenterXAdjust = -64;
    h.maxTopYAdjust = 12 * 64;
    h.maxAdvance[0] = 9 * 64;
    h.maxAdvance[1] = 14 * 64;
    h.maxSize[0] = 8 * 64;
    h.maxSize[1] = 12 * 64;
    h.maxGlyphWidth = 8;
    h.maxGlyphHeight = 12;
    h.dimTableLength = 2;
    h.xAdjustTableLength = 2;
    h.yAdjustTableLength = 2;
    h.advanceTableLength = 2;
    h.shadowMapLength = 1;
    h.shadowMapBpe = 8;

    std::vector<uint8_t> out(sizeof(h));
    std::memcpy(out.data(), &h, sizeof(h));
    auto putU32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    };
    if (revision == 3) {
        PgfHeaderRev3Extra x{};
        x.compCharMapBpe1 = 16;
        x.compCharMapLength1 = 0x10001; // only the low 16 bits count
        x.compCharMapBpe2 = 16;
        x.compCharMapLength2 = 2;
        const auto *p = reinterpret_cast<const uint8_t *>(&x);
        out.insert(out.end(), p, p + sizeof(x));
    }
    // dimension, xAdjust, yAdjust, advance tables: (H, V) pairs.
    const uint32_t dims[2][2] = {{0, 0}, {4 * 64, 3 * 64}};
    const uint32_t xadj[2][2] = {{0, 0}, {64, 32}};
    const uint32_t yadj[2][2] = {{0, 0}, {10 * 64, 5 * 64}};
    const uint32_t adv[2][2] = {{0, 0}, {6 * 64, 12 * 64}};
    for (auto *table : {dims, xadj, yadj, adv}) {
        for (int i = 0; i < 2; ++i) {
            putU32(table[i][0]);
            putU32(table[i][1]);
        }
    }
    // Shadow map: 1 entry of 8 bits (glyph 0), padded to 32 bits.
    Bits shadowMap;
    shadowMap.put(0, 8);
    shadowMap.alignTo(4);
    out.insert(out.end(), shadowMap.bytes().begin(), shadowMap.bytes().end());
    if (revision == 3) {
        // compCharMapLength1 (1) + compCharMapLength2 (2) pairs of u16.
        for (int i = 0; i < 3 * 2; ++i) {
            out.push_back(static_cast<uint8_t>(i));
            out.push_back(0);
        }
    }
    // Char map: 0x30 entries of 9 bits; unmapped entries are 0x1FF.
    Bits charMap;
    for (int c = 0x20; c < 0x50; ++c) {
        uint32_t g = 0x1FF;
        if (c == 'A') {
            g = 0;
        } else if (c == 'B') {
            g = 1;
        } else if (c == '?') {
            g = 2;
        }
        charMap.put(g, 9);
    }
    charMap.alignTo(4);
    out.insert(out.end(), charMap.bytes().begin(), charMap.bytes().end());

    // Font data: glyph records at 4-byte aligned offsets.
    Bits data;
    std::vector<uint32_t> pointers;
    // Glyph 'A': H rows + all four metric indices = flags 0x3D.
    pointers.push_back(static_cast<uint32_t>(data.pos() / 32));
    const size_t aStart = data.pos();
    putGlyphHeader(data, 0 /* patched below */, 4, 3, 1, 10, 0x3D, 0x2A, 0);
    data.put(1, 8); // dimension index
    data.put(1, 8); // xAdjust index
    data.put(1, 8); // yAdjust index
    data.put(1, 8); // advance index
    // Pixels: 0 F F 0 | F 0 0 F | 8 8 8 8
    putLiterals(data, {0});
    putRepeat(data, 2, 15);
    putLiterals(data, {0, 15, 0, 0, 15});
    putRepeat(data, 4, 8);
    data.alignTo(1);
    // Shadow record for 'A', right after it. Its bitmap starts at the byte
    // holding bit 40 (ptr = bit 42 / 8), so the first nibble overlaps the
    // two top bits of `top` (zero here) and must have its low two bits clear.
    const size_t shadowOffset = (data.pos() - aStart) / 8;
    const size_t sStart = data.pos();
    data.put(0, 14);
    data.put(2, 7);                                // w
    data.put(2, 7);                                // h
    data.put(static_cast<uint32_t>(-1 & 0x7F), 7); // left
    data.put(9, 7);                                // top
    data.seek(sStart + 40);
    data.put(0, 2);        // (top bits 5..6, already zero)
    data.put(0x4 >> 2, 2); // nibble 4 (with the two bits above): repeat 5 times
    data.put(0xC, 4);      // value 0xC
    data.alignTo(4);
    {
        // Patch the size field of 'A' (14 bits at its start).
        Bits patch;
        patch.put(static_cast<uint32_t>(shadowOffset), 14);
        for (size_t i = 0; i < 14; ++i) {
            const size_t bit = aStart + i;
            const uint8_t mask = static_cast<uint8_t>(1u << (bit % 8));
            if ((patch.bytes()[i / 8] >> (i % 8)) & 1) {
                data.bytes()[bit / 8] = static_cast<uint8_t>(data.bytes()[bit / 8] | mask);
            }
        }
    }

    // Glyph 'B': V rows, explicit metrics (flags 0x02), negative left/top.
    pointers.push_back(static_cast<uint32_t>(data.pos() / 32));
    putGlyphHeader(data, 0, 2, 3, -2, -3, 0x02, 0, 5);
    data.put(2 * 64, 32);                      // dimensionWidth
    data.put(3 * 64, 32);                      // dimensionHeight
    data.put(static_cast<uint32_t>(-128), 32); // xAdjustH
    data.put(7, 32);                           // xAdjustV
    data.put(static_cast<uint32_t>(-192), 32); // yAdjustH
    data.put(8, 32);                           // yAdjustV
    data.put(5 * 64, 32);                      // advanceH
    data.put(9 * 64, 32);                      // advanceV
    // Column-major: column 0 = 1 3 5, column 1 = 2 4 6.
    putLiterals(data, {1, 3, 5, 2, 4, 6});
    data.alignTo(4);

    // Glyph '?': 1x1, H rows, explicit metrics.
    pointers.push_back(static_cast<uint32_t>(data.pos() / 32));
    putGlyphHeader(data, 0, 1, 1, 0, 1, 0x01, 0, 0);
    for (int i = 0; i < 8; ++i) {
        data.put(static_cast<uint32_t>(64 * (i + 1)), 32);
    }
    putRepeat(data, 1, 9);
    data.alignTo(4);
    data.put(0, 32); // slack so the last glyph is not at the very end

    Bits ptrTable;
    for (uint32_t p : pointers) {
        ptrTable.put(p, 13);
    }
    ptrTable.alignTo(4);
    out.insert(out.end(), ptrTable.bytes().begin(), ptrTable.bytes().end());
    out.insert(out.end(), data.bytes().begin(), data.bytes().end());
    return {out};
}

} // namespace mpo::font::test
