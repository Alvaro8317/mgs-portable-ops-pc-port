// SPDX-License-Identifier: GPL-3.0-or-later
//
// PPGe (F18b): draws the sceUtility dialogs the way the firmware does, with a
// display list the HLE builds in kernel memory and enqueues on the GE over the
// game's frame. Ported from PPSSPP Core/Util/PPGeDraw.cpp @ v1.20.4
// (PPGeBegin/End, the vertex collector, PPGeDrawRect, PPGeScissor and the
// text-image path of PPGeDrawText/PPGeDrawTextWrapped/PPGeDrawTextImage).
//
// The list saves and restores the game's GE context (PspGeListArgs.context)
// and runs without GE interrupts, as in PPSSPP, so the game never sees it.
// softgpu and the Vulkan renderer draw it like any other list, so it shows in
// --capture and in frame comparisons.
//
// Text: PPSSPP renders it with the host's fonts (TextDrawer, an 18 px sans
// serif); here it is the firmware's own PGF fonts (mpo_font, F17a), ltn0 and
// jpn0 for what ltn0 lacks (the button symbols). Each string becomes a 4-bit
// texture with a 16-level alpha palette (PPSSPP's format), laid out at the
// font's size and scaled so that a style scale of 1 gives 18 px lines.
// Differences from PPSSPP, on purpose:
//   - lines of a multi-line text are aligned inside its box like the box
//     (PPSSPP left-aligns them inside a centred box);
//   - there is no image cache across frames: the text arena is rebuilt in
//     every begin(), so nothing is decimated.
// PPSSPP's atlas images (button icons, 4-patches) are not used: the dialogs
// draw the button symbols as text.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pgf.h"

namespace mpo::hle {

class System;

class Ppge {
public:
    // PPGeAlign.
    enum Align : std::uint32_t {
        kLeft = 0x00,
        kRight = 0x01,
        kHCenter = 0x02,
        kTop = 0x00,
        kBottom = 0x10,
        kVCenter = 0x20,
        kCenter = 0x22,
    };
    // PPGE_LINE_*.
    enum Wrap : int {
        kWrapNone = 0,
        kEllipsis = 1,
        kWrapWord = 2,
    };

    // PPGeStyle. Colors are ABGR (the GE's 8888).
    struct Style {
        std::uint32_t align = kLeft;
        float scale = 1.0f;
        std::uint32_t color = 0xFFFFFFFFu;
        bool shadow = false;
        std::uint32_t shadow_color = 0x80000000u;
    };

    struct Stats {
        std::uint64_t lists = 0;         // Lists enqueued.
        std::uint64_t texts = 0;         // Text images drawn.
        std::uint64_t text_overflow = 0; // Texts dropped: the arena was full.
    };

    // The 18 px of PPSSPP's TextDrawer font: a line at scale 1.
    static constexpr float kLinePixels = 18.0f;

    explicit Ppge(System &system);

    // PPGeBegin: resets the buffers and writes the UI state. False when PPGe
    // cannot draw (no kernel memory); then every other call does nothing.
    bool begin();
    // PPGeEnd: FINISH, END and, if anything was drawn, sceGeListEnQueue.
    void end();

    void scissor(int x1, int y1, int x2, int y2); // x2/y2 exclusive
    void scissor_reset();
    void draw_rect(float x1, float y1, float x2, float y2, std::uint32_t color);
    // One line, cut with an ellipsis at the right edge of the screen.
    void draw_text(std::string_view text, float x, float y, const Style &style);
    // Wrapped at `wrap_width` (0: to the right edge); scaled down to fit
    // `wrap_height` when it is not 0.
    void draw_text_wrapped(std::string_view text, float x, float y, float wrap_width,
                           float wrap_height, const Style &style);
    // PPGeMeasureText, in screen pixels.
    void measure_text(float *width, float *height, std::string_view text, float scale,
                      int wrap = kWrapNone, float wrap_width = 0.0f);

    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }
    // Whether the fonts are there (without them, text is not drawn).
    [[nodiscard]] bool has_fonts();

    // alphaMul and PSPDialog::CalcFadedColor.
    static std::uint32_t alpha_mul(std::uint32_t color, float factor);
    static std::uint32_t faded(std::uint32_t color, std::uint32_t fade);

private:
    struct Glyph {
        font::GlyphBitmap bitmap;
        float advance = 0.0f;
        bool valid = false;
    };
    struct PlacedGlyph {
        const Glyph *glyph = nullptr;
        float x = 0.0f;
        int line = 0;
    };
    struct Layout {
        std::vector<PlacedGlyph> glyphs;
        std::vector<float> line_widths;
        float width = 0.0f;
        int lines = 0;
    };
    struct TextImage {
        std::uint32_t ptr = 0;
        int width = 0; // In font pixels.
        int height = 0;
        int bm_width = 0;
        int bm_height = 0;
    };

    bool init();
    void write_cmd(std::uint8_t cmd, std::uint32_t data);
    void write_cmd_addr(std::uint8_t cmd, std::uint32_t address);
    void begin_vertices();
    void vertex(float x, float y, float u, float v, int tw, int th, std::uint32_t color);
    void end_vertices_and_draw(int prim);
    void set_text_texture_state();

    const Glyph *glyph(char32_t code);
    // In font pixels; max_width <= 0: no limit.
    Layout layout(std::u32string_view text, float max_width, int wrap);
    TextImage text_image(std::string_view text, float max_width_px, int wrap, std::uint32_t align);
    void draw_text_image(const TextImage &image, float x, float y, const Style &style);
    // Screen pixels per font pixel at a style scale.
    float pixel_scale(float scale);
    float line_height();
    float ascent();

    System &system_;
    bool initialized_ = false;
    bool available_ = false;
    bool fonts_checked_ = false;
    const font::Pgf *font_ = nullptr;
    const font::Pgf *fallback_ = nullptr;
    std::map<char32_t, Glyph> glyphs_;

    std::uint32_t list_args_ = 0;
    std::uint32_t context_ = 0;
    std::uint32_t palette_ = 0;
    std::uint32_t dl_ = 0;
    std::uint32_t dl_write_ = 0;
    std::uint32_t data_ = 0;
    std::uint32_t data_write_ = 0;
    std::uint32_t text_ = 0;
    std::uint32_t text_write_ = 0;
    std::uint32_t vertex_start_ = 0;
    std::uint32_t vertex_count_ = 0;
    bool drawing_ = false;
    Stats stats_;
};

} // namespace mpo::hle
