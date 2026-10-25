// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim for PPSSPP's Common/Swap.h (see Common/Common.h in this directory). The
// PSP and every supported host are little-endian, so the *_le types are the
// plain integer types.

#pragma once

#include "Common/Common.h"

#include <bit>

static_assert(std::endian::native == std::endian::little,
              "PrxDecrypter needs a little-endian host");

typedef u16 u16_le;
typedef u32 u32_le;
typedef s32 s32_le;
