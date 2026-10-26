// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Core/Core.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/MIPS/JitCommon/JitCommon.h"

enum class ExecExceptionType {
    JUMP,
    THREAD,
};

void Core_ExecException(u32 address, u32 pc, ExecExceptionType type);
void Core_BreakException(u32 pc);
