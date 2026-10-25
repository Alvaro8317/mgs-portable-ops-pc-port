// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout references: ECMA-119 (ISO9660) §8.4 (primary volume descriptor) and
// §9.1 (directory records). Multi-byte fields are stored both-endian; only the
// little-endian half is read.

#include "disc/iso9660.hpp"

#include <algorithm>
#include <cctype>

namespace mpo::disc {
namespace {

constexpr std::uint32_t kFirstDescriptorSector = 16;
constexpr std::uint32_t kMaxDescriptors = 64;
constexpr int kMaxDirectoryDepth = 32;
constexpr std::size_t kMaxEntries = 1u << 20;
constexpr std::uint8_t kFlagDirectory = 0x02;
constexpr std::uint8_t kFlagMultiExtent = 0x80;
constexpr std::size_t kDirectoryRecordHeader = 33;

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string trimmed(const std::uint8_t *p, std::size_t size) {
    std::string value(reinterpret_cast<const char *>(p), size);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) value.pop_back();
    return value;
}

std::string upper(std::string_view value) {
    std::string out(value);
    for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// Rejects names that could escape a destination directory when extracted.
bool is_safe_component(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    return std::none_of(name.begin(), name.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || c == '/' || c == '\\' || c == ':';
    });
}

} // namespace

bool IsoImage::open(const std::filesystem::path &path, std::string &error) {
    file_ = std::ifstream(path, std::ios::binary);
    entries_.clear();
    volume_ = {};
    if (!file_) {
        error = "cannot open " + path.string();
        return false;
    }
    file_.seekg(0, std::ios::end);
    const auto end = file_.tellg();
    if (end < 0) {
        error = "cannot determine the size of " + path.string();
        return false;
    }
    image_size_ = static_cast<std::uint64_t>(end);
    if (image_size_ < (kFirstDescriptorSector + 1) * std::uint64_t{kIsoSectorSize}) {
        error = "image is too small to be ISO9660";
        return false;
    }

    std::vector<std::uint8_t> sector;
    bool found_primary = false;
    for (std::uint32_t i = 0; i < kMaxDescriptors && !found_primary; ++i) {
        if (!read_sectors(kFirstDescriptorSector + i, 1, sector, error)) return false;
        if (std::string_view(reinterpret_cast<const char *>(&sector[1]), 5) != "CD001") {
            error = "missing ISO9660 volume descriptor signature";
            return false;
        }
        const std::uint8_t type = sector[0];
        if (type == 255) break; // Volume descriptor set terminator.
        if (type != 1) continue;
        found_primary = true;
        volume_.system_id = trimmed(&sector[8], 32);
        volume_.volume_id = trimmed(&sector[40], 32);
        volume_.volume_sectors = le32(&sector[80]);
        volume_.creation_date = trimmed(&sector[813], 16);
        const std::uint32_t logical_block_size = static_cast<std::uint32_t>(sector[128]) |
                                                 (static_cast<std::uint32_t>(sector[129]) << 8);
        if (logical_block_size != kIsoSectorSize) {
            error = "unsupported logical block size " + std::to_string(logical_block_size);
            return false;
        }
        const std::uint8_t *root = &sector[156];
        if (root[0] < kDirectoryRecordHeader || (root[25] & kFlagDirectory) == 0) {
            error = "malformed root directory record";
            return false;
        }
        std::vector<std::uint32_t> visited;
        if (!read_directory(le32(&root[2]), le32(&root[10]), "", 0, visited, error)) return false;
    }
    if (!found_primary) {
        error = "no primary volume descriptor";
        return false;
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const IsoEntry &a, const IsoEntry &b) { return a.path < b.path; });
    return true;
}

const IsoEntry *IsoImage::find(std::string_view path) const {
    const std::string wanted = upper(path);
    for (const IsoEntry &entry : entries_) {
        if (upper(entry.path) == wanted) return &entry;
    }
    return nullptr;
}

