// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fonts and drawings below are original to this port (F25a).

#include "icon_art.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace mpo::prompticons {
namespace {

using frontend::PromptButton;
using frontend::PromptLabel;
using frontend::PromptStyle;

struct Glyph5 {
    char c;
    std::uint8_t rows[7]; // bit 4 is the left column
};
struct Glyph3 {
    char c;
    std::uint8_t rows[5]; // bit 2 is the left column
};

// 5 x 7.
constexpr Glyph5 kFont5[] = {
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {'?', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
};

// 3 x 5.
constexpr Glyph3 kFont3[] = {
    {'A', {0b010, 0b101, 0b111, 0b101, 0b101}}, {'B', {0b110, 0b101, 0b110, 0b101, 0b110}},
    {'C', {0b011, 0b100, 0b100, 0b100, 0b011}}, {'D', {0b110, 0b101, 0b101, 0b101, 0b110}},
    {'E', {0b111, 0b100, 0b110, 0b100, 0b111}}, {'F', {0b111, 0b100, 0b110, 0b100, 0b100}},
    {'G', {0b011, 0b100, 0b101, 0b101, 0b011}}, {'H', {0b101, 0b101, 0b111, 0b101, 0b101}},
    {'I', {0b111, 0b010, 0b010, 0b010, 0b111}}, {'J', {0b001, 0b001, 0b001, 0b101, 0b010}},
    {'K', {0b101, 0b101, 0b110, 0b101, 0b101}}, {'L', {0b100, 0b100, 0b100, 0b100, 0b111}},
    {'M', {0b101, 0b111, 0b111, 0b101, 0b101}}, {'N', {0b110, 0b101, 0b101, 0b101, 0b101}},
    {'O', {0b010, 0b101, 0b101, 0b101, 0b010}}, {'P', {0b110, 0b101, 0b110, 0b100, 0b100}},
    {'Q', {0b010, 0b101, 0b101, 0b110, 0b011}}, {'R', {0b110, 0b101, 0b110, 0b101, 0b101}},
    {'S', {0b011, 0b100, 0b010, 0b001, 0b110}}, {'T', {0b111, 0b010, 0b010, 0b010, 0b010}},
    {'U', {0b101, 0b101, 0b101, 0b101, 0b111}}, {'V', {0b101, 0b101, 0b101, 0b101, 0b010}},
    {'W', {0b101, 0b101, 0b111, 0b111, 0b101}}, {'X', {0b101, 0b101, 0b010, 0b101, 0b101}},
    {'Y', {0b101, 0b101, 0b010, 0b010, 0b010}}, {'Z', {0b111, 0b001, 0b010, 0b100, 0b111}},
    {'0', {0b111, 0b101, 0b101, 0b101, 0b111}}, {'1', {0b010, 0b110, 0b010, 0b010, 0b111}},
    {'2', {0b110, 0b001, 0b010, 0b100, 0b111}}, {'3', {0b110, 0b001, 0b010, 0b001, 0b110}},
    {'4', {0b101, 0b101, 0b111, 0b001, 0b001}}, {'5', {0b111, 0b100, 0b110, 0b001, 0b110}},
    {'6', {0b011, 0b100, 0b111, 0b101, 0b111}}, {'7', {0b111, 0b001, 0b010, 0b010, 0b010}},
    {'8', {0b111, 0b101, 0b111, 0b101, 0b111}}, {'9', {0b111, 0b101, 0b111, 0b001, 0b110}},
    {'+', {0b000, 0b010, 0b111, 0b010, 0b000}}, {'-', {0b000, 0b000, 0b111, 0b000, 0b000}},
    {'.', {0b000, 0b000, 0b000, 0b000, 0b010}}, {'?', {0b110, 0b001, 0b010, 0b000, 0b010}},
};

const Glyph5 *FindGlyph5(char c) {
    for (const Glyph5 &g : kFont5)
        if (g.c == c) return &g;
    return nullptr;
}
const Glyph3 *FindGlyph3(char c) {
    for (const Glyph3 &g : kFont3)
        if (g.c == c) return &g;
    return nullptr;
}

std::string Upper(std::string_view s) {
    std::string out;
    for (const char c : s) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// The pixels of a w x h texture, clipped to a frame.
class Canvas {
public:
    Canvas(std::vector<std::uint8_t> *rgba, int w, int h, const IconFrame &clip)
        : rgba_(rgba), w_(w), h_(h), clip_(clip) {
        rgba_->assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    }
    void Put(int x, int y, std::uint32_t c) {
        if (x < clip_.x || y < clip_.y || x >= clip_.x + clip_.w || y >= clip_.y + clip_.h) return;
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        std::uint8_t *p = &(*rgba_)[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
                                     static_cast<std::size_t>(x)) *
                                    4];
        for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(c >> (8 * i));
    }
    void Fill(int x0, int y0, int x1, int y1, std::uint32_t c) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) Put(x, y, c);
    }
    void Clear(int x, int y) { Put(x, y, 0); }

private:
    std::vector<std::uint8_t> *rgba_;
    int w_;
    int h_;
    IconFrame clip_;
};

