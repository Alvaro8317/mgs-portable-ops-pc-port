// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_savedata (F18a): PARAM.SFO, the savedata files and the savedata
// dialog, end to end in a temporary ms0:/ (never the real save folder).
// The requests follow MPO's savedata library (docs/analysis/utility.md):
// gameName ULUS10202, saveName "%03d", file "00000000.000", 0xA610 bytes,
// a non-zero key with secureVersion 0, focus LATEST. The key, the data and
// the titles here are made up; no game data is needed.

#include "disc/param_sfo.hpp"
#include "savedata/guest_text.hpp"
#include "savedata/save_dialog.hpp"
#include "savedata/save_paths.hpp"
#include "savedata/savedata_cipher.hpp"
#include "savedata/savedata_store.hpp"
#include "savedata/sfo_file.hpp"
#include "savedata_test_util.hpp"
#include "util/sha256.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace savedata_test;
namespace sd = mpo::savedata;
namespace btn = mpo::savedata::button;
namespace fs = std::filesystem;
using mpo::kernel::VirtualClock;

constexpr std::uint32_t kRequest = 0x08900000u;
constexpr std::uint32_t kNameList = 0x08901000u;
constexpr std::uint32_t kDataBuf = 0x08910000u;
constexpr std::uint32_t kDataBufSize = 0x20000u;
constexpr std::uint32_t kIconBuf = 0x08940000u;
constexpr std::uint32_t kMpoDataSize = 0xA610u;
constexpr std::uint32_t kMpoSdk = 0x02080010u;

// A temporary ms0:/ root, removed at the end.
class TempRoot {
public:
    TempRoot() {
        std::random_device rd;
        std::uniform_int_distribution<unsigned> dist;
        for (int attempt = 0; attempt < 16; ++attempt) {
            char name[48];
            std::snprintf(name, sizeof(name), "mpo_savedata_test_%08x%08x", dist(rd), dist(rd));
            path_ = fs::temp_directory_path() / name;
            std::error_code ec;
            if (fs::create_directory(path_, ec) && !ec) return;
        }
        path_.clear();
    }
    ~TempRoot() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempRoot(const TempRoot &) = delete;
    TempRoot &operator=(const TempRoot &) = delete;
    [[nodiscard]] const fs::path &path() const { return path_; }

private:
    fs::path path_;
};

