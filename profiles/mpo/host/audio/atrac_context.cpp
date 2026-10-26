// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/AtracCtx2.cpp @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// See atrac_context.h for the changes from upstream. The buffer, loop and
// skip-frame arithmetic is unchanged.

#include "atrac_context.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "audio_errors.h"

namespace mpo::audio {
namespace {

constexpr int Err(uint32_t code) {
    return static_cast<int>(code);
}

struct AT3BitrateMeta {
    uint16_t sampleSize;
    uint8_t dataByte;
    uint8_t jointStereo;
};

const AT3BitrateMeta g_at3BitrateMeta[5] = {
    {0x180, 0x04, 0}, {0x130, 0x06, 0}, {0x0C0, 0x0B, 1}, {0x0C0, 0x0E, 0}, {0x098, 0x0F, 0},
};

// Needs to support negative numbers, and to handle non-powers-of-two.
int RoundDownToMultiple(int size, int grain) {
    return size - (size % grain);
}

int RoundDownToMultipleWithOffset(int offset, int size, int grain) {
    if (size > offset) {
        return ((size - offset) / grain) * grain + offset;
    }
    return size;
}

int ComputeSkipFrames(const SceAtracIdInfo &info, int seekPos) {
    // No idea why this is the rule, but this is the rule.
    return (seekPos & info.SamplesFrameMask()) < info.SkipSamples() ? 2 : 1;
}

int ComputeFileOffset(const SceAtracIdInfo &info, int seekPos) {
    int frameOffset = ((seekPos / info.SamplesPerFrame()) - 1) * info.sampleSize;
    if ((seekPos & info.SamplesFrameMask()) < info.SkipSamples() && (frameOffset != 0)) {
        frameOffset -= info.sampleSize;
    }
    return frameOffset + info.dataOff;
}

// Unlike the above, this one need to be inclusive.
int ComputeLoopEndFileOffset(const SceAtracIdInfo &info, int seekPos) {
    return (seekPos / info.SamplesPerFrame() + 1) * info.sampleSize + info.dataOff;
}

int ComputeSpaceUsed(const SceAtracIdInfo &info) {
    // The odd case: If streaming from the second buffer, and we're past the
    // loop end (we're in the tail)...
    if (info.decodePos > info.loopEnd && info.curBuffer == 1) {
        int space = static_cast<int>(info.secondBufferByte);
        if (info.secondStreamOff < space) {
            space = RoundDownToMultipleWithOffset(
                info.secondStreamOff, static_cast<int>(info.secondBufferByte), info.sampleSize);
        }
        if ((info.secondStreamOff <= space) &&
            (space - info.secondStreamOff < info.streamDataByte)) {
            return info.streamDataByte - (space - info.secondStreamOff);
        }
        return 0;
    }
    // The normal case.
    return info.streamDataByte;
}

int ComputeRemainFrameStream(const SceAtracIdInfo &info) {
    if (info.streamDataByte >= info.fileDataEnd - info.curFileOff) {
        // Already done.
        return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
    }
    // Since we're streaming, the remaining frames are what's valid in the buffer.
    return std::max(0,
                    info.streamDataByte / info.sampleSize - static_cast<int>(info.numSkipFrames));
}

// This got so complicated!
int ComputeRemainFrameLooped(const SceAtracIdInfo &info) {
    const int loopStartFileOffset = ComputeFileOffset(info, info.loopStart);
    const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
    const int writeFileOff = info.curFileOff + info.streamDataByte;
    const int leftToRead = writeFileOff - loopEndFileOffset;

    int remainFrames;
    if (writeFileOff <= loopEndFileOffset) {
        // Simple case - just divide to find the number of frames remaining in the buffer.
        remainFrames = info.streamDataByte / info.sampleSize;
    } else {
        // Darn, we need to take looping into account...
        const int skipFramesAtLoopStart = ComputeSkipFrames(info, info.loopStart);
        const int firstPartLength = loopEndFileOffset - loopStartFileOffset;
        const int secondPartLength = leftToRead % firstPartLength;
        remainFrames = (loopEndFileOffset - info.curFileOff) / info.sampleSize +
                       (leftToRead / firstPartLength) *
                           (firstPartLength / info.sampleSize - skipFramesAtLoopStart);
        if (secondPartLength > skipFramesAtLoopStart * info.sampleSize) {
            remainFrames += secondPartLength / info.sampleSize - skipFramesAtLoopStart;
        }
    }

    // Clamp to zero.
    remainFrames = std::max(0, remainFrames - static_cast<int>(info.numSkipFrames));
    if (info.loopNum < 0) {
        // Infinite looping while streaming, we never return that we're done reading data.
        return remainFrames;
    }

    // Additional check for distance to end of playback if we're looping a
    // finite amount of times.
    const int streamBufferEndFileOffset = info.curFileOff + info.streamDataByte;
    if (streamBufferEndFileOffset >= loopEndFileOffset) {
        const int numBufferedLoops = (streamBufferEndFileOffset - loopEndFileOffset) /
                                     (loopEndFileOffset - loopStartFileOffset);
        if (info.loopNum <= numBufferedLoops) {
            return PSP_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY;
        }
    }
    return remainFrames;
}

void InitLengthAndLoop(SceAtracIdInfo *ctx, int endSample, int waveDataSize, int firstSampleOffset,
                       int loopBegin, int loopEnd) {
    const int off = ctx->codec == 0x1001 ? 0x45 : 0x170;
    const int blockShift = 0x100b - ctx->codec;
    const int firstValidSample = firstSampleOffset + off;
    int numSamplesInFile;
    if (endSample == 0) {
        numSamplesInFile = waveDataSize / ctx->sampleSize << (blockShift & 0x1f);
    } else {
        numSamplesInFile = endSample + firstValidSample;
    }
    ctx->decodePos = firstValidSample;
    ctx->loopNum = 0;
    ctx->endSample = numSamplesInFile - 1;
    ctx->numSkipFrames = static_cast<uint8_t>(firstValidSample >> (blockShift & 0x1f));
    if (-1 < loopBegin) {
        ctx->loopEnd = loopEnd + off;
        ctx->loopStart = loopBegin + off;
        return;
    }
    ctx->loopEnd = 0;
    ctx->loopStart = 0;
}

int ComputeAtracStateAndInitSecondBuffer(SceAtracIdInfo *info, uint32_t readSize,
                                         uint32_t bufferSize) {
    AtracStatus state;
    if (bufferSize < static_cast<uint32_t>(info->fileDataEnd)) {
        if (info->streamDataByte < static_cast<int32_t>(info->sampleSize) * 2) {
            return Err(kErrAtracSizeTooSmall);
        }
        int loopEnd = info->loopEnd;
        state = ATRAC_STATUS_STREAMED_WITHOUT_LOOP;
        if (loopEnd != 0) {
            state = ATRAC_STATUS_STREAMED_LOOP_FROM_END;
            if (loopEnd != info->endSample) {
                const int loopEndFileOffset = ComputeLoopEndFileOffset(*info, loopEnd);
                loopEnd = (loopEndFileOffset - info->dataOff) + 1;
                info->state = ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER;
                if (loopEnd < info->streamDataByte) {
                    info->streamDataByte = loopEnd;
                }
                info->secondStreamOff = 0;
                info->secondBuffer = 0;
                info->secondBufferByte = 0;
                return 0;
            }
        }
    } else {
        state = ATRAC_STATUS_HALFWAY_BUFFER;
        if (readSize >= static_cast<uint32_t>(info->fileDataEnd)) {
            state = ATRAC_STATUS_ALL_DATA_LOADED;
        }
    }
    info->state = state;
    return 0;
}

int ComputeLoopedStreamWritableBytes(const SceAtracIdInfo &info, const int loopStartFileOffset,
                                     const uint32_t loopEndFileOffset) {
    const uint32_t writeOffset = static_cast<uint32_t>(info.curFileOff + info.streamDataByte);
    if (writeOffset >= loopEndFileOffset) {
        const int loopLength = static_cast<int>(loopEndFileOffset) - loopStartFileOffset;
        return loopLength -
               static_cast<int>((static_cast<uint32_t>(info.curFileOff + info.streamDataByte) -
                                 loopEndFileOffset) %
                                static_cast<uint32_t>(loopLength));
    }
    return static_cast<int>(loopEndFileOffset - writeOffset);
}

int IncrementAndLoop(int curOffset, int increment, int loopStart, int loopEnd) {
    const int sum = curOffset + increment;
    if (sum >= loopEnd) {
        return loopStart + (sum - loopEnd) % (loopEnd - loopStart);
    }
    return sum;
}

int WrapAroundRoundedBufferSize(int offset, int bufferSize, int addend, int grainSize) {
    bufferSize = RoundDownToMultipleWithOffset(offset, bufferSize, grainSize);
    const int sum = offset + addend;
    if (bufferSize <= sum) {
        return sum - bufferSize;
    }
    return sum;
}

void ComputeStreamBufferDataInfo(const SceAtracIdInfo &info, uint32_t *writePtr,
                                 uint32_t *bytesToWrite, uint32_t *readFileOffset) {
    // This really is the core logic of sceAtrac.
    const int streamOff = info.curBuffer != 1 ? info.streamOff : 0;
    const int spaceUsed = ComputeSpaceUsed(info);
    const int spaceLeftAfterStreamOff = RoundDownToMultipleWithOffset(
        streamOff, static_cast<int>(info.bufferByte), info.sampleSize);
    const int streamPos = streamOff + spaceUsed;
    int spaceLeftInBuffer;
    if (streamPos >= spaceLeftAfterStreamOff) {
        spaceLeftInBuffer = spaceLeftAfterStreamOff - spaceUsed;
    } else {
        spaceLeftInBuffer = spaceLeftAfterStreamOff - streamPos;
    }
    const int loopStartFileOffset = ComputeFileOffset(info, info.loopStart);
    const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);

