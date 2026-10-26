// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal stand-in for PPSSPP's ppsspp_config.h (v1.20.4), reduced to the
// architecture macros that at3_standalone/atrac3plusdsp.cpp uses. Part of the
// MGS: Portable Ops PC port build of at3_standalone (see ../NOTICE.md).

#pragma once

#define PPSSPP_ARCH(PPSSPP_FEATURE) (PPSSPP_ARCH_##PPSSPP_FEATURE)

#if defined(_M_IX86) || defined(__i386__)
#define PPSSPP_ARCH_X86 1
#define PPSSPP_ARCH_SSE2 1
#endif

#if defined(_M_X64) || defined(__amd64__) || defined(__x86_64__)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_SSE2 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define PPSSPP_ARCH_ARM64 1
#define PPSSPP_ARCH_ARM_NEON 1
#endif

#ifndef PPSSPP_ARCH_X86
#define PPSSPP_ARCH_X86 0
#endif
#ifndef PPSSPP_ARCH_AMD64
#define PPSSPP_ARCH_AMD64 0
#endif
#ifndef PPSSPP_ARCH_SSE2
#define PPSSPP_ARCH_SSE2 0
#endif
#ifndef PPSSPP_ARCH_ARM64
#define PPSSPP_ARCH_ARM64 0
#endif
#ifndef PPSSPP_ARCH_ARM_NEON
#define PPSSPP_ARCH_ARM_NEON 0
#endif