bool IsoImage::read_sectors(std::uint32_t lba, std::uint32_t count, std::vector<std::uint8_t> &out,
                            std::string &error) {
    const std::uint64_t offset = std::uint64_t{lba} * kIsoSectorSize;
    const std::uint64_t size = std::uint64_t{count} * kIsoSectorSize;
    if (offset > image_size_ || size > image_size_ - offset) {
        error = "sector range out of bounds (lba " + std::to_string(lba) + ", " +
                std::to_string(count) + " sectors)";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
    if (!file_) {
        error = "read error at lba " + std::to_string(lba);
        return false;
    }
    return true;
}

bool IsoImage::read_directory(std::uint32_t lba, std::uint32_t size, const std::string &prefix,
                              int depth, std::vector<std::uint32_t> &visited, std::string &error) {
    if (depth > kMaxDirectoryDepth) {
        error = "directory tree too deep at " + prefix;
        return false;
    }
    if (std::find(visited.begin(), visited.end(), lba) != visited.end()) {
        error = "directory loop at lba " + std::to_string(lba);
        return false;
    }
    visited.push_back(lba);

    const std::uint32_t sectors = (size + kIsoSectorSize - 1) / kIsoSectorSize;
    std::vector<std::uint8_t> data;
    if (!read_sectors(lba, sectors, data, error)) return false;

    std::size_t pos = 0;
    while (pos < size) {
        const std::size_t sector_end = (pos / kIsoSectorSize + 1) * kIsoSectorSize;
        const std::uint8_t length = data[pos];
        // A zero length pads to the next sector; records never span sectors.
        if (length == 0) {
            pos = sector_end;
            continue;
        }
        if (length < kDirectoryRecordHeader || pos + length > sector_end) {
            error = "malformed directory record in " + (prefix.empty() ? "/" : prefix);
            return false;
        }
        const std::uint8_t *record = &data[pos];
        const std::uint8_t name_length = record[32];
        if (kDirectoryRecordHeader + name_length > length) {
            error = "directory record name overflows in " + (prefix.empty() ? "/" : prefix);
            return false;
        }
        pos += length;

        std::string name(reinterpret_cast<const char *>(&record[33]), name_length);
        if (name_length == 1 && (name[0] == '\0' || name[0] == '\1')) continue; // "." and ".."
        if (const auto semicolon = name.find(';'); semicolon != std::string::npos) {
            name.resize(semicolon);
        }
        if (!name.empty() && name.back() == '.') name.pop_back(); // "NAME." with no extension.
        if (!is_safe_component(name)) {
            error = "unsafe file name in " + (prefix.empty() ? "/" : prefix);
            return false;
        }

        const std::uint8_t flags = record[25];
        if ((flags & kFlagMultiExtent) != 0) {
            error = "multi-extent files are not supported: " + prefix + name;
            return false;
        }
        IsoEntry entry;
        entry.path = prefix + name;
        entry.lba = le32(&record[2]);
        entry.size = le32(&record[10]);
        entry.directory = (flags & kFlagDirectory) != 0;
        const std::uint64_t extent_end =
            std::uint64_t{entry.lba} * kIsoSectorSize + std::uint64_t{entry.size};
        if (extent_end > image_size_) {
            error = "extent of " + entry.path + " is outside the image";
            return false;
        }
        if (entries_.size() >= kMaxEntries) {
            error = "too many directory entries";
            return false;
        }
        entries_.push_back(entry);
        if (entry.directory &&
            !read_directory(entry.lba, entry.size, entry.path + "/", depth + 1, visited, error)) {
            return false;
        }
    }
    return true;
}

bool IsoImage::read(const IsoEntry &entry, std::uint64_t offset, std::span<std::uint8_t> out,
                    std::string &error) {
    if (entry.directory) {
        error = entry.path + " is a directory";
        return false;
    }
    if (offset > entry.size || out.size() > entry.size - offset) {
        error = "read past the end of " + entry.path;
        return false;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(std::uint64_t{entry.lba} * kIsoSectorSize + offset));
    file_.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file_) {
        error = "read error in " + entry.path;
        return false;
    }
    return true;
}

bool IsoImage::read_all(const IsoEntry &entry, std::uint32_t max_size,
                        std::vector<std::uint8_t> &out, std::string &error) {
    if (entry.size > max_size) {
        error = entry.path + " is larger than " + std::to_string(max_size) + " bytes";
        return false;
    }
    out.resize(entry.size);
    return read(entry, 0, out, error);
}

} // namespace mpo::disc
