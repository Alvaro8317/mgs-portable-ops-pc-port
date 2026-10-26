// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_at3_to_wav: decodes an ATRAC3/ATRAC3plus file (RIFF .at3, or OMA/AA3
// with --aa3) to a 16-bit WAV through the same path the game uses: the file
// is placed in a fake PSP memory and played with AtracManager
// (sceAtracSetDataAndGetID + sceAtracDecodeData until it finishes).
//
// Usage: mpo_at3_to_wav [--aa3] [--loops N] input output.wav
// Prints one summary line (key=value) to stdout. Game files stay local.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "atrac_manager.h"
#include "audio_errors.h"
#include "wav_writer.h"

namespace {

constexpr uint32_t kRamBase = 0x08800000;
constexpr uint32_t kBssAddr = 0x08800000;
constexpr uint32_t kOutAddr = 0x08801000;
constexpr uint32_t kVarsAddr = 0x08808000;
constexpr uint32_t kFileAddr = 0x08810000;
// Generous cap: the largest audio stream on the disc is a few MB.
constexpr size_t kMaxFile = 64u << 20;

int Usage() {
    std::fprintf(stderr, "usage: mpo_at3_to_wav [--aa3] [--loops N] input output.wav\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    bool aa3 = false;
    int loops = 0;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--aa3") {
            aa3 = true;
        } else if (arg == "--loops" && i + 1 < argc) {
            loops = std::atoi(argv[++i]);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2) {
        return Usage();
    }

    std::ifstream in(positional[0], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", positional[0].c_str());
        return 1;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (file.empty() || file.size() > kMaxFile) {
        std::fprintf(stderr, "bad input size %zu\n", file.size());
        return 1;
    }

    std::vector<uint8_t> ram(kFileAddr - kRamBase + file.size() + 0x1000, 0);
    std::memcpy(ram.data() + (kFileAddr - kRamBase), file.data(), file.size());
    const mpo::audio::GuestMemory mem(ram.data(), kRamBase, static_cast<uint32_t>(ram.size()));

    mpo::audio::AtracManager atrac(mem);
    if (!atrac.NotifyLoadModule(0x105, kBssAddr, 0x67C)) {
        std::fprintf(stderr, "bad BSS\n");
        return 1;
    }
    const uint32_t size = static_cast<uint32_t>(file.size());
    const int id = aa3 ? atrac.SetAA3DataAndGetID(kFileAddr, size, size, 0)
                       : atrac.SetDataAndGetID(kFileAddr, static_cast<int>(size));
    if (id < 0) {
        std::fprintf(stderr, "SetDataAndGetID failed: %08x\n", static_cast<uint32_t>(id));
        return 1;
    }
    mpo::audio::AtracContext *ctx = atrac.Get(id);
    const mpo::audio::SceAtracIdInfo &info = ctx->Info();

    const uint32_t endAddr = kVarsAddr, loopStartAddr = kVarsAddr + 4, loopEndAddr = kVarsAddr + 8;
    atrac.GetSoundSample(id, endAddr, loopStartAddr, loopEndAddr);
    const int endSample = static_cast<int>(mem.Read32(endAddr));
    const int loopStart = static_cast<int>(mem.Read32(loopStartAddr));
    const int loopEnd = static_cast<int>(mem.Read32(loopEndAddr));
    if (loops != 0) {
        atrac.SetLoopNum(id, loops);
    }

    const int channels = ctx->GetOutputChannels();
    std::vector<int16_t> pcm;
    const uint32_t numAddr = kVarsAddr + 16, finishAddr = kVarsAddr + 20,
                   remainAddr = kVarsAddr + 24;
    int frames = 0;
    int badFrames = 0;
    uint32_t lastError = 0;
    // Bound the loop: a looping track with --loops -1 would never finish.
    const int maxFrames = 1 << 20;
    while (frames < maxFrames) {
        const uint32_t ret = atrac.DecodeData(id, kOutAddr, numAddr, finishAddr, remainAddr);
        if (ret == mpo::audio::kErrAtracApiFail) {
            ++badFrames;
            lastError = ret;
            if (badFrames > 16) {
                break;
            }
            continue;
        }
        if (ret != 0) {
            lastError = ret;
            break;
        }
        ++frames;
        const uint32_t n = mem.Read32(numAddr);
        const auto *out = reinterpret_cast<const int16_t *>(
            mem.Ptr(kOutAddr, n * static_cast<uint32_t>(channels) * 2));
        if (out != nullptr) {
            pcm.insert(pcm.end(), out, out + n * static_cast<uint32_t>(channels));
        }
        if (mem.Read32(finishAddr) != 0) {
            break;
        }
    }

    if (!mpo::audio::WriteWav16(positional[1], pcm, channels, 44100)) {
        std::fprintf(stderr, "cannot write %s\n", positional[1].c_str());
        return 1;
    }
    const size_t sampleFrames = pcm.size() / static_cast<size_t>(channels);
    int64_t sumSquares = 0;
    int peak = 0;
    for (int16_t s : pcm) {
        sumSquares += static_cast<int64_t>(s) * s;
        peak = std::max(peak, std::abs(static_cast<int>(s)));
    }
    std::printf(
        "codec=%s channels=%d bytes_per_frame=%d end_sample=%d loop_start=%d loop_end=%d "
        "frames=%d bad_frames=%d samples=%zu seconds=%.3f peak=%d rms=%.1f last_error=%08x\n",
        info.codec == 0x1000 ? "atrac3plus" : "atrac3", info.numChan, info.sampleSize, endSample,
        loopStart, loopEnd, frames, badFrames, sampleFrames,
        static_cast<double>(sampleFrames) / 44100.0, peak,
        pcm.empty() ? 0.0
                    : std::sqrt(static_cast<double>(sumSquares) / static_cast<double>(pcm.size())),
        lastError);
    return 0;
}
