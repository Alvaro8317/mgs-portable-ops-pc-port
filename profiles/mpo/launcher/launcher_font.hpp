// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's font (F25d): Dear ImGui's embedded scalable one, which
// covers Latin-1 (á é í ó ú ñ ü ¿ ¡ for the Spanish texts). One function for
// the launcher and for mpo_launcher_font_test, which checks that every
// character of launcher_strings.inc has a glyph in it.

#pragma once

#include "imgui.h"

namespace mpo::launcher {

inline ImFont *add_launcher_font(ImFontAtlas &atlas) {
    // Not AddFontDefault(): it picks the bitmap font below 15 px, and the
    // test would check one font while the launcher shows the other.
    return atlas.AddFontDefaultVector();
}

} // namespace mpo::launcher
