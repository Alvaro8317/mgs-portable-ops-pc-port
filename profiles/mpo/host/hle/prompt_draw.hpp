// SPDX-License-Identifier: GPL-3.0-or-later
//
// The button prompts of the sceUtility dialogs (F24b), drawn with PPGe from a
// frontend::PromptSet (frontend/prompts.h).
//
// Every prompt sits where the PSP's dialogs draw theirs (PSPDialog,
// PSPOskDialog, PSPNetconfDialog in PPSSPP v1.20.4), with its caption next to
// it:
//   - a symbol (the four shapes) is drawn exactly like before F24b, centred
//     at the PSP's spot, and the caption stays where it was;
//   - a word (the PSP's "Start", "Select") too, at its top-left spot;
//   - a key (key names, A/B/X/Y, Menu, LB...) is drawn as a key cap: the
//     text in a dark box with a light border, starting at the left edge of
//     the PSP's symbol and centred on the caption's line. The caption moves
//     right when the cap is wider than the symbol was.
// So the PSP set draws the same lists as before, byte for byte, and both the
// firmware PGF fonts and the fallback font (F23c: Latin-1 plus ○ △ □) have
// every glyph: the gamepad and key names are plain ASCII and the shapes are
// the four symbols the fonts already had.

#pragma once

#include <string_view>

#include "hle/ppge.hpp"
#include "prompts.h"

namespace mpo::hle {

// Where one prompt and its caption go, as the PSP set draws them.
struct PromptPlace {
    float left = 0.0f;     // A key cap's left edge.
    float symbol_x = 0.0f; // A symbol's centre (Ppge::kCenter).
    float symbol_y = 0.0f;
    float word_x = 0.0f; // A word's top-left (Ppge::kLeft).
    float word_y = 0.0f;
    float caption_x = 0.0f; // The caption's top-left (Ppge::kLeft).
    float caption_y = 0.0f;
};

// The styles: a symbol's (kCenter), a word's (kLeft) and the caption's
// (kLeft). A key cap's text uses the caption's scale and colors.
struct PromptStyles {
    Ppge::Style symbol;
    Ppge::Style word;
    Ppge::Style caption;
};

// How far right the prompt and its caption reach (screen pixels), without
// drawing.
float prompt_right(Ppge &ppge, const frontend::PromptLabel &label, const PromptPlace &place,
                   const PromptStyles &styles, std::string_view caption);
// Draws only the prompt; returns the x of its caption.
float draw_prompt_label(Ppge &ppge, const frontend::PromptLabel &label, const PromptPlace &place,
                        const PromptStyles &styles);
// Draws a caption at (x, place.caption_y); returns how far right it reaches.
float draw_caption(Ppge &ppge, std::string_view caption, float x, const PromptPlace &place,
                   const PromptStyles &styles);
// Draws the prompt and its caption; returns how far right they reach.
float draw_prompt(Ppge &ppge, const frontend::PromptLabel &label, const PromptPlace &place,
                  const PromptStyles &styles, std::string_view caption);
// `place` moved `dx` pixels right.
[[nodiscard]] PromptPlace shifted(const PromptPlace &place, float dx);

} // namespace mpo::hle
