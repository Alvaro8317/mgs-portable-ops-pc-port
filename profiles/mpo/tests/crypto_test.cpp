// SPDX-License-Identifier: GPL-3.0-or-later
//
// ~PSP header parsing and decryption failure paths on synthetic modules. No
// game data and no real encrypted module are needed: a successful decryption
// is only checked against the user's own disc (docs/sessions/F02.md).

#include "crypto/psp_module.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <random>
#include <string>
#include <vector>

namespace {

using mpo::crypto::DecryptedModule;
using mpo::crypto::PspHeader;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

void store_le16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void store_le32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

// A ~PSP file with a plausible header and `fill` everywhere else. The fields
// that matter to the parser are set; the encrypted parts are noise.
std::vector<std::uint8_t> make_module(std::uint32_t tag, std::uint32_t psp_size,
                                      std::uint32_t elf_size, std::mt19937 *noise = nullptr) {
    std::vector<std::uint8_t> bytes(psp_size, 0);
    if (noise != nullptr) {
        for (auto &byte : bytes) byte = static_cast<std::uint8_t>((*noise)());
    }
    bytes[0] = '~';
    bytes[1] = 'P';
    bytes[2] = 'S';
    bytes[3] = 'P';
    store_le16(bytes, 0x04, 0x0000);
    store_le16(bytes, 0x06, 0x0000);
    bytes[0x08] = 1;
    bytes[0x09] = 1;
    const char name[] = "synthetic_module";
    for (std::size_t i = 0; i < 28; ++i) {
        bytes[0x0A + i] = i < sizeof(name) ? static_cast<std::uint8_t>(name[i]) : 0;
    }
    store_le32(bytes, 0x28, elf_size);
    store_le32(bytes, 0x2C, psp_size);
    store_le32(bytes, 0x7C, 9);
    store_le32(bytes, 0xB0, elf_size);
    store_le32(bytes, 0xD0, tag);
    return bytes;
}

void test_header_parsing() {
    PspHeader header;
    std::string error;
    const auto good = make_module(0x12345678, 0x400, 0x300);
    expect(mpo::crypto::is_psp_module(good), "synthetic module is detected");
    expect(mpo::crypto::parse_psp_header(good, header, error), "valid header: " + error);
    expect(header.module_name == "synthetic_module", "module name is read");
    expect(header.elf_size == 0x300 && header.psp_size == 0x400, "sizes are read");
    expect(header.tag == 0x12345678 && header.decrypt_mode == 9, "tag and mode are read");
    expect(header.module_version_major == 1 && header.module_version_minor == 1, "version read");

    // Every truncation must be rejected without reading out of bounds.
    for (std::size_t size = 0; size < good.size(); ++size) {
        const std::vector<std::uint8_t> cut(good.begin(),
                                            good.begin() + static_cast<std::ptrdiff_t>(size));
        expect(!mpo::crypto::parse_psp_header(cut, header, error),
               "truncated header (" + std::to_string(size) + " bytes) must be rejected");
    }

    auto bad_magic = good;
    bad_magic[0] = 0x7F;
    expect(!mpo::crypto::is_psp_module(bad_magic) &&
               !mpo::crypto::parse_psp_header(bad_magic, header, error),
           "bad magic must be rejected");

    auto no_payload = good;
    store_le32(no_payload, 0x2C, 0x150);
    expect(!mpo::crypto::parse_psp_header(no_payload, header, error),
           "psp_size without payload must be rejected");

    auto no_elf = good;
    store_le32(no_elf, 0x28, 0);
    expect(!mpo::crypto::parse_psp_header(no_elf, header, error), "elf_size 0 rejected");

    auto huge_elf = good;
    store_le32(huge_elf, 0x28, mpo::crypto::kMaxElfSize + 1);
    expect(!mpo::crypto::parse_psp_header(huge_elf, header, error), "huge elf_size rejected");

    // A long name without a terminator stays inside its 28-byte field.
    auto long_name = good;
    for (std::size_t i = 0; i < 28; ++i) long_name[0x0A + i] = 'n';
    expect(mpo::crypto::parse_psp_header(long_name, header, error) &&
               header.module_name == std::string(28, 'n'),
           "unterminated module name is cut at 28 bytes");
}

void test_decrypt_rejections() {
    DecryptedModule module;
    std::string error;

    auto compressed = make_module(0x12345678, 0x400, 0x300);
    store_le16(compressed, 0x06, 1);
    expect(!mpo::crypto::decrypt_psp_module(compressed, module, error) &&
               error.find("gzip") != std::string::npos,
           "gzip-compressed module must be rejected: " + error);

    const auto unknown_tag = make_module(0x12345678, 0x400, 0x300);
    const bool decrypted = mpo::crypto::decrypt_psp_module(unknown_tag, module, error);
    expect(!decrypted && module.buffer.empty(), "unknown tag must not decrypt");
    if (mpo::crypto::decryption_available()) {
        expect(error.find("no known key") != std::string::npos, "unknown tag error: " + error);
    } else {
        expect(error.find("MPO_ENABLE_PRX_DECRYPT=OFF") != std::string::npos,
               "disabled build says so: " + error);
    }
}

// Known tags with noise instead of a real payload: the decrypter must reject
// them at its SHA-1 check, before any KIRK size field is trusted. Runs under
// ASan/UBSan in CI. The tags select keys in PrxDecrypter's tables (type 0/1
// and type 2 paths); they are identifiers, not keys.
void test_decrypt_noise() {
    if (!mpo::crypto::decryption_available()) return;
    const std::uint32_t tags[] = {0xC0CB167C, 0x4C9494F0, 0xD91605F0, 0x00000000};
    std::mt19937 noise(20261025);
    DecryptedModule module;
    std::string error;
    for (const std::uint32_t tag : tags) {
        for (int round = 0; round < 16; ++round) {
            const auto bytes = make_module(tag, 0x1000, 0xE00, &noise);
            expect(!mpo::crypto::decrypt_psp_module(bytes, module, error),
                   "noise must not decrypt (tag " + std::to_string(tag) + ")");
            expect(module.buffer.empty(), "failed decryption leaves no buffer");
        }
    }
}

} // namespace

int main() {
    try {
        test_header_parsing();
        test_decrypt_rejections();
        test_decrypt_noise();
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "FAIL: uncaught exception: %s\n", exception.what());
        return EXIT_FAILURE;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("mpo_crypto_test: all checks passed (decryption %s)\n",
                mpo::crypto::decryption_available() ? "built in" : "disabled");
    return EXIT_SUCCESS;
}
