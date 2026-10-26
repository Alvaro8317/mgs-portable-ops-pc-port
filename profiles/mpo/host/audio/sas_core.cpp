// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/sceSas.cpp @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Same argument checks and side effects as upstream; logging, the HLE delay
// and the optional mixer thread are left out (see sas_core.h).

#include "sas_core.h"

#include <cstdlib>

#include "audio_errors.h"

namespace mpo::audio {
namespace {

bool BadVoice(int voiceNum) {
    return voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0;
}

} // namespace

uint32_t SasCore::Init(const GuestMemory &mem, uint32_t core, uint32_t grainSize,
                       uint32_t maxVoices, uint32_t outputMode, uint32_t sampleRate) {
    if (!mem.IsValidAddress(core) || (core & 0x3F) != 0) {
        return kErrSasBadAddress;
    }
    if (maxVoices == 0 || maxVoices > PSP_SAS_VOICES_MAX) {
        return kErrSasInvalidMaxVoices;
    }
    if (grainSize < 0x40 || grainSize > 0x800 || (grainSize & 0x1F) != 0) {
        return kErrSasInvalidGrain;
    }
    if (outputMode != 0 && outputMode != 1) {
        return kErrSasInvalidOutputMode;
    }
    if (sampleRate != 44100) {
        return kErrSasInvalidSampleRate;
    }

    sas_.SetGrainSize(static_cast<int>(grainSize));
    // Seems like the maxVoices param is actually ignored for all intents and purposes.
    sas_.maxVoices = PSP_SAS_VOICES_MAX;
    sas_.outputMode = static_cast<int>(outputMode);
    for (int i = 0; i < sas_.maxVoices; i++) {
        sas_.voices[i].sampleRate = static_cast<int>(sampleRate);
        sas_.voices[i].playing = false;
        sas_.voices[i].loop = false;
    }
    return 0;
}

uint32_t SasCore::GetEndFlag(uint32_t /*core*/) const {
    uint32_t endFlag = 0;
    for (int i = 0; i < sas_.maxVoices; i++) {
        if (!sas_.voices[i].playing) {
            endFlag |= (1u << i);
        }
    }
    return endFlag;
}

uint32_t SasCore::Core(const GuestMemory &mem, uint32_t /*core*/, uint32_t outAddr,
                       bool dispatch_enabled) {
    if (!mem.IsValidAddress(outAddr)) {
        return kErrSasInvalidParameter;
    }
    if (!dispatch_enabled) {
        return kErrKernelCanNotWait;
    }
    sas_.Mix(mem, outAddr, 0, 0, 0, mute);
    return 0;
}

uint32_t SasCore::CoreWithMix(const GuestMemory &mem, uint32_t /*core*/, uint32_t inoutAddr,
                              int leftVolume, int rightVolume, bool dispatch_enabled) {
    if (!mem.IsValidAddress(inoutAddr)) {
        return kErrSasInvalidParameter;
    }
    if (sas_.outputMode == PSP_SAS_OUTPUTMODE_RAW) {
        return kErrKernelBadArgument;
    }
    if (!dispatch_enabled) {
        return kErrKernelCanNotWait;
    }
    sas_.Mix(mem, inoutAddr, inoutAddr, leftVolume, rightVolume, mute);
    return 0;
}

uint32_t SasCore::SetVoice(const GuestMemory &mem, uint32_t /*core*/, int voiceNum,
                           uint32_t vagAddr, int size, int loop) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    if (size == 0 || (static_cast<uint32_t>(size) & 0xF) != 0) {
        return kErrSasInvalidParameter;
    }
    if (loop != 0 && loop != 1) {
        return kErrSasInvalidLoopPos;
    }
    if (!mem.IsValidAddress(vagAddr)) {
        // Upstream logs "Ignoring invalid VAG audio address" and returns 0.
        return 0;
    }

    SasVoice &v = sas_.voices[voiceNum];
    if (v.type == VOICETYPE_ATRAC3) {
        return kErrSasAtrac3AlreadySet;
    }

