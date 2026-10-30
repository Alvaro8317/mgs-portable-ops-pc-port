// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Util/PPGeDraw.cpp @ v1.20.4 (see ppge.hpp).

#include "hle/ppge.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "hle/system.hpp"

namespace mpo::hle {

namespace {

// Kernel memory PPGe takes (__PPGeInit): the list, the vertices and the text
// images (PPSSPP allocates each text image on its own).
constexpr std::uint32_t kListSize = 0x10000u;
constexpr std::uint32_t kDataSize = 0x10000u;
constexpr std::uint32_t kTextSize = 0x80000u;
constexpr std::uint32_t kListArgsSize = 16u;      // PspGeListArgs
constexpr std::uint32_t kContextSize = 512u * 4u; // The saved GE context.
constexpr std::uint32_t kPaletteSize = 16u * 2u;
constexpr std::uint32_t kVertexSize = 20u; // u16 u, v; u32 color; float x, y, z

constexpr std::uint8_t kCmdOffsetAddr = 0x13;
constexpr std::uint8_t kCmdVaddr = 0x01;
constexpr std::uint8_t kCmdPrim = 0x04;
constexpr std::uint8_t kCmdFinish = 0x0F;
constexpr std::uint8_t kCmdEnd = 0x0C;
constexpr std::uint8_t kCmdBase = 0x10;
constexpr std::uint8_t kCmdVertexType = 0x12;
constexpr std::uint8_t kCmdLightingEnable = 0x17;
constexpr std::uint8_t kCmdCullFaceEnable = 0x1D;
constexpr std::uint8_t kCmdTextureMapEnable = 0x1E;
constexpr std::uint8_t kCmdFogEnable = 0x1F;
constexpr std::uint8_t kCmdDitherEnable = 0x20;
constexpr std::uint8_t kCmdAlphaBlendEnable = 0x21;
constexpr std::uint8_t kCmdAlphaTestEnable = 0x22;
constexpr std::uint8_t kCmdZTestEnable = 0x23;
constexpr std::uint8_t kCmdStencilTestEnable = 0x24;
constexpr std::uint8_t kCmdColorTestEnable = 0x27;
constexpr std::uint8_t kCmdTexAddr0 = 0xA0;
constexpr std::uint8_t kCmdTexBufWidth0 = 0xA8;
constexpr std::uint8_t kCmdClutAddr = 0xB0;
constexpr std::uint8_t kCmdClutAddrUpper = 0xB1;
constexpr std::uint8_t kCmdTexSize0 = 0xB8;
constexpr std::uint8_t kCmdTexMapMode = 0xC0;
constexpr std::uint8_t kCmdTexMode = 0xC2;
constexpr std::uint8_t kCmdTexFormat = 0xC3;
constexpr std::uint8_t kCmdLoadClut = 0xC4;
constexpr std::uint8_t kCmdClutFormat = 0xC5;
constexpr std::uint8_t kCmdTexFilter = 0xC6;
constexpr std::uint8_t kCmdTexWrap = 0xC7;
constexpr std::uint8_t kCmdTexFunc = 0xC9;
constexpr std::uint8_t kCmdTexFlush = 0xCB;
constexpr std::uint8_t kCmdClearMode = 0xD3;
constexpr std::uint8_t kCmdScissor1 = 0xD4;
constexpr std::uint8_t kCmdScissor2 = 0xD5;
constexpr std::uint8_t kCmdMinZ = 0xD6;
constexpr std::uint8_t kCmdMaxZ = 0xD7;
constexpr std::uint8_t kCmdBlendMode = 0xDF;
constexpr std::uint8_t kCmdMaskRgb = 0xE8;
constexpr std::uint8_t kCmdMaskAlpha = 0xE9;

constexpr int kPrimRectangles = 6;
// GE_VTYPE_TC_16BIT | GE_VTYPE_COL_8888 | GE_VTYPE_POS_FLOAT | GE_VTYPE_THROUGH.
constexpr std::uint32_t kVertexType = 2u | (7u << 2u) | (3u << 7u) | (1u << 23u);
constexpr std::uint32_t kTexFormatClut4 = 4u;

int pow2_log(int value) {
    int shift = 0;
    while ((1 << shift) < value) ++shift;
    return shift;
}

// UTF-8 to code points; invalid bytes become U+FFFD. '\r' is dropped, as in
// PPGeGetTextImage.
std::u32string decode_utf8(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t code = 0xFFFD;
        std::size_t length = 1;
        if (c < 0x80u) {
            code = c;
        } else if ((c & 0xE0u) == 0xC0u) {
            length = 2;
            code = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {
            length = 3;
            code = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {
            length = 4;
            code = c & 0x07u;
        } else {
            ++i;
            out.push_back(0xFFFD);
            continue;
        }
        if (i + length > text.size()) {
            out.push_back(0xFFFD);
            break;
        }
        bool valid = true;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0u) != 0x80u) valid = false;
            code = (code << 6u) | (next & 0x3Fu);
        }
        i += valid ? length : 1;
        if (!valid) code = 0xFFFD;
        if (code != U'\r') out.push_back(code);
    }
    return out;
}

} // namespace

