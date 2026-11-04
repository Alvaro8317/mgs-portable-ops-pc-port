// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_launcher_core_test (F22b): the launcher's logic without a window. What
// `setup.py status --json` prints, the progress lines of the steps, the
// release layouts, the folder listing of the built-in browser. F24a: the
// game settings (from `status --json`, and settings.toml written back).
// F25d: the texts in English and Spanish, the system's locale and the
// language setting.

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "launcher_core.hpp"
#include "launcher_text.hpp"

namespace {

namespace fs = std::filesystem;
using namespace mpo::launcher;

int g_failures = 0;

void check(bool condition, const char *what) {
    if (condition) return;
    std::fprintf(stderr, "mpo_launcher_core_test: FAILED: %s\n", what);
    ++g_failures;
}

void touch(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

void test_json() {
    const auto value =
        parse_json(R"({"a": [1, -2.5e1, true, false, null], "b": "x\"\u00e9\ud83d\ude00",
                                       "c": {}})");
    check(value && value->kind == JsonValue::Kind::kObject, "object");
    if (!value) return;
    const JsonValue *a = value->find("a");
    check(a != nullptr && a->items.size() == 5u, "array");
    if (a != nullptr && a->items.size() == 5u) {
        check(a->items[1].number == -25.0, "number");
        check(a->items[2].boolean && !a->items[3].boolean, "booleans");
        check(a->items[4].kind == JsonValue::Kind::kNull, "null");
    }
    const JsonValue *b = value->find("b");
    check(b != nullptr && b->string == "x\"\xC3\xA9\xF0\x9F\x98\x80", "escapes and UTF-8");
    check(value->find("missing") == nullptr, "missing key");
    for (const char *bad :
         {"", "{", "{\"a\":}", "[1,]", "{\"a\":1} x", "\"\\ud800\"", "01x", "\"tab\there\""}) {
        check(!parse_json(bad), bad);
    }
    std::string deep(40, '[');
    deep += std::string(40, ']');
    check(!parse_json(deep), "nesting limit");
}

void test_status() {
    // As setup.py status --json prints it (F22b).
    const auto status = parse_status(R"({
  "data": "C:\\Users\\p\\AppData\\Local\\mpo-pc-port\\install",
  "iso": null,
  "toolchain": {"llvm-mingw": "20261020"},
  "disc_installed": true,
  "playable": true,
  "fonts": true,
  "stages_total": 92,
  "stages_installed": 7,
  "stages_optimized": 5,
  "stages_quick_only": 2,
  "stages_compiled_seconds": 1234
})");
    check(status.has_value(), "status parses");
    if (!status) return;
    check(status->data == "C:\\Users\\p\\AppData\\Local\\mpo-pc-port\\install", "data");
    check(status->iso.empty(), "null ISO");
    check(status->disc_installed && status->playable && status->fonts, "flags");
    check(status->stages_total == 92 && status->stages_installed == 7 &&
              status->stages_optimized == 5 && status->stages_quick_only == 2,
          "stage counts");
    const auto unknown = parse_status(R"({"stages_total": null})");
    check(unknown && unknown->stages_total == -1 && !unknown->playable && !unknown->fonts,
          "unknown total");
    check(!parse_status("[]"), "not an object");
}

