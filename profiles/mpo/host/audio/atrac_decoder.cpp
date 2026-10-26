// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/Atrac3Standalone.cpp @ v1.20.4, with
// AtracBase::CreateDecoder (Core/HLE/AtracCtx.cpp) and
// IsAtrac3StreamJointStereo (Core/HLE/sceAudiocodec.cpp).
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Changes from upstream: no logging, SetChannels() is not carried over (only
// sceMpeg needs it), and buffers are owned with new[]/delete[] as upstream.

#include "atrac_decoder.h"

#include <algorithm>
#include <iterator>

#include "at3_decoders.h"
#include "atrac_track.h"

namespace mpo::audio {
namespace {

int16_t clamp16(float f) {
    if (f >= 1.0f) {
        return 32767;
    } else if (f <= -1.0f) {
        return -32767;
    }
    return static_cast<int16_t>(f * 32767);
}

struct At3HeaderMap {
    uint16_t bytes;
    uint16_t channels;
    uint8_t jointStereo;

    bool Matches(int bytesPerFrame, int encodedChannels) const {
        return bytes == bytesPerFrame && channels == encodedChannels;
    }
};

// These should represent all possible supported bitrates (66, 104, and 132
// for stereo.)
const At3HeaderMap at3HeaderMap[] = {
    {0x00C0, 1, 0}, // 132/2 (66) kbps mono
    {0x0098, 1, 0}, // 105/2 (52.5) kbps mono
    {0x0180, 2, 0}, // 132 kbps stereo
    {0x0130, 2, 0}, // 105 kbps stereo
    // At this size, stereo can only use joint stereo.
    {0x00C0, 2, 1}, // 66 kbps stereo
};

} // namespace

Atrac3Audio::Atrac3Audio(uint32_t codecType, int channels, size_t blockAlign,
                         const uint8_t *extraData, size_t extraDataSize)
    : channels_(channels), audioType_(codecType) {
    blockAlign_ = static_cast<int>(blockAlign);
    if (audioType_ == PSP_CODEC_AT3) {
        at3Ctx_ = atrac3_alloc(channels, &blockAlign_, extraData, static_cast<int>(extraDataSize));
        if (at3Ctx_) {
            codecOpen_ = true;
        } else {
            codecFailed_ = true;
        }
    }
    for (float *&buffer : buffers_) {
        buffer = new float[4096];
    }
}

Atrac3Audio::~Atrac3Audio() {
    if (at3Ctx_) {
        atrac3_free(at3Ctx_);
    }
    if (at3pCtx_) {
        atrac3p_free(at3pCtx_);
    }
    for (float *buffer : buffers_) {
        delete[] buffer;
    }
}

void Atrac3Audio::FlushBuffers() {
    if (at3Ctx_) {
        atrac3_flush_buffers(at3Ctx_);
    }
    if (at3pCtx_) {
        atrac3p_flush_buffers(at3pCtx_);
    }
}

bool Atrac3Audio::Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed,
                         int outputChannels, int16_t *outbuf, int *outSamples) {
    if (outSamples) {
        *outSamples = 0;
    }
    if (inbytesConsumed) {
        *inbytesConsumed = 0;
    }
    if (!codecOpen_) {
        // The ATRAC3plus context is opened on the first decode (upstream does
        // this so a late SetChannels() can take effect).
        if (codecFailed_) {
            return false;
        }
        if (audioType_ == PSP_CODEC_AT3PLUS) {
            at3pCtx_ = atrac3p_alloc(channels_, &blockAlign_);
            if (at3pCtx_) {
                codecOpen_ = true;
            } else {
                codecFailed_ = true;
            }
        }
        if (!codecOpen_) {
            return false;
        }
    }

    blockAlign_ = inbytes;
    // We just call the decode function directly without going through the
    // whole packet machinery.
    int result;
    int nb_samples = 0;
    if (audioType_ == PSP_CODEC_AT3PLUS) {
        result = atrac3p_decode_frame(at3pCtx_, buffers_, &nb_samples, inbuf, inbytes);
    } else {
        result = atrac3_decode_frame(at3Ctx_, buffers_, &nb_samples, inbuf, inbytes);
    }
    if (result < 0) {
        // To recover from single bad packets, report the regular packet size.
        if (inbytesConsumed) {
            *inbytesConsumed = inbytes;
        }
        if (outSamples) {
            if (*outSamples != 0) {
                nb_samples = std::min(*outSamples, nb_samples);
            }
            *outSamples = nb_samples;
        }
        return false;
    }
    if (inbytesConsumed) {
        *inbytesConsumed = result;
    }
    if (outSamples) {
        // Allow capping the output samples by setting *outSamples to non-zero.
        if (*outSamples != 0) {
            nb_samples = std::min(*outSamples, nb_samples);
        }
        *outSamples = nb_samples;
    }
    if (nb_samples > 0) {
        if (outSamples) {
            *outSamples = nb_samples;
        }
        if (outbuf) {
            const float *left = buffers_[0];
            if (outputChannels == 2) {
                // Stereo output, standard.
                const float *right = channels_ == 2 ? buffers_[1] : buffers_[0];
                for (int i = 0; i < nb_samples; i++) {
                    outbuf[i * 2] = clamp16(left[i]);
                    outbuf[i * 2 + 1] = clamp16(right[i]);
                }
            } else {
                // Mono output, just take the left channel.
                for (int i = 0; i < nb_samples; i++) {
                    outbuf[i] = clamp16(left[i]);
                }
            }
        }
    }
    return true;
}

bool IsAtrac3StreamJointStereo(uint32_t codecType, int bytesPerFrame, int channels) {
    if (codecType != PSP_CODEC_AT3) {
        // Well, might actually be, but it's not used in codec setup.
        return false;
    }
    for (const At3HeaderMap &entry : at3HeaderMap) {
        if (entry.Matches(bytesPerFrame, channels)) {
            return entry.jointStereo != 0;
        }
    }
    return false;
}

std::unique_ptr<AudioDecoder> CreateAtracDecoder(uint32_t codecType, int bytesPerFrame,
                                                 int channels) {
    if (codecType == PSP_CODEC_AT3) {
        const bool jointStereo = IsAtrac3StreamJointStereo(codecType, bytesPerFrame, channels);
        // We don't pull this from the RIFF so that we can support OMA also.
        uint8_t extraData[14]{};
        // The only thing that changes are the jointStereo_ values.
        extraData[0] = 1;
        extraData[3] = static_cast<uint8_t>(channels << 3);
        extraData[6] = jointStereo;
        extraData[8] = jointStereo;
        extraData[10] = 1;
        return std::make_unique<Atrac3Audio>(PSP_CODEC_AT3, channels,
                                             static_cast<size_t>(bytesPerFrame), extraData,
                                             sizeof(extraData));
    }
    return std::make_unique<Atrac3Audio>(PSP_CODEC_AT3PLUS, channels,
                                         static_cast<size_t>(bytesPerFrame), nullptr, 0);
}

} // namespace mpo::audio
