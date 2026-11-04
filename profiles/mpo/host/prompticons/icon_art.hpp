// SPDX-License-Identifier: GPL-3.0-or-later
//
// F25a: the port's own button icons, drawn by code (so they are this file's
// license, GPL-3.0-or-later, like the rest of the port): nothing is copied or
// traced from the game's textures, and there are no brand logos, only the
// usual generic shapes (CLAUDE.md §1, docs/analysis/button-prompts.md §7.2).
//
// - A gamepad's face button (one letter: A, B, X, Y): a disc with a ring and
//   the letter.
// - L, R, Start and Select on a gamepad (LB, ZR, Options, +...): a pill with
//   the text; Menu and View (Xbox, Steam) are their usual pictograms (three
//   lines, two overlapping squares).
// - A keyboard key: a key cap with its short name (SPC, CTL, ESC, Q...).
// - A mouse button: a mouse with that button filled.
// - The four shapes (a symbol prompt, the PSP and PlayStation sets) are not
//   drawn: the game's own icon stays.
//
// The letters are two pixel fonts of this file (5 x 7 and 3 x 5). The colours
// come from the icon being replaced (its brightest colour for the lines and
// the text, its strongest alpha), so the icon keeps the layout's look and its
// transparency (many HUD palettes are 0..0x80 alpha).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "icon_inventory.hpp"
#include "prompts.h"

namespace mpo::prompticons {

enum class IconShape : std::uint8_t { kNone, kRound, kPill, kKey, kMouse, kMenu, kView };

struct IconPlan {
    IconShape shape = IconShape::kNone;
    std::string text;     // upper case, in the fonts' characters
    int mouse_button = 0; // kMouse: 0 left, 1 right, 2 middle
    friend bool operator==(const IconPlan &, const IconPlan &) = default;
};

// What to draw for `button` showing `label` in a set of `style`.
[[nodiscard]] IconPlan PlanIcon(frontend::PromptStyle style, frontend::PromptButton button,
                                const frontend::PromptLabel &label);

// The short text of a key or button name: "Space" -> "SPC", "Ctrl" -> "CTL",
// "Num 5" -> "N5", "LB" -> "LB"; at most 3 characters, unknown ones as '?'.
[[nodiscard]] std::string ShortName(const std::string &name);

struct IconColors {
    std::uint32_t line = 0xFFF0F0F0u; // RGBA8888 as 0xAABBGGRR
    std::uint32_t fill = 0xFF202020u;
};

// The colours for an icon over `original` (RGBA8 bytes, w x h): the
// brightest colour of the frame for the lines (at the frame's strongest
// alpha), and a dark fill at that alpha. The defaults when the frame is
// fully transparent.
[[nodiscard]] IconColors ColorsFrom(const std::vector<std::uint8_t> &original, int w, int h,
                                    const IconFrame &frame);

// The frame clipped to a w x h texture (a zero size is the whole texture).
[[nodiscard]] IconFrame ClipFrame(const IconFrame &frame, int w, int h);

// Draws `plan` into the frame of a w x h texture: RGBA8 bytes, transparent
// outside what it draws. False (and nothing written) for kNone or a frame too
// small to draw in.
bool DrawIcon(const IconPlan &plan, int w, int h, const IconFrame &frame, const IconColors &colors,
              std::vector<std::uint8_t> *rgba);

// Bumped whenever the drawings change, so a recording knows its icons
// (prompt_icons.hpp Digest).
inline constexpr const char *kIconArtVersion = "f25a-1";

} // namespace mpo::prompticons