void test_settings_in_status() {
    // As setup.py status --json prints them (F24a).
    const auto status = parse_status(R"({
  "playable": true,
  "settings_file": "/home/p/.config/mpo-pc-port/settings.toml",
  "settings": {"renderer": "soft", "render_scale": 4, "window_scale": 3, "fullscreen": true,
               "language": "es"},
  "settings_notices": ["settings.toml: window_scale = 30 is not valid (1 to 8): using 2", 7]
})");
    check(status.has_value(), "settings status parses");
    if (!status) return;
    check(status->settings_file == "/home/p/.config/mpo-pc-port/settings.toml", "settings file");
    const GameSettings expected{"soft", 4, 3, true, "native", "es"};
    check(status->settings == expected, "settings values");
    check(status->settings_notices.size() == 1u &&
              status->settings_notices[0].find("window_scale") != std::string::npos,
          "notices (strings only)");
    // Before F24a, or with values setup.py would not write: the defaults.
    const auto old = parse_status(R"({"playable": true})");
    check(old && old->settings == GameSettings{} && old->settings_file.empty() &&
              old->settings_notices.empty(),
          "no settings: defaults");
    check(!old->update_needed, "no update_needed: not needed");
    check(old->settings.texture_pack && old->texture_pack_dir.empty() && !old->texture_pack_found,
          "texture pack: on by default, nothing known");
    const auto pack = parse_status(R"({"settings": {"texture_pack": false},
                                       "texture_pack_dir": "/d/textures",
                                       "texture_pack_found": true})");
    check(pack && !pack->settings.texture_pack && pack->texture_pack_dir == "/d/textures" &&
              pack->texture_pack_found,
          "texture pack in the status");
    const auto pack_bad = parse_status(R"({"settings": {"texture_pack": "no"}})");
    check(pack_bad && pack_bad->settings.texture_pack, "texture pack: invalid is on");
    const auto update = parse_status(R"({"playable": true, "update_needed": true})");
    check(update && update->playable && update->update_needed, "update_needed (F25)");
    const auto bad = parse_status(R"({"settings": {"renderer": "opengl", "render_scale": 9,
                                                   "window_scale": 0, "fullscreen": "yes",
                                                   "language": "fr"}})");
    check(bad && bad->settings == GameSettings{}, "invalid values: defaults");
    const auto edge = parse_status(R"({"settings": {"render_scale": 8, "window_scale": 1}})");
    check(edge && edge->settings.render_scale == 8 && edge->settings.window_scale == 1 &&
              edge->settings.renderer == "vulkan",
          "range ends");
    check(GameSettings{} == (GameSettings{"vulkan", 1, 2, false, "native", "auto"}), "defaults");
    // The language: each valid value, and the rest as "auto".
    for (const char *language : {"auto", "en", "es"}) {
        const auto one =
            parse_status(std::string(R"({"settings": {"language": ")") + language + "\"}}");
        check(one && one->settings.language == language, language);
    }
    for (const char *language : {R"("ES")", R"("es_ES")", R"("")", "1", "true", "null", "[]"}) {
        const auto one =
            parse_status(std::string(R"({"settings": {"language": )") + language + "}}");
        check(one && one->settings.language == "auto", language);
    }
}

std::string read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // A Windows checkout may have turned the golden file into CRLF.
    std::string lf;
    for (const char c : text)
        if (c != '\r') lf += c;
    return lf;
}

void test_settings_file(const fs::path &tmp, const fs::path &golden) {
    // setup_script_test.py reads the same golden file with setup.py's
    // load_settings: what the launcher writes is what play reads.
    const GameSettings settings{"vulkan", 4, 3, true, "64:27", "es", false};
    const std::string text = format_settings(settings);
    check(text == read_text(golden), "format_settings matches settings_golden.txt");
    check(format_settings(GameSettings{}).find("render_scale = 1\nwindow_scale") ==
              std::string::npos,
          "a comment between keys");
    check(format_settings(GameSettings{}).find("renderer = \"vulkan\"") != std::string::npos &&
              format_settings(GameSettings{}).find("fullscreen = false") != std::string::npos,
          "defaults formatted");
    // Out of range values never reach the file.
    const std::string clamped =
        format_settings(GameSettings{"opengl", 20, 0, false, "16:10", "fr"});
    check(clamped.find("language = \"auto\"") != std::string::npos, "invalid language: auto");
    check(clamped.find("renderer = \"vulkan\"") != std::string::npos &&
              clamped.find("aspect = \"native\"") != std::string::npos &&
              clamped.find("render_scale = 8") != std::string::npos &&
              clamped.find("window_scale = 1") != std::string::npos,
          "clamped");

    const fs::path file = tmp / "config" / "mpo-pc-port" / "settings.toml";
    std::string error;
    check(save_settings(file, settings, error), "save creates the folder");
    check(read_text(file) == text, "saved text");
    check(read_text(file).find("language = \"es\"\n") != std::string::npos, "saved language");
    check(save_settings(file, GameSettings{"vulkan", 1, 2, false, "native", "en"}, error) &&
              read_text(file).find("language = \"en\"\n") != std::string::npos,
          "language replaced");
    check(save_settings(file, GameSettings{}, error), "save replaces");
    check(read_text(file) == format_settings(GameSettings{}), "replaced text");
    check(!fs::exists(fs::path(file) += ".tmp"), "no temporary file left");
    // A file where the folder should be.
    touch(tmp / "blocked");
    error.clear();
    check(!save_settings(tmp / "blocked" / "settings.toml", settings, error) && !error.empty(),
          "save fails with a reason");
}

