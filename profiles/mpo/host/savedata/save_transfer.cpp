// SPDX-License-Identifier: GPL-3.0-or-later
//
// See save_transfer.hpp. The PARAM.SFO rules are those of PPSSPP's
// SavedataParam::Save (Core/Dialog/SavedataParam.cpp @ v1.20.4), as in
// savedata_store.cpp.

#include "savedata/save_transfer.hpp"

#include "savedata/guest_text.hpp"
#include "savedata/savedata_store.hpp"
#include "savedata/sfo_file.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>

namespace mpo::savedata {
namespace fs = std::filesystem;
namespace {

constexpr const char *kSfoFileName = "PARAM.SFO";
constexpr const char *kFileListKey = "SAVEDATA_FILE_LIST";
constexpr const char *kParamsKey = "SAVEDATA_PARAMS";
// SaveSFOFileListEntry: filename[13], hash[16], pad[3].
constexpr std::size_t kFileListEntrySize = 32;
constexpr std::size_t kFileListCountMax = 99;
constexpr std::size_t kSavedataParamsSize = 128;
// A save directory holds a handful of files (PARAM.SFO, the data, the icons).
constexpr std::size_t kMaxFiles = 64;
constexpr const char *kTemporaryPrefix = ".mpo-tmp-";

enum class Direction { kImport, kExport };

std::string path_text(const fs::path &path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

// PSP save and file names are ASCII; anything else is never touched.
bool ascii_name(const fs::path &name, std::string &out) {
    const auto &native = name.native();
    if (!std::all_of(native.begin(), native.end(), [](auto c) { return c > 0 && c < 0x80; })) {
        return false;
    }
    out = name.string();
    return SavedataStore::is_safe_component(out);
}

bool read_file(const fs::path &path, std::uint64_t max_size, std::vector<std::uint8_t> &out,
               std::string &error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        error = "cannot read " + path_text(path) + ": " + ec.message();
        return false;
    }
    if (size > max_size) {
        error = path_text(path) + " is too large (" + std::to_string(size) + " bytes)";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path_text(path);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if (out.empty()) return true;
    in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    if (static_cast<std::size_t>(in.gcount()) != out.size()) {
        error = "cannot read " + path_text(path);
        return false;
    }
    return true;
}

bool write_file(const fs::path &path, const std::vector<std::uint8_t> &data, std::string &error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out && !data.empty()) {
        out.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    out.flush();
    if (!out) {
        error = "cannot write " + path_text(path);
        return false;
    }
    return true;
}

bool is_zero(const std::uint8_t *bytes, std::size_t size) {
    return std::all_of(bytes, bytes + size, [](std::uint8_t b) { return b == 0; });
}

// The regular files of a save directory, by name. Anything else (a
// subdirectory, a link, a non-ASCII name) is refused rather than dropped.
bool list_files(const fs::path &dir, std::map<std::string, fs::path> &files, std::string &error) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        const fs::file_status status = it->symlink_status(type_ec);
        std::string name;
        if (type_ec || !fs::is_regular_file(status) || !ascii_name(it->path().filename(), name)) {
            error = path_text(it->path()) + ": a save directory holds only plain files";
            return false;
        }
        if (files.size() == kMaxFiles) {
            error = path_text(dir) + ": too many files for a save directory";
            return false;
        }
        files.emplace(name, it->path());
    }
    if (ec) {
        error = "cannot list " + path_text(dir) + ": " + ec.message();
        return false;
    }
    return true;
}

bool transfer(Direction direction, const fs::path &source_in, const fs::path &destination,
              SavedataCipher &cipher, const TransferProfile &profile,
              const TransferOptions &options, TransferResult &result, std::string &error) {
    result = TransferResult{};
    const fs::path source = source_in.has_filename() ? source_in : source_in.parent_path();
    std::string name;
    if (!ascii_name(source.filename(), name) || name.size() <= profile.game_name.size() ||
        name.compare(0, profile.game_name.size(), profile.game_name) != 0) {
        error = path_text(source) + ": not a " + profile.game_name + " save directory (" +
                profile.game_name + "<slot>)";
        return false;
    }
    result.name = name;
    std::error_code ec;
    if (!fs::is_directory(source, ec)) {
        error = path_text(source) + " is not a directory";
        return false;
    }
    const fs::path final_dir = destination / name;
    if (fs::exists(final_dir, ec)) {
        if (fs::equivalent(source, final_dir, ec)) {
            error = path_text(source) + ": the source and the destination are the same";
            return false;
        }
        if (!options.overwrite) {
            error = path_text(final_dir) + " already exists (use --overwrite to replace it)";
            return false;
        }
    }

    std::map<std::string, fs::path> files;
    if (!list_files(source, files, error)) return false;
    const auto sfo_it = files.find(kSfoFileName);
    if (sfo_it == files.end()) {
        error = path_text(source) + " has no PARAM.SFO";
        return false;
    }
    std::vector<std::uint8_t> sfo_bytes;
    if (!read_file(sfo_it->second, profile.max_file_size, sfo_bytes, error)) return false;
    SfoFile sfo;
    if (std::string parse_error; !sfo.parse(sfo_bytes, parse_error)) {
        error = path_text(sfo_it->second) + ": " + parse_error;
        return false;
    }
    result.source_mode = SavedataStore::crypt_mode_of(sfo);

    // The output files, by name; PARAM.SFO and the data files are rewritten.
    std::map<std::string, std::vector<std::uint8_t>> output;
    const bool convert =
        direction == Direction::kImport ? result.source_mode != 0 : result.source_mode == 0;
    if (!convert) {
        result.kind = TransferKind::kCopied;
        result.result_mode = result.source_mode;
        output.emplace(kSfoFileName, std::move(sfo_bytes));
    } else {
        const auto list_it = sfo.values().find(kFileListKey);
        if (list_it == sfo.values().end() || list_it->second.format != SfoFile::Format::kBinary) {
            error = path_text(sfo_it->second) + " has no SAVEDATA_FILE_LIST";
            return false;
        }
        std::vector<std::uint8_t> file_list = list_it->second.bytes;
        const std::uint32_t file_list_max = list_it->second.max_length;
        const int mode = direction == Direction::kImport ? result.source_mode : profile.crypt_mode;
        const std::uint8_t *key = mode > 1 ? profile.key.data() : nullptr;
        result.result_mode = direction == Direction::kImport ? 0 : mode;

        const std::size_t count =
            std::min(kFileListCountMax, file_list.size() / kFileListEntrySize);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint8_t *entry = file_list.data() + i * kFileListEntrySize;
            if (entry[0] == 0) continue;
            const std::string data_name = fixed_string(reinterpret_cast<const char *>(entry), 13);
            const auto data_it = files.find(data_name);
            if (data_name == kSfoFileName || data_it == files.end() ||
                output.count(data_name) != 0) {
                error = path_text(source) + ": SAVEDATA_FILE_LIST names '" + data_name +
                        "', which is not a data file of the save";
                return false;
            }
            std::vector<std::uint8_t> data;
            if (!read_file(data_it->second, profile.max_file_size, data, error)) return false;
            std::uint8_t *hash = entry + 13;
            if (direction == Direction::kImport) {
                if (is_zero(hash, 16)) {
                    error = path_text(data_it->second) +
                            ": the save is encrypted but has no file hash to check it against";
                    return false;
                }
                if (const int rc = cipher.decrypt(mode, key, data, hash); rc != 0) {
                    error = path_text(data_it->second) + ": decryption failed in mode " +
                            std::to_string(mode) +
                            (rc == -8 ? " (hash mismatch: another game's key, or a damaged save)"
                                      : " (error " + std::to_string(rc) + ")");
                    return false;
                }
                std::memset(hash, 0, 16);
            } else {
                if (data.empty()) {
                    error = path_text(data_it->second) + " is empty";
                    return false;
                }
                std::vector<std::uint8_t> encrypted = data;
                std::array<std::uint8_t, 16> new_hash{};
                if (const int rc = cipher.encrypt(mode, key, encrypted, new_hash); rc != 0) {
                    error = path_text(data_it->second) + ": encryption failed in mode " +
                            std::to_string(mode) + " (error " + std::to_string(rc) + ")";
                    return false;
                }
                // What a PSP or PPSSPP does on load: decrypt and check the hash.
                std::vector<std::uint8_t> check = encrypted;
                if (cipher.decrypt(mode, key, check, new_hash.data()) != 0 || check != data) {
                    error = path_text(data_it->second) + ": the encrypted file does not decrypt "
                                                         "back to the save";
                    return false;
                }
                std::memcpy(hash, new_hash.data(), 16);
                data = std::move(encrypted);
            }
            result.data.push_back(data_name);
            output.emplace(data_name, std::move(data));
        }
        if (result.data.empty()) {
            error = path_text(source) + ": SAVEDATA_FILE_LIST names no data file";
            return false;
        }
        sfo.set_binary(kFileListKey, file_list, file_list_max);
        const std::array<std::uint8_t, kSavedataParamsSize> zeroes{};
        sfo.set_binary(kParamsKey, zeroes, static_cast<std::uint32_t>(kSavedataParamsSize));
        std::vector<std::uint8_t> new_sfo = sfo.write();
        if (direction == Direction::kExport) {
            const auto offset = sfo.data_offset(kParamsKey);
            if (!offset || cipher.update_sfo_hash(new_sfo, *offset, mode) != 0) {
                error = path_text(sfo_it->second) + ": cannot hash PARAM.SFO";
                return false;
            }
        }
        output.emplace(kSfoFileName, std::move(new_sfo));
    }
    for (const auto &[file_name, path] : files) {
        if (output.count(file_name) != 0) continue;
        std::vector<std::uint8_t> bytes;
        if (!read_file(path, profile.max_file_size, bytes, error)) return false;
        result.copied.push_back(file_name);
        output.emplace(file_name, std::move(bytes));
    }

