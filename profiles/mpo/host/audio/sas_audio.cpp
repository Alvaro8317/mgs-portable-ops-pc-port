// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/SasAudio.cpp @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// See sas_audio.h for the list of changes from upstream. The mixing arithmetic
// (VAG decoding, ADSR curves, resampling, envelope and volume scaling, reverb
// send) is unchanged, so the output matches PPSSPP sample for sample; the test
// mpo_audio_ppsspp_difftest checks that against the upstream code.

#include "sas_audio.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace mpo::audio {
namespace {

const uint8_t f[16][2] = {
    {0, 0},
    {60, 0},
    {115, 52},
    {98, 55},
    {122, 60},
    // TODO(upstream): The below values could use more testing, but match
    // initial tests. Not sure if they are used by games, found by tests.
    {0, 0},
    {0, 0},
    {52, 0},
    {55, 2},
    {60, 125},
    {0, 0},
    {0, 91},
    {0, 0},
    {2, 216},
    {125, 6},
    {0, 151},
};

// http://code.google.com/p/jpcsp/source/browse/trunk/src/jpcsp/HLE/modules150/sceSasCore.java

int simpleRate(int n) {
    n &= 0x7F;
    if (n == 0x7F) {
        return 0;
    }
    int rate = ((7 - (n & 0x3)) << 26) >> (n >> 2);
    if (rate == 0) {
        return 1;
    }
    return rate;
}

int exponentRate(int n) {
    n &= 0x7F;
    if (n == 0x7F) {
        return 0;
    }
    int rate = ((7 - (n & 0x3)) << 24) >> (n >> 2);
    if (rate == 0) {
        return 1;
    }
    return rate;
}

int getAttackRate(int bitfield1) {
    return simpleRate(bitfield1 >> 8);
}

int getAttackType(int bitfield1) {
    return (bitfield1 & 0x8000) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE
                                     : PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT;
}

int getDecayRate(int bitfield1) {
    int n = (bitfield1 >> 4) & 0x000F;
    if (n == 0) {
        return 0x7FFFFFFF;
    }
    return static_cast<int>(0x80000000u >> n);
}

int getSustainType(int bitfield2) {
    return (bitfield2 >> 14) & 3;
}

int getSustainRate(int bitfield2) {
    if (getSustainType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE) {
        return exponentRate(bitfield2 >> 6);
    }
    return simpleRate(bitfield2 >> 6);
}

int getReleaseType(int bitfield2) {
    return (bitfield2 & 0x0020) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE
                                     : PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
}

int getReleaseRate(int bitfield2) {
    int n = bitfield2 & 0x001F;
    if (n == 31) {
        return 0;
    }
    if (getReleaseType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE) {
        if (n == 30) {
            return 0x40000000;
        } else if (n == 29) {
            return 1;
        }
        return 0x10000000 >> n;
    }
    if (n == 0) {
        return 0x7FFFFFFF;
    }
    return static_cast<int>(0x80000000u >> n);
}

int getSustainLevel(int bitfield1) {
    return ((bitfield1 & 0x000F) + 1) << 26;
}

} // namespace

// ---------------------------------------------------------------- VagDecoder

void VagDecoder::Start(uint32_t data, uint32_t vagSize, bool loopEnabled) {
    loopEnabled_ = loopEnabled;
    loopAtNextBlock_ = false;
    loopStartBlock_ = -1;
    numBlocks_ = static_cast<int>(vagSize / 16);
    end_ = false;
    data_ = data;
    read_ = data;
    curSample = 28;
    curBlock_ = -1;
    s_1 = 0; // per block?
    s_2 = 0;
}

