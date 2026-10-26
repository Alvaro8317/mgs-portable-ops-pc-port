// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/StereoResampler.cpp @ v1.20.4
// Copyright (c) 2015- PPSSPP Project and Dolphin Project (GPL-2.0-or-later).
//
// Changes from upstream: settings are constructor arguments instead of
// g_Config/System properties, no fast-forward handling, no SIMD (the scalar
// clamp gives the same values), and the debug statistics are reduced.

#include "stereo_resampler.h"

#include <algorithm>
#include <cstring>

#include "guest_memory.h"

namespace mpo::audio {
namespace {

// These must be powers of 2.
constexpr int MAX_BUFSIZE_DEFAULT = 4096; // 2*64ms
constexpr int MAX_BUFSIZE_EXTRA = 8192;

constexpr int TARGET_BUFSIZE_MARGIN = 512;

constexpr int TARGET_BUFSIZE_DEFAULT = 1680; // 40 ms
constexpr int TARGET_BUFSIZE_EXTRA = 3360;   // 80 ms

constexpr float MAX_FREQ_SHIFT = 600.0f; // how far off can we be from 44100 Hz
constexpr float CONTROL_FACTOR = 0.2f;   // in freq_shift per fifo size offset
constexpr float CONTROL_AVG = 32.0f;

// Upstream: s1 + (((s2 - s1) * frac) >> 16) in int, which can overflow for a
// full-scale step; computed with the same wrapping, without UB.
int16_t MixSingleSample(int16_t s1, int16_t s2, uint16_t frac) {
    const int32_t delta =
        static_cast<int32_t>(static_cast<uint32_t>(s2 - s1) * static_cast<uint32_t>(frac));
    const int32_t value = s1 + (delta >> 16);
    if (value < -32767) {
        return -32767;
    } else if (value > 32767) {
        return 32767;
    }
    return static_cast<int16_t>(value);
}

} // namespace

StereoResampler::StereoResampler(bool extra_buffering, int device_frames_per_buffer) {
    if (extra_buffering) {
        maxBufsize_ = MAX_BUFSIZE_EXTRA;
        targetBufsize_ = TARGET_BUFSIZE_EXTRA;
    } else {
        maxBufsize_ = MAX_BUFSIZE_DEFAULT;
        targetBufsize_ = TARGET_BUFSIZE_DEFAULT;
        if (device_frames_per_buffer > 0 &&
            targetBufsize_ < device_frames_per_buffer + TARGET_BUFSIZE_MARGIN) {
            targetBufsize_ = std::min(4096, device_frames_per_buffer + TARGET_BUFSIZE_MARGIN);
            if (targetBufsize_ * 2 > MAX_BUFSIZE_DEFAULT) {
                maxBufsize_ = MAX_BUFSIZE_EXTRA;
            }
        }
    }
    // Need to have space for the worst case.
    buffer_.assign(static_cast<size_t>(MAX_BUFSIZE_EXTRA) * 2, 0);
}

void StereoResampler::Clear() {
    std::fill(buffer_.begin(), buffer_.end(), int16_t{0});
}

void StereoResampler::Mix(int16_t *samples, unsigned int numSamples, int sample_rate) {
    if (!samples || sample_rate <= 0) {
        return;
    }

    unsigned int currentSample;

    // The writer only ever increases indexW_, so caching it is safe.
    uint32_t indexR = indexR_.load();
    const uint32_t indexW = indexW_.load();

    const uint32_t INDEX_MASK = static_cast<uint32_t>(maxBufsize_ * 2 - 1);

    lastBufSize_ = static_cast<int>(((indexW - indexR) & INDEX_MASK) / 2);

    // Drift prevention mechanism.
    const float numLeft = static_cast<float>(((indexW - indexR) & INDEX_MASK) / 2);

    // numLeftI_ here becomes a lowpass filtered version of numLeft.
    numLeftI_ = (numLeft + numLeftI_ * (CONTROL_AVG - 1.0f)) / CONTROL_AVG;

    // Keep the buffer size around the target by adjusting the speed.
    float offset = (numLeftI_ - static_cast<float>(targetBufsize_)) * CONTROL_FACTOR;
    offset = std::clamp(offset, -MAX_FREQ_SHIFT, MAX_FREQ_SHIFT);

    outputSampleRateHz_ = static_cast<float>(inputSampleRateHz_) + offset;
    const uint32_t ratio = static_cast<uint32_t>(
        65536.0 * static_cast<double>(outputSampleRateHz_) / static_cast<double>(sample_rate));
    uint32_t frac = frac_;
    for (currentSample = 0; currentSample < numSamples * 2; currentSample += 2) {
        if (((indexW - indexR) & INDEX_MASK) <= 2) {
            // Ran out!
            underrunCount_++;
            break;
        }
        const uint32_t indexR2 = indexR + 2; // next sample
        const int16_t l1 = buffer_[indexR & INDEX_MASK];
        const int16_t r1 = buffer_[(indexR + 1) & INDEX_MASK];
        const int16_t l2 = buffer_[indexR2 & INDEX_MASK];
        const int16_t r2 = buffer_[(indexR2 + 1) & INDEX_MASK];
        samples[currentSample] = MixSingleSample(l1, l2, static_cast<uint16_t>(frac));
        samples[currentSample + 1] = MixSingleSample(r1, r2, static_cast<uint16_t>(frac));
        frac += ratio;
        indexR += 2 * (frac >> 16);
        frac &= 0xffff;
    }
    frac_ = frac;

    // Padding with the last value to reduce clicking.
    const int16_t s0 = buffer_[(indexR - 1) & INDEX_MASK];
    const int16_t s1 = buffer_[(indexR - 2) & INDEX_MASK];
    for (; currentSample < numSamples * 2; currentSample += 2) {
        samples[currentSample] = s0;
        samples[currentSample + 1] = s1;
    }

    indexR_.store(indexR);
}

void StereoResampler::PushSamples(const int32_t *samples, unsigned int numSamples, float volume) {
    const uint32_t INDEX_MASK = static_cast<uint32_t>(maxBufsize_ * 2 - 1);
    const uint32_t indexW = indexW_.load();
    const uint32_t cap = static_cast<uint32_t>(maxBufsize_) * 2;

    // indexW == indexR results in empty buffer, so indexR must always be
    // smaller than indexW.
    if (numSamples * 2 + ((indexW - indexR_.load()) & INDEX_MASK) >= cap) {
        overrunCount_++;
        return;
    }

    // 12-bit volume.
    const int vol = static_cast<int>(volume * 4096.0f);
    auto convert = [vol](int32_t v) -> int16_t {
        if (vol >= 4096) {
            return ClampS16(v);
        } else if (vol <= 0) {
            return 0;
        }
        return ClampS16((v * vol) >> 12);
    };

    for (uint32_t i = 0; i < numSamples * 2; ++i) {
        buffer_[(indexW + i) & INDEX_MASK] = convert(samples[i]);
    }
    indexW_.store(indexW + numSamples * 2);
}

StereoResampler::Stats StereoResampler::GetStats() const {
    return {lastBufSize_, targetBufsize_, underrunCount_.load(), overrunCount_.load(),
            outputSampleRateHz_};
}

} // namespace mpo::audio
