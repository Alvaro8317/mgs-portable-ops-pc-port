// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/MediaEngine.cpp @ v1.20.4 and DoSwizzleTex16
// (GPU/Common/TextureDecoder.cpp @ v1.20.4).
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
// See media_engine.h for the changes from upstream.

#include "media_engine.h"

#include <algorithm>
#include <cstring>

#include "atrac_track.h"

#ifndef MPO_HAVE_FFMPEG
#define MPO_HAVE_FFMPEG 0
#endif

#if MPO_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#endif

namespace mpo::video {

int VideoPixelModeBytes(int videoPixelMode) {
    switch (videoPixelMode) {
    case GE_CMODE_16BIT_BGR5650:
    case GE_CMODE_16BIT_ABGR5551:
    case GE_CMODE_16BIT_ABGR4444:
        return 2;
    case GE_CMODE_32BIT_ABGR8888:
    default:
        return 4;
    }
}

// Line size in bytes for a destination of `frameWidth` pixels; 0 for an
// unknown mode (upstream's switch leaves it at 0 too).
static int videoLineBytes(int frameWidth, int videoPixelMode) {
    switch (videoPixelMode) {
    case GE_CMODE_32BIT_ABGR8888:
        return frameWidth * 4;
    case GE_CMODE_16BIT_BGR5650:
    case GE_CMODE_16BIT_ABGR5551:
    case GE_CMODE_16BIT_ABGR4444:
        return frameWidth * 2;
    default:
        return 0;
    }
}

void DoSwizzleTex16(const uint8_t *src, uint8_t *dst, int bxc, int byc, int pitch) {
    for (int by = 0; by < byc; by++) {
        const uint8_t *xsrc = src + static_cast<size_t>(by) * 8 * static_cast<size_t>(pitch);
        for (int bx = 0; bx < bxc; bx++) {
            const uint8_t *block = xsrc + static_cast<size_t>(bx) * 16;
            for (int n = 0; n < 8; n++) {
                std::memcpy(dst, block + static_cast<size_t>(n) * static_cast<size_t>(pitch), 16);
                dst += 16;
            }
        }
    }
}

// Helpers that null out alpha (which seems to be the case on the PSP.)
// Some games depend on this, for example Sword Art Online (doesn't clear A's
// from buffer.)
[[maybe_unused]] static void writeVideoLine(uint8_t *dest, const uint8_t *src, int width,
                                            int videoPixelMode) {
    switch (videoPixelMode) {
    case GE_CMODE_32BIT_ABGR8888:
        for (int i = 0; i < width; ++i) {
            uint32_t v;
            std::memcpy(&v, src + i * 4, 4);
            v &= 0x00FFFFFF;
            std::memcpy(dest + i * 4, &v, 4);
        }
        break;
    case GE_CMODE_16BIT_BGR5650:
        std::memcpy(dest, src, static_cast<size_t>(width) * 2);
        break;
    case GE_CMODE_16BIT_ABGR5551:
    case GE_CMODE_16BIT_ABGR4444: {
        const uint16_t mask = videoPixelMode == GE_CMODE_16BIT_ABGR5551 ? 0x7FFF : 0x0FFF;
        for (int i = 0; i < width; ++i) {
            uint16_t v;
            std::memcpy(&v, src + i * 2, 2);
            v &= mask;
            std::memcpy(dest + i * 2, &v, 2);
        }
        break;
    }
    default:
        break;
    }
}

#if MPO_HAVE_FFMPEG

static AVPixelFormat getSwsFormat(int pspFormat) {
    switch (pspFormat) {
    case GE_CMODE_16BIT_BGR5650:
        return AV_PIX_FMT_BGR565LE;
    case GE_CMODE_16BIT_ABGR5551:
        return AV_PIX_FMT_BGR555LE;
    case GE_CMODE_16BIT_ABGR4444:
        return AV_PIX_FMT_BGR444LE;
    case GE_CMODE_32BIT_ABGR8888:
        return AV_PIX_FMT_RGBA;
    default:
        return AV_PIX_FMT_NONE;
    }
}

struct MediaEngine::Codec {
    AVCodecContext *ctx = nullptr;
    AVCodecParserContext *parser = nullptr;
    AVFrame *frame = nullptr;     // decode target (receive_frame unrefs it)
    AVFrame *lastFrame = nullptr; // last decoded picture, kept for conversions
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;
    int swsSrcW = 0;
    int swsSrcH = 0;
    int swsSrcFmt = -1;
    bool frameValid = false; // `frame` holds the last decoded picture
    VideoChunk chunk; // data padded with AV_INPUT_BUFFER_PADDING_SIZE zeros
    size_t chunkLen = 0; // payload bytes in `chunk`
    size_t chunkPos = 0;
    bool chunkPtsUsed = false;
    bool parserFlushed = false;

