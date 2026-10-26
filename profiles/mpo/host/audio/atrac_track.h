// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/Util/AtracTrack.h @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Parsing of RIFF/WAVE (.at3) and OMA/AA3 headers for sceAtrac.
//
// Changes from upstream: every read goes through a bounded view (`avail` bytes
// that really exist behind the pointer); bytes past it read as zero instead of
// running off the end of the buffer. With in-range data the results are the
// same as upstream.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mpo::audio {

enum PspAudioCodec : uint32_t {
    PSP_CODEC_AT3PLUS = 0x00001000,
    PSP_CODEC_AT3 = 0x00001001,
};

inline constexpr int ATRAC3_MAX_SAMPLES = 0x400;     // 1024
inline constexpr int ATRAC3PLUS_MAX_SAMPLES = 0x800; // 2048

struct AtracLoopInfo {
    int cuePointID;
    int type;
    int startSample;
    int endSample;
    int fraction;
    int playCount;
};

// This is (mostly) constant info, once a track has been loaded.
struct Track {
    uint32_t codecType = 0;
    // Size of the full track being streamed or played.
    uint32_t fileSize = 0;
    uint32_t bitrate = 64;
    int jointStereo = 0;
    uint16_t channels = 2;
    // The size of an encoded frame in bytes.
    uint16_t bytesPerFrame = 0;
    // Byte offset of the first encoded frame in the input buffer.
    int dataByteOffset = 0;
    // How many samples to skip from the beginning of a track when decoding
    // (FirstOffsetExtra() is added on top).
    int firstSampleOffset = 0;
    // Last sample number. Inclusive.
    int endSample = -1;

    std::vector<AtracLoopInfo> loopinfo;
    int loopStartSample = -1;
    int loopEndSample = -1;

    int BytesPerFrame() const { return bytesPerFrame; }
    int FirstOffsetExtra() const { return codecType == PSP_CODEC_AT3PLUS ? 0x170 : 0x45; }
    int FirstSampleOffsetFull() const { return FirstOffsetExtra() + firstSampleOffset; }
    int SamplesPerFrame() const {
        return codecType == PSP_CODEC_AT3PLUS ? ATRAC3PLUS_MAX_SAMPLES : ATRAC3_MAX_SAMPLES;
    }
    int Bitrate() const {
        int rate = (bytesPerFrame * 352800) / 1000;
        if (codecType == PSP_CODEC_AT3PLUS) {
            rate = ((rate >> 11) + 8) & ~0xF; // upstream: & 0xFFFFFFF0
        } else {
            rate = (rate + 511) >> 10;
        }
        return rate;
    }
};

// The newer parser output that the Atrac2 context uses.
struct TrackInfo {
    uint16_t numChans;
    uint16_t blockAlign;
    uint8_t sampleSizeMaybe;
    uint8_t tailFlag;
    uint8_t unused[2];
    uint32_t dataOff;
    uint32_t endSample;
    uint32_t waveDataSize;
    uint32_t firstSampleOffset;
    uint32_t loopStart;
    uint32_t loopEnd;
};

// Old WAVE parser, used by sceAtracSetData* to find the codec type.
// `buffer` may be null (it then reads zeros). `avail` is how many bytes really
// exist behind `buffer`; `size` is what the game claims.
int AnalyzeAtracTrack(const uint8_t *buffer, uint32_t avail, uint32_t size, Track *track,
                      std::string *error);
int AnalyzeAA3Track(const uint8_t *buffer, uint32_t avail, uint32_t size, uint32_t filesize,
                    Track *track, std::string *error);

// Returns the codec (PSP_CODEC_*) or a negative PSP error code.
int ParseWaveAT3(const uint8_t *data, uint32_t avail, uint32_t length, TrackInfo *track);
int ParseAA3(const uint8_t *data, uint32_t avail, uint32_t readSize, uint32_t fileSize,
             TrackInfo *track);

} // namespace mpo::audio
