// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Common/File/VFS/VFS.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include <cstddef>
#include <cstdint>

// MIPSVFPUUtils.cpp loads its tables with g_VFS.ReadFile("vfpu/<name>.dat").
// The stand-in aborts when a table is missing, so the silent fallback to the
// approximate MIPSVFPUFallbacks.cpp code can never produce golden vectors.
class MpoVfsShim {
public:
    uint8_t *ReadFile(const char *filename, size_t *size);
};

extern MpoVfsShim g_VFS;