    ~Codec() {
        sws_freeContext(sws);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&lastFrame);
        if (parser) {
            av_parser_close(parser);
        }
        avcodec_free_context(&ctx);
    }
};

bool HaveVideoDecoder() {
    return avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr;
}

const char *VideoDecoderVersion() {
    return av_version_info();
}

#else

struct MediaEngine::Codec {};

bool HaveVideoDecoder() {
    return false;
}

const char *VideoDecoderVersion() {
    return "none";
}

#endif // MPO_HAVE_FFMPEG

MediaEngine::MediaEngine() {
    m_audioFactory = [](uint32_t codecType, int bytesPerFrame, int channels) {
        return mpo::audio::CreateAtracDecoder(codecType, bytesPerFrame, channels);
    };
}

MediaEngine::~MediaEngine() {
    closeMedia();
}

void MediaEngine::setAudioDecoderFactory(mpo::audio::AtracDecoderFactory factory) {
    m_audioFactory = std::move(factory);
}

void MediaEngine::closeMedia() {
    closeContext();
    m_demux.reset();
    m_videoDemux.reset();
    m_audioContext.reset();
    m_audioChannels = 0;
    m_isVideoEnd = false;
}

void MediaEngine::closeContext() {
    m_codec.reset();
    m_buffer.clear();
    m_sws_fmt = -1;
    m_haveFrame = false;
}

bool MediaEngine::loadStream(const uint8_t *buffer, int readSize, int ringbufferSize) {
    closeMedia();

    m_videopts = 0;
    m_lastPts = -1;
    m_audiopts = 0;
    m_desWidth = 0;
    m_desHeight = 0;
    m_videoStream = -1;
    m_expectedVideoStreams = 0;
    m_videoStreamIds.clear();
    m_lastAudioHeaderCode1 = -1;
    m_inputFinished = false;
    m_decoderDrained = false;
    m_stats = MediaEngineStats{};
    m_ringbuffersize = ringbufferSize;
    // The fixed header fields up to the stream count must be there.
    if (buffer == nullptr || readSize < PSMF_STREAM_TABLE_OFFSET + 2 || ringbufferSize < 0) {
        m_headerValid = false;
        return false;
    }

    m_headerValid = ParsePsmfHeader(std::span<const uint8_t>(buffer, static_cast<size_t>(readSize)),
                                    &m_header) == PsmfError::None;
    m_firstTimeStamp = GetMpegTimeStamp(buffer + PSMF_FIRST_TIMESTAMP_OFFSET);
    m_lastTimeStamp = GetMpegTimeStamp(buffer + PSMF_LAST_TIMESTAMP_OFFSET);
    const int mpegoffset = static_cast<int>(ReadU32BE(buffer + PSMF_STREAM_OFFSET_OFFSET));
    m_demux = std::make_unique<MpegDemux>(ringbufferSize + 2048, mpegoffset);
    m_videoDemux = std::make_unique<MpegDemux>(ringbufferSize + 2048, mpegoffset);
    if (!m_demux->addStreamData(buffer, readSize) || !m_videoDemux->addStreamData(buffer, readSize)) {
        m_demux.reset();
        m_videoDemux.reset();
        return false;
    }
    m_bytesReceived = static_cast<uint64_t>(readSize);
    m_expectedBytes = 0;
    if (m_headerValid && m_header.streamSize != 0) {
        m_expectedBytes = static_cast<uint64_t>(m_header.streamOffset) + m_header.streamSize;
    }

    // SetupStreams: only video streams are set up; audio is demuxed apart.
    int videoStreamNum = -1;
    if (m_headerValid) {
        for (const PsmfStream &s : m_header.streams) {
            if (s.type == PSMF_AVC_STREAM) {
                ++videoStreamNum;
                addVideoStream(videoStreamNum, s.streamId);
            }
        }
    }
    if (videoStreamNum < 0) {
        // Upstream falls back to FFmpeg's stream probing; the PSP layout
        // always puts the first video stream at 0xE0.
        addVideoStream(0);
    }
    openContext();
    return true;
}

