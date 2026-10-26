// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Core/MemMap.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Opcode.h"

// Guest memory backed by the difftest CpuState that is being executed.
namespace Memory {

u8 Read_U8(u32 address);
u16 Read_U16(u32 address);
u32 Read_U32(u32 address);
float Read_Float(u32 address);
void Write_U8(u8 data, u32 address);
void Write_U16(u16 data, u32 address);
void Write_U32(u32 data, u32 address);
void Write_Float(float data, u32 address);
const u8 *GetPointerRange(u32 address, u32 size);
u8 *GetPointerWriteRange(u32 address, u32 size);
bool IsValidAddress(u32 address);
Opcode Read_Instruction(u32 address, bool resolveReplacements = false);
Opcode Read_Opcode_JIT(u32 address);

} // namespace Memory
