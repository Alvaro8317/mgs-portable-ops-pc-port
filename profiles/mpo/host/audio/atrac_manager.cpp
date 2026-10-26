// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/sceAtrac.cpp @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Same checks, writes and return codes as upstream; logging, the mute flags
// and savestates are left out, and the HLE delays are reported through
// LastDelayUs() (see atrac_manager.h).

#include "atrac_manager.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "audio_errors.h"

namespace mpo::audio {
namespace {

constexpr int kAtracDecodeDelayUs = 2300;

constexpr int Err(uint32_t code) {
    return static_cast<int>(code);
}

} // namespace

AtracManager::AtracManager(const GuestMemory &mem, AtracDecoderFactory factory)
    : mem_(mem), factory_(std::move(factory)) {
    // __AtracInit: start with 2 of each in this order.
    contextTypes_ = {PSP_CODEC_AT3PLUS, PSP_CODEC_AT3PLUS, PSP_CODEC_AT3, PSP_CODEC_AT3, 0, 0};
}

bool AtracManager::NotifyLoadModule(int version, uint32_t bssAddr, uint32_t bssSize) {
    libVersion_ = version;
    const int maxContexts = version <= 0x101 ? 4 : 6;
    const uint32_t needed = static_cast<uint32_t>(maxContexts) * 256;
    if (bssSize < needed || (bssAddr & 3) != 0 || !mem_.IsValidRange(bssAddr, needed)) {
        bss_ = 0;
        return false;
    }
    bss_ = bssAddr;
    maxContexts_ = maxContexts;
    mem_.Memset(bss_, 0, needed);
    return true;
}

void AtracManager::NotifyUnloadModule() {
    libVersion_ = 0;
    bss_ = 0;
    maxContexts_ = 6; // Upstream TODO: should probably be zero.
}

AtracContext *AtracManager::Get(int atracID) {
    if (atracID < 0 || atracID >= PSP_MAX_ATRAC_IDS) {
        return nullptr;
    }
    return contexts_[static_cast<size_t>(atracID)].get();
}

int AtracManager::AllocAndRegister(uint32_t codecType) {
    if (bss_ == 0) {
        // See atrac_manager.h: no legacy (non-BSS) contexts.
        return Err(kErrAtracNoAtracId);
    }
    for (int i = 0; i < maxContexts_; ++i) {
        const size_t idx = static_cast<size_t>(i);
        if (contextTypes_[idx] == codecType && !contexts_[idx]) {
            contexts_[idx] = std::make_unique<AtracContext>(mem_, ContextAddress(i),
                                                            static_cast<int>(codecType), factory_);
            return i;
        }
    }
    return Err(kErrAtracNoAtracId);
}

int AtracManager::UnregisterAndDelete(int atracID) {
    if (atracID >= 0 && atracID < PSP_MAX_ATRAC_IDS && contexts_[static_cast<size_t>(atracID)]) {
        contexts_[static_cast<size_t>(atracID)].reset();
        return 0;
    }
    return Err(kErrAtracBadAtracId);
}

uint32_t AtracManager::ValidateData(const AtracContext *atrac) const {
    if (!atrac) {
        return kErrAtracBadAtracId;
    } else if (atrac->BufferState() == ATRAC_STATUS_NO_DATA) {
        return kErrAtracNoData;
    }
    return 0;
}

uint32_t AtracManager::ValidateManaged(const AtracContext *atrac) const {
    if (!atrac) {
        return kErrAtracBadAtracId;
    } else if (atrac->BufferState() == ATRAC_STATUS_NO_DATA) {
        return kErrAtracNoData;
    } else if (atrac->BufferState() == ATRAC_STATUS_LOW_LEVEL) {
        return kErrAtracIsLowLevel;
    } else if (atrac->BufferState() == ATRAC_STATUS_FOR_SCESAS) {
        return kErrAtracIsForSceSas;
    }
    return 0;
}

// Really, allocate an Atrac context of a specific codec type.
uint32_t AtracManager::GetAtracID(int codecType) {
    lastDelayUs_ = 0;
    if (codecType != static_cast<int>(PSP_CODEC_AT3) &&
        codecType != static_cast<int>(PSP_CODEC_AT3PLUS)) {
        return kErrAtracInvalidCodecType;
    }
    return static_cast<uint32_t>(AllocAndRegister(static_cast<uint32_t>(codecType)));
}

// Notifies that more data is (or will be very soon) available in the buffer.
uint32_t AtracManager::AddStreamData(int atracID, uint32_t bytesToAdd) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    if (atrac->BufferState() == ATRAC_STATUS_ALL_DATA_LOADED) {
        return kErrAtracAllDataLoaded;
    }
    return static_cast<uint32_t>(atrac->AddStreamData(bytesToAdd));
}