bool MediaEngine::addVideoStream(int streamNum, int streamId) {
    if (streamNum < 0) {
        return false;
    }
    if (static_cast<size_t>(streamNum) < m_videoStreamIds.size()) {
        // no need to add an existing stream.
        return true;
    }
    while (m_videoStreamIds.size() <= static_cast<size_t>(streamNum)) {
        const int num = static_cast<int>(m_videoStreamIds.size());
        int id = PSMF_VIDEO_STREAM_ID | (num & 0x0F);
        if (num == streamNum && streamId != -1) {
            id = streamId;
        }
        m_videoStreamIds.push_back(id);
    }
    if (streamNum >= m_expectedVideoStreams) {
        ++m_expectedVideoStreams;
    }
    return true;
}

int MediaEngine::currentVideoStreamId() const {
    if (m_videoStream < 0 || static_cast<size_t>(m_videoStream) >= m_videoStreamIds.size()) {
        return -1;
    }
    return m_videoStreamIds[static_cast<size_t>(m_videoStream)];
}

bool MediaEngine::openContext() {
#if MPO_HAVE_FFMPEG
    if (m_codec || !m_videoDemux) {
        return false;
    }
    const AVCodec *h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!h264) {
        return false;
    }
    auto codec = std::make_unique<Codec>();
    codec->ctx = avcodec_alloc_context3(h264);
    codec->parser = av_parser_init(AV_CODEC_ID_H264);
    codec->frame = av_frame_alloc();
    codec->lastFrame = av_frame_alloc();
    codec->pkt = av_packet_alloc();
    if (!codec->ctx || !codec->parser || !codec->frame || !codec->lastFrame || !codec->pkt) {
        return false;
    }
    codec->ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT | AV_CODEC_FLAG_LOW_DELAY;
    // Upstream lets FFmpeg pick the thread count; one thread keeps the
    // decoder deterministic and a 480x272 stream needs no more.
    codec->ctx->thread_count = 1;
    if (avcodec_open2(codec->ctx, h264, nullptr) < 0) {
        return false;
    }
    m_codec = std::move(codec);
#endif
    m_videoStream = -1;
    if (!setVideoStream(0, true)) {
        return false;
    }
    setVideoDim();
    m_isVideoEnd = false;
    return true;
}

bool MediaEngine::setVideoStream(int streamNum, bool force) {
    if (m_videoStream == streamNum && !force) {
        // Yay, nothing to do.
        return true;
    }
    if (streamNum < 0 || static_cast<size_t>(streamNum) >= m_videoStreamIds.size()) {
        return false;
    }
#if MPO_HAVE_FFMPEG
    if (m_codec && m_videoStream != streamNum && m_videoStream >= 0) {
        // A different elementary stream: start the parser and decoder over.
        avcodec_flush_buffers(m_codec->ctx);
        av_parser_close(m_codec->parser);
        m_codec->parser = av_parser_init(AV_CODEC_ID_H264);
        m_codec->chunk = VideoChunk{};
        m_codec->chunkLen = 0;
        m_codec->chunkPos = 0;
        m_codec->parserFlushed = false;
        m_codec->frameValid = false;
        m_decoderDrained = false;
    }
#endif
    m_videoStream = streamNum;
    return true;
}

bool MediaEngine::setVideoDim(int width, int height) {
#if MPO_HAVE_FFMPEG
    if (!m_codec) {
        return false;
    }
    if (width == 0 && height == 0) {
        // use the orignal video size
        m_desWidth = m_codec->ctx->width;
        m_desHeight = m_codec->ctx->height;
    } else {
        m_desWidth = width;
        m_desHeight = height;
    }
    m_sws_fmt = -1;
    if (m_desWidth <= 0 || m_desHeight <= 0) {
        // Can't setup SWS yet, so stop for now.
        m_desWidth = 0;
        m_desHeight = 0;
        return false;
    }
    m_buffer.assign(static_cast<size_t>(m_desWidth) * static_cast<size_t>(m_desHeight) * 4, 0);
    return true;
#else
    (void)width;
    (void)height;
    return false;
#endif
}

