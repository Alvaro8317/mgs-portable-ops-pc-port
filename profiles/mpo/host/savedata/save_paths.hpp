// SPDX-License-Identifier: GPL-3.0-or-later
//
// Default host folder for ms0:/ (F18a).
//
//   Windows: %APPDATA%\mpo-pc-port\ms0
//   Linux and others: $XDG_DATA_HOME/mpo-pc-port/ms0, or
//                     ~/.local/share/mpo-pc-port/ms0 when XDG_DATA_HOME is
//                     unset or not absolute (XDG Base Directory spec).
// Saves then live in <ms0>/PSP/SAVEDATA/, as on a memory stick. F18b passes
// this root to the IO of F09 (mpo_boot --ms0) unless the user gives one.
//
// The environment is injected, so tests never touch the real folder.

#pragma once

#include <filesystem>
#include <optional>

namespace mpo::savedata {

inline constexpr const char *kProjectDirName = "mpo-pc-port";

enum class HostOs : unsigned char { kWindows, kPosix };

[[nodiscard]] constexpr HostOs current_host_os() {
#ifdef _WIN32
    return HostOs::kWindows;
#else
    return HostOs::kPosix;
#endif
}

// The variables the default depends on; unset ones are empty.
struct PathEnvironment {
    std::optional<std::filesystem::path> appdata;       // %APPDATA%
    std::optional<std::filesystem::path> xdg_data_home; // $XDG_DATA_HOME
    std::optional<std::filesystem::path> home;          // $HOME

    // Reads the process environment (wide on Windows, without C4996).
    static PathEnvironment from_process();
};

// <data dir>/mpo-pc-port, or nothing when the environment has no usable base.
[[nodiscard]] std::optional<std::filesystem::path> default_data_dir(const PathEnvironment &env,
                                                                    HostOs os = current_host_os());

// <data dir>/mpo-pc-port/ms0.
[[nodiscard]] std::optional<std::filesystem::path> default_ms0_root(const PathEnvironment &env,
                                                                    HostOs os = current_host_os());

} // namespace mpo::savedata
