// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_launcher_core_test (F22b): the launcher's logic without a window. What
// `setup.py status --json` prints, the progress lines of the steps, the
// release layouts, the folder listing of the built-in browser.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    check(status->disc_installed && status->playable, "flags");
    check(status->stages_total == 92 && status->stages_installed == 7 &&
              status->stages_optimized == 5 && status->stages_quick_only == 2,
          "stage counts");
    const auto unknown = parse_status(R"({"stages_total": null})");
    check(unknown && unknown->stages_total == -1 && !unknown->playable, "unknown total");
    check(!parse_status("[]"), "not an object");
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
}

void test_duration() {
    check(format_duration(45.2) == "45 s", "seconds");
    check(format_duration(750.0) == "12 min 30 s", "minutes");
    check(format_duration(3900.0) == "1 h 05 min", "hours");
    check(format_duration(-3.0) == "0 s", "negative");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mpo_launcher_core_test <scratch directory>\n");
        return 2;
    }
    const fs::path tmp = fs::path(argv[1]) / "launcher_core_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    test_json();
    test_status();
    test_progress();
    test_line_buffer();
    test_paths(tmp);
    test_listing(tmp);
    test_duration();
    fs::remove_all(tmp, ec);
    std::printf("mpo_launcher_core_test: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