std::vector<std::uint8_t> read_all(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void write_all(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// SHA-256 of every file under `dir`, by relative path.
std::map<std::string, std::string> tree_hashes(const fs::path &dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        const std::string rel = fs::relative(entry.path(), dir).generic_string();
        out[rel] = entry.is_regular_file() ? mpo::sha256_hex(read_all(entry.path())) : "<dir>";
    }
    return out;
}

std::map<std::string, std::string> without_prefix(std::map<std::string, std::string> tree,
                                                  const std::string &prefix) {
    for (auto it = tree.begin(); it != tree.end();) {
        it = it->first.rfind(prefix, 0) == 0 ? tree.erase(it) : std::next(it);
    }
    return tree;
}

std::vector<std::uint8_t> pattern(std::uint32_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> out(size);
    std::uint32_t x = seed * 2654435761u + 1;
    for (auto &b : out) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    return out;
}

constexpr std::array<std::uint8_t, 16> kFakeKey = {0x13, 0x37, 0xC0, 0xDE, 1, 2,  3,  4,
                                                   5,    6,    7,    8,    9, 10, 11, 12};

// FUN_08b97f5c + the load/save builders of title, with made-up content.
sd::SavedataParam mpo_request(sd::SavedataMode mode, int slot) {
    sd::SavedataParam p{};
    p.common = mpo_common(sd::kSavedataSizeV3, 0, 0x18);
    p.mode = static_cast<std::uint32_t>(mode);
    std::memcpy(p.game_name, "ULUS10202", 9);
    if (slot > 0) std::snprintf(p.save_name, sizeof(p.save_name), "%03d", slot);
    std::snprintf(p.file_name, sizeof(p.file_name), "%s", "00000000.000");
    p.data_buf = kDataBuf;
    p.data_buf_size = kDataBufSize;
    p.focus = static_cast<std::uint32_t>(sd::SavedataFocus::kLatest);
    std::memcpy(p.key, kFakeKey.data(), kFakeKey.size());
    p.secure_version = 0;
    return p;
}

sd::SavedataParam mpo_save_request(sd::SavedataMode mode, int slot, std::uint32_t seed) {
    sd::SavedataParam p = mpo_request(mode, slot);
    p.overwrite_mode = 1;
    p.data_size = kMpoDataSize;
    std::snprintf(p.sfo_param.title, sizeof(p.sfo_param.title), "TEST TITLE %u", seed);
    std::snprintf(p.sfo_param.savedata_title, sizeof(p.sfo_param.savedata_title), "SLOT");
    std::snprintf(p.sfo_param.detail, sizeof(p.sfo_param.detail), "line 1\nline 2");
    p.sfo_param.parental_level = 9;
    p.icon0_file_data = {kIconBuf, 0x100, 0x40, 0};
    return p;
}

// Lists "001".."count" at kNameList, like the builders with saveNameList.
void put_name_list(Guest &guest, sd::SavedataParam &p, int count) {
    for (int i = 0; i <= count; ++i) {
        sd::SaveName name{};
        if (i < count) std::snprintf(name, sizeof(name), "%03d", i + 1);
        std::memcpy(guest.at(kNameList + static_cast<std::uint32_t>(i) * 20), name, 20);
    }
    p.save_name_list = kNameList;
    p.save_name[0] = 0;
}

struct Session {
    VirtualClock clock;
    Guest guest;
    sd::SavedataStore store;
    sd::SaveDialog dialog;
    explicit Session(const fs::path &root, sd::SavedataCipher *cipher = nullptr)
        : store(root, cipher, sd::StoreConfig{kMpoSdk, true, 32u << 20}), dialog(clock, store) {}

    sd::SavedataParam result() { return guest.get<sd::SavedataParam>(kRequest); }

    // InitStart and wait for RUNNING (fade done).
    int start(const sd::SavedataParam &request) {
        guest.put(kRequest, request);
        const int r = dialog.init(guest.memory, kRequest);
        if (r != 0) return r;
        wait_running(dialog, clock, sd::SaveDialog::kInitDelayUs);
        // An invisible action works on its first Update: no fade to wait for.
        if (dialog.screen() != sd::SaveScreen::kNone) finish_fade_in(dialog, guest);
        return 0;
    }
    void update(const sd::ButtonState &buttons = {}) { dialog.update(guest.memory, 2, buttons); }
    // Updates until FINISHED, then ShutdownStart until NONE.
    void close() {
        run_until_finished(dialog, guest);
        CHECK_EQ(dialog.shutdown(), 0);
        clock.advance(sd::SaveDialog::kShutdownDelayUs);
        CHECK_EQ(static_cast<int>(dialog.status()), static_cast<int>(DialogStatus::kNone));
    }
    // An invisible action: 2 updates, the request written back.
    sd::SavedataParam run_invisible(const sd::SavedataParam &request) {
        CHECK_EQ(start(request), 0);
        CHECK(dialog.screen() == sd::SaveScreen::kNone);
        update();
        update();
        CHECK_EQ(static_cast<int>(dialog.status()), static_cast<int>(DialogStatus::kFinished));
        close();
        return result();
    }
};

// --- PARAM.SFO --------------------------------------------------------------

void test_sfo_round_trip() {
    sd::SfoFile sfo;
    sfo.set_string("TITLE", "METAL GEAR SOLID", 128);
    sfo.set_string("CATEGORY", "MS", 4);
    sfo.set_integer("PARENTAL_LEVEL", 9);
    const std::vector<std::uint8_t> params(128, 0);
    sfo.set_binary("SAVEDATA_PARAMS", params, 128);
    sfo.set_string("LONG", std::string(300, 'x'), 16); // truncated to 15 + NUL
    const std::vector<std::uint8_t> bytes = sfo.write();

    // PPSSPP's layout: version 0x0101, keys sorted, values at max_length.
    CHECK_EQ(bytes[4] | (bytes[5] << 8), 0x0101);
    sd::SfoFile again;
    std::string error;
    CHECK(again.parse(bytes, error));
    CHECK_EQ(again.string("TITLE").value_or(""), std::string("METAL GEAR SOLID"));
    CHECK_EQ(again.string("LONG").value_or(""), std::string(15, 'x'));
    CHECK_EQ(again.integer("PARENTAL_LEVEL").value_or(0), 9u);
    CHECK(again.binary("SAVEDATA_PARAMS") != nullptr);
    CHECK_EQ(again.binary("SAVEDATA_PARAMS")->size(), 128u);
    CHECK_EQ(again.values().at("TITLE").max_length, 128u);
    CHECK(again.write() == bytes);
    // data_offset points at the value inside write()'s bytes.
    const auto offset = again.data_offset("TITLE");
    CHECK(offset.has_value());
    CHECK(std::memcmp(bytes.data() + *offset, "METAL GEAR SOLID", 16) == 0);
    std::vector<std::string> keys;
    for (const auto &entry : again.values()) keys.push_back(entry.first);
    CHECK(std::is_sorted(keys.begin(), keys.end()));

    // The disc's reader (host/disc) reads the same strings and integers.
    mpo::disc::ParamSfo disc;
    CHECK(disc.parse(bytes, error));
    CHECK_EQ(disc.string("TITLE").value_or(""), std::string("METAL GEAR SOLID"));
    CHECK_EQ(disc.integer("PARENTAL_LEVEL").value_or(0), 9u);

    // Truncated or corrupted input never crashes and never reads past it.
    for (std::size_t size = 0; size < bytes.size(); size += 7) {
        sd::SfoFile cut;
        cut.parse({bytes.data(), size}, error);
    }
    std::mt19937 rng(1234);
    for (int round = 0; round < 500; ++round) {
        std::vector<std::uint8_t> bad = bytes;
        for (int flips = 0; flips < 4; ++flips) {
            bad[rng() % bad.size()] = static_cast<std::uint8_t>(rng());
        }
        sd::SfoFile corrupt;
        if (corrupt.parse(bad, error)) {
            // Whatever parses must write without overflowing its values.
            const auto rewritten = corrupt.write();
            CHECK(!rewritten.empty());
        }
    }
    CHECK(!sd::SfoFile().parse(std::vector<std::uint8_t>(10, 0), error));
}

// --- Save -> load -> delete -------------------------------------------------

void test_save_load_delete_cycle() {
    TempRoot root;
    CHECK(!root.path().empty());
    Session s(root.path());
    const fs::path savedata = s.store.savedata_dir();
    const fs::path slot1 = savedata / "ULUS10202001";

    // A save of another game that must survive everything below.
    write_all(savedata / "ULJM05193DATA00" / "PARAM.SFO", pattern(300, 7));
    write_all(savedata / "ULJM05193DATA00" / "DATA.BIN", pattern(1000, 8));
    const auto other_before = tree_hashes(savedata / "ULJM05193DATA00");

    // AUTOSAVE to slot 1 (MPO's flag 0x41).
    const auto data = pattern(kMpoDataSize, 1);
    s.guest.memory.write_bytes(kDataBuf, data);
    s.guest.memory.write_bytes(kIconBuf, pattern(0x40, 2));
    auto saved = s.run_invisible(mpo_save_request(sd::SavedataMode::kAutoSave, 1, 1));
    CHECK_EQ(saved.common.result, 0);
    CHECK(fs::is_directory(slot1));
    CHECK(read_all(slot1 / "00000000.000") == data);
    CHECK_EQ(read_all(slot1 / "ICON0.PNG").size(), 0x40u);
    CHECK_EQ(sd::fixed_string(saved.save_name, sizeof(saved.save_name)), std::string("001"));
    // No temporary files are left.
    for (const auto &entry : fs::directory_iterator(slot1)) {
        CHECK(entry.path().extension() != ".tmp");
    }
    // The SFO PPSSPP would write: plain save, so SAVEDATA_PARAMS is zero.
    sd::SfoFile sfo;
    std::string error;
    CHECK(sfo.parse(read_all(slot1 / "PARAM.SFO"), error));
    CHECK_EQ(sfo.string("TITLE").value_or(""), std::string("TEST TITLE 1"));
    CHECK_EQ(sfo.string("SAVEDATA_DETAIL").value_or(""), std::string("line 1\nline 2"));
    CHECK_EQ(sfo.string("CATEGORY").value_or(""), std::string("MS"));
    CHECK_EQ(sfo.string("SAVEDATA_DIRECTORY").value_or(""), std::string("ULUS10202001"));
    CHECK_EQ(sfo.integer("PARENTAL_LEVEL").value_or(0), 9u);
    CHECK(sfo.binary("SAVEDATA_PARAMS") != nullptr);
    CHECK(std::all_of(sfo.binary("SAVEDATA_PARAMS")->begin(), sfo.binary("SAVEDATA_PARAMS")->end(),
                      [](std::uint8_t b) { return b == 0; }));
    const auto *list = sfo.binary("SAVEDATA_FILE_LIST");
    CHECK(list != nullptr && list->size() == 99u * 32u);
    CHECK(list != nullptr && std::memcmp(list->data(), "00000000.000", 13) == 0);
    CHECK_EQ(s.store.save_crypt_mode(saved, "001"), 0);
    const auto after_save = tree_hashes(savedata);

    // AUTOLOAD back into a cleared buffer (flag 4).
    s.guest.memory.fill(kDataBuf, 0, kDataBufSize);
    auto load = mpo_request(sd::SavedataMode::kAutoLoad, 1);
    load.icon0_file_data = {kIconBuf, 0x100, 0, 0};
    const auto loaded = s.run_invisible(load);
    CHECK_EQ(loaded.common.result, 0);
    CHECK_EQ(loaded.data_size, kMpoDataSize); // MPO checks this exact size
    CHECK_EQ(loaded.bind, 1021);
    CHECK_EQ(loaded.icon0_file_data.size, 0x40u);
    CHECK_EQ(sd::fixed_string(loaded.sfo_param.title, sizeof(loaded.sfo_param.title)),
             std::string("TEST TITLE 1"));
    CHECK_EQ(loaded.sfo_param.parental_level, 9);
    CHECK(std::memcmp(s.guest.at(kDataBuf), data.data(), data.size()) == 0);
    // Loading never writes to the memory stick.
    CHECK(tree_hashes(savedata) == after_save);

    // LISTSAVE (flag 3) over slots 001-003; focus LATEST is slot 1.
    auto list_save = mpo_save_request(sd::SavedataMode::kListSave, 0, 2);
    put_name_list(s.guest, list_save, 3);
    const auto data2 = pattern(kMpoDataSize, 2);
    s.guest.memory.write_bytes(kDataBuf, data2);
    CHECK_EQ(s.start(list_save), 0);
    CHECK(s.dialog.screen() == sd::SaveScreen::kSaveListChoice);
    auto view = s.dialog.view();
    CHECK_EQ(view.entries.size(), 3u);
    CHECK_EQ(view.selected, 0);
    CHECK(view.entries.size() == 3 && view.entries[0].size > 0 && view.entries[1].size == 0);
    s.update(press(btn::kDown));
    CHECK_EQ(s.dialog.view().selected, 1);
    s.update(press(btn::kCircle)); // empty slot: saves at once
    CHECK(s.dialog.screen() == sd::SaveScreen::kSaveDone);
    s.update(press(btn::kCross));
    s.close();
    CHECK_EQ(s.result().common.result, 0);
    CHECK_EQ(sd::fixed_string(s.result().save_name, sizeof(sd::SaveName)), std::string("002"));
    CHECK(read_all(savedata / "ULUS10202002" / "00000000.000") == data2);
    // Slot 1 is untouched.
    CHECK(read_all(slot1 / "00000000.000") == data);

    // LISTSAVE over slot 1: overwrite asks, starting on "No".
    s.guest.memory.write_bytes(kDataBuf, pattern(kMpoDataSize, 3));
    CHECK_EQ(s.start(list_save), 0);
    // FOCUS_LATEST may land on either save (their dates can tie): go to 001.
    s.update(press(btn::kUp));
    CHECK_EQ(s.dialog.view().selected, 0);
    s.update(press(btn::kCircle));
    CHECK(s.dialog.screen() == sd::SaveScreen::kSaveConfirmOverwrite);
    CHECK(!s.dialog.view().yes_selected);
    s.update(press(btn::kCircle)); // "No": back to the list
    CHECK(s.dialog.screen() == sd::SaveScreen::kSaveListChoice);
    s.update(press(btn::kCross)); // cancel
    s.close();
    CHECK_EQ(s.result().common.result, sd::kDialogResultCancel);
    CHECK(read_all(slot1 / "00000000.000") == data);

    // LISTLOAD (flag 2) lists only the saves that exist.
    auto list_load = mpo_request(sd::SavedataMode::kListLoad, 0);
    put_name_list(s.guest, list_load, 3);
    CHECK_EQ(s.start(list_load), 0);
    CHECK(s.dialog.screen() == sd::SaveScreen::kLoadListChoice);
    CHECK_EQ(s.dialog.view().entries.size(), 2u);
    s.update(press(btn::kDown));
    s.update(press(btn::kCircle));
    CHECK(s.dialog.screen() == sd::SaveScreen::kLoadDone);
    s.update(press(btn::kCircle));
    s.close();
    CHECK_EQ(s.result().common.result, 0);
    CHECK_EQ(sd::fixed_string(s.result().save_name, sizeof(sd::SaveName)), std::string("002"));
    CHECK(std::memcmp(s.guest.at(kDataBuf), data2.data(), data2.size()) == 0);

    // LISTALLDELETE (flag 0x20): every save on the stick, any game. Delete
    // ULUS10202002 only.
    const auto before_delete = tree_hashes(savedata);
    auto all_delete = mpo_request(sd::SavedataMode::kListAllDelete, 0);
    std::memset(all_delete.game_name, 0, sizeof(all_delete.game_name)); // MPO leaves it empty
    std::memset(all_delete.file_name, 0, sizeof(all_delete.file_name));
    CHECK_EQ(s.start(all_delete), 0);
    CHECK(s.dialog.screen() == sd::SaveScreen::kDeleteListChoice);
    view = s.dialog.view();
    CHECK_EQ(view.entries.size(), 3u);
    int target = -1;
    for (std::size_t i = 0; i < view.entries.size(); ++i) {
        if (view.entries[i].save_dir == "ULUS10202002") target = static_cast<int>(i);
    }
    CHECK(target >= 0);
    for (int i = 0; i < 3; ++i) s.update(press(btn::kUp));
    CHECK_EQ(s.dialog.view().selected, 0);
    for (int i = 0; i < target; ++i) s.update(press(btn::kDown));
    CHECK_EQ(s.dialog.view().selected, target);
    s.update(press(btn::kCircle));
    CHECK(s.dialog.screen() == sd::SaveScreen::kDeleteConfirm);
    s.update(press(btn::kLeft)); // "Yes"
    s.update(press(btn::kCircle));
    CHECK(s.dialog.screen() == sd::SaveScreen::kDeleteDone);
    s.update(press(btn::kCross)); // back to the list (two saves left)
    CHECK(s.dialog.screen() == sd::SaveScreen::kDeleteListChoice);
    CHECK_EQ(s.dialog.view().entries.size(), 2u);
    s.update(press(btn::kCross));
    s.close();
    CHECK_EQ(s.result().common.result, sd::kDialogResultCancel);
    // #6600: only that save's files changed.
    CHECK(!fs::exists(savedata / "ULUS10202002"));
    CHECK(fs::is_directory(savedata));
    CHECK(tree_hashes(savedata) == without_prefix(before_delete, "ULUS10202002"));
    CHECK(tree_hashes(savedata / "ULJM05193DATA00") == other_before);
}

// --- PPSSPP #6600: delete must only touch the chosen save ---------------------

void test_delete_safety() {
    TempRoot root;
    Session s(root.path());
    const fs::path savedata = s.store.savedata_dir();
    write_all(savedata / "ULUS10202001" / "PARAM.SFO", pattern(64, 1));
    write_all(savedata / "ULUS10202001" / "00000000.000", pattern(64, 2));
    write_all(savedata / "ULUS10202003" / "00000000.000", pattern(64, 3));
    write_all(savedata / "LOOSEFILE.BIN", pattern(16, 4)); // a file, not a save
    write_all(root.path() / "OUTSIDE" / "KEEP.BIN", pattern(16, 5));
    const auto before = tree_hashes(root.path());

    // DELETE with an empty gameName is refused (PPSSPP's MGS PW demo check).
    auto request = mpo_request(sd::SavedataMode::kDelete, 1);
    std::memset(request.game_name, 0, sizeof(request.game_name));
    std::snprintf(request.save_name, sizeof(request.save_name), "ULUS10202001");
    s.store.set_param(s.guest.memory, &request);
    CHECK(!s.store.remove(request, 0));
    CHECK(tree_hashes(root.path()) == before);

    // A save that does not exist: nothing is removed, and never SAVEDATA.
    request = mpo_request(sd::SavedataMode::kDelete, 7);
    s.store.set_param(s.guest.memory, &request);
    CHECK(!s.store.remove(request, 0));
    CHECK(!s.store.remove(request, 5)); // out of range
    CHECK(!s.store.remove(request, -1));
    CHECK(tree_hashes(root.path()) == before);

    // Unsafe names never reach the file system.
    for (const char *name : {"/../../OUTSIDE", "/..", "\\..\\x", "a/b", ":", ""}) {
        auto bad = mpo_save_request(sd::SavedataMode::kAutoSave, 0, 9);
        std::snprintf(bad.save_name, sizeof(bad.save_name), "%s", name);
        if (name[0] == '\0') std::memset(bad.game_name, 0, sizeof(bad.game_name));
        CHECK_EQ(s.store.save(s.guest.memory, bad, sd::SavedataStore::save_name(bad)),
                 static_cast<int>(sd::error::kSavedataSaveParam));
        s.store.set_param(s.guest.memory, &bad);
        CHECK(!s.store.remove(bad, 0));
    }
    auto bad_file = mpo_save_request(sd::SavedataMode::kAutoSave, 1, 9);
    std::snprintf(bad_file.file_name, sizeof(bad_file.file_name), "../x.bin");
    CHECK_EQ(s.store.save(s.guest.memory, bad_file, "001"),
             static_cast<int>(sd::error::kSavedataSaveParam));
    CHECK(tree_hashes(root.path()) == before);
    // Unsafe names in a saveNameList are not listed.
    auto list = mpo_request(sd::SavedataMode::kListSave, 0);
    sd::SaveName names[3] = {"../../X", "002", ""};
    std::memcpy(s.guest.at(kNameList), names, sizeof(names));
    list.save_name_list = kNameList;
    s.store.set_param(s.guest.memory, &list);
    CHECK_EQ(s.store.listed_count(), 1);
    CHECK_EQ(s.store.entries()[0].save_name, std::string("002"));
    CHECK(!sd::SavedataStore::is_safe_component("."));
    CHECK(!sd::SavedataStore::is_safe_component("ABC."));
    CHECK(sd::SavedataStore::is_safe_component("ULUS10202001"));

    // A symlinked "save" pointing outside SAVEDATA is not followed.
    std::error_code ec;
    fs::create_directory_symlink(root.path() / "OUTSIDE", savedata / "ULUS10202LINK", ec);
    if (!ec) {
        auto all = mpo_request(sd::SavedataMode::kListAllDelete, 0);
        s.store.set_param(s.guest.memory, &all);
        for (int i = 0; i < s.store.listed_count(); ++i) {
            if (s.store.entries()[static_cast<std::size_t>(i)].save_dir == "ULUS10202LINK") {
                CHECK(!s.store.remove(all, i));
            }
        }
        CHECK(fs::exists(root.path() / "OUTSIDE" / "KEEP.BIN"));
        fs::remove(savedata / "ULUS10202LINK", ec);
    } else {
        std::printf("note: no symlinks here (%s); symlink case skipped\n", ec.message().c_str());
    }

    // LISTALLDELETE never lists the loose file and deletes one directory.
    auto all = mpo_request(sd::SavedataMode::kListAllDelete, 0);
    std::memset(all.game_name, 0, sizeof(all.game_name));
    s.store.set_param(s.guest.memory, &all);
    CHECK_EQ(s.store.listed_count(), 2);
    CHECK(s.store.remove(all, 0));
    CHECK(!fs::exists(savedata / "ULUS10202001"));
    CHECK(fs::exists(savedata / "ULUS10202003" / "00000000.000"));
    CHECK(fs::exists(savedata / "LOOSEFILE.BIN"));
    CHECK(fs::exists(root.path() / "OUTSIDE" / "KEEP.BIN"));
    CHECK(tree_hashes(root.path()) == without_prefix(before, "PSP/SAVEDATA/ULUS10202001"));
    // The stale entry cannot delete twice.
    CHECK(!s.store.remove(all, 0));
}

// --- Invalid parameters and load errors ---------------------------------------

void test_invalid_params() {
    TempRoot root;
    Session s(root.path());
    const fs::path savedata = s.store.savedata_dir();

    // Only the V1, V2 and V3 sizes start.
    auto request = mpo_request(sd::SavedataMode::kAutoLoad, 1);
    request.common.size = 1000;
    s.guest.put(kRequest, request);
    CHECK_EQ(s.dialog.init(s.guest.memory, kRequest),
             static_cast<int>(sd::error::kInvalidParamSize));
    CHECK_EQ(static_cast<int>(s.dialog.status()), static_cast<int>(DialogStatus::kNone));

    // Load errors.
    auto r = s.run_invisible(mpo_request(sd::SavedataMode::kAutoLoad, 1));
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataLoadNoData);
    write_all(savedata / "ULUS10202001" / "PARAM.SFO", {});
    r = s.run_invisible(mpo_request(sd::SavedataMode::kAutoLoad, 1));
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataLoadFileNotFound);
    write_all(savedata / "ULUS10202001" / "00000000.000", pattern(64, 1));
    r = s.run_invisible(mpo_request(sd::SavedataMode::kAutoLoad, 1));
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataLoadDataBroken);
    CHECK_EQ(r.data_size, 64u); // the data loaded; only the SFO is broken
    auto small = mpo_request(sd::SavedataMode::kAutoLoad, 1);
    small.data_buf_size = 16;
    r = s.run_invisible(small);
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataLoadDataBroken);
    CHECK_EQ(r.data_size, 0u);
    auto version = mpo_request(sd::SavedataMode::kAutoLoad, 1);
    version.secure_version = 4;
    r = s.run_invisible(version);
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataLoadParam);
    // READDATA maps the errors to the RW codes.
    r = s.run_invisible(mpo_request(sd::SavedataMode::kReadData, 2));
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataRwNoData);

    // Save errors.
    auto overflow = mpo_save_request(sd::SavedataMode::kAutoSave, 2, 1);
    overflow.data_size = kDataBufSize + 1;
    r = s.run_invisible(overflow);
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataRwBadParams);
    auto icon = mpo_save_request(sd::SavedataMode::kAutoSave, 2, 1);
    icon.icon0_file_data.size = icon.icon0_file_data.buf_size + 1;
    r = s.run_invisible(icon);
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataRwBadParams);
    auto secure = mpo_save_request(sd::SavedataMode::kAutoSave, 2, 1);
    secure.secure_version = 2;
    std::memset(secure.key, 0, sizeof(secure.key));
    r = s.run_invisible(secure);
    CHECK_EQ(static_cast<std::uint32_t>(r.common.result), sd::error::kSavedataSaveParam);
    CHECK(!fs::exists(savedata / "ULUS10202002"));

    // LISTLOAD with nothing to show: "There is no data", LOAD_NO_DATA.
    auto empty_list = mpo_request(sd::SavedataMode::kListLoad, 0);
    put_name_list(s.guest, empty_list, 3);
    fs::remove_all(savedata / "ULUS10202001");
    CHECK_EQ(s.start(empty_list), 0);
    CHECK(s.dialog.screen() == sd::SaveScreen::kLoadNoData);
    CHECK_EQ(s.dialog.view().message, std::string("There is no data"));
    s.update(press(btn::kCross));
    s.close();
    CHECK_EQ(static_cast<std::uint32_t>(s.result().common.result), sd::error::kSavedataLoadNoData);

    // A mode this port does not implement is reported, not silent.
    auto sizes = mpo_request(sd::SavedataMode::kSizes, 1);
    CHECK_EQ(s.start(sizes), 0);
    CHECK(s.dialog.unsupported_mode().has_value());
    CHECK_EQ(s.dialog.unsupported_mode().value_or(0), 8u);
    s.update();
    s.close();
}

