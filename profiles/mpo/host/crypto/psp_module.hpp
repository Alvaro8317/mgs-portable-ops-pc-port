// SPDX-License-Identifier: GPL-3.0-or-later
//
// ~PSP (encrypted) module header parsing and decryption. Decryption uses
// PPSSPP's PrxDecrypter and libkirk, fetched at configure time and linked only
// into the installer (third_party/ppsspp_crypto/NOTICE.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mpo::crypto {

inline constexpr std::size_t kPspHeaderSize = 0x150;
// PPSSPP refuses decrypted modules larger than this; so do we.
inline constexpr std::uint32_t kMaxElfSize = 24 * 1024 * 1024;

struct PspHeader {
    std::string module_name;
    std::uint16_t attribute = 0;
    std::uint16_t comp_attribute = 0; // Bit 0: payload is gzip-compressed.
    std::uint8_t module_version_major = 0;
    std::uint8_t module_version_minor = 0;
    std::uint32_t elf_size = 0; // Size of the decrypted ELF.
    std::uint32_t psp_size = 0; // Size of the whole ~PSP file.
    std::uint32_t devkit_version = 0;
    std::uint32_t decrypt_mode = 0;
    std::uint32_t tag = 0; // Selects the key in PrxDecrypter's tables.
};

struct DecryptedModule {
    PspHeader header;
    // max(elf_size, psp_size) bytes, exactly as PPSSPP's loader leaves its
    // decryption buffer (and as "Dump decrypted EBOOT.BIN" writes it). Only the
    // first elf_size bytes are the ELF; the rest is decryption residue.
    std::vector<std::uint8_t> buffer;

    [[nodiscard]] std::span<const std::uint8_t> elf() const {
        return std::span<const std::uint8_t>(buffer).first(header.elf_size);
    }
};

[[nodiscard]] bool is_psp_module(std::span<const std::uint8_t> data);

// Validates the header against the file size. `data` is untrusted.
bool parse_psp_header(std::span<const std::uint8_t> data, PspHeader &header, std::string &error);

// False when built with MPO_ENABLE_PRX_DECRYPT=OFF.
[[nodiscard]] bool decryption_available();

// Decrypts an uncompressed ~PSP module into an ELF. libkirk trusts sizes
// inside the encrypted KIRK header before it can verify them, so call this only
// on data whose SHA-256 has already been validated (CLAUDE.md §1.2).
bool decrypt_psp_module(std::span<const std::uint8_t> data, DecryptedModule &out,
                        std::string &error);

} // namespace mpo::crypto
