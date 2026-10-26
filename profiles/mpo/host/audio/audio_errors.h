// SPDX-License-Identifier: GPL-3.0-or-later
//
// PSP error codes returned by sceSasCore, sceAtrac3plus and sceAudio.
// Values from PPSSPP Core/HLE/ErrorCodes.h @ v1.20.4.

#pragma once

#include <cstdint>

namespace mpo::audio {

// Kernel.
inline constexpr uint32_t kErrKernelBadArgument = 0x80000004;
inline constexpr uint32_t kErrKernelBusy = 0x80000021;
inline constexpr uint32_t kErrKernelOutOfMemory = 0x80000022;
inline constexpr uint32_t kErrKernelInvalidPointer = 0x80000103;
inline constexpr uint32_t kErrKernelInvalidSize = 0x80000104;
inline constexpr uint32_t kErrKernelIllegalAddr = 0x800200d3;
inline constexpr uint32_t kErrKernelCanNotWait = 0x800201a7;

// sceAudio.
inline constexpr uint32_t kErrAudioChannelNotInit = 0x80260001;
inline constexpr uint32_t kErrAudioChannelBusy = 0x80260002;
inline constexpr uint32_t kErrAudioInvalidChannel = 0x80260003;
inline constexpr uint32_t kErrAudioNoChannelsAvailable = 0x80260005;
inline constexpr uint32_t kErrAudioOutputSampleDataSizeNotAligned = 0x80260006;
inline constexpr uint32_t kErrAudioInvalidFormat = 0x80260007;
inline constexpr uint32_t kErrAudioChannelNotReserved = 0x80260008;
inline constexpr uint32_t kErrAudioInvalidFrequency = 0x8026000A;
inline constexpr uint32_t kErrAudioInvalidVolume = 0x8026000B;
inline constexpr uint32_t kErrAudioChannelAlreadyReserved = 0x80268002;

// sceAtrac3plus.
inline constexpr uint32_t kErrAtracApiFail = 0x80630002;
inline constexpr uint32_t kErrAtracNoAtracId = 0x80630003;
inline constexpr uint32_t kErrAtracInvalidCodecType = 0x80630004;
inline constexpr uint32_t kErrAtracBadAtracId = 0x80630005;
inline constexpr uint32_t kErrAtracUnknownFormat = 0x80630006;
inline constexpr uint32_t kErrAtracWrongCodecType = 0x80630007;
inline constexpr uint32_t kErrAtracBadCodecParams = 0x80630008;
inline constexpr uint32_t kErrAtracAllDataLoaded = 0x80630009;
inline constexpr uint32_t kErrAtracNoData = 0x80630010;
inline constexpr uint32_t kErrAtracSizeTooSmall = 0x80630011;
inline constexpr uint32_t kErrAtracSecondBufferNeeded = 0x80630012;
inline constexpr uint32_t kErrAtracIncorrectReadSize = 0x80630013;
inline constexpr uint32_t kErrAtracBadAlignment = 0x80630014;
inline constexpr uint32_t kErrAtracBadSample = 0x80630015;
inline constexpr uint32_t kErrAtracBadFirstResetSize = 0x80630016;
inline constexpr uint32_t kErrAtracBadSecondResetSize = 0x80630017;
inline constexpr uint32_t kErrAtracAddDataIsTooBig = 0x80630018;
inline constexpr uint32_t kErrAtracNotMono = 0x80630019;
inline constexpr uint32_t kErrAtracNoLoopInformation = 0x80630021;
inline constexpr uint32_t kErrAtracSecondBufferNotNeeded = 0x80630022;
inline constexpr uint32_t kErrAtracBufferIsEmpty = 0x80630023;
inline constexpr uint32_t kErrAtracAllDataDecoded = 0x80630024;
inline constexpr uint32_t kErrAtracIsLowLevel = 0x80630031;
inline constexpr uint32_t kErrAtracIsForSceSas = 0x80630040;
inline constexpr uint32_t kErrAtracAa3OtherFailure = 0x80631002;
inline constexpr uint32_t kErrAtracAa3InvalidData = 0x80631003;
inline constexpr uint32_t kErrAtracAa3SizeTooSmall = 0x80631004;
inline constexpr uint32_t kErrAtracAa3BadCodecParams = 0x80631005;

// sceSasCore.
inline constexpr uint32_t kErrSasInvalidGrain = 0x80420001;
inline constexpr uint32_t kErrSasInvalidMaxVoices = 0x80420002;
inline constexpr uint32_t kErrSasInvalidOutputMode = 0x80420003;
inline constexpr uint32_t kErrSasInvalidSampleRate = 0x80420004;
inline constexpr uint32_t kErrSasBadAddress = 0x80420005;
inline constexpr uint32_t kErrSasInvalidVoice = 0x80420010;
inline constexpr uint32_t kErrSasInvalidNoiseFreq = 0x80420011;
inline constexpr uint32_t kErrSasInvalidPitch = 0x80420012;
inline constexpr uint32_t kErrSasInvalidAdsrCurveMode = 0x80420013;
inline constexpr uint32_t kErrSasInvalidParameter = 0x80420014;
inline constexpr uint32_t kErrSasInvalidLoopPos = 0x80420015;
inline constexpr uint32_t kErrSasVoicePaused = 0x80420016;
inline constexpr uint32_t kErrSasInvalidVolume = 0x80420018;
inline constexpr uint32_t kErrSasInvalidAdsrRate = 0x80420019;
inline constexpr uint32_t kErrSasInvalidPcmSize = 0x8042001A;
inline constexpr uint32_t kErrSasRevInvalidType = 0x80420020;
inline constexpr uint32_t kErrSasRevInvalidFeedback = 0x80420021;
inline constexpr uint32_t kErrSasRevInvalidDelay = 0x80420022;
inline constexpr uint32_t kErrSasRevInvalidVolume = 0x80420023;
inline constexpr uint32_t kErrSasAtrac3AlreadySet = 0x80420040;
inline constexpr uint32_t kErrSasAtrac3NotSet = 0x80420041;
inline constexpr uint32_t kErrSasAtrac3AlreadyQueued = 0x80420042;

} // namespace mpo::audio
