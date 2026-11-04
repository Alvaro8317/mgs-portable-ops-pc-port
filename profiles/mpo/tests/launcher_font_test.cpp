// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_launcher_font_test (F25d): every character of the launcher's texts, in
// English and in Spanish, has a glyph in the font the launcher loads (no "?"
// boxes in "Resolución" or "Añadir"), and so do the language names. Dear ImGui
// only, without a window or a renderer.

#include <cstdint>
#include <cstdio>
#include <set>
#include <string_view>

#include "imgui.h"
#include "launcher_font.hpp"
#include "launcher_text.hpp"

namespace {

using namespace mpo::launcher;

int g_failures = 0;

// The code points of a UTF-8 text; false when it is not valid UTF-8.
bool decode(std::string_view text, std::set<std::uint32_t> &out) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        int length = byte < 0x80u ? 1 : (byte >> 5u) == 0x6u ? 2 : (byte >> 4u) == 0xEu ? 3 : 0;
        if (length == 0 || i + static_cast<std::size_t>(length) > text.size()) return false;
        std::uint32_t cp = length == 1 ? byte : length == 2 ? (byte & 0x1Fu) : (byte & 0x0Fu);
        for (int k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((next & 0xC0u) != 0x80u) return false;
            cp = (cp << 6u) | (next & 0x3Fu);
        }
        out.insert(cp);
        i += static_cast<std::size_t>(length);
    }
    return true;
}

} // namespace

int main() {
    std::set<std::uint32_t> codepoints;
    for (std::size_t i = 0; i < kTextCount; ++i) {
        for (const Language language : {Language::kEnglish, Language::kSpanish}) {
            if (!decode(text(language, static_cast<Text>(i)), codepoints)) {
                std::fprintf(stderr, "mpo_launcher_font_test: FAILED: %s is not UTF-8\n",
                             text_id(static_cast<Text>(i)));
                ++g_failures;
            }
        }
    }
    for (const Language language : {Language::kEnglish, Language::kSpanish})
        decode(language_name(language), codepoints);
    // What the Spanish texts need, also if a text that has it changes.
    for (const std::uint32_t cp : {0xE1u, 0xE9u, 0xEDu, 0xF3u, 0xFAu, 0xF1u, 0xFCu, 0xBFu, 0xA1u,
                                   0xC1u, 0xC9u, 0xCDu, 0xD3u, 0xDAu, 0xD1u, 0xDCu})
        codepoints.insert(cp);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImFont *font = add_launcher_font(*ImGui::GetIO().Fonts);
    if (font == nullptr) {
        std::fprintf(stderr, "mpo_launcher_font_test: FAILED: no font\n");
        return 1;
    }
    int non_ascii = 0;
    for (const std::uint32_t cp : codepoints) {
        if (cp < 0x20u) continue;
        if (cp >= 0x80u) ++non_ascii;
        if (!font->IsGlyphInFont(static_cast<ImWchar>(cp))) {
            std::fprintf(stderr, "mpo_launcher_font_test: FAILED: no glyph for U+%04X\n",
                         static_cast<unsigned>(cp));
            ++g_failures;
        }
    }
    // The check can fail: the font has no CJK (a Japanese text would need
    // another font).
    if (font->IsGlyphInFont(static_cast<ImWchar>(0x4E00u))) {
        std::fprintf(stderr, "mpo_launcher_font_test: FAILED: U+4E00 should not be there\n");
        ++g_failures;
    }
    ImGui::DestroyContext();
    std::printf("mpo_launcher_font_test: %zu characters (%d not ASCII): %s\n", codepoints.size(),
                non_ascii, g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
