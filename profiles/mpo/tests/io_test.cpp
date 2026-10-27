// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_io (F09) on a synthetic ISO and a temporary directory: the disc0: and
// umd0: views, sce_lbn ranges, the memory stick directory, the device table
// and the date helpers. No game data.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "disc/iso9660.hpp"
#include "io/directory_file_system.hpp"
#include "io/iso_file_system.hpp"
#include "io/meta_file_system.hpp"
#include "synthetic_disc.hpp"

namespace {

namespace fs = std::filesystem;
namespace io = mpo::io;

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const auto check_a = (a);                                                                  \
        const auto check_b = (b);                                                                  \
        if (!(check_a == check_b)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", __FILE__,       \
                         __LINE__, #a, #b, static_cast<long long>(check_a),                        \
                         static_cast<long long>(check_b));                                         \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

constexpr std::uint32_t kAll = io::access::kRead;

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>(seed + i * 7u);
    return out;
}

struct Disc {
    fs::path path;
    mpo::test::SyntheticIso iso;
    mpo::disc::IsoImage image;
    io::HandleAllocator handles;
    std::unique_ptr<io::IsoFileSystem> files;

    Disc() {
        // Files are written in this order: zeta before alpha on the disc.
        iso = mpo::test::build_iso({
            {"PSP_GAME/USRDIR/zeta.bin", pattern(5000, 1)},
            {"PSP_GAME/USRDIR/alpha.bin", pattern(10, 2)},
            {"PSP_GAME/USRDIR/stage/st01/_zar", pattern(4100, 3)},
        });
        path = mpo::test::write_temp_file("mpo_io_test.iso", iso.bytes);
        std::string error;
        CHECK(image.open(path, error));
        files = std::make_unique<io::IsoFileSystem>(handles, image);
    }
    ~Disc() {
        image.close();
        std::error_code ec;
        fs::remove(path, ec);
    }
    [[nodiscard]] std::uint32_t lba_of(const std::string &file) const {
        const mpo::disc::IsoEntry *entry = image.find(file);
        return entry != nullptr ? entry->lba : 0u;
    }
};

void test_iso_files() {
    Disc disc;
    io::IsoFileSystem &iso = *disc.files;
    int us = 0;
    // Case sensitive, like PPSSPP's GetFromPath.
    CHECK(iso.open("/PSP_GAME/USRDIR/ZETA.BIN", kAll, "disc0:") < 0);
    CHECK_EQ(iso.open("/PSP_GAME/USRDIR/missing", kAll, "disc0:"),
             io::error_result(io::error::kFileNotFound));
    CHECK_EQ(iso.open("/PSP_GAME/USRDIR/zeta.bin", io::access::kWrite, "disc0:"),
             io::error_result(io::error::kInvalidFlag));
    const std::int64_t handle = iso.open("/PSP_GAME/USRDIR/zeta.bin", kAll, "disc0:");
    CHECK(handle > 0);
    const auto h = static_cast<std::uint32_t>(handle);
    CHECK_EQ(iso.dev_type(h), io::dev_type::kFile);
    std::vector<std::uint8_t> buffer(6000, 0xEE);
    CHECK_EQ(iso.read(h, std::span<std::uint8_t>(buffer.data(), 100), us), 100);
    CHECK(std::equal(buffer.begin(), buffer.begin() + 100, pattern(5000, 1).begin()));
    // Reads clamp at the end of the file.
    CHECK_EQ(iso.seek(h, -10, io::Move::kEnd), 4990u);
    CHECK_EQ(iso.read(h, buffer, us), 10);
    CHECK_EQ(iso.read(h, buffer, us), 0);
    CHECK_EQ(iso.seek(h, 2000, io::Move::kBegin), 2000u);
    CHECK_EQ(iso.seek(h, 48, io::Move::kCurrent), 2048u);
    CHECK_EQ(iso.read(h, std::span<std::uint8_t>(buffer.data(), 3000), us), 2952);
    CHECK(std::equal(buffer.begin(), buffer.begin() + 2952, pattern(5000, 1).begin() + 2048));
    const io::FileInfo info = iso.info("/PSP_GAME/USRDIR/zeta.bin");
    CHECK(info.exists && !info.directory);
    CHECK_EQ(info.size, 5000u);
    CHECK_EQ(info.access, 0555u);
    CHECK_EQ(info.start_sector, disc.lba_of("PSP_GAME/USRDIR/zeta.bin"));
    CHECK(iso.info("./PSP_GAME").directory);
    iso.close(h);
    CHECK(!iso.owns(h));

    // Listings keep the on-disc order (directories first in this image).
    bool exists = false;
    const std::vector<io::FileInfo> list = iso.list("/PSP_GAME/USRDIR", exists);
    CHECK(exists);
    CHECK_EQ(list.size(), 3u);
    if (list.size() == 3u) {
        CHECK(list[0].name == "stage" && list[0].directory);
        CHECK(list[1].name == "zeta.bin");
        CHECK(list[2].name == "alpha.bin");
    }
    (void)iso.list("/nothing", exists);
    CHECK(!exists);
}

void test_iso_lbn_and_block_device() {
    Disc disc;
    io::IsoFileSystem &iso = *disc.files;
    int us = 0;
    std::uint32_t start = 0;
    std::uint32_t size = 0;
    CHECK(io::parse_lbn("/sce_lbn0x10_size0x800", start, size));
    CHECK_EQ(start, 0x10u);
    CHECK_EQ(size, 0x800u);
    CHECK(io::parse_lbn("/sce_lbn10_size100", start, size));
    CHECK_EQ(start, 0x10u);
    CHECK_EQ(size, 0x100u);
    CHECK(!io::parse_lbn("/sce_lbn0x10", start, size));
    CHECK(!io::parse_lbn("/sce_lbn0x10_size0x800_______________", start, size));

    // A raw range: KCEJ_FS reads slot.dat like this.
    const std::uint32_t lba = disc.lba_of("PSP_GAME/USRDIR/stage/st01/_zar");
    char lbn[40];
    std::snprintf(lbn, sizeof(lbn), "/sce_lbn0x%x_size0x%x", lba, 4100u);
    const std::int64_t handle = iso.open(lbn, kAll, "disc0:");
    CHECK(handle > 0);
    const auto h = static_cast<std::uint32_t>(handle);
    CHECK_EQ(iso.dev_type(h), io::dev_type::kFile | io::dev_type::kEmuLbn);
    std::vector<std::uint8_t> buffer(8192, 0);
    CHECK_EQ(iso.read(h, buffer, us), 4100);
    CHECK(std::equal(buffer.begin(), buffer.begin() + 4100, pattern(4100, 3).begin()));
    CHECK_EQ(iso.info(lbn).size, 4100u);
    CHECK_EQ(iso.info(lbn).start_sector, lba);
    CHECK_EQ(iso.open("/sce_lbn0xFFFFFF_size0x10", kAll, "disc0:"),
             io::error_result(io::error::kFileNotFound));

    // umd0: is the whole disc in sectors, whatever the path.
    std::shared_ptr<io::FileSystem> block = iso.block_device();
    const std::int64_t whole = block->open("/anything", kAll, "umd0:");
    CHECK(whole > 0);
    const auto w = static_cast<std::uint32_t>(whole);
    CHECK_EQ(block->dev_type(w), io::dev_type::kBlock);
    CHECK_EQ(block->seek(w, static_cast<std::int32_t>(lba), io::Move::kBegin), lba);
    std::vector<std::uint8_t> sectors(2u * 2048u, 0);
    us = 0;
    CHECK_EQ(block->read(w, sectors, us), 2);
    CHECK_EQ(us, 0); // The raw read above left the head next to it.
    CHECK(std::equal(sectors.begin(), sectors.begin() + 4096, pattern(4100, 3).begin()));
    CHECK_EQ(block->position(w), lba + 2u);
    CHECK_EQ(block->info("").size, iso.sectors());
    std::vector<std::uint8_t> volume(2048, 0);
    CHECK(iso.read_sectors(16u, volume));
    CHECK(volume[1] == 'C' && volume[2] == 'D' && volume[5] == '1');
}

void test_directory_file_system() {
    const fs::path root = fs::temp_directory_path() / "mpo_io_test_ms0";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "PSP" / "SAVEDATA", ec);
    io::HandleAllocator handles;
    io::DirectoryFileSystem stick(handles, root, 3600);
    int us = 0;
    // The root never leaks.
    CHECK(stick.host_path("/../../etc/passwd").empty());
    CHECK_EQ(stick.open("/PSP/missing.bin", kAll, "ms0:"),
             io::error_result(io::error::kFileNotFound));
    const std::uint32_t create = io::access::kWrite | io::access::kCreate;
    std::int64_t handle = stick.open("/PSP/SAVEDATA/log.txt", create, "ms0:");
    CHECK(handle > 0);
    auto h = static_cast<std::uint32_t>(handle);
    const std::vector<std::uint8_t> text = mpo::test::bytes_of("hello, memory stick");
    CHECK_EQ(stick.write(h, text), static_cast<std::int64_t>(text.size()));
    stick.close(h);
    CHECK_EQ(stick.open("/PSP/SAVEDATA/log.txt", create | io::access::kExclusive, "ms0:"),
             io::error_result(io::error::kFileAlreadyExists));
    // FAT is case-insensitive.
    handle = stick.open("/psp/savedata/LOG.TXT", kAll, "ms0:");
    CHECK(handle > 0);
    h = static_cast<std::uint32_t>(handle);
    std::vector<std::uint8_t> buffer(64, 0);
    CHECK_EQ(stick.read(h, buffer, us), static_cast<std::int64_t>(text.size()));
    CHECK(std::equal(text.begin(), text.end(), buffer.begin()));
    CHECK_EQ(stick.info_by_handle(h).size, text.size());
    stick.close(h);
    // O_TRUNC truncates to what was written, at close (PPSSPP's PSP model).
    handle =
        stick.open("/PSP/SAVEDATA/log.txt", io::access::kWrite | io::access::kTruncate, "ms0:");
    h = static_cast<std::uint32_t>(handle);
    CHECK_EQ(stick.write(h, mpo::test::bytes_of("bye")), 3);
    stick.close(h);
    CHECK_EQ(fs::file_size(root / "PSP" / "SAVEDATA" / "log.txt"), 3u);
    // Append always writes at the end.
    handle = stick.open("/PSP/SAVEDATA/log.txt", io::access::kWrite | io::access::kAppend, "ms0:");
    h = static_cast<std::uint32_t>(handle);
    CHECK_EQ(stick.seek(h, 0, io::Move::kBegin), 0u);
    CHECK_EQ(stick.write(h, mpo::test::bytes_of("!!")), 2);
    stick.close(h);
    CHECK_EQ(fs::file_size(root / "PSP" / "SAVEDATA" / "log.txt"), 5u);