// The width of `text` in a font (with 1-pixel gaps); 0 for an empty text.
int TextWidth(const std::string &text, int glyph_w) {
    const int n = static_cast<int>(text.size());
    return n == 0 ? 0 : n * glyph_w + (n - 1);
}

// Whether `text` fits in max_w x max_h in one of the fonts.
bool Fits(const std::string &text, int max_w, int max_h) {
    return (TextWidth(text, 5) <= max_w && 7 <= max_h) ||
           (TextWidth(text, 3) <= max_w && 5 <= max_h);
}

// Draws `text` centred on (cx, cy) in the largest font that fits in
// max_w x max_h. False when not even the small one fits.
bool DrawText(Canvas &canvas, const std::string &text, double cx, double cy, int max_w, int max_h,
              std::uint32_t color) {
    const bool big = TextWidth(text, 5) <= max_w && 7 <= max_h;
    const bool small = TextWidth(text, 3) <= max_w && 5 <= max_h;
    if (!big && !small) return false;
    const int gw = big ? 5 : 3;
    const int gh = big ? 7 : 5;
    int x = static_cast<int>(std::floor(cx - TextWidth(text, gw) / 2.0));
    const int y = static_cast<int>(std::floor(cy - gh / 2.0));
    for (const char c : text) {
        for (int row = 0; row < gh; ++row) {
            std::uint8_t bits = 0;
            if (big) {
                const Glyph5 *g = FindGlyph5(c);
                bits = (g != nullptr ? g : FindGlyph5('?'))->rows[row];
            } else {
                const Glyph3 *g = FindGlyph3(c);
                bits = (g != nullptr ? g : FindGlyph3('?'))->rows[row];
            }
            for (int col = 0; col < gw; ++col)
                if ((bits >> (gw - 1 - col)) & 1) canvas.Put(x + col, y + row, color);
        }
        x += gw + 1;
    }
    return true;
}

// A rectangle with a 1-pixel border, `bottom` pixels thick at the bottom,
// and its corners cut by `round` pixels.
void Box(Canvas &canvas, int x0, int y0, int x1, int y1, int bottom, int round,
         const IconColors &colors) {
    canvas.Fill(x0, y0, x1, y1, colors.line);
    canvas.Fill(x0 + 1, y0 + 1, x1 - 1, y1 - bottom, colors.fill);
    for (int i = 0; i < round; ++i) {
        for (int j = 0; j < round - i; ++j) {
            canvas.Clear(x0 + j, y0 + i);
            canvas.Clear(x1 - 1 - j, y0 + i);
            canvas.Clear(x0 + j, y1 - 1 - i);
            canvas.Clear(x1 - 1 - j, y1 - 1 - i);
        }
    }
    if (round >= 2) {
        // The cut corners get their border back, one pixel in.
        canvas.Put(x0 + 1, y0 + 1, colors.line);
        canvas.Put(x1 - 2, y0 + 1, colors.line);
        canvas.Put(x0 + 1, y1 - 2, colors.line);
        canvas.Put(x1 - 2, y1 - 2, colors.line);
    }
}

// A box whose text needs its sides: the side borders become fill (a wide
// name like SPC in a 12-pixel key cap keeps its top and bottom lines).
void OpenSides(Canvas &canvas, int x0, int y0, int x1, int y1, int bottom,
               const IconColors &colors) {
    canvas.Fill(x0, y0 + 2, x0 + 1, y1 - bottom - 1, colors.fill);
    canvas.Fill(x1 - 1, y0 + 2, x1, y1 - bottom - 1, colors.fill);
}

} // namespace