    if (size < 0) {
        // Upstream "POSSIBLE HACK": negative sizes return OK but should not
        // play any audio (PPSSPP issue #5652).
        size = 0;
    }

    bool reset = false;
    if (v.type != VOICETYPE_VAG || v.vagAddr != vagAddr ||
        v.vagSize != static_cast<uint32_t>(size) || v.loop != (loop != 0)) {
        v.type = VOICETYPE_VAG;
        reset = true;
    }
    v.vagAddr = vagAddr; // Real VAG header is 0x30 bytes behind the vagAddr
    v.vagSize = static_cast<uint32_t>(size);
    v.loop = loop != 0;
    if (v.on) {
        v.playing = true;
    }
    if (reset) {
        v.vag.Start(vagAddr, static_cast<uint32_t>(size), loop != 0);
    }
    return 0;
}

uint32_t SasCore::SetVoicePCM(const GuestMemory &mem, uint32_t /*core*/, int voiceNum,
                              uint32_t pcmAddr, int size, int loopPos) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    if (size <= 0 || size > 0x10000) {
        return kErrSasInvalidPcmSize;
    }
    if (loopPos >= size) {
        return kErrSasInvalidLoopPos;
    }
    if (!mem.IsValidAddress(pcmAddr)) {
        // Upstream logs "Ignoring invalid PCM audio address" and returns 0.
        return 0;
    }

    SasVoice &v = sas_.voices[voiceNum];
    if (v.type == VOICETYPE_ATRAC3) {
        return kErrSasAtrac3AlreadySet;
    }

    v.type = VOICETYPE_PCM;
    v.pcmAddr = pcmAddr;
    v.pcmSize = size;
    v.pcmIndex = 0;
    v.pcmLoopPos = loopPos >= 0 ? loopPos : 0;
    v.loop = loopPos >= 0;
    v.playing = true;
    return 0;
}

uint32_t SasCore::GetPauseFlag(uint32_t /*core*/) const {
    uint32_t pauseFlag = 0;
    for (int i = 0; i < sas_.maxVoices; i++) {
        if (sas_.voices[i].paused) {
            pauseFlag |= (1u << i);
        }
    }
    return pauseFlag;
}

uint32_t SasCore::SetPause(uint32_t /*core*/, uint32_t voicebit, int pause) {
    for (int i = 0; voicebit != 0; i++, voicebit >>= 1) {
        if (i < PSP_SAS_VOICES_MAX && i >= 0) {
            if ((voicebit & 1) != 0) {
                sas_.voices[i].paused = pause != 0;
            }
        }
    }
    return 0;
}

uint32_t SasCore::SetVolume(uint32_t /*core*/, int voiceNum, int leftVol, int rightVol,
                            int effectLeftVol, int effectRightVol) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    bool overVolume = std::abs(leftVol) > PSP_SAS_VOL_MAX || std::abs(rightVol) > PSP_SAS_VOL_MAX;
    overVolume = overVolume || std::abs(effectLeftVol) > PSP_SAS_VOL_MAX ||
                 std::abs(effectRightVol) > PSP_SAS_VOL_MAX;
    if (overVolume) {
        return kErrSasInvalidVolume;
    }

    SasVoice &v = sas_.voices[voiceNum];
    v.volumeLeft = leftVol;
    v.volumeRight = rightVol;
    v.effectLeft = effectLeftVol;
    v.effectRight = effectRightVol;
    return 0;
}

uint32_t SasCore::SetPitch(uint32_t /*core*/, int voiceNum, int pitch) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    if (pitch < PSP_SAS_PITCH_MIN || pitch > PSP_SAS_PITCH_MAX) {
        return kErrSasInvalidPitch;
    }
    sas_.voices[voiceNum].pitch = pitch;
    return 0;
}

uint32_t SasCore::SetKeyOn(const GuestMemory &mem, uint32_t /*core*/, int voiceNum) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    if (sas_.voices[voiceNum].paused || sas_.voices[voiceNum].on) {
        return kErrSasVoicePaused;
    }
    sas_.voices[voiceNum].KeyOn(mem);
    return 0;
}

