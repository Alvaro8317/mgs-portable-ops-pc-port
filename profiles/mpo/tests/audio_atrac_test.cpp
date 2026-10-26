// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceAtrac tests without game data: header parsing, the AtracManager API on a
// fake codec (IDs, decode loop, loops, errors), the real at3_standalone codec
// on garbage input, and golden traces of random scenarios whose values come
// from PPSSPP v1.20.4's AtracCtx2.cpp (see audio_scenarios.h).

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "atrac_manager.h"
#include "audio_errors.h"
#include "audio_scenarios.h"
#include "audio_test_util.h"

namespace {

using audio_test::Expect;
using audio_test::ExpectEq;
using audio_test::FakeRam;
using namespace mpo::audio;

constexpr uint32_t kBss = 0x08800000;
constexpr uint32_t kFile = 0x08900000;
constexpr uint32_t kOut = 0x08880000;
constexpr uint32_t kVars = 0x08870000;

class FakeDecoder : public AudioDecoder {
public:
    FakeDecoder(uint32_t codec, int channels) : codec_(codec), channels_(channels) {}
    bool Decode(const uint8_t *in, int inbytes, int *consumed, int outCh, int16_t *out,
                int *outSamples) override {
        return audio_test::FakeAtracDecode(codec_ == PSP_CODEC_AT3PLUS ? 2048 : 1024, channels_, in,
                                           inbytes, consumed, outCh, out, outSamples);
    }
    bool IsOK() const override { return true; }

private:
    uint32_t codec_;
    int channels_;
};

AtracDecoderFactory FakeFactory() {
    return [](uint32_t codec, int, int channels) -> std::unique_ptr<AudioDecoder> {
        return std::make_unique<FakeDecoder>(codec, channels);
    };
}

void TestParseWave() {
    for (int plus = 0; plus <= 1; ++plus) {
        for (int channels = 1; channels <= 2; ++channels) {
            audio_test::Rng rng(static_cast<uint32_t>(10 + plus * 2 + channels));
            const audio_test::SyntheticAt3 f =
                audio_test::MakeSyntheticAt3(rng, plus != 0, channels, 8, true, false, true, -1);
            TrackInfo info{};
            const uint32_t size = static_cast<uint32_t>(f.bytes.size());
            const int codec = ParseWaveAT3(f.bytes.data(), size, size, &info);
            const std::string tag =
                std::string(plus ? "at3+" : "at3") + " ch" + std::to_string(channels);
            ExpectEq(codec, static_cast<int>(f.codec), tag + " codec");
            ExpectEq(info.numChans, channels, tag + " channels");
            ExpectEq(info.blockAlign, f.frameSize, tag + " block align");
            ExpectEq(info.waveDataSize, static_cast<uint32_t>(f.frameSize * 8), tag + " data size");
            Expect(info.loopStart != 0xFFFFFFFF && info.loopEnd > info.loopStart, tag + " loop");

            Track track;
            std::string error;
            ExpectEq(AnalyzeAtracTrack(f.bytes.data(), size, size, &track, &error), 0,
                     tag + " analyze");
            ExpectEq(track.codecType, f.codec, tag + " analyze codec");
            ExpectEq(track.bytesPerFrame, f.frameSize, tag + " analyze frame size");

            // A header cut short: an error, never a read past the buffer.
            TrackInfo cut{};
            const int r = ParseWaveAT3(f.bytes.data(), 40, 40, &cut);
            ExpectEq(r, static_cast<int>(kErrAtracSizeTooSmall), tag + " truncated header");
        }
    }

    Track track;
    std::string error;
    std::vector<uint8_t> junk(100, 0x55);
    ExpectEq(AnalyzeAtracTrack(junk.data(), 100, 100, &track, &error),
             static_cast<int>(kErrAtracUnknownFormat), "no RIFF magic");
    ExpectEq(AnalyzeAtracTrack(junk.data(), 100, 71, &track, &error),
             static_cast<int>(kErrAtracSizeTooSmall), "under 72 bytes");
    ExpectEq(AnalyzeAtracTrack(nullptr, 0, 100, &track, &error),
             static_cast<int>(kErrKernelInvalidPointer), "null buffer");
    // The game claims more bytes than exist: reads stop at `avail`.
    audio_test::Rng rng(3);
    const auto f = audio_test::MakeSyntheticAt3(rng, true, 2, 4, true, false, false, -1);
    Track t2;
    AnalyzeAtracTrack(f.bytes.data(), 60, 0x10000000, &t2, &error);
}

void TestParseAa3() {
    // ID3 tag of 0 bytes, then an EA3 header for ATRAC3plus stereo, 0x2E8-byte frames.
    std::vector<uint8_t> b(10 + 96 + 0x2E8 * 4, 0);
    b[0] = 'e';
    b[1] = 'a';
    b[2] = '3';
    b[3] = 3;
    uint8_t *ea3 = b.data() + 10;
    ea3[0] = 'E';
    ea3[1] = 'A';
    ea3[2] = '3';
    ea3[4] = 0;
    ea3[5] = 96; // header size
    ea3[6] = 0xFF;
    ea3[7] = 0xFF;
    // codec 1 (at3+), params: 44.1k (1 << 13), stereo (2 << 10), (0x2E8 - 8) / 8 = 0x5C.
    const uint32_t params = (1u << 13) | (2u << 10) | 0x5Cu;
    ea3[32] = 1;
    ea3[33] = static_cast<uint8_t>(params >> 16);
    ea3[34] = static_cast<uint8_t>(params >> 8);
    ea3[35] = static_cast<uint8_t>(params);
    TrackInfo info{};
    const uint32_t size = static_cast<uint32_t>(b.size());
    ExpectEq(ParseAA3(b.data(), size, size, size, &info), static_cast<int>(PSP_CODEC_AT3PLUS),
             "AA3 codec");
    ExpectEq(info.blockAlign, 0x2E8, "AA3 block align");
    ExpectEq(info.dataOff, 10u + 96u, "AA3 data offset");
}

struct Harness {
    FakeRam ram{8u << 20};
    GuestMemory mem = ram.View();
    AtracManager atrac{mem, FakeFactory()};