std::string ShortName(const std::string &name) {
    struct Abbrev {
        std::string_view from;
        std::string_view to;
    };
    static constexpr Abbrev kAbbrevs[] = {
        {"SPACE", "SPC"},     {"ENTER", "ENT"},       {"RETURN", "ENT"},  {"ESC", "ESC"},
        {"ESCAPE", "ESC"},    {"CTRL", "CTL"},        {"SHIFT", "SHF"},   {"ALT", "ALT"},
        {"TAB", "TAB"},       {"BACKSPACE", "BS"},    {"DELETE", "DEL"},  {"INSERT", "INS"},
        {"HOME", "HOM"},      {"END", "END"},         {"PAGE UP", "PGU"}, {"PAGE DOWN", "PGD"},
        {"CAPS LOCK", "CAP"}, {"UP", "UP"},           {"DOWN", "DN"},     {"LEFT", "LT"},
        {"RIGHT", "RT"},      {"OPTIONS", "OPT"},     {"SHARE", "SHR"},   {"CREATE", "CRT"},
        {"START", "STA"},     {"SELECT", "SEL"},      {"BACK", "BCK"},    {"MOUSE 4", "M4"},
        {"MOUSE 5", "M5"},    {"MIDDLE CLICK", "M3"},
    };
    const std::string upper = Upper(name);
    for (const Abbrev &a : kAbbrevs)
        if (upper == a.from) return std::string(a.to);
    // "Left Ctrl" and the like: the last word.
    std::string_view word = upper;
    if (const std::size_t space = word.rfind(' '); space != std::string_view::npos) {
        const std::string_view first = word.substr(0, space);
        const std::string_view last = word.substr(space + 1);
        if (first == "NUM" || first == "KEYPAD") return "N" + ShortName(std::string(last));
        for (const Abbrev &a : kAbbrevs)
            if (last == a.from) return std::string(a.to);
    }
    std::string out;
    for (const char c : upper) {
        if (c == ' ') continue;
        const bool known = FindGlyph5(c) != nullptr;
        out += known ? c : '?';
        if (out.size() == 3) break;
    }
    return out;
}

IconPlan PlanIcon(PromptStyle style, PromptButton button, const PromptLabel &label) {
    IconPlan plan;
    if (label.kind == PromptLabel::Kind::kSymbol || label.text.empty()) return plan;
    const std::string upper = Upper(label.text);
    if (upper == "LEFT CLICK" || upper == "RIGHT CLICK" || upper == "MIDDLE CLICK") {
        plan.shape = IconShape::kMouse;
        plan.mouse_button = upper[0] == 'L' ? 0 : upper[0] == 'R' ? 1 : 2;
        return plan;
    }
    if (style == PromptStyle::kKeyboard) {
        plan.shape = IconShape::kKey;
        plan.text = ShortName(label.text);
        return plan;
    }
    if (upper == "MENU") {
        plan.shape = IconShape::kMenu;
        return plan;
    }
    if (upper == "VIEW") {
        plan.shape = IconShape::kView;
        return plan;
    }
    const bool face = button == PromptButton::kCross || button == PromptButton::kCircle ||
                      button == PromptButton::kTriangle || button == PromptButton::kSquare;
    plan.text = ShortName(label.text);
    plan.shape = face && plan.text.size() == 1 ? IconShape::kRound : IconShape::kPill;
    return plan;
}

IconColors ColorsFrom(const std::vector<std::uint8_t> &original, int w, int h,
                      const IconFrame &frame) {
    IconColors colors;
    const IconFrame f = ClipFrame(frame, w, h);
    if (original.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4)
        return colors;
    const auto pixel = [&](int x, int y) {
        return &original[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                          static_cast<std::size_t>(x)) *
                         4];
    };
    int max_a = 0;
    for (int y = f.y; y < f.y + f.h; ++y)
        for (int x = f.x; x < f.x + f.w; ++x) max_a = std::max<int>(max_a, pixel(x, y)[3]);
    if (max_a == 0) return colors;
    int best = -1;
    std::uint32_t best_rgb = 0;
    for (int y = f.y; y < f.y + f.h; ++y) {
        for (int x = f.x; x < f.x + f.w; ++x) {
            const std::uint8_t *p = pixel(x, y);
            if (p[3] * 2 < max_a) continue;
            const int luma = p[0] * 299 + p[1] * 587 + p[2] * 114;
            if (luma > best) {
                best = luma;
                best_rgb = static_cast<std::uint32_t>(p[0]) |
                           (static_cast<std::uint32_t>(p[1]) << 8) |
                           (static_cast<std::uint32_t>(p[2]) << 16);
            }
        }
    }
    // A dark icon (its lines are its darkest part): white lines instead.
    if (best < 128 * 1000) best_rgb = 0xF0F0F0u;
    const auto alpha = static_cast<std::uint32_t>(max_a) << 24;
    colors.line = best_rgb | alpha;
    colors.fill = 0x202020u | alpha;
    return colors;
}

IconFrame ClipFrame(const IconFrame &frame, int w, int h) {
    if (frame.w == 0 || frame.h == 0) return {0, 0, w, h};
    IconFrame f = frame;
    f.x = std::clamp(f.x, 0, w);
    f.y = std::clamp(f.y, 0, h);
    f.w = std::clamp(f.w, 0, w - f.x);
    f.h = std::clamp(f.h, 0, h - f.y);
    return f;
}

