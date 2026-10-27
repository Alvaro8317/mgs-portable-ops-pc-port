// SPDX-License-Identifier: GPL-3.0-or-later

#include "savedata/save_paths.hpp"

#include <cstdlib>
#include <string>

namespace mpo::savedata {
namespace fs = std::filesystem;
namespace {

std::optional<fs::path> env_path(const char *name) {
#ifdef _WIN32
    // Wide, so non-ASCII user names survive; _wdupenv_s avoids C4996.
    std::wstring wide_name;
    for (const char *c = name; *c != '\0'; ++c) wide_name.push_back(static_cast<wchar_t>(*c));
    wchar_t *value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, wide_name.c_str()) != 0 || value == nullptr) return std::nullopt;
    fs::path out(value);
    std::free(value);
    if (out.empty()) return std::nullopt;
    return out;
#else
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return fs::path(value);
#endif
}

} // namespace

PathEnvironment PathEnvironment::from_process() {
    PathEnvironment env;
    env.appdata = env_path("APPDATA");
    env.xdg_data_home = env_path("XDG_DATA_HOME");
    env.home = env_path("HOME");
    return env;
}

std::optional<fs::path> default_data_dir(const PathEnvironment &env, HostOs os) {
    if (os == HostOs::kWindows) {
        if (!env.appdata || env.appdata->empty()) return std::nullopt;
        return *env.appdata / kProjectDirName;
    }
    // XDG: a relative XDG_DATA_HOME is invalid and must be ignored.
    if (env.xdg_data_home && !env.xdg_data_home->empty() &&
        env.xdg_data_home->has_root_directory()) {
        return *env.xdg_data_home / kProjectDirName;
    }
    if (env.home && !env.home->empty()) {
        return *env.home / ".local" / "share" / kProjectDirName;
    }
    return std::nullopt;
}

std::optional<fs::path> default_ms0_root(const PathEnvironment &env, HostOs os) {
    auto dir = default_data_dir(env, os);
    if (!dir) return std::nullopt;
    return *dir / "ms0";
}

} // namespace mpo::savedata