Ppge::Ppge(System &system) : system_(system) {}

std::uint32_t Ppge::alpha_mul(std::uint32_t color, float factor) {
    const auto alpha = static_cast<std::int32_t>(static_cast<float>(color >> 24u) * factor);
    return (static_cast<std::uint32_t>(std::clamp(alpha, 0, 255)) << 24u) | (color & 0xFFFFFFu);
}

std::uint32_t Ppge::faded(std::uint32_t color, std::uint32_t fade) {
    const std::uint32_t alpha = (color >> 24u) * fade / 255u;
    return (color & 0x00FFFFFFu) | (alpha << 24u);
}

bool Ppge::init() {
    if (initialized_) return available_;
    initialized_ = true;
    // __PPGeInit / __PPGeSetupListArgs.
    dl_ = system_.alloc_kernel_scratch(kListSize, "PPGe Display List");
    data_ = system_.alloc_kernel_scratch(kDataSize, "PPGe Vertex Data");
    text_ = system_.alloc_kernel_scratch(kTextSize, "PPGe Text");
    list_args_ = system_.alloc_kernel_scratch(kListArgsSize, "PPGe List Args");
    context_ = system_.alloc_kernel_scratch(kContextSize, "PPGe Saved Context");
    palette_ = system_.alloc_kernel_scratch(kPaletteSize, "PPGe Texture Palette");
    available_ = dl_ != 0u && data_ != 0u && text_ != 0u && list_args_ != 0u && context_ != 0u &&
                 palette_ != 0u;
    if (!available_) return false;
    kernel::GuestMemory &m = system_.kernel().memory();
    m.fill(list_args_, 0u, kListArgsSize);
    m.write_u32(list_args_, 8u);
    m.write_u32(list_args_ + 4u, context_);
    // A 16-level alpha ramp of white, in 4444.
    for (std::uint32_t i = 0; i < 16u; ++i) {
        const auto entry = static_cast<std::uint16_t>((i << 12u) | 0xFFFu);
        const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(entry & 0xFFu),
                                       static_cast<std::uint8_t>(entry >> 8u)};
        (void)m.write_bytes(palette_ + 2u * i, bytes);
    }
    return true;
}

bool Ppge::has_fonts() {
    if (!fonts_checked_) {
        fonts_checked_ = true;
        font_ = system_.fonts().firmware_font("ltn0.pgf");
        fallback_ = system_.fonts().firmware_font("jpn0.pgf");
        if (font_ == nullptr) std::swap(font_, fallback_);
    }
    return font_ != nullptr;
}