void VagDecoder::DecodeBlock(const uint8_t *&read_pointer) {
    if (curBlock_ == numBlocks_ - 1) {
        end_ = true;
        return;
    }

    const uint8_t *readp = read_pointer;
    int predict_nr = *readp++;
    int shift_factor = predict_nr & 0xf;
    predict_nr >>= 4;
    int flags = *readp++;
    if (flags == 7) {
        end_ = true;
        return;
    } else if (flags == 6) {
        loopStartBlock_ = curBlock_;
    } else if (flags == 3) {
        if (loopEnabled_) {
            loopAtNextBlock_ = true;
        }
    }

    // Keep state in locals to avoid bouncing to memory.
    int s1 = s_1;
    int s2 = s_2;

    int coef1 = f[predict_nr][0];
    int coef2 = -f[predict_nr][1];

    for (int i = 0; i < 28; i += 2) {
        uint8_t d = *readp++;
        int sample1 = static_cast<int16_t>((d & 0xf) << 12) >> shift_factor;
        int sample2 = static_cast<int16_t>((d & 0xf0) << 8) >> shift_factor;
        s2 = ClampS16(sample1 + ((s1 * coef1 + s2 * coef2) >> 6));
        s1 = ClampS16(sample2 + ((s2 * coef1 + s1 * coef2) >> 6));
        samples[i] = static_cast<int16_t>(s2);
        samples[i + 1] = static_cast<int16_t>(s1);
    }

    s_1 = s1;
    s_2 = s2;
    curSample = 0;
    curBlock_++;

    read_pointer = readp;
}

void VagDecoder::GetSamples(const GuestMemory &mem, int16_t *outSamples, int numSamples) {
    if (end_) {
        std::memset(outSamples, 0, static_cast<size_t>(numSamples) * sizeof(int16_t));
        return;
    }
    const uint8_t *base = mem.Ptr(read_, static_cast<uint32_t>(numBlocks_) * 16);
    if (base == nullptr) {
        // Upstream: "Bad VAG samples address?" - leaves the output untouched.
        return;
    }

    const uint8_t *readp = base;
    const uint8_t *origp = readp;

    for (int i = 0; i < numSamples; i++) {
        if (curSample == 28) {
            if (loopAtNextBlock_) {
                // data_ starts at curBlock = -1.
                read_ = data_ + 16 * static_cast<uint32_t>(loopStartBlock_) + 16;
                readp = mem.Ptr(read_, 16);
                if (readp == nullptr) {
                    // Cannot happen with a valid VAG (the loop start lies
                    // before the current block); upstream would read garbage.
                    end_ = true;
                    std::memset(&outSamples[i], 0,
                                static_cast<size_t>(numSamples - i) * sizeof(int16_t));
                    return;
                }
                origp = readp;
                curBlock_ = loopStartBlock_;
                loopAtNextBlock_ = false;
            }
            DecodeBlock(readp);
            if (end_) {
                // Clear the rest of the buffer and return.
                std::memset(&outSamples[i], 0,
                            static_cast<size_t>(numSamples - i) * sizeof(int16_t));
                return;
            }
        }
        outSamples[i] = samples[curSample++];
    }

    if (readp > origp) {
        read_ += static_cast<uint32_t>(readp - origp);
    }
}

// ---------------------------------------------------------------- SampleQueue

bool SampleQueue::push(const uint8_t *data, int bytes) {
    const int capacity = static_cast<int>(buf_.size());
    if (bytes < 0 || capacity - filled_ < bytes) {
        return false;
    }
    for (int i = 0; i < bytes; ++i) {
        buf_[static_cast<size_t>(end_)] = data[i];
        end_ = (end_ + 1) % capacity;
    }
    filled_ += bytes;
    return true;
}

int SampleQueue::pop_front(uint8_t *out, int bytes) {
    if (bytes <= 0) {
        return 0;
    }
    const int capacity = static_cast<int>(buf_.size());
    const int got = std::min(bytes, filled_);
    for (int i = 0; i < got; ++i) {
        if (out != nullptr) {
            out[i] = buf_[static_cast<size_t>(start_)];
        }
        start_ = (start_ + 1) % capacity;
    }
    filled_ -= got;
    return got;
}

// ---------------------------------------------------------------- SasAtrac3