    if (spaceLeftInBuffer < 0) {
        // Most likely, the file was truncated. Stumbling along.
        spaceLeftInBuffer = 0;
    }

    const int bufferByte = static_cast<int>(info.bufferByte);
    switch (info.state) {
    case ATRAC_STATUS_STREAMED_WITHOUT_LOOP: {
        *bytesToWrite = static_cast<uint32_t>(std::clamp(
            info.fileDataEnd - (info.curFileOff + info.streamDataByte), 0, spaceLeftInBuffer));
        const int streamFileOff = info.curFileOff + info.streamDataByte;
        if (streamFileOff < info.fileDataEnd) {
            *readFileOffset = static_cast<uint32_t>(streamFileOff);
            *writePtr = info.buffer +
                        static_cast<uint32_t>(WrapAroundRoundedBufferSize(
                            info.streamOff, bufferByte, info.streamDataByte, info.sampleSize));
        } else {
            *readFileOffset = 0;
            *writePtr = info.buffer;
        }
        break;
    }

    case ATRAC_STATUS_STREAMED_LOOP_FROM_END:
        *bytesToWrite = static_cast<uint32_t>(
            std::min(ComputeLoopedStreamWritableBytes(info, loopStartFileOffset,
                                                      static_cast<uint32_t>(loopEndFileOffset)),
                     spaceLeftInBuffer));
        *readFileOffset = static_cast<uint32_t>(IncrementAndLoop(
            info.curFileOff, info.streamDataByte, loopStartFileOffset, loopEndFileOffset));
        *writePtr =
            info.buffer + static_cast<uint32_t>(WrapAroundRoundedBufferSize(
                              info.streamOff, bufferByte, info.streamDataByte, info.sampleSize));
        break;

    case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER: {
        // Behaves like WITHOUT_LOOP or STREAMED_LOOP_FROM_END depending on the
        // decode position.
        if (info.decodePos <= info.loopEnd) {
            *bytesToWrite = static_cast<uint32_t>(
                std::min(ComputeLoopedStreamWritableBytes(info, loopStartFileOffset,
                                                          static_cast<uint32_t>(loopEndFileOffset)),
                         spaceLeftInBuffer));
            *readFileOffset = static_cast<uint32_t>(IncrementAndLoop(
                info.curFileOff, info.streamDataByte, loopStartFileOffset, loopEndFileOffset));
        } else {
            const int streamFileOff = info.curFileOff + info.streamDataByte;
            *bytesToWrite = static_cast<uint32_t>(
                std::clamp(info.fileDataEnd - streamFileOff, 0, spaceLeftInBuffer));
            if (streamFileOff < info.fileDataEnd) {
                *readFileOffset = static_cast<uint32_t>(streamFileOff);
            } else {
                *readFileOffset = 0;
            }
        }
        if (info.decodePos <= info.loopEnd || info.curBuffer != 1) {
            *writePtr = info.buffer +
                        static_cast<uint32_t>(WrapAroundRoundedBufferSize(
                            info.streamOff, bufferByte, info.streamDataByte, info.sampleSize));
        } else {
            *writePtr = info.buffer + static_cast<uint32_t>(WrapAroundRoundedBufferSize(
                                          0, bufferByte, spaceUsed, info.sampleSize));
        }
        break;
    }

    default:
        // unreachable
        break;
    }
}

} // namespace