// Note that outAddr being null is completely valid here, used to skip data.
uint32_t AtracManager::DecodeData(int atracID, uint32_t outAddr, uint32_t numSamplesAddr,
                                  uint32_t finishFlagAddr, uint32_t remainAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    if (outAddr & 1) {
        return kErrAtracBadAlignment;
    }

    int numSamplesWritten = 0;
    int finish = 0;
    int remains = 0;
    if (outAddr != 0 && !mem_.IsValidAddress(outAddr)) {
        // Upstream: unknown error code, but it has to bail.
        return kErrAtracSizeTooSmall;
    }

    const uint32_t ret = atrac->DecodeData(outAddr, &numSamplesWritten, &finish, &remains);
    if (ret != kErrAtracBadAtracId && ret != kErrAtracNoData) {
        mem_.Write32(numSamplesAddr, static_cast<uint32_t>(numSamplesWritten));
        mem_.Write32(finishFlagAddr, static_cast<uint32_t>(finish));
        // On error, no remaining frame value is written.
        if (ret == 0) {
            mem_.Write32(remainAddr, static_cast<uint32_t>(remains));
        }
    }

    if (ret == 0 || ret == kErrAtracApiFail) {
        // Decoded or at least attempted to decode data, delay thread.
        return static_cast<uint32_t>(Delay(static_cast<int>(ret), kAtracDecodeDelayUs));
    }
    return ret;
}

uint32_t AtracManager::ReleaseResources() {
    lastDelayUs_ = 0;
    // The real one just calls sceAudiocodecReleaseEDRAM if any has been allocated.
    return 0;
}

uint32_t AtracManager::GetBufferInfoForResetting(int atracID, int sample, uint32_t bufferInfoAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    uint8_t *guest = mem_.Ptr(bufferInfoAddr, sizeof(AtracResetBufferInfo));
    if (guest == nullptr) {
        return kErrKernelIllegalAddr; // "invalid buffer, should crash"
    }
    // Upstream writes straight into PSP memory; read-modify-write keeps the
    // fields an early error leaves untouched.
    AtracResetBufferInfo info;
    std::memcpy(&info, guest, sizeof(info));
    bool delay = false;
    const int ret = atrac->GetBufferInfoForResetting(&info, sample, &delay);
    std::memcpy(guest, &info, sizeof(info));
    if (delay) {
        return static_cast<uint32_t>(Delay(ret, 300));
    }
    return static_cast<uint32_t>(ret);
}

uint32_t AtracManager::GetBitrate(int atracID, uint32_t outBitrateAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    mem_.Write32(outBitrateAddr, static_cast<uint32_t>(atrac->Bitrate()));
    return 0;
}

uint32_t AtracManager::GetChannel(int atracID, uint32_t channelAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    mem_.Write32(channelAddr, static_cast<uint32_t>(atrac->Channels()));
    return 0;
}

uint32_t AtracManager::GetLoopStatus(int atracID, uint32_t loopNumAddr, uint32_t statusAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    mem_.Write32(loopNumAddr, static_cast<uint32_t>(atrac->LoopNum()));
    mem_.Write32(statusAddr, static_cast<uint32_t>(atrac->LoopStatus()));
    return 0;
}