// --- Encryption behind the interface -------------------------------------------

// Not the PSP's algorithm: a reversible stand-in that records its calls.
class FakeCipher final : public sd::SavedataCipher {
public:
    int encrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                std::array<std::uint8_t, 16> &hash) override {
        ++encrypts;
        last_mode = mode;
        std::vector<std::uint8_t> out(16, 0);
        out[0] = static_cast<std::uint8_t>(mode);
        for (std::size_t i = 0; i < data.size(); ++i) {
            out.push_back(static_cast<std::uint8_t>(data[i] ^ stream(mode, key, i)));
        }
        hash = digest(data);
        data = std::move(out);
        return 0;
    }
    int decrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                const std::uint8_t *expected_hash) override {
        ++decrypts;
        if (data.size() < 16 || data[0] != mode) return -1;
        std::vector<std::uint8_t> out;
        for (std::size_t i = 16; i < data.size(); ++i) {
            out.push_back(static_cast<std::uint8_t>(data[i] ^ stream(mode, key, i - 16)));
        }
        if (expected_hash != nullptr) {
            const auto hash = digest(out);
            if (std::memcmp(hash.data(), expected_hash, 16) != 0) return -8;
        }
        data = std::move(out);
        return 0;
    }
    int update_sfo_hash(std::vector<std::uint8_t> &sfo, std::size_t offset, int mode) override {
        ++sfo_hashes;
        if (offset + 128 > sfo.size() || sfo.size() % 16 != 0) return -1;
        // The flag byte as PPSSPP's UpdateHash sets it.
        sfo[offset] = static_cast<std::uint8_t>(0x01 | ((mode & 6) << 4));
        return 0;
    }

    int encrypts = 0;
    int decrypts = 0;
    int sfo_hashes = 0;
    int last_mode = 0;

