// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Common/StringUtils.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

#include <cstddef>
#include <string>

std::string StringFromFormat(const char *format, ...);
void truncate_cpy(char *dest, std::size_t destSize, const char *src);
