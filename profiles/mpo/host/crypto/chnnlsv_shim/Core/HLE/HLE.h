// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim for PPSSPP's Core/HLE/HLE.h (see Core/MemMapHelpers.h in this
// directory): the logging helpers and the HLE module table types that
// sceChnnlsv.cpp uses. Nothing is registered and nothing is logged.

#pragma once

#include "Common/Common.h"
#include "Common/Swap.h"

#include <cstddef>
#include <cstring>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef ERROR_LOG
#define ERROR_LOG(...) ((void)0)
#endif

namespace Log {
enum Type { sceKernel, sceMisc };
} // namespace Log

template <typename T>
inline T hleLogDebug(Log::Type, T value, ...) {
    return value;
}
template <typename T>
inline T hleLogError(Log::Type, T value, ...) {
    return value;
}
template <typename T>
inline T hleNoLog(T value) {
    return value;
}

typedef void (*HLEFunc)();

struct HLEFunction {
    u32 ID;
    HLEFunc func;
    const char *name;
    char retmask;
    const char *argmask;
};

inline void RegisterHLEModule(const char *, int, const HLEFunction *) {}
