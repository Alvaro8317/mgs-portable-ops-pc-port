// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only ISO9660 image reader for PSP UMD dumps (2048-byte sectors, plain
// ISO9660 directory records). The image is untrusted input: every extent,
// record length and directory is bounds-checked against the image size.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpo::disc {

inline constexpr std::uint32_t kIsoSectorSize = 2048;

struct IsoEntry {
    // Path relative to the image root, '/'-separated, without the ";1"
    // version suffix, e.g. "PSP_GAME/SYSDIR/EBOOT.BIN".
    std::string path;
    std::uint32_t lba = 0;
    std::uint32_t size = 0;
    bool directory = false;
};

struct IsoVolumeInfo {
    std::string system_id;
    std::string volume_id;
    // "YYYYMMDDHHMMSScc" as stored in the primary volume descriptor.
    std::string creation_date;
    std::uint32_t volume_sectors = 0;
};

class IsoImage {
public:
    // Opens and indexes the image. Returns false and fills `error` on failure.
    bool open(const std::filesystem::path &path, std::string &error);

    [[nodiscard]] const IsoVolumeInfo &volume() const noexcept { return volume_; }
    // Every file and directory below the root, sorted by path.
    [[nodiscard]] const std::vector<IsoEntry> &entries() const noexcept { return entries_; }
    [[nodiscard]] std::uint64_t image_size() const noexcept { return image_size_; }

    // Case-insensitive lookup (ISO9660 names are upper case; PSP paths are not
    // case sensitive). Returns nullptr when absent.
    [[nodiscard]] const IsoEntry *find(std::string_view path) const;

    // Reads `out.size()` bytes of a file starting at `offset`.
    bool read(const IsoEntry &entry, std::uint64_t offset, std::span<std::uint8_t> out,
              std::string &error);
    // Reads a whole file; refuses files larger than `max_size`.
    bool read_all(const IsoEntry &entry, std::uint32_t max_size, std::vector<std::uint8_t> &out,
                  std::string &error);

private:
    bool read_sectors(std::uint32_t lba, std::uint32_t count, std::vector<std::uint8_t> &out,
                      std::string &error);
    bool read_directory(std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth,
                        std::vector<std::uint32_t> &visited, std::string &error);

    std::ifstream file_;
    std::uint64_t image_size_ = 0;
    IsoVolumeInfo volume_;
    std::vector<IsoEntry> entries_;
};

} // namespace mpo::disc
