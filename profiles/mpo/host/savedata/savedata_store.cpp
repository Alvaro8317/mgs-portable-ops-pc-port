// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Dialog/SavedataParam.cpp @ v1.20.4 (see
// savedata_store.hpp for the layout, the safety rules and the deviations).

#include "savedata/savedata_store.hpp"

#include "savedata/guest_text.hpp"
#include "savedata/sfo_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

namespace mpo::savedata {
namespace fs = std::filesystem;
namespace {

constexpr const char *kSfoFileName = "PARAM.SFO";
constexpr const char *kIcon0FileName = "ICON0.PNG";
constexpr const char *kIcon1FileName = "ICON1.PMF";
constexpr const char *kPic1FileName = "PIC1.PNG";
constexpr const char *kSnd0FileName = "SND0.AT3";

// SaveSFOFileListEntry: filename[13], hash[16], pad[3].
constexpr std::size_t kFileListEntrySize = 32;
constexpr std::size_t kFileListCountMax = 99;
constexpr std::size_t kFileListTotalSize = kFileListEntrySize * kFileListCountMax;
constexpr std::size_t kSavedataParamsSize = 128;
// Bound on the guest's saveNameList (it ends with an empty name).
constexpr int kMaxSaveNames = 1024;
// PPSSPP: "PSP always respond this and this unlock some game".
constexpr std::int32_t kLoadedBind = 1021;

bool is_mode(const SavedataParam &param, SavedataMode mode) {
    return param.mode == static_cast<std::uint32_t>(mode);
}

int sdk_main_version(std::uint32_t sdk) {
    if (sdk > 0x0307FFFFu) return 6;
    if (sdk > 0x0300FFFFu) return 5;
    if (sdk > 0x0206FFFFu) return 4;
    if (sdk > 0x0205FFFFu) return 3;
    if (sdk >= 0x02000000u) return 2;
    if (sdk >= 0x01000000u) return 1;
    return 0;
}

// Directory names under SAVEDATA, sorted (PPSSPP uses the listing order).
std::vector<std::string> list_directories(const fs::path &dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_directory(type_ec) || type_ec) continue;
        // PSP save directories are ASCII (gameName + saveName). Other names
        // are skipped: path::string() could not represent them on Windows.
        const fs::path name = it->path().filename();
        const auto &native = name.native();
        const bool ascii =
            std::all_of(native.begin(), native.end(), [](auto c) { return c > 0 && c < 0x80; });
        if (ascii) names.push_back(name.string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool load_sfo(const fs::path &path, SfoFile &sfo, std::uint64_t max_size) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > max_size) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in && !in.eof()) return false;
    std::string error;
    return sfo.parse(bytes, error);
}

// A path for messages, as UTF-8 (path::string() can throw on Windows).
std::string path_text(const fs::path &path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

std::uint8_t *guest_range(kernel::GuestMemory &memory, std::uint32_t address, std::uint32_t size) {
    if (address == 0) return nullptr;
    return memory.translate(address, size == 0 ? 1u : size);
}

} // namespace

SavedataStore::SavedataStore(fs::path ms0_root, SavedataCipher *cipher, StoreConfig config)
    : ms0_root_(std::move(ms0_root)), cipher_(cipher), config_(config) {}

fs::path SavedataStore::savedata_dir() const {
    return ms0_root_ / "PSP" / "SAVEDATA";
}

bool SavedataStore::is_safe_component(const std::string &name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    // Trailing dots and spaces are dropped by Windows, so "ABC." would alias
    // "ABC" there.
    return name.back() != '.' && name.back() != ' ';
}

std::string SavedataStore::game_name(const SavedataParam &param) {
    return fixed_string(param.game_name, sizeof(param.game_name));
}

std::string SavedataStore::save_name(const SavedataParam &param) {
    std::string name = fixed_string(param.save_name, sizeof(param.save_name));
    return name == "<>" ? std::string() : name;
}

std::string SavedataStore::file_name(const SavedataParam &param) {
    return fixed_string(param.file_name, sizeof(param.file_name));
}

bool SavedataStore::has_key(const SavedataParam &param) {
    return std::any_of(std::begin(param.key), std::end(param.key),
                       [](std::uint8_t b) { return b != 0; });
}

