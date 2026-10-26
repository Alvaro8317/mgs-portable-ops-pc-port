// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/MpegDemux.cpp @ v1.20.4 ("Thanks to JPCSP
// project"). Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
// See mpeg_demux.h for the changes from upstream.

#include "mpeg_demux.h"

#include <algorithm>
#include <cstring>

namespace mpo::video {

// http://dvd.sourceforge.net/dvdinfo/mpeghdrs.html

MpegDemux::MpegDemux(int size, int offset) : m_audioStream(size) {
    m_len = size > 0 ? size : 1;
    m_buf.assign(static_cast<size_t>(m_len), 0);
    m_index = offset > 0 ? offset : 0;
    m_audioChannel = -1;
    m_readSize = 0;
    std::memset(m_audioFrame, 0, sizeof(m_audioFrame));
}

bool MpegDemux::addStreamData(const uint8_t *buf, int addSize) {
    if (addSize < 0 || m_readSize + addSize > m_len) {
        return false;
    }
    if (addSize > 0) {
        std::memcpy(m_buf.data() + m_readSize, buf, static_cast<size_t>(addSize));
    }
    m_readSize += addSize;
    return true;
}

int MpegDemux::readPesHeader(PesHeader &pesHeader, int length, int startCode) {
    int c = 0;
    while (length > 0) {
        c = read8();
        length--;
        if (c != 0xFF) {
            break;
        }
    }
    if ((c & 0xC0) == 0x40) {
        read8();
        c = read8();
        length -= 2;
    }
    pesHeader.pts = 0;
    pesHeader.dts = 0;
    if ((c & 0xE0) == 0x20) {
        pesHeader.dts = pesHeader.pts = readPts(c);
        pesHeader.hasPts = true;
        length -= 4;
        if ((c & 0x10) != 0) {
            pesHeader.dts = readPts();
            length -= 5;
        }
    } else if ((c & 0xC0) == 0x80) {
        int flags = read8();
        int headerLength = read8();
        length -= 2;
        length -= headerLength;
        if ((flags & 0x80) != 0) {
            pesHeader.dts = pesHeader.pts = readPts();
            pesHeader.hasPts = true;
            headerLength -= 5;
            if ((flags & 0x40) != 0) {
                pesHeader.dts = readPts();
                headerLength -= 5;
            }
        }
        if ((flags & 0x3F) != 0 && headerLength == 0) {
            flags &= 0xC0;
        }
        if ((flags & 0x01) != 0) {
            int pesExt = read8();
            headerLength--;
            int skipLen = (pesExt >> 4) & 0x0B;
            skipLen += skipLen & 0x09;
            if ((pesExt & 0x40) != 0 || skipLen > headerLength) {
                pesExt = skipLen = 0;
            }
            this->skip(skipLen);
            headerLength -= skipLen;
            if ((pesExt & 0x01) != 0) {
                const int ext2Length = read8();
                headerLength--;
                if ((ext2Length & 0x7F) != 0) {
                    const int idExt = read8();
                    headerLength--;
                    if ((idExt & 0x80) == 0) {
                        startCode = ((startCode & 0xFF) << 8) | idExt;
                    }
                }
            }
        }
        skip(headerLength);
    }
    if (startCode == PRIVATE_STREAM_1) {
        const int channel = read8();
        pesHeader.channel = channel;
        length--;
        if (channel >= 0x80 && channel <= 0xCF) {
            // Skip audio header
            skip(3);
            length -= 3;
            if (channel >= 0xB0 && channel <= 0xBF) {
                skip(1);
                length--;
            }
        } else {
            // PSP audio has additional 3 bytes in header
            skip(3);
            length -= 3;
        }
    }
    return length;
}

int MpegDemux::demuxStream(bool bdemux, int startCode, int length, int channel) {
    if (bdemux) {
        PesHeader pesHeader(channel);
        length = readPesHeader(pesHeader, length, startCode);
        // Never hand out bytes past the data that was added.
        length = std::min(length, m_readSize - m_index);
        if (pesHeader.channel == channel || channel < 0) {
            channel = pesHeader.channel;
            if (length > 0) {
                if (!m_audioStream.push(m_buf.data() + m_index, length, pesHeader.pts)) {
                    stats_.droppedAudioBytes += static_cast<uint64_t>(length);
                }
            }
        }
        skip(length);
    } else {
        skip(length);
    }
    return channel;
}

void MpegDemux::demuxVideo(int startCode, int length) {
    PesHeader pesHeader(-1);
    length = readPesHeader(pesHeader, length, startCode);
    length = std::min(length, m_readSize - m_index);
    if (length > 0) {
        if (videoQueuedBytes_ + static_cast<size_t>(length) > kMaxVideoQueueBytes) {
            stats_.droppedVideoBytes += static_cast<uint64_t>(length);
        } else {
            VideoChunk chunk;
            const uint8_t *begin = m_buf.data() + m_index;
            chunk.data.assign(begin, begin + length);
            chunk.pts = pesHeader.hasPts ? pesHeader.pts : -1;
            chunk.dts = pesHeader.hasPts ? pesHeader.dts : -1;
            videoQueuedBytes_ += static_cast<size_t>(length);
            videoQueue_.push_back(std::move(chunk));
        }
    }
    skip(length);
}

bool MpegDemux::popVideoChunk(VideoChunk *out) {
    if (videoQueue_.empty()) {
        return false;
    }
    *out = std::move(videoQueue_.front());
    videoQueue_.pop_front();
    videoQueuedBytes_ -= out->data.size();
    return true;
}

bool MpegDemux::skipPackHeader() {
    // MPEG version / SCR
    if ((read8() & 0xC4) != 0x44) {
        return false;
    }
    skip(1);
    if ((read8() & 0x04) != 0x04) {
        return false;
    }
    skip(1);
    if ((read8() & 0x04) != 0x04) {
        return false;
    }
    // SCR_ext
    if ((read8() & 0x01) != 0x01) {
        return false;
    }

    const int muxrate = read24();
    if ((muxrate & 3) != 3) {
        return false;
    }
    int stuffing = read8() & 7;
    while (stuffing > 0) {
        if (read8() != 0xFF) {
            return false;
        }
        --stuffing;
    }
    return true;
}

bool MpegDemux::demux(int audioChannel, int videoStreamId, bool stopAfterVideo) {
    const bool wantAudio = audioChannel != kNoAudio;
    if (audioChannel >= 0) {
        m_audioChannel = audioChannel;
    }

    bool looksValid = false;
    bool needMore = false;
    bool stop = false;
    while (m_index < m_readSize && !needMore && !stop) {
        // Search for start code
        uint32_t startCode = 0xFF;
        while ((startCode & static_cast<uint32_t>(PACKET_START_CODE_MASK)) !=
                   static_cast<uint32_t>(PACKET_START_CODE_PREFIX) &&
               !isEOF()) {
            startCode = (startCode << 8) | static_cast<uint32_t>(read8());
        }
        // Not enough data available yet.
        if (m_readSize - m_index < 16) {
            m_index = std::max(m_index - 4, 0);
            break;
        }

        switch (startCode) {
        case PACK_START_CODE:
            stats_.packs++;
            if (skipPackHeader()) {
                looksValid = true;
            } else {
                stats_.badPackHeaders++;
            }
            break;
        case SYSTEM_HEADER_START_CODE:
        case PADDING_STREAM:
        case PRIVATE_STREAM_2: {
            looksValid = true;
            const int length = read16();
            if (m_readSize - m_index < length) {
                m_index -= 4 + 2;
                needMore = true;
                break;
            }
            stats_.otherPackets++;
            skip(length);
            break;
        }
        case PRIVATE_STREAM_1: {
            // AUDIO stream
            const int length = read16();
            if (m_readSize - m_index < length) {
                m_index -= 4 + 2;
                needMore = true;
                break;
            }
            stats_.audioPackets++;
            if (wantAudio) {
                m_audioChannel =
                    demuxStream(true, static_cast<int>(startCode), length, m_audioChannel);
            } else {
                skip(length);
            }
            looksValid = true;
            break;
        }
        case 0x1E0:
        case 0x1E1:
        case 0x1E2:
        case 0x1E3:
        case 0x1E4:
        case 0x1E5:
        case 0x1E6:
        case 0x1E7:
        case 0x1E8:
        case 0x1E9:
        case 0x1EA:
        case 0x1EB:
        case 0x1EC:
        case 0x1ED:
        case 0x1EE:
        case 0x1EF: {
            // Video Stream
            const int length = read16();
            // Check for PES header marker.
            looksValid =
                m_index < m_readSize && (m_buf[static_cast<size_t>(m_index)] & 0xC0) == 0x80;
            if (m_readSize - m_index < length) {
                m_index -= 4 + 2;
                needMore = true;
                break;
            }
            if (videoStreamId >= 0 && static_cast<int>(startCode & 0xFF) == videoStreamId) {
                stats_.videoPackets++;
                demuxVideo(static_cast<int>(startCode), length);
                stop = stopAfterVideo;
            } else {
                stats_.otherPackets++;
                demuxStream(false, static_cast<int>(startCode), length, -1);
            }
            break;
        }
        case USER_DATA_START_CODE:
            // User data, probably same as queried by sceMpegGetUserdataAu.
            // Not implemented upstream either.
            looksValid = true;
            break;
        default:
            break;
        }
    }
    if (m_index < m_readSize) {
        const int size = m_readSize - m_index;
        std::memmove(m_buf.data(), m_buf.data() + m_index, static_cast<size_t>(size));
        m_index = 0;
        m_readSize = size;
    } else {
        // Keep any part of a skip (header offset, packet) that runs past the data.
        m_index -= m_readSize;
        m_readSize = 0;
    }

    return looksValid;
}

static bool isHeader(const uint8_t *audioStream, int offset) {
    const uint8_t header1 = 0x0F;
    const uint8_t header2 = 0xD0;
    return (audioStream[offset] == header1) && (audioStream[offset + 1] == header2);
}

static int getNextHeaderPosition(const uint8_t *audioStream, int curpos, int limit, int frameSize) {
    const int endScan = limit - 1;

    // Most common case: the header can be found at each frameSize
    const int offset = curpos + frameSize - 8;
    if (offset < endScan && isHeader(audioStream, offset)) {
        return offset;
    }
    for (int scan = curpos; scan < endScan; scan++) {
        if (isHeader(audioStream, scan)) {
            return scan;
        }
    }
    return -1;
}

int MpegDemux::getNextAudioFrame(uint8_t **buf, int *headerCode1, int *headerCode2, int64_t *pts) {
    int gotsize;
    int frameSize;
    if (!hasNextAudioFrame(&gotsize, &frameSize, headerCode1, headerCode2)) {
        return 0;
    }
    int audioPos = 8;
    const int nextHeader = getNextHeaderPosition(m_audioFrame, audioPos, gotsize, frameSize);
    if (nextHeader >= 0) {
        audioPos = nextHeader;
    } else {
        audioPos = gotsize;
    }
    m_audioStream.pop_front(nullptr, audioPos, pts);
    if (buf) {
        *buf = m_audioFrame + 8;
    }
    return frameSize - 8;
}

bool MpegDemux::hasNextAudioFrame(int *gotsizeOut, int *frameSizeOut, int *headerCode1,
                                  int *headerCode2) {
    const int gotsize = m_audioStream.get_front(m_audioFrame, 0x2000);
    if (gotsize < 4 || !isHeader(m_audioFrame, 0)) {
        return false;
    }
    const uint8_t code1 = m_audioFrame[2];
    const uint8_t code2 = m_audioFrame[3];
    const int frameSize = (((code1 & 0x03) << 8) | (code2 * 8)) + 0x10;
    if (frameSize > gotsize) {
        return false;
    }

    if (gotsizeOut) {
        *gotsizeOut = gotsize;
    }
    if (frameSizeOut) {
        *frameSizeOut = frameSize;
    }
    if (headerCode1) {
        *headerCode1 = code1;
    }
    if (headerCode2) {
        *headerCode2 = code2;
    }
    return true;
}

} // namespace mpo::video
