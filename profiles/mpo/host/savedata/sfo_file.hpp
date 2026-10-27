// SPDX-License-Identifier: GPL-3.0-or-later
//
// PARAM.SFO for savedata (F18a): parse and write, keeping each entry's
// format and maximum length, binary values included (SAVEDATA_FILE_LIST,
// SAVEDATA_PARAMS). The written layout is PPSSPP's (Core/ELF/ParamSFO.cpp
// @ v1.20.4, WriteSFO): header version 0x0101, entries sorted by key, keys
// padded to 4 bytes and every value taking its full max_length, so saves
// written here open in PPSSPP and the other way round.
//
// disc::ParamSfo (host/disc) is the read-only parser of the disc's SFO; it
// drops binary values and maximum lengths, which a savedata rewrite needs,
// so savedata keeps its own codec and the tests cross-check the two.
//
// The input is untrusted: every offset and length is checked.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mpo::savedata {

class SfoFile {
public:
    enum class Format : std::uint16_t {
        kBinary = 0x0004,  // "UTF-8 special": raw bytes
        kString = 0x0204,  // NUL-terminated UTF-8
        kInteger = 0x0404, // u32
    };

    struct Value {
        Format format = Format::kString;
        std::vector<std::uint8_t> bytes; // string without its NUL, or raw bytes
        std::uint32_t integer = 0;
        std::uint32_t max_length = 0;
    };

    bool parse(std::span<const std::uint8_t> data, std::string &error);
    [[nodiscard]] std::vector<std::uint8_t> write() const;

    // Strings are truncated so that the NUL fits in max_length.
    void set_string(const std::string &key, const std::string &value, std::uint32_t max_length);
    void set_integer(const std::string &key, std::uint32_t value, std::uint32_t max_length = 4);
    // Binary values are truncated to max_length.
    void set_binary(const std::string &key, std::span<const std::uint8_t> value,
                    std::uint32_t max_length);

    [[nodiscard]] std::optional<std::string> string(const std::string &key) const;
    [[nodiscard]] std::optional<std::uint32_t> integer(const std::string &key) const;
    [[nodiscard]] const std::vector<std::uint8_t> *binary(const std::string &key) const;
    [[nodiscard]] const std::map<std::string, Value> &values() const noexcept { return values_; }

    // Offset of a value's data in the bytes write() produces.
    [[nodiscard]] std::optional<std::size_t> data_offset(const std::string &key) const;

private:
    std::map<std::string, Value> values_;
};

} // namespace mpo::savedata
