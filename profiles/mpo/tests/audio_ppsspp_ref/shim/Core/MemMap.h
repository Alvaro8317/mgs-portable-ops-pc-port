// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Core/MemMap.h (mpo_audio_ppsspp_difftest only).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"

// The difftest points this at its fake RAM before running PPSSPP's code.
struct PpssppRefRam {
    uint8_t *base = nullptr;
    uint32_t baseAddress = 0;
    uint32_t size = 0;
};
inline PpssppRefRam g_ppssppRefRam;

namespace Memory {
inline uint32_t Fold(uint32_t a) {
    return a & 0x1FFFFFFF;
}
inline bool IsValidAddress(uint32_t addr) {
    const uint32_t a = Fold(addr);
    return g_ppssppRefRam.base && a >= g_ppssppRefRam.baseAddress &&
           a - g_ppssppRefRam.baseAddress < g_ppssppRefRam.size;
}
inline bool IsValidRange(uint32_t addr, uint32_t len) {
    return IsValidAddress(addr) &&
           g_ppssppRefRam.size - (Fold(addr) - g_ppssppRefRam.baseAddress) >= len;
}
inline u8 *GetPointerUnchecked(uint32_t addr) {
    return g_ppssppRefRam.base + (Fold(addr) - g_ppssppRefRam.baseAddress);
}
inline u8 *GetPointer(uint32_t addr) {
    return IsValidAddress(addr) ? GetPointerUnchecked(addr) : nullptr;
}
inline u8 *GetPointerWrite(uint32_t addr) {
    return GetPointer(addr);
}
inline u8 *GetPointerOrNull(uint32_t addr) {
    return GetPointer(addr);
}
inline u8 *GetPointerWriteRange(uint32_t addr, uint32_t len) {
    return IsValidRange(addr, len) ? GetPointerUnchecked(addr) : nullptr;
}
inline const u8 *GetPointerRange(uint32_t addr, uint32_t len) {
    return IsValidRange(addr, len) ? GetPointerUnchecked(addr) : nullptr;
}
inline u32 Read_U32(uint32_t addr) {
    u32 v = 0;
    if (IsValidRange(addr, 4)) {
        std::memcpy(&v, GetPointerUnchecked(addr), 4);
    }
    return v;
}
inline void Write_U32(u32 v, uint32_t addr) {
    if (IsValidRange(addr, 4)) {
        std::memcpy(GetPointerUnchecked(addr), &v, 4);
    }
}
inline void WriteUnchecked_U32(u32 v, uint32_t addr) {
    Write_U32(v, addr);
}
} // namespace Memory

template <class T>
struct PSPPointer {
    u32 ptr = 0;
    static PSPPointer<T> Create(u32 p) {
        PSPPointer<T> r;
        r.ptr = p;
        return r;
    }
    T *get() const { return reinterpret_cast<T *>(Memory::GetPointerUnchecked(ptr)); }
    T *operator->() const { return get(); }
    T &operator*() const { return *get(); }
    operator T *() const { return get(); }
    bool IsValid() const { return Memory::IsValidRange(ptr, (u32)sizeof(T)); }
    bool Equals(u32 p) const { return ptr == p; }
};
