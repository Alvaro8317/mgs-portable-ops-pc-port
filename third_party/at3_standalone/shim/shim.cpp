// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Implementations behind the shim headers in this directory. Logging is off
// unless the environment variable MPO_AT3_LOG is set to a level (1-4).
// See ../NOTICE.md.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Common/Log.h"
#include "Common/MemoryUtil.h"

void *AllocateAlignedMemory(size_t size, size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    void *ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void FreeAlignedMemory(void *ptr) {
    if (!ptr) {
        return;
    }
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static int at3_log_threshold() {
    static const int threshold = [] {
        const char *env = std::getenv("MPO_AT3_LOG");
        return env ? std::atoi(env) : 0;
    }();
    return threshold;
}

void at3_standalone_log(int level, const char *fmt, ...) {
    if (level > at3_log_threshold()) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::fputs("[at3_standalone] ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
}