void MediaEngine::updateSwsFormat(int videoPixelMode) {
#if MPO_HAVE_FFMPEG
    if (!m_codec || !m_codec->frameValid) {
        return;
    }
    const AVFrame *f = m_codec->lastFrame;
    const AVPixelFormat swsDesired = getSwsFormat(videoPixelMode);
    if (swsDesired == AV_PIX_FMT_NONE) {
        return;
    }
    if (swsDesired != m_sws_fmt || f->width != m_codec->swsSrcW || f->height != m_codec->swsSrcH ||
        f->format != m_codec->swsSrcFmt) {
        m_sws_fmt = swsDesired;
        m_codec->swsSrcW = f->width;
        m_codec->swsSrcH = f->height;
        m_codec->swsSrcFmt = f->format;
        m_codec->sws = sws_getCachedContext(m_codec->sws, f->width, f->height,
                                            static_cast<AVPixelFormat>(f->format), m_desWidth,
                                            m_desHeight, swsDesired,
                                            SWS_BILINEAR | SWS_ACCURATE_RND | SWS_BITEXACT,
                                            nullptr, nullptr, nullptr);
        if (!m_codec->sws) {
            return;
        }
        int *inv_coefficients;
        int *coefficients;
        int srcRange, dstRange;
        int brightness, contrast, saturation;
        if (sws_getColorspaceDetails(m_codec->sws, &inv_coefficients, &srcRange, &coefficients,
                                     &dstRange, &brightness, &contrast, &saturation) != -1) {
            srcRange = 0;
            dstRange = 0;
            sws_setColorspaceDetails(m_codec->sws, inv_coefficients, srcRange, coefficients,
                                     dstRange, brightness, contrast, saturation);
        }
    }
#else
    (void)videoPixelMode;
#endif
}

bool MediaEngine::inputComplete() const {
    return m_inputFinished || (m_expectedBytes != 0 && m_bytesReceived >= m_expectedBytes);
}

void MediaEngine::finishInput() {
    m_inputFinished = true;
}

void MediaEngine::demuxAudio() {
    if (m_demux) {
        m_demux->demux(m_audioStream, -1);
    }
}

int MediaEngine::addStreamData(const uint8_t *buffer, int addSize) {
    int size = addSize;
    if (size > 0 && m_videoDemux) {
        // The video side is upstream's m_pdata: it decides whether the data fits.
        if (!m_videoDemux->addStreamData(buffer, addSize)) {
            size = 0;
        } else {
            m_bytesReceived += static_cast<uint64_t>(addSize);
            if (m_demux->getRemainSize() < addSize) {
                // Make room: upstream demuxes audio lazily and would drop the data.
                demuxAudio();
            }
            if (!m_demux->addStreamData(buffer, addSize)) {
                m_stats.audioInputDropped += static_cast<uint64_t>(addSize);
            }
        }
#if MPO_HAVE_FFMPEG
        if (size > 0 && m_codec && m_decoderDrained) {
            // More data after a drain: reopen the decoder for it.
            avcodec_flush_buffers(m_codec->ctx);
            m_codec->parserFlushed = false;
            m_decoderDrained = false;
        }
#endif
        // We added data, so... not the end anymore?
        m_isVideoEnd = false;
    }
    return size;
}