bool DrawIcon(const IconPlan &plan, int w, int h, const IconFrame &frame, const IconColors &colors,
              std::vector<std::uint8_t> *rgba) {
    if (plan.shape == IconShape::kNone || w <= 0 || h <= 0) return false;
    const IconFrame f = ClipFrame(frame, w, h);
    const int s = std::min(f.w, f.h);
    if (s < 7) return false;
    std::vector<std::uint8_t> out;
    Canvas canvas(&out, w, h, f);
    // The square of side s centred in the frame.
    const int sx = f.x + (f.w - s) / 2;
    const int sy = f.y + (f.h - s) / 2;
    const double cx = sx + s / 2.0;
    const double cy = sy + s / 2.0;
    switch (plan.shape) {
    case IconShape::kNone:
        return false;
    case IconShape::kRound: {
        const double r = s / 2.0;
        for (int y = sy; y < sy + s; ++y) {
            for (int x = sx; x < sx + s; ++x) {
                const double dx = x + 0.5 - cx;
                const double dy = y + 0.5 - cy;
                const double d = std::sqrt(dx * dx + dy * dy);
                if (d <= r - 1.25)
                    canvas.Put(x, y, colors.fill);
                else if (d <= r)
                    canvas.Put(x, y, colors.line);
            }
        }
        if (!DrawText(canvas, plan.text, cx, cy, s - 4, s - 4, colors.line)) return false;
        break;
    }
    case IconShape::kKey: {
        Box(canvas, sx, sy, sx + s, sy + s, 2, 1, colors);
        const bool narrow = Fits(plan.text, s - 3, s - 4);
        if (!narrow) OpenSides(canvas, sx, sy, sx + s, sy + s, 2, colors);
        if (!DrawText(canvas, plan.text, cx, cy - 0.5, narrow ? s - 3 : s - 1, s - 4, colors.line))
            return false;
        break;
    }
    case IconShape::kPill: {
        Box(canvas, f.x, sy, f.x + f.w, sy + s, 1, 2, colors);
        const bool narrow = Fits(plan.text, f.w - 4, s - 4);
        if (!narrow) OpenSides(canvas, f.x, sy, f.x + f.w, sy + s, 1, colors);
        if (!DrawText(canvas, plan.text, f.x + f.w / 2.0, cy, narrow ? f.w - 4 : f.w - 1, s - 4,
                      colors.line))
            return false;
        break;
    }
    case IconShape::kMenu: {
        Box(canvas, f.x, sy, f.x + f.w, sy + s, 1, 2, colors);
        const int lw = std::min(f.w, s) - 6;
        const int lx = static_cast<int>(std::floor(f.x + f.w / 2.0 - lw / 2.0));
        const int ly = static_cast<int>(std::floor(cy)) - 2;
        for (int i = 0; i < 3; ++i)
            canvas.Fill(lx, ly + 2 * i - 1, lx + lw, ly + 2 * i, colors.line);
        break;
    }
    case IconShape::kView: {
        Box(canvas, f.x, sy, f.x + f.w, sy + s, 1, 2, colors);
        const int q = std::max(3, (s - 4) / 2 + 1);
        const int ox = static_cast<int>(std::floor(f.x + f.w / 2.0 - (q + 2) / 2.0));
        const int oy = static_cast<int>(std::floor(cy - (q + 2) / 2.0));
        for (int k = 0; k < 2; ++k) {
            const int x0 = ox + 2 * k;
            const int y0 = oy + 2 * k;
            for (int i = 0; i < q; ++i) {
                canvas.Put(x0 + i, y0, colors.line);
                canvas.Put(x0 + i, y0 + q - 1, colors.line);
                canvas.Put(x0, y0 + i, colors.line);
                canvas.Put(x0 + q - 1, y0 + i, colors.line);
            }
        }
        break;
    }
    case IconShape::kMouse: {
        const int mw = std::max(5, (s * 5 + 4) / 8) | 1; // odd, so the split is centred
        const int mx = static_cast<int>(std::floor(cx - mw / 2.0));
        Box(canvas, mx, sy, mx + mw, sy + s, 1, 2, colors);
        const int split = sy + (s * 9) / 20;
        const int mid = mx + mw / 2;
        canvas.Fill(mx + 1, split, mx + mw - 1, split + 1, colors.line);
        canvas.Fill(mid, sy + 1, mid + 1, split, colors.line);
        if (plan.mouse_button == 0) {
            canvas.Fill(mx + 1, sy + 1, mid, split, colors.line);
        } else if (plan.mouse_button == 1) {
            canvas.Fill(mid + 1, sy + 1, mx + mw - 1, split, colors.line);
        } else {
            canvas.Fill(mid - 1, sy + 2, mid + 2, split - 1, colors.line);
        }
        break;
    }
    }
    *rgba = std::move(out);
    return true;
}

} // namespace mpo::prompticons