int SasAtrac3::SetContext(SasAtracBridge *bridge, uint32_t contextAddr) {
    bridge_ = bridge;
    contextAddr_ = contextAddr;
    // Note: On hardware, atracID_ is also stored in the loopNum member of the
    // context. But we don't actually mirror our struct to memory.
    atracID_ = bridge_ != nullptr ? bridge_->SasBindContextAndGetId(contextAddr) : -1;
    sampleQueue_.clear();
    end_ = false;
    return 0;
}

void SasAtrac3::GetNextSamples(int16_t *outbuf, int wantedSamples) {
    if (atracID_ < 0 || bridge_ == nullptr) {
        end_ = true;
        return;
    }

    if (buf_.empty()) {
        buf_.assign(0x800, 0);
    }

    int finish = 0;
    const int wantedbytes = wantedSamples * static_cast<int>(sizeof(int16_t));
    while (!finish && sampleQueue_.size() < wantedbytes) {
        int numSamples = 0;
        bridge_->SasDecodeData(atracID_, buf_.data(), &numSamples, &finish);
        if (numSamples > 0) {
            sampleQueue_.push(reinterpret_cast<const uint8_t *>(buf_.data()),
                              numSamples * static_cast<int>(sizeof(int16_t)));
        } else {
            finish = 1;
        }
    }
    sampleQueue_.pop_front(reinterpret_cast<uint8_t *>(outbuf), wantedbytes);
    end_ = finish == 1;
}

int SasAtrac3::Concatenate(uint32_t bufPtr, uint32_t addbytes) {
    if (atracID_ >= 0 && bridge_ != nullptr) {
        bridge_->SasAddStreamData(atracID_, bufPtr, addbytes);
    }
    return 0;
}

// ---------------------------------------------------------------- ADSREnvelope

void ADSREnvelope::SetEnvelope(int flag, int a, int d, int s, int r) {
    if ((flag & 0x1) != 0) {
        attackType = static_cast<SasADSRCurveMode>(a);
    }
    if ((flag & 0x2) != 0) {
        decayType = static_cast<SasADSRCurveMode>(d);
    }
    if ((flag & 0x4) != 0) {
        sustainType = static_cast<SasADSRCurveMode>(s);
    }
    if ((flag & 0x8) != 0) {
        releaseType = static_cast<SasADSRCurveMode>(r);
    }
}

void ADSREnvelope::SetRate(int flag, int a, int d, int s, int r) {
    if ((flag & 0x1) != 0) {
        attackRate = a;
    }
    if ((flag & 0x2) != 0) {
        decayRate = d;
    }
    if ((flag & 0x4) != 0) {
        sustainRate = s;
    }
    if ((flag & 0x8) != 0) {
        releaseRate = r;
    }
}

void ADSREnvelope::SetSimpleEnvelope(uint32_t ADSREnv1, uint32_t ADSREnv2) {
    const int env1 = static_cast<int>(ADSREnv1);
    const int env2 = static_cast<int>(ADSREnv2);
    attackRate = getAttackRate(env1);
    attackType = static_cast<SasADSRCurveMode>(getAttackType(env1));
    decayRate = getDecayRate(env1);
    decayType = PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
    sustainRate = getSustainRate(env2);
    sustainType = static_cast<SasADSRCurveMode>(getSustainType(env2));
    releaseRate = getReleaseRate(env2);
    releaseType = static_cast<SasADSRCurveMode>(getReleaseType(env2));
    sustainLevel = getSustainLevel(env1);
}

void ADSREnvelope::WalkCurve(int type, int rate) {
    int64_t expDelta;
    switch (type) {
    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE:
        height_ += rate;
        break;

    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE:
        height_ -= rate;
        break;

    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT:
        if (height_ <= static_cast<int64_t>(PSP_SAS_ENVELOPE_HEIGHT_MAX) * 3 / 4) {
            height_ += rate;
        } else {
            height_ += rate / 4;
        }
        break;

    case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE:
        expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
        // Flipping the sign so that we can shift in the top bits.
        expDelta += (-expDelta * rate) >> 32;
        // Upstream: `- (rate + 3UL) / 4UL`; unsigned long is 64-bit on LP64,
        // 32-bit on Windows. Rates are non-negative ints below 2^31, so both
        // give the same value; do it in 64-bit unsigned like Linux PPSSPP.
        height_ =
            expDelta + PSP_SAS_ENVELOPE_HEIGHT_MAX -
            static_cast<int64_t>((static_cast<uint64_t>(static_cast<int64_t>(rate)) + 3u) / 4u);
        break;

    case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE:
        expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
        // Flipping the sign so that we can shift in the top bits.
        expDelta += (-expDelta * rate) >> 32;
        height_ = expDelta + 0x4000 + PSP_SAS_ENVELOPE_HEIGHT_MAX;
        break;

    case PSP_SAS_ADSR_CURVE_MODE_DIRECT:
        height_ = rate; // Simple :)
        break;
    }
}