uint32_t AtracManager::GetInternalErrorInfo(int atracID, uint32_t errorAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    mem_.Write32(errorAddr, atrac->GetInternalCodecError());
    return 0;
}

uint32_t AtracManager::GetMaxSample(int atracID, uint32_t maxSamplesAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    mem_.Write32(maxSamplesAddr, static_cast<uint32_t>(atrac->SamplesPerFrame()));
    return 0;
}

uint32_t AtracManager::GetNextDecodePosition(int atracID, uint32_t outposAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    if (!mem_.IsValidAddress(outposAddr)) {
        return 0; // "invalid address"
    }
    int pos = 0;
    const int ret = atrac->GetNextDecodePosition(&pos);
    if (ret < 0) {
        return static_cast<uint32_t>(ret);
    }
    mem_.Write32(outposAddr, static_cast<uint32_t>(pos));
    return 0;
}

uint32_t AtracManager::GetNextSample(int atracID, uint32_t outNAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    mem_.Write32(outNAddr, atrac->GetNextSamples());
    return 0;
}

// Number of frames remaining in the buffer which can be decoded. When no more
// data would be needed, this is a negative number.
uint32_t AtracManager::GetRemainFrame(int atracID, uint32_t remainAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    if (!mem_.IsValidAddress(remainAddr)) {
        return kErrKernelIllegalAddr; // Would crash.
    }
    mem_.Write32(remainAddr, static_cast<uint32_t>(atrac->RemainingFrames()));
    return 0;
}

uint32_t AtracManager::GetSecondBufferInfo(int atracID, uint32_t fileOffsetAddr,
                                           uint32_t desiredSizeAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    if (!mem_.IsValidRange(fileOffsetAddr, 4) || !mem_.IsValidRange(desiredSizeAddr, 4)) {
        return kErrKernelIllegalAddr; // Would crash.
    }
    uint32_t fileOffset = 0;
    uint32_t desiredSize = 0;
    const int result = atrac->GetSecondBufferInfo(&fileOffset, &desiredSize);
    mem_.Write32(fileOffsetAddr, fileOffset);
    mem_.Write32(desiredSizeAddr, desiredSize);
    return static_cast<uint32_t>(result);
}

uint32_t AtracManager::GetSoundSample(int atracID, uint32_t outEndSampleAddr,
                                      uint32_t outLoopStartSampleAddr,
                                      uint32_t outLoopEndSampleAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    int endSample = -1;
    int loopStart = -1;
    int loopEnd = -1;
    const int retval = atrac->GetSoundSample(&endSample, &loopStart, &loopEnd);
    if (retval < 0) {
        return static_cast<uint32_t>(retval);
    }
    mem_.Write32(outEndSampleAddr, static_cast<uint32_t>(endSample));
    mem_.Write32(outLoopStartSampleAddr, static_cast<uint32_t>(loopStart));
    mem_.Write32(outLoopEndSampleAddr, static_cast<uint32_t>(loopEnd));
    return static_cast<uint32_t>(retval);
}

// Where the next data has to be read from and written to.
uint32_t AtracManager::GetStreamDataInfo(int atracID, uint32_t writePtrAddr,
                                         uint32_t writableBytesAddr, uint32_t readOffsetAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    uint32_t writePtr = 0;
    uint32_t writableBytes = 0;
    uint32_t readOffset = 0;
    atrac->GetStreamDataInfo(&writePtr, &writableBytes, &readOffset);
    mem_.Write32(writePtrAddr, writePtr);
    mem_.Write32(writableBytesAddr, writableBytes);
    mem_.Write32(readOffsetAddr, readOffset);
    return 0;
}

uint32_t AtracManager::ReleaseAtracID(int atracID) {
    lastDelayUs_ = 0;
    return static_cast<uint32_t>(UnregisterAndDelete(atracID));
}

