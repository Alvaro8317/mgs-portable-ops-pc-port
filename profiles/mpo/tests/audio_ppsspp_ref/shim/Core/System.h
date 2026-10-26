// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Core/System.h (mpo_audio_ppsspp_difftest only).

#pragma once

struct PpssppRefCompatFlags {
    bool RockmanDash2SoundFix = false;
};
struct PpssppRefCompat {
    PpssppRefCompatFlags flags_;
    const PpssppRefCompatFlags &flags() const { return flags_; }
};
struct PpssppRefCoreParameter {
    PpssppRefCompat compat;
};
inline PpssppRefCoreParameter &PSP_CoreParameter() {
    static PpssppRefCoreParameter param;
    return param;
}
