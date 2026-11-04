// SPDX-License-Identifier: GPL-3.0-or-later
//
// F25a: the inventory of the game's button icons (config/prompt_icons.toml).
// Each entry is the content key of one texture (F24c, without the address),
// the PSP button it shows and the frame the layout shows of it. Only keys and
// numbers: the icons' art is never in the repository (CLAUDE.md §1).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "prompts.h"
#include "texture_key.hpp"

namespace mpo::prompticons {

struct IconFrame {
    int x = 0;
    int y = 0;
    int w = 0; // 0: the whole texture
    int h = 0;
    friend bool operator==(const IconFrame &, const IconFrame &) = default;
};

struct IconEntry {
    texreplace::ReplacementKey key; // the address half is zero
    frontend::PromptButton button = frontend::PromptButton::kCross;
    IconFrame frame;
    std::string source;
    bool verified = false;
};

class Inventory {
public:
    // Adds the [[icon]] entries of a prompt_icons.toml (an entry with a key
    // already present replaces it). [[page]] and `version` are accepted and
    // skipped. On an error nothing is added and `error` names the entry.
    bool Merge(std::string_view toml_text, std::string *error);

    [[nodiscard]] const std::vector<IconEntry> &icons() const { return icons_; }
    [[nodiscard]] bool empty() const { return icons_.empty(); }
    // The entry of an address-free key, or nullptr.
    [[nodiscard]] const IconEntry *Find(const texreplace::ReplacementKey &key) const;
    // True when some entry has this CLUT/size half: only those textures need
    // their texels hashed.
    [[nodiscard]] bool HasCacheKey(std::uint32_t low) const { return lows_.count(low) != 0; }
    // One line per entry (key, button, frame), in key order: what the icons
    // depend on, for the recording's digest.
    [[nodiscard]] std::string Canonical() const;

private:
    std::vector<IconEntry> icons_;
    std::unordered_map<texreplace::ReplacementKey, std::size_t, texreplace::ReplacementKeyHash>
        index_;
    std::unordered_map<std::uint32_t, int> lows_;
};

// config/prompt_icons.toml, built into the program.
[[nodiscard]] std::string_view BuiltinInventoryText();

// "cross", "circle", "triangle", "square", "start", "select", "l", "r".
[[nodiscard]] const char *PromptButtonName(frontend::PromptButton button);
[[nodiscard]] std::optional<frontend::PromptButton> PromptButtonFromName(std::string_view name);

} // namespace mpo::prompticons
