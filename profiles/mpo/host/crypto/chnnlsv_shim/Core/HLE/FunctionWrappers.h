// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim for PPSSPP's Core/HLE/FunctionWrappers.h (see Core/MemMapHelpers.h in
// this directory): the wrappers that sceChnnlsv.cpp puts in its HLE tables.
// The tables are never registered, so the wrappers do nothing.

#pragma once

#include "Common/Common.h"

template <int func(u32)>
void WrapI_U() {}
template <int func(u32, int)>
void WrapI_UI() {}
template <int func(u32, u32, int)>
void WrapI_UUI() {}
template <int func(u32, u32, u32)>
void WrapI_UUU() {}
template <int func(u32, int, int, u32, u32)>
void WrapI_UIIUU() {}
template <int func(u32, int, u32, int, int)>
void WrapI_UIUII() {}
template <u32 func(u32, int, u32, int, int)>
void WrapU_UIUII() {}
