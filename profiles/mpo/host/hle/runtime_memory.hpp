// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_kernel's view of PSP memory (kernel::GuestMemory) over the memory of a
// psprecomp::Runtime, so the kernel writes thread stacks, VPL headers and
// *Refer*Status results into the same bytes the recompiled code reads.

#pragma once

#include <cstdint>

#include "kernel/guest_memory.hpp"
#include "psprecomp/guest_memory.hpp"

namespace mpo::hle {

class RuntimeMemory final : public kernel::GuestMemory {
public:
    explicit RuntimeMemory(psprecomp::GuestMemory &memory) : memory_(memory) {}

    // RAM (0x08000000 + size) and the 2 MiB of VRAM with its mirrors, through
    // the cached/uncached/kernel aliases. A range never spans two regions.
    std::uint8_t *translate(std::uint32_t address, std::uint32_t size) override;

private:
    psprecomp::GuestMemory &memory_;
};

} // namespace mpo::hle
