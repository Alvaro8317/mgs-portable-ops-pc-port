// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's Common/Log.h, just enough to compile the audio files
// of PPSSPP v1.20.4 for mpo_audio_ppsspp_difftest. Logging is dropped;
// assertions abort.

#pragma once

#include <cstdio>
#include <cstdlib>

#include "ppsspp_config.h"

namespace Log {
enum LogType { Atrac, ME, SasMix, sceSas, SaveState, HLE, Audio, Common, sceAudio };
}

#define PPSSPP_REF_NOLOG(...) ((void)0)
#define ERROR_LOG(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define WARN_LOG(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define NOTICE_LOG(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define INFO_LOG(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define DEBUG_LOG(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)
#define VERBOSE_LOG(t, ...) PPSSPP_REF_NOLOG(__VA_ARGS__)

#define _assert_(cond)                                                                             \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "PPSSPP assert failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
#define _assert_msg_(cond, ...) _assert_(cond)
#define _dbg_assert_(cond) ((void)0)
#define _dbg_assert_msg_(cond, ...) ((void)0)