void Ppge::write_cmd(std::uint8_t cmd, std::uint32_t data) {
    if (dl_write_ + 4u > dl_ + kListSize - 8u) return; // Keep room for FINISH and END.
    system_.kernel().memory().write_u32(dl_write_, (static_cast<std::uint32_t>(cmd) << 24u) |
                                                       (data & 0xFFFFFFu));
    dl_write_ += 4u;
}

void Ppge::write_cmd_addr(std::uint8_t cmd, std::uint32_t address) {
    write_cmd(kCmdBase, (address >> 8u) & 0xFF0000u);
    write_cmd(cmd, address & 0xFFFFFFu);
}

bool Ppge::begin() {
    drawing_ = false;
    if (!init()) return false;
    dl_write_ = dl_;
    data_write_ = data_;
    text_write_ = text_;
    drawing_ = true;

    write_cmd(kCmdOffsetAddr, 0u);
    write_cmd(kCmdAlphaBlendEnable, 1u);
    write_cmd(kCmdBlendMode, 2u | (3u << 4u));
    write_cmd(kCmdAlphaTestEnable, 0u);
    write_cmd(kCmdColorTestEnable, 0u);
    write_cmd(kCmdZTestEnable, 0u);
    write_cmd(kCmdLightingEnable, 0u);
    write_cmd(kCmdFogEnable, 0u);
    write_cmd(kCmdStencilTestEnable, 0u);
    write_cmd(kCmdCullFaceEnable, 0u);
    write_cmd(kCmdClearMode, 0u);
    write_cmd(kCmdMaskRgb, 0u);
    write_cmd(kCmdMaskAlpha, 0u);
    write_cmd(kCmdDitherEnable, 0u);
    // PPGeSetDefaultTexture, without the atlas: the palette and the text
    // texture state.
    set_text_texture_state();
    scissor_reset();
    write_cmd(kCmdMinZ, 0u);
    write_cmd(kCmdMaxZ, 0xFFFFu);
    write_cmd(kCmdVertexType, kVertexType);
    return true;
}

void Ppge::end() {
    if (!drawing_) return;
    drawing_ = false;
    system_.kernel().memory().write_u32(dl_write_, static_cast<std::uint32_t>(kCmdFinish) << 24u);
    system_.kernel().memory().write_u32(dl_write_ + 4u, static_cast<std::uint32_t>(kCmdEnd) << 24u);
    dl_write_ += 8u;
    if (data_write_ > data_ || text_write_ > text_) {
        const std::uint32_t id = system_.ge().enqueue(dl_, dl_write_, -1, list_args_, false, false);
        system_.hle().trace_event("ppge: list %08X-%08X enqueued (%u)", dl_, dl_write_, id);
        ++stats_.lists;
    }
}

void Ppge::set_text_texture_state() {
    write_cmd(kCmdTextureMapEnable, 1u);
    write_cmd(kCmdClutAddr, palette_ & 0xFFFFF0u);
    write_cmd(kCmdClutAddrUpper, (palette_ & 0xFF000000u) >> 8u);
    write_cmd(kCmdClutFormat, 0x00FF02u);
    write_cmd(kCmdLoadClut, 2u);
    write_cmd(kCmdTexMapMode, 0u | (1u << 8u));
    write_cmd(kCmdTexMode, 0u);
    write_cmd(kCmdTexFormat, kTexFormatClut4);
    write_cmd(kCmdTexFilter, (1u << 8u) | 1u);             // Linear.
    write_cmd(kCmdTexWrap, (1u << 8u) | 1u);               // Clamp.
    write_cmd(kCmdTexFunc, (0u << 16u) | (1u << 8u) | 0u); // RGBA, modulate.
    write_cmd(kCmdTexFlush, 0u);
}

void Ppge::scissor(int x1, int y1, int x2, int y2) {
    x1 = std::clamp(x1, 0, 480);
    x2 = std::clamp(x2, 1, 480);
    y1 = std::clamp(y1, 0, 272);
    y2 = std::clamp(y2, 1, 272);
    write_cmd(kCmdScissor1, static_cast<std::uint32_t>((y1 << 10) | x1));
    write_cmd(kCmdScissor2, static_cast<std::uint32_t>(((y2 - 1) << 10) | (x2 - 1)));
}

