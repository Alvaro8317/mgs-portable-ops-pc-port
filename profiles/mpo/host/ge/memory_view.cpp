// SPDX-License-Identifier: GPL-3.0-or-later

#include "memory_view.hpp"

namespace mpo::ge {

std::uint8_t *MemoryView::Translate(std::uint32_t address, std::uint32_t size) const {
    const std::uint64_t a = Canonical(address);
    const std::uint64_t end = a + size;
    if (a >= kVramBase && a < kVramMirrorEnd) {
        const std::uint64_t off = (a - kVramBase) & (kVramSize - 1);
        if (vram_.size() < kVramSize || off + size > kVramSize) return nullptr;
        return vram_.data() + off;
    }
    if (a >= ram_base_ && end <= static_cast<std::uint64_t>(ram_base_) + ram_.size()) {
        return ram_.data() + (a - ram_base_);
    }
    if (a >= kScratchpadBase &&
        end <= static_cast<std::uint64_t>(kScratchpadBase) + scratchpad_.size()) {
        return scratchpad_.data() + (a - kScratchpadBase);
    }
    return nullptr;
}

std::uint32_t MemoryView::ClampValidSize(std::uint32_t address, std::uint32_t size) const {
    if (Translate(address, 1) == nullptr) return 0;
    // Regions are contiguous, so a binary search over the length is exact.
    std::uint32_t lo = 1;
    std::uint32_t hi = size;
    if (Translate(address, hi) != nullptr) return hi;
    while (hi - lo > 1) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (Translate(address, mid) != nullptr) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool MemoryView::CopyIn(std::uint32_t address, std::span<const std::uint8_t> data) const {
    if (data.empty()) return true;
    std::uint8_t *p = Translate(address, static_cast<std::uint32_t>(data.size()));
    if (!p) return false;
    std::memcpy(p, data.data(), data.size());
    return true;
}

} // namespace mpo::ge
