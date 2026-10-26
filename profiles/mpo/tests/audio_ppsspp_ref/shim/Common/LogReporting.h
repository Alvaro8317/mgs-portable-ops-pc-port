// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Common/LogReporting.h (see Common/Log.h here).

#pragma once

#include "Common/Log.h"

#define ERROR_LOG_REPORT(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define WARN_LOG_REPORT(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define ERROR_LOG_REPORT_ONCE(n, t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define WARN_LOG_REPORT_ONCE(n, t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define DEBUG_LOG_REPORT_ONCE(n, t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define WARN_LOG_N_TIMES(n, c, t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