const char *AtracStatusToString(AtracStatus status) {
    switch (status) {
    case ATRAC_STATUS_NO_DATA:
        return "NO_DATA";
    case ATRAC_STATUS_ALL_DATA_LOADED:
        return "ALL_DATA_LOADED";
    case ATRAC_STATUS_HALFWAY_BUFFER:
        return "HALFWAY_BUFFER";
    case ATRAC_STATUS_STREAMED_WITHOUT_LOOP:
        return "STREAMED_WITHOUT_LOOP";
    case ATRAC_STATUS_STREAMED_LOOP_FROM_END:
        return "STREAMED_LOOP_FROM_END";
    case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER:
        return "STREAMED_LOOP_WITH_TRAILER";
    case ATRAC_STATUS_LOW_LEVEL:
        return "LOW_LEVEL";
    case ATRAC_STATUS_FOR_SCESAS:
        return "FOR_SCESAS";
    default:
        return "(unknown!)";
    }
}

int InitContextFromTrackInfo(SceAtracContext *ctx, const TrackInfo *wave, uint32_t bufferAddr,
                             int readSize, int bufferSize) {
    SceAtracIdInfo &info = ctx->info;
    if (wave->blockAlign == 0) {
        // Upstream would divide by zero below (only reachable with a broken
        // OMA header or an invalid buffer pointer).
        return Err(kErrAtracBadCodecParams);
    }
    info.numChan = static_cast<uint8_t>(wave->numChans);
    const int extraSamples = info.codec == 0x1001 ? 0x45 : 0x170;
    info.firstValidSample = extraSamples + static_cast<int>(wave->firstSampleOffset);
    const int endSample3 = static_cast<int>(wave->endSample);
    info.sampleSize = wave->blockAlign;
    InitLengthAndLoop(&info, endSample3, static_cast<int>(wave->waveDataSize),
                      static_cast<int>(wave->firstSampleOffset), static_cast<int>(wave->loopStart),
                      static_cast<int>(wave->loopEnd));
    const int dataOff = static_cast<int>(wave->dataOff);
    const int endSample = info.endSample;
    info.streamDataByte = readSize - dataOff;
    info.buffer = bufferAddr;
    info.curFileOff = dataOff;
    info.dataOff = dataOff;
    info.fileDataEnd = static_cast<int>(wave->waveDataSize) + dataOff;
    info.curBuffer = 0;
    info.bufferByte = static_cast<uint32_t>(bufferSize);
    info.streamOff = dataOff;
    if (info.loopEnd > endSample) {
        return Err(kErrAtracBadCodecParams);
    }

    const int numChunks = (endSample >> ((0x100b - info.codec) & 0x1f));

    if (static_cast<uint32_t>(numChunks) * info.sampleSize <
        (wave->waveDataSize + static_cast<uint32_t>(dataOff)) - static_cast<uint32_t>(dataOff)) {
        const int retval = ComputeAtracStateAndInitSecondBuffer(
            &info, static_cast<uint32_t>(readSize), static_cast<uint32_t>(bufferSize));
        if (retval < 0) {
            return retval;
        }
        if (info.codec != PSP_CODEC_AT3) {
            // At3plus: configure the codec for the sample size.
            ctx->codec.unk40 = static_cast<int8_t>(wave->sampleSizeMaybe);
            ctx->codec.unk48 = 0;
            ctx->codec.unk41 = static_cast<int8_t>(wave->tailFlag);
            return 0;
        }
        // At3. Set up the hardware codec (not actually needed by our decoder).
        for (int counter = 4; counter >= 0; counter--) {
            if ((g_at3BitrateMeta[counter].sampleSize == info.sampleSize) &&
                (static_cast<int>(g_at3BitrateMeta[counter].dataByte) == wave->sampleSizeMaybe)) {
                ctx->codec.unk40 = static_cast<int8_t>(g_at3BitrateMeta[counter].jointStereo);
                ctx->codec.unk41 = 0;
                ctx->codec.unk42 = 0;
                ctx->codec.unk43 = 0;
                return 0;
            }
        }
        return 0;
    }
    return Err(kErrAtracBadCodecParams);
}

AtracContext::AtracContext(const GuestMemory &mem, uint32_t contextAddr, int codecType,
                           AtracDecoderFactory factory)
    : mem_(mem), contextAddr_(contextAddr), factory_(std::move(factory)) {
    // First-time initialization. The rest is initialized in SetData.
    SceAtracIdInfo &info = Info();
    info.codec = static_cast<uint16_t>(codecType);
    info.state = ATRAC_STATUS_NO_DATA;
    info.curBuffer = 0;

    sas_.streamOffset = 0;
    sas_.bufPtr[0] = 0;
    sas_.bufPtr[1] = 0;
}

SceAtracContext &AtracContext::Context() const {
    // The manager only creates contexts on a validated, aligned 256-byte slot.
    return *reinterpret_cast<SceAtracContext *>(mem_.Ptr(contextAddr_, sizeof(SceAtracContext)));
}

void AtracContext::CreateDecoder(int codecType, int bytesPerFrame, int channels) {
    decoder_ = factory_
                   ? factory_(static_cast<uint32_t>(codecType), bytesPerFrame, channels)
                   : CreateAtracDecoder(static_cast<uint32_t>(codecType), bytesPerFrame, channels);
}

bool AtracContext::DecodePacket(const uint8_t *src, int size, int *bytesConsumed, int channels,
                                int16_t *out, int *outSamples) {
    if (!decoder_) {
        return false;
    }
    // FFmpeg-style bit readers may look a few bytes past the packet; decode
    // from a zero-padded copy so that can never leave PSP memory.
    packet_.assign(static_cast<size_t>(size) + 64, 0);
    std::memcpy(packet_.data(), src, static_cast<size_t>(size));
    return decoder_->Decode(packet_.data(), size, bytesConsumed, channels, out, outSamples);
}

