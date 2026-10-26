// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/MediaEngine.h and Core/HW/MediaEngine.cpp
// @ v1.20.4, plus DoSwizzleTex16 (GPU/Common/TextureDecoder.cpp).
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// The PSP media engine as sceMpeg/scePsmf see it: PSMF data goes in (as the
// ring buffer delivers it), decoded H.264 frames and ATRAC3plus PCM come out.
// Same method names and timestamp rules as upstream, so F17b can port
// sceMpeg.cpp on top of it.
//
// Differences from upstream:
//   - no libavformat: a second MpegDemux reads the program stream on demand
//     for the video side (its input buffer is upstream's m_pdata and sets
//     getRemainSize()), extracts the H.264 stream, and the FFmpeg h264 parser
//     splits it into access units. Audio keeps upstream's own MpegDemux;
//   - if the audio demuxer's buffer is full when data arrives, it demuxes
//     first instead of dropping the data;
//   - when the whole stream has arrived (header streamOffset + streamSize
//     bytes, or finishInput()), the decoder is drained, so the frames it
//     holds back for reordering come out too; upstream never drains;
//   - swscale gets SWS_ACCURATE_RND | SWS_BITEXACT on top of SWS_BILINEAR so
//     the RGB output does not depend on the CPU or on the FFmpeg asm;
//   - outputs go to spans (F17b builds them over guest memory and decides
//     whether the destination is swizzled VRAM);
//   - writes never run past a line when frameWidth < video width;
//   - without FFmpeg (MPO_HAVE_FFMPEG=0) stepVideo() only advances the video
//     timestamp, like upstream's !USE_FFMPEG build; audio still works.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "atrac_decoder.h"
#include "mpeg_demux.h"
#include "psmf.h"

namespace mpo::video {

// GE pixel formats accepted by sceMpegAvcDecodeMode / scePsmfPlayer.
enum VideoPixelMode : int {
    GE_CMODE_16BIT_BGR5650 = 0,
    GE_CMODE_16BIT_ABGR5551 = 1,
    GE_CMODE_16BIT_ABGR4444 = 2,
    GE_CMODE_32BIT_ABGR8888 = 3,
};

// Bytes per pixel of a VideoPixelMode (4 for unknown modes, like upstream).
int VideoPixelModeBytes(int videoPixelMode);

// Block swizzle of GE textures: 16-byte x 8-row blocks, stored block after
// block. `src` holds `byc * 8` rows of `pitch` bytes.
void DoSwizzleTex16(const uint8_t *src, uint8_t *dst, int bxc, int byc, int pitch);

// True when this build can decode H.264 (linked against FFmpeg).
bool HaveVideoDecoder();
// "libavcodec x.y.z" or "none".
const char *VideoDecoderVersion();

struct MediaEngineStats {
    uint64_t videoPacketsSent = 0;   // access units handed to the decoder
    uint64_t videoFramesDecoded = 0; // frames that came out
    uint64_t videoDecodeErrors = 0;  // send/receive errors other than EAGAIN/EOF
    uint64_t audioFrames = 0;        // ATRAC3plus frames decoded
    uint64_t audioDecodeErrors = 0;
    uint64_t audioInputDropped = 0;  // bytes the audio demuxer had no room for
    uint64_t corruptFrames = 0;      // frames flagged AV_FRAME_FLAG_CORRUPT
};

class MediaEngine {
public:
    MediaEngine();
    ~MediaEngine();
    MediaEngine(const MediaEngine &) = delete;
    MediaEngine &operator=(const MediaEngine &) = delete;

    // Replaces the ATRAC3plus decoder (tests use a fake one). Takes effect
    // for the next stream.
    void setAudioDecoderFactory(mpo::audio::AtracDecoderFactory factory);

    void closeMedia();
    // `buffer` starts with the PSMF header; `readSize` bytes of it are used.
    // `ringbufferSize` sizes the internal buffers (upstream adds 2048).
    bool loadStream(const uint8_t *buffer, int readSize, int ringbufferSize);
    bool addVideoStream(int streamNum, int streamId = -1);
    // Returns the number of bytes accepted (0 if they do not fit).
    int addStreamData(const uint8_t *buffer, int addSize);
    // Marks the end of the input: the decoder may be drained.
    void finishInput();
    bool seekTo(int64_t timestamp, int videoPixelMode);

    bool setVideoStream(int streamNum, bool force = false);
    // TODO (upstream): Return false if the stream doesn't exist.
    bool setAudioStream(int streamNum) {
        m_audioStream = streamNum;
        return true;
    }

