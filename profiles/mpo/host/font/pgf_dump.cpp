// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_pgf_dump: renders every glyph of a PGF font into a sheet (PNG) through
// Pgf::DrawCharacter, the path sceFontGetCharGlyphImage takes, plus a line of
// sample text laid out with the font's advances. Prints a summary line. For
// local analysis of fonts the user provides; nothing it writes belongs in the
// repo.
//
// Usage:
//   mpo_pgf_dump [--out SHEET.png] [--text "sample"] [--columns N] [--max-glyphs N] FONT.pgf
//
// Exit code: 0 if the font parsed and every mapped glyph decoded, 1 otherwise,
// 2 on usage.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "image.hpp"
#include "pgf.h"

namespace {

constexpr size_t kMaxFontSize = 32u << 20;

bool readFile(const std::string &path, std::vector<uint8_t> *out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxFontSize) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(out->data()), static_cast<std::streamsize>(out->size()));
    return static_cast<bool>(in);
}

// Decodes UTF-8 to code points (invalid bytes become U+FFFD).
std::vector<int> utf8Decode(const std::string &s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            n = 3;
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

} // namespace

int main(int argc, char **argv) {
    std::string fontPath;
    std::string outPath;
    std::string text = "The quick brown fox jumps over the lazy dog. 0123456789 MGS: Portable Ops";
    int columns = 32;
    int maxGlyphs = 4096;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--out" && hasValue) {
            outPath = argv[++i];
        } else if (arg == "--text" && hasValue) {
            text = argv[++i];
        } else if (arg == "--columns" && hasValue) {
            columns = std::clamp(std::atoi(argv[++i]), 1, 256);
        } else if (arg == "--max-glyphs" && hasValue) {
            maxGlyphs = std::clamp(std::atoi(argv[++i]), 1, 65536);
        } else if (!arg.empty() && arg[0] == '-') {
            fontPath.clear();
            break;
        } else {
            fontPath = arg;
        }
    }
    if (fontPath.empty()) {
        std::fprintf(stderr, "usage: mpo_pgf_dump [--out SHEET.png] [--text \"sample\"] "
                             "[--columns N] [--max-glyphs N] FONT.pgf\n");
        return 2;
    }

    std::vector<uint8_t> data;
    if (!readFile(fontPath, &data)) {
        std::printf("%s: cannot read (or larger than %zu bytes)\n", fontPath.c_str(), kMaxFontSize);
        return 1;
    }
    mpo::font::Pgf pgf;
    if (!pgf.ReadPtr(data.data(), data.size())) {
        std::printf("%s: not a valid PGF\n", fontPath.c_str());
        return 1;
    }
    const mpo::font::PgfHeader &h = pgf.header();
    mpo::font::PgfFontInfo info{};
    pgf.GetFontInfo(&info);

    // Mapped characters from firstGlyph to lastGlyph.
    std::vector<int> chars;
    int undecodable = 0;
    int maxW = 1;
    int maxH = 1;
    int maxTop = 0;
    int maxBelow = 0;
    for (int c = h.firstGlyph; c <= h.lastGlyph && static_cast<int>(chars.size()) < maxGlyphs;
         ++c) {
        if (!pgf.HasGlyph(c)) {
            continue;
        }
        mpo::font::GlyphBitmap bmp;
        if (!pgf.RasterizeGlyph(c, &bmp)) {
            // Zero-size glyphs (spaces) are fine; anything else is not.
            mpo::font::PgfCharInfo ci{};
            pgf.GetCharInfo(c, &ci, 0);
            if (ci.bitmapWidth != 0 && ci.bitmapHeight != 0) {
                ++undecodable;
            }
            continue;
        }
        chars.push_back(c);
        maxW = std::max(maxW, bmp.width + std::max(bmp.left, 0));
        maxH = std::max(maxH, bmp.height);
        maxTop = std::max(maxTop, bmp.top);
        maxBelow = std::max(maxBelow, bmp.height - bmp.top);
    }

    const int pad = 2;
    const int cellW = maxW + pad * 2;
    const int cellH = std::max(maxH, maxTop + maxBelow) + pad * 2;
    const int rows = (static_cast<int>(chars.size()) + columns - 1) / columns;

    // Sample line with the horizontal advances (26.6 fixed point).
    const std::vector<int> sample = utf8Decode(text);
    int sampleWidth64 = 0;
    for (int c : sample) {
        mpo::font::PgfCharInfo ci{};
        if (pgf.GetCharInfo(c, &ci, '?')) {
            sampleWidth64 += ci.sfp26AdvanceH;
        }
    }
    const int sampleH = cellH + pad * 2;
    const int width = std::max(columns * cellW, sampleWidth64 / 64 + pad * 4);
    const int height = sampleH + rows * cellH;

    // 8-bit target, drawn like sceFontGetCharGlyphImage_Clip does.
    std::vector<uint8_t> canvas(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    mpo::font::GlyphImage image{};
    image.pixelFormat = mpo::font::PSP_FONT_PIXELFORMAT_8;
    image.bufWidth = static_cast<uint16_t>(std::min(width, 65535));
    image.bufHeight = static_cast<uint16_t>(std::min(height, 65535));
    image.bytesPerLine = static_cast<uint16_t>(std::min(width, 65535));
    if (width > 65535 || height > 65535) {
        std::printf("%s: sheet too large (%dx%d)\n", fontPath.c_str(), width, height);
        return 1;
    }

    // Sample line: pen at x, baseline at maxTop; subpixel x positions use the
    // fractional path of DrawCharacter.
    int pen64 = pad * 2 * 64;
    for (int c : sample) {
        mpo::font::PgfCharInfo ci{};
        if (!pgf.GetCharInfo(c, &ci, '?')) {
            continue;
        }
        image.xPos64 = pen64 + static_cast<int32_t>(ci.bitmapLeft) * 64;
        image.yPos64 = (pad + maxTop - static_cast<int32_t>(ci.bitmapTop)) * 64;
        pgf.DrawCharacter(image, canvas, 0, 0, width, sampleH, c, '?',
                          mpo::font::FONT_PGF_CHARGLYPH);
        pen64 += ci.sfp26AdvanceH;
    }

    for (size_t i = 0; i < chars.size(); ++i) {
        const int col = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const int c = chars[i];
        mpo::font::PgfCharInfo ci{};
        pgf.GetCharInfo(c, &ci, 0);
        const int cellX = col * cellW;
        const int cellY = sampleH + row * cellH;
        image.xPos64 = (cellX + pad + std::max(static_cast<int32_t>(ci.bitmapLeft), 0)) * 64;
        image.yPos64 = (cellY + pad + maxTop - static_cast<int32_t>(ci.bitmapTop)) * 64;
        // Clip to the cell.
        pgf.DrawCharacter(image, canvas, cellX, cellY, cellW, cellH, c, 0,
                          mpo::font::FONT_PGF_CHARGLYPH);
    }

    int inked = 0;
    for (uint8_t v : canvas) {
        inked += v != 0;
    }
    if (!outPath.empty()) {
        // Dark glyphs on a light background, with a faint grid.
        mpo::ge::replay::Image png;
        png.width = width;
        png.height = height;
        png.rgba.resize(canvas.size() * 4);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t idx =
                    static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                uint8_t bg = 255;
                if (y >= sampleH && ((x % cellW) == 0 || ((y - sampleH) % cellH) == 0)) {
                    bg = 220;
                }
                const uint8_t v = static_cast<uint8_t>(bg - (bg * canvas[idx]) / 255);
                png.rgba[idx * 4 + 0] = v;
                png.rgba[idx * 4 + 1] = v;
                png.rgba[idx * 4 + 2] = v;
                png.rgba[idx * 4 + 3] = 255;
            }
        }
        const std::string err = mpo::ge::replay::WritePng(outPath, png);
        if (!err.empty()) {
            std::printf("%s: %s\n", outPath.c_str(), err.c_str());
            return 1;
        }
    }

    const bool ok = undecodable == 0 && !chars.empty();
    std::printf("%s: %s name=\"%s\" type=\"%s\" rev=%d ver=%d bpp=%d size=%dx%d res=%dx%d "
                "glyphs=%zu chars=0x%04X..0x%04X charmap=%d drawn=%zu undecodable=%d "
                "maxGlyph=%dx%d ascender=%.2f descender=%.2f sheet=%dx%d inked=%d\n",
                std::filesystem::path(fontPath).filename().string().c_str(), ok ? "OK" : "FAIL",
                h.fontName, h.fontType, h.revision, h.version, h.bpp, h.hSize / 64, h.vSize / 64,
                h.hResolution / 64, h.vResolution / 64, pgf.glyphCount(), h.firstGlyph, h.lastGlyph,
                h.charMapLength, chars.size(), undecodable, info.maxGlyphWidth, info.maxGlyphHeight,
                static_cast<double>(info.maxGlyphAscenderF),
                static_cast<double>(info.maxGlyphDescenderF), width, height, inked);
    return ok ? 0 : 1;
}
