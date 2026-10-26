// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/sceAudio.cpp and Core/HLE/__sceAudio.cpp
// @ v1.20.4, with AdjustVolumeBlock from Core/Util/AudioFormat.cpp.
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Changes from upstream: no globals, no CoreTiming (the caller runs Update()
// every 64 samples), thread waits go through AudioWaitHost, no microphone,
// routing or WAV dumping, and two invalid-memory corner cases are made
// deterministic (noted inline). Sample values are unchanged.

#include "audio_channels.h"

#include <algorithm>
#include <cstring>

#include "audio_errors.h"

namespace mpo::audio {
namespace {

constexpr uint32_t PSP_AUDIO_ERROR_SRC_FORMAT_4 = 0x80000003;

int16_t ApplySampleVolume(int16_t sample, int vol) {
    return ClampS16((sample * vol) >> 16);
}

// We sacrifice a little volume precision to fit in 32 bits, for speed.
int16_t ApplySampleVolume20Bit(int16_t sample, int vol20) {
    return ClampS16((sample * (vol20 >> 4)) >> 12);
}

// Upstream: ((a << 16) + (b - a) * frac) >> 16 in int, which can overflow
// (UB, wraps on every PPSSPP target). Same value computed with defined wrapping.
int SrcLerp(int16_t a, int16_t b, uint16_t frac) {
    const uint32_t sum = static_cast<uint32_t>(a) * 65536u +
                         static_cast<uint32_t>(b - a) * static_cast<uint32_t>(frac);
    return static_cast<int32_t>(sum) >> 16;
}

bool SRCFrequencyAllowed(uint32_t freq) {
    if (freq == 44100 || freq == 22050 || freq == 11025) {
        return true;
    }
    return freq == 48000 || freq == 32000 || freq == 24000 || freq == 16000 || freq == 12000 ||
           freq == 8000;
}

} // namespace

void AdjustVolumeBlock(int16_t *out, const int16_t *in, size_t size, int leftVol, int rightVol) {
    if (leftVol <= 0x7fff && -leftVol <= 0x8000 && rightVol <= 0x7fff && -rightVol <= 0x8000) {
        for (size_t i = 0; i < size; i += 2) {
            out[i] = ApplySampleVolume(in[i], leftVol);
            out[i + 1] = ApplySampleVolume(in[i + 1], rightVol);
        }
    } else {
        for (size_t i = 0; i < size; i += 2) {
            out[i] = ApplySampleVolume20Bit(in[i], leftVol);
            out[i + 1] = ApplySampleVolume20Bit(in[i + 1], rightVol);
        }
    }
}

// ---------------------------------------------------------------- SampleRing

void AudioChannels::SampleRing::clear() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    std::fill(storage_.begin(), storage_.end(), int16_t{0});
}

void AudioChannels::SampleRing::Push(const int16_t *data, size_t n) {
    n = std::min(n, kCapacity - count_);
    for (size_t i = 0; i < n; ++i) {
        storage_[tail_] = data[i];
        tail_ = (tail_ + 1) % kCapacity;
    }
    count_ += n;
}

void AudioChannels::SampleRing::Pop(size_t size, const int16_t **src1, size_t *sz1,
                                    const int16_t **src2, size_t *sz2) {
    size = std::min(size, count_);
    if (head_ + size < kCapacity) {
        *src1 = &storage_[head_];
        *sz1 = size;
        head_ += size;
        *src2 = nullptr;
        *sz2 = 0;
    } else {
        *src1 = &storage_[head_];
        *sz1 = kCapacity - head_;
        head_ = size - *sz1;
        *src2 = &storage_[0];
        *sz2 = head_;
    }
    count_ -= size;
}

// ---------------------------------------------------------------- AudioChannels

AudioChannels::AudioChannels(const GuestMemory &mem) : mem_(mem) {
    for (Channel &chan : chans_) {
        Clear(chan);
    }
}

void AudioChannels::Clear(Channel &chan) {
    chan.reserved = false;
    chan.leftVolume = 0;
    chan.rightVolume = 0;
    chan.format = 0;
    chan.sampleAddress = 0;
    chan.sampleCount = 0;
    chan.queue.clear();
    chan.waitingThreads.clear();
}

void AudioChannels::Reset(Channel &chan) {
    WakeThreads(chan, static_cast<int>(kErrAudioChannelNotReserved), 0x7FFFFFFF);
    Clear(chan);
}

void AudioChannels::WakeThreads(Channel &chan, int result, int step) {
    for (size_t w = 0; w < chan.waitingThreads.size(); ++w) {
        Waiter &waitInfo = chan.waitingThreads[w];
        waitInfo.numSamples -= step;

        // If it's done (there will still be samples on queue) and actually
        // still waiting, wake it up.
        const bool waiting = waitHost_ != nullptr && waitHost_->IsWaitingOnAudio(waitInfo.thread);
        if (waitInfo.numSamples <= 0 && waiting) {
            const uint32_t ret = result == 0 ? waitInfo.result : kErrAudioChannelNotReserved;
            waitHost_->WakeFromAudio(waitInfo.thread, ret);
            chan.waitingThreads.erase(chan.waitingThreads.begin() + static_cast<ptrdiff_t>(w));
            --w;
        } else if (!waiting) {
            // The thread stopped waiting, so stop trying to wake it.
            chan.waitingThreads.erase(chan.waitingThreads.begin() + static_cast<ptrdiff_t>(w));
            --w;
        }
    }
}

uint32_t AudioChannels::Enqueue(int chanNum, bool blocking, const Caller *caller, bool *must_wait) {
    if (must_wait) {
        *must_wait = false;
    }
    Channel &chan = chans_[static_cast<size_t>(chanNum)];
    uint32_t ret = chan.sampleCount;

    if (chan.sampleAddress == 0) {
        // For some reason, multichannel audio lies and returns the sample count here.
        if (chanNum == PSP_AUDIO_CHANNEL_SRC || chanNum == PSP_AUDIO_CHANNEL_OUTPUT2) {
            ret = 0;
        }
    }

    // If there's anything on the queue at all, it should be busy.
    if (chan.queue.size() > 0) {
        if (blocking) {
            const int blockSamples = static_cast<int>(chan.queue.size() / 2);
            if (caller != nullptr && caller->dispatchEnabled && waitHost_ != nullptr) {
                chan.waitingThreads.push_back({caller->thread, blockSamples, ret});
                if (must_wait) {
                    *must_wait = true;
                }
            } else {
                ret = kErrKernelCanNotWait;
            }
            // Fall through to the sample queueing; the PSP would enqueue after
            // blocking.
        } else {
            // Non-blocking doesn't even enqueue.
            return kErrAudioChannelBusy;
        }
    }

    if (chan.sampleAddress == 0) {
        return ret;
    }

    // NOTE (upstream): this reads the samples at enqueue time; the PSP reads
    // them when dequeuing (PPSSPP issue #20095).
    int leftVol = chan.leftVolume;
    int rightVol = chan.rightVolume;

    if (leftVol == (1 << 15) && rightVol == (1 << 15) && chan.format == PSP_AUDIO_FORMAT_STEREO) {
        // The volume (1 << 15) doesn't change the values: direct copy.
        const size_t totalSamples = static_cast<size_t>(chan.sampleCount) * 2;
        std::vector<int16_t> tmp(totalSamples, 0);
        // Upstream queues stale data when the buffer is invalid; zeros here.
        mem_.CopyFrom(tmp.data(), chan.sampleAddress, static_cast<uint32_t>(totalSamples * 2));
        chan.queue.Push(tmp.data(), totalSamples);
    } else {
        // Maximum volume is 0xFFFFF so the left shift is no issue; this way we
        // can shift by 16.
        leftVol <<= 1;
        rightVol <<= 1;

        if (chan.format == PSP_AUDIO_FORMAT_STEREO) {
            const size_t totalSamples = static_cast<size_t>(chan.sampleCount) * 2;
            const auto *sampleData = reinterpret_cast<const int16_t *>(
                mem_.Ptr(chan.sampleAddress, static_cast<uint32_t>(totalSamples * 2)));
            if (sampleData != nullptr) {
                std::vector<int16_t> tmp(totalSamples, 0);
                AdjustVolumeBlock(tmp.data(), sampleData, totalSamples, leftVol, rightVol);
                chan.queue.Push(tmp.data(), totalSamples);
            }
        } else if (chan.format == PSP_AUDIO_FORMAT_MONO) {
            // Rare, so unoptimized. Expands to stereo.
            std::vector<int16_t> tmp(static_cast<size_t>(chan.sampleCount) * 2, 0);
            for (uint32_t i = 0; i < chan.sampleCount; i++) {
                int16_t sample = 0;
                mem_.CopyFrom(&sample, chan.sampleAddress + 2 * i, 2);
                tmp[i * 2] = ApplySampleVolume(sample, leftVol);
                tmp[i * 2 + 1] = ApplySampleVolume(sample, rightVol);
            }
            chan.queue.Push(tmp.data(), tmp.size());
        }
    }
    return ret;
}

uint32_t AudioChannels::OutputBlocking(uint32_t chan, int vol, uint32_t samplePtr,
                                       const Caller &caller, bool *must_wait) {
    if (must_wait) {
        *must_wait = false;
    }
    if (vol > 0xFFFF) {
        return kErrAudioInvalidVolume;
    } else if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotInit;
    }
    if (vol >= 0) {
        chans_[chan].leftVolume = vol;
        chans_[chan].rightVolume = vol;
    }
    chans_[chan].sampleAddress = samplePtr;
    return Enqueue(static_cast<int>(chan), true, &caller, must_wait);
}