// F25b: the aspects (setup.py's valid_aspect) and the Settings' choices.
void test_aspect() {
    for (const char *ok :
         {"native", "16:9", "30:17", "21:9", "64:27", "43:18", "32:9", "4:1", "99999:25000"})
        check(valid_aspect(ok), (std::string("valid aspect ") + ok).c_str());
    for (const char *bad : {"", "16:10", "4:3", "41:10", "wide", "21:09x", "0:9", "021:9",
                            "100000:30000", "21x9", "2.37", "Native", " 21:9", "21:", ":9"})
        check(!valid_aspect(bad), (std::string("invalid aspect '") + bad + "'").c_str());
    for (const AspectChoice &choice : kAspectChoices)
        check(valid_aspect(choice.value) && choice.label[0] != '\0',
              (std::string("choice ") + choice.value).c_str());
    check(std::string(kAspectChoices[0].value) == GameSettings{}.aspect, "native first");
    const auto wide = parse_status(R"({"settings": {"aspect": "32:9"}})");
    check(wide && wide->settings.aspect == "32:9", "aspect from status");
    const auto bad = parse_status(R"({"settings": {"aspect": "4:3"}})");
    check(bad && bad->settings.aspect == "native", "invalid aspect: native");
}

void test_pixel_size() {
    check(pixel_size(1) == "480x272", "1x");
    check(pixel_size(4) == "1920x1088", "4x");
    check(pixel_size(8) == "3840x2176", "8x");
}

void test_progress() {
    auto p = parse_progress_line("setup: stages: compiling st02 (3/92, quick (-O0))");
    check(p && p->step == "stages" && p->done == 3 && p->total == 92, "stage progress");
    p = parse_progress_line("setup: generate: stage_title (93/96)");
    check(p && p->step == "generate" && p->done == 93 && p->total == 96, "generate progress");
    p = parse_progress_line("setup: toolchain: llvm-mingw 40% (76 of 181 MB)");
    check(p && p->step == "toolchain" && p->done == -1, "no fraction in a percentage");
    p = parse_progress_line("setup: main: compiling mgp_main (the long wait)");
    check(p && p->message == "compiling mgp_main (the long wait)" && p->total == -1, "plain");
    check(!parse_progress_line("[12/602] Building CXX object"), "not a setup line");
    check(!parse_progress_line("setup: this is a sentence: no"), "step has no spaces");
}

void test_line_buffer() {
    LineBuffer buffer(3);
    auto lines = buffer.append("one\r\ntw");
    check(lines.size() == 1u && lines[0] == "one", "CRLF line");
    lines = buffer.append("o\nthree\nfour\nfi");
    check(lines.size() == 3u && lines[0] == "two", "split line");
    check(buffer.lines().size() == 3u && buffer.lines().front() == "two", "capacity");
    lines = buffer.finish();
    check(lines.size() == 1u && lines[0] == "fi" && buffer.lines().back() == "fi", "finish");
}