int AtracContext::RemainingFrames() const {
    const SceAtracIdInfo &info = Info();

    // Handle the easy cases first.
    switch (info.state) {
    case ATRAC_STATUS_UNINITIALIZED:
    case ATRAC_STATUS_NO_DATA:
        return 0;
    case ATRAC_STATUS_ALL_DATA_LOADED:
        return PSP_ATRAC_ALLDATA_IS_ON_MEMORY; // Not sure about no data.
    case ATRAC_STATUS_HALFWAY_BUFFER: {
        // Compute the remaining space, and divide by the sample size,
        // adjusting for frames-to-skip.
        const int writeFileOff = info.dataOff + info.streamDataByte;
        if (info.curFileOff < writeFileOff) {
            return std::max(0, (writeFileOff - info.curFileOff) / info.sampleSize -
                                   static_cast<int>(info.numSkipFrames));
        }
        return 0;
    }
    case ATRAC_STATUS_STREAMED_WITHOUT_LOOP:
        return ComputeRemainFrameStream(info);

    case ATRAC_STATUS_STREAMED_LOOP_FROM_END:
        return ComputeRemainFrameLooped(info);

    case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER:
        if (info.decodePos <= info.loopEnd) {
            // If before the tail, just treat it as looped.
            return ComputeRemainFrameLooped(info);
        }
        // If in tail, treat is as unlooped.
        return ComputeRemainFrameStream(info);
    default:
        return Err(kErrAtracBadAtracId);
    }
}

int AtracContext::GetSoundSample(int *outEndSample, int *outLoopStartSample,
                                 int *outLoopEndSample) const {
    const SceAtracIdInfo &info = Info();
    *outEndSample = info.endSample - info.firstValidSample;
    if (info.loopEnd == 0) {
        *outLoopStartSample = -1;
        *outLoopEndSample = -1;
    } else {
        *outLoopStartSample = info.loopStart - info.firstValidSample;
        *outLoopEndSample = info.loopEnd - info.firstValidSample;
    }
    return 0;
}

int AtracContext::ResetPlayPosition(int seekPos, int bytesWrittenFirstBuf,
                                    int bytesWrittenSecondBuf, bool *delay) {
    *delay = false;

    SceAtracIdInfo &info = Info();
    if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.secondBufferByte == 0) {
        return Err(kErrAtracSecondBufferNeeded);
    }

    seekPos += info.firstValidSample;

    if (static_cast<uint32_t>(seekPos) > static_cast<uint32_t>(info.endSample)) {
        return Err(kErrAtracBadSample);
    }

    int result = static_cast<int>(
        ResetPlayPositionInternal(seekPos, bytesWrittenFirstBuf, bytesWrittenSecondBuf));
    if (result >= 0) {
        int skipCount = 0;
        result = static_cast<int>(SkipFrames(&skipCount));
        if (skipCount) {
            *delay = true;
        }
    }
    return result;
}

