// SPDX-License-Identifier: GPL-3.0-or-later
//
// Save conversion between a PSP or PPSSPP and the port (F18c), behind
// `mpo --import-save` and `mpo --export-save`.
//
// A PSP, and PPSSPP with "Encrypt savedata" (on by default), write MPO's saves
// encrypted in mode 5 (docs/analysis/utility.md §2.4). The runtime holds no
// keys and writes them plain (CLAUDE.md §9, decision 9), so the installer
// converts whole save directories:
//   - import: encrypted (mode 1, 3 or 5) -> plain, the way the runtime writes
//     a plain save: the data file decrypted, its hash in SAVEDATA_FILE_LIST
//     and the whole SAVEDATA_PARAMS zeroed;
//   - export: plain -> encrypted in the profile's mode, the way PPSSPP's
//     SavedataParam::Save does: a new IV and file hash, then UpdateHash over
//     PARAM.SFO. The result is decrypted back and compared before writing.
// Both copy every other regular file (ICON0.PNG, PIC1.PNG, ...) as is, and a
// save that is already in the wanted form is copied as is.
//
// This file holds no keys: the cipher is injected (the real one,
// crypto::KirkSavedataCipher, is linked only by the installer) and so is the
// game's savedata key (install/save_profile.hpp reads it from the user's
// disc). Inputs are untrusted: names, sizes and PARAM.SFO are checked, and
// nothing is written outside <destination>/<save directory name>.

#pragma once

#include "savedata/savedata_cipher.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mpo::savedata {

struct TransferProfile {
    // Save directories are <game_name><save name>, e.g. "ULUS10202001".
    std::string game_name;
    // The mode an export writes (5 for MPO: a key and SDK 0x02080010).
    int crypt_mode = 5;
    // The game's savedata key (SceUtilitySavedataParam::key).
    std::array<std::uint8_t, 16> key{};
    // Bound on every file read.
    std::uint64_t max_file_size = 16u << 20;
};

struct TransferOptions {
    // Replace an existing destination directory instead of failing.
    bool overwrite = false;
};

enum class TransferKind {
    kConverted, // decrypted (import) or encrypted (export)
    kCopied,    // already plain (import) or already encrypted (export)
};

struct TransferResult {
    std::string name; // the save directory name
    TransferKind kind = TransferKind::kConverted;
    int source_mode = 0;             // crypt mode of the source (0: plain)
    int result_mode = 0;             // crypt mode written
    std::vector<std::string> data;   // data files converted
    std::vector<std::string> copied; // other files copied
};

// The save directories under `path`: `path` itself when it holds PARAM.SFO,
// otherwise its subdirectories named <game_name>..., sorted. Other entries
// are skipped. Empty if there are none.
[[nodiscard]] std::vector<std::filesystem::path>
find_save_directories(const std::filesystem::path &path, const std::string &game_name);

// Converts the save directory `source` into <destination_savedata>/<name>,
// where <name> is the name of `source`. Returns false with `error` set, and
// then writes nothing: the files go to a temporary directory first.
bool import_save(const std::filesystem::path &source,
                 const std::filesystem::path &destination_savedata, SavedataCipher &cipher,
                 const TransferProfile &profile, const TransferOptions &options,
                 TransferResult &result, std::string &error);
bool export_save(const std::filesystem::path &source,
                 const std::filesystem::path &destination_savedata, SavedataCipher &cipher,
                 const TransferProfile &profile, const TransferOptions &options,
                 TransferResult &result, std::string &error);

} // namespace mpo::savedata
