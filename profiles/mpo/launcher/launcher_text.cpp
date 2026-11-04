// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher in English and Spanish (F25d); see launcher_text.hpp.

#include "launcher_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <optional>
#include <utility>

namespace mpo::launcher {

namespace {

struct Entry {
    const char *id;
    std::array<const char *, kLanguageCount> texts;
};

constexpr Entry kEntries[] = {
#define MPO_TEXT(id, en, es) {#id, {en, es}},
#include "launcher_strings.inc"
#undef MPO_TEXT
};

static_assert(std::size(kEntries) == kTextCount);

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

} // namespace

const char *text(Language language, Text id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kTextCount) return "";
    return kEntries[index].texts[static_cast<std::size_t>(language)];
}

const char *text_id(Text id) {
    const auto index = static_cast<std::size_t>(id);
    return index < kTextCount ? kEntries[index].id : "";
}

namespace {

// The number of a "{<digits>}" at `pos`, and where it ends.
std::optional<std::pair<int, std::size_t>> placeholder_at(std::string_view pattern,
                                                          std::size_t pos) {
    std::size_t end = pos + 1;
    int number = 0;
    while (end < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[end])) &&
           end - pos <= 3) {
        number = number * 10 + (pattern[end] - '0');
        ++end;
    }
    if (end == pos + 1 || end >= pattern.size() || pattern[end] != '}') return std::nullopt;
    return std::make_pair(number, end + 1);
}

} // namespace

std::string format_text(std::string_view pattern, std::initializer_list<std::string_view> args) {
    std::string out;
    out.reserve(pattern.size());
    std::size_t pos = 0;
    while (pos < pattern.size()) {
        if (pattern[pos] == '{' && pos + 1 < pattern.size() && pattern[pos + 1] == '{') {
            out += '{';
            pos += 2;
            continue;
        }
        if (pattern[pos] == '{') {
            if (const auto found = placeholder_at(pattern, pos);
                found && static_cast<std::size_t>(found->first) < args.size()) {
                out += *(args.begin() + found->first);
                pos = found->second;
                continue;
            }
        }
        out += pattern[pos++];
    }
    return out;
}

std::vector<int> placeholders(std::string_view pattern) {
    std::vector<int> found;
    std::size_t pos = 0;
    while (pos < pattern.size()) {
        if (pattern[pos] == '{' && pos + 1 < pattern.size() && pattern[pos + 1] == '{') {
            pos += 2;
        } else if (const auto at =
                       pattern[pos] == '{' ? placeholder_at(pattern, pos) : std::nullopt) {
            found.push_back(at->first);
            pos = at->second;
        } else {
            ++pos;
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

const char *language_name(Language language) {
    return language == Language::kSpanish ? "Español" : "English";
}

const char *language_code(Language language) {
    return language == Language::kSpanish ? "es" : "en";
}

std::optional<Language> locale_language(std::string_view locale) {
    // The language is what comes before the territory, the codeset or the
    // modifier: "es" in "es_ES.UTF-8", "es-419" and "es@euro".
    const std::size_t end = locale.find_first_of("_-.@");
    std::string language;
    for (const char c : locale.substr(0, end)) language += lower(c);
    if (language == "es") return Language::kSpanish;
    if (language == "en") return Language::kEnglish;
    return std::nullopt;
}

Language language_from_locales(std::span<const std::string> locales) {
    for (const std::string &locale : locales)
        if (const std::optional<Language> language = locale_language(locale)) return *language;
    return Language::kEnglish;
}

std::vector<std::string> posix_locales(const std::function<const char *(const char *)> &getenv) {
    const auto value = [&](const char *name) -> std::string {
        const char *text = getenv(name);
        return text != nullptr ? text : "";
    };
    std::string locale = value("LC_ALL");
    if (locale.empty()) locale = value("LC_MESSAGES");
    if (locale.empty()) locale = value("LANG");
    std::vector<std::string> locales;
    if (locale.empty() || locale == "C" || locale == "POSIX" || locale.starts_with("C.")) {
        // gettext ignores LANGUAGE then: the program is untranslated.
        return locales;
    }
    const std::string list = value("LANGUAGE");
    std::size_t start = 0;
    while (start <= list.size() && !list.empty()) {
        const std::size_t colon = std::min(list.find(':', start), list.size());
        if (colon > start) locales.push_back(list.substr(start, colon - start));
        start = colon + 1;
    }
    locales.push_back(locale);
    return locales;
}

bool valid_language_setting(std::string_view setting) {
    return setting == kLanguageAuto || setting == "en" || setting == "es";
}

Language resolve_language(std::string_view setting, Language system) {
    if (setting == "en") return Language::kEnglish;
    if (setting == "es") return Language::kSpanish;
    return system;
}

} // namespace mpo::launcher
