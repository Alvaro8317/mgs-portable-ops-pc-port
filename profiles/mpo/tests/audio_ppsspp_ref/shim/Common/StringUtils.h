// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Common/StringUtils.h (mpo_audio_ppsspp_difftest only).

#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

inline std::string StringFromFormat(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

inline void DataToHexString(const unsigned char *, size_t, std::string *out, bool = true) {
    out->clear();
}
