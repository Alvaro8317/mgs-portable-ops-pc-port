// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/scePsmf.cpp (Psmf, PsmfStream) and the PSMF
// constants of Core/HLE/sceMpeg.h and Core/HW/MediaEngine.h @ v1.20.4.
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// PSMF header (the first 0x800 bytes of a .pmf): stream list, timestamps and
// the EP (entry point) map. Every offset is checked against the buffer; the
// upstream code trusts the guest's pointers instead.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mpo::video {

inline constexpr uint32_t PSMF_MAGIC = 0x464D5350; // "PSMF", read little-endian
inline constexpr int PSMF_STREAM_VERSION_OFFSET = 0x4;
inline constexpr int PSMF_STREAM_OFFSET_OFFSET = 0x8;
inline constexpr int PSMF_STREAM_SIZE_OFFSET = 0xC;
inline constexpr int PSMF_FIRST_TIMESTAMP_OFFSET = 0x54;
inline constexpr int PSMF_LAST_TIMESTAMP_OFFSET = 0x5A;
inline constexpr int PSMF_VIDEO_STREAM_ID = 0xE0;
inline constexpr int PSMF_AUDIO_STREAM_ID = 0xBD;
inline constexpr int PSMF_STREAM_TABLE_OFFSET = 0x80; // u16 BE count, then 16-byte entries
inline constexpr int PSMF_STREAM_ENTRY_SIZE = 16;
inline constexpr int PSMF_EP_MAP_STRIDE = 10;
// sceMpeg/scePsmf use a 90 kHz clock; one video frame at 29.97 fps.
inline constexpr int64_t MPEG_TIMESTAMP_PER_SECOND = 90000;
inline constexpr int VIDEO_FRAME_DURATION_TS = 3003;
// One ATRAC3plus frame (2048 samples at 44.1 kHz) in 90 kHz units.
inline constexpr int AUDIO_FRAME_DURATION_TS = 4180;

// scePsmf stream types.
enum PsmfStreamType : int {
    PSMF_AVC_STREAM = 0,
    PSMF_ATRAC_STREAM = 1,
    PSMF_PCM_STREAM = 2,
    PSMF_DATA_STREAM = 3,
    PSMF_AUDIO_STREAM = 15, // matches ATRAC or PCM in scePsmfSetStreamWithType
};

struct PsmfEntry {
    int EPIndex = 0;
    int EPPicOffset = 0;
    int EPPts = 0;
    int EPOffset = 0;
};

struct PsmfStream {
    int type = PSMF_AVC_STREAM;
    int channel = 0;         // streamId & 0x0F (video) or privateStreamId & 0x0F (audio)
    int streamId = 0;        // 0xE0..0xEF or 0xBD
    int privateStreamId = 0; // second byte of the entry
    // Video: in pixels (the header stores units of 16). -1 if not video.
    int videoWidth = -1;
    int videoHeight = -1;
    // Audio: raw header bytes 14 and 15 ("frequency" is usually 2). -1 if not audio.
    int audioChannels = -1;
    int audioFrequency = -1;

    bool MatchesType(int ty) const {
        if (ty == PSMF_AUDIO_STREAM) {
            return type == PSMF_ATRAC_STREAM || type == PSMF_PCM_STREAM;
        }
        return type == ty;
    }
};

struct PsmfHeader {
    uint32_t magic = 0;
    uint32_t version = 0; // four ASCII digits read little-endian, e.g. "0014"
    uint32_t streamOffset = 0;
    uint32_t streamSize = 0;
    uint32_t streamDataTotalSize = 0;
    uint32_t presentationStartTime = 0; // 90 kHz
    uint32_t presentationEndTime = 0;
    uint32_t streamDataNextBlockSize = 0;
    uint32_t streamDataNextInnerBlockSize = 0;
    int numStreams = 0; // as declared in the header
    // Stream entries that were recognised (video or private audio), in header
    // order, like PPSSPP's streamMap.
    std::vector<PsmfStream> streams;
    // From the last video stream entry, like upstream (each one overwrites it).
    uint32_t EPMapOffset = 0;
    uint32_t EPMapEntriesNum = 0;
    std::vector<PsmfEntry> EPMap;

    std::string VersionString() const;
    // Number of streams of a scePsmf type.
    int CountStreams(int type) const;
    // Index of the n-th stream of a type in `streams`, or -1.
    int FindStreamWithTypeNumber(int type, int n) const;
    // Index of the stream with a type and channel, or -1.
    int FindStreamWithTypeChannel(int type, int channel) const;
    // Index of the EP map entry for a timestamp (PPSSPP's FindEPWithTimestamp), or -1.
    int FindEPWithTimestamp(int pts) const;
};

enum class PsmfError {
    None,
    TooSmall,      // buffer shorter than the fixed header
    BadMagic,
    BadStreamCount // the stream table does not fit in the buffer
};

const char *PsmfErrorName(PsmfError error);

// Big-endian helpers shared by the PSMF and MPEG code.
uint32_t ReadU32BE(const uint8_t *p);
uint16_t ReadU16BE(const uint8_t *p);
// 6-byte timestamp of the PSMF header (MediaEngine.h getMpegTimeStamp).
int64_t GetMpegTimeStamp(const uint8_t *buf);

// Parses the header. `data` is the start of the file; it should hold at
// least the 0x800-byte header. A stream table that runs past the buffer is
// an error; an EP map that runs past it is truncated to the entries that fit
// (upstream clamps it to valid memory the same way).
PsmfError ParsePsmfHeader(std::span<const uint8_t> data, PsmfHeader *out);

} // namespace mpo::video
