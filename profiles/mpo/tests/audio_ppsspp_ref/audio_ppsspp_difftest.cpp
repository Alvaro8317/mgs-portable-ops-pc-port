// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_audio_ppsspp_difftest: runs the same seeded scenarios through mpo_audio
// and through PPSSPP v1.20.4's own SasAudio.cpp, SasReverb.cpp, AtracCtx2.cpp,
// AtracTrack.cpp and Atrac3Standalone.cpp (compiled from the PPSSPP checkout
// in MPO_PPSSPP_SOURCE_DIR), and requires identical traces.
//
//   mpo_audio_ppsspp_difftest [--seeds N]          random scenarios (default 300)
//   mpo_audio_ppsspp_difftest --print-golden       golden lines for the CI tests
//   mpo_audio_ppsspp_difftest --at3 FILE...        decode real files with both
//                                                   (bit-exact PCM comparison)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// PPSSPP (global namespace).
#include "Core/HLE/AtracCtx2.h"
#include "Core/HW/SasAudio.h"
#include "Core/Util/AtracTrack.h"
#include "glue.h"

// mpo_audio.
#include "atrac_context.h"
#include "audio_scenarios.h"
#include "sas_audio.h"

namespace {

using audio_test::FakeRam;

void UseRam(FakeRam &ram) {
    g_ppssppRefRam.base = ram.bytes().data();
    g_ppssppRefRam.baseAddress = FakeRam::kBase;
    g_ppssppRefRam.size = static_cast<uint32_t>(ram.bytes().size());
}

// ---------------------------------------------------------------- SAS adapters

class PpssppSas {
public:
    explicit PpssppSas(FakeRam &ram) : ram_(ram) { UseRam(ram); }
    ::SasInstance &sas() { return sas_; }
    void KeyOn(int voice) { sas_.voices[voice].KeyOn(); }
    void Mix(uint32_t out, uint32_t in, int l, int r) {
        UseRam(ram_);
        sas_.Mix(out, in, l, r, false);
    }
    uint8_t *Ram(uint32_t addr) { return ram_.At(addr); }

private:
    FakeRam &ram_;
    ::SasInstance sas_;
};

class MpoSas {
public:
    explicit MpoSas(FakeRam &ram) : ram_(ram), mem_(ram.View()) {}
    mpo::audio::SasInstance &sas() { return sas_; }
    void KeyOn(int voice) { sas_.voices[voice].KeyOn(mem_); }
    void Mix(uint32_t out, uint32_t in, int l, int r) { sas_.Mix(mem_, out, in, l, r, false); }
    uint8_t *Ram(uint32_t addr) { return ram_.At(addr); }

private:
    FakeRam &ram_;
    mpo::audio::GuestMemory mem_;
    mpo::audio::SasInstance sas_;
};

// ---------------------------------------------------------------- ATRAC adapters

class PpssppAtrac {
public:
    explicit PpssppAtrac(FakeRam &ram) : ram_(ram) { UseRam(ram); }
    void Create(uint32_t ctxAddr, int codec) { ctx_ = std::make_unique<::Atrac2>(ctxAddr, codec); }
    int SetData(uint32_t buffer, uint32_t readSize, uint32_t bufferSize, int outChannels) {
        ::Track track;
        return ctx_->SetData(track, buffer, readSize, bufferSize, 0, outChannels, false);
    }
    uint32_t DecodeData(uint32_t outAddr, int *samples, int *finish, int *remains) {
        return ctx_->DecodeData(outAddr ? Memory::GetPointer(outAddr) : nullptr, outAddr, samples,
                                finish, remains);
    }
    void GetStreamDataInfo(uint32_t *w, uint32_t *b, uint32_t *r) {
        ctx_->GetStreamDataInfo(w, b, r);
    }
    int AddStreamData(uint32_t bytes) { return ctx_->AddStreamData(bytes); }
    int GetBufferInfoForResetting(::AtracResetBufferInfo *info, int sample, bool *delay) {
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
    int OutputChannels() const { return ctx_->GetOutputChannels(); }
    uint8_t *Ram(uint32_t addr) { return ram_.At(addr); }

private:
    FakeRam &ram_;
    std::unique_ptr<::Atrac2> ctx_;
};

class FakeMpoDecoder : public mpo::audio::AudioDecoder {
public:
    FakeMpoDecoder(uint32_t codec, int channels) : codec_(codec), channels_(channels) {}
    bool Decode(const uint8_t *in, int inbytes, int *consumed, int outCh, int16_t *out,
                int *outSamples) override {
        return audio_test::FakeAtracDecode(codec_ == 0x1000 ? 2048 : 1024, channels_, in, inbytes,
                                           consumed, outCh, out, outSamples);
    }
    bool IsOK() const override { return true; }

private:
    uint32_t codec_;
    int channels_;
};

class MpoAtrac {
public:
    MpoAtrac(FakeRam &ram, bool fake) : ram_(ram), fake_(fake) {}
    void Create(uint32_t ctxAddr, int codec) {
        mpo::audio::AtracDecoderFactory factory;
        if (fake_) {
            factory = [](uint32_t c, int, int ch) -> std::unique_ptr<mpo::audio::AudioDecoder> {
                return std::make_unique<FakeMpoDecoder>(c, ch);
            };
        }
        ctx_ = std::make_unique<mpo::audio::AtracContext>(ram_.View(), ctxAddr, codec, factory);
    }
    int SetData(uint32_t buffer, uint32_t readSize, uint32_t bufferSize, int outChannels) {
        mpo::audio::Track track;
        return ctx_->SetData(track, buffer, readSize, bufferSize, 0, outChannels, false);
    }
    uint32_t DecodeData(uint32_t outAddr, int *samples, int *finish, int *remains) {
        return ctx_->DecodeData(outAddr, samples, finish, remains);
    }
    void GetStreamDataInfo(uint32_t *w, uint32_t *b, uint32_t *r) {
        ctx_->GetStreamDataInfo(w, b, r);
    }
    int AddStreamData(uint32_t bytes) { return ctx_->AddStreamData(bytes); }
    int GetBufferInfoForResetting(mpo::audio::AtracResetBufferInfo *info, int sample, bool *delay) {
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
    int OutputChannels() const { return ctx_->GetOutputChannels(); }
    uint8_t *Ram(uint32_t addr) { return ram_.At(addr); }

private:
    FakeRam &ram_;
    bool fake_;
    std::unique_ptr<mpo::audio::AtracContext> ctx_;
};

// ---------------------------------------------------------------- comparisons

bool SameTrace(const std::vector<uint64_t> &a, const std::vector<uint64_t> &b, const char *what,
               uint32_t seed) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            std::fprintf(stderr, "%s seed %u: step %zu differs (%016llx vs PPSSPP %016llx)\n", what,
                         seed, i, static_cast<unsigned long long>(a[i]),
                         static_cast<unsigned long long>(b[i]));
            return false;
        }
    }
    if (a.size() != b.size()) {
        std::fprintf(stderr, "%s seed %u: %zu steps vs PPSSPP %zu\n", what, seed, a.size(),
                     b.size());
        return false;
    }
    return true;
}

