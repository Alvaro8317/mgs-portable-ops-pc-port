// SPDX-License-Identifier: GPL-3.0-or-later

#include "crypto/psp_module.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if MPO_HAVE_PRX_DECRYPT
#include "Core/ELF/PrxDecrypter.h"
#endif

namespace mpo::crypto {
namespace {

constexpr std::uint32_t kPspMagic = 0x5053507E; // "~PSP"

std::uint16_t read_u16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

#if MPO_HAVE_PRX_DECRYPT
constexpr std::uint8_t kElfMagic[] = {0x7F, 'E', 'L', 'F'};

std::string hex32(std::uint32_t value) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}
#endif

} // namespace

bool is_psp_module(std::span<const std::uint8_t> data) {
    return data.size() >= kPspHeaderSize && read_u32(data, 0x00) == kPspMagic;
}

bool parse_psp_header(std::span<const std::uint8_t> data, PspHeader &header, std::string &error) {
    header = {};
    if (!is_psp_module(data)) {
        error = "not a ~PSP module";
        return false;
    }
    header.attribute = read_u16(data, 0x04);
    header.comp_attribute = read_u16(data, 0x06);
    header.module_version_minor = data[0x08];
    header.module_version_major = data[0x09];
    const auto *name = reinterpret_cast<const char *>(data.data() + 0x0A);
    header.module_name.assign(name, strnlen(name, 28));
    header.elf_size = read_u32(data, 0x28);
    header.psp_size = read_u32(data, 0x2C);
    header.devkit_version = read_u32(data, 0x78);
    header.decrypt_mode = read_u32(data, 0x7C);
    header.tag = read_u32(data, 0xD0);

    if (header.psp_size <= kPspHeaderSize || header.psp_size > data.size()) {
        error = "~PSP size " + std::to_string(header.psp_size) + " does not fit the " +
                std::to_string(data.size()) + "-byte file";
        return false;
    }
    if (header.elf_size == 0 || header.elf_size > kMaxElfSize) {
        error = "unreasonable decrypted ELF size " + std::to_string(header.elf_size);
        return false;
    }
    return true;
}

bool decryption_available() {
    return MPO_HAVE_PRX_DECRYPT != 0;
}

bool decrypt_psp_module(std::span<const std::uint8_t> data, DecryptedModule &out,
                        std::string &error) {
    out = {};
    if (!parse_psp_header(data, out.header, error)) return false;
    const PspHeader &header = out.header;
    if ((header.comp_attribute & 1) != 0) {
        // PPSSPP inflates these after decrypting; no supported module needs it.
        error = "gzip-compressed ~PSP modules are not supported (" + header.module_name + ")";
        return false;
    }
#if MPO_HAVE_PRX_DECRYPT
    // Same buffer and call as PPSSPP's __KernelLoadELFFromPtr, so the whole
    // buffer can be compared with its decrypted EBOOT dump.
    out.buffer.assign(std::max(header.elf_size, header.psp_size), 0);
    const int decrypted = pspDecryptPRX(data.data(), out.buffer.data(), header.psp_size);
    if (decrypted < 0) {
        error = "no known key for " + header.module_name + " (tag " + hex32(header.tag) +
                ", decrypt mode " + std::to_string(header.decrypt_mode) + "): error " +
                std::to_string(decrypted);
        out.buffer.clear();
        return false;
    }
    if (static_cast<std::uint32_t>(decrypted) != header.elf_size) {
        error = header.module_name + ": decrypted " + std::to_string(decrypted) +
                " bytes, header says " + std::to_string(header.elf_size);
        out.buffer.clear();
        return false;
    }
    if (std::memcmp(out.buffer.data(), kElfMagic, sizeof(kElfMagic)) != 0) {
        error = header.module_name + ": decrypted data is not an ELF";
        out.buffer.clear();
        return false;
    }
    return true;
#else
    error = "this build has no ~PSP decryption (configured with MPO_ENABLE_PRX_DECRYPT=OFF)";
    return false;
#endif
}

} // namespace mpo::crypto