    explicit Harness(bool loadModule = true) {
        if (loadModule) {
            Expect(atrac.NotifyLoadModule(0x105, kBss, 0x67C), "NotifyLoadModule");
        }
    }
    int Load(const audio_test::SyntheticAt3 &f) {
        ram.Put(kFile, f.bytes.data(), f.bytes.size());
        return atrac.SetDataAndGetID(kFile, static_cast<int>(f.bytes.size()));
    }
    // Decodes until finish/error; returns total sample frames.
    int DecodeAll(int id, int *calls, uint32_t *lastRet, int channels) {
        int total = 0;
        *calls = 0;
        for (int i = 0; i < 100000; ++i) {
            const uint32_t r = atrac.DecodeData(id, kOut, kVars, kVars + 4, kVars + 8);
            *lastRet = r;
            if (r != 0) {
                break;
            }
            ++*calls;
            const int n = static_cast<int>(ram.Get32(kVars));
            total += n;
            // The output was filled with 0xCD bytes (-12851) before decoding.
            if (n > 0) {
                Expect(ram.Get16(kOut) != -12851 || channels == 0, "decoded samples written");
            }
            if (ram.Get32(kVars + 4) != 0) {
                break;
            }
        }
        return total;
    }
};

void TestManagerDecode() {
    for (int plus = 0; plus <= 1; ++plus) {
        Harness h;
        audio_test::Rng rng(static_cast<uint32_t>(77 + plus));
        const auto f = audio_test::MakeSyntheticAt3(rng, plus != 0, 2, 30, true, false, true, -1);
        const int id = h.Load(f);
        // IDs 0-1 are ATRAC3plus and 2-3 ATRAC3 after __AtracInit.
        ExpectEq(id, plus ? 0 : 2, "atrac ID by codec");
        ExpectEq(h.atrac.LastDelayUs(), 100, "SetDataAndGetID delays 100 us");

        ExpectEq(h.atrac.GetSoundSample(id, kVars + 16, kVars + 20, kVars + 24), 0u,
                 "sound sample");
        const int endSample = static_cast<int>(h.ram.Get32(kVars + 16));
        const int loopStart = static_cast<int>(h.ram.Get32(kVars + 20));
        const int loopEnd = static_cast<int>(h.ram.Get32(kVars + 24));
        Expect(loopStart >= 0 && loopEnd > loopStart, "loop points");
        ExpectEq(h.atrac.GetMaxSample(id, kVars + 28), 0u, "max sample");
        ExpectEq(h.ram.Get32(kVars + 28), plus ? 2048u : 1024u, "samples per frame");
        ExpectEq(h.atrac.GetChannel(id, kVars + 32), 0u, "channel");
        ExpectEq(h.ram.Get32(kVars + 32), 2u, "stereo");
        ExpectEq(h.atrac.GetLoopStatus(id, kVars + 36, kVars + 40), 0u, "loop status");
        ExpectEq(h.ram.Get32(kVars + 36), 0u, "loop count 0");
        Expect(h.ram.Get32(kVars + 40) <= 1u, "loop status is 0 or 1");
        ExpectEq(h.atrac.GetRemainFrame(id, kVars + 44), 0u, "remain frame");
        ExpectEq(static_cast<int>(h.ram.Get32(kVars + 44)), PSP_ATRAC_ALLDATA_IS_ON_MEMORY,
                 "all data on memory");

        int calls = 0;
        uint32_t last = 0;
        std::memset(h.ram.At(kOut), 0xCD, 0x4000);
        const int total = h.DecodeAll(id, &calls, &last, 2);
        ExpectEq(total, endSample + 1, "decoded every sample once");
        ExpectEq(last, 0u, "ends with the finish flag, not an error");
        ExpectEq(h.atrac.LastDelayUs(), 2300, "decode delays 2300 us");
        ExpectEq(h.atrac.DecodeData(id, kOut, kVars, kVars + 4, kVars + 8), kErrAtracAllDataDecoded,
                 "nothing left");

        // Play it again, looping once: everything plus one more pass of the loop.
        ExpectEq(h.atrac.ResetPlayPosition(id, 0, 0, 0), 0u, "reset to 0");
        ExpectEq(h.atrac.LastDelayUs(), 3000, "reset delays 3000 us");
        ExpectEq(h.atrac.SetLoopNum(id, 1), 0u, "loop once");
        const int looped = h.DecodeAll(id, &calls, &last, 2);
        Expect(looped > total && looped <= total + (loopEnd - loopStart + 1) + 2048,
               "one extra loop pass (" + std::to_string(looped) + " vs " + std::to_string(total) +
                   ")");

        ExpectEq(h.atrac.ReleaseAtracID(id), 0u, "release");
        ExpectEq(h.atrac.ReleaseAtracID(id), kErrAtracBadAtracId, "release twice");
    }
}

void TestManagerErrors() {
    {
        Harness h(false);
        audio_test::Rng rng(5);
        const auto f = audio_test::MakeSyntheticAt3(rng, true, 2, 8, true, false, false, -1);
        ExpectEq(h.Load(f), static_cast<int>(kErrAtracNoAtracId), "no BSS, no IDs");
        Expect(!h.atrac.NotifyLoadModule(0x105, 0x100, 0x67C), "BSS outside RAM refused");
        Expect(!h.atrac.NotifyLoadModule(0x105, kBss, 0x100), "BSS too small refused");
    }
    Harness h;
    audio_test::Rng rng(6);
    const auto f = audio_test::MakeSyntheticAt3(rng, true, 2, 8, true, false, false, -1);
    ExpectEq(h.Load(f), 0, "first at3+ ID");
    ExpectEq(h.atrac.SetDataAndGetID(kFile, static_cast<int>(f.bytes.size())), 1, "second");
    ExpectEq(h.atrac.SetDataAndGetID(kFile, static_cast<int>(f.bytes.size())),
             static_cast<int>(kErrAtracNoAtracId), "only two at3+ IDs");
    ExpectEq(h.atrac.SetLoopNum(0, 2), kErrAtracNoLoopInformation, "no loop info");
    ExpectEq(h.atrac.DecodeData(0, kOut + 1, kVars, kVars + 4, kVars + 8), kErrAtracBadAlignment,
             "odd output address");
    ExpectEq(h.atrac.DecodeData(5, kOut, kVars, kVars + 4, kVars + 8), kErrAtracBadAtracId,
             "unused ID");
    ExpectEq(h.atrac.DecodeData(9, kOut, kVars, kVars + 4, kVars + 8), kErrAtracBadAtracId,
             "ID out of range");
    ExpectEq(h.atrac.Reinit(1, 1), static_cast<int>(kErrKernelBusy), "reinit while in use");
    h.atrac.ReleaseAtracID(0);
    h.atrac.ReleaseAtracID(1);
    // 6 slots: two at3+ (2 each) + two at3 = 6.
    ExpectEq(h.atrac.Reinit(2, 2), 0, "reinit 2 + 2");
    ExpectEq(h.atrac.GetAtracID(static_cast<int>(PSP_CODEC_AT3)), 2u, "at3 after the at3+ IDs");
    h.atrac.ReleaseAtracID(2);
    ExpectEq(h.atrac.Reinit(4, 4), static_cast<int>(kErrKernelOutOfMemory), "over budget");
    ExpectEq(h.atrac.GetAtracID(0x1234), kErrAtracInvalidCodecType, "bad codec type");
    ExpectEq(h.atrac.SetDataAndGetID(kFile, 40), static_cast<int>(kErrAtracSizeTooSmall),
             "tiny buffer");
    ExpectEq(h.atrac.SetDataAndGetID(0, 1000), static_cast<int>(kErrKernelInvalidPointer),
             "null buffer");
}

void TestBadFrame() {
    Harness h;
    audio_test::Rng rng(9);
    // Frame 5 cannot be decoded: the call fails with API_FAIL and moves on.
    const auto f = audio_test::MakeSyntheticAt3(rng, false, 2, 12, true, false, false, 5);
    const int id = h.Load(f);
    Expect(id >= 0, "load with a bad frame");
    // Like PPSSPP, a packet that fails to decode is not skipped: the read
    // position stays on it and every later call fails the same way.
    int good = 0;
    uint32_t r = 0;
    for (int i = 0; i < 100; ++i) {
        r = h.atrac.DecodeData(id, kOut, kVars, kVars + 4, kVars + 8);
        if (r != 0) {
            break;
        }
        ++good;
    }
    ExpectEq(r, kErrAtracApiFail, "bad frame -> API_FAIL");
    Expect(good >= 3 && good <= 5, "the frames before it decode");
    ExpectEq(h.atrac.LastDelayUs(), 2300, "API_FAIL still delays");
    ExpectEq(h.atrac.GetInternalErrorInfo(id, kVars + 48), 0u, "error info");
    ExpectEq(h.ram.Get32(kVars + 48), 0x20Bu, "codec error 0x20B");
    ExpectEq(h.atrac.GetNextDecodePosition(id, kVars + 52), 0u, "position");
    const uint32_t pos = h.ram.Get32(kVars + 52);
    ExpectEq(h.atrac.DecodeData(id, kOut, kVars, kVars + 4, kVars + 8), kErrAtracApiFail,
             "still failing");
    h.atrac.GetNextDecodePosition(id, kVars + 52);
    ExpectEq(h.ram.Get32(kVars + 52), pos, "the position did not move");
}

void TestRealCodecOnGarbage() {
    // at3_standalone must reject or survive garbage without touching memory
    // out of bounds (run under ASan in CI).
    audio_test::Rng rng(1234);
    for (int plus = 0; plus <= 1; ++plus) {
        const int frameSize = plus ? 0x2E8 : 0x180;
        auto dec = CreateAtracDecoder(plus ? PSP_CODEC_AT3PLUS : PSP_CODEC_AT3, frameSize, 2);
        std::vector<uint8_t> frame(static_cast<size_t>(frameSize) + 64, 0);
        std::vector<int16_t> out(2048 * 2);
        for (int i = 0; i < 64; ++i) {
            for (int j = 0; j < frameSize; ++j) {
                frame[static_cast<size_t>(j)] = i == 0 ? 0 : static_cast<uint8_t>(rng.Next());
            }
            int consumed = 0;
            int samples = 0;
            dec->Decode(frame.data(), frameSize, &consumed, 2, out.data(), &samples);
            ExpectEq(consumed, frameSize, "garbage packet consumed");
            Expect(samples >= 0 && samples <= (plus ? 2048 : 1024), "sample count in range");
        }
    }
}

class MpoAtracAdapter {
public:
    explicit MpoAtracAdapter(FakeRam &ram) : ram_(ram) {}
    void Create(uint32_t ctxAddr, int codec) {
        ctx_ = std::make_unique<AtracContext>(ram_.View(), ctxAddr, codec, FakeFactory());
    }
    int SetData(uint32_t buffer, uint32_t readSize, uint32_t bufferSize, int outChannels) {
        Track track;
        return ctx_->SetData(track, buffer, readSize, bufferSize, 0, outChannels, false);
    }
    uint32_t DecodeData(uint32_t outAddr, int *samples, int *finish, int *remains) {
        return ctx_->DecodeData(outAddr, samples, finish, remains);
    }
    void GetStreamDataInfo(uint32_t *w, uint32_t *b, uint32_t *r) {
        ctx_->GetStreamDataInfo(w, b, r);
    }
    int AddStreamData(uint32_t bytes) { return ctx_->AddStreamData(bytes); }
    int GetBufferInfoForResetting(AtracResetBufferInfo *info, int sample, bool *delay) {
        return ctx_->GetBufferInfoForResetting(info, sample, delay);
    }
    int ResetPlayPosition(int sample, int first, int second, bool *delay) {
        return ctx_->ResetPlayPosition(sample, first, second, delay);
    }
    int SetLoopNum(int n) { return ctx_->SetLoopNum(n); }
    int LoopStatus() { return ctx_->LoopStatus(); }
    int RemainingFrames() { return ctx_->RemainingFrames(); }
    int GetSecondBufferInfo(uint32_t *off, uint32_t *size) {
        return ctx_->GetSecondBufferInfo(off, size);
    }
    int SetSecondBuffer(uint32_t addr, uint32_t size) { return ctx_->SetSecondBuffer(addr, size); }
    int GetSoundSample(int *e, int *ls, int *le) { return ctx_->GetSoundSample(e, ls, le); }
    uint32_t GetNextSamples() { return ctx_->GetNextSamples(); }
    int GetNextDecodePosition(int *pos) { return ctx_->GetNextDecodePosition(pos); }
    uint8_t *Ram(uint32_t addr) { return ram_.At(addr); }

private:
    FakeRam &ram_;
    std::unique_ptr<AtracContext> ctx_;
};

struct Golden {
    uint32_t seed;
    uint64_t hash;
};
// Final hashes of RunAtracScenario(seed) computed with PPSSPP v1.20.4's
// AtracCtx2.cpp (mpo_audio_ppsspp_difftest --print-golden).
constexpr Golden kAtracGolden[] = {
#include "audio_atrac_golden.inc"
};

void TestGoldenScenarios() {
    for (const Golden &g : kAtracGolden) {
        FakeRam ram(0x00400000);
        MpoAtracAdapter a(ram);
        const uint64_t got =
            audio_test::RunAtracScenario<MpoAtracAdapter, AtracResetBufferInfo>(a, g.seed).Final();
        if (got != g.hash) {
            std::fprintf(stderr, "FAIL: ATRAC scenario %u: %016llx != PPSSPP %016llx\n", g.seed,
                         static_cast<unsigned long long>(got),
                         static_cast<unsigned long long>(g.hash));
            ++audio_test::g_failures;
        }
    }
    Expect(std::size(kAtracGolden) >= 16, "golden ATRAC scenarios present");
}

} // namespace

int main() {
    TestParseWave();
    TestParseAa3();
    TestManagerDecode();
    TestManagerErrors();
    TestBadFrame();
    TestRealCodecOnGarbage();
    TestGoldenScenarios();
    return audio_test::Finish("audio_atrac_test");
}