void Ppge::scissor_reset() {
    scissor(0, 0, 480, 272);
}

void Ppge::begin_vertices() {
    vertex_count_ = 0u;
    vertex_start_ = data_write_;
}

void Ppge::vertex(float x, float y, float u, float v, int tw, int th, std::uint32_t color) {
    if (data_write_ + kVertexSize > data_ + kDataSize) return;
    kernel::GuestMemory &m = system_.kernel().memory();
    const auto tu =
        static_cast<std::uint32_t>(static_cast<std::uint16_t>(u * static_cast<float>(tw)));
    const auto tv =
        static_cast<std::uint32_t>(static_cast<std::uint16_t>(v * static_cast<float>(th)));
    m.write_u32(data_write_, tu | (tv << 16u));
    m.write_u32(data_write_ + 4u, color);
    m.write_u32(data_write_ + 8u, std::bit_cast<std::uint32_t>(x));
    m.write_u32(data_write_ + 12u, std::bit_cast<std::uint32_t>(y));
    m.write_u32(data_write_ + 16u, 0u);
    data_write_ += kVertexSize;
    ++vertex_count_;
}

void Ppge::end_vertices_and_draw(int prim) {
    if (vertex_count_ != 0u) {
        write_cmd_addr(kCmdVaddr, vertex_start_);
        write_cmd(kCmdPrim, (static_cast<std::uint32_t>(prim) << 16u) | vertex_count_);
    }
    vertex_start_ = 0u;
}

void Ppge::draw_rect(float x1, float y1, float x2, float y2, std::uint32_t color) {
    if (!drawing_) return;
    write_cmd(kCmdTextureMapEnable, 0u);
    begin_vertices();
    vertex(x1, y1, 0.0f, 0.0f, 0, 0, color);
    vertex(x2, y2, 0.0f, 0.0f, 0, 0, color);
    end_vertices_and_draw(kPrimRectangles);
    write_cmd(kCmdTextureMapEnable, 1u);
}

// ---- Text ------------------------------------------------------------------

float Ppge::line_height() {
    if (!has_fonts()) return 1.0f;
    const font::PgfHeader &h = font_->header();
    return std::max(1.0f, static_cast<float>(h.maxAscender - h.maxDescender) / 64.0f);
}

float Ppge::ascent() {
    if (!has_fonts()) return 0.0f;
    return static_cast<float>(font_->header().maxAscender) / 64.0f;
}

float Ppge::pixel_scale(float scale) {
    return scale * kLinePixels / line_height();
}

const Ppge::Glyph *Ppge::glyph(char32_t code) {
    const auto found = glyphs_.find(code);
    if (found != glyphs_.end()) return found->second.valid ? &found->second : nullptr;
    Glyph g;
    for (const font::Pgf *pgf : {font_, fallback_}) {
        if (pgf == nullptr || !pgf->HasGlyph(static_cast<int>(code))) continue;
        font::PgfCharInfo info{};
        if (!pgf->GetCharInfo(static_cast<int>(code), &info, -1)) continue;
        g.advance = static_cast<float>(info.sfp26AdvanceH) / 64.0f;
        // A space has no bitmap.
        (void)pgf->RasterizeGlyph(static_cast<int>(code), &g.bitmap);
        g.valid = true;
        break;
    }
    auto [it, inserted] = glyphs_.emplace(code, std::move(g));
    (void)inserted;
    return it->second.valid ? &it->second : nullptr;
}