bool MediaEngine::nextVideoPacket() {
#if MPO_HAVE_FFMPEG
    Codec &c = *m_codec;
    while (true) {
        if (c.chunkPos < c.chunkLen) {
            uint8_t *out = nullptr;
            int outSize = 0;
            // Like libavformat's parse_packet: the PES timestamps go with the
            // first parse call of the chunk only.
            int64_t pts = AV_NOPTS_VALUE;
            int64_t dts = AV_NOPTS_VALUE;
            if (!c.chunkPtsUsed) {
                pts = c.chunk.pts >= 0 ? c.chunk.pts : AV_NOPTS_VALUE;
                dts = c.chunk.dts >= 0 ? c.chunk.dts : AV_NOPTS_VALUE;
                c.chunkPtsUsed = true;
            }
            const int remaining = static_cast<int>(c.chunkLen - c.chunkPos);
            const int len = av_parser_parse2(c.parser, c.ctx, &out, &outSize,
                                             c.chunk.data.data() + c.chunkPos, remaining, pts,
                                             dts, 0);
            if (len <= 0 && outSize <= 0) {
                c.chunkPos = c.chunkLen;
            } else if (len > 0) {
                c.chunkPos += static_cast<size_t>(len);
            }
            if (outSize > 0) {
                c.pkt->buf = nullptr;
                c.pkt->data = out;
                c.pkt->size = outSize;
                c.pkt->pts = c.parser->pts;
                c.pkt->dts = c.parser->dts;
                c.pkt->duration = 0;
                c.pkt->flags = c.parser->key_frame == 1 ? AV_PKT_FLAG_KEY : 0;
                return true;
            }
            continue;
        }
        if (m_videoDemux && m_videoDemux->popVideoChunk(&c.chunk)) {
            // The parser reads up to AV_INPUT_BUFFER_PADDING_SIZE bytes past the input.
            c.chunkLen = c.chunk.data.size();
            c.chunk.data.resize(c.chunkLen + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            c.chunkPos = 0;
            c.chunkPtsUsed = false;
            continue;
        }
        if (m_videoDemux && m_videoDemux->getPendingSize() > 0) {
            // Read on until the next video packet, like av_read_frame().
            const size_t before = m_videoDemux->videoQueuedChunks();
            const int pendingBefore = m_videoDemux->getPendingSize();
            m_videoDemux->demux(MpegDemux::kNoAudio, currentVideoStreamId(), true);
            if (m_videoDemux->videoQueuedChunks() > before ||
                m_videoDemux->getPendingSize() != pendingBefore) {
                continue;
            }
        }
        if (inputComplete() && !c.parserFlushed) {
            c.parserFlushed = true;
            uint8_t *out = nullptr;
            int outSize = 0;
            av_parser_parse2(c.parser, c.ctx, &out, &outSize, nullptr, 0, AV_NOPTS_VALUE,
                             AV_NOPTS_VALUE, 0);
            if (outSize > 0) {
                c.pkt->buf = nullptr;
                c.pkt->data = out;
                c.pkt->size = outSize;
                c.pkt->pts = c.parser->pts;
                c.pkt->dts = c.parser->dts;
                c.pkt->duration = 0;
                c.pkt->flags = c.parser->key_frame == 1 ? AV_PKT_FLAG_KEY : 0;
                return true;
            }
        }
        return false;
    }
#else
    return false;
#endif
}

bool MediaEngine::stepVideo(int videoPixelMode, bool skipFrame) {
#if MPO_HAVE_FFMPEG
    if (!m_codec) {
        return false;
    }
    Codec &c = *m_codec;
    bool bGetFrame = false;
    while (!bGetFrame) {
        const int result = avcodec_receive_frame(c.ctx, c.frame);
        if (result == 0) {
            av_frame_unref(c.lastFrame);
            av_frame_move_ref(c.lastFrame, c.frame);
            c.frameValid = true;
            m_stats.videoFramesDecoded++;
#ifdef AV_FRAME_FLAG_CORRUPT
            if ((c.lastFrame->flags & AV_FRAME_FLAG_CORRUPT) != 0 ||
                c.lastFrame->decode_error_flags != 0) {
                m_stats.corruptFrames++;
            }
#else
            if (c.lastFrame->decode_error_flags != 0) {
                m_stats.corruptFrames++;
            }
#endif
            if (m_buffer.empty() || m_desWidth == 0) {
                setVideoDim();
            }
            if (!m_buffer.empty() && !skipFrame) {
                updateSwsFormat(videoPixelMode);
                if (m_codec->sws) {
                    uint8_t *dstData[4] = {m_buffer.data(), nullptr, nullptr, nullptr};
                    int dstLinesize[4] = {VideoPixelModeBytes(videoPixelMode) * m_desWidth, 0, 0, 0};
                    sws_scale(c.sws, c.lastFrame->data, c.lastFrame->linesize, 0, c.lastFrame->height, dstData,
                              dstLinesize);
                    m_lastPixelMode = videoPixelMode;
                    m_haveFrame = true;
                }
            }

            const int64_t bestPts = c.lastFrame->best_effort_timestamp;
            // Our packets carry no duration, so this is upstream's
            // "ptsDuration == 0" branch. The NOPTS test goes first: upstream
            // subtracts from AV_NOPTS_VALUE (INT64_MIN) before testing it.
            if (bestPts == AV_NOPTS_VALUE || m_lastPts == bestPts - m_firstTimeStamp) {
                // TODO (upstream): Assuming 29.97 if missing.
                m_videopts += VIDEO_FRAME_DURATION_TS;
            } else {
                m_videopts = bestPts - m_firstTimeStamp;
                m_lastPts = m_videopts;
            }
            bGetFrame = true;
            break;
        }
        if (result == AVERROR_EOF) {
            // Drained: every frame of the stream is out.
            m_isVideoEnd = true;
            break;
        }
        if (result != AVERROR(EAGAIN)) {
            m_stats.videoDecodeErrors++;
        }
        if (nextVideoPacket()) {
            const int sent = avcodec_send_packet(c.ctx, c.pkt);
            m_stats.videoPacketsSent++;
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                // A bad access unit: count it and go on with the next one.
                m_stats.videoDecodeErrors++;
            }
            continue;
        }
        if (inputComplete() && !m_decoderDrained) {
            // The decoder gives us the re-ordered frames with a NULL packet.
            avcodec_send_packet(c.ctx, nullptr);
            m_decoderDrained = true;
            continue;
        }
        // No frame from the data available now.
        m_isVideoEnd = true;
        break;
    }
    return bGetFrame;
#else
    (void)skipFrame;
    m_lastPixelMode = videoPixelMode;
    // If video engine is not available, just add to the timestamp at least.
    m_videopts += VIDEO_FRAME_DURATION_TS;
    return true;
#endif
}

