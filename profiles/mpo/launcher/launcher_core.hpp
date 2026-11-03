// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's logic without a window (F22b), so it is tested without SDL:
// it reads what `setup.py status --json` says, follows the "setup: <step>:"
// lines the steps print, finds the Python and setup.py of the release layout
// and lists folders for the built-in file browser (a gamepad can drive it,
// unlike the system's file dialogs in the Steam Deck's game mode).
//
// The launcher only drives setup.py (docs/analysis/distribution.md §4): every
// step, its logs and its resumability stay in Python.

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mpo::launcher {

// Enough JSON for `setup.py status --json`.
struct JsonValue {
    enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };
    Kind kind{Kind::kNull};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<JsonValue> items;
    // An object: keys[i] names values[i] (a pair would need JsonValue complete).
    std::vector<std::string> keys;
    std::vector<JsonValue> values;

    [[nodiscard]] const JsonValue *find(std::string_view key) const;
};

// Nullopt on malformed input or nesting deeper than 32.
std::optional<JsonValue> parse_json(std::string_view text);

struct InstallStatus {
    std::string data;
    std::string iso;
    bool disc_installed{};
    bool playable{};
    // -1 when setup.py could not count them.
    int stages_total{-1};
    int stages_installed{};
    int stages_optimized{};
    int stages_quick_only{};
};

std::optional<InstallStatus> parse_status(std::string_view json);

// A "setup: <step>: <message>" line. `done`/`total` come from the first
// "(<done>/<total>" in the message, -1 when there is none.
struct Progress {
    std::string step;
    std::string message;
    int done{-1};
    int total{-1};
};

std::optional<Progress> parse_progress_line(std::string_view line);

// Splits a byte stream into lines, keeping the last `capacity`.
class LineBuffer {
public:
    explicit LineBuffer(std::size_t capacity = 400) : capacity_(capacity) {}
    // Returns the lines this chunk completed.
    std::vector<std::string> append(std::string_view bytes);
    // The unterminated rest, once the process is gone.
    std::vector<std::string> finish();
    [[nodiscard]] const std::vector<std::string> &lines() const noexcept { return lines_; }
    void clear();

private:
    void push(std::string line);

    std::size_t capacity_;
    std::string partial_;
    std::vector<std::string> lines_;
};

struct LauncherPaths {
    std::filesystem::path python;
    std::filesystem::path setup_script;
};

// Where things are, first match wins:
//   setup.py: `source` (--source), <base>/source (the Windows ZIP),
//             <base>/../share/mpo-pc-port/source (Flatpak, /app/bin), then
//             `compiled_source` (the tree the launcher was built from);
//   Python:   `python` (--python), <base>/python/python.exe on Windows, else
//             python3 from PATH ("python3", resolved by the OS).
std::optional<LauncherPaths> find_paths(const std::filesystem::path &base,
                                        const std::filesystem::path &source,
                                        const std::filesystem::path &python,
                                        const std::filesystem::path &compiled_source, bool windows);

// python -I setup.py <subcommand> [--data <dir>] <args...>
std::vector<std::string> setup_command(const LauncherPaths &paths, std::string_view subcommand,
                                       const std::filesystem::path &data,
                                       std::span<const std::string> args);

struct DirEntry {
    std::string name;
    bool directory{};
};

// Folders first, then the files whose extension is in `extensions` (lower
// case, with the dot; empty: folders only), each group sorted
// case-insensitively. Hidden entries (".name") are left out unless
// `show_hidden` (PPSSPP's saves are under ~/.config on Linux, F22c).
std::vector<DirEntry> list_directory(const std::filesystem::path &folder,
                                     std::span<const std::string_view> extensions,
                                     std::error_code &ec, bool show_hidden = false);

// The SAVEDATA folders of PPSSPP installs that exist on this machine, for the
// browser's shortcut when importing or exporting saves (F22c): Windows keeps
// them in Documents\PPSSPP; Linux in ~/.config/ppsspp, or in the PPSSPP
// Flatpak's folder.
std::vector<std::filesystem::path> ppsspp_save_folders(const std::filesystem::path &home,
                                                       const std::filesystem::path &documents,
                                                       bool windows);

// "1 h 05 min", "12 min 30 s", "45 s".
std::string format_duration(double seconds);

} // namespace mpo::launcher
