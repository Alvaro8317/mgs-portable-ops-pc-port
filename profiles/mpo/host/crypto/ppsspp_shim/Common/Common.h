// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim for PPSSPP's Common/Common.h, only for compiling the unmodified
// Core/ELF/PrxDecrypter.cpp (see third_party/ppsspp_crypto/NOTICE.md). It
// provides just the integer typedefs that file uses.

#pragma once

#include <cstdint>

typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
typedef std::int8_t s8;
typedef std::int16_t s16;
typedef std::int32_t s32;
typedef std::int64_t s64;