uint32_t AtracContext::ResetPlayPositionInternal(int seekPos, int bytesWrittenFirstBuf,
                                                 int bytesWrittenSecondBuf) {
    // Redo the same calculation as before, for input validation.
    AtracResetBufferInfo bufferInfo;
    GetResetBufferInfoInternal(&bufferInfo, seekPos);

    // Input validation.
    if (static_cast<uint32_t>(bytesWrittenFirstBuf) < bufferInfo.first.minWriteBytes ||
        static_cast<uint32_t>(bytesWrittenFirstBuf) > bufferInfo.first.writableBytes) {
        return kErrAtracBadFirstResetSize;
    }
    if (static_cast<uint32_t>(bytesWrittenSecondBuf) < bufferInfo.second.minWriteBytes ||
        static_cast<uint32_t>(bytesWrittenSecondBuf) > bufferInfo.second.writableBytes) {
        return kErrAtracBadSecondResetSize;
    }

    SceAtracIdInfo &info = Info();
    info.decodePos = seekPos;
    info.numSkipFrames = static_cast<uint8_t>(ComputeSkipFrames(info, seekPos));
    info.loopNum = 0;
    info.curFileOff = ComputeFileOffset(info, seekPos);

    Context().codec.err = 0x20b; // wtf? testing shows it.

    switch (info.state) {
    case ATRAC_STATUS_ALL_DATA_LOADED:
        return 0;

    case ATRAC_STATUS_HALFWAY_BUFFER:
        info.streamDataByte += bytesWrittenFirstBuf;
        if (info.dataOff + info.streamDataByte >= info.fileDataEnd) {
            // Buffer full, we can transition to a full buffer here.
            info.state = ATRAC_STATUS_ALL_DATA_LOADED;
        }
        return 0;

    case ATRAC_STATUS_STREAMED_WITHOUT_LOOP:
    case ATRAC_STATUS_STREAMED_LOOP_FROM_END:
        // We just adopt the bytes that were written as our stream data.
        info.streamDataByte = bytesWrittenFirstBuf;
        info.curBuffer = 0;
        info.streamOff = 0;
        return 0;

    case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER: {
        const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
        if (info.curFileOff >= loopEndFileOffset) {
            const int secondBufferSizeRounded =
                RoundDownToMultiple(static_cast<int>(info.secondBufferByte), info.sampleSize);
            if (info.curFileOff < loopEndFileOffset + secondBufferSizeRounded) {
                info.streamDataByte =
                    ((loopEndFileOffset + secondBufferSizeRounded) - info.curFileOff) +
                    bytesWrittenFirstBuf;
                info.curBuffer = 1;
                info.secondStreamOff = info.curFileOff - loopEndFileOffset;
            } else {
                info.streamDataByte = bytesWrittenFirstBuf;
                info.curBuffer = 2; // Temporary value! Will immediately switch back to 0.
                info.streamOff = 0;
            }
        } else {
            info.streamDataByte = bytesWrittenFirstBuf;
            info.curBuffer = 0;
            info.streamOff = 0;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int AtracContext::GetBufferInfoForResetting(AtracResetBufferInfo *bufferInfo, int seekPos,
                                            bool *delay) {
    const SceAtracIdInfo &info = Info();

    if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.secondBufferByte == 0) {
        return Err(kErrAtracSecondBufferNeeded);
    }

    seekPos += info.firstValidSample;

    if (static_cast<uint32_t>(seekPos) > static_cast<uint32_t>(info.endSample)) {
        return Err(kErrAtracBadSample);
    }

    GetResetBufferInfoInternal(bufferInfo, seekPos);
    // Yes, this happens here! If there are any frames to skip, they get skipped!
    int skipCount = 0;
    const int retval = static_cast<int>(SkipFrames(&skipCount));
    if (skipCount > 0) {
        *delay = true;
    }
    return retval;
}

void AtracContext::GetResetBufferInfoInternal(AtracResetBufferInfo *bufferInfo, int seekPos) const {
    const SceAtracIdInfo &info = Info();
    auto u = [](int v) { return static_cast<uint32_t>(v); };

    switch (info.state) {
    case ATRAC_STATUS_NO_DATA:
    case ATRAC_STATUS_ALL_DATA_LOADED:
        // Everything is loaded, so nothing needs to be read.
        bufferInfo->first.writePosPtr = info.buffer;
        bufferInfo->first.writableBytes = 0;
        bufferInfo->first.minWriteBytes = 0;
        bufferInfo->first.filePos = 0;
        break;
    case ATRAC_STATUS_HALFWAY_BUFFER: {
        // Just ask to fill up the missing part of the buffer.
        const int streamPos = info.dataOff + info.streamDataByte;
        const int fileOff = info.dataOff + (seekPos / info.SamplesPerFrame() + 1) * info.sampleSize;
        bufferInfo->first.writePosPtr = info.buffer + u(streamPos);
        bufferInfo->first.writableBytes = u(info.fileDataEnd - streamPos);
        bufferInfo->first.filePos = u(streamPos);
        bufferInfo->first.minWriteBytes = u(std::max(0, fileOff - streamPos));
        break;
    }

    case ATRAC_STATUS_STREAMED_WITHOUT_LOOP:
    case ATRAC_STATUS_STREAMED_LOOP_FROM_END: {
        // Relatively easy, just can't forget those skipped frames.
        const int curFileOffset = ComputeFileOffset(info, seekPos);
        const int bufferEnd =
            RoundDownToMultiple(static_cast<int>(info.bufferByte), info.sampleSize);
        bufferInfo->first.writePosPtr = info.buffer;
        bufferInfo->first.writableBytes = u(std::min(info.fileDataEnd - curFileOffset, bufferEnd));
        bufferInfo->first.minWriteBytes =
            u((ComputeSkipFrames(info, seekPos) + 1) * info.sampleSize);
        bufferInfo->first.filePos = u(curFileOffset);
        break;
    }
    case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER: {
        // As usual, with the second buffer, things get crazy complicated...
        const int seekFileOffset = ComputeFileOffset(info, seekPos);
        const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd) - 1;
        const int bufferEnd =
            RoundDownToMultiple(static_cast<int>(info.bufferByte), info.sampleSize);
        const int skipBytes = (ComputeSkipFrames(info, seekPos) + 1) * info.sampleSize;
        const int secondBufferEnd =
            RoundDownToMultiple(static_cast<int>(info.secondBufferByte), info.sampleSize);
        if (seekFileOffset < loopEndFileOffset) {
            const int remainingBeforeLoop = (loopEndFileOffset - seekFileOffset) + 1;
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes = u(std::min(bufferEnd, remainingBeforeLoop));
            bufferInfo->first.minWriteBytes = u(std::min(skipBytes, remainingBeforeLoop));
            bufferInfo->first.filePos = u(seekFileOffset);
        } else if (loopEndFileOffset + secondBufferEnd <= seekFileOffset) {
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes =
                u(std::min(info.fileDataEnd - seekFileOffset, bufferEnd));
            bufferInfo->first.minWriteBytes = u(skipBytes);
            bufferInfo->first.filePos = u(seekFileOffset);
        } else if (loopEndFileOffset + static_cast<int>(info.secondBufferByte) + 1 <
                   info.fileDataEnd) {
            const int endOffset = loopEndFileOffset + secondBufferEnd + 1;
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes = u(std::min(info.fileDataEnd - endOffset, bufferEnd));
            bufferInfo->first.minWriteBytes =
                u(std::max(0, seekFileOffset + skipBytes - endOffset));
            bufferInfo->first.filePos = u(endOffset);
        } else {
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes = 0;
            bufferInfo->first.minWriteBytes = 0;
            bufferInfo->first.filePos = 0;
        }
        break;
    }
    default:
        // Upstream asserts; leave the first half as zeros.
        bufferInfo->first = {info.buffer, 0, 0, 0};
        break;
    }

    // Reset never needs a second buffer write, since the loop is in a fixed
    // place. second.writePosPtr is always the first buffer's pos, weirdly.
    bufferInfo->second.writePosPtr = info.buffer;
    bufferInfo->second.writableBytes = 0;
    bufferInfo->second.minWriteBytes = 0;
    bufferInfo->second.filePos = 0;
}

int AtracContext::SetLoopNum(int loopNum) {
    SceAtracIdInfo &info = Info();
    if (info.loopEnd <= 0) {
        // File doesn't contain loop information, looping isn't allowed.
        return Err(kErrAtracNoLoopInformation);
    }
    // Just override the current loop counter.
    info.loopNum = loopNum;
    return 0;
}

int AtracContext::LoopStatus() const {
    const SceAtracIdInfo &info = Info();
    if (info.loopEnd == 0) {
        // No loop available.
        return 0;
    } else if (info.loopNum != 0) {
        // We've got at least one loop to go.
        return 1;
    }
    // Return 1 if we haven't passed the loop point.
    return info.decodePos <= info.loopEnd ? 1 : 0;
}

uint32_t AtracContext::GetNextSamples() const {
    const SceAtracIdInfo &info = Info();
    const int endOfCurrentFrame = info.decodePos | info.SamplesFrameMask(); // bit trick!
    const int remainder = std::max(0, endOfCurrentFrame - info.endSample);
    const int adjusted = (info.decodePos & info.SamplesFrameMask()) + remainder;
    return static_cast<uint32_t>(std::max(0, info.SamplesPerFrame() - adjusted));
}

int AtracContext::GetNextDecodePosition(int *pos) const {
    const SceAtracIdInfo &info = Info();
    // Check if we reached the end.
    if (info.decodePos > info.endSample) {
        return Err(kErrAtracAllDataDecoded);
    }
    // Check if remaining data in the file is smaller than a frame.
    if (info.fileDataEnd - info.curFileOff < info.sampleSize) {
        return Err(kErrAtracAllDataDecoded);
    }
    *pos = info.decodePos - info.firstValidSample;
    return 0;
}

int AtracContext::AddStreamData(uint32_t bytesToAdd) {
    SceAtracIdInfo &info = Info();
    const int add = static_cast<int>(bytesToAdd);

    // WARNING: bytesToAdd might not be sampleSize aligned.
    if (info.state == ATRAC_STATUS_HALFWAY_BUFFER) {
        const int newFileOffset = info.streamDataByte + info.dataOff + add;
        if (newFileOffset == info.fileDataEnd) {
            info.state = ATRAC_STATUS_ALL_DATA_LOADED;
        } else if (newFileOffset > info.fileDataEnd) {
            return Err(kErrAtracAddDataIsTooBig);
        }
        info.streamDataByte += add;
    } else {
        info.streamDataByte += add;
    }
    return 0;
}

void AtracContext::GetStreamDataInfo(uint32_t *writePtr, uint32_t *bytesToWrite,
                                     uint32_t *readFileOffset) const {
    const SceAtracIdInfo &info = Info();

    switch (info.state) {
    case ATRAC_STATUS_ALL_DATA_LOADED:
        // Nothing to do, the whole track is loaded already.
        *writePtr = info.buffer;
        *bytesToWrite = 0;
        *readFileOffset = 0;
        break;

    case ATRAC_STATUS_HALFWAY_BUFFER: {
        // Both the file offset and the offset in the buffer (direct mapped).
        const int fileOffset = info.dataOff + info.streamDataByte;
        const int bytesLeftInFile = info.fileDataEnd - fileOffset;
        *writePtr = info.buffer + static_cast<uint32_t>(fileOffset);
        *bytesToWrite = static_cast<uint32_t>(bytesLeftInFile);
        *readFileOffset = static_cast<uint32_t>(fileOffset);
        break;
    }

    default:
        ComputeStreamBufferDataInfo(info, writePtr, bytesToWrite, readFileOffset);
        break;
    }
}

uint32_t AtracContext::DecodeData(uint32_t outbufAddr, int *SamplesNum, int *finish, int *remains) {
    SceAtracIdInfo &info = Info();

    const int tries = info.numSkipFrames + 1;
    for (int i = 0; i < tries; i++) {
        const uint32_t result = DecodeInternal(outbufAddr, SamplesNum, finish);
        if (result != 0) {
            *SamplesNum = 0;
            return result;
        }
    }

    *remains = RemainingFrames();
    return 0;
}

uint32_t AtracContext::DecodeInternal(uint32_t outbufAddr, int *SamplesNum, int *finish) {
    SceAtracIdInfo &info = Info();

    // Check for end of file.
    const int samplesToDecode = static_cast<int>(GetNextSamples());
    const int nextFileOff = info.curFileOff + info.sampleSize;
    if (nextFileOff > info.fileDataEnd || info.decodePos > info.endSample) {
        *finish = 1;
        return kErrAtracAllDataDecoded;
    }

    // Check for streaming buffer run-out.
    if (AtracStatusIsStreaming(info.state) && info.streamDataByte < info.sampleSize) {
        *finish = 0;
        return kErrAtracBufferIsEmpty;
    }

    // Check for halfway buffer end.
    if (info.state == ATRAC_STATUS_HALFWAY_BUFFER &&
        info.dataOff + info.streamDataByte < nextFileOff) {
        *finish = 0;
        return kErrAtracBufferIsEmpty;
    }

    uint32_t streamOff;
    uint32_t bufferPtr;
    if (!AtracStatusIsStreaming(info.state)) {
        bufferPtr = info.buffer;
        streamOff = static_cast<uint32_t>(info.curFileOff);
    } else {
        const int bufferIndex = info.curBuffer & 1;
        bufferPtr = bufferIndex == 0 ? info.buffer : info.secondBuffer;
        streamOff = static_cast<uint32_t>(bufferIndex == 0 ? info.streamOff : info.secondStreamOff);
    }

    const uint32_t inAddr = bufferPtr + streamOff;
    const size_t frameValues =
        static_cast<size_t>(info.SamplesPerFrame()) * static_cast<size_t>(outputChannels_);
    int16_t *outPtr;

    if (samplesToDecode != info.SamplesPerFrame()) {
        if (decodeTemp_.size() < frameValues) {
            decodeTemp_.assign(frameValues, 0);
        }
        outPtr = decodeTemp_.data();
    } else {
        // outbufAddr can be 0 during skip! An out-of-range buffer is treated
        // the same way (upstream would write through a bad pointer).
        outPtr = outbufAddr ? reinterpret_cast<int16_t *>(
                                  mem_.Ptr(outbufAddr, static_cast<uint32_t>(frameValues * 2)))
                            : nullptr;
    }

    Context().codec.inBuf = inAddr;
    Context().codec.outBuf = outbufAddr;

    const uint8_t *src = mem_.Ptr(inAddr, info.sampleSize);
    if (src == nullptr) {
        return kErrAtracApiFail;
    }

    int bytesConsumed = 0;
    int outSamples = 0;
    if (!DecodePacket(src, info.sampleSize, &bytesConsumed, outputChannels_, outPtr, &outSamples)) {
        // Decode failed.
        *finish = 0;
        // 0x20b and 0x20c have been observed for 0xFF and/or garbage data.
        Context().codec.err = 0x20b;
        return kErrAtracApiFail; // tested.
    }
    Context().codec.err = 0;

    // Advance the file offset.
    info.curFileOff += info.sampleSize;

    if (info.numSkipFrames == 0) {
        *SamplesNum = samplesToDecode;
        if (info.endSample < info.decodePos + samplesToDecode) {
            *finish = info.loopNum == 0;
        } else {
            *finish = 0;
        }
        if (samplesToDecode != info.SamplesPerFrame() && samplesToDecode != 0 && outbufAddr) {
            const uint32_t bytes = static_cast<uint32_t>(samplesToDecode * outputChannels_ * 2);
            if (uint8_t *outBuf = mem_.Ptr(outbufAddr, bytes)) {
                std::memcpy(outBuf, decodeTemp_.data(), bytes);
            }
        }

        // Handle increments and looping.
        info.decodePos += samplesToDecode;
        if (info.loopEnd != 0 && info.loopNum != 0 && info.decodePos > info.loopEnd) {
            info.curFileOff = ComputeFileOffset(info, info.loopStart);
            info.numSkipFrames = static_cast<uint8_t>(ComputeSkipFrames(info, info.loopStart));
            info.decodePos = info.loopStart;
            if (info.loopNum > 0) {
                info.loopNum--;
            }
        }
    } else {
        info.numSkipFrames--;
    }

    // Handle streaming special cases.
    if (AtracStatusIsStreaming(info.state)) {
        info.streamDataByte -= info.sampleSize;
        if (info.curBuffer == 1) {
            // If currently streaming from the second buffer...
            const int nextStreamOff = info.secondStreamOff + info.sampleSize;
            if (static_cast<int>(info.secondBufferByte) < nextStreamOff + info.sampleSize) {
                // Done/ran out
                info.streamOff = 0;
                info.secondStreamOff = 0;
                info.curBuffer = 2;
            } else {
                info.secondStreamOff = nextStreamOff;
            }
        } else {
            // Normal streaming from the main buffer. First, wrapping around the end...
            const int nextStreamOff = info.streamOff + info.sampleSize;
            if (nextStreamOff + info.sampleSize > static_cast<int>(info.bufferByte)) {
                info.streamOff = 0;
            } else {
                info.streamOff = nextStreamOff;
            }

            // If we're in LOOP_WITH_TRAILER, streaming from the main buffer,
            // and either there's no loop or we're done with the final loop
            // and past the loop point...
            if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.curBuffer == 0 &&
                (info.loopEnd == 0 || (info.loopNum == 0 && info.loopEnd < info.decodePos))) {
                // ...and our file streaming offset has reached the loop point...
                if (info.curFileOff >= ComputeLoopEndFileOffset(info, info.loopEnd)) {
                    // ...switch to the secondary buffer, and copy the last
                    // partial packet from the second buffer back to the start
                    // of the main buffer.
                    info.curBuffer = 1;
                    info.streamDataByte = static_cast<int>(info.secondBufferByte);
                    info.secondStreamOff = 0;
                    const uint32_t partial = info.secondBufferByte % info.sampleSize;
                    mem_.Memmove(info.buffer, info.secondBuffer + (info.secondBufferByte - partial),
                                 partial);
                }
            }
        }
    }
    return 0;
}

int AtracContext::SetData(const Track & /*track*/, uint32_t bufferAddr, uint32_t readSize,
                          uint32_t bufferSize, uint32_t fileSize, int outputChannels, bool isAA3) {
    TrackInfo trackInfo{};
    if (mem_.IsValidAddress(bufferAddr)) {
        // Games can abuse bufferSize, so it is not used to bound the parse;
        // readSize is, clamped to what really exists in PSP memory.
        const uint8_t *bufferPtr = mem_.Ptr(bufferAddr, 1);
        const uint32_t avail = mem_.BytesFrom(bufferAddr);
        if (!isAA3) {
            const int retval = ParseWaveAT3(bufferPtr, avail, readSize, &trackInfo);
            if (retval < 0) {
                return retval;
            }
        } else {
            const int retval = ParseAA3(bufferPtr, avail, readSize, fileSize, &trackInfo);
            if (retval < 0) {
                return retval;
            }
        }
    }

    int retval = InitContextFromTrackInfo(&Context(), &trackInfo, bufferAddr,
                                          static_cast<int>(readSize), static_cast<int>(bufferSize));
    if (retval < 0) {
        return retval;
    }

    SceAtracIdInfo &info = Info();

    CreateDecoder(info.codec, info.sampleSize, info.numChan);

    outputChannels_ = static_cast<uint16_t>(outputChannels);

    int skipCount = 0;
    retval = static_cast<int>(SkipFrames(&skipCount));
    // Upstream logs SCE_ERROR_ATRAC_API_FAIL ("bad frame during initial skip")
    // and other errors, and returns them as is.
    WrapLastPacket();
    return retval;
}

void AtracContext::WrapLastPacket() {
    SceAtracIdInfo &info = Info();

    // If streaming, we need to handle wrapping the overshot partial packet at
    // the end. If not we don't.
    if (!AtracStatusIsStreaming(info.state)) {
        return;
    }

    const int distanceToEnd =
        RoundDownToMultiple(static_cast<int>(info.bufferByte) - info.streamOff, info.sampleSize);
    if (info.streamDataByte < distanceToEnd) {
        // There's space left without wrapping. Upstream zeroes 128 bytes here
        // to match a hardware hexdump.
        mem_.Memset(info.buffer, 0, 128);
    } else {
        // Wraps around: copy the split packet to the start of the buffer.
        const int copyStart = info.streamOff + distanceToEnd;
        const int copyLen = static_cast<int>(info.bufferByte) - copyStart;
        mem_.Memmove(info.buffer, info.buffer + static_cast<uint32_t>(copyStart),
                     static_cast<uint32_t>(copyLen));
    }
}

uint32_t AtracContext::SkipFrames(int *skipCount) {
    SceAtracIdInfo &info = Info();
    *skipCount = 0;
    int finishIgnored;
    while (true) {
        if (info.numSkipFrames == 0) {
            return 0;
        }
        const uint32_t retval = DecodeInternal(0, nullptr, &finishIgnored);
        if (retval != 0) {
            if (retval == kErrAtracApiFail) {
                (*skipCount)++;
            }
            return retval;
        }
        (*skipCount)++;
    }
}

int AtracContext::GetSecondBufferInfo(uint32_t *fileOffset, uint32_t *readSize) const {
    const SceAtracIdInfo &info = Info();
    if (info.state != ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER) {
        // No second buffer needed in this state.
        *fileOffset = 0;
        *readSize = 0;
        return Err(kErrAtracSecondBufferNotNeeded);
    }

    const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
    *fileOffset = static_cast<uint32_t>(loopEndFileOffset);
    *readSize = static_cast<uint32_t>(info.fileDataEnd - loopEndFileOffset);
    return 0;
}

int AtracContext::SetSecondBuffer(uint32_t secondBuffer, uint32_t secondBufferSize) {
    SceAtracIdInfo &info = Info();

    const uint32_t loopEndFileOffset =
        static_cast<uint32_t>(ComputeLoopEndFileOffset(info, info.loopEnd));
    if ((info.sampleSize * 3 <= static_cast<int>(secondBufferSize) ||
         (info.fileDataEnd - static_cast<int>(loopEndFileOffset)) <=
             static_cast<int>(secondBufferSize))) {
        if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER) {
            info.secondBuffer = secondBuffer;
            info.secondBufferByte = secondBufferSize;
            info.secondStreamOff = 0;
            return 0;
        }
        return Err(kErrAtracSecondBufferNotNeeded);
    }
    return Err(kErrAtracSizeTooSmall);
}

