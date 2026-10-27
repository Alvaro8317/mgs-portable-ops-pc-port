// SPDX-License-Identifier: GPL-3.0-or-later
// Write layout ported from PPSSPP Core/ELF/ParamSFO.cpp @ v1.20.4
// (ParamSFOData::WriteSFO and GetDataOffset); see sfo_file.hpp.

#include "savedata/sfo_file.hpp"

#include <algorithm>
#include <cstring>

namespace mpo::savedata {
namespace {

constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kIndexEntrySize = 16;
constexpr std::uint32_t kMaxEntries = 256;
constexpr std::uint32_t kMaxValueLength = 0x10000;
constexpr std::uint32_t kVersion = 0x00000101u;

std::uint16_t le16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put16(std::uint8_t *p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value & 0xFFu);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put32(std::uint8_t *p, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
}

std::size_t key_table_size(const std::map<std::string, SfoFile::Value> &values) {
    std::size_t size = 0;
    for (const auto &entry : values) size += entry.first.size() + 1;
    return (size + 3) & ~std::size_t{3};
}

} // namespace

bool SfoFile::parse(std::span<const std::uint8_t> data, std::string &error) {
    values_.clear();
    if (data.size() < kHeaderSize || std::memcmp(data.data(), "\0PSF", 4) != 0) {
        error = "PARAM.SFO: bad magic";
        return false;
    }
    const std::uint32_t key_table = le32(&data[8]);
    const std::uint32_t data_table = le32(&data[12]);
    const std::uint32_t count = le32(&data[16]);
    if (count > kMaxEntries || kHeaderSize + std::size_t{count} * kIndexEntrySize > data.size() ||
        key_table > data.size() || data_table > data.size()) {
        error = "PARAM.SFO: header out of bounds";
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *index = &data[kHeaderSize + std::size_t{i} * kIndexEntrySize];
        const std::size_t key_offset = std::size_t{key_table} + le16(&index[0]);
        const std::uint16_t format = le16(&index[2]);
        const std::uint32_t length = le32(&index[4]);
        const std::uint32_t max_length = le32(&index[8]);
        const std::size_t value_offset = std::size_t{data_table} + le32(&index[12]);
        if (key_offset >= data.size() || value_offset > data.size() ||
            length > data.size() - value_offset || length > max_length ||
            max_length > kMaxValueLength) {
            error = "PARAM.SFO: entry " + std::to_string(i) + " out of bounds";
            return false;
        }
        const void *key_end = std::memchr(&data[key_offset], '\0', data.size() - key_offset);
        if (key_end == nullptr) {
            error = "PARAM.SFO: unterminated key";
            return false;
        }
        std::string key(reinterpret_cast<const char *>(&data[key_offset]),
                        static_cast<std::size_t>(static_cast<const std::uint8_t *>(key_end) -
                                                 &data[key_offset]));
        Value value;
        value.max_length = max_length;
        const std::uint8_t *bytes = data.data() + value_offset;
        switch (format) {
        case static_cast<std::uint16_t>(Format::kInteger):
            if (length != 4) {
                error = "PARAM.SFO: integer " + key + " has length " + std::to_string(length);
                return false;
            }
            value.format = Format::kInteger;
            value.integer = le32(bytes);
            break;
        case static_cast<std::uint16_t>(Format::kString): {
            value.format = Format::kString;
            const void *nul = std::memchr(bytes, '\0', length);
            const std::size_t text_length =
                nul != nullptr
                    ? static_cast<std::size_t>(static_cast<const std::uint8_t *>(nul) - bytes)
                    : length;
            value.bytes.assign(bytes, bytes + text_length);
            break;
        }
        case static_cast<std::uint16_t>(Format::kBinary):
            value.format = Format::kBinary;
            value.bytes.assign(bytes, bytes + length);
            break;
        default:
            error = "PARAM.SFO: unknown format " + std::to_string(format) + " for " + key;
            return false;
        }
        values_[key] = std::move(value);
    }
    return true;
}

std::vector<std::uint8_t> SfoFile::write() const {
    const auto count = static_cast<std::uint32_t>(values_.size());
    const std::size_t keys_size = key_table_size(values_);
    std::size_t data_size = 0;
    for (const auto &entry : values_) data_size += entry.second.max_length;
    const std::size_t key_table = kHeaderSize + std::size_t{count} * kIndexEntrySize;
    const std::size_t data_table = key_table + keys_size;
    std::vector<std::uint8_t> out(data_table + data_size, 0);

    std::memcpy(out.data(), "\0PSF", 4);
    put32(&out[4], kVersion);
    put32(&out[8], static_cast<std::uint32_t>(key_table));
    put32(&out[12], static_cast<std::uint32_t>(data_table));
    put32(&out[16], count);

    std::size_t key_offset = 0;
    std::size_t value_offset = 0;
    std::size_t index = kHeaderSize;
    for (const auto &[key, value] : values_) {
        std::uint8_t *entry = &out[index];
        put16(&entry[0], static_cast<std::uint32_t>(key_offset));
        put32(&entry[8], value.max_length);
        put32(&entry[12], static_cast<std::uint32_t>(value_offset));
        std::uint8_t *data = &out[data_table + value_offset];
        switch (value.format) {
        case Format::kInteger:
            put16(&entry[2], static_cast<std::uint32_t>(Format::kInteger));
            put32(&entry[4], 4);
            if (value.max_length >= 4) put32(data, value.integer);
            break;
        case Format::kBinary: {
            // A value never spills into the next one's data.
            const std::size_t length = std::min<std::size_t>(value.bytes.size(), value.max_length);
            put16(&entry[2], static_cast<std::uint32_t>(Format::kBinary));
            put32(&entry[4], static_cast<std::uint32_t>(length));
            if (length > 0) std::memcpy(data, value.bytes.data(), length);
            break;
        }
        case Format::kString: {
            const std::size_t length =
                value.max_length > 0
                    ? std::min<std::size_t>(value.bytes.size(), value.max_length - 1)
                    : 0;
            put16(&entry[2], static_cast<std::uint32_t>(Format::kString));
            put32(&entry[4], value.max_length > 0 ? static_cast<std::uint32_t>(length + 1) : 0u);
            if (length > 0) std::memcpy(data, value.bytes.data(), length);
            break;
        }
        }
        std::memcpy(&out[key_table + key_offset], key.data(), key.size());
        key_offset += key.size() + 1;
        value_offset += value.max_length;
        index += kIndexEntrySize;
    }
    return out;
}

void SfoFile::set_string(const std::string &key, const std::string &value,
                         std::uint32_t max_length) {
    Value entry;
    entry.format = Format::kString;
    entry.max_length = std::max<std::uint32_t>(max_length, 1);
    const std::size_t length = std::min<std::size_t>(value.size(), entry.max_length - 1);
    entry.bytes.assign(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
    values_[key] = std::move(entry);
}

void SfoFile::set_integer(const std::string &key, std::uint32_t value, std::uint32_t max_length) {
    Value entry;
    entry.format = Format::kInteger;
    entry.integer = value;
    entry.max_length = std::max<std::uint32_t>(max_length, 4);
    values_[key] = std::move(entry);
}

void SfoFile::set_binary(const std::string &key, std::span<const std::uint8_t> value,
                         std::uint32_t max_length) {
    Value entry;
    entry.format = Format::kBinary;
    entry.max_length = max_length;
    const std::size_t length = std::min<std::size_t>(value.size(), max_length);
    entry.bytes.assign(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
    values_[key] = std::move(entry);
}

std::optional<std::string> SfoFile::string(const std::string &key) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.format != Format::kString) return std::nullopt;
    return std::string(it->second.bytes.begin(), it->second.bytes.end());
}

std::optional<std::uint32_t> SfoFile::integer(const std::string &key) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.format != Format::kInteger) return std::nullopt;
    return it->second.integer;
}

const std::vector<std::uint8_t> *SfoFile::binary(const std::string &key) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.format != Format::kBinary) return nullptr;
    return &it->second.bytes;
}

std::optional<std::size_t> SfoFile::data_offset(const std::string &key) const {
    const std::size_t data_table =
        kHeaderSize + values_.size() * kIndexEntrySize + key_table_size(values_);
    std::size_t offset = 0;
    for (const auto &[name, value] : values_) {
        if (name == key) return data_table + offset;
        offset += value.max_length;
    }
    return std::nullopt;
}

} // namespace mpo::savedata
