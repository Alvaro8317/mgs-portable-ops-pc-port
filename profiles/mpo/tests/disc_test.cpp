// SPDX-License-Identifier: GPL-3.0-or-later
//
// Disc reading, validation and installation tests on synthetic images. No game
// data is needed.

#include "disc/iso9660.hpp"
#include "disc/param_sfo.hpp"
#include "install/installer.hpp"
#include "install/supported_versions.hpp"
#include "synthetic_disc.hpp"
#include "util/sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mpo::test::SyntheticFile;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::vector<SyntheticFile> sample_disc(const std::string &disc_version = "1.02") {
    return {
        {"PSP_GAME/PARAM.SFO", mpo::test::build_param_sfo({{"DISC_ID", "ULUS10202"},
                                                           {"DISC_VERSION", disc_version},
                                                           {"TITLE", "SYNTHETIC"},
                                                           {"PSP_SYSTEM_VER", "2.81"}},
                                                          {{"PARENTAL_LEVEL", 5}})},
        {"PSP_GAME/SYSDIR/EBOOT.BIN", mpo::test::bytes_of("~PSP fake encrypted executable")},
        {"PSP_GAME/SYSDIR/BOOT.BIN", std::vector<std::uint8_t>(100, 0)},
        {"PSP_GAME/SYSDIR/UPDATE/DATA.BIN", std::vector<std::uint8_t>(5000, 0)},
        {"PSP_GAME/USRDIR/stage/n01/n01.prx", std::vector<std::uint8_t>(3000, 0x5A)},
        {"PSP_GAME/USRDIR/stage/n01/_zar", mpo::test::bytes_of("stage data")},
        {"UMD_DATA.BIN", mpo::test::bytes_of("ULUS-10202|0000000000000001|0001|G")},
    };
}

// Builds the supported table that matches sample_disc() byte for byte.
std::string supported_toml_for(const std::vector<SyntheticFile> &files) {
    std::ostringstream toml;
    toml << "[[version]]\ndisc_id = \"ULUS10202\"\ndisc_version = \"1.02\"\n"
         << "title = \"SYNTHETIC\"\n"
         << "ignored = [\"PSP_GAME/SYSDIR/UPDATE/\", \"PSP_GAME/SYSDIR/BOOT.BIN\"]\n"
         << "extract = [\"PSP_GAME/PARAM.SFO\", \"PSP_GAME/SYSDIR/EBOOT.BIN\", \"*.prx\"]\n"
         << "file = [\n";
    for (const SyntheticFile &file : files) {
        if (file.path.starts_with("PSP_GAME/SYSDIR/UPDATE/") ||
            file.path == "PSP_GAME/SYSDIR/BOOT.BIN") {
            continue;
        }
        toml << "  { path = \"" << file.path << "\", size = " << file.data.size() << ", sha256 = \""
             << mpo::sha256_hex(file.data) << "\" },\n";
    }
    toml << "]\n";
    return toml.str();
}

std::vector<mpo::install::SupportedVersion> supported_for(const std::vector<SyntheticFile> &files) {
    std::vector<mpo::install::SupportedVersion> versions;
    std::string error;
    const bool ok =
        mpo::install::parse_supported_versions(supported_toml_for(files), versions, error);
    expect(ok, "synthetic supported.toml did not parse: " + error);
    return versions;
}

// Validates a synthetic disc against `versions`; returns the result.
mpo::install::ValidationResult validate(const std::vector<std::uint8_t> &image,
                                        const std::vector<mpo::install::SupportedVersion> &versions,
                                        const std::string &name) {
    mpo::install::ValidationResult result;
    const fs::path path = mpo::test::write_temp_file(name, image);
    mpo::disc::IsoImage iso;
    std::string error;
    expect(iso.open(path, error), name + ": open failed: " + error);
    expect(mpo::install::validate_image(iso, versions, result, error),
           name + ": validate_image failed: " + error);
    fs::remove(path);
    return result;
}

bool has_problem(const mpo::install::ValidationResult &result, const std::string &text) {
    for (const std::string &problem : result.problems) {
        if (problem.find(text) != std::string::npos) return true;
    }
    return false;
}

