// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/AtracCtx2.h, Core/HLE/AtracBase.h and the
// SceAudiocodecCodec layout from Core/HLE/sceAudiocodec.h @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// One sceAtrac context ("Atrac2" in PPSSPP, its default implementation). The
// whole state lives in the 256-byte SceAtracContext in PSP memory, like on the
// real firmware: buffer modes (all data, halfway, streamed with and without
// loop, second buffer), loops, frame skipping, the low-level decoder and the
// sceSas integration.
//
// Changes from upstream: memory goes through GuestMemory with bounds checks
// (an out-of-range buffer fails cleanly instead of crashing), each packet is
// copied to a padded scratch buffer before decoding, the decoder comes from an
// injectable factory, and there are no savestates or dumps. PPSSPP's legacy
// context ("Atrac", used when no libatrac3plus BSS exists) is not ported: the
// runtime always provides the BSS through sceUtilityLoadAvModule.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "atrac_decoder.h"
#include "atrac_track.h"
#include "guest_memory.h"

namespace mpo::audio {

enum AtracStatus : uint8_t {
    ATRAC_STATUS_UNINITIALIZED = 0,
    ATRAC_STATUS_NO_DATA = 1,
    // The entire file is loaded into memory.
    ATRAC_STATUS_ALL_DATA_LOADED = 2,
    // The buffer fits the whole file but is only partially loaded.
    ATRAC_STATUS_HALFWAY_BUFFER = 3,
    // The buffer is smaller than the file and data is streamed into it.
    ATRAC_STATUS_STREAMED_WITHOUT_LOOP = 4,
    ATRAC_STATUS_STREAMED_LOOP_FROM_END = 5,
    // Additional audio after the loop: a second buffer is needed.
    ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER = 6,
    // Only sceAtracLowLevelDecode is used.
    ATRAC_STATUS_LOW_LEVEL = 8,
    // Context used as the source of an sceSas voice.
    ATRAC_STATUS_FOR_SCESAS = 16,
    ATRAC_STATUS_STREAMED_MASK = 4,
};

const char *AtracStatusToString(AtracStatus status);

inline bool AtracStatusIsStreaming(AtracStatus status) {
    return (status & ATRAC_STATUS_STREAMED_MASK) != 0;
}

// First half of SceAtracContext (only the fields sceAtrac touches are named).
struct SceAudiocodecCodec {
    int32_t unk_init;
    int32_t unk4;
    int32_t err; // 8
    int32_t edramAddr;
    int32_t neededMem;
    int32_t inited;
    uint32_t inBuf; // 18
    int32_t srcBytesRead;
    uint32_t outBuf; // 20
    int32_t dstSamplesWritten;
    int8_t unk40; // 28
    int8_t unk41;
    int8_t unk42;
    int8_t unk43;
    uint8_t unk44; // 2C
    int8_t unk45;
    int8_t unk46;
    int8_t unk47;
    int32_t unk48; // 30
    int32_t rest[19];
};

struct SceAtracIdInfo {
    int32_t decodePos;         // Sample position in the song that we'll next be decoding from.
    int32_t endSample;         // Last sample index of the track.
    int32_t loopStart;         // Start of the loop (sample index)
    int32_t loopEnd;           // End of the loop (sample index)
    int32_t firstValidSample;  // Number of skipped samples at the start.
    uint8_t numSkipFrames;     // Frames still to skip (1 after a loop or seek).
    AtracStatus state;         // See AtracStatus.
    uint8_t curBuffer;         // Current buffer (1 == second, 2 == done?)
    uint8_t numChan;           // Number of audio channels, usually 2 but 1 is possible.
    uint16_t sampleSize;       // Size in bytes of an encoded audio frame.
    uint16_t codec;            // 0x1000 is Atrac3+, 0x1001 is Atrac3.
    int32_t dataOff;           // File offset where the frames start (first dummy packet).
    int32_t curFileOff;        // File offset of the next packet to decode.
    int32_t fileDataEnd;       // File size in bytes.
    int32_t loopNum;           // Loop counter. -1 loops forever.
    int32_t streamDataByte;    // Bytes of queued data.
    int32_t streamOff;         // Streaming: read offset inside the RAM buffer.
    int32_t secondStreamOff;   // Read offset inside the second buffer.
    uint32_t buffer;           // Address of the main buffer.
    uint32_t secondBuffer;     // Address of the second buffer, or 0.
    uint32_t bufferByte;       // Size of the main buffer.
    uint32_t secondBufferByte; // Size of the second buffer.
    uint32_t unk[14];