uint32_t AudioChannels::OutputPannedBlocking(uint32_t chan, int leftvol, int rightvol,
                                             uint32_t samplePtr, const Caller &caller,
                                             bool *must_wait) {
    if (must_wait) {
        *must_wait = false;
    }
    // For some reason, this is the only one that checks for negative.
    if (leftvol > 0xFFFF || rightvol > 0xFFFF || leftvol < 0 || rightvol < 0) {
        return kErrAudioInvalidVolume;
    } else if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotInit;
    }
    chans_[chan].leftVolume = leftvol;
    chans_[chan].rightVolume = rightvol;
    chans_[chan].sampleAddress = samplePtr;
    return Enqueue(static_cast<int>(chan), true, &caller, must_wait);
}

uint32_t AudioChannels::Output(uint32_t chan, int vol, uint32_t samplePtr) {
    if (vol > 0xFFFF) {
        return kErrAudioInvalidVolume;
    } else if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotInit;
    }
    if (vol >= 0) {
        chans_[chan].leftVolume = vol;
        chans_[chan].rightVolume = vol;
    }
    chans_[chan].sampleAddress = samplePtr;
    return Enqueue(static_cast<int>(chan), false, nullptr, nullptr);
}

uint32_t AudioChannels::OutputPanned(uint32_t chan, int leftvol, int rightvol, uint32_t samplePtr) {
    if (leftvol > 0xFFFF || rightvol > 0xFFFF) {
        return kErrAudioInvalidVolume;
    } else if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotInit;
    }
    if (leftvol >= 0) {
        chans_[chan].leftVolume = leftvol;
    }
    if (rightvol >= 0) {
        chans_[chan].rightVolume = rightvol;
    }
    chans_[chan].sampleAddress = samplePtr;
    return Enqueue(static_cast<int>(chan), false, nullptr, nullptr);
}