void ADSREnvelope::SetState(ADSRState state) {
    if (height_ > PSP_SAS_ENVELOPE_HEIGHT_MAX) {
        height_ = PSP_SAS_ENVELOPE_HEIGHT_MAX;
    }
    state_ = state;
}

void ADSREnvelope::Step() {
    switch (state_) {
    case STATE_ATTACK:
        WalkCurve(attackType, attackRate);
        if (height_ >= PSP_SAS_ENVELOPE_HEIGHT_MAX || height_ < 0) {
            SetState(STATE_DECAY);
        }
        break;
    case STATE_DECAY:
        WalkCurve(decayType, decayRate);
        if (height_ < sustainLevel) {
            SetState(STATE_SUSTAIN);
        }
        break;
    case STATE_SUSTAIN:
        WalkCurve(sustainType, sustainRate);
        if (height_ <= 0) {
            height_ = 0;
            SetState(STATE_RELEASE);
        }
        break;
    case STATE_RELEASE:
        WalkCurve(releaseType, releaseRate);
        if (height_ <= 0) {
            height_ = 0;
            SetState(STATE_OFF);
        }
        break;
    case STATE_OFF:
        break;

    case STATE_KEYON:
        height_ = 0;
        SetState(STATE_KEYON_STEP);
        break;
    case STATE_KEYON_STEP:
        // This entire state is pretty much a hack to reproduce PSP behavior.
        // It takes 32 steps at 0 for keyon to "kick in", 31 should shift to 0.
        height_++;
        if (height_ >= 31) {
            height_ = 0;
            SetState(STATE_ATTACK);
        }
        break;
    }
}

void ADSREnvelope::KeyOn() {
    SetState(STATE_KEYON);
}

void ADSREnvelope::KeyOff() {
    SetState(STATE_RELEASE);
}

void ADSREnvelope::End() {
    SetState(STATE_OFF);
    height_ = 0;
}

// ---------------------------------------------------------------- SasVoice

void SasVoice::Reset() {
    resampleHist[0] = 0;
    resampleHist[1] = 0;
}

void SasVoice::KeyOn(const GuestMemory &mem) {
    envelope.KeyOn();
    switch (type) {
    case VOICETYPE_VAG:
        if (mem.IsValidAddress(vagAddr)) {
            vag.Start(vagAddr, vagSize, loop);
        } else {
            // Upstream: "Invalid VAG address" - the voice is not started.
            return;
        }
        break;
    default:
        break;
    }
    playing = true;
    on = true;
    paused = false;
    sampleFrac = 0;
}

void SasVoice::KeyOff() {
    on = false;
    envelope.KeyOff();
}

void SasVoice::ReadSamples(const GuestMemory &mem, int16_t *output, int numSamples) {
    // Read N samples into the resample buffer. Could do either PCM or VAG here.
    switch (type) {
    case VOICETYPE_VAG:
        vag.GetSamples(mem, output, numSamples);
        break;
    case VOICETYPE_PCM: {
        int needed = numSamples;
        int16_t *out = output;
        while (needed > 0) {
            const int size = std::min(pcmSize - pcmIndex, needed);
            if (!on) {
                pcmIndex = 0;
                break;
            }
            mem.CopyFrom(out, pcmAddr + static_cast<uint32_t>(pcmIndex) * 2,
                         static_cast<uint32_t>(size) * 2);
            pcmIndex += size;
            needed -= size;
            out += size;
            if (pcmIndex >= pcmSize) {
                if (!loop) {
                    // All out, quit. We'll end in HaveSamplesEnded().
                    break;
                }
                pcmIndex = pcmLoopPos;
            }
        }
        if (needed > 0) {
            std::memset(out, 0, static_cast<size_t>(needed) * sizeof(int16_t));
        }
        break;
    }
    case VOICETYPE_ATRAC3:
        atrac3.GetNextSamples(output, numSamples);
        break;
    default:
        std::memset(output, 0, static_cast<size_t>(numSamples) * sizeof(int16_t));
        break;
    }
}

