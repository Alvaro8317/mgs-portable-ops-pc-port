// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/StereoResampler.h @ v1.20.4
// Copyright (c) 2015- PPSSPP Project and Dolphin Project (GPL-2.0-or-later).
//
// Single-producer / single-consumer bridge between the emulated audio clock
// and the host device: the emulator pushes 44.1 kHz stereo blocks, the device
// callback pulls at its own rate. A linear resampler converts the rate, and
// its ratio is nudged (up to +/-600 Hz) to keep the buffer near a target
// fill, which absorbs clock drift between the two sides.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpo::audio {

class StereoResampler {
public:
    // `extra_buffering` doubles the buffer (PPSSPP's "extra audio buffering").
    // `device_frames_per_buffer` (0 = unknown) raises the target fill so one
    // device period always fits.
    explicit StereoResampler(bool extra_buffering = false, int device_frames_per_buffer = 0);

    // Device/audio thread: writes `numSamples` stereo frames at `sampleRate`.
    void Mix(int16_t *samples, unsigned int numSamples, int sampleRate);

    // Emulator thread: clamps to s16 (with `volume` in 0..1) and queues.
    // Drops the block if the buffer is full (an overrun).
    void PushSamples(const int32_t *samples, unsigned int numSamples, float volume = 1.0f);

    void Clear();

    struct Stats {
        int bufferedFrames;
        int targetFrames;
        int underruns;
        int overruns;
        float outputSampleRateHz;
    };
    Stats GetStats() const;

private:
    int maxBufsize_;
    int targetBufsize_;
    int inputSampleRateHz_ = 44100;

    std::vector<int16_t> buffer_;
    std::atomic<uint32_t> indexW_{0};
    std::atomic<uint32_t> indexR_{0};
    float numLeftI_ = 0.0f;

    uint32_t frac_ = 0;
    std::atomic<float> outputSampleRateHz_{0.0f};
    std::atomic<int> lastBufSize_{0};

    std::atomic<int> underrunCount_{0};
    std::atomic<int> overrunCount_{0};
};

} // namespace mpo::audio
