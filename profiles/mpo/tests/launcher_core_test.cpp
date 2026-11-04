// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_launcher_core_test (F22b): the launcher's logic without a window. What
// `setup.py status --json` prints, the progress lines of the steps, the
// release layouts, the folder listing of the built-in browser. F24a: the
// game settings (from `status --json`, and settings.toml written back).

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "launcher_core.hpp"

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
  "settings": {"renderer": "soft", "render_scale": 4, "window_scale": 3, "fullscreen": true},
  "settings_notices": ["settings.toml: window_scale = 30 is not valid (1 to 8): using 2", 7]
})");
    check(status.has_value(), "settings status parses");
    if (!status) return;
    check(status->settings_file == "/home/p/.config/mpo-pc-port/settings.toml", "settings file");
    const GameSettings expected{"soft", 4, 3, true};
    check(status->settings == expected, "settings values");
    check(status->settings_notices.size() == 1u &&
              status->settings_notices[0].find("window_scale") != std::string::npos,
          "notices (strings only)");
    // Before F24a, or with values setup.py would not write: the defaults.
    const auto old = parse_status(R"({"playable": true})");
    check(old && old->settings == GameSettings{} && old->settings_file.empty() &&
              old->settings_notices.empty(),
          "no settings: defaults");
    const auto bad = parse_status(R"({"settings": {"renderer": "opengl", "render_scale": 9,
                                                   "window_scale": 0, "fullscreen": "yes"}})");
    check(bad && bad->settings == GameSettings{}, "invalid values: defaults");
    const auto edge = parse_status(R"({"settings": {"render_scale": 8, "window_scale": 1}})");
    check(edge && edge->settings.render_scale == 8 && edge->settings.window_scale == 1 &&
              edge->settings.renderer == "vulkan",
          "range ends");
    check(GameSettings{} == (GameSettings{"vulkan", 1, 2, false}), "defaults");
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
    const GameSettings settings{"vulkan", 4, 3, true, "64:27"};
    const std::string text = format_settings(settings);
    check(text == read_text(golden), "format_settings matches settings_golden.txt");
    check(format_settings(GameSettings{}).find("render_scale = 1\nwindow_scale") ==
              std::string::npos,
          "a comment between keys");
    check(format_settings(GameSettings{}).find("renderer = \"vulkan\"") != std::string::npos &&
              format_settings(GameSettings{}).find("fullscreen = false") != std::string::npos,
          "defaults formatted");
    // Out of range values never reach the file.
    const std::string clamped = format_settings(GameSettings{"opengl", 20, 0, false, "16:10"});
    check(clamped.find("renderer = \"vulkan\"") != std::string::npos &&
              clamped.find("aspect = \"native\"") != std::string::npos &&
              clamped.find("render_scale = 8") != std::string::npos &&
              clamped.find("window_scale = 1") != std::string::npos,
          "clamped");

    const fs::path file = tmp / "config" / "mpo-pc-port" / "settings.toml";
    std::string error;
    check(save_settings(file, settings, error), "save creates the folder");
    check(read_text(file) == text, "saved text");
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
    fs::remove_all(tmp, ec);
    std::printf("mpo_launcher_core_test: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
