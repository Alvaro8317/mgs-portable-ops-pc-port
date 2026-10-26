// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Core/HLE/ReplaceTables.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

typedef int (*ReplaceFunc)();

enum {
    REPFLAG_ALLOWINLINE = 0x01,
    REPFLAG_DISABLED = 0x02,
    REPFLAG_HOOKENTER = 0x04,
    REPFLAG_HOOKEXIT = 0x08,
};

struct ReplacementTableEntry {
    const char *name;
    ReplaceFunc replaceFunc;
    const void *jitReplaceFunc;
    int flags;
    s32 hookOffset;
};

const ReplacementTableEntry *GetReplacementFunc(size_t index);