void test_sha256_vectors() {
    expect(mpo::sha256_hex({}) ==
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "sha256 of empty input");
    expect(mpo::sha256_hex(mpo::test::bytes_of("abc")) ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "sha256 of abc");
    expect(mpo::sha256_hex(
               mpo::test::bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "sha256 of the two-block FIPS vector");
    // One million 'a' fed in uneven chunks exercises the buffering path.
    mpo::Sha256 hash;
    const std::vector<std::uint8_t> chunk(997, 'a');
    std::size_t remaining = 1'000'000;
    while (remaining > 0) {
        const std::size_t take = remaining < chunk.size() ? remaining : chunk.size();
        hash.update(std::span<const std::uint8_t>(chunk.data(), take));
        remaining -= take;
    }
    expect(hash.hex_digest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
           "sha256 of one million 'a'");
}

void test_param_sfo() {
    const auto data = mpo::test::build_param_sfo({{"DISC_ID", "ULUS10202"}}, {{"REGION", 32768}});
    mpo::disc::ParamSfo sfo;
    std::string error;
    expect(sfo.parse(data, error), "PARAM.SFO parse failed: " + error);
    expect(sfo.string("DISC_ID") == "ULUS10202", "PARAM.SFO string value");
    expect(sfo.integer("REGION") == 32768u, "PARAM.SFO integer value");
    expect(!sfo.string("REGION").has_value(), "PARAM.SFO type mismatch must be empty");

    auto bad_magic = data;
    bad_magic[1] = 'X';
    expect(!sfo.parse(bad_magic, error), "PARAM.SFO with bad magic must fail");
    for (std::size_t size = 0; size < data.size(); ++size) {
        // Every truncation must fail cleanly (sanitizers catch overreads).
        std::vector<std::uint8_t> truncated(data.begin(),
                                            data.begin() + static_cast<std::ptrdiff_t>(size));
        sfo.parse(truncated, error);
    }
    auto huge_count = data;
    mpo::test::store_le32(huge_count, 16, 0xFFFFFFFFu);
    expect(!sfo.parse(huge_count, error), "PARAM.SFO with a huge entry count must fail");
}

void test_iso_reader() {
    const auto files = sample_disc();
    const auto iso_image = mpo::test::build_iso(files);
    const fs::path path = mpo::test::write_temp_file("mpo_disc_test_reader.iso", iso_image.bytes);
    mpo::disc::IsoImage iso;
    std::string error;
    expect(iso.open(path, error), "synthetic ISO did not open: " + error);
    expect(iso.volume().volume_id == "SYNTHETIC", "volume id");
    expect(iso.volume().creation_date == "2006102918040200", "creation date");

    const mpo::disc::IsoEntry *eboot = iso.find("psp_game/sysdir/eboot.bin");
    expect(eboot != nullptr, "case-insensitive find of EBOOT.BIN");
    if (eboot != nullptr) {
        expect(eboot->path == "PSP_GAME/SYSDIR/EBOOT.BIN", "';1' version suffix is stripped");
        std::vector<std::uint8_t> data;
        expect(iso.read_all(*eboot, 1024, data, error), "read EBOOT.BIN: " + error);
        expect(data == files[1].data, "EBOOT.BIN contents round-trip");
        std::vector<std::uint8_t> past_end(4);
        expect(!iso.read(*eboot, eboot->size - 2, past_end, error), "read past end must fail");
        expect(!iso.read_all(*eboot, 4, data, error), "read_all must honour max_size");
    }
    const mpo::disc::IsoEntry *stage = iso.find("PSP_GAME/USRDIR/stage");
    expect(stage != nullptr && stage->directory, "nested directory is listed");
    std::size_t file_count = 0;
    for (const auto &entry : iso.entries()) file_count += entry.directory ? 0 : 1;
    expect(file_count == files.size(), "every file is listed");
    fs::remove(path);
}

void expect_open_fails(std::vector<std::uint8_t> bytes, const std::string &name) {
    const fs::path path = mpo::test::write_temp_file("mpo_disc_test_" + name + ".iso", bytes);
    mpo::disc::IsoImage iso;
    std::string error;
    expect(!iso.open(path, error), name + ": malformed ISO must be rejected");
    expect(!error.empty(), name + ": rejection must explain why");
    fs::remove(path);
}

void test_malformed_iso() {
    const auto good = mpo::test::build_iso(sample_disc());

    auto no_signature = good.bytes;
    no_signature[16 * 2048 + 1] = 'X';
    expect_open_fails(no_signature, "no_signature");

    auto out_of_bounds = good.bytes;
    mpo::test::store_le32(out_of_bounds, good.record_offsets.at("PSP_GAME/SYSDIR/EBOOT.BIN") + 2,
                          0x00FFFFFFu);
    expect_open_fails(out_of_bounds, "extent_out_of_bounds");

    auto loop = good.bytes; // A subdirectory pointing back at the root.
    mpo::test::store_le32(loop, good.record_offsets.at("PSP_GAME/USRDIR") + 2, good.root_lba);
    expect_open_fails(loop, "directory_loop");

    auto bad_length = good.bytes;
    bad_length[good.record_offsets.at("UMD_DATA.BIN")] = 10;
    expect_open_fails(bad_length, "short_record");

    auto unsafe_name = good.bytes;
    unsafe_name[good.record_offsets.at("UMD_DATA.BIN") + 33] = '/';
    expect_open_fails(unsafe_name, "unsafe_name");

    expect_open_fails(std::vector<std::uint8_t>(4096, 0), "too_small");
}

void test_supported_parsing() {
    std::vector<mpo::install::SupportedVersion> versions;
    std::string error;
    const std::string sha(64, 'a');
    const std::string ok = "[[version]]\ndisc_id = \"X\"\ndisc_version = \"1.00\"\n"
                           "file = [{ path = \"A\", size = 1, sha256 = \"" +
                           sha + "\" }]\n";
    expect(mpo::install::parse_supported_versions(ok, versions, error),
           "minimal supported.toml: " + error);

    const std::string cases[] = {
        "not toml = = =",
        "[[version]]\ndisc_id = \"X\"\nfile = [{ path = \"A\", size = 1, sha256 = \"" + sha +
            "\" }]\n",
        "[[version]]\ndisc_id = \"X\"\ndisc_version = \"1.00\"\nfile = []\n",
        "[[version]]\ndisc_id = \"X\"\ndisc_version = \"1.00\"\n"
        "file = [{ path = \"A\", size = 1, sha256 = \"ABC\" }]\n",
        "[[version]]\ndisc_id = \"X\"\ndisc_version = \"1.00\"\n"
        "file = [{ path = \"A\", size = -1, sha256 = \"" +
            sha + "\" }]\n",
        "[[version]]\ndisc_id = \"X\"\ndisc_version = \"1.00\"\n"
        "file = [{ path = \"A\", size = 1, sha256 = \"" +
            sha +
            "\" },"
            " { path = \"A\", size = 1, sha256 = \"" +
            sha + "\" }]\n",
    };
    for (const std::string &text : cases) {
        expect(!mpo::install::parse_supported_versions(text, versions, error),
               "invalid supported.toml must be rejected:\n" + text);
    }

    mpo::install::SupportedVersion version;
    version.ignored = {"PSP_GAME/SYSDIR/UPDATE/"};
    version.extract = {"*.prx", "PSP_GAME/PARAM.SFO"};
    expect(mpo::install::is_ignored(version, "PSP_GAME/SYSDIR/UPDATE/DATA.BIN"), "dir ignore");
    expect(!mpo::install::is_ignored(version, "PSP_GAME/SYSDIR/UPDATEX"), "dir ignore prefix");
    expect(mpo::install::is_extracted(version, "a/b/n01.PRX"), "suffix match ignores case");
    expect(!mpo::install::is_extracted(version, "a/PSP_GAME/PARAM.SFO"), "exact match only");
}

// The versioned table must stay loadable and keep the base version (§2).
void test_profile_supported_table() {
    std::vector<mpo::install::SupportedVersion> versions;
    std::string error;
    expect(mpo::install::load_supported_versions(MPO_SUPPORTED_TOML, versions, error),
           "profiles/mpo/config/supported.toml: " + error);
    bool found = false;
    for (const auto &version : versions) {
        if (version.disc_id != "ULUS10202" || version.disc_version != "1.02") continue;
        for (const auto &file : version.files) found |= file.path == "PSP_GAME/SYSDIR/EBOOT.BIN";
    }
    expect(found, "supported.toml lists ULUS10202 1.02 with its EBOOT.BIN hash");
}

void test_validation() {
    const auto files = sample_disc();
    const auto versions = supported_for(files);
    const auto good = mpo::test::build_iso(files);

    const auto accepted = validate(good.bytes, versions, "mpo_disc_test_good.iso");
    expect(accepted.accepted(), "exact image must be accepted");
    expect(accepted.identity.psp_system_version == "2.81", "PSP_SYSTEM_VER is read");

    auto altered = files;
    altered[4].data[1000] ^= 0xFF; // Same size, different stage PRX contents.
    auto result = validate(mpo::test::build_iso(altered).bytes, versions, "mpo_disc_test_alt.iso");
    expect(!result.accepted() && has_problem(result, "SHA-256 mismatch: PSP_GAME/USRDIR/stage"),
           "altered PRX must be rejected");

    auto resized = files;
    resized[1].data.push_back(0);
    result = validate(mpo::test::build_iso(resized).bytes, versions, "mpo_disc_test_size.iso");
    expect(!result.accepted() && has_problem(result, "size mismatch: PSP_GAME/SYSDIR/EBOOT.BIN"),
           "resized EBOOT.BIN must be rejected");

    auto extra = files;
    extra.push_back({"PSP_GAME/USRDIR/stage/n01/extra.prx", {1, 2, 3}});
    result = validate(mpo::test::build_iso(extra).bytes, versions, "mpo_disc_test_extra.iso");
    expect(!result.accepted() && has_problem(result, "unexpected file"),
           "extra file must be rejected");

    auto missing = files;
    missing.erase(missing.begin() + 5); // stage/n01/_zar
    result = validate(mpo::test::build_iso(missing).bytes, versions, "mpo_disc_test_miss.iso");
    expect(!result.accepted() &&
               has_problem(result, "missing file: PSP_GAME/USRDIR/stage/n01/_zar"),
           "missing file must be rejected");

    auto ignored_changes = files;
    ignored_changes[2].data.assign(100, 0x77); // BOOT.BIN
    ignored_changes[3].data.assign(10, 0x77);  // UPDATE/DATA.BIN, different size too
    result = validate(mpo::test::build_iso(ignored_changes).bytes, versions,
                      "mpo_disc_test_ignored.iso");
    expect(result.accepted(), "changes to ignored paths must not matter");

    result = validate(mpo::test::build_iso(sample_disc("1.01")).bytes, versions,
                      "mpo_disc_test_unsupported.iso");
    expect(result.version == nullptr && !result.accepted(), "unsupported version is rejected");
    expect(result.identity.disc_version == "1.01", "unsupported version is reported");
}

void test_install() {
    const auto files = sample_disc();
    const auto versions = supported_for(files);
    const fs::path iso_path =
        mpo::test::write_temp_file("mpo_disc_test_install.iso", mpo::test::build_iso(files).bytes);
    const fs::path game_dir = fs::temp_directory_path() / "mpo_disc_test_game";
    fs::remove_all(game_dir);

    mpo::disc::IsoImage iso;
    std::string error;
    mpo::install::ValidationResult result;
    expect(iso.open(iso_path, error), "install: open: " + error);
    expect(mpo::install::validate_image(iso, versions, result, error), "install: validate");
    expect(mpo::install::install_image(iso, iso_path, result, game_dir, error),
           "install_image failed: " + error);

    const fs::path eboot = game_dir / "disc/PSP_GAME/SYSDIR/EBOOT.BIN";
    std::ifstream in(eboot, std::ios::binary);
    const std::vector<std::uint8_t> copied((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
    expect(copied == files[1].data, "EBOOT.BIN is extracted byte for byte");
    expect(fs::exists(game_dir / "disc/PSP_GAME/USRDIR/stage/n01/n01.prx"), "PRX extracted");
    expect(fs::exists(game_dir / "disc/PSP_GAME/PARAM.SFO"), "PARAM.SFO extracted");
    expect(!fs::exists(game_dir / "disc/PSP_GAME/USRDIR/stage/n01/_zar"), "data not extracted");
    expect(!fs::exists(game_dir / "disc/PSP_GAME/SYSDIR/EBOOT.BIN.partial"), "no partial files");
    expect(fs::exists(game_dir / "install.toml"), "install.toml written");

    mpo::install::ValidationResult rejected = result;
    rejected.problems.push_back("forced");
    expect(!mpo::install::install_image(iso, iso_path, rejected, game_dir, error),
           "install_image must refuse a rejected validation");

    fs::remove_all(game_dir);
    fs::remove(iso_path);
}

} // namespace

int main() {
    test_sha256_vectors();
    test_param_sfo();
    test_iso_reader();
    test_malformed_iso();
    test_supported_parsing();
    test_profile_supported_table();
    test_validation();
    test_install();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("mpo_disc_test: all checks passed");
    return EXIT_SUCCESS;
}
