// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builds small ISO9660 images and PARAM.SFO files in memory, so disc code can
// be tested without any game data.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mpo::test {

struct SyntheticFile {
    std::string path; // '/'-separated, e.g. "PSP_GAME/SYSDIR/EBOOT.BIN".
    std::vector<std::uint8_t> data;
};

struct SyntheticIso {
    std::vector<std::uint8_t> bytes;
    // Byte offset of each directory record in `bytes`, keyed by path
    // (directories without a trailing '/'). Lets tests corrupt records.
    std::map<std::string, std::size_t> record_offsets;
    std::uint32_t root_lba = 0;
};

[[nodiscard]] SyntheticIso build_iso(const std::vector<SyntheticFile> &files);

[[nodiscard]] std::vector<std::uint8_t>
build_param_sfo(const std::vector<std::pair<std::string, std::string>> &strings,
                const std::vector<std::pair<std::string, std::uint32_t>> &integers = {});

[[nodiscard]] std::vector<std::uint8_t> bytes_of(const std::string &text);

void store_le32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value);

// Writes bytes to a fresh file under the system temp directory.
[[nodiscard]] std::filesystem::path write_temp_file(const std::string &name,
                                                    const std::vector<std::uint8_t> &bytes);

} // namespace mpo::test
