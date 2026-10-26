// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/SasAudio.h @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// SAS is a software mixer that runs on the PSP's Media Engine. This is the
// mixer itself (voices, VAG decoding, ADSR envelopes, pitch and the reverb
// send). The sceSasCore API on top of it is in sas_core.h.
//
// Changes from upstream: PSP memory goes through GuestMemory; ATRAC3 voices
// reach sceAtrac through SasAtracBridge instead of globals; no savestates,
// debugger text or audio dumping; the RockmanDash2SoundFix compat flag is not
// carried over (MPO does not need it).

#pragma once

#include <cstdint>
#include <vector>

#include "guest_memory.h"
#include "sas_reverb.h"

namespace mpo::audio {

// General constants.
enum {
    PSP_SAS_VOICES_MAX = 32,
    PSP_SAS_VOL_MAX = 0x1000,
    PSP_SAS_MAX_GRAIN = 2048, // Matches the max value of the parameter to sceSasInit
    PSP_SAS_PITCH_MIN = 0x0000,
    PSP_SAS_PITCH_BASE = 0x1000,
    PSP_SAS_PITCH_MASK = 0xFFF,
    PSP_SAS_PITCH_BASE_SHIFT = 12,
    PSP_SAS_PITCH_MAX = 0x4000,
    PSP_SAS_ENVELOPE_HEIGHT_MAX = 0x40000000,
    PSP_SAS_ENVELOPE_FREQ_MAX = 0x7FFFFFFF,
};

enum SasADSRCurveMode : int {
    PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE = 0,
    PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE = 1,
    PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT = 2,
    PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE = 3,
    PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE = 4,
    PSP_SAS_ADSR_CURVE_MODE_DIRECT = 5,
};

enum {
    PSP_SAS_ADSR_ATTACK = 1,
    PSP_SAS_ADSR_DECAY = 2,
    PSP_SAS_ADSR_SUSTAIN = 4,
    PSP_SAS_ADSR_RELEASE = 8,
};

enum SasEffectType {
    PSP_SAS_EFFECT_TYPE_OFF = -1,
    PSP_SAS_EFFECT_TYPE_ROOM = 0,
    PSP_SAS_EFFECT_TYPE_STUDIO_SMALL = 1,
    PSP_SAS_EFFECT_TYPE_STUDIO_MEDIUM = 2,
    PSP_SAS_EFFECT_TYPE_STUDIO_LARGE = 3,
    PSP_SAS_EFFECT_TYPE_HALL = 4,
    PSP_SAS_EFFECT_TYPE_SPACE = 5,
    PSP_SAS_EFFECT_TYPE_ECHO = 6,
    PSP_SAS_EFFECT_TYPE_DELAY = 7,
    PSP_SAS_EFFECT_TYPE_PIPE = 8,
    PSP_SAS_EFFECT_TYPE_MAX = 8,
};

enum SasOutputMode {
    PSP_SAS_OUTPUTMODE_MIXED = 0,
    PSP_SAS_OUTPUTMODE_RAW = 1,
};

struct WaveformEffect {
    int type = PSP_SAS_EFFECT_TYPE_OFF;
    int delay = 0;
    int feedback = 0;
    int leftVol = 0;
    int rightVol = 0;
    int isDryOn = 1;
    int isWetOn = 0;
};

enum VoiceType {
    VOICETYPE_OFF,
    VOICETYPE_VAG, // default
    VOICETYPE_NOISE,
    VOICETYPE_TRIWAVE,
    VOICETYPE_PULSEWAVE,
    VOICETYPE_PCM,
    VOICETYPE_ATRAC3,
};

// How an ATRAC3 SAS voice talks to sceAtrac (implemented by AtracManager).
class SasAtracBridge {
public:
    virtual ~SasAtracBridge() = default;
    // Returns the atrac ID whose context lives at `contextAddr`, or -1.
    virtual int SasBindContextAndGetId(uint32_t contextAddr) = 0;
    // Decodes one frame of mono samples for SAS.
    virtual void SasDecodeData(int atracId, int16_t *out, int *numSamples, int *finish) = 0;
    // __sceSasConcatenateATRAC3.
    virtual uint32_t SasAddStreamData(int atracId, uint32_t bufPtr, uint32_t bytesToAdd) = 0;
};

// VAG is a Sony ADPCM audio compression format, which goes all the way back to
// the PSX. It compresses 28 16-bit samples into a block of 16 bytes.
class VagDecoder {
public:
    void Start(uint32_t dataPtr, uint32_t vagSize, bool loopEnabled);

    void GetSamples(const GuestMemory &mem, int16_t *outSamples, int numSamples);

    void DecodeBlock(const uint8_t *&readp);
    bool End() const { return end_; }

    uint32_t GetReadPtr() const { return read_; }

private:
    int16_t samples[28]{};
    int curSample = 0;

    uint32_t data_ = 0;
    uint32_t read_ = 0;
    int curBlock_ = -1;
    int loopStartBlock_ = -1;
    int numBlocks_ = 0;

    // rolling state. start at 0, should probably reset to 0 on loops?
    int s_1 = 0;
    int s_2 = 0;