bool SavedataStore::would_have_multi_save_name(const SavedataParam &param) {
    switch (static_cast<SavedataMode>(param.mode)) {
    case SavedataMode::kLoad:
    case SavedataMode::kAutoLoad:
    case SavedataMode::kSave:
    case SavedataMode::kAutoSave:
    case SavedataMode::kMakeDataSecure:
    case SavedataMode::kMakeData:
    case SavedataMode::kReadDataSecure:
    case SavedataMode::kReadData:
    case SavedataMode::kWriteDataSecure:
    case SavedataMode::kWriteData:
    case SavedataMode::kAutoDelete:
    case SavedataMode::kDelete:
    case SavedataMode::kEraseSecure:
    case SavedataMode::kErase:
    case SavedataMode::kDeleteData:
        return false;
    default:
        return true;
    }
}

SavedataStore::DirInfo SavedataStore::dir_info(const std::string &dir_name) const {
    DirInfo info;
    if (!is_safe_component(dir_name)) return info;
    const fs::path dir = savedata_dir() / dir_name;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return info;
    info.exists = true;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) continue;
        const auto size = it->file_size(file_ec);
        if (!file_ec) info.size += size;
        const auto time = it->last_write_time(file_ec);
        if (!file_ec && (!info.modified || *info.modified < time)) info.modified = time;
    }
    return info;
}

void SavedataStore::set_file_info(SaveFileInfo &info, const DirInfo &dir,
                                  const std::string &listed_name,
                                  const std::string &save_dir) const {
    info = SaveFileInfo{};
    info.size = dir.size;
    info.save_name = listed_name;
    info.save_dir = save_dir;
    info.modified = dir.modified;
    SfoFile sfo;
    if (load_sfo(savedata_dir() / save_dir / kSfoFileName, sfo, config_.max_file_size)) {
        info.title = sfo.string("TITLE").value_or("");
        info.save_title = sfo.string("SAVEDATA_TITLE").value_or("");
        info.save_detail = sfo.string("SAVEDATA_DETAIL").value_or("");
    } else {
        info.broken = true;
        info.title = save_dir;
    }
}

int SavedataStore::set_param(kernel::GuestMemory &memory, const SavedataParam *param) {
    list_.clear();
    listed_count_ = 0;
    if (param == nullptr) return 0;

    if (is_mode(*param, SavedataMode::kListAllDelete)) {
        // Every save on the memory stick, whatever its game.
        for (const std::string &dir : list_directories(savedata_dir())) {
            if (!is_safe_component(dir)) continue;
            SaveFileInfo info;
            set_file_info(info, dir_info(dir), "", dir);
            info.idx = static_cast<int>(list_.size());
            list_.push_back(std::move(info));
        }
        listed_count_ = static_cast<int>(list_.size());
        return 0;
    }

    const bool list_empty_file =
        !is_mode(*param, SavedataMode::kListLoad) && !is_mode(*param, SavedataMode::kListDelete);
    const std::string game = game_name(*param);

    bool has_multiple = false;
    if (param->save_name_list != 0) {
        std::vector<std::string> names;
        for (int i = 0; i < kMaxSaveNames; ++i) {
            constexpr auto kNameSize = static_cast<std::uint32_t>(sizeof(SaveName));
            const std::uint8_t *name = memory.translate(
                param->save_name_list + static_cast<std::uint32_t>(i) * kNameSize, kNameSize);
            if (name == nullptr || name[0] == 0) break;
            names.push_back(fixed_string(reinterpret_cast<const char *>(name), sizeof(SaveName)));
        }
        if (!names.empty() && would_have_multi_save_name(*param)) {
            has_multiple = true;
            const std::vector<std::string> all = list_directories(savedata_dir());
            for (const std::string &name : names) {
                if (name == "<>") {
                    // "<>": the first of the game's saves not listed yet.
                    for (const std::string &dir : all) {
                        if (dir.compare(0, game.size(), game) != 0) continue;
                        const std::string candidate = dir.substr(game.size());
                        const bool listed =
                            std::any_of(list_.begin(), list_.end(), [&](const SaveFileInfo &e) {
                                return e.save_name == candidate;
                            });
                        if (listed) continue;
                        SaveFileInfo info;
                        set_file_info(info, dir_info(dir), candidate, dir);
                        list_.push_back(std::move(info));
                        break;
                    }
                    continue;
                }
                const std::string folder = game + name;
                if (!is_safe_component(folder)) continue; // never listed, never written
                const DirInfo dir = dir_info(folder);
                if (dir.exists) {
                    SaveFileInfo info;
                    set_file_info(info, dir, name, folder);
                    list_.push_back(std::move(info));
                } else if (list_empty_file) {
                    SaveFileInfo info;
                    info.save_name = name;
                    info.save_dir = folder;
                    list_.push_back(std::move(info));
                }
            }
            for (std::size_t i = 0; i < list_.size(); ++i) list_[i].idx = static_cast<int>(i);
            listed_count_ = static_cast<int>(list_.size());
        }
    }

    if (!has_multiple) {
        // Only the save the request names.
        const std::string name = save_name(*param);
        const std::string folder = game + name;
        SaveFileInfo info;
        info.save_name = name;
        info.save_dir = folder;
        const DirInfo dir = dir_info(folder);
        if (dir.exists) {
            set_file_info(info, dir, name, folder);
            listed_count_ = 1;
        }
        list_.push_back(std::move(info));
    }
    return 0;
}