bool SasVoice::HaveSamplesEnded() const {
    switch (type) {
    case VOICETYPE_VAG:
        return vag.End();
    case VOICETYPE_PCM:
        return pcmIndex >= pcmSize;
    case VOICETYPE_ATRAC3:
        return atrac3.End();
    default:
        return false;
    }
}

// ---------------------------------------------------------------- SasInstance

void SasInstance::ClearGrainSize() {
    mixBuffer.clear();
    sendBuffer.clear();
    sendBufferDownsampled.clear();
    sendBufferProcessed.clear();
}

void SasInstance::SetGrainSize(int newGrainSize) {
    grainSize = newGrainSize;
    const size_t grain = static_cast<size_t>(std::max(grainSize, 0));
    mixBuffer.assign(grain * 2, 0);
    sendBuffer.assign(grain * 2, 0);
    sendBufferDownsampled.assign(grain, 0);
    sendBufferProcessed.assign(grain * 2, 0);
}

int SasInstance::EstimateMixUs() const {
    int voicesPlayingCount = 0;
    for (const SasVoice &voice : voices) {
        if (!voice.playing || voice.paused) {
            continue;
        }
        voicesPlayingCount++;
    }

    // Each voice costs extra time, and each byte of grain costs extra time.
    int cycles = 20 + voicesPlayingCount * 68 + (grainSize * 60) / 100;
    // Cap to 1200 to fix FFT, see PPSSPP issue #9956.
    return std::min(cycles, 1200);
}