    // Rename keeps the file in its directory, whatever `to` says.
    CHECK_EQ(stick.rename("/PSP/SAVEDATA/log.txt", "/elsewhere/new.txt"), 0);
    CHECK(fs::exists(root / "PSP" / "SAVEDATA" / "new.txt"));
    CHECK_EQ(stick.rename("/PSP/SAVEDATA/new.txt", "new.txt"),
             io::error_result(io::error::kFileAlreadyExists));
    bool exists = false;
    const std::vector<io::FileInfo> list = stick.list("/PSP/SAVEDATA", exists);
    CHECK(exists);
    CHECK_EQ(list.size(), 1u);
    if (!list.empty()) {
        CHECK(list[0].name == "new.txt");
        CHECK_EQ(list[0].access, 0666u);
        CHECK(list[0].mtime.year >= 2024u);
    }
    CHECK_EQ(stick.make_directory("/PSP/GAME"), 0);
    CHECK_EQ(stick.make_directory("/PSP/GAME"), io::error_result(io::error::kFileAlreadyExists));
    CHECK(stick.info("/PSP/GAME").directory);
    CHECK_EQ(stick.remove("/PSP/SAVEDATA/new.txt"), 0);
    CHECK_EQ(stick.remove("/PSP/SAVEDATA/new.txt"), io::error_result(io::error::kFileNotFound));
    fs::remove_all(root, ec);
}