const SaveFileInfo *SavedataStore::entry(int idx) const {
    if (idx < 0 || static_cast<std::size_t>(idx) >= list_.size()) return nullptr;
    return &list_[static_cast<std::size_t>(idx)];
}

std::string SavedataStore::save_dir_name(const SavedataParam &param, int save_id) const {
    if (save_id >= 0 && listed_count_ > 0) {
        const SaveFileInfo *info = entry(save_id);
        return info != nullptr ? info->save_name : std::string();
    }
    return save_name(param);
}

int SavedataStore::focus_index(const SavedataParam &param) const {
    if (!would_have_multi_save_name(param)) return 0;
    const int count = listed_count_;
    auto has_data = [&](int i) { return list_[static_cast<std::size_t>(i)].size != 0; };
    switch (static_cast<SavedataFocus>(param.focus)) {
    case SavedataFocus::kName: {
        const std::string name = save_name(param);
        for (int i = 0; i < count; ++i) {
            if (list_[static_cast<std::size_t>(i)].save_name == name) return i;
        }
        return 0;
    }
    case SavedataFocus::kFirstList:
        return 0;
    case SavedataFocus::kLastList:
        return count > 0 ? count - 1 : 0;
    case SavedataFocus::kLatest:
    case SavedataFocus::kOldest: {
        const bool latest = static_cast<SavedataFocus>(param.focus) == SavedataFocus::kLatest;
        int idx = 0;
        std::optional<fs::file_time_type> best;
        for (int i = 0; i < count; ++i) {
            const auto &modified = list_[static_cast<std::size_t>(i)].modified;
            if (!has_data(i) || !modified) continue;
            if (!best || (latest ? *best < *modified : *modified < *best)) {
                best = modified;
                idx = i;
            }
        }
        return idx;
    }
    case SavedataFocus::kFirstData:
        for (int i = 0; i < count; ++i) {
            if (has_data(i)) return i;
        }
        return 0;
    case SavedataFocus::kLastData:
        for (int i = count - 1; i >= 0; --i) {
            if (has_data(i)) return i;
        }
        return 0;
    case SavedataFocus::kFirstEmpty:
        for (int i = 0; i < count; ++i) {
            if (!has_data(i)) return i;
        }
        return 0;
    case SavedataFocus::kLastEmpty:
        for (int i = count - 1; i >= 0; --i) {
            if (!has_data(i)) return i;
        }
        return 0;
    }
    return 0;
}

int SavedataStore::determine_crypt_mode(const SavedataParam &param) const {
    const bool sdk4 = sdk_main_version(config_.compiled_sdk_version) >= 4;
    switch (param.secure_version) {
    case 1:
        return 1;
    case 2:
        return 3;
    case 3:
        return sdk4 ? 5 : 1;
    default:
        return has_key(param) ? (sdk4 ? 5 : 3) : 1;
    }
}

