// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host audio output over SDL3: opens the default playback device at its own
// sample rate and, from SDL's audio thread, pulls stereo s16 frames out of a
// StereoResampler (which converts 44.1 kHz to the device rate). The emulator
// side only calls StereoResampler::PushSamples().

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "stereo_resampler.h"

struct SDL_AudioStream;

namespace mpo::audio {

class SdlAudioOutput {
public:
    struct Options {
        // SDL audio driver to use ("dummy", "disk", "pulseaudio"...); empty
        // keeps SDL's choice (or the SDL_AUDIO_DRIVER environment variable).
        std::string driver;
        // Forces the stream rate; 0 uses the device's preferred rate.
        int sample_rate = 0;
    };

    SdlAudioOutput() = default;
    ~SdlAudioOutput();
    SdlAudioOutput(const SdlAudioOutput &) = delete;
    SdlAudioOutput &operator=(const SdlAudioOutput &) = delete;

    // `resampler` must outlive the output (or Close() first).
    bool Open(StereoResampler *resampler, const Options &options, std::string *error);
    void Close();

    bool IsOpen() const { return stream_ != nullptr; }
    int SampleRate() const { return sampleRate_; }
    // Frames per device period reported by SDL (0 if unknown).
    int DeviceFrames() const { return deviceFrames_; }
    // Frames handed to SDL so far (for tests and stats).
    uint64_t FramesPulled() const { return framesPulled_.load(); }
    std::string DriverName() const;

private:
    static void Callback(void *userdata, SDL_AudioStream *stream, int additional_amount,
                         int total_amount);

    StereoResampler *resampler_ = nullptr;
    SDL_AudioStream *stream_ = nullptr;
    bool initedSubsystem_ = false;
    int sampleRate_ = 0;
    int deviceFrames_ = 0;
    std::vector<int16_t> scratch_;
    std::atomic<uint64_t> framesPulled_{0};
};

} // namespace mpo::audio
