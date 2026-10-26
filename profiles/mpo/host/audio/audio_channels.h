// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/sceAudio.cpp and Core/HLE/__sceAudio.cpp
// @ v1.20.4, with AdjustVolumeBlock from Core/Util/AudioFormat.cpp.
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// The PSP audio output channels (sceAudio) without their HLE glue: 8 normal
// channels plus the shared SRC/Output2 channel, each with a sample queue, and
// the hardware mixer that every 64 samples (at 44.1 kHz) pops one block from
// every reserved channel and sums it.
//
// Blocking: on the PSP a blocking output call puts the thread to sleep until
// the channel has room. Here the call reports it through `*must_wait` and
// registers the thread; Update() later asks the AudioWaitHost to wake it. The
// kernel (F08/F16b) implements AudioWaitHost.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "guest_memory.h"

namespace mpo::audio {

inline constexpr uint32_t PSP_AUDIO_CHANNEL_MAX = 8;
inline constexpr int PSP_AUDIO_CHANNEL_SRC = 8;
inline constexpr int PSP_AUDIO_CHANNEL_OUTPUT2 = 8;
inline constexpr uint32_t PSP_AUDIO_SAMPLE_MAX = 65536 - 64;

enum PspAudioFormat : uint32_t {
    PSP_AUDIO_FORMAT_STEREO = 0,
    PSP_AUDIO_FORMAT_MONO = 0x10,
};

// The PSP mixes 64 stereo samples at a time, at 44.1 kHz.
inline constexpr int kHwSampleRate = 44100;
inline constexpr int kHwBlockSize = 64;

class AudioWaitHost {
public:
    virtual ~AudioWaitHost() = default;
    // __KernelGetWaitID(thread, WAITTYPE_AUDIOCHANNEL) != 0.
    virtual bool IsWaitingOnAudio(uint64_t thread) = 0;
    // __KernelResumeThreadFromWait(thread, result).
    virtual void WakeFromAudio(uint64_t thread, uint32_t result) = 0;
};

class AudioChannels {
public:
    explicit AudioChannels(const GuestMemory &mem);

    void SetWaitHost(AudioWaitHost *host) { waitHost_ = host; }
    // The thread making the next blocking call, and whether it may wait
    // (__KernelIsDispatchEnabled()).
    struct Caller {
        uint64_t thread = 0;
        bool dispatchEnabled = true;
    };

    // sceAudio "traditional" channels. `*must_wait` (may be null) is set when
    // the caller has to sleep until Update() wakes it with the returned value.
    uint32_t OutputBlocking(uint32_t chan, int vol, uint32_t samplePtr, const Caller &caller,
                            bool *must_wait);
    uint32_t OutputPannedBlocking(uint32_t chan, int leftvol, int rightvol, uint32_t samplePtr,
                                  const Caller &caller, bool *must_wait);
    uint32_t Output(uint32_t chan, int vol, uint32_t samplePtr);
    uint32_t OutputPanned(uint32_t chan, int leftvol, int rightvol, uint32_t samplePtr);
    int GetChannelRestLen(uint32_t chan) const;
    int GetChannelRestLength(uint32_t chan) const { return GetChannelRestLen(chan); }
    uint32_t ChReserve(int chan, uint32_t sampleCount, uint32_t format);
    uint32_t ChRelease(uint32_t chan);
    uint32_t SetChannelDataLen(uint32_t chan, uint32_t len);
    uint32_t ChangeChannelConfig(uint32_t chan, uint32_t format);
    uint32_t ChangeChannelVolume(uint32_t chan, uint32_t leftvol, uint32_t rightvol);
    uint32_t SetFrequency(uint32_t freq);

    // Output2 and SRC share the extra channel.
    uint32_t Output2Reserve(uint32_t sampleCount);
    uint32_t Output2OutputBlocking(uint32_t vol, uint32_t dataPtr, const Caller &caller,
                                   bool *must_wait);
    uint32_t Output2ChangeLength(uint32_t sampleCount);
    uint32_t Output2GetRestSample() const;
    uint32_t Output2Release();
    uint32_t SRCChReserve(uint32_t sampleCount, uint32_t freq, uint32_t format);
    uint32_t SRCChRelease();
    uint32_t SRCOutputBlocking(uint32_t vol, uint32_t buf, const Caller &caller, bool *must_wait);

    // __AudioUpdate: called every kHwBlockSize samples of emulated time.
    // Writes kHwBlockSize stereo frames (2 * 64 values, not clamped) to `mix`.
    void Update(int32_t *mix);

    int MixFrequency() const { return mixFrequency_; }

private:
    struct Waiter {
        uint64_t thread;
        int numSamples;
        uint32_t result;
    };

    // FixedSizeQueue<s16, 32768 * 8> upstream.
    class SampleRing {
    public:
        SampleRing() : storage_(kCapacity, 0) {}
        void clear();
        size_t size() const { return count_; }
        bool empty() const { return count_ == 0; }
        // Upstream does not check for overflow; values past the capacity are
        // dropped here instead of corrupting the queue.
        void Push(const int16_t *data, size_t n);
        void Pop(size_t n, const int16_t **src1, size_t *sz1, const int16_t **src2, size_t *sz2);

    private:
        static constexpr size_t kCapacity = 32768 * 8;
        std::vector<int16_t> storage_;
        size_t head_ = 0;
        size_t tail_ = 0;
        size_t count_ = 0;
    };

    struct Channel {
        bool reserved = false;
        uint32_t sampleAddress = 0;
        uint32_t sampleCount = 0;
        int leftVolume = 0;
        int rightVolume = 0;
        uint32_t format = 0;
        std::vector<Waiter> waitingThreads;
        SampleRing queue;
    };

    void Clear(Channel &chan);
    void Reset(Channel &chan);
    uint32_t Enqueue(int chanNum, bool blocking, const Caller *caller, bool *must_wait);
    void WakeThreads(Channel &chan, int result, int step);
    uint32_t GetFreeChannel() const;

    GuestMemory mem_;
    AudioWaitHost *waitHost_ = nullptr;
    std::array<Channel, PSP_AUDIO_CHANNEL_MAX + 1> chans_;
    int mixFrequency_ = 44100;
    int srcFrequency_ = 0;
    std::array<int32_t, kHwBlockSize * 2> mixBuffer_{};
};

// AdjustVolumeBlock (scalar path): out = in * vol >> 16, per channel.
void AdjustVolumeBlock(int16_t *out, const int16_t *in, size_t size, int leftVol, int rightVol);

} // namespace mpo::audio
