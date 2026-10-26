// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seeded random scenarios for the SAS mixer and the sceAtrac context, written
// as templates so the same scenario can drive either mpo_audio or PPSSPP's own
// code (the optional mpo_audio_ppsspp_difftest). Each scenario returns a trace:
// one hash per step (outputs, envelope heights, return codes, context bytes).
// audio_sas_test and audio_atrac_test pin the final hashes of a few seeds;
// those golden values come from running PPSSPP v1.20.4's code through the
// difftest harness.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio_test_util.h"

namespace audio_test {

// ---------------------------------------------------------------- memory map

inline constexpr uint32_t kOutAddr = 0x08800000;     // mixer output (up to 4 * 2048 * 2 bytes)
inline constexpr uint32_t kInAddr = 0x08804000;      // CoreWithMix input
inline constexpr uint32_t kDataAddr = 0x08810000;    // VAG / PCM sample data
inline constexpr uint32_t kCtxAddr = 0x08808000;     // sceAtrac context (256 bytes)
inline constexpr uint32_t kAtracBuf = 0x08900000;    // sceAtrac main buffer
inline constexpr uint32_t kAtracBuf2 = 0x08A00000;   // sceAtrac second buffer
inline constexpr uint32_t kAtracOut = 0x08B00000;    // sceAtrac PCM output
inline constexpr uint32_t kScenarioRam = 0x00400000; // bytes of fake RAM needed

// ---------------------------------------------------------------- SAS

struct SasTrace {
    std::vector<uint64_t> steps;
    uint64_t Final() const { return Fnv1a(steps.data(), steps.size() * sizeof(uint64_t)); }
};

// Adapter requirements (see audio_sas_test.cpp / audio_ppsspp_difftest.cpp):
//   A::Instance &sas();           SasInstance with voices[], waveformEffect...
//   void KeyOn(int voice);        SasVoice::KeyOn()
//   void Mix(uint32_t out, uint32_t in, int l, int r);
//   uint8_t *Ram(uint32_t addr);  host pointer into the fake RAM
template <typename A>
SasTrace RunSasScenario(A &a, uint32_t seed, int grains = 48) {
    Rng rng(seed);
    SasTrace trace;
    auto &sas = a.sas();

    static constexpr int kGrains[] = {64, 128, 256, 512, 1024, 2048};
    const int grain = kGrains[rng.Range(0, 5)];
    sas.SetGrainSize(grain);
    sas.outputMode = rng.Range(0, 9) == 0 ? 1 : 0; // raw mode now and then

    // Sample data: VAG blocks and PCM, written once.
    uint32_t dataPos = kDataAddr;
    const int numVoices = rng.Range(1, 10);
    for (int v = 0; v < numVoices; ++v) {
        const int idx = rng.Range(0, 31);
        auto &voice = sas.voices[idx];
        const bool pcm = rng.Range(0, 3) == 0;
        if (pcm) {
            const int samples = rng.Range(16, 3000);
            for (int i = 0; i < samples; ++i) {
                const int16_t s = static_cast<int16_t>(rng.Next());
                std::memcpy(a.Ram(dataPos + static_cast<uint32_t>(i) * 2), &s, 2);
            }
            voice.type = static_cast<decltype(voice.type)>(5); // VOICETYPE_PCM
            voice.pcmAddr = dataPos;
            voice.pcmSize = samples;
            voice.pcmIndex = 0;
            const int loopPos = rng.Range(-1, samples - 1);
            voice.pcmLoopPos = loopPos >= 0 ? loopPos : 0;
            voice.loop = loopPos >= 0;
            voice.playing = true;
            dataPos += static_cast<uint32_t>(samples) * 2 + 16;
        } else {
            const int blocks = rng.Range(2, 400);
            const int loopStart = rng.Range(0, blocks - 1);
            for (int b = 0; b < blocks; ++b) {
                uint8_t *blk = a.Ram(dataPos + static_cast<uint32_t>(b) * 16);
                const int predict = rng.Range(0, 15);
                const int shift = rng.Range(0, 12);
                blk[0] = static_cast<uint8_t>((predict << 4) | shift);
                uint8_t flags = 0;
                if (b == loopStart) {
                    flags = 6;
                } else if (b == blocks - 1) {
                    flags = rng.Range(0, 1) ? 3 : 7;
                } else if (rng.Range(0, 60) == 0) {
                    flags = 7;
                }
                blk[1] = flags;
                for (int i = 2; i < 16; ++i) {
                    blk[i] = static_cast<uint8_t>(rng.Next());
                }
            }
            voice.type = static_cast<decltype(voice.type)>(1); // VOICETYPE_VAG
            voice.vagAddr = dataPos;
            voice.vagSize = static_cast<uint32_t>(blocks) * 16;
            voice.loop = rng.Range(0, 1) != 0;
            voice.vag.Start(voice.vagAddr, voice.vagSize, voice.loop);
            dataPos += static_cast<uint32_t>(blocks) * 16 + 16;
        }
        voice.pitch = rng.Range(0, 7) == 0 ? 0x1000 : rng.Range(0, 0x4000);
        voice.volumeLeft = rng.Range(-0x1000, 0x1000);
        voice.volumeRight = rng.Range(-0x1000, 0x1000);
        voice.effectLeft = rng.Range(-0x1000, 0x1000);
        voice.effectRight = rng.Range(-0x1000, 0x1000);
        auto &env = voice.envelope;
        if (rng.Range(0, 1)) {
            // Every random value goes through a named local: the order in
            // which function arguments are evaluated differs between
            // compilers, and the golden traces must not.
            const uint32_t env1 = rng.Next() & 0xFFFF;
            const uint32_t env2 = rng.Next() & 0xDFFF;
            env.SetSimpleEnvelope(env1, env2);
        } else {
            static constexpr int kAttack[] = {0, 2, 4};
            static constexpr int kDecay[] = {1, 3, 5};
            const int s = rng.Range(0, 5);
            const int attackMode = kAttack[rng.Range(0, 2)];
            const int decayMode = kDecay[rng.Range(0, 2)];
            const int releaseMode = kDecay[rng.Range(0, 2)];
            env.SetEnvelope(0xF, attackMode, decayMode, s == 0 ? 1 : s, releaseMode);
            int rates[4];
            for (int &rate : rates) {
                const int value = static_cast<int>(rng.Next() & 0x7FFFFFFF);
                rate = value >> rng.Range(0, 24);
            }
            env.SetRate(0xF, rates[0], rates[1], rates[2], rates[3]);
            env.SetSustainLevel(static_cast<int>(rng.Next() & 0x3FFFFFFF));
        }
        a.KeyOn(idx);
    }

    sas.SetWaveformEffectType(rng.Range(-1, 8));
    sas.waveformEffect.leftVol = rng.Range(0, 0x1000);
    sas.waveformEffect.rightVol = rng.Range(0, 0x1000);
    sas.waveformEffect.isDryOn = rng.Range(0, 3) != 0;
    sas.waveformEffect.isWetOn = rng.Range(0, 1);

    const uint32_t outBytes = static_cast<uint32_t>(grain) * (sas.outputMode ? 8u : 4u);
    for (int g = 0; g < grains; ++g) {
        // Random events between grains.
        const int idx = rng.Range(0, 31);
        auto &voice = sas.voices[idx];
        switch (rng.Range(0, 9)) {
        case 0:
            if (voice.type != 0 && !voice.on && !voice.paused) {
                a.KeyOn(idx);
            }
            break;
        case 1:
            if (voice.on && !voice.paused) {
                voice.KeyOff();
            }
            break;
        case 2:
            voice.paused = !voice.paused;
            break;
        case 3:
            voice.pitch = rng.Range(0, 0x4000);
            break;
        case 4:
            sas.SetWaveformEffectType(rng.Range(-1, 8));
            break;
        default:
            break;
        }

        const bool withInput = sas.outputMode == 0 && rng.Range(0, 3) == 0;
        uint32_t inAddr = 0;
        int l = 0;
        int r = 0;
        if (withInput) {
            inAddr = kInAddr;
            for (uint32_t i = 0; i < static_cast<uint32_t>(grain) * 4; ++i) {
                *a.Ram(inAddr + i) = static_cast<uint8_t>(rng.Next());
            }
            l = rng.Range(0, 0x1000);
            r = rng.Range(0, 0x1000);
        }
        a.Mix(kOutAddr, inAddr, l, r);

        uint64_t h = Fnv1a(a.Ram(kOutAddr), outBytes);
        for (int v = 0; v < 32; ++v) {
            const int height = sas.voices[v].envelope.GetHeight();
            const uint8_t flags =
                static_cast<uint8_t>((sas.voices[v].playing ? 1 : 0) | (sas.voices[v].on ? 2 : 0));
            h = Fnv1a(&height, sizeof(height), h);
            h = Fnv1a(&flags, 1, h);
        }
        trace.steps.push_back(h);
    }
    return trace;
}

// ---------------------------------------------------------------- ATRAC

// A RIFF/WAVE ATRAC3 or ATRAC3plus file whose frames are random bytes (the
// fake decoder below turns them into samples).
struct SyntheticAt3 {
    std::vector<uint8_t> bytes;
    uint32_t codec = 0; // 0x1000 at3+, 0x1001 at3
    int channels = 2;
    int frameSize = 0;
    int frames = 0;
};

inline void PutLe16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
inline void PutLe32(std::vector<uint8_t> &b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}
inline void PutTag(std::vector<uint8_t> &b, const char *tag) {
    b.insert(b.end(), tag, tag + 4);
}

inline SyntheticAt3 MakeSyntheticAt3(Rng &rng, bool plus, int channels, int frames, bool fact,
                                     bool extendedFact, bool loop, int badFrame) {
    SyntheticAt3 f;
    f.codec = plus ? 0x1000 : 0x1001;
    f.channels = channels;
    f.frames = frames;
    f.frameSize = plus ? (channels == 1 ? 0x178 : 0x2E8) : (channels == 1 ? 0xC0 : 0x180);
    const int samplesPerFrame = plus ? 2048 : 1024;

    std::vector<uint8_t> &b = f.bytes;
    PutTag(b, "RIFF");
    PutLe32(b, 0); // patched below
    PutTag(b, "WAVE");

    PutTag(b, "fmt ");
    if (plus) {
        PutLe32(b, 0x34);
        PutLe16(b, 0xFFFE);
        PutLe16(b, static_cast<uint16_t>(channels));
        PutLe32(b, 44100);
        PutLe32(b, static_cast<uint32_t>(f.frameSize) * 44100 / 2048);
        PutLe16(b, static_cast<uint16_t>(f.frameSize));
        PutLe16(b, 0);
        PutLe16(b, 0x22);
        PutLe16(b, 0x0800);
        PutLe32(b, channels == 1 ? 4 : 3);
        static constexpr uint8_t kCheck[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                               0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
        b.insert(b.end(), kCheck, kCheck + 16);
        PutLe16(b, 0);
        // sampleSizeMaybe (fmt + 42): bits 2..4 hold the channel count.
        const uint8_t sampleSize = static_cast<uint8_t>(0x20 | (channels << 2) | 0x2);
        PutLe16(b, static_cast<uint16_t>(sampleSize | (0x2E << 8)));
        for (int i = 0; i < 8; ++i) {
            b.push_back(0);
        }
    } else {
        PutLe32(b, 0x20);
        PutLe16(b, 0x0270);
        PutLe16(b, static_cast<uint16_t>(channels));
        PutLe32(b, 44100);
        PutLe32(b, static_cast<uint32_t>(f.frameSize) * 44100 / 1024);
        PutLe16(b, static_cast<uint16_t>(f.frameSize));
        PutLe16(b, 0);
        PutLe16(b, 14); // cbSize
        PutLe16(b, 1);  // "jointStereo" field must be 1
        PutLe32(b, 0x1000);
        PutLe16(b, 0x000E); // sampleSize / tail flag
        PutLe16(b, 0x000E); // must repeat it
        PutLe32(b, 1);
    }

    const int extra = plus ? 0x170 : 0x45;
    const int firstSampleOffset = rng.Range(0, 1) ? (plus ? 0x800 : 0x400) : rng.Range(0, 300);
    const int endSample = frames * samplesPerFrame - firstSampleOffset - extra - rng.Range(0, 500);
    if (fact) {
        PutTag(b, "fact");
        if (extendedFact) {
            PutLe32(b, 12);
            PutLe32(b, static_cast<uint32_t>(endSample));
            PutLe32(b, 0);
            PutLe32(b, static_cast<uint32_t>(firstSampleOffset));
        } else {
            PutLe32(b, 8);
            PutLe32(b, static_cast<uint32_t>(endSample));
            PutLe32(b, static_cast<uint32_t>(firstSampleOffset));
        }
    }
    if (loop) {
        // Loop points count from the start of the file's samples (they
        // include firstSampleOffset), like in real .at3 files.
        const int loopStart = firstSampleOffset + rng.Range(0, std::max(1, endSample / 2));
        const int loopEnd =
            rng.Range(loopStart + 1, std::max(loopStart + 1, firstSampleOffset + endSample - 1));
        PutTag(b, "smpl");
        PutLe32(b, 0x3C);
        for (int i = 0; i < 7; ++i) {
            PutLe32(b, 0);
        }
        PutLe32(b, 1); // loops
        PutLe32(b, 0);
        PutLe32(b, 0);
        PutLe32(b, 0);
        PutLe32(b, static_cast<uint32_t>(loopStart));
        PutLe32(b, static_cast<uint32_t>(loopEnd));
        PutLe32(b, 0);
        PutLe32(b, 0);
    }
    PutTag(b, "data");
    PutLe32(b, static_cast<uint32_t>(frames * f.frameSize));
    for (int fr = 0; fr < frames; ++fr) {
        for (int i = 0; i < f.frameSize; ++i) {
            uint8_t v = static_cast<uint8_t>(rng.Next());
            if (i == 0) {
                v = static_cast<uint8_t>(fr == badFrame ? 0xFF : v % 0xFF);
            }
            b.push_back(v);
        }
    }
    const uint32_t riffSize = static_cast<uint32_t>(b.size() - 8);
    std::memcpy(&b[4], &riffSize, 4);
    return f;
}

// Deterministic stand-in for the ATRAC codec: every input byte pattern maps to
// a fixed frame of samples; a frame starting with 0xFF fails to decode.
inline bool FakeAtracDecode(int samplesPerFrame, int encChannels, const uint8_t *in, int inbytes,
                            int *consumed, int outputChannels, int16_t *out, int *outSamples) {
    if (outSamples) {
        *outSamples = 0;
    }
    if (consumed) {
        *consumed = 0;
    }
    if (inbytes <= 0 || in[0] == 0xFF) {
        if (consumed) {
            *consumed = inbytes;
        }
        return false;
    }
    if (consumed) {
        *consumed = inbytes;
    }
    if (outSamples) {
        *outSamples = samplesPerFrame;
    }
    if (out) {
        for (int i = 0; i < samplesPerFrame; ++i) {
            for (int c = 0; c < outputChannels; ++c) {
                const int src = (i * 7 + c * 13 + encChannels) % inbytes;
                out[i * outputChannels + c] =
                    static_cast<int16_t>((in[src] << 8) ^ (in[(src + 1) % inbytes]) ^ (i << 3));
            }
        }
    }
    return true;
}

struct AtracTrace {
    std::vector<uint64_t> steps;
    uint64_t Final() const { return Fnv1a(steps.data(), steps.size() * sizeof(uint64_t)); }
};

// Adapter requirements (see audio_atrac_test.cpp / audio_ppsspp_difftest.cpp):
//   void Create(uint32_t ctxAddr, int codec);
//   int SetData(uint32_t buffer, uint32_t readSize, uint32_t bufferSize, int outChannels);
//   uint32_t DecodeData(uint32_t outAddr, int *samples, int *finish, int *remains);
//   void GetStreamDataInfo(uint32_t *writePtr, uint32_t *writable, uint32_t *readOffset);
//   int AddStreamData(uint32_t bytes);
//   int GetBufferInfoForResetting(ResetInfo *info, int sample, bool *delay);
//   int ResetPlayPosition(int sample, int first, int second, bool *delay);
//   int SetLoopNum(int n); int LoopStatus(); int RemainingFrames();
//   int GetSecondBufferInfo(uint32_t *off, uint32_t *size);
//   int SetSecondBuffer(uint32_t addr, uint32_t size);
//   int GetSoundSample(int *end, int *ls, int *le); uint32_t GetNextSamples();
//   int GetNextDecodePosition(int *pos);
//   uint8_t *Ram(uint32_t addr);
// ResetInfo is the adapter's AtracResetBufferInfo (8 u32).
template <typename A, typename ResetInfo>
AtracTrace RunAtracScenario(A &a, uint32_t seed, int maxCalls = 400) {
    Rng rng(seed);
    AtracTrace trace;
    auto log = [&](uint64_t value) {
        const uint64_t ctx = Fnv1a(a.Ram(kCtxAddr), 256);
        trace.steps.push_back(Fnv1a(&value, sizeof(value), ctx));
    };

    const bool plus = rng.Range(0, 1) != 0;
    const int channels = rng.Range(1, 2);
    const int frames = rng.Range(4, 120);
    const bool loop = rng.Range(0, 1) != 0;
    // Named locals keep the RNG order independent of the compiler.
    const bool fact = rng.Range(0, 3) != 0;
    const bool extendedFact = rng.Range(0, 3) == 0;
    const int badFrame = rng.Range(0, 7) == 0 ? rng.Range(0, frames - 1) : -1;
    const SyntheticAt3 file =
        MakeSyntheticAt3(rng, plus, channels, frames, fact, extendedFact, loop, badFrame);
    const uint32_t fileSize = static_cast<uint32_t>(file.bytes.size());
    const int outChannels = rng.Range(0, 3) == 0 ? 1 : 2;

    // Buffer mode: all data, halfway, or streamed through a small buffer.
    const int mode = rng.Range(0, 2);
    uint32_t bufferSize = fileSize;
    uint32_t readSize = fileSize;
    if (mode == 1) {
        readSize = static_cast<uint32_t>(
            rng.Range(static_cast<int>(fileSize / 4), static_cast<int>(fileSize)));
    } else if (mode == 2) {
        bufferSize = static_cast<uint32_t>(
            std::max(file.frameSize * 3 + 0x100,
                     rng.Range(file.frameSize * 3, static_cast<int>(fileSize) - 1)));
        // Games usually size the buffer in whole frames, which hits the exact
        // wrap-around boundaries.
        const int shape = rng.Range(0, 2);
        if (shape == 1) {
            bufferSize -= bufferSize % static_cast<uint32_t>(file.frameSize);
        } else if (shape == 2) {
            bufferSize -= bufferSize % static_cast<uint32_t>(file.frameSize);
            bufferSize += static_cast<uint32_t>(file.bytes.size()) -
                          static_cast<uint32_t>(file.frames * file.frameSize); // the header
        }
        bufferSize = std::max<uint32_t>(bufferSize, static_cast<uint32_t>(file.frameSize) * 3);
        bufferSize = std::min(bufferSize, fileSize - 1);
        readSize = bufferSize;
    }
    std::memcpy(a.Ram(kAtracBuf), file.bytes.data(), readSize);

    a.Create(kCtxAddr, static_cast<int>(file.codec));
    int ret = a.SetData(kAtracBuf, readSize, bufferSize, outChannels);
    log(static_cast<uint32_t>(ret));
    if (ret < 0) {
        return trace;
    }
    int end = 0;
    int ls = 0;
    int le = 0;
    a.GetSoundSample(&end, &ls, &le);
    log((static_cast<uint64_t>(static_cast<uint32_t>(end)) << 32) ^ static_cast<uint32_t>(ls) ^
        (static_cast<uint64_t>(static_cast<uint32_t>(le)) << 7));
    if (loop && rng.Range(0, 1)) {
        log(static_cast<uint32_t>(a.SetLoopNum(rng.Range(-1, 3))));
    }

    auto refill = [&]() {
        uint32_t writePtr = 0;
        uint32_t writable = 0;
        uint32_t readOffset = 0;
        a.GetStreamDataInfo(&writePtr, &writable, &readOffset);
        log((static_cast<uint64_t>(writePtr) << 32) ^ writable ^
            (static_cast<uint64_t>(readOffset) << 11));
        if (writable == 0) {
            return;
        }
        uint32_t amount = writable;
        if (rng.Range(0, 2) == 0) {
            amount = static_cast<uint32_t>(rng.Range(0, static_cast<int>(writable)));
        }
        amount = std::min(amount, fileSize > readOffset ? fileSize - readOffset : 0u);
        std::memcpy(a.Ram(writePtr), file.bytes.data() + readOffset, amount);
        log(static_cast<uint32_t>(a.AddStreamData(amount)));
    };

    auto secondBuffer = [&]() {
        uint32_t off = 0;
        uint32_t size = 0;
        const int r = a.GetSecondBufferInfo(&off, &size);
        log(static_cast<uint32_t>(r) ^ (static_cast<uint64_t>(off) << 32) ^ (size << 1));
        if (r == 0 && off < fileSize) {
            size = std::min(size, fileSize - off);
            std::memcpy(a.Ram(kAtracBuf2), file.bytes.data() + off, size);
            log(static_cast<uint32_t>(a.SetSecondBuffer(kAtracBuf2, size)));
        }
    };
    secondBuffer();

    for (int call = 0; call < maxCalls; ++call) {
        const int op = rng.Range(0, 19);
        if (op == 0) {
            // Seek.
            const int sample = rng.Range(0, std::max(0, end));
            ResetInfo info{};
            bool delay = false;
            int r = a.GetBufferInfoForResetting(&info, sample, &delay);
            log(Fnv1a(&info, sizeof(info), static_cast<uint32_t>(r)) ^ (delay ? 1 : 0));
            if (r == 0) {
                const uint32_t *words = reinterpret_cast<const uint32_t *>(&info);
                const uint32_t writePos = words[0];
                const uint32_t writable = words[1];
                const uint32_t minWrite = words[2];
                const uint32_t filePos = words[3];
                uint32_t amount = writable;
                if (minWrite <= writable && rng.Range(0, 3) == 0) {
                    amount = static_cast<uint32_t>(
                        rng.Range(static_cast<int>(minWrite), static_cast<int>(writable)));
                }
                if (filePos < fileSize && writePos != 0) {
                    const uint32_t copy = std::min(amount, fileSize - filePos);
                    std::memcpy(a.Ram(writePos), file.bytes.data() + filePos, copy);
                }
                r = a.ResetPlayPosition(sample, static_cast<int>(amount), 0, &delay);
                log(static_cast<uint32_t>(r) ^ (delay ? 0x100000000ull : 0));
            }
            continue;
        }
        if (op == 1 && loop) {
            log(static_cast<uint32_t>(a.SetLoopNum(rng.Range(-1, 2))));
            continue;
        }
        if (op <= 4) {
            refill();
            continue;
        }
        int samples = 0;
        int finish = 0;
        int remains = 0;
        const uint32_t outAddr = rng.Range(0, 9) == 0 ? 0 : kAtracOut;
        if (outAddr != 0) {
            std::memset(a.Ram(kAtracOut), 0xCD, 2048 * 2 * 2);
        }
        const uint32_t r = a.DecodeData(outAddr, &samples, &finish, &remains);
        uint64_t h = (static_cast<uint64_t>(r) << 32) ^ static_cast<uint32_t>(samples) ^
                     (static_cast<uint64_t>(finish) << 20) ^
                     (static_cast<uint64_t>(static_cast<uint32_t>(remains)) << 24);
        if (outAddr != 0) {
            h = Fnv1a(a.Ram(kAtracOut), 2048 * 2 * 2, h);
        }
        int pos = 0;
        const int pr = a.GetNextDecodePosition(&pos);
        h ^= (static_cast<uint64_t>(static_cast<uint32_t>(pr)) << 1) ^
             (static_cast<uint64_t>(static_cast<uint32_t>(pos)) << 9) ^ a.GetNextSamples() ^
             (static_cast<uint64_t>(static_cast<uint32_t>(a.LoopStatus())) << 40) ^
             (static_cast<uint64_t>(static_cast<uint32_t>(a.RemainingFrames())) << 44);
        log(h);
        if (r == 0x80630024u) { // ALL_DATA_DECODED
            break;
        }
    }
    // Final state of the buffers.
    trace.steps.push_back(Fnv1a(a.Ram(kAtracBuf), std::min<uint32_t>(bufferSize, 0x40000)));
    return trace;
}

} // namespace audio_test