Ppge::Layout Ppge::layout(std::u32string_view text, float max_width, int wrap) {
    Layout out;
    // Split into lines: explicit '\n' and, with kWrapWord, at spaces.
    std::vector<std::u32string> lines;
    std::u32string current;
    float current_width = 0.0f;
    const auto width_of = [this](std::u32string_view s) {
        float w = 0.0f;
        for (const char32_t c : s) {
            const Glyph *g = glyph(c);
            if (g == nullptr) g = glyph(U'?');
            if (g != nullptr) w += g->advance;
        }
        return w;
    };
    const auto flush = [&] {
        lines.push_back(current);
        current.clear();
        current_width = 0.0f;
    };
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == U'\n') {
            flush();
            ++i;
            continue;
        }
        // The next word with the spaces before it.
        std::size_t end = i;
        while (end < text.size() && text[end] == U' ') ++end;
        while (end < text.size() && text[end] != U' ' && text[end] != U'\n') ++end;
        const std::u32string_view word = text.substr(i, end - i);
        const float w = width_of(word);
        if ((wrap & kWrapWord) != 0 && max_width > 0.0f && !current.empty() &&
            current_width + w > max_width) {
            flush();
            std::size_t skip = 0;
            while (skip < word.size() && word[skip] == U' ') ++skip;
            current.append(word.substr(skip));
            current_width = width_of(current);
        } else {
            current.append(word);
            current_width += w;
        }
        i = end;
    }
    flush();

    const Glyph *dot = glyph(U'.');
    const float ellipsis = dot != nullptr ? 3.0f * dot->advance : 0.0f;
    for (std::u32string &line : lines) {
        // Trailing spaces do not count.
        while (!line.empty() && line.back() == U' ') line.pop_back();
        if ((wrap & kEllipsis) != 0 && max_width > 0.0f && width_of(line) > max_width) {
            while (!line.empty() && width_of(line) + ellipsis > max_width) line.pop_back();
            line.append(U"...");
        }
        const int index = out.lines++;
        float x = 0.0f;
        for (const char32_t c : line) {
            const Glyph *g = glyph(c);
            if (g == nullptr) g = glyph(U'?');
            if (g == nullptr) continue;
            out.glyphs.push_back({g, x, index});
            x += g->advance;
        }
        out.line_widths.push_back(x);
        out.width = std::max(out.width, x);
    }
    return out;
}

void Ppge::measure_text(float *width, float *height, std::string_view text, float scale, int wrap,
                        float wrap_width) {
    if (width != nullptr) *width = 0.0f;
    if (height != nullptr) *height = 0.0f;
    if (!has_fonts()) return;
    const float k = pixel_scale(scale);
    const Layout l =
        layout(decode_utf8(text), (wrap_width <= 0.0f ? 480.0f : wrap_width) / k, wrap);
    if (width != nullptr) *width = l.width * k;
    if (height != nullptr) *height = static_cast<float>(l.lines) * line_height() * k;
}

