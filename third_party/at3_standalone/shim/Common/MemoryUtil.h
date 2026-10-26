// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal stand-in for PPSSPP's Common/MemoryUtil.h (v1.20.4): only the aligned
// allocation helpers that at3_standalone/mem.cpp uses. See ../../NOTICE.md.

#pragma once

#include <cstddef>

void *AllocateAlignedMemory(size_t size, size_t alignment);
void FreeAlignedMemory(void *ptr);