void test_paths(const fs::path &tmp) {
    const fs::path script = fs::path("profiles") / "mpo" / "scripts" / "setup.py";
    // The Windows ZIP: launcher, python/ and source/ side by side.
    const fs::path zip = tmp / "zip";
    touch(zip / "source" / script);
    touch(zip / "python" / "python.exe");
    auto paths = find_paths(zip, {}, {}, {}, true);
    check(paths && paths->setup_script == zip / "source" / script, "ZIP setup.py");
    check(paths && paths->python == zip / "python" / "python.exe", "embedded Python");
    paths = find_paths(zip, {}, {}, {}, false);
    check(paths && paths->python == "python3", "POSIX Python");
    // Flatpak: /app/bin/mpo_launcher, /app/share/mpo-pc-port/source.
    const fs::path app = tmp / "app";
    touch(app / "share" / "mpo-pc-port" / "source" / script);
    paths = find_paths(app / "bin", {}, {}, {}, false);
    check(paths && paths->setup_script == app / "share" / "mpo-pc-port" / "source" / script,
          "Flatpak setup.py");
    // The tree it was built from, and the explicit options.
    paths = find_paths(tmp / "nowhere", {}, {}, zip / "source", false);
    check(paths && paths->setup_script == zip / "source" / script, "compiled-in tree");
    paths = find_paths(tmp / "nowhere", zip / "source", "/opt/py", app, true);
    check(paths && paths->python == "/opt/py", "explicit Python");
    check(!find_paths(tmp / "nowhere", tmp / "empty", {}, zip / "source", false),
          "an explicit --source must hold setup.py");
    if (paths) {
        const std::array<std::string, 2> args{"--iso", "game.iso"};
        const auto command = setup_command(*paths, "install", tmp / "data", args);
        check(command.size() == 8u && command[1] == "-I" && command[3] == "install" &&
                  command[4] == "--data" && command[7] == "game.iso",
              "setup command");
        check(setup_command(*paths, "status", {}, {}).size() == 4u, "no --data");
    }
}

void test_listing(const fs::path &tmp) {
    const fs::path folder = tmp / "list";
    touch(folder / "b.ISO");
    touch(folder / "a.iso");
    touch(folder / "notes.txt");
    touch(folder / ".hidden.iso");
    fs::create_directories(folder / "Zeta");
    fs::create_directories(folder / "alpha");
    std::error_code ec;
    static constexpr std::array<std::string_view, 1> kIso{".iso"};
    auto entries = list_directory(folder, kIso, ec);
    check(!ec && entries.size() == 4u, "folders and ISOs");
    if (entries.size() == 4u) {
        check(entries[0].directory && entries[0].name == "alpha", "folders first, sorted");
        check(entries[1].directory && entries[1].name == "Zeta", "case-insensitive");
        check(!entries[2].directory && entries[2].name == "a.iso", "files sorted");
        check(entries[3].name == "b.ISO", "extension case");
    }
    entries = list_directory(folder, {}, ec);
    check(entries.size() == 2u, "folders only");
    entries = list_directory(folder / "missing", kIso, ec);
    check(ec && entries.empty(), "missing folder");
    fs::create_directories(folder / ".config");
    entries = list_directory(folder, {}, ec, /*show_hidden=*/true);
    check(!ec && entries.size() == 3u && entries[0].name == ".config", "hidden folders shown");
    entries = list_directory(folder, kIso, ec, /*show_hidden=*/true);
    check(entries.size() == 6u, "hidden files shown");
}

void test_ppsspp_saves(const fs::path &tmp) {
    const fs::path home = tmp / "home";
    const fs::path documents = home / "Documents";
    check(ppsspp_save_folders(home, documents, false).empty(), "no PPSSPP");
    check(ppsspp_save_folders(home, documents, true).empty(), "no PPSSPP (Windows)");
    const fs::path linux_saves = home / ".config" / "ppsspp" / "PSP" / "SAVEDATA";
    const fs::path flatpak_saves =
        home / ".var" / "app" / "org.ppsspp.PPSSPP" / "config" / "ppsspp" / "PSP" / "SAVEDATA";
    const fs::path windows_saves = documents / "PPSSPP" / "PSP" / "SAVEDATA";
    fs::create_directories(linux_saves);
    fs::create_directories(flatpak_saves);
    fs::create_directories(windows_saves);
    const auto on_linux = ppsspp_save_folders(home, documents, false);
    check(on_linux.size() == 2u && on_linux[0] == linux_saves && on_linux[1] == flatpak_saves,
          "PPSSPP saves on Linux");
    const auto on_windows = ppsspp_save_folders(home, documents, true);
    check(on_windows.size() == 1u && on_windows[0] == windows_saves, "PPSSPP saves on Windows");
    check(ppsspp_save_folders(home, {}, true).empty(), "no Documents folder");
}

