// SPDX-License-Identifier: GPL-3.0-or-later
//
// The save converter of the installer (F18c), without game data:
//   - the savedata cipher on sceChnnlsv and libkirk (when the build has it,
//     MPO_TEST_KIRK): round trips in modes 1, 3 and 5, hash checks, the
//     PARAM.SFO hash flags, and known answers that pin the decryption and
//     UpdateHash across compilers;
//   - mpo::savedata::import_save/export_save, with a fake cipher and with the
//     real one, on synthetic saves in temporary directories (never the real
//     save folder);
//   - the savedata profile and the key read from a synthetic installed disc.
// The real save of the user is only used locally (docs/sessions/F18c.md).

#include "savedata_test_util.hpp"

#include "install/save_profile.hpp"
#include "savedata/save_transfer.hpp"
#include "savedata/savedata_store.hpp"
#include "savedata/sfo_file.hpp"
#include "util/sha256.hpp"
#if MPO_TEST_KIRK
#include "crypto/savedata_cipher_kirk.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#ifndef MPO_SAVEDATA_TOML
#define MPO_SAVEDATA_TOML "savedata.toml"
#endif

namespace {

using namespace savedata_test;
namespace sd = mpo::savedata;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kMpoDataSize = 0xA610u;
constexpr const char *kSlot = "ULUS10202001";
constexpr const char *kDataFile = "00000000.000";

std::array<std::uint8_t, 16> test_key() {
    std::array<std::uint8_t, 16> key{};
    const Bytes bytes = pattern(16, 3);
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::string hex(const std::uint8_t *data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

Bytes unhex(const std::string &text) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// A PARAM.SFO the way the runtime writes a plain MPO save.
sd::SfoFile plain_sfo(const std::string &dir_name) {
    sd::SfoFile sfo;
    sfo.set_string("CATEGORY", "MS", 4);
    sfo.set_integer("PARENTAL_LEVEL", 9, 4);
    sfo.set_string("SAVEDATA_DETAIL", "Player ID : 01\nNew Game\nNumber of Times Cleared : 0",
                   1024);
    sfo.set_string("SAVEDATA_DIRECTORY", dir_name, 64);
    Bytes list(99 * 32, 0);
    std::memcpy(list.data(), kDataFile, std::strlen(kDataFile));
    sfo.set_binary("SAVEDATA_FILE_LIST", list, static_cast<std::uint32_t>(list.size()));
    const Bytes params(128, 0);
    sfo.set_binary("SAVEDATA_PARAMS", params, 128);
    sfo.set_string("SAVEDATA_TITLE", "TESTER", 128);
    sfo.set_string("TITLE", "SYNTHETIC SAVE", 128);
    return sfo;
}

// <savedata>/<name>, plain, with its data and two icons.
fs::path make_plain_save(const fs::path &savedata, const std::string &name, std::uint32_t seed) {
    const fs::path dir = savedata / name;
    write_all(dir / "PARAM.SFO", plain_sfo(name).write());
    write_all(dir / kDataFile, pattern(kMpoDataSize, seed));
    write_all(dir / "ICON0.PNG", pattern(0x400, seed + 1));
    write_all(dir / "PIC1.PNG", pattern(0x300, seed + 2));
    return dir;
}

std::map<std::string, Bytes> files_of(const fs::path &dir) {
    std::map<std::string, Bytes> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto &entry : fs::directory_iterator(dir)) {
        out[entry.path().filename().string()] = read_all(entry.path());
    }
    return out;
}

std::size_t entries_in(const fs::path &dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    std::size_t count = 0;
    for ([[maybe_unused]] const auto &entry : fs::directory_iterator(dir)) ++count;
    return count;
}

Bytes sfo_binary(const fs::path &path, const char *key) {
    sd::SfoFile sfo;
    std::string error;
    if (!sfo.parse(read_all(path), error)) return {};
    const Bytes *value = sfo.binary(key);
    return value != nullptr ? *value : Bytes{};
}

// A reversible cipher with a keyed digest, like the store test's.
class FakeCipher final : public sd::SavedataCipher {
public:
    int encrypt(int mode, const std::uint8_t *key, Bytes &data,
                std::array<std::uint8_t, 16> &hash) override {
        Bytes out(16, 0);
        out[0] = static_cast<std::uint8_t>(mode);
        out[1] = static_cast<std::uint8_t>(++counter_); // a fresh "IV" each time
        for (std::size_t i = 0; i < data.size(); ++i) {
            out.push_back(static_cast<std::uint8_t>(data[i] ^ stream(key, i)));
        }
        hash = digest(data, key);
        data = std::move(out);
        return 0;
    }
    int decrypt(int mode, const std::uint8_t *key, Bytes &data,
                const std::uint8_t *expected_hash) override {
        if (data.size() <= 16 || data[0] != mode) return -1;
        Bytes out;
        for (std::size_t i = 16; i < data.size(); ++i) {
            out.push_back(static_cast<std::uint8_t>(data[i] ^ stream(key, i - 16)));
        }
        if (expected_hash != nullptr &&
            std::memcmp(digest(out, key).data(), expected_hash, 16) != 0) {
            return -8;
        }
        data = std::move(out);
        return 0;
    }
    int update_sfo_hash(Bytes &sfo, std::size_t offset, int mode) override {
        if (offset + 128 > sfo.size()) return -1;
        sfo[offset] = static_cast<std::uint8_t>(0x01 | ((mode & 6) << 4));
        return 0;
    }

private:
    static std::uint8_t stream(const std::uint8_t *key, std::size_t i) {
        const std::uint8_t k = key != nullptr ? key[i % 16] : 0x5A;
        return static_cast<std::uint8_t>(k ^ (i * 31));
    }
    static std::array<std::uint8_t, 16> digest(const Bytes &data, const std::uint8_t *key) {
        std::array<std::uint8_t, 16> out{};
        for (std::size_t i = 0; i < 16; ++i) out[i] = key != nullptr ? key[i] : 1;
        for (std::size_t i = 0; i < data.size(); ++i) {
            out[i % 16] = static_cast<std::uint8_t>(out[i % 16] * 33 + data[i]);
        }
        return out;
    }
    unsigned counter_ = 0;
};

sd::TransferProfile mpo_profile() {
    sd::TransferProfile profile;
    profile.game_name = "ULUS10202";
    profile.crypt_mode = 5;
    profile.key = test_key();
    return profile;
}

// Export -> import gives back the same files; the encrypted save looks like
// one of PPSSPP's.
void check_round_trip(sd::SavedataCipher &cipher, bool real_cipher) {
    TempRoot root;
    const fs::path ms0_savedata = root.path() / "ms0" / "PSP" / "SAVEDATA";
    const fs::path exported = root.path() / "psp" / "SAVEDATA";
    const fs::path reimported = root.path() / "ms0b" / "PSP" / "SAVEDATA";
    const fs::path plain = make_plain_save(ms0_savedata, kSlot, 21);
    fs::create_directories(exported);
    fs::create_directories(reimported);
    const auto profile = mpo_profile();
    const auto original = files_of(plain);
    std::string error;

    sd::TransferResult result;
    CHECK(sd::export_save(plain, exported, cipher, profile, {}, result, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(result.name, std::string(kSlot));
    CHECK(result.kind == sd::TransferKind::kConverted);
    CHECK_EQ(result.source_mode, 0);
    CHECK_EQ(result.result_mode, 5);
    CHECK(result.data == std::vector<std::string>{kDataFile});
    CHECK((result.copied == std::vector<std::string>{"ICON0.PNG", "PIC1.PNG"}));
    const fs::path encrypted = exported / kSlot;
    const auto written = files_of(encrypted);
    CHECK_EQ(written.size(), 4u);
    CHECK_EQ(entries_in(exported), 1u); // no temporary directory left
    CHECK_EQ(written.at(kDataFile).size(), kMpoDataSize + 16u);
    CHECK(written.at("ICON0.PNG") == original.at("ICON0.PNG"));
    CHECK(written.at("PIC1.PNG") == original.at("PIC1.PNG"));
    const Bytes params = sfo_binary(encrypted / "PARAM.SFO", "SAVEDATA_PARAMS");
    CHECK_EQ(params.size(), 128u);
    CHECK_EQ(params.empty() ? 0 : params[0], 0x41); // mode 5, as PPSSPP writes it
    const Bytes list = sfo_binary(encrypted / "PARAM.SFO", "SAVEDATA_FILE_LIST");
    CHECK(list.size() == 99 * 32 &&
          std::any_of(list.begin() + 13, list.begin() + 29, [](std::uint8_t b) { return b != 0; }));
    // The store sees the export as a mode 5 save.
    {
        sd::SfoFile sfo;
        CHECK(sfo.parse(read_all(encrypted / "PARAM.SFO"), error));
        CHECK_EQ(sd::SavedataStore::crypt_mode_of(sfo), 5);
    }
    // Everything but the hashes is the plain PARAM.SFO.
    {
        const Bytes a = original.at("PARAM.SFO");
        const Bytes b = written.at("PARAM.SFO");
        CHECK_EQ(a.size(), b.size());
        sd::SfoFile sfo;
        CHECK(sfo.parse(a, error));
        const std::size_t list_at = *sfo.data_offset("SAVEDATA_FILE_LIST");
        const std::size_t params_at = *sfo.data_offset("SAVEDATA_PARAMS");
        std::size_t outside = 0;
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            const bool hash = i >= list_at + 13 && i < list_at + 29;
            const bool param = i >= params_at && i < params_at + 128;
            if (a[i] != b[i] && !hash && !param) ++outside;
        }
        CHECK_EQ(outside, 0u);
    }

    CHECK(sd::import_save(encrypted, reimported, cipher, profile, {}, result, error));
    CHECK(result.kind == sd::TransferKind::kConverted);
    CHECK_EQ(result.source_mode, 5);
    CHECK_EQ(result.result_mode, 0);
    // Back to the same bytes, PARAM.SFO included.
    CHECK(files_of(reimported / kSlot) == original);

    // A second export makes a different file (new IV) that imports the same.
    {
        TempRoot other;
        CHECK(sd::export_save(plain, other.path(), cipher, profile, {}, result, error));
        CHECK(read_all(other.path() / kSlot / kDataFile) != written.at(kDataFile));
        TempRoot back;
        CHECK(
            sd::import_save(other.path() / kSlot, back.path(), cipher, profile, {}, result, error));
        CHECK(files_of(back.path() / kSlot) == original);
    }

    // The destination is never replaced without --overwrite.
    CHECK(!sd::export_save(plain, exported, cipher, profile, {}, result, error));
    CHECK(error.find("already exists") != std::string::npos);
    CHECK(files_of(encrypted) == written);
    sd::TransferOptions overwrite;
    overwrite.overwrite = true;
    CHECK(sd::export_save(plain, exported, cipher, profile, overwrite, result, error));
    CHECK(files_of(encrypted) != written); // a new IV
    CHECK_EQ(entries_in(exported), 1u);

    // Already in the wanted form: copied as is.
    TempRoot copies;
    CHECK(sd::import_save(plain, copies.path(), cipher, profile, {}, result, error));
    CHECK(result.kind == sd::TransferKind::kCopied);
    CHECK(files_of(copies.path() / kSlot) == original);
    TempRoot copies2;
    CHECK(sd::export_save(encrypted, copies2.path(), cipher, profile, {}, result, error));
    CHECK(result.kind == sd::TransferKind::kCopied);
    CHECK_EQ(result.result_mode, 5);
    CHECK(files_of(copies2.path() / kSlot) == files_of(encrypted));

    // The source and the destination cannot be the same directory.
    CHECK(!sd::import_save(plain, ms0_savedata, cipher, profile, overwrite, result, error));
    CHECK(files_of(plain) == original);

    // The wrong key fails the hash and writes nothing.
    auto wrong = profile;
    wrong.key[0] ^= 1;
    TempRoot nothing;
    CHECK(!sd::import_save(encrypted, nothing.path(), cipher, wrong, {}, result, error));
    CHECK(error.find("hash mismatch") != std::string::npos);
    CHECK_EQ(entries_in(nothing.path()), 0u);
    if (real_cipher) std::printf("  real cipher: export/import round trip done\n");
}

// Saves that are refused, with nothing written.
void test_rejections() {
    FakeCipher cipher;
    const auto profile = mpo_profile();
    TempRoot root;
    const fs::path savedata = root.path() / "SAVEDATA";
    const fs::path out = root.path() / "out";
    fs::create_directories(out);
    std::string error;
    sd::TransferResult result;
    auto refused = [&](const fs::path &dir, bool importing) {
        error.clear();
        const bool ok = importing ? sd::import_save(dir, out, cipher, profile, {}, result, error)
                                  : sd::export_save(dir, out, cipher, profile, {}, result, error);
        return !ok && !error.empty() && entries_in(out) == 0;
    };

    // Another game's directory, a bare game name and unsafe names.
    CHECK(refused(make_plain_save(savedata, "ULJM05193DATA00", 1), false));
    CHECK(refused(make_plain_save(savedata, "ULUS10202", 1), false));
    CHECK(refused(savedata / "ULUS10202001..", false));
    CHECK(refused(savedata / "ULUS10202404", false)); // missing

    // An encrypted save to break in several ways.
    const fs::path plain = make_plain_save(savedata, "ULUS10202002", 5);
    const fs::path staging = root.path() / "staging";
    CHECK(sd::export_save(plain, staging, cipher, profile, {}, result, error));
    const fs::path good = staging / "ULUS10202002";
    auto broken_copy = [&](const std::string &name) {
        const fs::path dir = root.path() / "broken" / name;
        fs::create_directories(dir.parent_path());
        fs::copy(good, dir, fs::copy_options::recursive);
        return dir;
    };
    CHECK(refused(good, true) == false); // sanity: the good one imports...
    fs::remove_all(out);
    fs::create_directories(out);

    { // ...a tampered data file does not,
        const fs::path dir = broken_copy("ULUS10202003");
        Bytes data = read_all(dir / kDataFile);
        data[100] ^= 0xFF;
        write_all(dir / kDataFile, data);
        CHECK(refused(dir, true));
        CHECK(error.find("hash mismatch") != std::string::npos);
    }
    { // nor one without its file hash,
        const fs::path dir = broken_copy("ULUS10202004");
        sd::SfoFile sfo;
        CHECK(sfo.parse(read_all(dir / "PARAM.SFO"), error));
        Bytes list = *sfo.binary("SAVEDATA_FILE_LIST");
        std::fill(list.begin() + 13, list.begin() + 29, std::uint8_t{0});
        sfo.set_binary("SAVEDATA_FILE_LIST", list, static_cast<std::uint32_t>(list.size()));
        write_all(dir / "PARAM.SFO", sfo.write());
        CHECK(refused(dir, true));
        CHECK(error.find("no file hash") != std::string::npos);
    }
    { // nor one whose file list names a missing file,
        const fs::path dir = broken_copy("ULUS10202005");
        fs::remove(dir / kDataFile);
        CHECK(refused(dir, true));
    }
    { // nor one whose file list names PARAM.SFO,
        const fs::path dir = broken_copy("ULUS10202006");
        sd::SfoFile sfo;
        CHECK(sfo.parse(read_all(dir / "PARAM.SFO"), error));
        Bytes list = *sfo.binary("SAVEDATA_FILE_LIST");
        std::memset(list.data(), 0, 13);
        std::memcpy(list.data(), "PARAM.SFO", 9);
        sfo.set_binary("SAVEDATA_FILE_LIST", list, static_cast<std::uint32_t>(list.size()));
        write_all(dir / "PARAM.SFO", sfo.write());
        CHECK(refused(dir, true));
    }
    { // nor one with a subdirectory,
        const fs::path dir = broken_copy("ULUS10202007");
        fs::create_directories(dir / "extra");
        CHECK(refused(dir, true));
    }
    { // nor one without PARAM.SFO, or with a broken one.
        const fs::path dir = broken_copy("ULUS10202008");
        fs::remove(dir / "PARAM.SFO");
        CHECK(refused(dir, true));
        write_all(dir / "PARAM.SFO", pattern(64, 9));
        CHECK(refused(dir, true));
    }
    { // A plain save whose file list is empty has nothing to encrypt.
        const fs::path dir = savedata / "ULUS10202009";
        sd::SfoFile sfo = plain_sfo("ULUS10202009");
        const Bytes list(99 * 32, 0);
        sfo.set_binary("SAVEDATA_FILE_LIST", list, static_cast<std::uint32_t>(list.size()));
        write_all(dir / "PARAM.SFO", sfo.write());
        write_all(dir / kDataFile, pattern(64, 1));
        CHECK(refused(dir, false));
    }
    { // An over-large file.
        auto small = profile;
        small.max_file_size = 1000;
        CHECK(!sd::export_save(plain, out, cipher, small, {}, result, error));
        CHECK(error.find("too large") != std::string::npos);
        CHECK_EQ(entries_in(out), 0u);
    }
}

void test_find_save_directories() {
    TempRoot root;
    const fs::path savedata = root.path() / "SAVEDATA";
    make_plain_save(savedata, "ULUS10202002", 1);
    make_plain_save(savedata, "ULUS10202001", 2);
    make_plain_save(savedata, "ULJM05193DATA00", 3);
    make_plain_save(savedata, "ULUS10202", 4);           // no save name
    write_all(savedata / "ULUS10202003", pattern(8, 1)); // a file
    const auto found = sd::find_save_directories(savedata, "ULUS10202");
    CHECK_EQ(found.size(), 2u);
    if (found.size() == 2) {
        CHECK_EQ(found[0].filename().string(), std::string("ULUS10202001"));
        CHECK_EQ(found[1].filename().string(), std::string("ULUS10202002"));
    }
    // A save directory itself.
    const auto one = sd::find_save_directories(savedata / "ULUS10202001", "ULUS10202");
    CHECK_EQ(one.size(), 1u);
    CHECK(sd::find_save_directories(root.path() / "missing", "ULUS10202").empty());
}

void test_save_profile() {
    namespace in = mpo::install;
    std::vector<in::SaveProfile> profiles;
    std::string error;
    // The profile in the repo parses.
    CHECK(in::load_save_profiles(MPO_SAVEDATA_TOML, profiles, error));
    CHECK_EQ(error, std::string());
    CHECK(!profiles.empty());
    if (!profiles.empty()) {
        CHECK_EQ(profiles[0].disc_id, std::string("ULUS10202"));
        CHECK_EQ(profiles[0].game_name, std::string("ULUS10202"));
        CHECK_EQ(profiles[0].crypt_mode, 5);
    }

    // A synthetic installed disc whose "stage" holds a key.
    TempRoot root;
    const fs::path game = root.path() / "game";
    const Bytes stage = pattern(0x4000, 5);
    const std::size_t offset = 0x1234;
    write_all(game / "disc" / "PSP_GAME" / "USRDIR" / "stage" / "t" / "t.prx", stage);
    const std::string manifest = "disc_id = \"TEST00001\"\ndisc_version = \"1.00\"\n";
    write_all(game / "install.toml", Bytes(manifest.begin(), manifest.end()));
    const std::string stage_sha = mpo::sha256_hex(stage);
    const std::string key_sha =
        mpo::sha256_hex(std::span<const std::uint8_t>(stage.data() + offset, 16));
    auto toml = [&](const std::string &file_sha, const std::string &k_sha, std::size_t at,
                    const std::string &extra = "") {
        return "[[version]]\ndisc_id = \"TEST00001\"\ndisc_version = \"1.00\"\n"
               "game_name = \"TEST00001\"\ncrypt_mode = 5\n"
               "key_file = \"PSP_GAME/USRDIR/stage/t/t.prx\"\n"
               "key_file_sha256 = \"" +
               file_sha + "\"\nkey_offset = " + std::to_string(at) + "\nkey_sha256 = \"" + k_sha +
               "\"\n" + extra;
    };
    CHECK(in::parse_save_profiles(toml(stage_sha, key_sha, offset), profiles, error));
    in::SaveProfile profile;
    std::array<std::uint8_t, 16> key{};
    CHECK(in::read_save_key(game, profiles, profile, key, error));
    CHECK(std::memcmp(key.data(), stage.data() + offset, 16) == 0);
    CHECK_EQ(profile.game_name, std::string("TEST00001"));

    // A different file, a wrong offset and a key past the end are refused.
    const std::string zero_sha(64, '0');
    CHECK(in::parse_save_profiles(toml(zero_sha, key_sha, offset), profiles, error));
    CHECK(!in::read_save_key(game, profiles, profile, key, error));
    CHECK(error.find("does not match") != std::string::npos);
    CHECK(in::parse_save_profiles(toml(stage_sha, key_sha, offset + 1), profiles, error));
    CHECK(!in::read_save_key(game, profiles, profile, key, error));
    CHECK(error.find("key_sha256") != std::string::npos);
    CHECK(std::all_of(key.begin(), key.end(), [](std::uint8_t b) { return b == 0; }));
    CHECK(in::parse_save_profiles(toml(stage_sha, key_sha, stage.size() - 8), profiles, error));
    CHECK(!in::read_save_key(game, profiles, profile, key, error));
    // Another installed version, and no installation at all.
    CHECK(in::parse_save_profiles(toml(stage_sha, key_sha, offset), profiles, error));
    profiles[0].disc_version = "1.01";
    CHECK(!in::read_save_key(game, profiles, profile, key, error));
    CHECK(error.find("no savedata profile") != std::string::npos);
    CHECK(!in::read_save_key(root.path() / "none", profiles, profile, key, error));
    CHECK(error.find("mpo --install") != std::string::npos);

    // Malformed profiles.
    CHECK(!in::parse_save_profiles("", profiles, error));
    CHECK(!in::parse_save_profiles("[[version]]\ndisc_id = \"X\"\n", profiles, error));
    auto bad = [&](const std::string &from, const std::string &to) {
        std::string text = toml(stage_sha, key_sha, offset);
        text.replace(text.find(from), from.size(), to);
        return !in::parse_save_profiles(text, profiles, error);
    };
    CHECK(bad("crypt_mode = 5", "crypt_mode = 4"));
    CHECK(bad("PSP_GAME/USRDIR", "../USRDIR"));
    CHECK(bad("PSP_GAME/USRDIR", "/PSP_GAME/USRDIR"));
    CHECK(bad("PSP_GAME/USRDIR", "PSP_GAME\\\\USRDIR"));
    CHECK(bad("game_name = \"TEST00001\"", "game_name = \"test/1\""));
    CHECK(bad("key_offset = ", "key_offset = -"));
    CHECK(bad(key_sha, key_sha.substr(1)));
}

#if MPO_TEST_KIRK
// pattern(40, 9) encrypted in mode 5 with test_key(), and its file hash.
constexpr const char *kKatFile = "b1461edac159beae1829f76cf42bc0011183243768b8a03480f413b47f3eee7f"
                                 "325cfd607d6b1da85a315413da08b35eb7a87f0d381a5046";
constexpr const char *kKatFileHash = "ae32bef07d1af11f26db73bab0ef25e8";
// SAVEDATA_PARAMS after UpdateHash (mode 5) of plain_sfo("ULUS10202001").
constexpr const char *kKatSfoParams =
    "410000000000000000000000000000008b92dd8ebc904ae2f31d71728275fff8"
    "0101010101010101010101010101010100000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000f67de2ba405f15fd0a36f6cc1f37b20a";

void test_kirk_cipher() {
    mpo::crypto::KirkSavedataCipher cipher;
    const auto key = test_key();
    for (const int mode : {1, 3, 5}) {
        const std::uint8_t *k = mode > 1 ? key.data() : nullptr;
        for (const std::uint32_t size :
             {1u, 15u, 16u, 17u, 2047u, 2048u, 2049u, 4101u, kMpoDataSize}) {
            const Bytes plain = pattern(size, size + static_cast<std::uint32_t>(mode));
            Bytes data = plain;
            std::array<std::uint8_t, 16> hash{};
            CHECK_EQ(cipher.encrypt(mode, k, data, hash), 0);
            CHECK_EQ(data.size(), size + 16u);
            CHECK(std::any_of(hash.begin(), hash.end(), [](std::uint8_t b) { return b != 0; }));
            Bytes back = data;
            CHECK_EQ(cipher.decrypt(mode, k, back, hash.data()), 0);
            CHECK(back == plain);
            back = data;
            CHECK_EQ(cipher.decrypt(mode, k, back, nullptr), 0);
            CHECK(back == plain);
            // A changed byte, anywhere, fails the hash.
            for (const std::size_t at : {std::size_t{0}, std::size_t{16}, data.size() - 1}) {
                Bytes tampered = data;
                tampered[at] ^= 0x10;
                CHECK_EQ(cipher.decrypt(mode, k, tampered, hash.data()), -8);
            }
            if (mode > 1) {
                auto other = key;
                other[15] ^= 0x80;
                Bytes wrong = data;
                CHECK_EQ(cipher.decrypt(mode, other.data(), wrong, hash.data()), -8);
            }
            // Another mode does not decrypt it either.
            const int other_mode = mode == 5 ? 3 : 5;
            Bytes cross = data;
            CHECK(cipher.decrypt(other_mode, key.data(), cross, hash.data()) != 0);
        }
        // A fresh IV each time.
        Bytes a = pattern(64, 1);
        Bytes b = a;
        std::array<std::uint8_t, 16> ha{};
        std::array<std::uint8_t, 16> hb{};
        CHECK_EQ(cipher.encrypt(mode, k, a, ha), 0);
        CHECK_EQ(cipher.encrypt(mode, k, b, hb), 0);
        CHECK(a != b);
    }
    // Bad input.
    Bytes empty;
    std::array<std::uint8_t, 16> hash{};
    CHECK(cipher.encrypt(5, key.data(), empty, hash) != 0);
    Bytes small(16, 0);
    CHECK(cipher.decrypt(5, key.data(), small, nullptr) != 0);
    Bytes any = pattern(32, 1);
    CHECK(cipher.encrypt(0, nullptr, any, hash) != 0);
    // Mode 6 needs the console's fuse key, which libkirk does not have.
    CHECK(cipher.encrypt(6, key.data(), any, hash) != 0);

    // Known answers, generated once with this cipher (whose decryption and
    // UpdateHash match PPSSPP's on a real save, checked locally in F18c):
    // they pin the results on every compiler. On a mismatch the test prints
    // the values this build computes.
    {
        const Bytes plain = pattern(40, 9);
        const Bytes file = unhex(kKatFile);
        const Bytes file_hash = unhex(kKatFileHash);
        Bytes data = file;
        const int rc = cipher.decrypt(5, key.data(), data, file_hash.data());
        CHECK_EQ(rc, 0);
        CHECK(data == plain);
        if (rc != 0 || data != plain) {
            Bytes fresh = plain;
            std::array<std::uint8_t, 16> fresh_hash{};
            cipher.encrypt(5, key.data(), fresh, fresh_hash);
            std::printf("  new KAT file: %s\n  new KAT hash: %s\n",
                        hex(fresh.data(), fresh.size()).c_str(),
                        hex(fresh_hash.data(), fresh_hash.size()).c_str());
        }
    }
    {
        const sd::SfoFile kat_sfo = plain_sfo(kSlot);
        Bytes bytes = kat_sfo.write();
        const std::size_t params_at = *kat_sfo.data_offset("SAVEDATA_PARAMS");
        CHECK_EQ(cipher.update_sfo_hash(bytes, params_at, 5), 0);
        const std::string params = hex(bytes.data() + params_at, 128);
        CHECK_EQ(params, std::string(kKatSfoParams));
        if (params != kKatSfoParams) std::printf("  new KAT params: %s\n", params.c_str());
    }

    // UpdateHash: the flags, the 16 bytes of 0x01 where the fuse key would be
    // needed (as PPSSPP writes them), and the same result padded or not.
    const sd::SfoFile sfo = plain_sfo(kSlot);
    const Bytes base = sfo.write();
    const std::size_t at = *sfo.data_offset("SAVEDATA_PARAMS");
    for (const int mode : {1, 3, 5}) {
        Bytes bytes = base;
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                  bytes.begin() + static_cast<std::ptrdiff_t>(at) + 128,
                  std::uint8_t{0xEE}); // ignored
        CHECK_EQ(cipher.update_sfo_hash(bytes, at, mode), 0);
        CHECK_EQ(bytes.size(), base.size());
        const std::uint8_t *params = bytes.data() + at;
        CHECK_EQ(params[0], mode == 1 ? 0x01 : mode == 3 ? 0x21 : 0x41);
        CHECK(std::all_of(params + 1, params + 0x10, [](std::uint8_t b) { return b == 0; }));
        CHECK(std::any_of(params + 0x10, params + 0x20, [](std::uint8_t b) { return b != 0; }));
        CHECK(std::all_of(params + 0x20, params + 0x30, [](std::uint8_t b) { return b == 1; }));
        CHECK_EQ(std::any_of(params + 0x70, params + 0x80, [](std::uint8_t b) { return b != 0; }),
                 mode != 1);
        // Only SAVEDATA_PARAMS changes.
        CHECK(std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                         base.begin()));
        Bytes padded = base;
        padded.resize((padded.size() + 15) & ~std::size_t{15}, 0);
        CHECK_EQ(cipher.update_sfo_hash(padded, at, mode), 0);
        CHECK(std::equal(bytes.begin(), bytes.end(), padded.begin()));
        Bytes again = base;
        CHECK_EQ(cipher.update_sfo_hash(again, at, mode), 0);
        CHECK(again == bytes);
    }
    Bytes short_sfo = base;
    CHECK(cipher.update_sfo_hash(short_sfo, base.size() - 100, 5) != 0);
}
#endif

} // namespace

int main() {
#if MPO_TEST_KIRK
    test_kirk_cipher();
#endif
    {
        FakeCipher cipher;
        check_round_trip(cipher, false);
    }
#if MPO_TEST_KIRK
    {
        mpo::crypto::KirkSavedataCipher cipher;
        check_round_trip(cipher, true);
    }
#endif
    test_rejections();
    test_find_save_directories();
    test_save_profile();
    return savedata_test::finish("mpo_savedata_transfer_test");
}
