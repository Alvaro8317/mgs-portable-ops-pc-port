// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/sceSas.cpp @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// The sceSasCore API without its HLE glue: every method validates its
// arguments exactly like PPSSPP and returns the PSP result code. Registering
// the NIDs, the thread delay after __sceSasCore (EstimateMixUs) and the
// optional mixer thread are the integration's job (F16b).
//
// `core` (the guest address of the SasCore struct) is kept in the signatures
// because the PSP API has it; like PPSSPP there is a single instance.

#pragma once

#include <cstdint>

#include "guest_memory.h"
#include "sas_audio.h"

namespace mpo::audio {

class SasCore {
public:
    SasCore() = default;

    // Global mute (debug); keeps the rest of the mix identical.
    bool mute = false;

    SasInstance &instance() { return sas_; }
    const SasInstance &instance() const { return sas_; }
    // Delay that PPSSPP applies to the calling thread after a mix.
    int EstimateMixUs() const { return sas_.EstimateMixUs(); }

    void SetAtracBridge(SasAtracBridge *bridge) { atrac_bridge_ = bridge; }

    uint32_t Init(const GuestMemory &mem, uint32_t core, uint32_t grainSize, uint32_t maxVoices,
                  uint32_t outputMode, uint32_t sampleRate);
    uint32_t GetEndFlag(uint32_t core) const;
    // __sceSasCore. `dispatch_enabled` mirrors __KernelIsDispatchEnabled().
    uint32_t Core(const GuestMemory &mem, uint32_t core, uint32_t outAddr,
                  bool dispatch_enabled = true);
    uint32_t CoreWithMix(const GuestMemory &mem, uint32_t core, uint32_t inoutAddr, int leftVolume,
                         int rightVolume, bool dispatch_enabled = true);
    uint32_t SetVoice(const GuestMemory &mem, uint32_t core, int voiceNum, uint32_t vagAddr,
                      int size, int loop);
    uint32_t SetVoicePCM(const GuestMemory &mem, uint32_t core, int voiceNum, uint32_t pcmAddr,
                         int size, int loopPos);
    uint32_t GetPauseFlag(uint32_t core) const;
    uint32_t SetPause(uint32_t core, uint32_t voicebit, int pause);
    uint32_t SetVolume(uint32_t core, int voiceNum, int leftVol, int rightVol, int effectLeftVol,
                       int effectRightVol);
    uint32_t SetPitch(uint32_t core, int voiceNum, int pitch);
    uint32_t SetKeyOn(const GuestMemory &mem, uint32_t core, int voiceNum);
    uint32_t SetKeyOff(uint32_t core, int voiceNum);
    uint32_t SetNoise(uint32_t core, int voiceNum, int freq);
    uint32_t SetSL(uint32_t core, int voiceNum, int level);
    uint32_t SetADSR(uint32_t core, int voiceNum, int flag, int a, int d, int s, int r);
    uint32_t SetADSRMode(uint32_t core, int voiceNum, int flag, int a, int d, int s, int r);
    uint32_t SetSimpleADSR(uint32_t core, int voiceNum, uint32_t ADSREnv1, uint32_t ADSREnv2);
    uint32_t GetEnvelopeHeight(uint32_t core, int voiceNum) const;
    uint32_t RevType(uint32_t core, int type);
    uint32_t RevParam(uint32_t core, int delay, int feedback);
    uint32_t RevEVOL(uint32_t core, uint32_t lv, uint32_t rv);
    uint32_t RevVON(uint32_t core, int dry, int wet);
    uint32_t GetGrain(uint32_t core) const;
    uint32_t SetGrain(uint32_t core, int grain);
    uint32_t GetOutputMode(uint32_t core) const;
    uint32_t SetOutputMode(uint32_t core, uint32_t outputMode);
    uint32_t GetAllEnvelopeHeights(const GuestMemory &mem, uint32_t core, uint32_t heightsAddr);
    // Unimplemented upstream too; they return 0.
    uint32_t SetTriangularWave(uint32_t core, int voice, int unknown);
    uint32_t SetSteepWave(uint32_t core, int voice, int unknown);
    uint32_t SetVoiceATRAC3(const GuestMemory &mem, uint32_t core, int voiceNum,
                            uint32_t atrac3Context);
    uint32_t ConcatenateATRAC3(uint32_t core, int voiceNum, uint32_t atrac3DataAddr,
                               int atrac3DataLength);
    uint32_t UnsetATRAC3(const GuestMemory &mem, uint32_t core, int voiceNum);

private:
    SasInstance sas_;
    SasAtracBridge *atrac_bridge_ = nullptr;
};

} // namespace mpo::audio