    int SamplesPerFrame() const {
        return codec == 0x1000 ? ATRAC3PLUS_MAX_SAMPLES : ATRAC3_MAX_SAMPLES;
    }
    int SamplesFrameMask() const { return SamplesPerFrame() - 1; }
    int SkipSamples() const {
        // These first samples are skipped, after first possibly skipping 0-2
        // full frames, it seems.
        return codec == 0x1000 ? 0x170 : 0x45;
    }
    int BitRate() const {
        int bitrate = (sampleSize * 352800) / 1000;
        if (codec == 0x1000) {                      // PSP_CODEC_AT3PLUS
            bitrate = ((bitrate >> 11) + 8) & ~0xF; // upstream: & 0xFFFFFFF0
        } else {
            bitrate = (bitrate + 511) >> 10;
        }
        return bitrate;
    }
};

struct SceAtracContext {
    SceAudiocodecCodec codec;
    SceAtracIdInfo info;
};

static_assert(sizeof(SceAudiocodecCodec) == 128);
static_assert(sizeof(SceAtracIdInfo) == 128);
static_assert(sizeof(SceAtracContext) == 256);
// Natural alignment, no padding: the layout matches the PSP byte for byte.
static_assert(offsetof(SceAtracIdInfo, sampleSize) == 24);
static_assert(offsetof(SceAtracIdInfo, buffer) == 56);

struct Atrac3LowLevelParams {
    int32_t encodedChannels;
    int32_t outputChannels;
    int32_t bytesPerFrame;
};

struct AtracSingleResetBufferInfo {
    uint32_t writePosPtr;
    uint32_t writableBytes;
    uint32_t minWriteBytes;
    uint32_t filePos;
};

struct AtracResetBufferInfo {
    AtracSingleResetBufferInfo first;
    AtracSingleResetBufferInfo second;
};

// Hidden sceSas state when an ATRAC context feeds a SAS voice.
struct AtracSasStreamState {
    uint32_t bufPtr[2]{};
    uint32_t bufSize[2]{};
    int streamOffset = 0;
    int fileOffset = 0;
    int curBuffer = 0;
    bool isStreaming = false;
};

inline constexpr int PSP_ATRAC_ALLDATA_IS_ON_MEMORY = -1;
inline constexpr int PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY = -2;
inline constexpr int PSP_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY = -3;

class AtracContext {
public:
    // `contextAddr` must hold 256 valid bytes (the manager checks it).
    AtracContext(const GuestMemory &mem, uint32_t contextAddr, int codecType,
                 AtracDecoderFactory factory);

    uint32_t ContextAddress() const { return contextAddr_; }
    SceAtracContext &Context() const;
    SceAtracIdInfo &Info() const { return Context().info; }

    AtracStatus BufferState() const { return Info().state; }
    int GetOutputChannels() const { return outputChannels_; }

    int GetNextDecodePosition(int *pos) const;
    int RemainingFrames() const;
    int LoopStatus() const;
    int Bitrate() const { return Info().BitRate(); }
    int LoopNum() const { return Info().loopNum; }
    int SamplesPerFrame() const { return Info().SamplesPerFrame(); }
    int Channels() const { return Info().numChan; }
    int BytesPerFrame() const { return Info().sampleSize; }
    int SetLoopNum(int loopNum);
    int CodecType() const { return Info().codec; }

    void GetStreamDataInfo(uint32_t *writePtr, uint32_t *writableBytes, uint32_t *readOffset) const;
    int GetSecondBufferInfo(uint32_t *fileOffset, uint32_t *desiredSize) const;
    int AddStreamData(uint32_t bytesToAdd);
    int ResetPlayPosition(int sample, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf,
                          bool *delay);
    // NOTE: Not const! This can cause SkipFrames!
    int GetBufferInfoForResetting(AtracResetBufferInfo *bufferInfo, int sample, bool *delay);
    int SetData(const Track &track, uint32_t buffer, uint32_t readSize, uint32_t bufferSize,
                uint32_t fileSize, int outputChannels, bool isAA3);
    int SetSecondBuffer(uint32_t secondBuffer, uint32_t secondBufferSize);
    bool HasSecondBuffer() const { return Info().secondBufferByte != 0; }

    // `outbufAddr` may be 0 (skip). Returns 0 or a PSP error code.
    uint32_t DecodeData(uint32_t outbufAddr, int *SamplesNum, int *finish, int *remains);
    int DecodeLowLevel(const uint8_t *srcData, int *bytesConsumed, int16_t *dstData,
                       int *bytesWritten);

    void CheckForSas();
    int EnqueueForSas(uint32_t address, uint32_t ptr);
    void DecodeForSas(int16_t *dstData, int *bytesWritten, int *finish);
    const AtracSasStreamState *StreamStateForSas() const {
        return Info().state == ATRAC_STATUS_FOR_SCESAS ? &sas_ : nullptr;
    }

    uint32_t GetNextSamples() const;
    void InitLowLevel(const Atrac3LowLevelParams &params, int codecType);
    int GetSoundSample(int *endSample, int *loopStartSample, int *loopEndSample) const;
    uint32_t GetInternalCodecError() const { return static_cast<uint32_t>(Context().codec.err); }

private:
    uint32_t DecodeInternal(uint32_t outbufAddr, int *SamplesNum, int *finish);
    void GetResetBufferInfoInternal(AtracResetBufferInfo *bufferInfo, int sample) const;
    uint32_t ResetPlayPositionInternal(int seekPos, int bytesWrittenFirstBuf,
                                       int bytesWrittenSecondBuf);
    uint32_t SkipFrames(int *skippedCount);
    void WrapLastPacket();
    void CreateDecoder(int codecType, int bytesPerFrame, int channels);
    // Decodes `sampleSize` bytes at guest address `inAddr` through a padded copy.
    bool DecodePacket(const uint8_t *src, int size, int *bytesConsumed, int channels, int16_t *out,
                      int *outSamples);

    GuestMemory mem_;
    uint32_t contextAddr_;
    AtracDecoderFactory factory_;
    std::unique_ptr<AudioDecoder> decoder_;
    uint16_t outputChannels_ = 2;
    // Current decoded frame, to cut off the first part of a partial frame.
    std::vector<int16_t> decodeTemp_;
    std::vector<uint8_t> packet_;
    AtracSasStreamState sas_;
};

// Pure helpers from AtracCtx2.cpp (exposed for tests).
int InitContextFromTrackInfo(SceAtracContext *ctx, const TrackInfo *wave, uint32_t bufferAddr,
                             int readSize, int bufferSize);

} // namespace mpo::audio