const uint8_t *MediaEngine::getFrameImage() const {
    return m_haveFrame ? m_buffer.data() : nullptr;
}

int MediaEngine::writeVideoImage(std::span<uint8_t> dest, bool swizzle, int frameWidth,
                                 int videoPixelMode) {
    return writeVideoImageWithRange(dest, swizzle, frameWidth, videoPixelMode, 0, 0, m_desWidth,
                                    m_desHeight);
}

int MediaEngine::writeVideoImageWithRange(std::span<uint8_t> dest, bool swizzle, int frameWidth,
                                          int videoPixelMode, int xpos, int ypos, int width,
                                          int height) {
    const int videoLineSize = videoLineBytes(frameWidth, videoPixelMode);
    if (frameWidth <= 0 || frameWidth > 2048 || videoLineSize == 0 || height <= 0) {
        // Clearly invalid values.  Let's just not.
        return 0;
    }
    const size_t videoImageSize = static_cast<size_t>(videoLineSize) * static_cast<size_t>(height);
    int byc = (height + 7) / 8;
    if (byc == 0) {
        byc = 1;
    }
    const size_t swizzledSize = static_cast<size_t>(videoLineSize) * static_cast<size_t>(byc) * 8;
    if (dest.size() < (swizzle ? swizzledSize : videoImageSize)) {
        return 0;
    }
#if MPO_HAVE_FFMPEG
    if (!m_haveFrame || xpos < 0 || ypos < 0 || xpos >= m_desWidth || ypos >= m_desHeight) {
        return 0;
    }
    if (videoPixelMode != m_lastPixelMode) {
        // Upstream converts in stepVideo() with the same mode; convert again
        // from the decoded picture if the caller changed it.
        if (!m_codec || !m_codec->frameValid) {
            return 0;
        }
        updateSwsFormat(videoPixelMode);
        if (!m_codec->sws) {
            return 0;
        }
        uint8_t *dstData[4] = {m_buffer.data(), nullptr, nullptr, nullptr};
        int dstLinesize[4] = {VideoPixelModeBytes(videoPixelMode) * m_desWidth, 0, 0, 0};
        sws_scale(m_codec->sws, m_codec->lastFrame->data, m_codec->lastFrame->linesize, 0,
                  m_codec->lastFrame->height, dstData, dstLinesize);
        m_lastPixelMode = videoPixelMode;
    }

    width = std::min(width, m_desWidth - xpos);
    height = std::min(height, m_desHeight - ypos);
    // Never write past the destination line.
    width = std::min(width, frameWidth);

    std::vector<uint8_t> swizzleTemp;
    uint8_t *imgbuf = dest.data();
    if (swizzle) {
        swizzleTemp.assign(swizzledSize, 0);
        imgbuf = swizzleTemp.data();
    }
    const int bpp = VideoPixelModeBytes(videoPixelMode);
    const uint8_t *data = m_buffer.data() +
                          (static_cast<size_t>(ypos) * static_cast<size_t>(m_desWidth) +
                           static_cast<size_t>(xpos)) * static_cast<size_t>(bpp);
    for (int y = 0; y < height; y++) {
        writeVideoLine(imgbuf + static_cast<size_t>(videoLineSize) * static_cast<size_t>(y), data,
                       width, videoPixelMode);
        data += static_cast<size_t>(m_desWidth) * static_cast<size_t>(bpp);
    }
    if (swizzle) {
        DoSwizzleTex16(imgbuf, dest.data(), videoLineSize / 16, byc, videoLineSize);
    }
    return static_cast<int>(videoImageSize);
#else
    (void)xpos;
    (void)ypos;
    (void)width;
    return 0;
#endif
}