int AudioChannels::GetChannelRestLen(uint32_t chan) const {
    if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return static_cast<int>(kErrAudioInvalidChannel);
    }
    return static_cast<int>(chans_[chan].queue.size() / 2);
}

uint32_t AudioChannels::GetFreeChannel() const {
    for (uint32_t i = PSP_AUDIO_CHANNEL_MAX - 1; i > 0; --i) {
        if (!chans_[i].reserved) {
            return i;
        }
    }
    return 0xFFFFFFFF;
}

uint32_t AudioChannels::ChReserve(int chan, uint32_t sampleCount, uint32_t format) {
    if (chan < 0) {
        chan = static_cast<int>(GetFreeChannel());
        if (chan < 0) {
            return kErrAudioNoChannelsAvailable;
        }
    }
    if (static_cast<uint32_t>(chan) >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    }
    if ((sampleCount & 63) != 0 || sampleCount == 0 || sampleCount > PSP_AUDIO_SAMPLE_MAX) {
        return kErrAudioOutputSampleDataSizeNotAligned;
    }
    if (format != PSP_AUDIO_FORMAT_MONO && format != PSP_AUDIO_FORMAT_STEREO) {
        return kErrAudioInvalidFormat;
    }
    Channel &c = chans_[static_cast<size_t>(chan)];
    if (c.reserved) {
        return kErrAudioInvalidChannel;
    }
    c.sampleCount = sampleCount;
    c.format = format;
    c.reserved = true;
    c.leftVolume = 0;
    c.rightVolume = 0;
    return static_cast<uint32_t>(chan);
}