Ppge::TextImage Ppge::text_image(std::string_view text, float max_width_px, int wrap,
                                 std::uint32_t align) {
    TextImage image;
    const Layout l = layout(decode_utf8(text), max_width_px, wrap);
    if (l.lines == 0 || l.width <= 0.0f) return image;
    const float lh = line_height();
    image.width = static_cast<int>(std::ceil(l.width)) + 1;
    image.height = static_cast<int>(std::ceil(static_cast<float>(l.lines) * lh));
    if (image.width > 1024 || image.height > 512) return image;
    image.bm_width = image.width;
    image.bm_height = image.height;

    // 8-bit coverage, then 4-bit palette indices (PPGeGetTextImage).
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(image.width * image.height), 0u);
    const float base = ascent();
    for (const PlacedGlyph &p : l.glyphs) {
        float line_x = 0.0f;
        const float line_width = l.line_widths[static_cast<std::size_t>(p.line)];
        if ((align & kHCenter) != 0u)
            line_x = std::floor((l.width - line_width) / 2.0f);
        else if ((align & kRight) != 0u)
            line_x = l.width - line_width;
        const font::GlyphBitmap &b = p.glyph->bitmap;
        const int ox = static_cast<int>(std::lround(line_x + p.x)) + b.left;
        const int oy =
            static_cast<int>(std::lround(static_cast<float>(p.line) * lh + base)) - b.top;
        for (int y = 0; y < b.height; ++y) {
            const int ty = oy + y;
            if (ty < 0 || ty >= image.height) continue;
            for (int x = 0; x < b.width; ++x) {
                const int tx = ox + x;
                if (tx < 0 || tx >= image.width) continue;
                std::uint8_t &dst = coverage[static_cast<std::size_t>(ty * image.width + tx)];
                dst = std::max(dst, b.pixels[static_cast<std::size_t>(y * b.width + x)]);
            }
        }
    }

    const int bufw_bytes = ((image.bm_width + 31) / 32) * 16;
    const auto size = static_cast<std::uint32_t>(bufw_bytes * (image.bm_height + 1));
    if (text_write_ + size > text_ + kTextSize) {
        ++stats_.text_overflow;
        return {};
    }
    std::vector<std::uint8_t> packed(size, 0u);
    for (int y = 0; y < image.bm_height; ++y) {
        for (int x = 0; x < image.bm_width; ++x) {
            const std::uint8_t c = coverage[static_cast<std::size_t>(y * image.width + x)];
            std::uint8_t &dst = packed[static_cast<std::size_t>(y * bufw_bytes + x / 2)];
            dst = static_cast<std::uint8_t>((x & 1) != 0 ? (dst | (c & 0xF0u)) : (dst | (c >> 4u)));
        }
    }
    image.ptr = text_write_;
    (void)system_.kernel().memory().write_bytes(image.ptr, packed);
    // Texture addresses are 16-byte aligned.
    text_write_ += (size + 15u) & ~15u;
    return image;
}