int SavedataStore::save_crypt_mode(const SavedataParam &param, const std::string &dir_name) const {
    const std::string folder = game_name(param) + dir_name;
    if (!is_safe_component(folder)) return 0;
    SfoFile sfo;
    if (!load_sfo(savedata_dir() / folder / kSfoFileName, sfo, config_.max_file_size)) return 0;
    const std::vector<std::uint8_t> *params = sfo.binary("SAVEDATA_PARAMS");
    if (params == nullptr || params->empty()) return 0;
    switch ((*params)[0]) {
    case 0x00:
        return 0;
    case 0x01:
        return 1;
    case 0x21:
        return 3;
    case 0x41:
        return 5;
    default:
        return 1; // not zero, so encrypted
    }
}

bool SavedataStore::read_file(const fs::path &path, std::vector<std::uint8_t> &out) const {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > config_.max_file_size) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<std::size_t>(in.gcount()) == out.size();
}

bool SavedataStore::write_file(const fs::path &path, const std::uint8_t *data, std::size_t size) {
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            last_error_ = "cannot create " + path_text(temporary);
            return false;
        }
        if (size > 0)
            out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
        out.flush();
        if (!out) {
            last_error_ = "cannot write " + path_text(temporary);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        last_error_ = "cannot replace " + path_text(path) + ": " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool SavedataStore::expected_hash(const fs::path &dir, const std::string &data_file,
                                  std::uint8_t hash[16]) const {
    SfoFile sfo;
    if (!load_sfo(dir / kSfoFileName, sfo, config_.max_file_size)) return false;
    const std::vector<std::uint8_t> *list = sfo.binary("SAVEDATA_FILE_LIST");
    if (list == nullptr) return false;
    const std::size_t count = std::min(kFileListCountMax, list->size() / kFileListEntrySize);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t *entry = list->data() + i * kFileListEntrySize;
        if (entry[0] == 0) continue;
        if (fixed_string(reinterpret_cast<const char *>(entry), 13) == data_file) {
            std::memcpy(hash, entry + 13, 16);
            return true;
        }
    }
    return false;
}

void SavedataStore::load_file(kernel::GuestMemory &memory, const fs::path &path,
                              SavedataFileData &file_data) const {
    if (file_data.buf == 0 || !memory.is_valid(file_data.buf)) return;
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes) || bytes.empty()) return;
    // PPSSPP clamps the read to the valid part of the buffer.
    std::uint32_t room = file_data.buf_size;
    while (room > 0 && memory.translate(file_data.buf, room) == nullptr) room /= 2;
    const auto size = static_cast<std::uint32_t>(std::min<std::size_t>(bytes.size(), room));
    if (size == 0) return;
    memory.write_bytes(file_data.buf, {bytes.data(), size});
    file_data.size = size;
}

int SavedataStore::load(kernel::GuestMemory &memory, SavedataParam &param,
                        const std::string &dir_name, bool secure_mode) {
    last_error_.clear();
    needs_cipher_ = false;
    const bool rw_mode =
        is_mode(param, SavedataMode::kReadData) || is_mode(param, SavedataMode::kReadDataSecure);
    const std::string folder = game_name(param) + dir_name;
    const std::string name = file_name(param);
    if (!is_safe_component(folder) || (!name.empty() && !is_safe_component(name))) {
        last_error_ = "unsafe save path";
        return static_cast<int>(rw_mode ? error::kSavedataRwBadParams : error::kSavedataLoadParam);
    }
    const fs::path dir = savedata_dir() / folder;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return static_cast<int>(rw_mode ? error::kSavedataRwNoData : error::kSavedataLoadNoData);
    }
    if (!name.empty() && !fs::is_regular_file(dir / name, ec)) {
        return static_cast<int>(rw_mode ? error::kSavedataRwFileNotFound
                                        : error::kSavedataLoadFileNotFound);
    }

    // Reset before loading, and kept at 0 on error.
    param.data_size = 0;
    if (const int result = load_save_data(memory, param, dir_name, dir, secure_mode); result != 0) {
        return result;
    }

    SfoFile sfo;
    if (!load_sfo(dir / kSfoFileName, sfo, config_.max_file_size)) {
        return static_cast<int>(rw_mode ? error::kSavedataRwDataBroken
                                        : error::kSavedataLoadDataBroken);
    }
    // strncpy semantics: the fields need not end with a NUL.
    set_fixed_string(param.sfo_param.title, sizeof(param.sfo_param.title),
                     sfo.string("TITLE").value_or(""), false);
    set_fixed_string(param.sfo_param.savedata_title, sizeof(param.sfo_param.savedata_title),
                     sfo.string("SAVEDATA_TITLE").value_or(""), false);
    set_fixed_string(param.sfo_param.detail, sizeof(param.sfo_param.detail),
                     sfo.string("SAVEDATA_DETAIL").value_or(""), false);
    param.sfo_param.parental_level =
        static_cast<std::uint8_t>(sfo.integer("PARENTAL_LEVEL").value_or(0) & 0xFFu);
    param.bind = kLoadedBind;

    load_file(memory, dir / kIcon0FileName, param.icon0_file_data);
    load_file(memory, dir / kIcon1FileName, param.icon1_file_data);
    load_file(memory, dir / kPic1FileName, param.pic1_file_data);
    load_file(memory, dir / kSnd0FileName, param.snd0_file_data);
    return 0;
}