uint32_t AudioChannels::ChRelease(uint32_t chan) {
    if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotInit;
    }
    Reset(chans_[chan]);
    chans_[chan].reserved = false;
    return 0;
}

uint32_t AudioChannels::SetChannelDataLen(uint32_t chan, uint32_t len) {
    if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotInit;
    } else if ((len & 63) != 0 || len == 0 || len > PSP_AUDIO_SAMPLE_MAX) {
        return kErrAudioOutputSampleDataSizeNotAligned;
    }
    chans_[chan].sampleCount = len;
    return 0;
}

uint32_t AudioChannels::ChangeChannelConfig(uint32_t chan, uint32_t format) {
    if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotReserved;
    }
    chans_[chan].format = format;
    return 0;
}

uint32_t AudioChannels::ChangeChannelVolume(uint32_t chan, uint32_t leftvol, uint32_t rightvol) {
    if (leftvol > 0xFFFF || rightvol > 0xFFFF) {
        return kErrAudioInvalidVolume;
    } else if (chan >= PSP_AUDIO_CHANNEL_MAX) {
        return kErrAudioInvalidChannel;
    } else if (!chans_[chan].reserved) {
        return kErrAudioChannelNotReserved;
    }
    chans_[chan].leftVolume = static_cast<int>(leftvol);
    chans_[chan].rightVolume = static_cast<int>(rightvol);
    return 0;
}

uint32_t AudioChannels::SetFrequency(uint32_t freq) {
    if (freq == 44100 || freq == 48000) {
        mixFrequency_ = static_cast<int>(freq);
        return 0;
    }
    return kErrAudioInvalidFrequency;
}

uint32_t AudioChannels::Output2Reserve(uint32_t sampleCount) {
    Channel &chan = chans_[PSP_AUDIO_CHANNEL_OUTPUT2];
    // This seems to ignore the MSB, for some reason.
    sampleCount &= 0x7FFFFFFF;
    if (sampleCount < 17 || sampleCount > 4111) {
        return kErrKernelInvalidSize;
    } else if (chan.reserved) {
        return kErrAudioChannelAlreadyReserved;
    }
    chan.sampleCount = sampleCount;
    chan.format = PSP_AUDIO_FORMAT_STEREO;
    chan.reserved = true;
    srcFrequency_ = 0;
    return 0;
}

