// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Core/MemMapHelpers.h (mpo_audio_ppsspp_difftest only).

#pragma once

#include <cstring>

#include "Core/MemMap.h"

enum class MemBlockFlags { ALLOC = 1, FREE = 2, WRITE = 4, READ = 8 };
inline bool MemBlockInfoDetailed() {
    return false;
}
template <typename... Args>
inline void NotifyMemInfo(Args &&...) {}

namespace Memory {
inline void Memcpy(void *to, u32 from, u32 len, const char * = nullptr, size_t = 0) {
    if (const u8 *p = GetPointerRange(from, len)) {
        std::memcpy(to, p, len);
    }
}
inline void Memcpy(u32 to, const void *from, u32 len, const char * = nullptr, size_t = 0) {
    if (u8 *p = GetPointerWriteRange(to, len)) {
        std::memcpy(p, from, len);
    }
}
inline void Memcpy(u32 to, u32 from, u32 len, const char * = nullptr, size_t = 0) {
    u8 *d = GetPointerWriteRange(to, len);
    const u8 *s = GetPointerRange(from, len);
    if (d && s) {
        std::memmove(d, s, len);
    }
}
inline void Memset(u32 addr, u8 value, u32 len, const char * = nullptr) {
    if (u8 *p = GetPointerWriteRange(addr, len)) {
        std::memset(p, value, len);
    }
}
} // namespace Memory
