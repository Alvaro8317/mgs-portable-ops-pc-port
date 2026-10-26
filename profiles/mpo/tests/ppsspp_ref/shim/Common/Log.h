// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Common/Log.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include <cstdlib>

#define GENERIC_LOG(...)                                                                           \
    do {                                                                                           \
    } while (0)
#define ERROR_LOG(...)                                                                             \
    do {                                                                                           \
    } while (0)
#define WARN_LOG(...)                                                                              \
    do {                                                                                           \
    } while (0)
#define NOTICE_LOG(...)                                                                            \
    do {                                                                                           \
    } while (0)
#define INFO_LOG(...)                                                                              \
    do {                                                                                           \
    } while (0)
#define DEBUG_LOG(...)                                                                             \
    do {                                                                                           \
    } while (0)
#define VERBOSE_LOG(...)                                                                           \
    do {                                                                                           \
    } while (0)

// Release-build semantics: debug asserts vanish, real asserts abort.
#define _dbg_assert_(...)                                                                          \
    do {                                                                                           \
    } while (0)
#define _dbg_assert_msg_(...)                                                                      \
    do {                                                                                           \
    } while (0)
#define _assert_(cond)                                                                             \
    do {                                                                                           \
        if (!(cond)) std::abort();                                                                 \
    } while (0)
#define _assert_msg_(cond, ...)                                                                    \
    do {                                                                                           \
        if (!(cond)) std::abort();                                                                 \
    } while (0)