// Seek (or "reset") to a position in the audio data.
uint32_t AtracManager::ResetPlayPosition(int atracID, int sample, int bytesWrittenFirstBuf,
                                         int bytesWrittenSecondBuf) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    bool delay = false;
    const int res =
        atrac->ResetPlayPosition(sample, bytesWrittenFirstBuf, bytesWrittenSecondBuf, &delay);
    if (res < 0) {
        return static_cast<uint32_t>(delay ? Delay(res, 200) : res);
    }
    return static_cast<uint32_t>(Delay(res, 3000));
}

uint32_t AtracManager::SetHalfwayBuffer(int atracID, uint32_t buffer, uint32_t readSize,
                                        uint32_t bufferSize) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    // Don't use ValidateManaged here.
    if (!atrac) {
        return kErrAtracBadAtracId;
    }
    if (readSize > bufferSize) {
        return kErrAtracIncorrectReadSize;
    }
    Track track;
    std::string error;
    int ret =
        AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), readSize, &track, &error);
    if (ret < 0) {
        return static_cast<uint32_t>(ret);
    }
    if (track.codecType != contextTypes_[static_cast<size_t>(atracID)]) {
        return kErrAtracWrongCodecType;
    }
    ret = atrac->SetData(track, buffer, readSize, bufferSize, 0, 2, false);
    if (ret < 0) {
        return static_cast<uint32_t>(ret); // Must not delay.
    }
    return static_cast<uint32_t>(Delay(ret, 100));
}

uint32_t AtracManager::SetSecondBuffer(int atracID, uint32_t secondBuffer,
                                       uint32_t secondBufferSize) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return err;
    }
    return static_cast<uint32_t>(atrac->SetSecondBuffer(secondBuffer, secondBufferSize));
}

uint32_t AtracManager::SetData(int atracID, uint32_t buffer, uint32_t bufferSize) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (!atrac) {
        return kErrAtracBadAtracId;
    }
    Track track;
    std::string error;
    int ret =
        AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), bufferSize, &track, &error);
    if (ret < 0) {
        return static_cast<uint32_t>(ret);
    }
    if (track.codecType != contextTypes_[static_cast<size_t>(atracID)]) {
        return kErrAtracWrongCodecType;
    }
    ret = atrac->SetData(track, buffer, bufferSize, bufferSize, 0, 2, false);
    if (ret < 0) {
        return static_cast<uint32_t>(ret); // Must not delay.
    }
    return static_cast<uint32_t>(Delay(ret, 100));
}

int AtracManager::SetDataAndGetID(uint32_t buffer, int bufferSize) {
    lastDelayUs_ = 0;
    // A large value happens in Tales of VS, and isn't handled somewhere
    // properly as a u32. It's impossible for it to be that big anyway, so cap it.
    if (bufferSize < 0) {
        bufferSize = 0x10000000;
    }
    const uint32_t size = static_cast<uint32_t>(bufferSize);

    Track track;
    std::string error;
    // A zero pointer goes through as zeroes, so the small size check can kick
    // in before it's accessed (Tomb Raider Legend, PPSSPP #20145).
    int ret = AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), size, &track, &error);
    if (ret < 0) {
        return ret;
    }

    const int atracID = AllocAndRegister(track.codecType);
    if (atracID < 0) {
        return atracID;
    }

    ret = contexts_[static_cast<size_t>(atracID)]->SetData(track, buffer, size, size, 0, 2, false);
    if (ret < 0) {
        UnregisterAndDelete(atracID);
        return ret;
    }
    return Delay(atracID, 100);
}

