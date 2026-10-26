// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle/runtime_memory.hpp"

#include <vector>

namespace mpo::hle {

std::uint8_t *RuntimeMemory::translate(std::uint32_t address, std::uint32_t size) {
    if (size == 0u) size = 1u;
    if (!memory_.contains(address, size)) return nullptr;
    const std::uint32_t canonical = psprecomp::GuestMemory::canonical(address);
    // The vectors are the Runtime's own (non-const) storage; GuestMemory only
    // hands them out as const references.
    if (canonical >= psprecomp::GuestMemory::kPhysicalBase) {
        const std::vector<std::uint8_t> &ram = memory_.bytes();
        return const_cast<std::uint8_t *>(ram.data()) +
               (canonical - psprecomp::GuestMemory::kPhysicalBase);
    }
    const std::uint32_t offset =
        (canonical - psprecomp::GuestMemory::kVramPhysicalBase) % psprecomp::GuestMemory::kVramSize;
    if (offset + size > psprecomp::GuestMemory::kVramSize) return nullptr;
    const std::vector<std::uint8_t> &vram = memory_.vram_bytes();
    return const_cast<std::uint8_t *>(vram.data()) + offset;
}

} // namespace mpo::hle
