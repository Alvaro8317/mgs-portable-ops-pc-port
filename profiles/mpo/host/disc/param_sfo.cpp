// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout (all little-endian): a 20-byte header {magic "\0PSF", version,
// key_table_offset, data_table_offset, entry_count} followed by 16-byte index
// entries {key_offset u16, format u16, length u32, max_length u32,
// data_offset u32}. Formats: 0x0004 UTF-8 special, 0x0204 UTF-8 (NUL
// terminated), 0x0404 u32. Reference: psdevwiki "PARAM.SFO".

#include "disc/param_sfo.hpp"

#include <cstring>

namespace mpo::disc {
namespace {

constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kIndexEntrySize = 16;
constexpr std::uint32_t kMaxEntries = 256;
constexpr std::uint16_t kFormatUtf8Special = 0x0004;
constexpr std::uint16_t kFormatUtf8 = 0x0204;
constexpr std::uint16_t kFormatInteger = 0x0404;

std::uint16_t le16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

bool ParamSfo::parse(std::span<const std::uint8_t> data, std::string &error) {
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
        const std::size_t value_offset = std::size_t{data_table} + le32(&index[12]);
        if (key_offset >= data.size() || value_offset > data.size() ||
            length > data.size() - value_offset) {
            error = "PARAM.SFO: entry " + std::to_string(i) + " out of bounds";
            return false;
        }
        const void *key_end = std::memchr(&data[key_offset], '\0', data.size() - key_offset);
        if (key_end == nullptr) {
            error = "PARAM.SFO: unterminated key";
            return false;
        }
        const auto key_length = static_cast<std::size_t>(
            static_cast<const std::uint8_t *>(key_end) - &data[key_offset]);
        std::string key(reinterpret_cast<const char *>(&data[key_offset]), key_length);

        if (format == kFormatInteger) {
            if (length != 4) {
                error = "PARAM.SFO: integer " + key + " has length " + std::to_string(length);
                return false;
            }
            values_[key] = le32(&data[value_offset]);
        } else if (format == kFormatUtf8 || format == kFormatUtf8Special) {
            std::string value(reinterpret_cast<const char *>(&data[value_offset]), length);
            if (const auto nul = value.find('\0'); nul != std::string::npos) value.resize(nul);
            values_[key] = std::move(value);
        } else {
            error = "PARAM.SFO: unknown format " + std::to_string(format) + " for " + key;
            return false;
        }
    }
    return true;
}

std::optional<std::string> ParamSfo::string(const std::string &key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    if (const auto *value = std::get_if<std::string>(&it->second)) return *value;
    return std::nullopt;
}

std::optional<std::uint32_t> ParamSfo::integer(const std::string &key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    if (const auto *value = std::get_if<std::uint32_t>(&it->second)) return *value;
    return std::nullopt;
}

} // namespace mpo::disc