// sceSasSetKeyOff can be used to start sounds, that just sound during the
// Release phase!
uint32_t SasCore::SetKeyOff(uint32_t /*core*/, int voiceNum) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    if (sas_.voices[voiceNum].paused || !sas_.voices[voiceNum].on) {
        return kErrSasVoicePaused; // this is ok
    }
    sas_.voices[voiceNum].KeyOff();
    return 0;
}

uint32_t SasCore::SetNoise(uint32_t /*core*/, int voiceNum, int freq) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    if (freq < 0 || freq >= 64) {
        return kErrSasInvalidNoiseFreq;
    }
    SasVoice &v = sas_.voices[voiceNum];
    v.type = VOICETYPE_NOISE;
    v.noiseFreq = freq;
    return 0;
}

uint32_t SasCore::SetSL(uint32_t /*core*/, int voiceNum, int level) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    sas_.voices[voiceNum].envelope.SetSustainLevel(level);
    return 0;
}

uint32_t SasCore::SetADSR(uint32_t /*core*/, int voiceNum, int flag, int a, int d, int s, int r) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    // Create a mask like flag for the invalid values.
    const int invalid =
        (a < 0 ? 0x1 : 0) | (d < 0 ? 0x2 : 0) | (s < 0 ? 0x4 : 0) | (r < 0 ? 0x8 : 0);
    if (invalid & flag) {
        return kErrSasInvalidAdsrRate;
    }
    sas_.voices[voiceNum].envelope.SetRate(flag, a, d, s, r);
    return 0;
}

uint32_t SasCore::SetADSRMode(uint32_t /*core*/, int voiceNum, int flag, int a, int d, int s,
                              int r) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }

    // Probably by accident (?), the PSP ignores the top bit of these values.
    a = a & ~static_cast<int>(0x80000000u);
    d = d & ~static_cast<int>(0x80000000u);
    s = s & ~static_cast<int>(0x80000000u);
    r = r & ~static_cast<int>(0x80000000u);

    // This will look like the update flag for the invalid modes.
    int invalid = 0;
    if (a > 5 || (a & 1) != 0) {
        invalid |= 0x1;
    }
    if (d > 5 || (d & 1) != 1) {
        invalid |= 0x2;
    }
    if (s > 5) {
        invalid |= 0x4;
    }
    if (r > 5 || (r & 1) != 1) {
        invalid |= 0x8;
    }
    if (invalid & flag) {
        // Some games pass 5/5/5/5 right at init; it fails on a PSP too.
        return kErrSasInvalidAdsrCurveMode;
    }

    sas_.voices[voiceNum].envelope.SetEnvelope(flag, a, d, s, r);
    return 0;
}

uint32_t SasCore::SetSimpleADSR(uint32_t /*core*/, int voiceNum, uint32_t ADSREnv1,
                                uint32_t ADSREnv2) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    // This bit could be related to decay type or sustain type, but gives an
    // error if you try to set it.
    if ((ADSREnv2 >> 13) & 1) {
        return kErrSasInvalidAdsrCurveMode;
    }
    sas_.voices[voiceNum].envelope.SetSimpleEnvelope(ADSREnv1 & 0xFFFF, ADSREnv2 & 0xFFFF);
    return 0;
}

uint32_t SasCore::GetEnvelopeHeight(uint32_t /*core*/, int voiceNum) const {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    return static_cast<uint32_t>(sas_.voices[voiceNum].envelope.GetHeight());
}

uint32_t SasCore::RevType(uint32_t /*core*/, int type) {
    if (type < PSP_SAS_EFFECT_TYPE_OFF || type > PSP_SAS_EFFECT_TYPE_MAX) {
        return kErrSasRevInvalidType;
    }
    sas_.SetWaveformEffectType(type);
    return 0;
}

uint32_t SasCore::RevParam(uint32_t /*core*/, int delay, int feedback) {
    if (delay < 0 || delay >= 128) {
        return kErrSasRevInvalidDelay;
    }
    if (feedback < 0 || feedback >= 128) {
        return kErrSasRevInvalidFeedback;
    }
    sas_.waveformEffect.delay = delay;
    sas_.waveformEffect.feedback = feedback;
    return 0;
}

