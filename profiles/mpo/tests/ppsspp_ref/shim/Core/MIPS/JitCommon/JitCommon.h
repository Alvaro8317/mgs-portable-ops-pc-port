// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Core/MIPS/JitCommon/JitCommon.h (v1.20.4). It declares only what
// the interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include "Common/CommonTypes.h"

// The interpreter only pokes the JIT to invalidate code; there is none here.
namespace MIPSComp {

class JitInterface {
public:
    virtual ~JitInterface() = default;
    virtual void InvalidateCacheAt(u32 em_address, int length = 4) = 0;
    virtual void UpdateFCR31() = 0;
};

extern JitInterface *jit;

} // namespace MIPSComp
