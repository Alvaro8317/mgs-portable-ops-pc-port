// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Core/HLE/sceAtrac.h (mpo_audio_ppsspp_difftest only).

#pragma once

#include "Common/CommonTypes.h"

// sceSas' ATRAC3 voices are not part of the difftest.
inline u32 AtracSasAddStreamData(int, u32, u32) {
    return 0;
}
inline void AtracSasDecodeData(int, u8 *, int *SamplesNum, int *finish) {
    *SamplesNum = 0;
    *finish = 1;
}
inline int AtracSasBindContextAndGetID(u32) {
    return -1;
}