int MediaEngine::getRemainSize() const {
    if (!m_videoDemux) {
        return 0;
    }
    return std::max(m_videoDemux->getRemainSize() - 2048, 0);
}

int MediaEngine::getAudioRemainSize() const {
    if (!m_demux) {
        // No audio, so it can't be full, return video instead.
        return getRemainSize();
    }
    return m_demux->getRemainSize();
}

int MediaEngine::getNextAudioFrame(uint8_t **buf, int *headerCode1, int *headerCode2) {
    if (!m_demux) {
        return 0;
    }
    // When getting a frame, increment pts
    m_audiopts += AUDIO_FRAME_DURATION_TS;

    // Demux now (rather than on add data) so that we select the right stream.
    demuxAudio();

    int64_t pts = 0;
    const int result = m_demux->getNextAudioFrame(buf, headerCode1, headerCode2, &pts);
    if (pts != 0) {
        // m_audiopts is supposed to be after the returned frame.
        m_audiopts = pts - m_firstTimeStamp + AUDIO_FRAME_DURATION_TS;
    }
    return result;
}

int MediaEngine::getAudioSamples(std::span<int16_t> out) {
    // Upstream logs a bad address and still consumes the frame.
    int16_t *buffer = out.size() >= 2048 * 2 ? out.data() : nullptr;
    if (!m_demux) {
        return 0;
    }

    uint8_t *audioFrame = nullptr;
    int headerCode1 = 0;
    int headerCode2 = 0;
    const int frameSize = getNextAudioFrame(&audioFrame, &headerCode1, &headerCode2);
    if (frameSize == 0) {
        return 0;
    }
    m_lastAudioHeaderCode1 = headerCode1;
    if (!m_audioContext && m_audioFactory) {
        // Upstream calls SetChannels(1) before decoding a mono frame; it only
        // takes effect before the codec opens, i.e. on the first frame.
        m_audioChannels = headerCode1 == 0x24 ? 1 : 2;
        m_audioContext = m_audioFactory(mpo::audio::PSP_CODEC_AT3PLUS, 0, m_audioChannels);
    }
    if (m_audioContext) {
        int outSamples = 0;
        int inbytesConsumed = 0;
        if (!m_audioContext->Decode(audioFrame, frameSize, &inbytesConsumed, 2, buffer, &outSamples)) {
            m_stats.audioDecodeErrors++;
        } else {
            m_stats.audioFrames++;
        }
    }
    return 0x2000;
}

bool MediaEngine::IsNoAudioData() {
    if (!m_demux) {
        return true;
    }
    // Let's double check.  Here should be a safe enough place to demux.
    demuxAudio();
    return !m_demux->hasNextAudioFrame(nullptr, nullptr, nullptr, nullptr);
}

int64_t MediaEngine::getAudioTimeStamp() const {
    return m_demux ? m_audiopts - AUDIO_FRAME_DURATION_TS : -1;
}

int64_t MediaEngine::getLastTimeStamp() const {
    if (!m_demux) {
        return 0;
    }
    return m_lastTimeStamp - m_firstTimeStamp;
}

bool MediaEngine::seekTo(int64_t timestamp, int videoPixelMode) {
    if (timestamp <= 0) {
        return true;
    }

    // Just doing it the not so great way to be sure audio is in sync.
    int timeout = 1000;
    while (getVideoTimeStamp() < timestamp - VIDEO_FRAME_DURATION_TS) {
        if (getAudioTimeStamp() < getVideoTimeStamp() - AUDIO_FRAME_DURATION_TS * 2) {
            getNextAudioFrame(nullptr, nullptr, nullptr);
        }
        if (!stepVideo(videoPixelMode, true)) {
            return false;
        }
        if (--timeout <= 0) {
            return true;
        }
    }

    while (getAudioTimeStamp() < getVideoTimeStamp() - AUDIO_FRAME_DURATION_TS * 2) {
        if (getNextAudioFrame(nullptr, nullptr, nullptr) == 0) {
            return false;
        }
        if (--timeout <= 0) {
            return true;
        }
    }
    return true;
}

} // namespace mpo::video
