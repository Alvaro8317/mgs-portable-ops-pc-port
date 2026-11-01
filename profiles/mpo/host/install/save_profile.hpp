// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the save converter (`mpo --import-save/--export-save`, F18c) needs to
// know about a supported version: the save directory prefix, the crypt mode
// and the game's savedata key. Loaded from profiles/mpo/config/savedata.toml.
//
// The key is a 16-byte constant of the game, so it is never in the repo
// (CLAUDE.md §1.1): read_save_key() takes it from the user's installed disc
// (<game_dir>/disc/<key_file>) and checks both the file's and the key's
// SHA-256, so a different disc or a wrong offset is refused, not guessed.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mpo::install {

struct SaveProfile {
    std::string disc_id;      // e.g. "ULUS10202"
    std::string disc_version; // e.g. "1.02"
    std::string game_name;    // save directory prefix, e.g. "ULUS10202"
    int crypt_mode = 0;       // 1, 3 or 5
    std::string key_file;     // relative to the disc root
    std::string key_file_sha256;
    std::uint64_t key_offset = 0;
    std::string key_sha256;
};

bool parse_save_profiles(std::string_view toml_text, std::vector<SaveProfile> &out,
                         std::string &error);
bool load_save_profiles(const std::filesystem::path &path, std::vector<SaveProfile> &out,
                        std::string &error);

// The installed version (disc_id and disc_version of <game_dir>/install.toml,
// written by `mpo --install`).
bool read_installed_version(const std::filesystem::path &game_dir, std::string &disc_id,
                            std::string &disc_version, std::string &error);

// The profile of the version installed in `game_dir`, with its key.
bool read_save_key(const std::filesystem::path &game_dir, const std::vector<SaveProfile> &profiles,
                   SaveProfile &profile, std::array<std::uint8_t, 16> &key, std::string &error);

} // namespace mpo::install
