// SPDX-License-Identifier: GPL-3.0-or-later

#include "image.hpp"

#include <algorithm>
#include <array>
#include <fstream>

#include "color.hpp"

namespace mpo::ge::replay {
namespace {

std::array<std::uint32_t, 256> MakeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

std::uint32_t Crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> kTable = MakeCrcTable();
    crc = ~crc;
    for (std::uint8_t b : data) crc = kTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void PutU32BE(std::vector<std::uint8_t> *out, std::uint32_t v) {
    out->push_back(static_cast<std::uint8_t>(v >> 24));
    out->push_back(static_cast<std::uint8_t>(v >> 16));
    out->push_back(static_cast<std::uint8_t>(v >> 8));
    out->push_back(static_cast<std::uint8_t>(v));
}

void PutChunk(std::vector<std::uint8_t> *out, const char type[4],
              std::span<const std::uint8_t> data) {
    PutU32BE(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out->size();
    out->insert(out->end(), type, type + 4);
    out->insert(out->end(), data.begin(), data.end());
    PutU32BE(out, Crc32(std::span<const std::uint8_t>(out->data() + start, out->size() - start)));
}

} // namespace

Image ReadFramebuffer(const MemoryView &mem, std::uint32_t addr, int stride, std::uint32_t format,
                      int width, int height, bool opaque) {
    Image img;
    img.width = width;
    img.height = height;
    img.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const std::uint32_t base = addr & (MemoryView::kVramSize - 1);
    const std::uint32_t bpp = format == 3 ? 4 : 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint32_t off = base + static_cast<std::uint32_t>(y * stride + x) * bpp;
            std::uint32_t c = 0;
            switch (format & 3) {
            case 0:
                c = RGB565ToRGBA8888(mem.VramRead16(off));
                break;
            case 1:
                c = RGBA5551ToRGBA8888(mem.VramRead16(off));
                break;
            case 2:
                c = RGBA4444ToRGBA8888(mem.VramRead16(off));
                break;
            default:
                c = mem.VramRead32(off);
                break;
            }
            if (opaque) c |= 0xFF000000u;
            std::uint8_t *p =
                &img.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)) *
                          4];
            p[0] = static_cast<std::uint8_t>(c);
            p[1] = static_cast<std::uint8_t>(c >> 8);
            p[2] = static_cast<std::uint8_t>(c >> 16);
            p[3] = static_cast<std::uint8_t>(c >> 24);
        }
    }
    return img;
}

std::vector<std::uint8_t> EncodePng(const Image &image) {
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    PutU32BE(&ihdr, static_cast<std::uint32_t>(image.width));
    PutU32BE(&ihdr, static_cast<std::uint32_t>(image.height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    PutChunk(&out, "IHDR", ihdr);

    // Raw scanlines with filter 0, wrapped in a zlib stream of stored blocks.
    std::vector<std::uint8_t> raw;
    const std::size_t row = static_cast<std::size_t>(image.width) * 4;
    raw.reserve((row + 1) * static_cast<std::size_t>(image.height));
    for (int y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const auto begin =
            image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * row);
        raw.insert(raw.end(), begin, begin + static_cast<std::ptrdiff_t>(row));
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    PutU32BE(&z, (b << 16) | a);
    PutChunk(&out, "IDAT", z);
    PutChunk(&out, "IEND", {});
    return out;
}

std::string WritePng(const std::filesystem::path &path, const Image &image) {
    const std::vector<std::uint8_t> png = EncodePng(image);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot create " + path.string();
    out.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) return "cannot write " + path.string();
    return {};
}

} // namespace mpo::ge::replay
