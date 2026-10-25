// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim for PPSSPP's Common/Log.h (see Common/Common.h in this directory). The
// installer reports errors itself, so the decrypter's log lines are dropped.

#pragma once

#define INFO_LOG(...) ((void)0)
