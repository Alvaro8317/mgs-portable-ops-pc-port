// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal stand-in for PPSSPP's Common/Log.h (v1.20.4): the four log macros that
// at3_standalone/compat.cpp uses, routed to at3_standalone_log() (shim.cpp).
// See ../../NOTICE.md.

#pragma once

namespace Log {
enum LogType { ME };
}

// Levels: 1 error, 2 warning, 3 info, 4 debug.
void at3_standalone_log(int level, const char *fmt, ...);

#define ERROR_LOG(type, ...) at3_standalone_log(1, __VA_ARGS__)
#define WARN_LOG(type, ...) at3_standalone_log(2, __VA_ARGS__)
#define INFO_LOG(type, ...) at3_standalone_log(3, __VA_ARGS__)
#define DEBUG_LOG(type, ...) at3_standalone_log(4, __VA_ARGS__)
