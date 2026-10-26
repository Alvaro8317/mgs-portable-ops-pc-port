// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Core/Reporting.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include "Common/Log.h"

#define ERROR_LOG_REPORT(...)                                                                      \
    do {                                                                                           \
    } while (0)
#define ERROR_LOG_REPORT_ONCE(...)                                                                 \
    do {                                                                                           \
    } while (0)
#define WARN_LOG_REPORT(...)                                                                       \
    do {                                                                                           \
    } while (0)
#define WARN_LOG_REPORT_ONCE(...)                                                                  \
    do {                                                                                           \
    } while (0)
#define NOTICE_LOG_REPORT(...)                                                                     \
    do {                                                                                           \
    } while (0)
#define INFO_LOG_REPORT(...)                                                                       \
    do {                                                                                           \
    } while (0)

namespace Reporting {
void ReportMessage(const char *message, ...);
} // namespace Reporting
