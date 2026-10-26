// SPDX-License-Identifier: GPL-3.0-or-later

#include "kernel/guest_memory.hpp"

#include <cstring>

namespace mpo::kernel {

std::uint8_t GuestMemory::read_u8(std::uint32_t address) {
    const std::uint8_t *host = translate(address, 1);
    return host != nullptr ? *host : 0u;
}

std::uint32_t GuestMemory::read_u32(std::uint32_t address) {
    const std::uint8_t *host = translate(address, 4);
    if (host == nullptr) return 0u;
    return static_cast<std::uint32_t>(host[0]) | (static_cast<std::uint32_t>(host[1]) << 8u) |
           (static_cast<std::uint32_t>(host[2]) << 16u) |
           (static_cast<std::uint32_t>(host[3]) << 24u);
}

void GuestMemory::write_u32(std::uint32_t address, std::uint32_t value) {
    std::uint8_t *host = translate(address, 4);
    if (host == nullptr) return;
    for (int i = 0; i < 4; ++i) host[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void GuestMemory::write_u64(std::uint32_t address, std::uint64_t value) {
    std::uint8_t *host = translate(address, 8);
    if (host == nullptr) return;
    for (int i = 0; i < 8; ++i) host[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void GuestMemory::fill(std::uint32_t address, std::uint8_t value, std::uint32_t size) {
    if (size == 0) return;
    std::uint8_t *host = translate(address, size);
    if (host != nullptr) std::memset(host, value, size);
}

bool GuestMemory::copy(std::uint32_t destination, std::uint32_t source, std::uint32_t size) {
    if (size == 0) return true;
    std::uint8_t *to = translate(destination, size);
    const std::uint8_t *from = translate(source, size);
    if (to == nullptr || from == nullptr) return false;
    std::memmove(to, from, size);
    return true;
}

bool GuestMemory::write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return true;
    std::uint8_t *to = translate(address, static_cast<std::uint32_t>(bytes.size()));
    if (to == nullptr) return false;
    std::memcpy(to, bytes.data(), bytes.size());
    return true;
}

std::string GuestMemory::read_c_string(std::uint32_t address, std::size_t max_length) {
    std::string out;
    for (std::size_t i = 0; i < max_length; ++i) {
        const std::uint8_t *host = translate(address + static_cast<std::uint32_t>(i), 1);
        if (host == nullptr || *host == 0) break;
        out.push_back(static_cast<char>(*host));
    }
    return out;
}

std::uint8_t *FlatGuestMemory::translate(std::uint32_t address, std::uint32_t size) {
    const std::uint32_t masked = address & kMirrorMask;
    if (masked < base_) return nullptr;
    const std::uint64_t offset = masked - base_;
    if (offset + size > bytes_.size()) return nullptr;
    return bytes_.data() + offset;
}

} // namespace mpo::kernel
