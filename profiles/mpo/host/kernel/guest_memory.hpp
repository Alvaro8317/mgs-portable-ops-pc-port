// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kernel's view of PSP memory. The kernel writes guest structures (thread
// stacks, VPL headers, *Refer*Status results, timeout and output pointers)
// through this interface, so it never depends on the runtime's memory class.
// F08b adapts PSPRecomp's GuestMemory; tests use FlatGuestMemory.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace mpo::kernel {

class GuestMemory {
public:
    virtual ~GuestMemory() = default;

    // Host pointer to [address, address + size), or nullptr when any byte of
    // the range is outside guest memory. The range never spans two regions.
    virtual std::uint8_t *translate(std::uint32_t address, std::uint32_t size) = 0;

    [[nodiscard]] bool is_valid(std::uint32_t address, std::uint32_t size = 1) {
        return translate(address, size) != nullptr;
    }

    // Invalid accesses read 0 and drop writes, like a guarded Memory:: call.
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t address);
    [[nodiscard]] std::uint32_t read_u32(std::uint32_t address);
    void write_u32(std::uint32_t address, std::uint32_t value);
    void write_u64(std::uint32_t address, std::uint64_t value);
    void fill(std::uint32_t address, std::uint8_t value, std::uint32_t size);
    // Copies between guest ranges; returns false (copying nothing) if either
    // range is invalid.
    bool copy(std::uint32_t destination, std::uint32_t source, std::uint32_t size);
    // Writes host bytes; returns false (writing nothing) if the range is invalid.
    bool write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::string read_c_string(std::uint32_t address, std::size_t max_length = 256u);
};

// One contiguous host buffer mapped at `base`. Addresses are masked with
// 0x1FFFFFFF first, so the cached/uncached/kernel mirrors (CLAUDE.md §5) all
// reach the same bytes.
class FlatGuestMemory final : public GuestMemory {
public:
    FlatGuestMemory(std::uint32_t base, std::span<std::uint8_t> bytes)
        : base_(base & kMirrorMask), bytes_(bytes) {}

    std::uint8_t *translate(std::uint32_t address, std::uint32_t size) override;

    static constexpr std::uint32_t kMirrorMask = 0x1FFFFFFFu;

private:
    std::uint32_t base_;
    std::span<std::uint8_t> bytes_;
};

} // namespace mpo::kernel
