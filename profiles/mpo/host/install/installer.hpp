// SPDX-License-Identifier: GPL-3.0-or-later
//
// Validates a user disc image against the supported versions and installs the
// files later phases need (CLAUDE.md §1.2: exact identity or nothing).

#pragma once

#include "disc/iso9660.hpp"
#include "install/supported_versions.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace mpo::install {

struct DiscIdentity {
    std::string disc_id;
    std::string disc_version;
    std::string title;
    std::string psp_system_version;
};

struct ValidationResult {
    DiscIdentity identity;
    // Null when the disc ID/version is not supported at all.
    const SupportedVersion *version = nullptr;
    // Human-readable problems; empty means the image is accepted.
    std::vector<std::string> problems;

    [[nodiscard]] bool accepted() const { return version != nullptr && problems.empty(); }
};

// Reads PSP_GAME/PARAM.SFO.
bool read_disc_identity(disc::IsoImage &iso, DiscIdentity &identity, std::string &error);

// SHA-256 of a file inside the image, streamed in fixed-size chunks.
bool hash_iso_file(disc::IsoImage &iso, const disc::IsoEntry &entry, std::string &sha256,
                   std::string &error);

// Compares every non-ignored file of the image with the matching supported
// version: same set of files, same sizes and same SHA-256.
bool validate_image(disc::IsoImage &iso, const std::vector<SupportedVersion> &versions,
                    ValidationResult &result, std::string &error);

// Copies the files selected by `version.extract` to <game_dir>/disc/<path> and
// writes <game_dir>/install.toml. Call only with an accepted validation.
bool install_image(disc::IsoImage &iso, const std::filesystem::path &iso_path,
                   const ValidationResult &result, const std::filesystem::path &game_dir,
                   std::string &error);

} // namespace mpo::install