void AtracContext::InitLowLevel(const Atrac3LowLevelParams &params, int codecType) {
    SceAtracIdInfo &info = Info();
    info.codec = static_cast<uint16_t>(codecType);
    info.numChan = static_cast<uint8_t>(params.encodedChannels);
    outputChannels_ = static_cast<uint16_t>(params.outputChannels);
    info.sampleSize = static_cast<uint16_t>(params.bytesPerFrame);
    info.dataOff = 0;
    info.decodePos = 0;
    info.state = ATRAC_STATUS_LOW_LEVEL;
    CreateDecoder(codecType, info.sampleSize, info.numChan);
}

int AtracContext::DecodeLowLevel(const uint8_t *srcData, int *bytesConsumed, int16_t *dstData,
                                 int *bytesWritten) {
    const SceAtracIdInfo &info = Info();
    const int channels = outputChannels_;
    int outSamples = 0;
    // Upstream proceeds even if decoding fails (PPSSPP issue #20452).
    DecodePacket(srcData, info.sampleSize, bytesConsumed, channels, dstData, &outSamples);
    *bytesWritten = outSamples * channels * static_cast<int>(sizeof(int16_t));
    return 0;
}

void AtracContext::CheckForSas() {
    const SceAtracIdInfo &info = Info();
    // Upstream warns if numChan != 1 or state != 0x10 (the caller sets them).
    sas_.isStreaming = info.fileDataEnd > static_cast<int32_t>(info.bufferByte);
}

