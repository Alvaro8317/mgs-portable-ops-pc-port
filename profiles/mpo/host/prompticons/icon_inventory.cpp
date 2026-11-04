// SPDX-License-Identifier: GPL-3.0-or-later

#include "icon_inventory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sstream>

#include <toml++/toml.hpp>

namespace mpo::prompticons {
namespace {

using frontend::PromptButton;

constexpr std::array<const char *, frontend::kPromptButtonCount> kButtonNames = {
    "cross", "circle", "triangle", "square", "start", "select", "l", "r"};

// A frame side or offset: textures are at most 512 texels a side.
constexpr std::int64_t kMaxFrame = 512;

bool ParseIcon(const toml::table &table, std::size_t index, IconEntry *out, std::string *error) {
    const std::string where = "[[icon]] " + std::to_string(index + 1);
    for (const auto &[key, node] : table) {
        const std::string_view name = key.str();
        if (name != "key" && name != "button" && name != "frame" && name != "source" &&
            name != "verified") {
            *error = where + ": unknown key '" + std::string(name) + "'";
            return false;
        }
        (void)node;
    }
    const auto key_text = table["key"].value<std::string>();
    if (!key_text) {
        *error = where + ": 'key' (24 hex digits) is missing";
        return false;
    }
    if (key_text->size() != 24 || !std::all_of(key_text->begin(), key_text->end(), [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        *error = where + ": key '" + *key_text + "' is not 24 hex digits";
        return false;
    }
    int level = 0;
    if (!texreplace::ParseHashName(*key_text, &out->key, &level) || level != 0) {
        *error = where + ": bad key '" + *key_text + "'";
        return false;
    }
    if ((out->key.cachekey >> 32) != 0) {
        *error = where + ": key '" + *key_text +
                 "' has an address (the first 8 digits must be 0: dump with ignoreAddress)";
        return false;
    }
    const auto button = table["button"].value<std::string>();
    if (!button) {
        *error = where + ": 'button' is missing";
        return false;
    }
    const auto parsed = PromptButtonFromName(*button);
    if (!parsed) {
        *error = where + ": unknown button '" + *button +
                 "' (cross, circle, triangle, square, start, select, l or r)";
        return false;
    }
    out->button = *parsed;
    if (const toml::node *frame = table.get("frame")) {
        const toml::array *array = frame->as_array();
        std::array<std::int64_t, 4> v{};
        bool ok = array != nullptr && array->size() == 4;
        for (std::size_t i = 0; ok && i < 4; ++i) {
            const auto n = (*array)[i].value<std::int64_t>();
            ok = n.has_value() && *n >= 0 && *n <= kMaxFrame;
            if (ok) v[i] = *n;
        }
        if (!ok || v[2] == 0 || v[3] == 0) {
            *error = where + ": 'frame' must be [x, y, w, h], each 0 to 512, w and h not 0";
            return false;
        }
        out->frame = {static_cast<int>(v[0]), static_cast<int>(v[1]), static_cast<int>(v[2]),
                      static_cast<int>(v[3])};
    }
    if (const toml::node *source = table.get("source")) {
        if (!source->is_string()) {
            *error = where + ": 'source' must be a string";
            return false;
        }
        out->source = *source->value<std::string>();
    }
    if (const toml::node *verified = table.get("verified")) {
        if (!verified->is_boolean()) {
            *error = where + ": 'verified' must be true or false";
            return false;
        }
        out->verified = *verified->value<bool>();
    }
    return true;
}

} // namespace

const char *PromptButtonName(PromptButton button) {
    return kButtonNames[static_cast<std::size_t>(button)];
}

std::optional<PromptButton> PromptButtonFromName(std::string_view name) {
    for (std::size_t i = 0; i < kButtonNames.size(); ++i)
        if (name == kButtonNames[i]) return static_cast<PromptButton>(i);
    return std::nullopt;
}

bool Inventory::Merge(std::string_view toml_text, std::string *error) {
    toml::parse_result result = toml::parse(toml_text);
    if (!result) {
        std::ostringstream message;
        message << result.error();
        *error = message.str();
        return false;
    }
    std::vector<IconEntry> parsed;
    for (const auto &[key, node] : result.table()) {
        const std::string_view name = key.str();
        if (name == "version") {
            if (node.value<std::int64_t>() != 1) {
                *error = "'version' must be 1";
                return false;
            }
            continue;
        }
        if (name != "icon" && name != "page") {
            *error = "unknown key '" + std::string(name) + "' (version, [[icon]] or [[page]])";
            return false;
        }
        const toml::array *array = node.as_array();
        if (array == nullptr || !array->is_array_of_tables()) {
            *error = "'" + std::string(name) + "' must be an array of tables ([[" +
                     std::string(name) + "]])";
            return false;
        }
        if (name == "page") continue;
        for (std::size_t i = 0; i < array->size(); ++i) {
            IconEntry entry;
            if (!ParseIcon(*(*array)[i].as_table(), i, &entry, error)) return false;
            parsed.push_back(std::move(entry));
        }
    }
    for (IconEntry &entry : parsed) {
        const auto low = static_cast<std::uint32_t>(entry.key.cachekey);
        if (auto it = index_.find(entry.key); it != index_.end()) {
            icons_[it->second] = std::move(entry);
            continue;
        }
        index_.emplace(entry.key, icons_.size());
        ++lows_[low];
        icons_.push_back(std::move(entry));
    }
    return true;
}

const IconEntry *Inventory::Find(const texreplace::ReplacementKey &key) const {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &icons_[it->second];
}

std::string Inventory::Canonical() const {
    std::vector<const IconEntry *> sorted;
    sorted.reserve(icons_.size());
    for (const IconEntry &e : icons_) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(),
              [](const IconEntry *a, const IconEntry *b) { return a->key < b->key; });
    std::string out;
    for (const IconEntry *e : sorted) {
        char line[96];
        std::snprintf(line, sizeof(line), "%s %s %d %d %d %d\n",
                      texreplace::HashName(e->key).c_str(), PromptButtonName(e->button), e->frame.x,
                      e->frame.y, e->frame.w, e->frame.h);
        out += line;
    }
    return out;
}

} // namespace mpo::prompticons
