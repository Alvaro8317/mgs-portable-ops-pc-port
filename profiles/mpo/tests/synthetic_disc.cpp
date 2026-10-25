// SPDX-License-Identifier: GPL-3.0-or-later

#include "synthetic_disc.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

namespace mpo::test {
namespace {

constexpr std::uint32_t kSector = 2048;
constexpr std::uint32_t kFirstDirectoryLba = 18;

void store_both32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    store_le32(bytes, offset, value);
    for (int i = 0; i < 4; ++i) {
        bytes[offset + 4 + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
}

void store_both16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

std::size_t record_length(std::size_t name_length) {
    const std::size_t length = 33 + name_length;
    return length + (length % 2); // Padded to an even length.
}

// Writes one directory record at `offset`; returns its length.
std::size_t write_record(std::vector<std::uint8_t> &bytes, std::size_t offset,
                         const std::string &name, std::uint32_t lba, std::uint32_t size,
                         bool directory) {
    const std::size_t length = record_length(name.size());
    bytes[offset] = static_cast<std::uint8_t>(length);
    store_both32(bytes, offset + 2, lba);
    store_both32(bytes, offset + 10, size);
    bytes[offset + 25] = directory ? 0x02 : 0x00;
    store_both16(bytes, offset + 28, 1);
    bytes[offset + 32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(&bytes[offset + 33], name.data(), name.size());
    return length;
}

std::string parent_of(const std::string &path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string leaf_of(const std::string &path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

void store_le32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

SyntheticIso build_iso(const std::vector<SyntheticFile> &files) {
    // Collect directories ("" is the root) and assign one sector to each.
    std::set<std::string> directories = {""};
    for (const SyntheticFile &file : files) {
        for (std::string dir = parent_of(file.path); !dir.empty(); dir = parent_of(dir)) {
            directories.insert(dir);
        }
    }
    std::map<std::string, std::uint32_t> directory_lba;
    std::uint32_t next_lba = kFirstDirectoryLba;
    for (const std::string &dir : directories) directory_lba[dir] = next_lba++;
    std::map<std::string, std::uint32_t> file_lba;
    for (const SyntheticFile &file : files) {
        file_lba[file.path] = next_lba;
        next_lba += std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>((file.data.size() + kSector - 1) / kSector));
    }

    SyntheticIso iso;
    iso.root_lba = directory_lba[""];
    iso.bytes.assign(std::size_t{next_lba} * kSector, 0);
    auto &bytes = iso.bytes;

    // Primary volume descriptor and set terminator.
    const std::size_t pvd = 16 * kSector;
    bytes[pvd] = 1;
    std::memcpy(&bytes[pvd + 1], "CD001", 5);
    bytes[pvd + 6] = 1;
    std::memset(&bytes[pvd + 8], ' ', 64);
    std::memcpy(&bytes[pvd + 8], "PSP GAME", 8);
    std::memcpy(&bytes[pvd + 40], "SYNTHETIC", 9);
    store_both32(bytes, pvd + 80, next_lba);
    store_both16(bytes, pvd + 128, kSector);
    std::memcpy(&bytes[pvd + 813], "2006102918040200", 16);
    write_record(bytes, pvd + 156, std::string(1, '\0'), iso.root_lba, kSector, true);
    const std::size_t terminator = 17 * kSector;
    bytes[terminator] = 255;
    std::memcpy(&bytes[terminator + 1], "CD001", 5);

    for (const std::string &dir : directories) {
        const std::uint32_t lba = directory_lba[dir];
        std::size_t offset = std::size_t{lba} * kSector;
        const std::size_t end = offset + kSector;
        offset += write_record(bytes, offset, std::string(1, '\0'), lba, kSector, true);
        const std::uint32_t parent_lba = dir.empty() ? lba : directory_lba[parent_of(dir)];
        offset += write_record(bytes, offset, std::string(1, '\1'), parent_lba, kSector, true);
        for (const std::string &child : directories) {
            if (child.empty() || parent_of(child) != dir) continue;
            iso.record_offsets[child] = offset;
            offset +=
                write_record(bytes, offset, leaf_of(child), directory_lba[child], kSector, true);
        }
        for (const SyntheticFile &file : files) {
            if (parent_of(file.path) != dir) continue;
            iso.record_offsets[file.path] = offset;
            offset += write_record(bytes, offset, leaf_of(file.path) + ";1", file_lba[file.path],
                                   static_cast<std::uint32_t>(file.data.size()), false);
        }
        if (offset > end) throw std::logic_error("synthetic directory exceeds one sector");
    }
    for (const SyntheticFile &file : files) {
        std::copy(file.data.begin(), file.data.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(file_lba[file.path] * kSector));
    }
    return iso;
}

std::vector<std::uint8_t>
build_param_sfo(const std::vector<std::pair<std::string, std::string>> &strings,
                const std::vector<std::pair<std::string, std::uint32_t>> &integers) {
    const std::size_t count = strings.size() + integers.size();
    std::vector<std::uint8_t> keys;
    std::vector<std::uint8_t> values;
    std::vector<std::uint8_t> index;
    auto add_entry = [&](const std::string &key, std::uint16_t format,
                         const std::vector<std::uint8_t> &value) {
        std::vector<std::uint8_t> entry(16, 0);
        entry[0] = static_cast<std::uint8_t>(keys.size());
        entry[1] = static_cast<std::uint8_t>(keys.size() >> 8);
        entry[2] = static_cast<std::uint8_t>(format);
        entry[3] = static_cast<std::uint8_t>(format >> 8);
        store_le32(entry, 4, static_cast<std::uint32_t>(value.size()));
        store_le32(entry, 8, static_cast<std::uint32_t>(value.size()));
        store_le32(entry, 12, static_cast<std::uint32_t>(values.size()));
        index.insert(index.end(), entry.begin(), entry.end());
        keys.insert(keys.end(), key.begin(), key.end());
        keys.push_back(0);
        values.insert(values.end(), value.begin(), value.end());
    };
    for (const auto &[key, text] : strings) {
        std::vector<std::uint8_t> value(text.begin(), text.end());
        value.push_back(0);
        add_entry(key, 0x0204, value);
    }
    for (const auto &[key, number] : integers) {
        std::vector<std::uint8_t> value(4, 0);
        store_le32(value, 0, number);
        add_entry(key, 0x0404, value);
    }
    while (keys.size() % 4 != 0) keys.push_back(0);

    std::vector<std::uint8_t> sfo(20, 0);
    std::memcpy(sfo.data(), "\0PSF", 4);
    store_le32(sfo, 4, 0x0101);
    const std::size_t key_table = 20 + count * 16;
    store_le32(sfo, 8, static_cast<std::uint32_t>(key_table));
    store_le32(sfo, 12, static_cast<std::uint32_t>(key_table + keys.size()));
    store_le32(sfo, 16, static_cast<std::uint32_t>(count));
    sfo.insert(sfo.end(), index.begin(), index.end());
    sfo.insert(sfo.end(), keys.begin(), keys.end());
    sfo.insert(sfo.end(), values.begin(), values.end());
    return sfo;
}

std::vector<std::uint8_t> bytes_of(const std::string &text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::filesystem::path write_temp_file(const std::string &name,
                                      const std::vector<std::uint8_t> &bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
    return path;
}

} // namespace mpo::test
