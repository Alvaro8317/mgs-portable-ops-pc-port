// SPDX-License-Identifier: GPL-3.0-or-later
//
// Narrow view of PSP memory for the audio library. The audio code never touches
// the runtime's memory arena directly: it receives one GuestMemory and resolves
// every PSP address through it, with bounds checks. The integration (F16b)
// builds it over the real arena; the tests build it over a std::vector.

#pragma once

#include <cstdint>
#include <cstring>

namespace mpo::audio {

class GuestMemory {
public:
    GuestMemory() = default;
    // `base` holds `size` bytes that the PSP sees at `base_address`.
    GuestMemory(uint8_t *base, uint32_t base_address, uint32_t size)
        : base_(base), base_address_(base_address), size_(size) {}

    // PSP mirrors (0x4xxxxxxx uncached, 0x8xxxxxxx kernel) fold onto the same
    // physical address (CLAUDE.md §5).
    static constexpr uint32_t Fold(uint32_t addr) { return addr & 0x1FFFFFFF; }

    // Bytes that can be read from `addr` up to the end of the view; 0 if `addr`
    // is outside of it.
    uint32_t BytesFrom(uint32_t addr) const {
        const uint32_t a = Fold(addr);
        if (base_ == nullptr || a < base_address_ || a - base_address_ >= size_) {
            return 0;
        }
        return size_ - (a - base_address_);
    }

    bool IsValidAddress(uint32_t addr) const { return BytesFrom(addr) != 0; }
    bool IsValidRange(uint32_t addr, uint32_t len) const {
        return IsValidAddress(addr) && BytesFrom(addr) >= len;
    }

    // Pointer to [addr, addr + len), or nullptr if any byte is outside the view.
    uint8_t *Ptr(uint32_t addr, uint32_t len) const {
        if (!IsValidRange(addr, len)) {
            return nullptr;
        }
        return base_ + (Fold(addr) - base_address_);
    }

    uint32_t Read32(uint32_t addr) const {
        uint32_t value = 0;
        if (const uint8_t *p = Ptr(addr, 4)) {
            std::memcpy(&value, p, 4);
        }
        return value;
    }
    bool Write32(uint32_t addr, uint32_t value) const {
        uint8_t *p = Ptr(addr, 4);
        if (p == nullptr) {
            return false;
        }
        std::memcpy(p, &value, 4);
        return true;
    }
    bool CopyFrom(void *dst, uint32_t addr, uint32_t len) const {
        const uint8_t *p = Ptr(addr, len);
        if (p == nullptr) {
            return false;
        }
        std::memcpy(dst, p, len);
        return true;
    }
    bool Memset(uint32_t addr, uint8_t value, uint32_t len) const {
        uint8_t *p = Ptr(addr, len);
        if (p == nullptr) {
            return false;
        }
        std::memset(p, value, len);
        return true;
    }
    // memmove semantics: the PSP code may copy within one buffer.
    bool Memmove(uint32_t dst, uint32_t src, uint32_t len) const {
        uint8_t *d = Ptr(dst, len);
        const uint8_t *s = Ptr(src, len);
        if (d == nullptr || s == nullptr) {
            return false;
        }
        std::memmove(d, s, len);
        return true;
    }

private:
    uint8_t *base_ = nullptr;
    uint32_t base_address_ = 0;
    uint32_t size_ = 0;
};

// Saturates to the s16 range (PPSSPP Core/Util/AudioFormat.h clamp_s16).
inline int16_t ClampS16(int value) {
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(value);
}

} // namespace mpo::audio