void test_duration() {
    check(format_duration(45.2) == "45 s", "seconds");
    check(format_duration(750.0) == "12 min 30 s", "minutes");
    check(format_duration(3900.0) == "1 h 05 min", "hours");
    check(format_duration(-3.0) == "0 s", "negative");
}

// F25d: the table in both languages.
void test_texts() {
    check(kTextCount > 50u, "the table has the launcher's texts");
    std::set<std::string> ids;
    for (std::size_t i = 0; i < kTextCount; ++i) {
        const auto id = static_cast<Text>(i);
        const std::string name = text_id(id);
        check(ids.insert(name).second, "ids are unique");
        const std::string_view en = text(Language::kEnglish, id);
        const std::string_view es = text(Language::kSpanish, id);
        const std::string what = name + ": ";
        check(!en.empty(), (what + "English is missing").c_str());
        check(!es.empty(), (what + "Spanish is missing").c_str());
        check(placeholders(en) == placeholders(es),
              (what + "both languages use the same placeholders").c_str());
        // The code adds the ImGui ids ("##iso"): a text never has one.
        check(en.find("##") == std::string_view::npos && es.find("##") == std::string_view::npos,
              (what + "no ImGui id in a text").c_str());
        for (const std::string_view t : {en, es})
            check(t.empty() || (t.front() != ' ' && t.back() != ' '),
                  (what + "no space at the ends").c_str());
    }
    // Out of range: empty, never a crash.
    check(std::strlen(text(Language::kSpanish, static_cast<Text>(kTextCount))) == 0u,
          "out of range");
    check(std::string_view(text(Language::kEnglish, Text::kPlayButton)) == "Play" &&
              std::string_view(text(Language::kSpanish, Text::kPlayButton)) == "Jugar",
          "Play / Jugar");
    check(std::string_view(text(Language::kSpanish, Text::kResolution)) == "Resoluci\xC3\xB3n",
          "UTF-8 Spanish");
    // Nearly every text is translated (names such as "ISO: {0}" stay).
    std::size_t same = 0;
    for (std::size_t i = 0; i < kTextCount; ++i)
        if (std::string_view(text(Language::kEnglish, static_cast<Text>(i))) ==
            text(Language::kSpanish, static_cast<Text>(i)))
            ++same;
    check(same * 20u < kTextCount, "nearly every text has its Spanish");

    check(format_text("Stages compiled: {0} of {1}", {"5", "92"}) == "Stages compiled: 5 of 92",
          "format");
    check(format_text("{1} y {0}", {"a", "b"}) == "b y a", "reordered");
    check(format_text("{0}{0}", {"x"}) == "xx", "repeated");
    check(format_text("{2} {x} {} {{0}} {", {"a"}) == "{2} {x} {} {0}} {", "left as they are");
    check(format_text("100%", {}) == "100%", "no placeholders");
    check(placeholders("{1} {0} {{2}} {0}") == std::vector<int>({0, 0, 1}), "placeholders");
    check(std::string_view(language_name(Language::kSpanish)) == "Espa\xC3\xB1ol" &&
              std::string_view(language_name(Language::kEnglish)) == "English",
          "language names");
    check(std::string_view(language_code(Language::kSpanish)) == "es", "language code");
}