private:
    static std::uint8_t stream(int mode, const std::uint8_t *key, std::size_t i) {
        const std::uint8_t k = key != nullptr ? key[i % 16] : 0x5A;
        return static_cast<std::uint8_t>(k ^ (i * 31) ^ static_cast<std::size_t>(mode));
    }
    static std::array<std::uint8_t, 16> digest(const std::vector<std::uint8_t> &data) {
        std::array<std::uint8_t, 16> out{};
        for (std::size_t i = 0; i < data.size(); ++i) {
            out[i % 16] = static_cast<std::uint8_t>(out[i % 16] * 33 + data[i]);
        }
        return out;
    }
};

void test_cipher() {
    TempRoot root;
    const fs::path slot = root.path() / "PSP" / "SAVEDATA" / "ULUS10202001";

    // MPO's request picks mode 5 (key, secureVersion 0, SDK 0x02080010 >= 4).
    sd::SavedataStore modes(root.path(), nullptr, sd::StoreConfig{kMpoSdk, true, 1u << 20});
    auto request = mpo_request(sd::SavedataMode::kAutoSave, 1);
    CHECK_EQ(modes.determine_crypt_mode(request), 5);
    sd::SavedataStore old_sdk(root.path(), nullptr, sd::StoreConfig{0x02000000u, true, 1u << 20});
    CHECK_EQ(old_sdk.determine_crypt_mode(request), 3);
    std::memset(request.key, 0, sizeof(request.key));
    CHECK_EQ(modes.determine_crypt_mode(request), 1);
    request.secure_version = 2;
    CHECK_EQ(modes.determine_crypt_mode(request), 3);

    FakeCipher cipher;
    const auto data = pattern(kMpoDataSize, 11);
    {
        Session s(root.path(), &cipher);
        s.guest.memory.write_bytes(kDataBuf, data);
        const auto saved = s.run_invisible(mpo_save_request(sd::SavedataMode::kAutoSave, 1, 1));
        CHECK_EQ(saved.common.result, 0);
        CHECK_EQ(cipher.encrypts, 1);
        CHECK_EQ(cipher.last_mode, 5);
        CHECK_EQ(cipher.sfo_hashes, 1);
        const auto file = read_all(slot / "00000000.000");
        CHECK_EQ(file.size(), data.size() + 16);
        CHECK(file != data);
        CHECK_EQ(s.store.save_crypt_mode(saved, "001"), 5); // flag 0x41
        // Load decrypts back, checking the hash in SAVEDATA_FILE_LIST.
        s.guest.memory.fill(kDataBuf, 0, kDataBufSize);
        const auto loaded = s.run_invisible(mpo_request(sd::SavedataMode::kAutoLoad, 1));
        CHECK_EQ(loaded.common.result, 0);
        CHECK_EQ(loaded.data_size, kMpoDataSize);
        CHECK(std::memcmp(s.guest.at(kDataBuf), data.data(), data.size()) == 0);
        CHECK(cipher.decrypts >= 1);
        // A tampered file fails the hash and then decrypts without it, as
        // PPSSPP does ("likely corrupt").
        auto tampered = file;
        tampered[100] ^= 0xFF;
        write_all(slot / "00000000.000", tampered);
        const auto retry = s.run_invisible(mpo_request(sd::SavedataMode::kAutoLoad, 1));
        CHECK_EQ(retry.common.result, 0);
        write_all(slot / "00000000.000", file);
    }
    {
        // Without a cipher an encrypted save fails visibly.
        Session s(root.path());
        const auto loaded = s.run_invisible(mpo_request(sd::SavedataMode::kAutoLoad, 1));
        CHECK_EQ(static_cast<std::uint32_t>(loaded.common.result),
                 sd::error::kSavedataLoadDataBroken);
        CHECK(s.store.needs_cipher());
        CHECK_EQ(loaded.data_size, 0u);
        // Saving without a cipher writes a plain save (crypt mode 0 again).
        s.guest.memory.write_bytes(kDataBuf, data);
        const auto saved = s.run_invisible(mpo_save_request(sd::SavedataMode::kAutoSave, 1, 1));
        CHECK_EQ(saved.common.result, 0);
        CHECK(read_all(slot / "00000000.000") == data);
        CHECK_EQ(s.store.save_crypt_mode(saved, "001"), 0);
    }
}

