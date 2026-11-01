// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim for PPSSPP's Core/MemMapHelpers.h, so that the unmodified
// Core/HLE/sceChnnlsv.cpp (fetched by third_party/ppsspp_crypto) compiles
// outside PPSSPP. Only its crypto core (sceSdMac*, sceSdCipher*) is used, by
// crypto/savedata_cipher_kirk.cpp; the guest-memory wrappers around it are
// never called, so there is no guest memory here: every pointer is invalid.

#pragma once

#include "Common/Common.h"
#include "Common/Swap.h"

#include <cstring>

namespace Memory {

inline u8 *GetPointerWrite(u32) {
    return nullptr;
}
inline u8 *GetPointerWriteUnchecked(u32) {
    return nullptr;
}
inline bool IsValidAddress(u32) {
    return false;
}
inline bool IsValidRange(u32, u32) {
    return false;
}

} // namespace Memory

template <typename T>
struct PSPPointer {
    T *ptr = nullptr;

    static PSPPointer Create(u32) { return PSPPointer{}; }
    [[nodiscard]] bool IsValid() const { return ptr != nullptr; }
    T &operator*() const { return *ptr; }
};