void SasInstance::MixVoice(const GuestMemory &mem, SasVoice &voice) {
    switch (voice.type) {
    case VOICETYPE_VAG:
        if (voice.type == VOICETYPE_VAG && !voice.vagAddr) {
            break;
        }
        [[fallthrough]];
    case VOICETYPE_PCM:
        if (voice.type == VOICETYPE_PCM && !voice.pcmAddr) {
            break;
        }
        [[fallthrough]];
    default: {
        // This feels a bit hacky. The first 32 samples after a keyon are 0s.
        int delay = 0;
        if (voice.envelope.NeedsKeyOn()) {
            const bool ignorePitch =
                voice.type == VOICETYPE_PCM && voice.pitch > PSP_SAS_PITCH_BASE;
            delay = ignorePitch ? 32
                                : static_cast<int>((32 * static_cast<uint32_t>(voice.pitch)) >>
                                                   PSP_SAS_PITCH_BASE_SHIFT);
            // VAG seems to have an extra sample delay (not shared by PCM.)
            if (voice.type == VOICETYPE_VAG) {
                ++delay;
            }
        }

        // Resample to the correct pitch, writing exactly "grainSize" samples.

        // Two passes: First read, then resample.
        mixTemp_[0] = voice.resampleHist[0];
        mixTemp_[1] = voice.resampleHist[1];

        const int voicePitch = voice.pitch;
        uint32_t sampleFrac = voice.sampleFrac;
        int samplesToRead = static_cast<int>(
            (sampleFrac + static_cast<uint32_t>(voicePitch * std::max(0, grainSize - delay))) >>
            PSP_SAS_PITCH_BASE_SHIFT);
        const int tempCapacity = static_cast<int>(std::size(mixTemp_)) - 2;
        if (samplesToRead > tempCapacity) {
            // Upstream: "Too many samples to read! This shouldn't happen."
            samplesToRead = tempCapacity;
        }
        int readPos = 2;
        if (voice.envelope.NeedsKeyOn()) {
            readPos = 0;
            samplesToRead += 2;
        }
        voice.ReadSamples(mem, &mixTemp_[readPos], samplesToRead);
        const int tempPos = readPos + samplesToRead;

        for (int i = 0; i < delay; ++i) {
            // Walk the curve. This means we'll reach ATTACK already, likely.
            voice.envelope.Step();
        }

        const bool needsInterp =
            voicePitch != PSP_SAS_PITCH_BASE || (sampleFrac & PSP_SAS_PITCH_MASK) != 0;
        for (int i = delay; i < grainSize; i++) {
            const int16_t *s = mixTemp_ + (sampleFrac >> PSP_SAS_PITCH_BASE_SHIFT);

            // Linear interpolation. Good enough.
            int sample = s[0];
            if (needsInterp) {
                const int frac = static_cast<int>(sampleFrac & PSP_SAS_PITCH_MASK);
                sample =
                    (s[0] * (PSP_SAS_PITCH_MASK - frac) + s[1] * frac) >> PSP_SAS_PITCH_BASE_SHIFT;
            }
            sampleFrac += static_cast<uint32_t>(voicePitch);

            // The maximum envelope height (PSP_SAS_ENVELOPE_HEIGHT_MAX) is
            // (1 << 30) - 1. Reduce it to 14 bits, by shifting off 15. Round
            // up by adding (1 << 14) first.
            int envelopeValue = voice.envelope.GetHeight();
            voice.envelope.Step();
            envelopeValue = (envelopeValue + (1 << 14)) >> 15;

            // We just scale by the envelope before we scale by volumes.
            sample = ((sample * envelopeValue) + (1 << 14)) >> 15;

            const size_t idx = static_cast<size_t>(i) * 2;
            mixBuffer[idx] += (sample * voice.volumeLeft) >> 12;
            mixBuffer[idx + 1] += (sample * voice.volumeRight) >> 12;
            sendBuffer[idx] += sample * voice.effectLeft >> 12;
            sendBuffer[idx + 1] += sample * voice.effectRight >> 12;
        }

        voice.resampleHist[0] = mixTemp_[tempPos - 2];
        voice.resampleHist[1] = mixTemp_[tempPos - 1];

        voice.sampleFrac = sampleFrac - static_cast<uint32_t>(tempPos - 2) * PSP_SAS_PITCH_BASE;

        if (voice.HaveSamplesEnded()) {
            voice.envelope.End();
        }
        if (voice.envelope.HasEnded()) {
            voice.playing = false;
            voice.on = false;
        }
    }
    }
}

void SasInstance::Mix(const GuestMemory &mem, uint32_t outAddr, uint32_t inAddr, int leftVol,
                      int rightVol, bool mute) {
    if (grainSize <= 0 || mixBuffer.empty()) {
        return;
    }
    for (SasVoice &voice : voices) {
        if (!voice.playing || voice.paused) {
            continue;
        }
        MixVoice(mem, voice);
    }

    const size_t grain = static_cast<size_t>(grainSize);
    // Apply mute if needed (keep everything else identical to the non-muted case).
    if (mute) {
        std::fill(mixBuffer.begin(), mixBuffer.end(), 0);
        std::fill(sendBuffer.begin(), sendBuffer.end(), 0);
    }

    // Convert and clip, and at the same time, wipe mixBuffer for next time.
    auto *outp = reinterpret_cast<int16_t *>(mem.Ptr(outAddr, static_cast<uint32_t>(4 * grain)));
    const auto *inp =
        inAddr
            ? reinterpret_cast<const int16_t *>(mem.Ptr(inAddr, static_cast<uint32_t>(4 * grain)))
            : nullptr;
    if (outp == nullptr) {
        // Upstream: "Bad SAS Mix output address" - nothing is written.
    } else if (outputMode == PSP_SAS_OUTPUTMODE_MIXED) {
        // Okay, apply effects processing to the Send buffer.
        WriteMixedOutput(outp, inp, leftVol, rightVol);
    } else {
        // Raw mode needs 4 planes; upstream only checks the stereo size.
        auto *raw = reinterpret_cast<int16_t *>(mem.Ptr(outAddr, static_cast<uint32_t>(8 * grain)));
        if (raw != nullptr) {
            int16_t *outpL = raw + grain * 0;
            int16_t *outpR = raw + grain * 1;
            int16_t *outpSendL = raw + grain * 2;
            int16_t *outpSendR = raw + grain * 3;
            for (size_t i = 0; i < grain * 2; i += 2) {
                *outpL++ = ClampS16(mixBuffer[i + 0]);
                *outpR++ = ClampS16(mixBuffer[i + 1]);
                *outpSendL++ = ClampS16(sendBuffer[i + 0]);
                *outpSendR++ = ClampS16(sendBuffer[i + 1]);
            }
        }
    }
    std::fill(mixBuffer.begin(), mixBuffer.end(), 0);
    std::fill(sendBuffer.begin(), sendBuffer.end(), 0);
}

