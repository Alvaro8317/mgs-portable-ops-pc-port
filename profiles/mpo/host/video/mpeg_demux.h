// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/MpegDemux.h and Core/HW/MpegDemux.cpp @ v1.20.4
// ("Thanks to JPCSP project").
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// MPEG-2 program stream demuxer for the PSMF payload. Upstream only extracts
// the ATRAC3plus private stream and lets libavformat read the video; here the
// same packet loop also extracts one H.264 video stream (0xE0..0xEF), so the
// build needs no libavformat.
//
// Other changes from upstream:
//   - every read is bounds-checked (a corrupt packet cannot read past the
//     data that was added);
//   - a header offset larger than the first chunk is kept across calls
//     instead of being lost;
//   - demux() can skip audio and stop after one video packet, so a second
//     instance can stand in for libavformat on the video side;
//   - no savestate code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "buffer_queue.h"

namespace mpo::video {

inline constexpr int PACKET_START_CODE_MASK = static_cast<int>(0xffffff00);
inline constexpr int PACKET_START_CODE_PREFIX = 0x00000100;
inline constexpr int USER_DATA_START_CODE = 0x000001b2;
inline constexpr int PACK_START_CODE = 0x000001ba;
inline constexpr int SYSTEM_HEADER_START_CODE = 0x000001bb;
inline constexpr int PRIVATE_STREAM_1 = 0x000001bd;
inline constexpr int PADDING_STREAM = 0x000001be;
inline constexpr int PRIVATE_STREAM_2 = 0x000001bf;

// One PES payload of the video stream.
struct VideoChunk {
    std::vector<uint8_t> data;
    int64_t pts = -1; // 90 kHz, -1 when the PES header had none
    int64_t dts = -1;
};

struct MpegDemuxStats {
    uint64_t packs = 0;
    uint64_t audioPackets = 0;
    uint64_t videoPackets = 0;
    uint64_t otherPackets = 0;   // padding, private stream 2, system headers, other video ids
    uint64_t badPackHeaders = 0; // pack headers that skipPackHeader() rejected
    uint64_t droppedAudioBytes = 0; // audio queue full
    uint64_t droppedVideoBytes = 0; // video queue over its limit
};

class MpegDemux {
public:
    // `size`: capacity of the input buffer and of the audio queue (upstream
    // uses the ring buffer size + 2048). `offset`: bytes to skip at the start
    // of the data (the PSMF header).
    MpegDemux(int size, int offset);

    // Appends raw program-stream bytes. Returns false (and adds nothing) if
    // they do not fit; demux() frees the space of the packets it consumed.
    bool addStreamData(const uint8_t *buf, int addSize);

    // Audio channel value that makes demux() skip the audio packets.
    static constexpr int kNoAudio = -2;

    // Consumes the complete packets in the buffer. Audio of `audioChannel`
    // (-1: the first channel seen, then that one; kNoAudio: none) goes to the
    // audio queue; video PES of `videoStreamId` (0xE0..0xEF, -1: none) to the
    // video queue. With `stopAfterVideo`, it stops right after the first
    // video PES so the rest stays in the input buffer (the video side reads
    // the stream on demand, like libavformat does upstream).
    // Returns whether the data looked like an MPEG program stream.
    bool demux(int audioChannel, int videoStreamId = -1, bool stopAfterVideo = false);

    // Pops the next ATRAC3plus frame. Returns the payload size (frame size
    // minus the 8-byte frame header) or 0; *buf points into an internal
    // buffer that stays valid until the next call.
    int getNextAudioFrame(uint8_t **buf, int *headerCode1, int *headerCode2, int64_t *pts = nullptr);
    bool hasNextAudioFrame(int *gotsizeOut, int *frameSizeOut, int *headerCode1, int *headerCode2);

    bool popVideoChunk(VideoChunk *out);
    size_t videoQueuedBytes() const { return videoQueuedBytes_; }
    size_t videoQueuedChunks() const { return videoQueue_.size(); }
    int audioQueuedBytes() const { return m_audioStream.getQueueSize(); }

    // Free space in the input buffer.
    int getRemainSize() const { return m_len - m_readSize; }
    // Bytes added but not demuxed yet.
    int getPendingSize() const { return m_readSize > m_index ? m_readSize - m_index : 0; }
    int audioChannel() const { return m_audioChannel; }
    const MpegDemuxStats &stats() const { return stats_; }

    // Upper bound for the video queue; data past it is dropped and counted.
    static constexpr size_t kMaxVideoQueueBytes = 16u << 20;

private:
    struct PesHeader {
        int64_t pts = 0;
        int64_t dts = 0;
        bool hasPts = false;
        int channel;
        explicit PesHeader(int chan) : channel(chan) {}
    };

    int read8() {
        // Past the data that was added, reads return 0 (upstream reads stale
        // or out-of-bounds bytes).
        const int v = m_index < m_readSize ? m_buf[static_cast<size_t>(m_index)] : 0;
        m_index++;
        return v;
    }
    int read16() {
        const int hi = read8();
        return (hi << 8) | read8();
    }
    int read24() {
        const int b0 = read8();
        const int b1 = read8();
        return (b0 << 16) | (b1 << 8) | read8();
    }
    int64_t readPts() { return readPts(read8()); }
    int64_t readPts(int c) {
        const int64_t mid = read16() >> 1;
        const int64_t low = read16() >> 1;
        return (static_cast<int64_t>(c & 0x0E) << 29) | (mid << 15) | low;
    }
    bool isEOF() const { return m_index >= m_readSize; }
    void skip(int n) {
        if (n > 0) {
            m_index += n;
        }
    }
    int readPesHeader(PesHeader &pesHeader, int length, int startCode);
    int demuxStream(bool bdemux, int startCode, int length, int channel);
    void demuxVideo(int startCode, int length);
    bool skipPackHeader();

    int m_index;
    int m_len;
    std::vector<uint8_t> m_buf;
    BufferQueue m_audioStream;
    uint8_t m_audioFrame[0x2000];
    int m_audioChannel;
    int m_readSize;

    std::deque<VideoChunk> videoQueue_;
    size_t videoQueuedBytes_ = 0;
    MpegDemuxStats stats_;
};

} // namespace mpo::video
