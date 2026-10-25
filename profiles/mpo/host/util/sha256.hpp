// SPDX-License-Identifier: GPL-3.0-or-later
//
// Incremental SHA-256 (FIPS 180-4). psprecomp::sha256_file() only hashes host
// files; the installer hashes files streamed out of the ISO.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace mpo {

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    // Finishes the hash and returns it as 64 lower-case hex digits. The object
    // must not be updated afterwards.
    [[nodiscard]] std::string hex_digest();

private:
    void compress(const std::uint8_t *block);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> data);

} // namespace mpo