int SavedataStore::load_save_data(kernel::GuestMemory &memory, SavedataParam &param,
                                  const std::string &dir_name, const fs::path &dir,
                                  bool secure_mode) {
    if (param.secure_version > 3) return static_cast<int>(error::kSavedataLoadParam);
    if (param.secure_version != 0 && param.secure_version != 1 && !has_key(param) && secure_mode) {
        return static_cast<int>(error::kSavedataLoadParam);
    }
    const std::string name = file_name(param);
    if (name.empty()) return 0; // a blank file name always succeeds

    std::vector<std::uint8_t> data;
    // PPSSPP's ReadPSPFile also fails on an empty file.
    if (!read_file(dir / name, data) || data.empty()) {
        return static_cast<int>(error::kSavedataLoadNoData);
    }
    set_fixed_string(param.save_name, sizeof(param.save_name), dir_name, false);

    const int previous_mode = save_crypt_mode(param, dir_name);
    if (previous_mode != 0 && secure_mode) {
        const int detected_mode = determine_crypt_mode(param);
        if (detected_mode > 1 && !has_key(param)) {
            return static_cast<int>(error::kSavedataLoadParam);
        }
        if (cipher_ == nullptr) {
            // PPSSPP would copy the encrypted bytes as if they were plain.
            needs_cipher_ = true;
            last_error_ = "the save is encrypted and no savedata cipher is configured";
            return static_cast<int>(error::kSavedataLoadDataBroken);
        }
        std::uint8_t hash[16];
        const bool has_hash = expected_hash(dir, name, hash);
        // The file's own mode wins; then the detected one; then no hash check.
        int mode = previous_mode;
        auto try_mode = [&](int try_with, const std::uint8_t *check) {
            std::vector<std::uint8_t> work = data;
            const std::uint8_t *key = try_with > 1 ? param.key : nullptr;
            if (cipher_->decrypt(try_with, key, work, check) != 0) return false;
            data = std::move(work);
            return true;
        };
        bool done = try_mode(mode, has_hash ? hash : nullptr);
        if (!done && detected_mode != mode) {
            mode = detected_mode;
            done = try_mode(mode, has_hash ? hash : nullptr);
        }
        if (!done && has_hash) done = try_mode(previous_mode, nullptr);
        if (!done) {
            last_error_ = "savedata decryption failed";
            return static_cast<int>(error::kSavedataLoadDataBroken);
        }
    }

    if (param.data_buf != 0 && memory.is_valid(param.data_buf)) {
        const auto size = static_cast<std::uint32_t>(data.size());
        if (size > param.data_buf_size ||
            (size > 0 && memory.translate(param.data_buf, size) == nullptr)) {
            return static_cast<int>(error::kSavedataLoadDataBroken);
        }
        memory.write_bytes(param.data_buf, data);
    }
    param.data_size = static_cast<std::uint32_t>(data.size());
    return 0;
}

