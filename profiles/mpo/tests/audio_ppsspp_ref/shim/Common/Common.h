// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Common/Common.h (see Common/Log.h in this directory).

#pragma once

#include <cstddef>
#include <cstdint>

#include "Common/CommonTypes.h"
#include "Common/Log.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

#define IS_LITTLE_ENDIAN 1
