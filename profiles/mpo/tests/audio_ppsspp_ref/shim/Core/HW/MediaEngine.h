// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Core/HW/MediaEngine.h (mpo_audio_ppsspp_difftest only).

#pragma once

// The real header pulls in the whole MPEG stack; SimpleAudioDec.h only needs
// PointerWrap and the codec enum from it.
#include <cstdint>
#include <map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Serialize/Serializer.h"
#include "Core/HLE/sceAudiocodec.h"