int SavedataStore::save(kernel::GuestMemory &memory, SavedataParam &param,
                        const std::string &dir_name, bool secure_mode) {
    last_error_.clear();
    if (param.data_size > param.data_buf_size) {
        return static_cast<int>(error::kSavedataRwBadParams);
    }
    for (const SavedataFileData *file : {&param.icon0_file_data, &param.icon1_file_data,
                                         &param.pic1_file_data, &param.snd0_file_data}) {
        if (file->buf != 0 && memory.is_valid(file->buf) && file->buf_size < file->size) {
            return static_cast<int>(error::kSavedataRwBadParams);
        }
    }
    if (param.secure_version > 3) return static_cast<int>(error::kSavedataSaveParam);
    if (param.secure_version != 0 && param.secure_version != 1 && !has_key(param) && secure_mode) {
        return static_cast<int>(error::kSavedataSaveParam);
    }
    const std::string folder = game_name(param) + dir_name;
    const std::string name = file_name(param);
    if (!is_safe_component(folder) || (!name.empty() && !is_safe_component(name))) {
        last_error_ = "unsafe save path";
        return static_cast<int>(error::kSavedataSaveParam);
    }

    const fs::path dir = savedata_dir() / folder;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        last_error_ = "cannot create " + path_text(dir);
        return static_cast<int>(error::kSavedataSaveMsNoSpace);
    }

    // The data to write: the guest buffer, encrypted if asked.
    const bool has_data = param.data_buf != 0 && memory.is_valid(param.data_buf);
    std::vector<std::uint8_t> data;
    if (has_data) {
        std::uint32_t size = param.data_size;
        if (size == 0 || size > param.data_buf_size) size = param.data_buf_size;
        const std::uint8_t *host = guest_range(memory, param.data_buf, size);
        if (host == nullptr) return static_cast<int>(error::kSavedataRwBadParams);
        data.assign(host, host + size);
    }
    bool encrypted = false;
    std::array<std::uint8_t, 16> hash{};
    const int crypt_mode = determine_crypt_mode(param);
    if (has_data && secure_mode && config_.encrypt_saves && cipher_ != nullptr) {
        if (crypt_mode > 1 && !has_key(param)) return static_cast<int>(error::kSavedataSaveParam);
        std::vector<std::uint8_t> work = data;
        if (cipher_->encrypt(crypt_mode, crypt_mode > 1 ? param.key : nullptr, work, hash) == 0) {
            data = std::move(work);
            encrypted = true;
        } else {
            // PPSSPP warns that the save will not work on a real PSP and
            // writes it plain.
            hash.fill(0);
            last_error_ = "savedata encryption failed; saved plain";
        }
    }

    // PARAM.SFO.
    const fs::path sfo_path = dir / kSfoFileName;
    SfoFile sfo;
    load_sfo(sfo_path, sfo, config_.max_file_size); // or start empty
    const bool sub_write =
        is_mode(param, SavedataMode::kWriteDataSecure) || is_mode(param, SavedataMode::kWriteData);
    const bool was_encrypted = save_crypt_mode(param, dir_name) != 0;
    // PPSSPP #18430/#18687: only a request with a title updates these.
    if (param.sfo_param.title[0] != 0) {
        sfo.set_string("TITLE", fixed_string(param.sfo_param.title, sizeof(param.sfo_param.title)),
                       128);
        sfo.set_string(
            "SAVEDATA_TITLE",
            fixed_string(param.sfo_param.savedata_title, sizeof(param.sfo_param.savedata_title)),
            128);
        sfo.set_string("SAVEDATA_DETAIL",
                       fixed_string(param.sfo_param.detail, sizeof(param.sfo_param.detail)), 1024);
        sfo.set_integer("PARENTAL_LEVEL", param.sfo_param.parental_level, 4);
        sfo.set_string("CATEGORY", "MS", 4);
        sfo.set_string("SAVEDATA_DIRECTORY", folder, 64);
    }
    // The file list always has 99 entries; the data file's hash is kept or
    // replaced (zero when plain).
    std::vector<std::uint8_t> file_list(kFileListTotalSize, 0);
    if (const std::vector<std::uint8_t> *old = sfo.binary("SAVEDATA_FILE_LIST")) {
        std::memcpy(file_list.data(), old->data(), std::min(old->size(), kFileListTotalSize));
    }
    if (secure_mode && has_data) {
        for (std::size_t i = 0; i < kFileListCountMax; ++i) {
            std::uint8_t *entry = file_list.data() + i * kFileListEntrySize;
            if (entry[0] != 0 && fixed_string(reinterpret_cast<const char *>(entry), 13) != name) {
                continue;
            }
            std::memset(entry, 0, 13);
            std::memcpy(entry, name.data(), std::min<std::size_t>(name.size(), 12));
            std::memcpy(entry + 13, hash.data(), hash.size());
            break;
        }
    }
    sfo.set_binary("SAVEDATA_FILE_LIST", file_list, static_cast<std::uint32_t>(kFileListTotalSize));
    const std::array<std::uint8_t, kSavedataParamsSize> zeroes{};
    sfo.set_binary("SAVEDATA_PARAMS", zeroes, static_cast<std::uint32_t>(kSavedataParamsSize));

    std::vector<std::uint8_t> sfo_bytes = sfo.write();
    const std::size_t sfo_size = sfo_bytes.size();
    if ((encrypted || (sub_write && was_encrypted)) && cipher_ != nullptr) {
        if (const auto offset = sfo.data_offset("SAVEDATA_PARAMS")) {
            sfo_bytes.resize((sfo_size + 15) & ~std::size_t{15}, 0);
            cipher_->update_sfo_hash(sfo_bytes, *offset, crypt_mode);
        }
    }
    if (!write_file(sfo_path, sfo_bytes.data(), sfo_size)) {
        return static_cast<int>(error::kSavedataSaveMsNoSpace);
    }

    if (has_data) {
        set_fixed_string(param.save_name, sizeof(param.save_name), dir_name, false);
        if (!name.empty() && !write_file(dir / name, data.data(), data.size())) {
            return static_cast<int>(error::kSavedataSaveMsNoSpace);
        }
    }

    // The other files: ICON0 and PIC1 even when empty, ICON1 and SND0 only
    // with data, as in PPSSPP.
    auto write_extra = [&](const SavedataFileData &file, const char *extra_name, bool skip_empty) {
        if (file.buf == 0 || !memory.is_valid(file.buf)) return;
        if (skip_empty && file.size == 0) return;
        const std::uint8_t *host =
            file.size > 0 ? guest_range(memory, file.buf, file.size) : nullptr;
        if (file.size > 0 && host == nullptr) return;
        write_file(dir / extra_name, host, file.size);
    };
    write_extra(param.icon0_file_data, kIcon0FileName, false);
    write_extra(param.icon1_file_data, kIcon1FileName, true);
    write_extra(param.pic1_file_data, kPic1FileName, false);
    write_extra(param.snd0_file_data, kSnd0FileName, true);
    return 0;
}