void test_meta_file_system() {
    std::string out;
    CHECK(io::real_path("disc0:/PSP_GAME/USRDIR", "slot.dat", out));
    CHECK(out == "disc0:/PSP_GAME/USRDIR/slot.dat");
    CHECK(io::real_path("", "disc0:/A/./B/../C\\D", out));
    CHECK(out == "disc0:/A/C/D");
    CHECK(io::real_path("", "disc0:/../../x", out));
    CHECK(out == "disc0:/x");
    CHECK(io::real_path("", "umd0:", out));
    CHECK(out == "umd0:");
    CHECK(io::real_path("", "ms0:/", out));
    CHECK(out == "ms0:/");
    CHECK(!io::real_path("", "relative", out));

    Disc disc;
    io::MetaFileSystem meta;
    io::IsoFileSystem iso(meta.handles(), disc.image);
    std::shared_ptr<io::FileSystem> block = iso.block_device();
    meta.mount("umd0:", block);
    meta.mount("disc0:", std::shared_ptr<io::FileSystem>(&iso, [](io::FileSystem *) {}));
    meta.set_starting_directory("disc0:/PSP_GAME/USRDIR");

    io::MetaFileSystem::Mapped mapped;
    CHECK_EQ(meta.map("  DISC0:/PSP_GAME/USRDIR/zeta.bin", mapped), 0u);
    CHECK(mapped.path == "/PSP_GAME/USRDIR/zeta.bin" && mapped.device == "disc0:");
    // No thread has a cwd: relative paths map, but report NOCWD.
    CHECK_EQ(meta.map("zeta.bin", mapped), io::error::kNoCwd);
    CHECK(mapped.path == "/PSP_GAME/USRDIR/zeta.bin");
    CHECK_EQ(meta.open("zeta.bin", kAll), io::error_result(io::error::kNoCwd));
    CHECK_EQ(meta.map("host0:/PSP_GAME", mapped), io::error::kNoCwd);
    CHECK_EQ(meta.map("flash0:/font/ltn0.pgf", mapped), io::error::kNoDevice);
    CHECK_EQ(meta.open("flash0:/font/ltn0.pgf", kAll), io::error_result(io::error::kNoDevice));
    CHECK(meta.flags_for("disc0:/x") == io::fs_flags::kUmd);

    const std::int64_t handle = meta.open("disc0:/PSP_GAME/USRDIR/alpha.bin", kAll);
    CHECK(handle > 0);
    // umd0: shares the handles of disc0: and forwards them (PPSSPP's
    // GetHandleOwner finds the block view first, too).
    CHECK(meta.owner(static_cast<std::uint32_t>(handle)) != nullptr);
    CHECK_EQ(meta.owner(static_cast<std::uint32_t>(handle))
                 ->dev_type(static_cast<std::uint32_t>(handle)),
             io::dev_type::kFile);
    const std::int64_t whole = meta.open("umd0:", kAll);
    CHECK(whole > 0 && whole != handle);
    CHECK(meta.owner(static_cast<std::uint32_t>(whole)) == block.get());
    CHECK_EQ(meta.info("umd0:").size, iso.sectors());
    CHECK_EQ(meta.info("disc0:/PSP_GAME/USRDIR/alpha.bin").size, 10u);
    meta.close(static_cast<std::uint32_t>(handle));
    meta.close(static_cast<std::uint32_t>(whole));
    CHECK(meta.owner(static_cast<std::uint32_t>(handle)) == nullptr);
    CHECK_EQ(meta.rename("disc0:/PSP_GAME/USRDIR/alpha.bin", "umd0:/x"),
             io::error_result(io::error::kCrossDevice));
}

void test_dates() {
    CHECK_EQ(io::days_from_civil(1970, 1, 1), 0);
    CHECK_EQ(io::days_from_civil(2000, 3, 1), 11017);
    std::int64_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;
    io::civil_from_days(20752, year, month, day); // 2026-10-26
    CHECK_EQ(year, 2026);
    CHECK_EQ(month, 10u);
    CHECK_EQ(day, 26u);
    const io::DateTime date = io::date_time_from_unix(951782400 + 3723, 42); // 2000-02-29 01:02:03
    CHECK_EQ(date.year, 2000u);
    CHECK_EQ(date.month, 2u);
    CHECK_EQ(date.day, 29u);
    CHECK_EQ(date.hour, 1u);
    CHECK_EQ(date.minute, 2u);
    CHECK_EQ(date.second, 3u);
    CHECK_EQ(date.microsecond, 42u);
    const io::DateTime before = io::date_time_from_unix(-1);
    CHECK_EQ(before.year, 1969u);
    CHECK_EQ(before.second, 59u);
}

} // namespace

int main() {
    test_iso_files();
    test_iso_lbn_and_block_device();
    test_directory_file_system();
    test_meta_file_system();
    test_dates();
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_io_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_io_test: ok\n");
    return 0;
}