int AtracContext::EnqueueForSas(uint32_t address, uint32_t ptr) {
    SceAtracIdInfo &info = Info();
    // Set the new buffer up to be adopted by the next call to Decode that
    // needs more data. Only valid when the decoder asked for another buffer.
    if (info.secondBuffer != 0xFFFFFFFF) {
        return Err(kErrSasAtrac3AlreadyQueued);
    }
    info.secondBuffer = address;
    info.secondBufferByte = ptr;
    return 0;
}

// Completely different streaming setup!
void AtracContext::DecodeForSas(int16_t *dstData, int *bytesWritten, int *finish) {
    SceAtracIdInfo &info = Info();
    *bytesWritten = 0;

    // First frame handling: set up the initial buffer as the current streaming
    // buffer. Also works for the non-streaming case.
    if (info.buffer) {
        sas_.curBuffer = 0;
        sas_.bufPtr[0] = info.buffer;
        sas_.bufSize[0] = info.bufferByte - static_cast<uint32_t>(info.streamOff);
        sas_.streamOffset = 0;
        sas_.fileOffset = static_cast<int>(info.bufferByte);
        info.buffer = 0; // yes, this happens.
    }

    const int sampleSize = info.sampleSize;
    const int cur = sas_.curBuffer;
    if (sas_.streamOffset + sampleSize <= static_cast<int>(sas_.bufSize[cur])) {
        // Just decode.
        const uint8_t *srcData =
            mem_.Ptr(sas_.bufPtr[cur] + static_cast<uint32_t>(sas_.streamOffset),
                     static_cast<uint32_t>(sampleSize));
        int bytesConsumed = 0;
        if (srcData != nullptr) {
            DecodePacket(srcData, sampleSize, &bytesConsumed, 1, dstData, bytesWritten);
        }
        sas_.streamOffset += bytesConsumed;
    } else if (sas_.isStreaming) {
        // Compute the part sizes using the current size.
        const int part1Size = static_cast<int>(sas_.bufSize[cur]) - sas_.streamOffset;
        const int part2Size = sampleSize - part1Size;
        std::vector<uint8_t> assembly(static_cast<size_t>(sampleSize), 0);
        if (part1Size >= 0) {
            // Grab the partial packet, before we switch over to the other buffer.
            mem_.CopyFrom(assembly.data(),
                          sas_.bufPtr[cur] + static_cast<uint32_t>(sas_.streamOffset),
                          static_cast<uint32_t>(part1Size));
        }

        // Check if we hit the end.
        if (sas_.fileOffset >= info.fileDataEnd) {
            *bytesWritten = 0;
            *finish = 1;
            return;
        }

        // Check that a new buffer actually exists.
        if (info.secondBuffer == sas_.bufPtr[cur]) {
            // Can't enqueue the same buffer twice in a row!
            *bytesWritten = 0;
            *finish = 1;
            return;
        }

        if (static_cast<int32_t>(info.secondBuffer) < 0) {
            // AtracSas streaming ran out of data, no secondbuffer pending.
            *bytesWritten = 0;
            *finish = 1;
            return;
        }

        // Switch to the other buffer.
        sas_.curBuffer ^= 1;

        sas_.bufPtr[sas_.curBuffer] = info.secondBuffer;
        sas_.bufSize[sas_.curBuffer] = info.secondBufferByte;
        sas_.fileOffset += static_cast<int>(info.secondBufferByte);

        sas_.streamOffset = part2Size;

        // If we'll reach the end during this buffer, set second buffer to 0,
        // signaling that we don't need more data.
        if (sas_.fileOffset >= info.fileDataEnd) {
            info.secondBuffer = 0;
        } else {
            // Signal to the caller that we accept a new next buffer.
            info.secondBuffer = 0xFFFFFFFF;
        }

        // Copy the second half (or if part1Size == 0, the whole packet).
        if (part1Size >= 0 && part2Size > 0) {
            mem_.CopyFrom(assembly.data() + part1Size, sas_.bufPtr[sas_.curBuffer],
                          static_cast<uint32_t>(part2Size));
        }
        int bytesConsumed = 0;
        DecodePacket(assembly.data(), sampleSize, &bytesConsumed, 1, dstData, bytesWritten);
    }
}

} // namespace mpo::audio
