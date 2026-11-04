// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher in English and Spanish (F25d), without SDL so it is tested
// without a window:
// - the texts, all in launcher_strings.inc (one line per text, both
//   languages side by side);
// - which language the system asks for (its preferred locales, "es_ES.UTF-8"
//   or "es-MX"): Spanish for "es", English for anything else;
// - the `language` setting of settings.toml: "auto" (the system's), "en" or
//   "es".

#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpo::launcher {

enum class Language { kEnglish, kSpanish };

inline constexpr std::size_t kLanguageCount = 2;

enum class Text : int {
#define MPO_TEXT(id, en, es) id,
#include "launcher_strings.inc"
#undef MPO_TEXT
};

inline constexpr std::size_t kTextCount = 0
#define MPO_TEXT(id, en, es) +1
#include "launcher_strings.inc"
#undef MPO_TEXT
    ;

// The text in that language (UTF-8, never null).
const char *text(Language language, Text id);
// The id as written in launcher_strings.inc ("kPlayButton"), for the tests.
const char *text_id(Text id);

// Fills {0}, {1}, ... with `args`; "{{" is a "{". A placeholder without its
// argument is left as it is, so a wrong translation shows, never crashes.
std::string format_text(std::string_view pattern, std::initializer_list<std::string_view> args);

// The placeholders a text uses, sorted ({1} and {0}: 0, 1), for the test that
// both languages agree.
std::vector<int> placeholders(std::string_view pattern);

// "English", "Español": each language by its own name.
const char *language_name(Language language);

// "en", "es".
const char *language_code(Language language);

// The language of a locale ("es_ES.UTF-8", "es-419", "ES", "en_US@euro"), or
// nullopt for one the launcher does not have (and for "C" and "POSIX").
std::optional<Language> locale_language(std::string_view locale);

// The first locale the launcher has a language for, else English.
Language language_from_locales(std::span<const std::string> locales);

// The locales a POSIX system asks for, as gettext reads them: LC_ALL, else
// LC_MESSAGES, else LANG; and before it the LANGUAGE list ("es:en"), unless
// that locale is "C" or "POSIX". `getenv` returns null when not set.
std::vector<std::string> posix_locales(const std::function<const char *(const char *)> &getenv);

// The `language` setting.
inline constexpr std::string_view kLanguageAuto = "auto";

// "auto", "en", "es"; anything else is not valid.
bool valid_language_setting(std::string_view setting);

// The language to show: the setting's, or the system's for "auto" (and for a
// value that is not valid).
Language resolve_language(std::string_view setting, Language system);

} // namespace mpo::launcher