// F25d: the system's language (Windows gives "es-MX", Linux "es_MX.UTF-8").
void test_locale() {
    struct Case {
        const char *locale;
        int language; // -1: none, 0: English, 1: Spanish
    };
    for (const Case &c :
         {Case{"es", 1}, Case{"es_ES.UTF-8", 1}, Case{"es-MX", 1}, Case{"es-419", 1}, Case{"ES", 1},
          Case{"es_ES@euro", 1}, Case{"es.UTF-8", 1}, Case{"en_US.UTF-8", 0}, Case{"en-GB", 0},
          Case{"en", 0}, Case{"fr_FR.UTF-8", -1}, Case{"pt-BR", -1}, Case{"est", -1}, Case{"C", -1},
          Case{"POSIX", -1}, Case{"C.UTF-8", -1}, Case{"", -1}}) {
        const std::optional<Language> found = locale_language(c.locale);
        const int got = found ? static_cast<int>(*found) : -1;
        check(got == c.language, c.locale);
    }
    using Locales = std::vector<std::string>;
    check(language_from_locales(Locales{}) == Language::kEnglish, "no locale: English");
    check(language_from_locales(Locales{"es_AR"}) == Language::kSpanish, "Spanish");
    check(language_from_locales(Locales{"de_DE", "fr"}) == Language::kEnglish, "neither: English");
    check(language_from_locales(Locales{"fr_FR", "es_ES", "en"}) == Language::kSpanish,
          "the first one the launcher has");
    check(language_from_locales(Locales{"en_US", "es"}) == Language::kEnglish, "order wins");

    const auto env = [](std::map<std::string, std::string> values) {
        return [values = std::move(values)](const char *name) -> const char * {
            const auto it = values.find(name);
            return it != values.end() ? it->second.c_str() : nullptr;
        };
    };
    check(posix_locales(env({})).empty(), "no variables");
    check(posix_locales(env({{"LANG", "es_ES.UTF-8"}})) == Locales{"es_ES.UTF-8"}, "LANG");
    check(posix_locales(env({{"LANG", "en_US.UTF-8"}, {"LC_MESSAGES", "es_MX.UTF-8"}})) ==
              Locales{"es_MX.UTF-8"},
          "LC_MESSAGES over LANG");
    check(posix_locales(env({{"LANG", "es_ES.UTF-8"},
                             {"LC_MESSAGES", "es_MX.UTF-8"},
                             {"LC_ALL", "en_GB.UTF-8"}})) == Locales{"en_GB.UTF-8"},
          "LC_ALL over everything");
    check(posix_locales(env({{"LANG", "en_US.UTF-8"}, {"LC_ALL", ""}})) == Locales{"en_US.UTF-8"},
          "an empty variable is not set");
    check(posix_locales(env({{"LANG", "de_DE.UTF-8"}, {"LANGUAGE", "fr:es::en"}})) ==
              (Locales{"fr", "es", "en", "de_DE.UTF-8"}),
          "LANGUAGE first");
    check(posix_locales(env({{"LANG", "C"}, {"LANGUAGE", "es"}})).empty(),
          "LANGUAGE does not count with C");
    check(posix_locales(env({{"LANG", "C.UTF-8"}})).empty(), "C.UTF-8");
    check(language_from_locales(posix_locales(
              env({{"LANG", "de_DE.UTF-8"}, {"LANGUAGE", "fr:es"}}))) == Language::kSpanish,
          "a LANGUAGE list");

    check(valid_language_setting("auto") && valid_language_setting("en") &&
              valid_language_setting("es"),
          "valid settings");
    for (const char *bad : {"", "ES", "es_ES", "spanish", "fr", " es"})
        check(!valid_language_setting(bad), bad);
    check(resolve_language("auto", Language::kSpanish) == Language::kSpanish, "auto: system");
    check(resolve_language("auto", Language::kEnglish) == Language::kEnglish, "auto: system en");
    check(resolve_language("en", Language::kSpanish) == Language::kEnglish, "en wins");
    check(resolve_language("es", Language::kEnglish) == Language::kSpanish, "es wins");
    check(resolve_language("klingon", Language::kSpanish) == Language::kSpanish,
          "invalid: the system's");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: mpo_launcher_core_test <scratch directory> <settings_golden.txt>\n");
        return 2;
    }
    const fs::path tmp = fs::path(argv[1]) / "launcher_core_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    test_json();
    test_status();
    test_settings_in_status();
    test_settings_file(tmp, fs::path(argv[2]));
    test_aspect();
    test_pixel_size();
    test_progress();
    test_line_buffer();
    test_paths(tmp);
    test_listing(tmp);
    test_ppsspp_saves(tmp);
    test_duration();
    test_texts();
    test_locale();
    fs::remove_all(tmp, ec);
    std::printf("mpo_launcher_core_test: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
