// SPDX-License-Identifier: GPL-3.0-or-later
//
// The view of PSP memory that the GE reads and writes. It does not own the
// memory: the runtime (F11b) passes its RAM and VRAM, and the frame dump
// replayer passes buffers of its own.
//
// Addresses follow the PSP map. The cached/uncached/kernel aliases are folded
// with `addr & 0x1FFFFFFF`, and VRAM (2 MiB at 0x04000000) repeats four times
// up to 0x047FFFFF, as in PPSSPP's memory map.

#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace mpo::ge {

class MemoryView {
public:
    static constexpr std::uint32_t kVramBase = 0x04000000;
    static constexpr std::uint32_t kVramSize = 0x00200000;
    static constexpr std::uint32_t kVramMirrorEnd = 0x04800000;
    static constexpr std::uint32_t kScratchpadBase = 0x00010000;
    static constexpr std::uint32_t kScratchpadSize = 0x00004000;

    MemoryView() = default;
    // `ram` is mapped at `ram_base` (0x08000000 on a PSP); `vram` must be
    // kVramSize bytes. `scratchpad` is optional.
    MemoryView(std::span<std::uint8_t> ram, std::uint32_t ram_base, std::span<std::uint8_t> vram,
               std::span<std::uint8_t> scratchpad = {})
        : ram_(ram), ram_base_(ram_base), vram_(vram), scratchpad_(scratchpad) {}

    [[nodiscard]] static constexpr std::uint32_t Canonical(std::uint32_t address) {
        return address & 0x1FFFFFFF;
    }
    [[nodiscard]] static constexpr bool IsVramAddress(std::uint32_t address) {
        const std::uint32_t a = Canonical(address);
        return a >= kVramBase && a < kVramMirrorEnd;
    }

    // Pointer to `size` contiguous bytes at `address`, or nullptr when the
    // range is not fully inside one region (a VRAM mirror counts as one).
    [[nodiscard]] std::uint8_t *Translate(std::uint32_t address, std::uint32_t size) const;
    [[nodiscard]] bool IsValidRange(std::uint32_t address, std::uint32_t size) const {
        return Translate(address, size) != nullptr;
    }
    // Largest n <= size such that [address, address + n) is valid.
    [[nodiscard]] std::uint32_t ClampValidSize(std::uint32_t address, std::uint32_t size) const;

    // Reads return 0 for invalid addresses; writes to invalid ones are dropped.
    [[nodiscard]] std::uint8_t Read8(std::uint32_t address) const {
        const std::uint8_t *p = Translate(address, 1);
        return p ? *p : 0;
    }
    [[nodiscard]] std::uint16_t Read16(std::uint32_t address) const {
        const std::uint8_t *p = Translate(address, 2);
        if (!p) return 0;
        std::uint16_t value;
        std::memcpy(&value, p, 2);
        return value;
    }
    [[nodiscard]] std::uint32_t Read32(std::uint32_t address) const {
        const std::uint8_t *p = Translate(address, 4);
        if (!p) return 0;
        std::uint32_t value;
        std::memcpy(&value, p, 4);
        return value;
    }
    void Write32(std::uint32_t address, std::uint32_t value) const {
        std::uint8_t *p = Translate(address, 4);
        if (p) std::memcpy(p, &value, 4);
    }
    // Copies into PSP memory; returns false (and copies nothing) when the
    // destination range is invalid.
    bool CopyIn(std::uint32_t address, std::span<const std::uint8_t> data) const;

    // VRAM, addressed by offset with wrap-around (mirrors). The rasterizer uses
    // these for framebuffer and depth access, so a draw outside the buffer
    // wraps inside VRAM instead of escaping it.
    [[nodiscard]] std::uint8_t *VramAt(std::uint32_t offset, std::uint32_t size) const {
        const std::uint32_t off = offset & (kVramSize - 1);
        if (vram_.size() < kVramSize || off + size > kVramSize) return nullptr;
        return vram_.data() + off;
    }
    [[nodiscard]] std::uint16_t VramRead16(std::uint32_t offset) const {
        const std::uint8_t *p = VramAt(offset, 2);
        if (!p) return 0;
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }
    [[nodiscard]] std::uint32_t VramRead32(std::uint32_t offset) const {
        const std::uint8_t *p = VramAt(offset, 4);
        if (!p) return 0;
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    void VramWrite16(std::uint32_t offset, std::uint16_t value) const {
        std::uint8_t *p = VramAt(offset, 2);
        if (p) std::memcpy(p, &value, 2);
    }
    void VramWrite32(std::uint32_t offset, std::uint32_t value) const {
        std::uint8_t *p = VramAt(offset, 4);
        if (p) std::memcpy(p, &value, 4);
    }

    [[nodiscard]] std::span<std::uint8_t> vram() const { return vram_; }
    [[nodiscard]] std::span<std::uint8_t> ram() const { return ram_; }
    [[nodiscard]] std::uint32_t ram_base() const { return ram_base_; }

private:
    std::span<std::uint8_t> ram_;
    std::uint32_t ram_base_ = 0x08000000;
    std::span<std::uint8_t> vram_;
    std::span<std::uint8_t> scratchpad_;
};

} // namespace mpo::ge
