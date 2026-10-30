// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceMpeg (F11b), ported from PPSSPP Core/HLE/sceMpeg.cpp @ v1.20.4 over the
// media engine of F17a (mpo_video::MediaEngine): the 23 NIDs the stages
// `title`, `ending` and `flashdemo` import.
//
// The ring buffer, the streams, the access units, the timestamps and the end
// of the stream are PPSSPP's: the data goes through the demuxer and the
// decoder, so the game sees the video advance and end as on the PSP. F17b:
// sceMpegAvcDecode writes the decoded image (swizzled on the swizzled VRAM
// mirror) and sceMpegAtracDecode the ATRAC3plus PCM. No stage imports
// scePsmf, so it is not registered.
//
// sceMpegRingbufferPut runs the game's read callback right away on the
// calling thread (CpuBridge::call_guest), where PPSSPP queues it to run when
// the call returns; the result is the same.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

#include "media_engine.h"

namespace mpo::hle {

class System;

class Mpeg {
public:
    explicit Mpeg(System &system);
    ~Mpeg();
    Mpeg(const Mpeg &) = delete;
    Mpeg &operator=(const Mpeg &) = delete;

    void register_hle();

    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    // ATRAC3plus frames written by sceMpegAtracDecode.
    [[nodiscard]] std::uint64_t audio_frames() const noexcept { return audio_frames_; }
    // Replaces the ATRAC3plus decoder of the contexts created from now on
    // (tests use a fake one; empty: the real one).
    void set_audio_decoder_factory(audio::AtracDecoderFactory factory) {
        audio_factory_ = std::move(factory);
    }

private:
    struct StreamInfo {
        int type = 0;
        int num = 0;
        int sid = 0;
        bool needs_reset = false;
    };

    static constexpr int kEsBuffers = 2; // MPEG_DATA_ES_BUFFERS

    struct Context {
        std::uint8_t header[2048]{};
        std::uint32_t default_frame_width = 0;
        int video_frame_count = 0;
        int audio_frame_count = 0;
        bool end_of_audio_reached = false;
        bool end_of_video_reached = false;
        int video_pixel_mode = 3; // GE_CMODE_32BIT_ABGR8888
        std::uint32_t magic = 0;
        int version = 0;
        std::uint32_t raw_version = 0;
        std::uint32_t offset = 0;
        std::uint32_t stream_size = 0;
        std::int64_t first_timestamp = 0;
        std::int64_t last_timestamp = 0;
        std::uint32_t ringbuffer_addr = 0;
        bool es_buffers[kEsBuffers]{};
        int avc_detail_frame_width = 0;
        int avc_detail_frame_height = 0;
        int avc_decode_result = 0;
        int avc_frame_status = 0;
        bool avc_registered = false;
        bool atrac_registered = false;
        bool pcm_registered = false;
        bool data_registered = false;
        bool analyzed = false;
        std::map<std::uint32_t, StreamInfo> streams;
        std::unique_ptr<video::MediaEngine> engine;
    };

    Context *context(std::uint32_t mpeg_addr);             // getMpegCtx
    void analyze(std::uint32_t buffer_addr, Context &ctx); // AnalyzeMpeg on guest data
    void analyze_bytes(const std::uint8_t *buffer, std::uint32_t valid, Context &ctx);
    std::uint32_t ringbuffer_construct(std::uint32_t ring, std::uint32_t packets,
                                       std::uint32_t data, std::uint32_t size,
                                       std::uint32_t callback, std::uint32_t callback_arg);
    std::uint32_t create(std::uint32_t mpeg_addr, std::uint32_t data, std::uint32_t size,
                         std::uint32_t ring, std::uint32_t frame_width);
    std::uint32_t ringbuffer_put(std::uint32_t ring, std::int32_t packets, std::int32_t available);
    void post_put(std::uint32_t ring, std::int32_t added);
    std::uint32_t get_avc_au(std::uint32_t mpeg, std::uint32_t stream, std::uint32_t au,
                             std::uint32_t attr);
    std::uint32_t get_atrac_au(std::uint32_t mpeg, std::uint32_t stream, std::uint32_t au,
                               std::uint32_t attr);
    std::uint32_t avc_decode(std::uint32_t mpeg, std::uint32_t au, std::uint32_t frame_width,
                             std::uint32_t buffer_addr, std::uint32_t init_addr);
    std::uint32_t atrac_decode(std::uint32_t mpeg, std::uint32_t au, std::uint32_t buffer);
    void write_video_image(Context &ctx, std::uint32_t buffer, std::uint32_t frame_width);

    System &system_;
    bool initialized_ = false;
    int lib_version_ = 0x010A; // No sceMpeg_library PRX: the firmware's.
    std::uint32_t next_stream_id_ = 1;
    std::int32_t put_packets_added_ = 0;
    std::map<std::uint32_t, std::unique_ptr<Context>> contexts_;
    std::uint64_t frames_ = 0;
    std::uint64_t audio_frames_ = 0;
    audio::AtracDecoderFactory audio_factory_;
};

} // namespace mpo::hle
