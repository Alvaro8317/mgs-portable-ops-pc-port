// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Common/File/FileUtil.h (mpo_audio_ppsspp_difftest only).

#pragma once

#include <cstdint>
#include <string>

#include "Common/StringUtils.h"

enum class DumpFileType { Atrac3 };
inline void DumpFileIfEnabled(const uint8_t *, uint32_t, const std::string &, DumpFileType) {}
