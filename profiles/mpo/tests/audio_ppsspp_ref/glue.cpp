// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glue for building PPSSPP v1.20.4's audio files outside PPSSPP
// (mpo_audio_ppsspp_difftest). AtracBase's destructor and CreateDecoder are
// copied from Core/HLE/AtracCtx.cpp and IsAtrac3StreamJointStereo from
// Core/HLE/sceAudiocodec.cpp @ v1.20.4 (GPL-2.0-or-later); the decoder factory
// can hand out the deterministic fake decoder of audio_scenarios.h instead of
// the real codec.

#include "glue.h"

#include "Core/HLE/AtracBase.h"
#include "Core/HLE/sceAudiocodec.h"
#include "Core/HW/SimpleAudioDec.h"
#include "audio_scenarios.h"

bool g_ppssppRefFakeDecoder = false;

// Atrac3Standalone.cpp is compiled with these two renamed (see CMakeLists).
AudioDecoder *Upstream_CreateAtrac3Audio(int channels, size_t blockAlign, const uint8_t *extraData,
                                         size_t extraDataSize);
AudioDecoder *Upstream_CreateAtrac3PlusAudio(int channels, size_t blockAlign);

namespace {

class FakeDecoder : public AudioDecoder {
public:
    FakeDecoder(PSPAudioType type, int channels) : type_(type), channels_(channels) {}
    PSPAudioType GetAudioType() const override { return type_; }
    bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels,
                int16_t *outbuf, int *outSamples) override {
        return audio_test::FakeAtracDecode(type_ == PSP_CODEC_AT3PLUS ? 2048 : 1024, channels_,
                                           inbuf, inbytes, inbytesConsumed, outputChannels, outbuf,
                                           outSamples);
    }
    bool IsOK() const override { return true; }
    void SetChannels(int channels) override { channels_ = channels; }

private:
    PSPAudioType type_;
    int channels_;
};

struct At3HeaderMap {
    u16 bytes;
    u16 channels;
    u8 jointStereo;

    bool Matches(int bytesPerFrame, int encodedChannels) const {
        return this->bytes == bytesPerFrame && this->channels == encodedChannels;
    }
};

const At3HeaderMap at3HeaderMap[] = {
    {0x00C0, 1, 0}, {0x0098, 1, 0}, {0x0180, 2, 0}, {0x0130, 2, 0}, {0x00C0, 2, 1},
};

} // namespace

AudioDecoder *CreateAtrac3Audio(int channels, size_t blockAlign, const uint8_t *extraData,
                                size_t extraDataSize) {
    if (g_ppssppRefFakeDecoder) {
        return new FakeDecoder(PSP_CODEC_AT3, channels);
    }
    return Upstream_CreateAtrac3Audio(channels, blockAlign, extraData, extraDataSize);
}

AudioDecoder *CreateAtrac3PlusAudio(int channels, size_t blockAlign) {
    if (g_ppssppRefFakeDecoder) {
        return new FakeDecoder(PSP_CODEC_AT3PLUS, channels);
    }
    return Upstream_CreateAtrac3PlusAudio(channels, blockAlign);
}

bool IsAtrac3StreamJointStereo(int codecType, int bytesPerFrame, int channels) {
    if (codecType != PSP_CODEC_AT3) {
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(at3HeaderMap); ++i) {
        if (at3HeaderMap[i].Matches(bytesPerFrame, channels)) {
            return at3HeaderMap[i].jointStereo;
        }
    }
    return false;
}

// From Core/HLE/AtracCtx.cpp.
AtracBase::~AtracBase() {
    delete decoder_;
}

void AtracBase::CreateDecoder(int codecType, int bytesPerFrame, int channels) {
    if (decoder_) {
        delete decoder_;
    }
    if (codecType == PSP_CODEC_AT3) {
        bool jointStereo = IsAtrac3StreamJointStereo(codecType, bytesPerFrame, channels);
        uint8_t extraData[14]{};
        extraData[0] = 1;
        extraData[3] = channels << 3;
        extraData[6] = jointStereo;
        extraData[8] = jointStereo;
        extraData[10] = 1;
        decoder_ = CreateAtrac3Audio(channels, bytesPerFrame, extraData, sizeof(extraData));
    } else {
        decoder_ = CreateAtrac3PlusAudio(channels, bytesPerFrame);
    }
}
