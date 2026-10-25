// SPDX-License-Identifier: GPL-3.0-or-later
//
// Supported disc versions, loaded from profiles/mpo/config/supported.toml.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mpo::install {

struct ExpectedFile {
    std::string path; // Relative to the disc root, as listed by IsoImage.
    std::uint32_t size = 0;
    std::string sha256; // Lower-case hex.
};

// A ~PSP module that the installer decrypts into <game_dir>/elf/<output>.
struct DecryptedFile {
    std::string path;       // Encrypted file on the disc; also listed in `files`.
    std::string module;     // Module name in the ~PSP header.
    std::string output;     // Plain file name under <game_dir>/elf/.
    std::uint32_t size = 0; // Size of the decrypted ELF.
    std::string sha256;     // Of the decrypted ELF, lower-case hex.
};

struct SupportedVersion {
    std::string disc_id;      // PARAM.SFO DISC_ID, e.g. "ULUS10202".
    std::string disc_version; // PARAM.SFO DISC_VERSION, e.g. "1.02".
    std::string title;
    // Disc paths that are not validated. An entry ending in '/' matches a
    // whole directory.
    std::vector<std::string> ignored;
    // Files copied to the game directory. "*.ext" matches by suffix.
    std::vector<std::string> extract;
    std::vector<ExpectedFile> files;
    std::vector<DecryptedFile> decrypt;
};

[[nodiscard]] bool path_matches(std::string_view path, std::string_view pattern);
[[nodiscard]] bool is_ignored(const SupportedVersion &version, std::string_view path);
[[nodiscard]] bool is_extracted(const SupportedVersion &version, std::string_view path);

bool parse_supported_versions(std::string_view toml_text, std::vector<SupportedVersion> &out,
                              std::string &error);
bool load_supported_versions(const std::filesystem::path &path, std::vector<SupportedVersion> &out,
                             std::string &error);

} // namespace mpo::install