    bool loopEnabled_ = false;
    bool loopAtNextBlock_ = false;
    bool end_ = true;
};

// Small byte FIFO (upstream uses Core/HW/BufferQueue.h, without the PTS part).
class SampleQueue {
public:
    explicit SampleQueue(int size = 0x20000) : buf_(static_cast<size_t>(size)) {}
    void clear() { start_ = end_ = filled_ = 0; }
    int size() const { return filled_; }
    bool push(const uint8_t *data, int bytes);
    int pop_front(uint8_t *out, int bytes);

private:
    std::vector<uint8_t> buf_;
    int start_ = 0;
    int end_ = 0;
    int filled_ = 0;
};

class SasAtrac3 {
public:
    int AtracID() const { return atracID_; }
    int SetContext(SasAtracBridge *bridge, uint32_t context);
    void GetNextSamples(int16_t *outbuf, int wantedSamples);
    int Concatenate(uint32_t bufPtr, uint32_t addbytes);
    bool End() const { return end_; }

private:
    SasAtracBridge *bridge_ = nullptr;
    uint32_t contextAddr_ = 0;
    int atracID_ = -1;
    SampleQueue sampleQueue_;
    bool end_ = false;
    std::vector<int16_t> buf_;
};

class ADSREnvelope {
public:
    void SetSimpleEnvelope(uint32_t ADSREnv1, uint32_t ADSREnv2);
    void SetEnvelope(int flag, int a, int d, int s, int r);
    void SetRate(int flag, int a, int d, int s, int r);
    void SetSustainLevel(int sl) { sustainLevel = sl; }

    void WalkCurve(int type, int rate);

    void KeyOn();
    void KeyOff();
    void End();

    void Step();

    int GetHeight() const {
        constexpr int64_t kMax = PSP_SAS_ENVELOPE_HEIGHT_MAX;
        return static_cast<int>(height_ > kMax ? kMax : height_);
    }
    bool NeedsKeyOn() const { return state_ == STATE_KEYON; }
    bool HasEnded() const { return state_ == STATE_OFF; }

    int attackRate = 0;
    int decayRate = 0;
    int sustainRate = 0;
    int sustainLevel = 0;
    int releaseRate = 0;

    SasADSRCurveMode attackType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE;
    SasADSRCurveMode decayType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
    SasADSRCurveMode sustainType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
    SasADSRCurveMode releaseType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;

private:
    // Actual PSP values.
    enum ADSRState {
        // Okay, this one isn't a real value but it might be.
        STATE_KEYON_STEP = -42,

        STATE_KEYON = -2,
        STATE_OFF = -1,
        STATE_ATTACK = 0,
        STATE_DECAY = 1,
        STATE_SUSTAIN = 2,
        STATE_RELEASE = 3,
    };
    void SetState(ADSRState state);

    ADSRState state_ = STATE_OFF;
    int64_t height_ = 0; // s64 to avoid having to care about overflow when calculating.
};

// A SAS voice.
struct SasVoice {
    void Reset();
    void KeyOn(const GuestMemory &mem);
    void KeyOff();

    void ReadSamples(const GuestMemory &mem, int16_t *output, int numSamples);
    bool HaveSamplesEnded() const;

    uint32_t GetReadAddress() const { return type == VOICETYPE_VAG ? vag.GetReadPtr() : 0; }

    bool playing = false;
    bool paused = false; // a voice can be playing AND paused. In that case, it won't play.
    bool on = false;     // key-on, key-off.

    VoiceType type = VOICETYPE_OFF;

    uint32_t vagAddr = 0;
    uint32_t vagSize = 0;
    uint32_t pcmAddr = 0;
    int pcmSize = 0;
    int pcmIndex = 0;
    int pcmLoopPos = 0;
    int sampleRate = 44100;

    uint32_t sampleFrac = 0;
    int pitch = PSP_SAS_PITCH_BASE;
    bool loop = false;

    int noiseFreq = 0;

    int volumeLeft = PSP_SAS_VOL_MAX;
    int volumeRight = PSP_SAS_VOL_MAX;

    // volume to "Send" (audio-lingo) to the effects processing engine, like reverb
    int effectLeft = PSP_SAS_VOL_MAX;
    int effectRight = PSP_SAS_VOL_MAX;
    int16_t resampleHist[2]{};

    ADSREnvelope envelope;

    VagDecoder vag;
    SasAtrac3 atrac3;
};

class SasInstance {
public:
    SasInstance() = default;

    void ClearGrainSize();
    void SetGrainSize(int newGrainSize);
    int GetGrainSize() const { return grainSize; }
    // Rough cost of one mix in microseconds (for the HLE delay in F16b).
    int EstimateMixUs() const;

    int maxVoices = PSP_SAS_VOICES_MAX;
    int sampleRate = 44100;
    int outputMode = PSP_SAS_OUTPUTMODE_MIXED;

    std::vector<int32_t> mixBuffer;
    std::vector<int32_t> sendBuffer;
    std::vector<int16_t> sendBufferDownsampled;
    std::vector<int16_t> sendBufferProcessed;

    // Mixes one grain into outAddr (stereo s16, or 4 planes in raw mode).
    // inAddr != 0 is __sceSasCoreWithMix: that buffer is scaled and added.
    void Mix(const GuestMemory &mem, uint32_t outAddr, uint32_t inAddr, int leftVol, int rightVol,
             bool mute);
    void MixVoice(const GuestMemory &mem, SasVoice &voice);

    // Applies reverb to send buffer, according to waveformEffect.
    void ApplyWaveformEffect();
    void SetWaveformEffectType(int type);
    void WriteMixedOutput(int16_t *outp, const int16_t *inp, int leftVol, int rightVol);

    SasVoice voices[PSP_SAS_VOICES_MAX];
    WaveformEffect waveformEffect;

    // PPSSPP's "reverb volume" setting; 1.0 is its default.
    float reverbVolumeMultiplier = 1.0f;

private:
    SasReverb reverb_;
    int grainSize = 0;
    int16_t mixTemp_[PSP_SAS_MAX_GRAIN * 4 + 2 + 16]{}; // extra margin for very high pitches.
};

const char *ADSRCurveModeAsString(SasADSRCurveMode mode);

} // namespace mpo::audio
