// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceAudio channels, the hardware mixer, the 44.1 kHz -> device resampler and
// (when SDL3 is built) the SDL3 output through SDL's "disk" driver, which
// writes what would be played to a file instead of a sound card.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "audio_channels.h"
#include "audio_errors.h"
#include "audio_test_util.h"
#include "stereo_resampler.h"
#if MPO_HAVE_SDL_AUDIO
#include "sdl_audio_output.h"
#endif

namespace {

using audio_test::Expect;
using audio_test::ExpectEq;
using audio_test::FakeRam;
using namespace mpo::audio;

constexpr uint32_t kSamples = 0x08810000;

class FakeWaitHost : public AudioWaitHost {
public:
    bool IsWaitingOnAudio(uint64_t thread) override { return waiting[thread]; }
    void WakeFromAudio(uint64_t thread, uint32_t result) override {
        waiting[thread] = false;
        woken.push_back({thread, result});
    }
    bool waiting[8]{};
    std::vector<std::pair<uint64_t, uint32_t>> woken;
};

void FillStereo(FakeRam &ram, uint32_t addr, int frames, int16_t l, int16_t r) {
    for (int i = 0; i < frames; ++i) {
        ram.Put16(addr + static_cast<uint32_t>(i) * 4, l);
        ram.Put16(addr + static_cast<uint32_t>(i) * 4 + 2, r);
    }
}

void TestReserveAndErrors() {
    FakeRam ram;
    AudioChannels audio(ram.View());
    ExpectEq(audio.ChReserve(-1, 1024, PSP_AUDIO_FORMAT_STEREO), 7u, "first free channel is 7");
    ExpectEq(audio.ChReserve(-1, 1024, PSP_AUDIO_FORMAT_STEREO), 6u, "then 6");
    ExpectEq(audio.ChReserve(7, 1024, PSP_AUDIO_FORMAT_STEREO), kErrAudioInvalidChannel,
             "already reserved");
    ExpectEq(audio.ChReserve(0, 100, PSP_AUDIO_FORMAT_STEREO),
             kErrAudioOutputSampleDataSizeNotAligned, "count not /64");
    ExpectEq(audio.ChReserve(0, 64, 0x20), kErrAudioInvalidFormat, "bad format");
    ExpectEq(audio.ChReserve(8, 64, PSP_AUDIO_FORMAT_STEREO), kErrAudioInvalidChannel, "channel 8");
    ExpectEq(audio.Output(0, 0x8000, kSamples), kErrAudioChannelNotInit, "not reserved");
    ExpectEq(audio.Output(7, 0x10000, kSamples), kErrAudioInvalidVolume, "volume too high");
    ExpectEq(audio.ChangeChannelVolume(5, 1, 1), kErrAudioChannelNotReserved, "volume unreserved");
    ExpectEq(audio.SetChannelDataLen(7, 128), 0u, "data len");
    ExpectEq(audio.ChRelease(7), 0u, "release");
    ExpectEq(audio.ChRelease(7), kErrAudioChannelNotInit, "release twice");
    ExpectEq(audio.SRCChReserve(256, 12345, 2), kErrAudioInvalidFrequency, "SRC bad rate");
    ExpectEq(audio.SRCChReserve(256, 22050, 4), 0x80000003u, "SRC format 4");
    ExpectEq(audio.Output2Reserve(16), kErrKernelInvalidSize, "Output2 too short");
}

void TestOutputAndMix() {
    FakeRam ram;
    AudioChannels audio(ram.View());
    ExpectEq(audio.ChReserve(0, 128, PSP_AUDIO_FORMAT_STEREO), 0u, "reserve 0");
    ExpectEq(audio.ChReserve(1, 64, PSP_AUDIO_FORMAT_MONO), 1u, "reserve 1 mono");
    FillStereo(ram, kSamples, 128, 1000, -2000);
    for (int i = 0; i < 64; ++i) {
        ram.Put16(kSamples + 0x1000 + static_cast<uint32_t>(i) * 2, 300);
    }
    // Volume 0x8000 copies as is; 0x4000 halves (sample * (vol << 1) >> 16).
    ExpectEq(audio.Output(0, 0x8000, kSamples), 128u, "output returns the sample count");
    ExpectEq(audio.GetChannelRestLen(0), 128, "queued");
    ExpectEq(audio.Output(0, 0x8000, kSamples), kErrAudioChannelBusy, "busy");
    ExpectEq(audio.OutputPanned(1, 0x4000, 0x2000, kSamples + 0x1000), 64u, "mono panned");

    int32_t mix[kHwBlockSize * 2];
    audio.Update(mix);
    ExpectEq(mix[0], 1000 + 150, "left = ch0 + ch1 * 0.5");
    ExpectEq(mix[1], -2000 + 75, "right = ch0 + ch1 * 0.25");
    ExpectEq(audio.GetChannelRestLen(0), 64, "64 left on channel 0");
    ExpectEq(audio.GetChannelRestLen(1), 0, "channel 1 drained");
    audio.Update(mix);
    ExpectEq(mix[126], 1000, "only channel 0 now");
    audio.Update(mix);
    ExpectEq(mix[0], 0, "silence when every queue is empty");

    // Over-range mixes are not clamped here (the resampler clamps).
    ExpectEq(audio.ChReserve(2, 64, PSP_AUDIO_FORMAT_STEREO), 2u, "reserve 2");
    FillStereo(ram, kSamples, 64, 30000, 30000);
    audio.Output(0, 0x8000, kSamples);
    audio.Output(2, 0x8000, kSamples);
    audio.Update(mix);
    ExpectEq(mix[0], 60000, "sum above s16");
}

void TestBlocking() {
    FakeRam ram;
    AudioChannels audio(ram.View());
    FakeWaitHost host;
    audio.SetWaitHost(&host);
    audio.ChReserve(3, 256, PSP_AUDIO_FORMAT_STEREO);
    FillStereo(ram, kSamples, 256, 1, 1);
    bool wait = true;
    AudioChannels::Caller caller{5, true};
    ExpectEq(audio.OutputBlocking(3, 0x8000, kSamples, caller, &wait), 256u, "first block");
    Expect(!wait, "an empty queue does not block");
    ExpectEq(audio.OutputBlocking(3, 0x8000, kSamples, caller, &wait), 256u, "second block");
    Expect(wait, "a busy queue blocks");
    host.waiting[5] = true;
    int32_t mix[kHwBlockSize * 2];
    // The waiter needs the 256 queued samples to drain: 4 updates of 64.
    for (int i = 0; i < 3; ++i) {
        audio.Update(mix);
    }
    Expect(host.woken.empty(), "still waiting after 192 samples");
    audio.Update(mix);
    ExpectEq(host.woken.size(), size_t{1}, "woken after 256 samples");
    if (!host.woken.empty()) {
        ExpectEq(host.woken[0].second, 256u, "wake value is the sample count");
    }
    // Dispatch disabled: no wait, CAN_NOT_WAIT, samples still queued.
    caller.dispatchEnabled = false;
    ExpectEq(audio.OutputBlocking(3, 0x8000, kSamples, caller, &wait), kErrKernelCanNotWait,
             "cannot wait");
    // Releasing a channel wakes its waiters with NOT_RESERVED.
    caller.dispatchEnabled = true;
    host.waiting[6] = true;
    audio.OutputBlocking(3, 0x8000, kSamples, AudioChannels::Caller{6, true}, &wait);
    audio.ChRelease(3);
    ExpectEq(host.woken.back().second, kErrAudioChannelNotReserved, "release wakes waiters");
}

void TestSrcChannel() {
    FakeRam ram;
    AudioChannels audio(ram.View());
    ExpectEq(audio.SRCChReserve(128, 22050, 2), 0u, "SRC 22.05 kHz");
    for (int i = 0; i < 128; ++i) {
        ram.Put16(kSamples + static_cast<uint32_t>(i) * 4, static_cast<int16_t>(i * 100));
        ram.Put16(kSamples + static_cast<uint32_t>(i) * 4 + 2, static_cast<int16_t>(-i * 100));
    }
    bool wait = false;
    ExpectEq(audio.SRCOutputBlocking(0x8000, kSamples, AudioChannels::Caller{}, &wait), 128u,
             "SRC output");
    int32_t mix[kHwBlockSize * 2];
    audio.Update(mix);
    // Half rate: each input sample spans two output samples, interpolated.
    ExpectEq(mix[0], 0, "src 0");
    ExpectEq(mix[2], 50, "src 1 (halfway)");
    ExpectEq(mix[4], 100, "src 2");
    ExpectEq(mix[5], -100, "src 2 right");
    ExpectEq(audio.GetChannelRestLen(0), 0, "unrelated channel");
}

std::vector<int32_t> SineBlock(double *phase, double freq, int frames, int amplitude) {
    std::vector<int32_t> block(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const int32_t v = static_cast<int32_t>(amplitude * std::sin(*phase));
        block[static_cast<size_t>(i) * 2] = v;
        block[static_cast<size_t>(i) * 2 + 1] = -v;
        *phase += 2 * 3.14159265358979 * freq / kHwSampleRate;
    }
    return block;
}

// Counts sign changes of the left channel to estimate the frequency.
double EstimateFrequency(const std::vector<int16_t> &stereo, int rate) {
    int crossings = 0;
    for (size_t i = 2; i < stereo.size(); i += 2) {
        if ((stereo[i - 2] < 0) != (stereo[i] < 0)) {
            ++crossings;
        }
    }
    const double seconds = static_cast<double>(stereo.size() / 2) / rate;
    return crossings / 2.0 / seconds;
}

void TestResampler() {
    StereoResampler resampler;
    double phase = 0;
    std::vector<int16_t> out;
    std::vector<int16_t> chunk(480 * 2);
    // 1 s of 1 kHz at 44.1 kHz in, 48 kHz out, pushing and pulling in step.
    for (int t = 0; t < 100; ++t) {
        for (int b = 0; b < 7; ++b) {
            const auto block = SineBlock(&phase, 1000.0, kHwBlockSize, 20000);
            resampler.PushSamples(block.data(), kHwBlockSize);
        }
        resampler.Mix(chunk.data(), 480, 48000);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    const std::vector<int16_t> tail(out.begin() + 48000, out.end());
    const double freq = EstimateFrequency(tail, 48000);
    Expect(freq > 980 && freq < 1020,
           "1 kHz survives 44.1 -> 48 kHz (" + std::to_string(freq) + " Hz)");
    Expect(tail[0] == -tail[1] || std::abs(tail[0] + tail[1]) <= 1, "right is the inverted left");

    // Overrun: pushing far more than the buffer holds drops blocks.
    StereoResampler small;
    const auto block = SineBlock(&phase, 1000.0, kHwBlockSize, 1000);
    for (int i = 0; i < 200; ++i) {
        small.PushSamples(block.data(), kHwBlockSize);
    }
    Expect(small.GetStats().overruns > 0, "overrun counted");

    // Underrun: an empty resampler pads with the last value.
    StereoResampler empty;
    std::vector<int16_t> silence(64 * 2, 123);
    empty.Mix(silence.data(), 64, 48000);
    Expect(empty.GetStats().underruns > 0, "underrun counted");
    ExpectEq(silence[10], 0, "padding from an empty buffer is zero");

    // Volume and clamping.
    StereoResampler loud;
    std::vector<int32_t> big(64 * 2, 100000);
    loud.PushSamples(big.data(), 64, 0.5f);
    loud.PushSamples(big.data(), 64, 0.5f);
    std::vector<int16_t> o(32 * 2);
    loud.Mix(o.data(), 32, 44100);
    ExpectEq(o[0], 32767, "clamped to s16 after the volume");
}

#if MPO_HAVE_SDL_AUDIO
void SetEnv(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Plays 1 kHz through SDL3's disk driver and reads back what SDL wrote.
void TestSdlDiskOutput() {
    const std::string path = "mpo_audio_output_test_disk.raw";
    std::remove(path.c_str());
    SetEnv("SDL_AUDIO_DISK_OUTPUT_FILE", path);
    SetEnv("SDL_AUDIO_DISK_TIMESCALE", "4"); // 4x faster than real time

    StereoResampler resampler;
    SdlAudioOutput output;
    SdlAudioOutput::Options options;
    options.driver = "disk";
    std::string error;
    if (!output.Open(&resampler, options, &error)) {
        Expect(false, "SDL disk output opens: " + error);
        return;
    }
    ExpectEq(output.DriverName() == "disk", true, "disk driver selected");
    Expect(output.SampleRate() > 0, "device rate known");

    double phase = 0;
    const auto start = std::chrono::steady_clock::now();
    while (output.FramesPulled() < static_cast<uint64_t>(output.SampleRate()) / 2 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        // Keep the resampler near its target fill, like the emulator would.
        if (resampler.GetStats().bufferedFrames < 2000) {
            for (int b = 0; b < 8; ++b) {
                const auto block = SineBlock(&phase, 1000.0, kHwBlockSize, 16000);
                resampler.PushSamples(block.data(), kHwBlockSize);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const int rate = output.SampleRate();
    const uint64_t pulled = output.FramesPulled();
    output.Close();
    Expect(pulled >= static_cast<uint64_t>(rate) / 2, "SDL pulled half a second of audio");

    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<int16_t> pcm(bytes.size() / 2);
    std::memcpy(pcm.data(), bytes.data(), pcm.size() * 2);
    Expect(pcm.size() >= static_cast<size_t>(rate) / 2, "SDL wrote the stream to disk");
    if (pcm.size() > static_cast<size_t>(rate) / 4) {
        // Skip the start (the resampler ramps up from an empty buffer).
        const std::vector<int16_t> tail(pcm.begin() + rate / 4 * 2, pcm.end());
        const double freq = EstimateFrequency(tail, rate);
        Expect(freq > 950 && freq < 1050, "SDL output carries the 1 kHz tone (" +
                                              std::to_string(freq) + " Hz at " +
                                              std::to_string(rate) + " Hz)");
    }
    in.close();
    std::remove(path.c_str());
}
#endif

} // namespace

int main() {
    TestReserveAndErrors();
    TestOutputAndMix();
    TestBlocking();
    TestSrcChannel();
    TestResampler();
#if MPO_HAVE_SDL_AUDIO
    TestSdlDiskOutput();
#else
    std::printf("audio_output_test: SDL3 not built, skipping the device test\n");
#endif
    return audio_test::Finish("audio_output_test");
}
