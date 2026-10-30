// SPDX-License-Identifier: GPL-3.0-or-later
//
// The savedata files on the host (F18a).
// Ported from PPSSPP Core/Dialog/SavedataParam.cpp @ v1.20.4 (list building,
// Load, Save, Delete, crypt mode detection), on std::filesystem.
//
// Layout, as in PPSSPP: <ms0 root>/PSP/SAVEDATA/<gameName><saveName>/ with
// PARAM.SFO, the data file and ICON0.PNG, ICON1.PMF, PIC1.PNG, SND0.AT3. The
// ms0 root is the same directory the IO (F09) serves as ms0:/, because MPO
// also lists ms0:/PSP/SAVEDATA with sceIoDopen.
//
// Safety (PPSSPP #6600, where deleting a save could empty SAVEDATA):
//   - every directory and file name taken from a request or a listing must
//     be one plain path component (no separators, no "." or "..");
//   - delete only removes a directory that is a direct child of SAVEDATA,
//     exists, and is not a symlink; never SAVEDATA itself;
//   - delete with an empty gameName is refused unless the mode is
//     LISTALLDELETE (PPSSPP's own check, added for the MGS: Peace Walker
//     demo).
// Files are written to a temporary name and renamed, so a crash never leaves
// a half-written PARAM.SFO or data file.
//
// Deviations from PPSSPP, all deterministic: listings are sorted by name;
// a save's date is its newest file's; FOCUS_OLDEST picks the oldest save
// (PPSSPP's comparison never moves off entry 0).

#pragma once

#include "kernel/guest_memory.hpp"
#include "savedata/psp_types.hpp"
#include "savedata/savedata_cipher.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mpo::savedata {

struct SaveFileInfo {
    std::uint64_t size = 0; // 0: no data
    std::string save_name;
    std::string save_dir; // directory name under SAVEDATA
    int idx = 0;
    std::string title;
    std::string save_title;
    std::string save_detail;
    bool broken = false; // the directory has no readable PARAM.SFO
    std::optional<std::filesystem::file_time_type> modified;
};

struct StoreConfig {
    // sceKernelGetCompiledSdkVersion of the game; picks the crypt mode
    // (MPO: 0x02080010, mode 3).
    std::uint32_t compiled_sdk_version = 0;
    // Encrypt secure saves when a cipher is present (PPSSPP: bEncryptSave).
    bool encrypt_saves = true;
    // Largest file the store reads (the input is untrusted).
    std::uint64_t max_file_size = 32u << 20;
};

class SavedataStore {
public:
    explicit SavedataStore(std::filesystem::path ms0_root, SavedataCipher *cipher = nullptr,
                           StoreConfig config = {});

    [[nodiscard]] const std::filesystem::path &ms0_root() const noexcept { return ms0_root_; }
    [[nodiscard]] std::filesystem::path savedata_dir() const;

    // SetPspParam: builds the list of saves the request can see. Returns 0.
    int set_param(kernel::GuestMemory &memory, const SavedataParam *param);

    // Load/Save of one save directory (saveDirName is the saveName part).
    // Both return 0 or a firmware error, and update `param` like PPSSPP.
    int load(kernel::GuestMemory &memory, SavedataParam &param, const std::string &dir_name,
             bool secure_mode = true);
    int save(kernel::GuestMemory &memory, SavedataParam &param, const std::string &dir_name,
             bool secure_mode = true);
    // Delete: removes the save of list entry `save_id`. True when removed.
    bool remove(const SavedataParam &param, int save_id);

    [[nodiscard]] const std::vector<SaveFileInfo> &entries() const noexcept { return list_; }
    // saveNameListDataCount: entries that exist (or that may be created).
    [[nodiscard]] int listed_count() const noexcept { return listed_count_; }
    [[nodiscard]] const SaveFileInfo *entry(int idx) const;

    [[nodiscard]] std::string save_dir_name(const SavedataParam &param, int save_id = -1) const;
    [[nodiscard]] int selected_save() const noexcept { return selected_save_; }
    void set_selected_save(int idx) noexcept { selected_save_ = idx; }

    // Focus helpers (SceUtilitySavedataFocus).
    [[nodiscard]] int focus_index(const SavedataParam &param) const;

    // Crypt mode a save was written with (0: plain), from SAVEDATA_PARAMS.
    [[nodiscard]] int save_crypt_mode(const SavedataParam &param,
                                      const std::string &dir_name) const;
    [[nodiscard]] int determine_crypt_mode(const SavedataParam &param) const;

    // Set when the last load met an encrypted save and no cipher.
    [[nodiscard]] bool needs_cipher() const noexcept { return needs_cipher_; }

    // What the store did (F18b's report): loads, saves and deletes that
    // succeeded, and the directory of the last one.
    struct Activity {
        std::uint64_t loads = 0;
        std::uint64_t saves = 0;
        std::uint64_t removes = 0;
        std::string last_dir;
    };
    [[nodiscard]] const Activity &activity() const noexcept { return activity_; }
    // A short host-side reason for the last failure (empty if none).
    [[nodiscard]] const std::string &last_error() const noexcept { return last_error_; }

    static bool is_safe_component(const std::string &name);
    static std::string game_name(const SavedataParam &param);
    static std::string save_name(const SavedataParam &param);
    static std::string file_name(const SavedataParam &param);
    static bool has_key(const SavedataParam &param);
    static bool would_have_multi_save_name(const SavedataParam &param);

private:
    struct DirInfo {
        bool exists = false;
        std::uint64_t size = 0;
        std::optional<std::filesystem::file_time_type> modified;
    };

    [[nodiscard]] DirInfo dir_info(const std::string &dir_name) const;
    void set_file_info(SaveFileInfo &info, const DirInfo &dir, const std::string &listed_name,
                       const std::string &save_dir) const;
    int load_save_data(kernel::GuestMemory &memory, SavedataParam &param,
                       const std::string &dir_name, const std::filesystem::path &dir,
                       bool secure_mode);
    void load_file(kernel::GuestMemory &memory, const std::filesystem::path &path,
                   SavedataFileData &file_data) const;
    bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> &out) const;
    bool write_file(const std::filesystem::path &path, const std::uint8_t *data, std::size_t size);
    bool expected_hash(const std::filesystem::path &dir, const std::string &data_file,
                       std::uint8_t hash[16]) const;

    std::filesystem::path ms0_root_;
    SavedataCipher *cipher_;
    StoreConfig config_;
    std::vector<SaveFileInfo> list_;
    int listed_count_ = 0;
    int selected_save_ = 0;
    bool needs_cipher_ = false;
    Activity activity_;
    std::string last_error_;
};

} // namespace mpo::savedata
