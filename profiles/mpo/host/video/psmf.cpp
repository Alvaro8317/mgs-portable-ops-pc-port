// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HLE/scePsmf.cpp @ v1.20.4 (Psmf::Psmf,
// PsmfStream::readMPEGVideoStreamParams / readPrivateAudioStreamParams,
// Psmf::FindEPWithTimestamp) and getMpegTimeStamp (Core/HW/MediaEngine.h).
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).

#include "psmf.h"

namespace mpo::video {

uint32_t ReadU32BE(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t ReadU16BE(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

int64_t GetMpegTimeStamp(const uint8_t *buf) {
    return static_cast<int64_t>(buf[5]) | (static_cast<int64_t>(buf[4]) << 8) |
           (static_cast<int64_t>(buf[3]) << 16) | (static_cast<int64_t>(buf[2]) << 24) |
           (static_cast<int64_t>(buf[1]) << 32) | (static_cast<int64_t>(buf[0]) << 36);
}

const char *PsmfErrorName(PsmfError error) {
    switch (error) {
    case PsmfError::None:
        return "ok";
    case PsmfError::TooSmall:
        return "buffer too small for a PSMF header";
    case PsmfError::BadMagic:
        return "bad PSMF magic";
    case PsmfError::BadStreamCount:
        return "stream table runs past the buffer";
    }
    return "unknown";
}

std::string PsmfHeader::VersionString() const {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((version >> (8 * i)) & 0xFF);
        s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return s;
}

int PsmfHeader::CountStreams(int type) const {
    int n = 0;
    for (const PsmfStream &s : streams) {
        if (s.MatchesType(type)) {
            ++n;
        }
    }
    return n;
}

int PsmfHeader::FindStreamWithTypeNumber(int type, int n) const {
    for (size_t i = 0; i < streams.size(); ++i) {
        if (streams[i].MatchesType(type)) {
            if (n == 0) {
                return static_cast<int>(i);
            }
            --n;
        }
    }
    return -1;
}

int PsmfHeader::FindStreamWithTypeChannel(int type, int channel) const {
    for (size_t i = 0; i < streams.size(); ++i) {
        if (streams[i].MatchesType(type) && streams[i].channel == channel) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int PsmfHeader::FindEPWithTimestamp(int pts) const {
    int best = -1;
    int bestPts = 0;
    for (int i = 0; i < static_cast<int>(EPMap.size()); ++i) {
        const int matchPts = EPMap[static_cast<size_t>(i)].EPPts;
        if (matchPts == pts) {
            // Exact match, take it.
            return i;
        }
        // TODO (upstream): Does it actually do fuzzy matching?
        if (matchPts < pts && matchPts >= bestPts) {
            best = i;
            bestPts = matchPts;
        }
    }
    return best;
}

PsmfError ParsePsmfHeader(std::span<const uint8_t> data, PsmfHeader *out) {
    *out = PsmfHeader{};
    // Fixed part up to the stream count at 0x80.
    if (data.size() < PSMF_STREAM_TABLE_OFFSET + 2) {
        return PsmfError::TooSmall;
    }
    const uint8_t *ptr = data.data();
    PsmfHeader &h = *out;
    h.magic = static_cast<uint32_t>(ptr[0]) | (static_cast<uint32_t>(ptr[1]) << 8) |
              (static_cast<uint32_t>(ptr[2]) << 16) | (static_cast<uint32_t>(ptr[3]) << 24);
    if (h.magic != PSMF_MAGIC) {
        return PsmfError::BadMagic;
    }
    h.version = static_cast<uint32_t>(ptr[4]) | (static_cast<uint32_t>(ptr[5]) << 8) |
                (static_cast<uint32_t>(ptr[6]) << 16) | (static_cast<uint32_t>(ptr[7]) << 24);
    h.streamOffset = ReadU32BE(&ptr[PSMF_STREAM_OFFSET_OFFSET]);
    h.streamSize = ReadU32BE(&ptr[PSMF_STREAM_SIZE_OFFSET]);
    h.streamDataTotalSize = ReadU32BE(&ptr[0x50]);
    h.presentationStartTime =
        static_cast<uint32_t>(GetMpegTimeStamp(ptr + PSMF_FIRST_TIMESTAMP_OFFSET));
    h.presentationEndTime =
        static_cast<uint32_t>(GetMpegTimeStamp(ptr + PSMF_LAST_TIMESTAMP_OFFSET));
    h.streamDataNextBlockSize = ReadU32BE(&ptr[0x6A]);
    h.streamDataNextInnerBlockSize = ReadU32BE(&ptr[0x7C]);
    h.numStreams = ReadU16BE(&ptr[PSMF_STREAM_TABLE_OFFSET]);

    const size_t tableStart = PSMF_STREAM_TABLE_OFFSET + 2;
    if (tableStart + static_cast<size_t>(h.numStreams) * PSMF_STREAM_ENTRY_SIZE > data.size()) {
        return PsmfError::BadStreamCount;
    }

    for (int i = 0; i < h.numStreams; ++i) {
        const uint8_t *addr = ptr + tableStart + static_cast<size_t>(i) * PSMF_STREAM_ENTRY_SIZE;
        const int streamId = addr[0];
        const int privateStreamId = addr[1];
        PsmfStream stream;
        stream.streamId = streamId;
        stream.privateStreamId = privateStreamId;
        if ((streamId & PSMF_VIDEO_STREAM_ID) == PSMF_VIDEO_STREAM_ID) {
            stream.type = PSMF_AVC_STREAM;
            stream.channel = streamId & 0x0F;
            // readMPEGVideoStreamParams: two unknown bytes, then the EP map.
            h.EPMapOffset = ReadU32BE(&addr[4]);
            h.EPMapEntriesNum = ReadU32BE(&addr[8]);
            stream.videoWidth = addr[12] * 16;
            stream.videoHeight = addr[13] * 16;

            h.EPMap.clear();
            size_t fit = 0;
            if (h.EPMapOffset < data.size()) {
                fit = (data.size() - h.EPMapOffset) / PSMF_EP_MAP_STRIDE;
            }
            if (h.EPMapEntriesNum > fit) {
                h.EPMapEntriesNum = static_cast<uint32_t>(fit);
            }
            h.EPMap.reserve(h.EPMapEntriesNum);
            for (uint32_t e = 0; e < h.EPMapEntriesNum; ++e) {
                const uint8_t *entryAddr = ptr + h.EPMapOffset + PSMF_EP_MAP_STRIDE * e;
                PsmfEntry entry;
                entry.EPIndex = entryAddr[0];
                entry.EPPicOffset = entryAddr[1];
                entry.EPPts = static_cast<int>(ReadU32BE(&entryAddr[2]));
                entry.EPOffset = static_cast<int>(ReadU32BE(&entryAddr[6]));
                h.EPMap.push_back(entry);
            }
        } else if ((streamId & PSMF_AUDIO_STREAM_ID) == PSMF_AUDIO_STREAM_ID) {
            // Upstream assumes PCM for any private stream id with high bits set.
            stream.type = (privateStreamId & 0xF0) != 0 ? PSMF_PCM_STREAM : PSMF_ATRAC_STREAM;
            stream.channel = privateStreamId & 0x0F;
            stream.audioChannels = addr[14];
            stream.audioFrequency = addr[15];
        } else {
            continue;
        }
        h.streams.push_back(stream);
    }
    return PsmfError::None;
}

} // namespace mpo::video