uint32_t AudioChannels::Output2OutputBlocking(uint32_t vol, uint32_t dataPtr, const Caller &caller,
                                              bool *must_wait) {
    if (must_wait) {
        *must_wait = false;
    }
    // Note: 0xFFFFF, not 0xFFFF!
    if (vol > 0xFFFFF) {
        return kErrAudioInvalidVolume;
    }
    Channel &chan = chans_[PSP_AUDIO_CHANNEL_OUTPUT2];
    if (!chan.reserved) {
        return kErrAudioChannelNotReserved;
    }
    chan.leftVolume = static_cast<int>(vol);
    chan.rightVolume = static_cast<int>(vol);
    chan.sampleAddress = dataPtr;
    return Enqueue(PSP_AUDIO_CHANNEL_OUTPUT2, true, &caller, must_wait);
}

uint32_t AudioChannels::Output2ChangeLength(uint32_t sampleCount) {
    Channel &chan = chans_[PSP_AUDIO_CHANNEL_OUTPUT2];
    if (!chan.reserved) {
        return kErrAudioChannelNotReserved;
    }
    chan.sampleCount = sampleCount;
    return 0;
}

uint32_t AudioChannels::Output2GetRestSample() const {
    const Channel &chan = chans_[PSP_AUDIO_CHANNEL_OUTPUT2];
    if (!chan.reserved) {
        return kErrAudioChannelNotReserved;
    }
    uint32_t size = static_cast<uint32_t>(chan.queue.size() / 2);
    // If ChangeLength reduces the size, it still gets output but this return is clamped.
    return std::min(size, chan.sampleCount);
}

uint32_t AudioChannels::Output2Release() {
    Channel &chan = chans_[PSP_AUDIO_CHANNEL_OUTPUT2];
    if (!chan.reserved) {
        return kErrAudioChannelNotReserved;
    }
    if (!chan.queue.empty()) {
        return kErrAudioChannelAlreadyReserved; // output busy
    }
    Reset(chan);
    chan.reserved = false;
    return 0;
}

uint32_t AudioChannels::SRCChReserve(uint32_t sampleCount, uint32_t freq, uint32_t format) {
    Channel &chan = chans_[PSP_AUDIO_CHANNEL_SRC];
    // This seems to ignore the MSB, for some reason.
    sampleCount &= 0x7FFFFFFF;
    if (format == 4) {
        return PSP_AUDIO_ERROR_SRC_FORMAT_4;
    } else if (format != 2) {
        return kErrKernelInvalidSize;
    } else if (sampleCount < 17 || sampleCount > 4111) {
        return kErrKernelInvalidSize;
    } else if (freq != 0 && !SRCFrequencyAllowed(freq)) {
        return kErrAudioInvalidFrequency;
    } else if (chan.reserved) {
        return kErrAudioChannelAlreadyReserved;
    }
    chan.reserved = true;
    chan.sampleCount = sampleCount;
    chan.format = PSP_AUDIO_FORMAT_STEREO;
    // Zero means default to 44.1kHz.
    srcFrequency_ = static_cast<int>(freq);
    return 0;
}

uint32_t AudioChannels::SRCChRelease() {
    Channel &chan = chans_[PSP_AUDIO_CHANNEL_SRC];
    if (!chan.reserved) {
        return kErrAudioChannelNotReserved;
    }
    if (!chan.queue.empty()) {
        return kErrAudioChannelAlreadyReserved; // output busy
    }
    Reset(chan);
    chan.reserved = false;
    return 0;
}

uint32_t AudioChannels::SRCOutputBlocking(uint32_t vol, uint32_t buf, const Caller &caller,
                                          bool *must_wait) {
    if (must_wait) {
        *must_wait = false;
    }
    if (vol > 0xFFFFF) {
        return kErrAudioInvalidVolume;
    }
    Channel &chan = chans_[PSP_AUDIO_CHANNEL_SRC];
    if (!chan.reserved) {
        return kErrAudioChannelNotReserved;
    }
    chan.leftVolume = static_cast<int>(vol);
    chan.rightVolume = static_cast<int>(vol);
    chan.sampleAddress = buf;
    return Enqueue(PSP_AUDIO_CHANNEL_SRC, true, &caller, must_wait);
}

