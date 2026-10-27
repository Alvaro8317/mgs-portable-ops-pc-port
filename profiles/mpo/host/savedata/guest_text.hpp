// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strings in guest structures (F18a): fixed-size char fields and NUL-ended
// UCS-2 strings in guest memory. Every read is bounded; invalid guest ranges
// read as empty.

#pragma once

#include "kernel/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace mpo::savedata {

// The string in a fixed-size field: up to the first NUL or `size` bytes.
inline std::string fixed_string(const char *field, std::size_t size) {
    const void *nul = std::memchr(field, '\0', size);
    const std::size_t length =
        nul != nullptr ? static_cast<std::size_t>(static_cast<const char *>(nul) - field) : size;
    return std::string(field, length);
}

// Copies `value` into a fixed-size field, zero-filling the rest; truncates so
// that a NUL always fits when `terminate` is set.
inline void set_fixed_string(char *field, std::size_t size, const std::string &value,
                             bool terminate = true) {
    std::memset(field, 0, size);
    const std::size_t room = terminate && size > 0 ? size - 1 : size;
    std::memcpy(field, value.data(), value.size() < room ? value.size() : room);
}

// Reads a NUL-terminated UCS-2 string at `address` (at most `max_units`
// units; PPSSPP stops at 2047).
inline std::u16string read_guest_u16string(kernel::GuestMemory &memory, std::uint32_t address,
                                           std::size_t max_units = 2047) {
    std::u16string out;
    if (address == 0) return out;
    for (std::size_t i = 0; i < max_units; ++i) {
        const std::uint8_t *unit = memory.translate(address + static_cast<std::uint32_t>(2 * i), 2);
        if (unit == nullptr) break;
        const auto value = static_cast<char16_t>(unit[0] | (unit[1] << 8));
        if (value == 0) break;
        out.push_back(value);
    }
    return out;
}

// UCS-2 to UTF-8 (no surrogate pairing, like PPSSPP's ConvertUCS2ToUTF8).
inline std::string ucs2_to_utf8(const std::u16string &text) {
    std::string out;
    for (const char16_t unit : text) {
        const auto c = static_cast<std::uint32_t>(unit);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

} // namespace mpo::savedata