bool SavedataStore::remove(const SavedataParam &param, int save_id) {
    last_error_.clear();
    // PPSSPP's check against the MGS: Peace Walker demo's empty gameName.
    if (game_name(param).empty() && !is_mode(param, SavedataMode::kListAllDelete)) {
        last_error_ = "refusing to delete with an empty gameName";
        return false;
    }
    const SaveFileInfo *info = entry(save_id);
    if (info == nullptr || !is_safe_component(info->save_dir)) {
        last_error_ = "no such save to delete";
        return false;
    }
    const fs::path base = savedata_dir();
    const fs::path dir = base / info->save_dir;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(dir, ec);
    if (ec || !fs::is_directory(status)) {
        last_error_ = "not a save directory: " + path_text(dir);
        return false;
    }
    // The resolved target must be a direct child of SAVEDATA (#6600).
    const fs::path canonical_base = fs::weakly_canonical(base, ec);
    if (ec) return false;
    const fs::path canonical_dir = fs::weakly_canonical(dir, ec);
    if (ec || canonical_dir.parent_path() != canonical_base || canonical_dir == canonical_base) {
        last_error_ = "refusing to delete outside SAVEDATA: " + path_text(dir);
        return false;
    }
    fs::remove_all(canonical_dir, ec);
    if (ec) {
        last_error_ = "cannot delete " + path_text(dir) + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace mpo::savedata
