// SPDX-License-Identifier: GPL-3.0-or-later
//
// SAS mixer tests without game data: hand-computed VAG/ADSR/mixer results,
// the sceSasCore argument checks, and golden traces of random scenarios whose
// values were produced by PPSSPP v1.20.4's own SasAudio.cpp
// (mpo_audio_ppsspp_difftest regenerates them, see docs/sessions/F16a.md).

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "audio_errors.h"
#include "audio_scenarios.h"
#include "audio_test_util.h"
#include "sas_core.h"

namespace {

using audio_test::Expect;
using audio_test::ExpectEq;
using audio_test::FakeRam;
using namespace mpo::audio;

// One VAG block: header, flags and 14 data bytes (28 nibbles).
void PutVagBlock(FakeRam &ram, uint32_t addr, int predict, int shift, int flags,
                 const std::vector<int> &nibbles) {
    uint8_t blk[16]{};
    blk[0] = static_cast<uint8_t>((predict << 4) | shift);
    blk[1] = static_cast<uint8_t>(flags);
    for (size_t i = 0; i < 28; ++i) {
        const int n = nibbles[i % nibbles.size()] & 0xF;
        blk[2 + i / 2] |= static_cast<uint8_t>(i % 2 == 0 ? n : n << 4);
    }
    ram.Put(addr, blk, 16);
}

void TestVagNoFilter() {
    FakeRam ram;
    const GuestMemory mem = ram.View();
    const uint32_t addr = audio_test::kDataAddr;
    // Filter 0, shift 0: every sample is the signed nibble * 4096.
    PutVagBlock(ram, addr, 0, 0, 0, {1, 2, 7, 0xF, 0x8, 0});
    PutVagBlock(ram, addr + 16, 0, 0, 7, {0}); // end marker
    VagDecoder vag;
    vag.Start(addr, 32, false);
    int16_t out[40];
    vag.GetSamples(mem, out, 28);
    const int expected[6] = {4096, 8192, 28672, -4096, -32768, 0};
    for (int i = 0; i < 28; ++i) {
        ExpectEq(out[i], expected[i % 6], "VAG filter 0 sample " + std::to_string(i));
    }
    Expect(!vag.End(), "VAG not ended after the first block");
    vag.GetSamples(mem, out, 10);
    Expect(vag.End(), "VAG ends on flag 7");
    for (int i = 0; i < 10; ++i) {
        ExpectEq(out[i], 0, "VAG zeros after the end");
    }
}

void TestVagFilter() {
    FakeRam ram;
    const GuestMemory mem = ram.View();
    const uint32_t addr = audio_test::kDataAddr;
    const std::vector<int> nibbles = {3, 0xD, 5, 1, 0xA, 7, 0};
    PutVagBlock(ram, addr, 2, 9, 0, nibbles); // filter 2: (115, -52)
    PutVagBlock(ram, addr + 16, 0, 0, 7, {0});
    VagDecoder vag;
    vag.Start(addr, 32, false);
    int16_t out[28];
    vag.GetSamples(mem, out, 28);
    // Independent re-derivation of the PSX ADPCM formula.
    int s1 = 0;
    int s2 = 0;
    for (int i = 0; i < 28; ++i) {
        const int n = nibbles[static_cast<size_t>(i) % nibbles.size()];
        const int raw = static_cast<int16_t>(n << 12) >> 9;
        int v = raw + ((s1 * 115 - s2 * 52) >> 6);
        v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        s2 = s1;
        s1 = v;
        ExpectEq(out[i], v, "VAG filter 2 sample " + std::to_string(i));
    }
}

void TestVagLoop() {
    FakeRam ram;
    const GuestMemory mem = ram.View();
    const uint32_t addr = audio_test::kDataAddr;
    PutVagBlock(ram, addr, 0, 0, 0, {1});      // block 0: 4096
    PutVagBlock(ram, addr + 16, 0, 0, 6, {2}); // block 1: loop start, 8192
    PutVagBlock(ram, addr + 32, 0, 0, 3, {3}); // block 2: loop end, 12288
    VagDecoder vag;
    vag.Start(addr, 48, true);
    std::vector<int16_t> out(28 * 6);
    vag.GetSamples(mem, out.data(), static_cast<int>(out.size()));
    const int blocks[6] = {4096, 8192, 12288, 8192, 12288, 8192};
    for (int b = 0; b < 6; ++b) {
        ExpectEq(out[static_cast<size_t>(b) * 28 + 5], blocks[b],
                 "VAG loop block " + std::to_string(b));
    }
    Expect(!vag.End(), "looping VAG never ends");
}

void TestAdsrLinear() {
    ADSREnvelope env;
    env.SetEnvelope(
        0xF, PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE, PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE,
        PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE, PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE);
    env.SetRate(0xF, 0x01000000, 0x00100000, 0x00010000, 0x00400000);
    env.SetSustainLevel(0x30000000);
    env.KeyOn();
    Expect(env.NeedsKeyOn(), "KeyOn leaves the envelope in KEYON");
    // KEYON, then 31 KEYON_STEP steps at height 1..31, then ATTACK from 0.
    for (int i = 0; i < 32; ++i) {
        env.Step();
    }
    ExpectEq(env.GetHeight(), 0, "height 0 when attack starts");
    env.Step();
    ExpectEq(env.GetHeight(), 0x01000000, "one attack step");
    for (int i = 0; i < 63; ++i) {
        env.Step();
    }
    ExpectEq(env.GetHeight(), PSP_SAS_ENVELOPE_HEIGHT_MAX, "attack reaches the maximum");
    // Decay down to the sustain level: (0x40000000 - 0x30000000) / 0x100000 steps.
    for (int i = 0; i < 0x100; ++i) {
        env.Step();
    }
    ExpectEq(env.GetHeight(), 0x30000000, "decay reaches the sustain level");
    env.Step(); // one more decay step puts it below and switches to sustain
    env.Step();
    ExpectEq(env.GetHeight(), 0x30000000 - 0x100000 - 0x10000, "sustain step");
    env.KeyOff();
    env.Step();
    ExpectEq(env.GetHeight(), 0x30000000 - 0x100000 - 0x10000 - 0x400000, "release step");
    for (int i = 0; i < 0x100 && !env.HasEnded(); ++i) {
        env.Step();
    }
    Expect(env.HasEnded(), "release ends the envelope");
    ExpectEq(env.GetHeight(), 0, "height 0 after release");
}

void TestSimpleAdsrDecode() {
    ADSREnvelope env;
    // env1: attack 0x15 linear, decay n=3, sustain level 7.
    // env2: sustain type 3 (exp decrease) rate 0x20, release linear n=4.
    env.SetSimpleEnvelope((0x15u << 8) | (3u << 4) | 7u, (3u << 14) | (0x20u << 6) | 4u);
    ExpectEq(env.attackType, PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE, "simple attack type");
    ExpectEq(env.attackRate, ((7 - (0x15 & 3)) << 26) >> (0x15 >> 2), "simple attack rate");
    ExpectEq(env.decayType, PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE, "simple decay type");
    ExpectEq(env.decayRate, static_cast<int>(0x80000000u >> 3), "simple decay rate");
    ExpectEq(env.sustainLevel, 8 << 26, "simple sustain level");
    ExpectEq(env.sustainType, PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE, "simple sustain type");
    ExpectEq(env.sustainRate, ((7 - (0x20 & 3)) << 24) >> (0x20 >> 2), "simple sustain rate");
    ExpectEq(env.releaseType, PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE, "simple release type");
    ExpectEq(env.releaseRate, 0x10000000 >> 4, "simple release rate");
}

// A PCM voice at the base pitch through a DIRECT envelope: the mix is
// computable by hand (see the comments).
void TestMixPcmVoice() {
    FakeRam ram;
    const GuestMemory mem = ram.View();
    SasCore core;
    const uint32_t coreAddr = 0x08800400;
    ExpectEq(core.Init(mem, coreAddr, 64, 32, 0, 44100), 0u, "Init");
    for (int i = 0; i < 256; ++i) {
        ram.Put16(audio_test::kDataAddr + static_cast<uint32_t>(i) * 2, 1000);
    }
    ExpectEq(core.SetVoicePCM(mem, coreAddr, 3, audio_test::kDataAddr, 256, 0), 0u, "PCM voice");
    ExpectEq(core.SetVolume(coreAddr, 3, 0x1000, 0x800, 0, 0), 0u, "volume");
    // Decay/sustain/release linear with rate 0: the level holds once reached.
    ExpectEq(core.SetADSRMode(coreAddr, 3, 0xF, 4, 1, 1, 1), 0u, "ADSR modes");
    ExpectEq(core.SetADSRMode(coreAddr, 3, 0x1, 5, 5, 5, 5), kErrSasInvalidAdsrCurveMode,
             "attack mode 5 is odd -> invalid");
    // The API refuses DIRECT for the attack; set it on the instance to get a
    // one-step attack whose output is easy to compute.
    core.instance().voices[3].envelope.attackType = PSP_SAS_ADSR_CURVE_MODE_DIRECT;
    core.instance().voices[3].envelope.attackRate = 0x40000000;
    ExpectEq(core.SetKeyOn(mem, coreAddr, 3), 0u, "key on");
    ExpectEq(core.SetKeyOn(mem, coreAddr, 3), kErrSasVoicePaused, "second key on fails");

    ExpectEq(core.Core(mem, coreAddr, audio_test::kOutAddr), 0u, "first grain");
    // 32 samples of key-on delay, one at envelope 0, then 1000 / 500.
    for (int i = 0; i < 64; ++i) {
        const int16_t l = ram.Get16(audio_test::kOutAddr + static_cast<uint32_t>(i) * 4);
        const int16_t r = ram.Get16(audio_test::kOutAddr + static_cast<uint32_t>(i) * 4 + 2);
        ExpectEq(l, i < 33 ? 0 : 1000, "grain 1 left " + std::to_string(i));
        ExpectEq(r, i < 33 ? 0 : 500, "grain 1 right " + std::to_string(i));
    }
    ExpectEq(core.Core(mem, coreAddr, audio_test::kOutAddr), 0u, "second grain");
    for (int i = 0; i < 64; ++i) {
        ExpectEq(ram.Get16(audio_test::kOutAddr + static_cast<uint32_t>(i) * 4), 1000,
                 "grain 2 left " + std::to_string(i));
    }
    ExpectEq(core.GetEnvelopeHeight(coreAddr, 3), static_cast<uint32_t>(0x40000000),
             "envelope at max");
    ExpectEq(core.GetEndFlag(coreAddr), ~(1u << 3), "only voice 3 is playing");

    // CoreWithMix: the input buffer is scaled by 0x800/0x1000 and added.
    for (int i = 0; i < 128; ++i) {
        ram.Put16(audio_test::kInAddr + static_cast<uint32_t>(i) * 2, 2000);
    }
    ExpectEq(core.CoreWithMix(mem, coreAddr, audio_test::kInAddr, 0x800, 0x1000), 0u, "with mix");
    ExpectEq(ram.Get16(audio_test::kInAddr), 1000 + 1000, "mixed left");
    ExpectEq(ram.Get16(audio_test::kInAddr + 2), 500 + 2000, "mixed right");

    // Key off with release rate 0 keeps the level; key off twice is an error.
    ExpectEq(core.SetKeyOff(coreAddr, 3), 0u, "key off");
    ExpectEq(core.SetKeyOff(coreAddr, 3), kErrSasVoicePaused, "key off twice");
}

void TestSasArgumentChecks() {
    FakeRam ram;
    const GuestMemory mem = ram.View();
    SasCore core;
    const uint32_t c = 0x08800400;
    ExpectEq(core.Init(mem, c + 4, 64, 32, 0, 44100), kErrSasBadAddress, "unaligned core");
    ExpectEq(core.Init(mem, c, 0x30, 32, 0, 44100), kErrSasInvalidGrain, "grain too small");
    ExpectEq(core.Init(mem, c, 0x810, 32, 0, 44100), kErrSasInvalidGrain, "grain too big");
    ExpectEq(core.Init(mem, c, 0x50, 32, 0, 44100), kErrSasInvalidGrain, "grain not /32");
    ExpectEq(core.Init(mem, c, 64, 33, 0, 44100), kErrSasInvalidMaxVoices, "33 voices");
    ExpectEq(core.Init(mem, c, 64, 32, 2, 44100), kErrSasInvalidOutputMode, "output mode 2");
    ExpectEq(core.Init(mem, c, 64, 32, 0, 48000), kErrSasInvalidSampleRate, "48 kHz");
    ExpectEq(core.Init(mem, c, 256, 32, 0, 44100), 0u, "valid init");
    ExpectEq(core.GetGrain(c), 256u, "grain");
    ExpectEq(core.GetEndFlag(c), 0xFFFFFFFFu, "no voice playing");
    ExpectEq(core.SetVoice(mem, c, 32, audio_test::kDataAddr, 16, 0), kErrSasInvalidVoice,
             "voice 32");
    ExpectEq(core.SetVoice(mem, c, 0, audio_test::kDataAddr, 15, 0), kErrSasInvalidParameter,
             "VAG size not /16");
    ExpectEq(core.SetVoice(mem, c, 0, audio_test::kDataAddr, 16, 2), kErrSasInvalidLoopPos,
             "loop flag 2");
    ExpectEq(core.SetVoice(mem, c, 0, 0x100, 16, 0), 0u, "bad VAG address is ignored");
    ExpectEq(core.SetPitch(c, 0, 0x4001), kErrSasInvalidPitch, "pitch too high");
    ExpectEq(core.SetVolume(c, 0, 0x1001, 0, 0, 0), kErrSasInvalidVolume, "volume too high");
    ExpectEq(core.SetVolume(c, 0, -0x1000, 0x1000, 0, 0), 0u, "negative volume is fine");
    ExpectEq(core.SetNoise(c, 0, 64), kErrSasInvalidNoiseFreq, "noise freq");
    ExpectEq(core.SetADSR(c, 0, 0x1, -1, 0, 0, 0), kErrSasInvalidAdsrRate, "negative rate");
    ExpectEq(core.SetADSR(c, 0, 0x2, -1, 0, 0, 0), 0u, "negative rate not selected by flag");
    ExpectEq(core.SetSimpleADSR(c, 0, 0, 1u << 13), kErrSasInvalidAdsrCurveMode, "env2 bit 13");
    ExpectEq(core.RevType(c, 9), kErrSasRevInvalidType, "reverb type 9");
    ExpectEq(core.RevType(c, -1), 0u, "reverb off");
    ExpectEq(core.RevParam(c, 128, 0), kErrSasRevInvalidDelay, "reverb delay");
    ExpectEq(core.RevParam(c, 0, 128), kErrSasRevInvalidFeedback, "reverb feedback");
    ExpectEq(core.RevEVOL(c, 0x1001, 0), kErrSasRevInvalidVolume, "reverb volume");
    ExpectEq(core.SetVoicePCM(mem, c, 1, audio_test::kDataAddr, 0x10001, 0), kErrSasInvalidPcmSize,
             "PCM size");
    ExpectEq(core.SetVoicePCM(mem, c, 1, audio_test::kDataAddr, 16, 16), kErrSasInvalidLoopPos,
             "PCM loop pos");
    ExpectEq(core.SetPause(c, 0x5, 1), 0u, "pause");
    ExpectEq(core.GetPauseFlag(c), 0x5u, "pause flags");
    ExpectEq(core.SetKeyOn(mem, c, 0), kErrSasVoicePaused, "key on a paused voice");
    ExpectEq(core.Core(mem, c, 0x100), kErrSasInvalidParameter, "bad output address");
    ExpectEq(core.Core(mem, c, audio_test::kOutAddr, false), kErrKernelCanNotWait,
             "dispatch disabled");
    ExpectEq(core.SetOutputMode(c, 1), 0u, "raw mode");
    ExpectEq(core.CoreWithMix(mem, c, audio_test::kInAddr, 0, 0), kErrKernelBadArgument,
             "with-mix in raw mode");
    ExpectEq(core.UnsetATRAC3(mem, c, 0), kErrSasAtrac3NotSet, "unset without ATRAC3");
    ExpectEq(core.GetAllEnvelopeHeights(mem, c, audio_test::kInAddr), 0u, "all heights");
}

void TestReverbOff() {
    SasReverb reverb;
    reverb.SetPreset(-1);
    const int16_t in[4] = {1000, -2000, 32767, -32768};
    int16_t out[8]{};
    reverb.ProcessReverb(out, in, 2, 0x4000, 0x8000);
    // OFF replicates the input scaled by vol >> 15 into both halves.
    const int16_t expected[8] = {500, -2000, 500, -2000, 16383, -32768, 16383, -32768};
    for (int i = 0; i < 8; ++i) {
        ExpectEq(out[i], expected[i], "reverb off value " + std::to_string(i));
    }
}

// Adapter for the shared scenarios.
class MpoSas {
public:
    explicit MpoSas(FakeRam &ram) : ram_(ram), mem_(ram.View()) {}
    SasInstance &sas() { return sas_; }
    void KeyOn(int voice) { sas_.voices[voice].KeyOn(mem_); }
    void Mix(uint32_t out, uint32_t in, int l, int r) { sas_.Mix(mem_, out, in, l, r, false); }
    uint8_t *Ram(uint32_t addr) { return ram_.At(addr); }

private:
    FakeRam &ram_;
    GuestMemory mem_;
    SasInstance sas_;
};

// Final hashes of RunSasScenario(seed) computed with PPSSPP v1.20.4's
// SasAudio.cpp/SasReverb.cpp (mpo_audio_ppsspp_difftest --print-golden).
struct Golden {
    uint32_t seed;
    uint64_t hash;
};
constexpr Golden kSasGolden[] = {
#include "audio_sas_golden.inc"
};

void TestGoldenScenarios() {
    for (const Golden &g : kSasGolden) {
        FakeRam ram(audio_test::kScenarioRam);
        MpoSas a(ram);
        const uint64_t got = audio_test::RunSasScenario(a, g.seed).Final();
        if (got != g.hash) {
            std::fprintf(stderr, "FAIL: SAS scenario %u: %016llx != PPSSPP %016llx\n", g.seed,
                         static_cast<unsigned long long>(got),
                         static_cast<unsigned long long>(g.hash));
            ++audio_test::g_failures;
        }
    }
    Expect(std::size(kSasGolden) >= 8, "golden SAS scenarios present");
}

} // namespace

int main() {
    TestVagNoFilter();
    TestVagFilter();
    TestVagLoop();
    TestAdsrLinear();
    TestSimpleAdsrDecode();
    TestMixPcmVoice();
    TestSasArgumentChecks();
    TestReverbOff();
    TestGoldenScenarios();
    return audio_test::Finish("audio_sas_test");
}