// --- Default folders ---------------------------------------------------------

void test_default_paths() {
    sd::PathEnvironment env;
    CHECK(!sd::default_ms0_root(env, sd::HostOs::kPosix).has_value());
    CHECK(!sd::default_ms0_root(env, sd::HostOs::kWindows).has_value());

    env.home = fs::path("/home/user");
    CHECK(sd::default_ms0_root(env, sd::HostOs::kPosix) ==
          fs::path("/home/user/.local/share/mpo-pc-port/ms0"));
    env.xdg_data_home = fs::path("relative/dir"); // invalid per XDG: ignored
    CHECK(sd::default_ms0_root(env, sd::HostOs::kPosix) ==
          fs::path("/home/user/.local/share/mpo-pc-port/ms0"));
    env.xdg_data_home = fs::path("/data");
    CHECK(sd::default_data_dir(env, sd::HostOs::kPosix) == fs::path("/data/mpo-pc-port"));
    // Windows uses only %APPDATA%.
    CHECK(!sd::default_ms0_root(env, sd::HostOs::kWindows).has_value());
    env.appdata = fs::path("Roaming");
    const auto windows = sd::default_ms0_root(env, sd::HostOs::kWindows);
    CHECK(windows.has_value() && windows->filename() == "ms0" &&
          windows->parent_path().filename() == "mpo-pc-port" &&
          windows->parent_path().parent_path() == fs::path("Roaming"));

    // The configuration (input.toml, F10b): XDG_CONFIG_HOME, ~/.config, or
    // the same %APPDATA% folder as the data on Windows.
    CHECK(sd::default_config_dir(env, sd::HostOs::kPosix) ==
          fs::path("/home/user/.config/mpo-pc-port"));
    env.xdg_config_home = fs::path("relative/conf"); // invalid per XDG: ignored
    CHECK(sd::default_config_dir(env, sd::HostOs::kPosix) ==
          fs::path("/home/user/.config/mpo-pc-port"));
    env.xdg_config_home = fs::path("/conf");
    CHECK(sd::default_config_dir(env, sd::HostOs::kPosix) == fs::path("/conf/mpo-pc-port"));
    CHECK(sd::default_config_dir(env, sd::HostOs::kWindows) ==
          sd::default_data_dir(env, sd::HostOs::kWindows));
    env.home.reset();
    env.xdg_config_home.reset();
    CHECK(!sd::default_config_dir(env, sd::HostOs::kPosix).has_value());
    // The process environment is only read, never used for files here.
    (void)sd::PathEnvironment::from_process();
}

} // namespace

int main() {
    test_sfo_round_trip();
    test_save_load_delete_cycle();
    test_delete_safety();
    test_invalid_params();
    test_cipher();
    test_default_paths();
    return savedata_test::finish("mpo_savedata_store_test");
}