void AudioChannels::Update(int32_t *mix) {
    bool firstChannel = true;
    constexpr size_t srcBufferSize = kHwBlockSize * 2;
    int16_t srcBuffer[srcBufferSize]{}; // upstream leaves it uninitialized

    for (size_t i = 0; i < chans_.size(); i++) {
        Channel &chan = chans_[i];
        if (!chan.reserved) {
            continue;
        }

        WakeThreads(chan, 0, kHwBlockSize);

        if (chan.queue.size() == 0) {
            continue;
        }

        const bool needsResample =
            i == PSP_AUDIO_CHANNEL_SRC && srcFrequency_ != 0 && srcFrequency_ != mixFrequency_;
        const size_t sz = needsResample ? (srcBufferSize * static_cast<size_t>(srcFrequency_)) /
                                              static_cast<size_t>(mixFrequency_)
                                        : srcBufferSize;

        const int16_t *buf1 = nullptr;
        const int16_t *buf2 = nullptr;
        size_t sz1 = 0;
        size_t sz2 = 0;
        // An underrun pops what there is (upstream logs it).
        chan.queue.Pop(sz, &buf1, &sz1, &buf2, &sz2);

        if (needsResample) {
            auto read = [&](size_t idx) {
                if (idx < sz1) {
                    return buf1[idx];
                }
                if (idx < sz1 + sz2) {
                    return buf2[idx - sz1];
                }
                if (buf2) {
                    return buf2[sz2 - 1];
                }
                return buf1[sz1 - 1];
            };

            // Upstream TODO: this works by small chunk and discards frac.
            const uint32_t ratio =
                static_cast<uint32_t>(65536.0 * srcFrequency_ / static_cast<double>(mixFrequency_));
            uint32_t frac = 0;
            size_t readIndex = 0;
            for (size_t outIndex = 0; readIndex < sz && outIndex < srcBufferSize; outIndex += 2) {
                const size_t readIndex2 = readIndex + 2;
                const int16_t l1 = read(readIndex);
                const int16_t r1 = read(readIndex + 1);
                const int16_t l2 = read(readIndex2);
                const int16_t r2 = read(readIndex2 + 1);
                const int sampleL = SrcLerp(l1, l2, static_cast<uint16_t>(frac));
                const int sampleR = SrcLerp(r1, r2, static_cast<uint16_t>(frac));
                srcBuffer[outIndex] = static_cast<int16_t>(sampleL);
                srcBuffer[outIndex + 1] = static_cast<int16_t>(sampleR);
                frac += ratio;
                readIndex += 2 * static_cast<uint16_t>(frac >> 16);
                frac &= 0xffff;
            }

            buf1 = srcBuffer;
            sz1 = srcBufferSize;
            buf2 = nullptr;
            sz2 = 0;
        }

        // Upstream quirk kept on purpose: on an underrun the tail of
        // mixBuffer_ keeps the previous block's values.
        if (firstChannel) {
            for (size_t s = 0; s < sz1; s++) {
                mixBuffer_[s] = buf1[s];
            }
            for (size_t s = 0; s < sz2; s++) {
                mixBuffer_[s + sz1] = buf2[s];
            }
            firstChannel = false;
        } else {
            for (size_t s = 0; s < sz1; s++) {
                mixBuffer_[s] += buf1[s];
            }
            for (size_t s = 0; s < sz2; s++) {
                mixBuffer_[s + sz1] += buf2[s];
            }
        }
    }

    if (firstChannel) {
        // Nothing was written above.
        mixBuffer_.fill(0);
    }
    std::copy(mixBuffer_.begin(), mixBuffer_.end(), mix);
}

} // namespace mpo::audio
