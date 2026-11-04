// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle/prompt_draw.hpp"

#include <algorithm>

namespace mpo::hle {

namespace {

using Kind = frontend::PromptLabel::Kind;

// The key cap: padding around the text, the border and the gap before the
// caption, in screen pixels.
constexpr float kCapPadX = 2.5f;
constexpr float kCapPadY = 0.5f;
constexpr float kCapBorder = 1.0f;
constexpr float kCapGap = 4.0f;
// ABGR at full opacity; the style's alpha (the dialog's fade) scales them.
constexpr std::uint32_t kCapBorderColor = 0xC0E0E0E0u;
constexpr std::uint32_t kCapFillColor = 0xD0302820u;

struct CapBox {
    float x1, y1, x2, y2;
    float text_width;
};

CapBox cap_box(Ppge &ppge, std::string_view text, const PromptPlace &place,
               const PromptStyles &styles) {
    float width = 0.0f;
    float height = 0.0f;
    ppge.measure_text(&width, &height, text, styles.caption.scale);
    const float line = Ppge::kLinePixels * styles.caption.scale;
    const float centre_y = place.caption_y + line / 2.0f;
    const float half = line / 2.0f + kCapPadY + kCapBorder;
    return {place.left, centre_y - half, place.left + width + 2.0f * (kCapPadX + kCapBorder),
            centre_y + half, width};
}

float caption_x_after_cap(const CapBox &box, const PromptPlace &place) {
    return std::max(place.caption_x, box.x2 + kCapGap);
}

float caption_width(Ppge &ppge, std::string_view caption, const PromptStyles &styles) {
    if (caption.empty()) return 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    ppge.measure_text(&width, &height, caption, styles.caption.scale);
    return width;
}

} // namespace

PromptPlace shifted(const PromptPlace &place, float dx) {
    PromptPlace out = place;
    out.left += dx;
    out.symbol_x += dx;
    out.word_x += dx;
    out.caption_x += dx;
    return out;
}

float prompt_right(Ppge &ppge, const frontend::PromptLabel &label, const PromptPlace &place,
                   const PromptStyles &styles, std::string_view caption) {
    float caption_x = place.caption_x;
    if (label.kind == Kind::kKey && !label.text.empty())
        caption_x = caption_x_after_cap(cap_box(ppge, label.text, place, styles), place);
    return caption_x + caption_width(ppge, caption, styles);
}

float draw_prompt_label(Ppge &ppge, const frontend::PromptLabel &label, const PromptPlace &place,
                        const PromptStyles &styles) {
    switch (label.kind) {
    case Kind::kSymbol:
        ppge.draw_text(label.text, place.symbol_x, place.symbol_y, styles.symbol);
        break;
    case Kind::kWord:
        ppge.draw_text(label.text, place.word_x, place.word_y, styles.word);
        break;
    case Kind::kKey: {
        if (label.text.empty()) break;
        const CapBox box = cap_box(ppge, label.text, place, styles);
        const float alpha = static_cast<float>(styles.caption.color >> 24u) / 255.0f;
        ppge.draw_rect(box.x1, box.y1, box.x2, box.y2, Ppge::alpha_mul(kCapBorderColor, alpha));
        ppge.draw_rect(box.x1 + kCapBorder, box.y1 + kCapBorder, box.x2 - kCapBorder,
                       box.y2 - kCapBorder, Ppge::alpha_mul(kCapFillColor, alpha));
        Ppge::Style text = styles.caption;
        text.align = Ppge::kCenter;
        text.shadow = false;
        ppge.draw_text(label.text, (box.x1 + box.x2) / 2.0f, (box.y1 + box.y2) / 2.0f, text);
        return caption_x_after_cap(box, place);
    }
    }
    return place.caption_x;
}

float draw_caption(Ppge &ppge, std::string_view caption, float x, const PromptPlace &place,
                   const PromptStyles &styles) {
    if (!caption.empty()) ppge.draw_text(caption, x, place.caption_y, styles.caption);
    return x + caption_width(ppge, caption, styles);
}

float draw_prompt(Ppge &ppge, const frontend::PromptLabel &label, const PromptPlace &place,
                  const PromptStyles &styles, std::string_view caption) {
    const float caption_x = draw_prompt_label(ppge, label, place, styles);
    return draw_caption(ppge, caption, caption_x, place, styles);
}

} // namespace mpo::hle