int AtracManager::SetHalfwayBufferAndGetID(uint32_t buffer, uint32_t readSize,
                                           uint32_t bufferSize) {
    lastDelayUs_ = 0;
    if (readSize > bufferSize) {
        return Err(kErrAtracIncorrectReadSize);
    }
    Track track;
    std::string error;
    int ret =
        AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), readSize, &track, &error);
    if (ret < 0) {
        return ret;
    }
    const int atracID = AllocAndRegister(track.codecType);
    if (atracID < 0) {
        return atracID;
    }
    ret = contexts_[static_cast<size_t>(atracID)]->SetData(track, buffer, readSize, bufferSize, 0,
                                                           2, false);
    if (ret < 0) {
        UnregisterAndDelete(atracID);
        return ret;
    }
    return Delay(atracID, 100);
}

uint32_t AtracManager::StartEntry() {
    lastDelayUs_ = 0;
    return 0; // Unimplemented upstream too.
}

uint32_t AtracManager::SetLoopNum(int atracID, int loopNum) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return err;
    }
    // NO_LOOP_INFORMATION with loopNum -1 or 0 is "not really an issue"
    // upstream; it is still returned as is.
    return static_cast<uint32_t>(atrac->SetLoopNum(loopNum));
}

int AtracManager::Reinit(int at3Count, int at3plusCount) {
    lastDelayUs_ = 0;
    for (const auto &context : contexts_) {
        if (context) {
            return Err(kErrKernelBusy); // cannot reinit while IDs in use
        }
    }

    contextTypes_.fill(0);
    size_t next = 0;
    int space = maxContexts_;

    // This seems to deinit things. Mostly, it causes a reschedule on next
    // deinit (but -1, -1 does not.)
    if (at3Count == 0 && at3plusCount == 0) {
        inited_ = false;
        return Delay(0, 200);
    }

    // First, ATRAC3+. These IDs seem to cost double (probably memory.)
    // Intentionally signed. 9999 tries to allocate, -1 does not.
    for (int i = 0; i < at3plusCount; ++i) {
        space -= 2;
        if (space >= 0) {
            contextTypes_[next++] = PSP_CODEC_AT3PLUS;
        }
    }
    for (int i = 0; i < at3Count; ++i) {
        space -= 1;
        if (space >= 0) {
            contextTypes_[next++] = PSP_CODEC_AT3;
        }
    }

    // If we ran out of space, we still initialize some, but return an error.
    const int result = space >= 0 ? 0 : Err(kErrKernelOutOfMemory);
    if (inited_ || next == 0) {
        inited_ = true;
        return result;
    }
    inited_ = true;
    return Delay(result, 400);
}

int AtracManager::GetOutputChannel(int atracID, uint32_t outputChanPtr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateData(atrac)) {
        return static_cast<int>(err);
    }
    mem_.Write32(outputChanPtr, static_cast<uint32_t>(atrac->GetOutputChannels()));
    return 0;
}

int AtracManager::IsSecondBufferNeeded(int atracID) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (const uint32_t err = ValidateManaged(atrac)) {
        return static_cast<int>(err);
    }
    // Note that this returns true whether the buffer is already set or not.
    return atrac->BufferState() == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER ? 1 : 0;
}

int AtracManager::SetMOutHalfwayBuffer(int atracID, uint32_t buffer, uint32_t readSize,
                                       uint32_t bufferSize) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (!atrac) {
        return Err(kErrAtracBadAtracId);
    }
    if (readSize > bufferSize) {
        return Err(kErrAtracIncorrectReadSize);
    }
    Track track;
    std::string error;
    int ret =
        AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), readSize, &track, &error);
    if (ret < 0) {
        return ret;
    }
    ret = atrac->SetData(track, buffer, readSize, bufferSize, 0, 1, false);
    if (ret < 0 && ret != Err(kErrAtracNotMono)) {
        return ret; // Must not delay.
    }
    return Delay(ret, 100);
}