    // Last converted frame: VideoWidth() x VideoHeight() pixels in the pixel
    // mode of the last stepVideo(), alpha as decoded (not yet cleared).
    const uint8_t *getFrameImage() const;
    int getRemainSize() const;
    int getAudioRemainSize() const;

    bool stepVideo(int videoPixelMode, bool skipFrame = false);
    // Writes the last frame like sceMpegAvcDecode: `frameWidth` pixels per
    // line, alpha (or the 16-bit top bits) cleared as on the PSP, swizzled if
    // the destination is swizzled VRAM. Returns the bytes written, or 0 if
    // `dest` is too small or frameWidth > 2048.
    int writeVideoImage(std::span<uint8_t> dest, bool swizzle, int frameWidth = 512,
                        int videoPixelMode = GE_CMODE_32BIT_ABGR8888);
    int writeVideoImageWithRange(std::span<uint8_t> dest, bool swizzle, int frameWidth,
                                 int videoPixelMode, int xpos, int ypos, int width, int height);
    // Decodes the next audio frame into `out` (2048 stereo samples = 4096
    // values). Returns 0x2000 if a frame was consumed, 0 otherwise.
    int getAudioSamples(std::span<int16_t> out);

    int64_t getVideoTimeStamp() const { return m_videopts; }
    int64_t getAudioTimeStamp() const;
    int64_t getLastTimeStamp() const;

    bool IsVideoEnd() const { return m_isVideoEnd; }
    bool IsNoAudioData();
    bool IsActuallyPlayingAudio() const { return getAudioTimeStamp() >= 0; }
    int VideoWidth() const { return m_desWidth; }
    int VideoHeight() const { return m_desHeight; }

    // Not upstream: the parsed header (valid after loadStream) and counters.
    const PsmfHeader &header() const { return m_header; }
    bool headerValid() const { return m_headerValid; }
    const MediaEngineStats &stats() const { return m_stats; }
    const MpegDemux *audioDemux() const { return m_demux.get(); }
    const MpegDemux *videoDemux() const { return m_videoDemux.get(); }
    // headerCode1 of the last ATRAC3plus frame header (0x24 = mono), or -1.
    int audioHeaderCode1() const { return m_lastAudioHeaderCode1; }
    // Channels the ATRAC3plus decoder was opened with (0 = not opened yet).
    int audioChannels() const { return m_audioChannels; }

private:
    struct Codec; // FFmpeg state (empty without FFmpeg)

    bool openContext();
    void closeContext();
    bool setVideoDim(int width = 0, int height = 0);
    void updateSwsFormat(int videoPixelMode);
    int getNextAudioFrame(uint8_t **buf, int *headerCode1, int *headerCode2);
    // Next access unit from the parser; false if none is available now.
    bool nextVideoPacket();
    void demuxAudio();
    bool inputComplete() const;
    int currentVideoStreamId() const;

    std::unique_ptr<Codec> m_codec;
    mpo::audio::AtracDecoderFactory m_audioFactory;
    std::unique_ptr<mpo::audio::AudioDecoder> m_audioContext;
    std::unique_ptr<MpegDemux> m_demux;      // audio, as upstream
    std::unique_ptr<MpegDemux> m_videoDemux; // video, in place of libavformat

    PsmfHeader m_header;
    bool m_headerValid = false;
    std::vector<int> m_videoStreamIds; // stream number -> 0xE0..0xEF

    std::vector<uint8_t> m_buffer; // converted frame
    int m_sws_fmt = -1;
    int m_lastPixelMode = GE_CMODE_32BIT_ABGR8888;
    bool m_haveFrame = false;

    int m_desWidth = 0;
    int m_desHeight = 0;
    int64_t m_videopts = 0;
    int64_t m_firstTimeStamp = 0;
    int64_t m_lastTimeStamp = 0;
    bool m_isVideoEnd = false;

    int m_videoStream = -1;
    int m_expectedVideoStreams = 0;
    int m_audioStream = -1;
    int64_t m_lastPts = -1;
    int64_t m_audiopts = 0;
    int m_ringbuffersize = 0;
    int m_lastAudioHeaderCode1 = -1;
    int m_audioChannels = 0;

    uint64_t m_bytesReceived = 0;  // including the header
    uint64_t m_expectedBytes = 0;  // streamOffset + streamSize, 0 if unknown
    bool m_inputFinished = false;
    bool m_decoderDrained = false; // NULL packet sent

    MediaEngineStats m_stats;
};

} // namespace mpo::video