void Ppge::draw_text_image(const TextImage &im, float x, float y, const Style &style) {
    if (im.ptr == 0u) return;
    ++stats_.texts;
    const float k = pixel_scale(style.scale);
    // Wider than 512 texels: a second tile (PPGeDrawTextImage).
    const int bufw = ((im.bm_width + 31) / 32) * 32;
    const int tile1_bm_width = std::min(512, im.bm_width);
    const int tile2_bm_width = tile1_bm_width == 512 ? im.bm_width - 512 : 0;
    const bool has_tile2 = tile2_bm_width != 0;
    const int tile1_wp2 = pow2_log(tile1_bm_width);
    const int tile2_wp2 = has_tile2 ? pow2_log(tile2_bm_width) : 0;
    const int hp2 = pow2_log(im.bm_height);

    write_cmd(kCmdTexAddr0, im.ptr & 0xFFFFF0u);
    write_cmd(kCmdTexBufWidth0, static_cast<std::uint32_t>(bufw) | ((im.ptr & 0xFF000000u) >> 8u));
    write_cmd(kCmdTexSize0, static_cast<std::uint32_t>(tile1_wp2 | (hp2 << 8)));
    write_cmd(kCmdTexFlush, 0u);

    const float w = static_cast<float>(im.width) * k;
    const float h = static_cast<float>(im.height) * k;
    if ((style.align & kHCenter) != 0u)
        x -= w / 2.0f;
    else if ((style.align & kRight) != 0u)
        x -= w;
    if ((style.align & kVCenter) != 0u)
        y -= h / 2.0f;
    else if ((style.align & kBottom) != 0u)
        y -= h;

    const float tile2x = 512.0f * k;
    const float tile1w = has_tile2 ? 512.0f * k : w;
    const float tile2w = has_tile2 ? w - tile1w : 0.0f;
    const float tile1u1 =
        has_tile2 ? 1.0f : static_cast<float>(im.width) / static_cast<float>(1 << tile1_wp2);
    const float tile2u1 =
        has_tile2 ? static_cast<float>(im.width - 512) / static_cast<float>(1 << tile2_wp2) : 0.0f;
    const float v1 = static_cast<float>(im.height) / static_cast<float>(1 << hp2);
    const int tw1 = 1 << tile1_wp2;
    const int tw2 = 1 << tile2_wp2;
    const int th = 1 << hp2;

    const auto select_tile = [&](bool second) {
        write_cmd(kCmdTexAddr0, ((im.ptr & 0xFFFFF0u) + (second ? 256u : 0u)) & 0xFFFFFFu);
        write_cmd(kCmdTexSize0,
                  static_cast<std::uint32_t>((second ? tile2_wp2 : tile1_wp2) | (hp2 << 8)));
    };

    begin_vertices();
    if (style.shadow) {
        // More shadows for a blurrier shadow.
        const std::uint32_t shadow = alpha_mul(style.shadow_color, 0.35f);
        for (float dy = 0.0f; dy <= 2.0f; dy += 1.0f) {
            for (float dx = 0.0f; dx <= 1.0f; dx += 0.5f) {
                if (dx == 0.0f && dy == 0.0f) continue;
                vertex(x + dx, y + dy, 0.0f, 0.0f, tw1, th, shadow);
                vertex(x + dx + tile1w, y + dy + h, tile1u1, v1, tw1, th, shadow);
            }
        }
        if (has_tile2) {
            end_vertices_and_draw(kPrimRectangles);
            begin_vertices();
            select_tile(true);
            for (float dy = 0.0f; dy <= 2.0f; dy += 1.0f) {
                for (float dx = 0.0f; dx <= 1.0f; dx += 0.5f) {
                    if (dx == 0.0f && dy == 0.0f) continue;
                    vertex(x + tile2x + dx, y + dy, 0.0f, 0.0f, tw2, th, shadow);
                    vertex(x + tile2x + dx + tile2w, y + dy + h, tile2u1, v1, tw2, th, shadow);
                }
            }
            end_vertices_and_draw(kPrimRectangles);
            begin_vertices();
            select_tile(false);
        }
    }
    vertex(x, y, 0.0f, 0.0f, tw1, th, style.color);
    vertex(x + tile1w, y + h, tile1u1, v1, tw1, th, style.color);
    if (has_tile2) {
        end_vertices_and_draw(kPrimRectangles);
        begin_vertices();
        select_tile(true);
        vertex(x + tile2x, y, 0.0f, 0.0f, tw2, th, style.color);
        vertex(x + tile2x + tile2w, y + h, tile2u1, v1, tw2, th, style.color);
    }
    end_vertices_and_draw(kPrimRectangles);
}

void Ppge::draw_text(std::string_view text, float x, float y, const Style &style) {
    if (!drawing_ || text.empty() || !has_fonts()) return;
    const float k = pixel_scale(style.scale);
    const TextImage im = text_image(text, (480.0f - x) / k, kEllipsis, style.align);
    draw_text_image(im, x, y, style);
}

void Ppge::draw_text_wrapped(std::string_view text, float x, float y, float wrap_width,
                             float wrap_height, const Style &style) {
    if (!drawing_ || text.empty() || !has_fonts()) return;
    std::string s(text);
    if (wrap_height != 0.0f) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\t'))
            s.pop_back();
    }
    const float width = wrap_width <= 0.0f ? 480.0f - x : wrap_width;
    Style adjusted = style;
    float actual_height = 0.0f;
    measure_text(nullptr, &actual_height, s, style.scale, kWrapWord, width);
    // Scale down to fit the height. PPSSPP crops to the lines that fit at
    // 1.3x smaller and adds "..."; MPO's texts are short, so this only scales.
    if (wrap_height != 0.0f && actual_height > wrap_height)
        adjusted.scale *= wrap_height / actual_height;
    const float k = pixel_scale(adjusted.scale);
    const TextImage im = text_image(s, width / k, kWrapWord | kEllipsis, adjusted.align);
    draw_text_image(im, x, y, adjusted);
}

} // namespace mpo::hle