uint32_t AtracManager::SetMOutData(int atracID, uint32_t buffer, uint32_t bufferSize) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (!atrac) {
        return kErrAtracBadAtracId;
    }
    Track track;
    std::string error;
    int ret =
        AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), bufferSize, &track, &error);
    if (ret < 0) {
        return static_cast<uint32_t>(ret);
    }
    ret = atrac->SetData(track, buffer, bufferSize, bufferSize, 0, 1, false);
    if (ret < 0 && ret != Err(kErrAtracNotMono)) {
        return static_cast<uint32_t>(ret);
    }
    return static_cast<uint32_t>(Delay(ret, 100));
}

int AtracManager::SetMOutDataAndGetID(uint32_t buffer, uint32_t bufferSize) {
    lastDelayUs_ = 0;
    Track track;
    std::string error;
    int ret =
        AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), bufferSize, &track, &error);
    if (ret < 0) {
        return ret;
    }
    if (track.channels != 1) {
        return Err(kErrAtracNotMono);
    }
    const int atracID = AllocAndRegister(track.codecType);
    if (atracID < 0) {
        return atracID;
    }
    ret = contexts_[static_cast<size_t>(atracID)]->SetData(track, buffer, bufferSize, bufferSize, 0,
                                                           1, false);
    if (ret < 0 && ret != Err(kErrAtracNotMono)) {
        UnregisterAndDelete(atracID);
        return ret;
    }
    return Delay(atracID, 100);
}

int AtracManager::SetMOutHalfwayBufferAndGetID(uint32_t buffer, uint32_t readSize,
                                               uint32_t bufferSize) {
    lastDelayUs_ = 0;
    if (readSize > bufferSize) {
        return Err(kErrAtracIncorrectReadSize);
    }
    Track track;
    std::string error;
    int ret =
        AnalyzeAtracTrack(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), readSize, &track, &error);
    if (ret < 0) {
        return ret;
    }
    if (track.channels != 1) {
        return Err(kErrAtracNotMono);
    }
    const int atracID = AllocAndRegister(track.codecType);
    if (atracID < 0) {
        return atracID;
    }
    ret = contexts_[static_cast<size_t>(atracID)]->SetData(track, buffer, readSize, bufferSize, 0,
                                                           1, false);
    if (ret < 0 && ret != Err(kErrAtracNotMono)) {
        UnregisterAndDelete(atracID);
        return ret;
    }
    return Delay(atracID, 100);
}

int AtracManager::SetAA3DataAndGetID(uint32_t buffer, uint32_t bufferSize, uint32_t fileSize,
                                     uint32_t /*metadataSizeAddr*/) {
    lastDelayUs_ = 0;
    Track track;
    std::string error;
    int ret = AnalyzeAA3Track(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), bufferSize, fileSize,
                              &track, &error);
    if (ret < 0) {
        return ret;
    }
    const int atracID = AllocAndRegister(track.codecType);
    if (atracID < 0) {
        return atracID;
    }
    ret = contexts_[static_cast<size_t>(atracID)]->SetData(track, buffer, bufferSize, bufferSize,
                                                           fileSize, 2, true);
    if (ret < 0) {
        UnregisterAndDelete(atracID);
        return ret;
    }
    return Delay(atracID, 100);
}

int AtracManager::SetAA3HalfwayBufferAndGetID(uint32_t buffer, uint32_t readSize,
                                              uint32_t bufferSize, uint32_t fileSize) {
    lastDelayUs_ = 0;
    if (readSize > bufferSize) {
        return Err(kErrAtracIncorrectReadSize);
    }
    Track track;
    std::string error;
    int ret = AnalyzeAA3Track(mem_.Ptr(buffer, 1), mem_.BytesFrom(buffer), readSize, fileSize,
                              &track, &error);
    if (ret < 0) {
        return ret;
    }
    const int atracID = AllocAndRegister(track.codecType);
    if (atracID < 0) {
        return atracID;
    }
    ret = contexts_[static_cast<size_t>(atracID)]->SetData(track, buffer, readSize, bufferSize,
                                                           fileSize, 2, true);
    if (ret < 0) {
        UnregisterAndDelete(atracID);
        return ret;
    }
    return Delay(atracID, 100);
}

