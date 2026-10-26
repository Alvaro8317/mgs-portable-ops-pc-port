// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Core/Config.h (mpo_audio_ppsspp_difftest only).

#pragma once

#include <cmath>

#include "Common/Common.h"

struct PpssppRefConfig {
    int iReverbVolume = 100;
};
inline PpssppRefConfig g_Config;

// Core/Config.cpp
inline float Volume100ToMultiplier(int volume) {
    if (volume > 100) {
        return volume / 100.0f;
    }
    return powf(volume * 0.01f, 1.75f);
}
