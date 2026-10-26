// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/sceAtrac.cpp @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// The sceAtrac3plus API without its HLE glue. Each method mirrors one PSP
// function: same argument checks, same results written to PSP memory, same
// return code. What stays for the integration (F16b) is the NID table and the
// thread delay: after every call, LastDelayUs() says how long PPSSPP would
// delay the calling thread (hleDelayResult), or 0.
//
// The contexts live in the libatrac3plus BSS (6 x 256 bytes) that
// sceUtilityLoadAvModule(PSP_AV_MODULE_ATRAC3PLUS) reserves; the runtime
// reports it with NotifyLoadModule(). Without it no atrac ID can be created
// (PPSSPP falls back to its legacy context there; MPO always loads the module).

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "atrac_context.h"
#include "guest_memory.h"
#include "sas_audio.h"

namespace mpo::audio {

inline constexpr int PSP_MAX_ATRAC_IDS = 6;

class AtracManager final : public SasAtracBridge {
public:
    explicit AtracManager(const GuestMemory &mem, AtracDecoderFactory factory = {});

    // __AtracNotifyLoadModule / __AtracNotifyUnloadModule. PPSSPP imitates
    // libatrac3plus 0x105 (6 contexts, BSS 0x67C bytes) at the module's load
    // address. Returns false if the BSS is not valid PSP memory.
    bool NotifyLoadModule(int version, uint32_t bssAddr, uint32_t bssSize);
    void NotifyUnloadModule();
    int MaxContexts() const { return maxContexts_; }

    AtracContext *Get(int atracID);
    int LastDelayUs() const { return lastDelayUs_; }

    uint32_t GetAtracID(int codecType);
    uint32_t AddStreamData(int atracID, uint32_t bytesToAdd);
    uint32_t DecodeData(int atracID, uint32_t outAddr, uint32_t numSamplesAddr,
                        uint32_t finishFlagAddr, uint32_t remainAddr);
    uint32_t ReleaseResources();
    uint32_t GetBufferInfoForResetting(int atracID, int sample, uint32_t bufferInfoAddr);
    uint32_t GetBitrate(int atracID, uint32_t outBitrateAddr);
    uint32_t GetChannel(int atracID, uint32_t channelAddr);
    uint32_t GetLoopStatus(int atracID, uint32_t loopNumAddr, uint32_t statusAddr);
    uint32_t GetInternalErrorInfo(int atracID, uint32_t errorAddr);
    uint32_t GetMaxSample(int atracID, uint32_t maxSamplesAddr);
    uint32_t GetNextDecodePosition(int atracID, uint32_t outposAddr);
    uint32_t GetNextSample(int atracID, uint32_t outNAddr);
    uint32_t GetRemainFrame(int atracID, uint32_t remainAddr);
    uint32_t GetSecondBufferInfo(int atracID, uint32_t fileOffsetAddr, uint32_t desiredSizeAddr);
    uint32_t GetSoundSample(int atracID, uint32_t outEndSampleAddr, uint32_t outLoopStartSampleAddr,
                            uint32_t outLoopEndSampleAddr);
    uint32_t GetStreamDataInfo(int atracID, uint32_t writePtrAddr, uint32_t writableBytesAddr,
                               uint32_t readOffsetAddr);
    uint32_t ReleaseAtracID(int atracID);
    uint32_t ResetPlayPosition(int atracID, int sample, int bytesWrittenFirstBuf,
                               int bytesWrittenSecondBuf);
    uint32_t SetHalfwayBuffer(int atracID, uint32_t buffer, uint32_t readSize, uint32_t bufferSize);
    uint32_t SetSecondBuffer(int atracID, uint32_t secondBuffer, uint32_t secondBufferSize);
    uint32_t SetData(int atracID, uint32_t buffer, uint32_t bufferSize);
    int SetDataAndGetID(uint32_t buffer, int bufferSize);
    int SetHalfwayBufferAndGetID(uint32_t buffer, uint32_t readSize, uint32_t bufferSize);
    uint32_t StartEntry();
    uint32_t SetLoopNum(int atracID, int loopNum);
    int Reinit(int at3Count, int at3plusCount);
    int GetOutputChannel(int atracID, uint32_t outputChanPtr);
    int IsSecondBufferNeeded(int atracID);
    int SetMOutHalfwayBuffer(int atracID, uint32_t buffer, uint32_t readSize, uint32_t bufferSize);
    uint32_t SetMOutData(int atracID, uint32_t buffer, uint32_t bufferSize);
    int SetMOutDataAndGetID(uint32_t buffer, uint32_t bufferSize);
    int SetMOutHalfwayBufferAndGetID(uint32_t buffer, uint32_t readSize, uint32_t bufferSize);
    int SetAA3DataAndGetID(uint32_t buffer, uint32_t bufferSize, uint32_t fileSize,
                           uint32_t metadataSizeAddr);
    int SetAA3HalfwayBufferAndGetID(uint32_t buffer, uint32_t readSize, uint32_t bufferSize,
                                    uint32_t fileSize);
    uint32_t GetContextAddress(int atracID);
    int LowLevelInitDecoder(int atracID, uint32_t paramsAddr);
    int LowLevelDecode(int atracID, uint32_t sourceAddr, uint32_t sourceBytesConsumedAddr,
                       uint32_t samplesAddr, uint32_t sampleBytesAddr);

    // SasAtracBridge (the sceSas side of an ATRAC3 voice).
    int SasBindContextAndGetId(uint32_t contextAddr) override;
    void SasDecodeData(int atracId, int16_t *out, int *numSamples, int *finish) override;
    uint32_t SasAddStreamData(int atracId, uint32_t bufPtr, uint32_t bytesToAdd) override;

private:
    int AllocAndRegister(uint32_t codecType);
    int UnregisterAndDelete(int atracID);
    uint32_t ValidateData(const AtracContext *atrac) const;
    uint32_t ValidateManaged(const AtracContext *atrac) const;
    uint32_t ContextAddress(int atracID) const {
        return bss_ + static_cast<uint32_t>(atracID) * 256;
    }
    int Delay(int result, int us) {
        lastDelayUs_ = us;
        return result;
    }

    GuestMemory mem_;
    AtracDecoderFactory factory_;
    std::array<std::unique_ptr<AtracContext>, PSP_MAX_ATRAC_IDS> contexts_;
    std::array<uint32_t, PSP_MAX_ATRAC_IDS> contextTypes_{};
    bool inited_ = true;
    int libVersion_ = 0;
    int maxContexts_ = 6;
    uint32_t bss_ = 0;
    int lastDelayUs_ = 0;
};

} // namespace mpo::audio