// Note: leftVol/rightVol here are how much to scale the inp content by, not
// the mixBuffer.
void SasInstance::WriteMixedOutput(int16_t *outp, const int16_t *inp, int leftVol, int rightVol) {
    const bool dry = waveformEffect.isDryOn != 0;
    const bool wet = waveformEffect.isWetOn != 0;
    if (wet) {
        ApplyWaveformEffect();
    }

    const size_t n = static_cast<size_t>(grainSize) * 2;
    if (inp) {
        for (size_t i = 0; i < n; i += 2) {
            int sampleL = ((*inp++) * leftVol >> 12);
            int sampleR = ((*inp++) * rightVol >> 12);
            if (dry) {
                sampleL += mixBuffer[i + 0];
                sampleR += mixBuffer[i + 1];
            }
            if (wet) {
                sampleL += sendBufferProcessed[i + 0];
                sampleR += sendBufferProcessed[i + 1];
            }
            *outp++ = ClampS16(sampleL);
            *outp++ = ClampS16(sampleR);
        }
    } else {
        for (size_t i = 0; i < n; i += 2) {
            int sampleL = 0;
            int sampleR = 0;
            if (dry) {
                sampleL += mixBuffer[i + 0];
                sampleR += mixBuffer[i + 1];
            }
            if (wet) {
                sampleL += sendBufferProcessed[i + 0];
                sampleR += sendBufferProcessed[i + 1];
            }
            *outp++ = ClampS16(sampleL);
            *outp++ = ClampS16(sampleR);
        }
    }
}

void SasInstance::SetWaveformEffectType(int type) {
    if (type != waveformEffect.type) {
        waveformEffect.type = type;
        reverb_.SetPreset(type);
    }
}

void SasInstance::ApplyWaveformEffect() {
    // First, downsample the send buffer to 22khz. We do this naively for now.
    for (int i = 0; i < grainSize / 2; i++) {
        const size_t o = static_cast<size_t>(i) * 2;
        const size_t s = static_cast<size_t>(i) * 4;
        sendBufferDownsampled[o] = ClampS16(sendBuffer[s]);
        sendBufferDownsampled[o + 1] = ClampS16(sendBuffer[s + 1]);
    }

    // Volume max is 0x1000, while our factor is up to 0x8000. Shifting left by
    // 3 fixes that.
    reverb_.ProcessReverb(
        sendBufferProcessed.data(), sendBufferDownsampled.data(),
        static_cast<size_t>(grainSize / 2), static_cast<uint16_t>(waveformEffect.leftVol << 3),
        static_cast<uint16_t>(waveformEffect.rightVol << 3), reverbVolumeMultiplier);
}

const char *ADSRCurveModeAsString(SasADSRCurveMode mode) {
    switch (mode) {
    case PSP_SAS_ADSR_CURVE_MODE_DIRECT:
        return "D";
    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE:
        return "L+";
    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE:
        return "L-";
    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT:
        return "LB";
    case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE:
        return "E-";
    case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE:
        return "E+";
    default:
        return "N/A";
    }
}

} // namespace mpo::audio
