// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/Atrac3Standalone.cpp and Core/HW/SimpleAudioDec.h
// @ v1.20.4, plus AtracBase::CreateDecoder (Core/HLE/AtracCtx.cpp) and
// IsAtrac3StreamJointStereo (Core/HLE/sceAudiocodec.cpp).
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Packet decoder for ATRAC3 / ATRAC3plus on top of at3_standalone
// (third_party/at3_standalone, LGPL-2.1-or-later).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

struct ATRAC3Context;
struct ATRAC3PContext;

namespace mpo::audio {

// Decodes packet by packet (no demuxing). Mirrors PPSSPP's AudioDecoder.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    // `outSamples` counts sample frames (multiply by channels for values).
    // If *outSamples != 0 on entry it caps the output (it can only shrink).
    // Returns false on a bad packet; *inbytesConsumed then still advances by
    // the packet size so callers do not retry the same packet forever.
    virtual bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels,
                        int16_t *outbuf, int *outSamples) = 0;
    virtual bool IsOK() const = 0;
    virtual void FlushBuffers() {}
};

class Atrac3Audio final : public AudioDecoder {
public:
    Atrac3Audio(uint32_t codecType, int channels, size_t blockAlign, const uint8_t *extraData,
                size_t extraDataSize);
    ~Atrac3Audio() override;
    Atrac3Audio(const Atrac3Audio &) = delete;
    Atrac3Audio &operator=(const Atrac3Audio &) = delete;

    bool IsOK() const override { return codecOpen_; }
    void FlushBuffers() override;
    bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels,
                int16_t *outbuf, int *outSamples) override;

private:
    ATRAC3PContext *at3pCtx_ = nullptr;
    ATRAC3Context *at3Ctx_ = nullptr;
    int channels_ = 0;
    int blockAlign_ = 0;
    float *buffers_[2]{};
    bool codecOpen_ = false;
    bool codecFailed_ = false;
    uint32_t audioType_;
};

bool IsAtrac3StreamJointStereo(uint32_t codecType, int bytesPerFrame, int channels);

// AtracBase::CreateDecoder: builds the ATRAC3 extradata from the frame size.
std::unique_ptr<AudioDecoder> CreateAtracDecoder(uint32_t codecType, int bytesPerFrame,
                                                 int channels);

// Lets tests swap the real codec for a deterministic fake.
using AtracDecoderFactory = std::function<std::unique_ptr<AudioDecoder>(
    uint32_t codecType, int bytesPerFrame, int channels)>;

} // namespace mpo::audio