    // Write everything to a temporary directory, then move it into place.
    // The prefix is not a game name, so the runtime never lists it.
    const fs::path temporary = destination / (kTemporaryPrefix + name);
    fs::remove_all(temporary, ec);
    fs::create_directories(temporary, ec);
    if (ec || !fs::is_directory(temporary, ec)) {
        error = "cannot create " + path_text(temporary);
        return false;
    }
    for (const auto &[file_name, bytes] : output) {
        if (!write_file(temporary / file_name, bytes, error)) {
            fs::remove_all(temporary, ec);
            return false;
        }
    }
    if (fs::exists(final_dir, ec)) {
        fs::remove_all(final_dir, ec);
        if (ec) {
            error = "cannot replace " + path_text(final_dir) + ": " + ec.message();
            fs::remove_all(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, final_dir, ec);
    if (ec) {
        error = "cannot create " + path_text(final_dir) + ": " + ec.message();
        fs::remove_all(temporary, ec);
        return false;
    }
    return true;
}

} // namespace

std::vector<fs::path> find_save_directories(const fs::path &path, const std::string &game_name) {
    std::error_code ec;
    if (fs::is_regular_file(path / kSfoFileName, ec)) return {path};
    std::vector<fs::path> found;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        std::string name;
        if (!it->is_directory(type_ec) || type_ec || !ascii_name(it->path().filename(), name)) {
            continue;
        }
        if (name.size() > game_name.size() && name.compare(0, game_name.size(), game_name) == 0) {
            found.push_back(it->path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

bool import_save(const fs::path &source, const fs::path &destination_savedata,
                 SavedataCipher &cipher, const TransferProfile &profile,
                 const TransferOptions &options, TransferResult &result, std::string &error) {
    return transfer(Direction::kImport, source, destination_savedata, cipher, profile, options,
                    result, error);
}

bool export_save(const fs::path &source, const fs::path &destination_savedata,
                 SavedataCipher &cipher, const TransferProfile &profile,
                 const TransferOptions &options, TransferResult &result, std::string &error) {
    return transfer(Direction::kExport, source, destination_savedata, cipher, profile, options,
                    result, error);
}

} // namespace mpo::savedata