uint32_t AtracManager::GetContextAddress(int atracID) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (!atrac) {
        return 0; // "bad atrac id"
    }
    return atrac->ContextAddress();
}

int AtracManager::LowLevelInitDecoder(int atracID, uint32_t paramsAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (!atrac) {
        return Err(kErrAtracBadAtracId);
    }
    Atrac3LowLevelParams params;
    if (!mem_.CopyFrom(&params, paramsAddr, sizeof(params))) {
        return 0; // The real library will probably just crash here.
    }
    atrac->InitLowLevel(params, static_cast<int>(contextTypes_[static_cast<size_t>(atracID)]));
    return 0;
}

int AtracManager::LowLevelDecode(int atracID, uint32_t sourceAddr, uint32_t sourceBytesConsumedAddr,
                                 uint32_t samplesAddr, uint32_t sampleBytesAddr) {
    lastDelayUs_ = 0;
    AtracContext *atrac = Get(atracID);
    if (!atrac) {
        return Err(kErrAtracBadAtracId);
    }
    const uint32_t frameBytes = static_cast<uint32_t>(atrac->BytesPerFrame());
    const uint8_t *src = mem_.Ptr(sourceAddr, frameBytes);
    if (src == nullptr || !mem_.IsValidRange(sourceBytesConsumedAddr, 4) ||
        !mem_.IsValidRange(samplesAddr, 2) || !mem_.IsValidRange(sampleBytesAddr, 4)) {
        return 0; // Upstream returns zero for invalid pointers.
    }

    int bytesConsumed = 0;
    int bytesWritten = 0;
    std::vector<int16_t> out(static_cast<size_t>(ATRAC3PLUS_MAX_SAMPLES) * 2, 0);
    const int retval = atrac->DecodeLowLevel(src, &bytesConsumed, out.data(), &bytesWritten);
    mem_.Write32(sourceBytesConsumedAddr, static_cast<uint32_t>(bytesConsumed));
    mem_.Write32(sampleBytesAddr, static_cast<uint32_t>(bytesWritten));
    if (bytesWritten > 0) {
        if (uint8_t *dst = mem_.Ptr(samplesAddr, static_cast<uint32_t>(bytesWritten))) {
            std::memcpy(dst, out.data(), static_cast<size_t>(bytesWritten));
        }
    }
    return Delay(retval, kAtracDecodeDelayUs);
}

int AtracManager::SasBindContextAndGetId(uint32_t contextAddr) {
    // Only the new (BSS) contexts exist here: find it by address.
    for (int i = 0; i < PSP_MAX_ATRAC_IDS; i++) {
        AtracContext *atrac = contexts_[static_cast<size_t>(i)].get();
        if (atrac && GuestMemory::Fold(atrac->ContextAddress()) == GuestMemory::Fold(contextAddr)) {
            atrac->CheckForSas();
            return i;
        }
    }
    return -1;
}

void AtracManager::SasDecodeData(int atracId, int16_t *out, int *numSamples, int *finish) {
    AtracContext *atrac = Get(atracId);
    if (!atrac) {
        *numSamples = 0;
        *finish = 1;
        return;
    }
    // If streaming, this writes 0xFFFFFFFF to secondBuffer in the context when
    // close to running out of data; __sceSasConcatenateATRAC3 then queues the
    // next buffer (which cannot be at the same address as the old one).
    atrac->DecodeForSas(out, numSamples, finish);
}

uint32_t AtracManager::SasAddStreamData(int atracId, uint32_t bufPtr, uint32_t bytesToAdd) {
    AtracContext *atrac = Get(atracId);
    if (!atrac) {
        return kErrAtracBadAtracId;
    }
    return static_cast<uint32_t>(atrac->EnqueueForSas(bufPtr, bytesToAdd));
}

} // namespace mpo::audio