audio_test::SasTrace RunPpssppSas(uint32_t seed) {
    FakeRam ram(audio_test::kScenarioRam);
    PpssppSas a(ram);
    return audio_test::RunSasScenario(a, seed);
}

audio_test::SasTrace RunMpoSas(uint32_t seed) {
    FakeRam ram(audio_test::kScenarioRam);
    MpoSas a(ram);
    return audio_test::RunSasScenario(a, seed);
}

audio_test::AtracTrace RunPpssppAtrac(uint32_t seed) {
    g_ppssppRefFakeDecoder = true;
    FakeRam ram(0x00400000);
    PpssppAtrac a(ram);
    return audio_test::RunAtracScenario<PpssppAtrac, ::AtracResetBufferInfo>(a, seed);
}

audio_test::AtracTrace RunMpoAtrac(uint32_t seed) {
    FakeRam ram(0x00400000);
    MpoAtrac a(ram, true);
    return audio_test::RunAtracScenario<MpoAtrac, mpo::audio::AtracResetBufferInfo>(a, seed);
}

int CompareFiles(int argc, char **argv, int first) {
    int failures = 0;
    for (int i = first; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        if (file.empty() || file.size() > 0x00F00000) {
            std::fprintf(stderr, "%s: skipped (size %zu)\n", argv[i], file.size());
            continue;
        }
        // The context's codec field must match the file before SetData
        // (sceAtracSetDataAndGetID picks the ID by codec).
        const bool plus = file.size() > 22 && file[20] == 0xFE && file[21] == 0xFF;
        g_ppssppRefFakeDecoder = false;
        FakeRam ramA(0x01000000);
        FakeRam ramB(0x01000000);
        PpssppAtrac ref(ramA);
        MpoAtrac mpo(ramB, false);
        int retRef = 0;
        int retMpo = 0;
        std::memcpy(ramA.At(audio_test::kAtracBuf), file.data(), file.size());
        std::memcpy(ramB.At(audio_test::kAtracBuf), file.data(), file.size());
        UseRam(ramA);
        ref.Create(audio_test::kCtxAddr, plus ? 0x1000 : 0x1001);
        mpo.Create(audio_test::kCtxAddr, plus ? 0x1000 : 0x1001);
        const uint32_t size = static_cast<uint32_t>(file.size());
        retRef = ref.SetData(audio_test::kAtracBuf, size, size, 2);
        retMpo = mpo.SetData(audio_test::kAtracBuf, size, size, 2);
        std::vector<int16_t> pcmRef;
        std::vector<int16_t> pcmMpo;
        auto pump = [](auto &a, FakeRam &ram, std::vector<int16_t> &pcm) {
            for (int f = 0; f < (1 << 20); ++f) {
                int samples = 0;
                int finish = 0;
                int remains = 0;
                const uint32_t r = a.DecodeData(audio_test::kAtracOut, &samples, &finish, &remains);
                if (r != 0 && r != 0x80630002u) {
                    break;
                }
                const auto *out = reinterpret_cast<const int16_t *>(ram.At(audio_test::kAtracOut));
                pcm.insert(pcm.end(), out, out + samples * a.OutputChannels());
                if (finish) {
                    break;
                }
            }
        };
        if (retRef >= 0) {
            UseRam(ramA);
            pump(ref, ramA, pcmRef);
        }
        if (retMpo >= 0) {
            pump(mpo, ramB, pcmMpo);
        }
        const bool same =
            retRef == retMpo && pcmRef == pcmMpo &&
            std::memcmp(ramA.At(audio_test::kCtxAddr), ramB.At(audio_test::kCtxAddr), 256) == 0;
        std::printf("%s: %s samples=%zu ret=%08x\n", argv[i], same ? "identical" : "DIFFERENT",
                    pcmMpo.size() / 2, static_cast<uint32_t>(retMpo));
        if (!same) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    int seeds = 300;
    bool printGolden = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--at3") {
            return CompareFiles(argc, argv, i + 1);
        } else if (arg == "--seeds" && i + 1 < argc) {
            seeds = std::atoi(argv[++i]);
        } else if (arg == "--print-golden") {
            printGolden = true;
        }
    }

    if (printGolden) {
        std::printf("// SAS\n");
        for (uint32_t seed = 1; seed <= 12; ++seed) {
            std::printf("{%uu, 0x%016llxull},\n", seed,
                        static_cast<unsigned long long>(RunPpssppSas(seed).Final()));
        }
        std::printf("// ATRAC\n");
        for (uint32_t seed = 1; seed <= 24; ++seed) {
            std::printf("{%uu, 0x%016llxull},\n", seed,
                        static_cast<unsigned long long>(RunPpssppAtrac(seed).Final()));
        }
        return 0;
    }

    int failures = 0;
    size_t sasSteps = 0;
    size_t atracSteps = 0;
    size_t atracShort = 0; // scenarios where SetData failed
    for (int s = 1; s <= seeds; ++s) {
        const uint32_t seed = static_cast<uint32_t>(s);
        const auto ref = RunPpssppSas(seed);
        const auto mine = RunMpoSas(seed);
        sasSteps += ref.steps.size();
        if (!SameTrace(mine.steps, ref.steps, "SAS", seed)) {
            ++failures;
        }
        const auto refA = RunPpssppAtrac(seed);
        const auto mineA = RunMpoAtrac(seed);
        atracSteps += refA.steps.size();
        atracShort += refA.steps.size() <= 3 ? 1 : 0;
        if (!SameTrace(mineA.steps, refA.steps, "ATRAC", seed)) {
            ++failures;
        }
    }
    std::printf("mpo_audio_ppsspp_difftest: %d seeds, %zu SAS grains, %zu ATRAC steps (%zu "
                "scenarios rejected by SetData), %d "
                "mismatching scenario(s)\n",
                seeds, sasSteps, atracSteps, atracShort, failures);
    return failures == 0 ? 0 : 1;
}