uint32_t SasCore::RevEVOL(uint32_t /*core*/, uint32_t lv, uint32_t rv) {
    if (lv > 0x1000 || rv > 0x1000) {
        return kErrSasRevInvalidVolume;
    }
    sas_.waveformEffect.leftVol = static_cast<int>(lv);
    sas_.waveformEffect.rightVol = static_cast<int>(rv);
    return 0;
}

uint32_t SasCore::RevVON(uint32_t /*core*/, int dry, int wet) {
    sas_.waveformEffect.isDryOn = dry != 0;
    sas_.waveformEffect.isWetOn = wet != 0;
    return 0;
}

uint32_t SasCore::GetGrain(uint32_t /*core*/) const {
    return static_cast<uint32_t>(sas_.GetGrainSize());
}

uint32_t SasCore::SetGrain(uint32_t /*core*/, int grain) {
    // Upstream does not validate this one.
    sas_.SetGrainSize(grain);
    return 0;
}

uint32_t SasCore::GetOutputMode(uint32_t /*core*/) const {
    return static_cast<uint32_t>(sas_.outputMode);
}

uint32_t SasCore::SetOutputMode(uint32_t /*core*/, uint32_t outputMode) {
    if (outputMode != 0 && outputMode != 1) {
        return kErrSasInvalidOutputMode;
    }
    sas_.outputMode = static_cast<int>(outputMode);
    return 0;
}

uint32_t SasCore::GetAllEnvelopeHeights(const GuestMemory &mem, uint32_t /*core*/,
                                        uint32_t heightsAddr) {
    if (!mem.IsValidAddress(heightsAddr)) {
        return kErrSasInvalidParameter;
    }
    for (int i = 0; i < PSP_SAS_VOICES_MAX; i++) {
        const int voiceHeight = sas_.voices[i].envelope.GetHeight();
        mem.Write32(heightsAddr + static_cast<uint32_t>(i) * 4, static_cast<uint32_t>(voiceHeight));
    }
    return 0;
}

uint32_t SasCore::SetTriangularWave(uint32_t /*core*/, int /*voice*/, int /*unknown*/) {
    return 0;
}

uint32_t SasCore::SetSteepWave(uint32_t /*core*/, int /*voice*/, int /*unknown*/) {
    return 0;
}

uint32_t SasCore::SetVoiceATRAC3(const GuestMemory &mem, uint32_t core, int voiceNum,
                                 uint32_t atrac3Context) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    SasVoice &v = sas_.voices[voiceNum];
    if (v.type == VOICETYPE_ATRAC3) {
        return kErrSasAtrac3AlreadySet;
    }
    v.type = VOICETYPE_ATRAC3;
    v.loop = false;
    v.playing = true;
    v.atrac3.SetContext(atrac_bridge_, atrac3Context);
    mem.Write32(core + 56 * static_cast<uint32_t>(voiceNum) + 20, atrac3Context);
    return 0;
}

uint32_t SasCore::ConcatenateATRAC3(uint32_t /*core*/, int voiceNum, uint32_t atrac3DataAddr,
                                    int atrac3DataLength) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    sas_.voices[voiceNum].atrac3.Concatenate(atrac3DataAddr,
                                             static_cast<uint32_t>(atrac3DataLength));
    return 0;
}

uint32_t SasCore::UnsetATRAC3(const GuestMemory &mem, uint32_t core, int voiceNum) {
    if (BadVoice(voiceNum)) {
        return kErrSasInvalidVoice;
    }
    SasVoice &v = sas_.voices[voiceNum];
    if (v.type != VOICETYPE_ATRAC3) {
        return kErrSasAtrac3NotSet;
    }
    v.type = VOICETYPE_OFF;
    v.playing = false;
    v.on = false;
    // This unpauses. Some games, like Sol Trigger, depend on this.
    v.paused = false;
    mem.Write32(core + 56 * static_cast<uint32_t>(voiceNum) + 20, 0);
    return 0;
}

} // namespace mpo::audio
