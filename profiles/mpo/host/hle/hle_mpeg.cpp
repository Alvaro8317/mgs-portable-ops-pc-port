// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceMpeg (F11b). Ported from PPSSPP Core/HLE/sceMpeg.cpp @ v1.20.4
// (GPL-2.0-or-later): the contexts and their handle, the PSMF header
// analysis, the ring buffer and its read callback, the stream registry, the
// access units with their timestamps, the end of the stream, the error codes
// and the delays. Left out: the PMP video path, the compat flags (none is set
// for MPO) and the ring buffer validation of libmpeg < 0x0105 (MPO uses the
// firmware's 0x010A). F17b writes the decoded image and the PCM.

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include "hle/hle_args.hpp"
#include "hle/mpeg.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kPsmfMagic = 0x464D5350u;
constexpr int kPsmfStreamVersionOffset = 0x4;
constexpr int kPsmfStreamOffsetOffset = 0x8;
constexpr int kPsmfStreamSizeOffset = 0xC;
constexpr int kPsmfFirstTimestampOffset = 0x54;
constexpr int kPsmfLastTimestampOffset = 0x5A;

constexpr int kMpegAvcStream = 0;
constexpr int kMpegAtracStream = 1;
constexpr int kMpegPcmStream = 2;
constexpr int kMpegDataStream = 3;
constexpr int kMpegAudioStream = 15;

constexpr std::uint32_t kMpegAvcEsSize = 2048;
constexpr std::uint32_t kMpegAtracEsSize = 2112;
constexpr std::uint32_t kMpegAtracEsOutputSize = 8192;
constexpr std::uint32_t kMpegMemsize0104 = 0x0B3DBu;
constexpr std::uint32_t kMpegMemsize0105 = 0x10000u;
constexpr int kMpegAvcDecodeSuccess = 1;

// Delays, in microseconds.
constexpr std::uint32_t kAtracDecodeDelay = 3000;
constexpr std::uint32_t kAvcFirstDelay = 3600;
constexpr std::uint32_t kAvcDecodeDelay = 5400;
constexpr std::uint32_t kAvcEmptyDelay = 320;
constexpr std::uint32_t kMpegDecodeErrorDelay = 100;
constexpr std::int64_t kVideoTimestampStep = 3003;
constexpr std::int64_t kUnknownTimestamp = -1;

constexpr std::uint32_t kErrIllegalAddress = 0x8002006Au;
constexpr std::uint32_t kErrMpegNoData = 0x80618001u;
constexpr std::uint32_t kErrMpegBadVersion = 0x80610002u;
constexpr std::uint32_t kErrMpegNotYetInit = 0x80618009u;
constexpr std::uint32_t kErrMpegNoMemory = 0x80610022u;
constexpr std::uint32_t kErrMpegInvalidValue = 0x806101FEu;
constexpr std::uint32_t kErrMpegAvcDecodeFatal = 0x80628002u;
constexpr std::uint32_t kFailure = 0xFFFFFFFFu;

// The PSMF 1.2-1.5 version strings, little-endian.
int mpeg_version(std::uint32_t raw) {
    switch (raw) {
    case 0x32313030u:
        return 0;
    case 0x33313030u:
        return 1;
    case 0x34313030u:
        return 2;
    case 0x35313030u:
        return 3;
    default:
        return -1;
    }
}

std::uint32_t read_be32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint32_t read_le32(const std::uint8_t *p) {
    std::uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}

// getMpegTimeStamp (MediaEngine.h).
std::int64_t mpeg_timestamp(const std::uint8_t *p) {
    return static_cast<std::int64_t>(p[5]) | (static_cast<std::int64_t>(p[4]) << 8) |
           (static_cast<std::int64_t>(p[3]) << 16) | (static_cast<std::int64_t>(p[2]) << 24) |
           (static_cast<std::int64_t>(p[1]) << 32) | (static_cast<std::int64_t>(p[0]) << 36);
}

// SceMpegRingBuffer.
namespace ring {
constexpr std::uint32_t kPackets = 0;
constexpr std::uint32_t kPacketsRead = 4;
constexpr std::uint32_t kPacketsWritePos = 8;
constexpr std::uint32_t kPacketsAvail = 12;
constexpr std::uint32_t kPacketSize = 16;
constexpr std::uint32_t kData = 20;
constexpr std::uint32_t kCallbackAddr = 24;
constexpr std::uint32_t kCallbackArgs = 28;
constexpr std::uint32_t kDataUpperBound = 32;
constexpr std::uint32_t kMpeg = 40;
constexpr std::uint32_t kGp = 44;
constexpr std::uint32_t kSize = 48;
} // namespace ring

// SceMpegAu: the timestamps keep their high word first.
struct Au {
    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::uint32_t es_buffer = 0;
    std::uint32_t es_size = 0;
};

std::int64_t read_ts(kernel::GuestMemory &m, std::uint32_t address) {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(m.read_u32(address)) << 32) |
                                     m.read_u32(address + 4u));
}

void write_ts(kernel::GuestMemory &m, std::uint32_t address, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    m.write_u32(address, static_cast<std::uint32_t>(bits >> 32));
    m.write_u32(address + 4u, static_cast<std::uint32_t>(bits));
}

Au read_au(kernel::GuestMemory &m, std::uint32_t address) {
    Au au;
    if (!m.is_valid(address, 24u)) return au;
    au.pts = read_ts(m, address);
    au.dts = read_ts(m, address + 8u);
    au.es_buffer = m.read_u32(address + 16u);
    au.es_size = m.read_u32(address + 20u);
    return au;
}

void write_au(kernel::GuestMemory &m, std::uint32_t address, const Au &au) {
    if (!m.is_valid(address, 24u)) return;
    write_ts(m, address, au.pts);
    write_ts(m, address + 8u, au.dts);
    m.write_u32(address + 16u, au.es_buffer);
    m.write_u32(address + 20u, au.es_size);
}

// Memory::ClampValidSizeAt.
std::uint32_t clamp_valid(kernel::GuestMemory &m, std::uint32_t address, std::uint32_t size) {
    if (size == 0u || !m.is_valid(address, 1u)) return 0u;
    if (m.is_valid(address, size)) return size;
    std::uint32_t lo = 1u;
    std::uint32_t hi = size;
    while (hi - lo > 1u) {
        const std::uint32_t mid = lo + (hi - lo) / 2u;
        (m.is_valid(address, mid) ? lo : hi) = mid;
    }
    return lo;
}

std::vector<std::uint8_t> read_guest(kernel::GuestMemory &m, std::uint32_t address,
                                     std::uint32_t size) {
    const std::uint32_t valid = clamp_valid(m, address, size);
    const std::uint8_t *p = valid != 0u ? m.translate(address, valid) : nullptr;
    return p != nullptr ? std::vector<std::uint8_t>(p, p + valid) : std::vector<std::uint8_t>{};
}

} // namespace

Mpeg::Mpeg(System &system) : system_(system) {}

Mpeg::~Mpeg() = default;

Mpeg::Context *Mpeg::context(std::uint32_t mpeg_addr) {
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(mpeg_addr, 4u)) return nullptr;
    const auto found = contexts_.find(m.read_u32(mpeg_addr));
    return found == contexts_.end() ? nullptr : found->second.get();
}

void Mpeg::analyze(std::uint32_t buffer_addr, Context &ctx) {
    std::vector<std::uint8_t> buffer = read_guest(system_.kernel().memory(), buffer_addr, 32768u);
    const auto valid = static_cast<std::uint32_t>(buffer.size());
    // The header fields go up to 0x8F.
    if (buffer.size() < 0x90u) buffer.resize(0x90u, 0u);
    analyze_bytes(buffer.data(), valid, ctx);
}

void Mpeg::analyze_bytes(const std::uint8_t *b, std::uint32_t valid, Context &ctx) {
    kernel::GuestMemory &m = system_.kernel().memory();
    ctx.magic = read_le32(b);
    ctx.raw_version = read_le32(b + kPsmfStreamVersionOffset);
    ctx.version = mpeg_version(ctx.raw_version);
    ctx.offset = read_be32(b + kPsmfStreamOffsetOffset);
    ctx.stream_size = read_be32(b + kPsmfStreamSizeOffset);
    ctx.first_timestamp = mpeg_timestamp(b + kPsmfFirstTimestampOffset);
    ctx.last_timestamp = mpeg_timestamp(b + kPsmfLastTimestampOffset);
    ctx.avc_detail_frame_width = b[142] * 0x10;
    ctx.avc_detail_frame_height = b[143] * 0x10;
    ctx.avc_decode_result = kMpegAvcDecodeSuccess;
    ctx.avc_frame_status = 0;
    ctx.video_frame_count = 0;
    ctx.audio_frame_count = 0;
    ctx.end_of_audio_reached = false;
    ctx.end_of_video_reached = false;
    if (ctx.magic != kPsmfMagic || ctx.version < 0 || (ctx.offset & 2047u) != 0u ||
        ctx.offset == 0u)
        return;
    if (!ctx.analyzed && ctx.engine && ctx.stream_size > 0u && valid >= ctx.offset) {
        std::uint32_t ring_bytes = 0u;
        if (m.is_valid(ctx.ringbuffer_addr, ring::kSize))
            ring_bytes = m.read_u32(ctx.ringbuffer_addr + ring::kPackets) *
                         m.read_u32(ctx.ringbuffer_addr + ring::kPacketSize);
        ctx.engine->loadStream(b, static_cast<int>(ctx.offset), static_cast<int>(ring_bytes));
        ctx.analyzed = true;
    }
    std::memmove(ctx.header, b, std::min<std::size_t>(valid, sizeof(ctx.header)));
    // The engine gets the header alone later: its stream starts at 2048.
    const std::uint32_t header_offset = 0x80000u;
    std::memcpy(ctx.header + kPsmfStreamOffsetOffset, &header_offset, 4);
    system_.hle().trace_event("mpeg: stream at %u, %u bytes, timestamps %lld..%lld", ctx.offset,
                              ctx.stream_size, static_cast<long long>(ctx.first_timestamp),
                              static_cast<long long>(ctx.last_timestamp));
}

std::uint32_t Mpeg::ringbuffer_construct(std::uint32_t ring_addr, std::uint32_t packets,
                                         std::uint32_t data, std::uint32_t size,
                                         std::uint32_t callback, std::uint32_t callback_arg) {
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(ring_addr, ring::kSize)) return kErrIllegalAddress;
    if (static_cast<std::int32_t>(size) < 0) return kErrMpegNoMemory;
    if (packets * (104u + 2048u) > size) {
        // The firmware lets some through, due to a bug in its validation.
        if (packets < 0x00100000u) return kErrMpegNoMemory;
    }
    m.write_u32(ring_addr + ring::kPackets, packets);
    m.write_u32(ring_addr + ring::kPacketsRead, 0u);
    m.write_u32(ring_addr + ring::kPacketsWritePos, 0u);
    m.write_u32(ring_addr + ring::kPacketsAvail, 0u);
    m.write_u32(ring_addr + ring::kPacketSize, 2048u);
    m.write_u32(ring_addr + ring::kData, data);
    m.write_u32(ring_addr + ring::kCallbackAddr, callback);
    m.write_u32(ring_addr + ring::kCallbackArgs, callback_arg);
    m.write_u32(ring_addr + ring::kDataUpperBound, data + packets * 2048u);
    m.write_u32(ring_addr + ring::kMpeg, 0u);
    // Not in 0104, but in 0105: the caller module's $gp.
    if (lib_version_ >= 0x0105) {
        ModuleManager &modules = system_.modules();
        const Module *module = modules.find(static_cast<kernel::Uid>(modules.get_module_id()));
        m.write_u32(ring_addr + ring::kGp, module != nullptr ? module->gp : 0u);
    }
    return 0u;
}

std::uint32_t Mpeg::create(std::uint32_t mpeg_addr, std::uint32_t data, std::uint32_t size,
                           std::uint32_t ring_addr, std::uint32_t frame_width) {
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(mpeg_addr, 4u)) return kFailure;
    if (size < (lib_version_ < 0x0105 ? kMpegMemsize0104 : kMpegMemsize0105))
        return kErrMpegNoMemory;
    const bool ring_valid = m.is_valid(ring_addr, ring::kSize);
    if (ring_valid) {
        const std::uint32_t packet_size = m.read_u32(ring_addr + ring::kPacketSize);
        const std::uint32_t packets = m.read_u32(ring_addr + ring::kPackets);
        const std::uint32_t avail = packet_size == 0u
                                        ? 0u
                                        : packets - (m.read_u32(ring_addr + ring::kDataUpperBound) -
                                                     m.read_u32(ring_addr + ring::kData)) /
                                                        packet_size;
        m.write_u32(ring_addr + ring::kPacketsAvail, avail);
        m.write_u32(ring_addr + ring::kMpeg, mpeg_addr);
    }
    // The handle, written into the mpeg data, with a fake mpeg struct.
    const std::uint32_t handle = data + 0x30u;
    m.write_u32(mpeg_addr, handle);
    if (m.is_valid(handle, 24u)) {
        static constexpr std::uint8_t kLibMpeg[12] = {'L', 'I', 'B', 'M', 'P', 'E',
                                                      'G', 0,   '0', '0', '1', 0};
        (void)m.write_bytes(handle, kLibMpeg);
        m.write_u32(handle + 12u, 0xFFFFFFFFu);
        if (ring_valid) {
            m.write_u32(handle + 16u, ring_addr);
            m.write_u32(handle + 20u, m.read_u32(ring_addr + ring::kDataUpperBound));
        }
    }
    auto ctx = std::make_unique<Context>();
    ctx->ringbuffer_addr = ring_addr;
    ctx->default_frame_width = frame_width;
    ctx->engine = std::make_unique<video::MediaEngine>();
    if (audio_factory_) ctx->engine->setAudioDecoderFactory(audio_factory_);
    contexts_[handle] = std::move(ctx);
    return system_.kernel().delay_result(0u, 29000u);
}

void Mpeg::post_put(std::uint32_t ring_addr, std::int32_t added) {
    kernel::GuestMemory &m = system_.kernel().memory();
    if (added > 0) put_packets_added_ += added;
    Context *ctx = context(m.read_u32(ring_addr + ring::kMpeg));
    if (ctx == nullptr) return;
    const auto packets = static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPackets));
    const auto read = static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPacketsRead));
    const auto write_pos =
        static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPacketsWritePos));
    const auto avail = static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPacketsAvail));
    if (packets <= 0) return;
    const std::int32_t write_offset = write_pos % packets;
    if (read == 0 && added > 0) {
        // The first data: start the engine on the saved header (its stream
        // offset patched to 2048).
        std::uint8_t header[sizeof(ctx->header)];
        std::memcpy(header, ctx->header, sizeof(header));
        analyze_bytes(header, sizeof(header), *ctx);
        ctx->engine->loadStream(
            header, 2048,
            packets * static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPacketSize)));
    }
    if (added <= 0) return;
    if (added > packets - avail) added = packets - avail;
    const std::uint32_t data =
        m.read_u32(ring_addr + ring::kData) + static_cast<std::uint32_t>(write_offset) * 2048u;
    const std::vector<std::uint8_t> bytes =
        read_guest(m, data, static_cast<std::uint32_t>(added) * 2048u);
    (void)ctx->engine->addStreamData(bytes.data(), static_cast<int>(bytes.size()));
    m.write_u32(ring_addr + ring::kPacketsRead, static_cast<std::uint32_t>(read + added));
    m.write_u32(ring_addr + ring::kPacketsWritePos, static_cast<std::uint32_t>(write_pos + added));
    m.write_u32(ring_addr + ring::kPacketsAvail, static_cast<std::uint32_t>(avail + added));
}

std::uint32_t Mpeg::ringbuffer_put(std::uint32_t ring_addr, std::int32_t num_packets,
                                   std::int32_t available) {
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(ring_addr, ring::kSize)) return kFailure;
    const auto packets = static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPackets));
    num_packets = std::min(num_packets, available);
    // Games call sceMpegRingbufferAvailableSize first, but check anyway.
    num_packets =
        std::min(num_packets,
                 packets - static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPacketsAvail)));
    if (num_packets <= 0) return 0u;
    if (context(m.read_u32(ring_addr + ring::kMpeg)) == nullptr) return kFailure;
    const std::uint32_t callback = m.read_u32(ring_addr + ring::kCallbackAddr);
    if (callback == 0u) return 0u;

    put_packets_added_ = 0;
    std::int32_t last = 0;
    // Called until it has num_packets, in pieces that do not wrap the ring.
    std::int32_t write_offset =
        static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPacketsWritePos)) % packets;
    while (num_packets > 0) {
        const std::int32_t this_round = std::min(num_packets, packets - write_offset);
        num_packets -= this_round;
        const std::uint32_t args[3] = {
            m.read_u32(ring_addr + ring::kData) + static_cast<std::uint32_t>(write_offset) * 2048u,
            static_cast<std::uint32_t>(this_round), m.read_u32(ring_addr + ring::kCallbackArgs)};
        kernel::Kernel &k = system_.kernel();
        last = static_cast<std::int32_t>(
            system_.cpu().call_guest(k, k.current_thread(), callback, args));
        if (system_.runtime().stopped()) return 0u;
        post_put(ring_addr, last);
        write_offset = (write_offset + this_round) % packets;
    }
    return static_cast<std::uint32_t>(last < 0 && put_packets_added_ == 0 ? last
                                                                          : put_packets_added_);
}

std::uint32_t Mpeg::get_avc_au(std::uint32_t mpeg, std::uint32_t stream, std::uint32_t au_addr,
                               std::uint32_t attr) {
    kernel::Kernel &k = system_.kernel();
    kernel::GuestMemory &m = k.memory();
    Context *ctx = context(mpeg);
    if (ctx == nullptr) return kFailure;
    const std::uint32_t ring_addr = ctx->ringbuffer_addr;
    if (!m.is_valid(ring_addr, ring::kSize)) return kFailure;
    Au au = read_au(m, au_addr);
    if (m.read_u32(ring_addr + ring::kPacketsRead) == 0u ||
        m.read_u32(ring_addr + ring::kPacketsAvail) == 0u) {
        au.pts = -1;
        au.dts = -1;
        write_au(m, au_addr, au);
        return k.delay_result(kErrMpegNoData, kMpegDecodeErrorDelay);
    }
    const auto info = ctx->streams.find(stream);
    if (info == ctx->streams.end()) return kFailure;
    if (info->second.needs_reset) {
        au.pts = 0;
        info->second.needs_reset = false;
    }
    // esBuffer keeps the stream number (PPSSPP does not write the data).
    au.es_buffer = static_cast<std::uint32_t>(info->second.num);
    std::uint32_t result = 0u;
    au.pts = ctx->engine->getVideoTimeStamp() + ctx->first_timestamp;
    au.dts = au.pts - kVideoTimestampStep;
    if (ctx->engine->IsVideoEnd()) {
        if (!ctx->end_of_video_reached)
            system_.hle().trace_event("mpeg: video end at pts %lld after %d frames",
                                      static_cast<long long>(au.pts), ctx->video_frame_count);
        ctx->end_of_video_reached = true;
        m.write_u32(ring_addr + ring::kPacketsAvail, 0u);
        result = kErrMpegNoData;
    }
    write_au(m, au_addr, au);
    if (m.is_valid(attr, 4u)) m.write_u32(attr, 1u);
    return k.delay_result(result, 100u);
}

std::uint32_t Mpeg::get_atrac_au(std::uint32_t mpeg, std::uint32_t stream, std::uint32_t au_addr,
                                 std::uint32_t attr) {
    kernel::Kernel &k = system_.kernel();
    kernel::GuestMemory &m = k.memory();
    Context *ctx = context(mpeg);
    if (ctx == nullptr) return kFailure;
    const std::uint32_t ring_addr = ctx->ringbuffer_addr;
    if (!m.is_valid(ring_addr, ring::kSize)) return kFailure;
    Au au = read_au(m, au_addr);
    const auto info = ctx->streams.find(stream);
    if (info != ctx->streams.end() && info->second.needs_reset) {
        au.pts = 0;
        info->second.needs_reset = false;
    }
    // The audio can end earlier than the video does.
    if (m.read_u32(ring_addr + ring::kPacketsAvail) == 0u)
        return k.delay_result(kErrMpegNoData, kMpegDecodeErrorDelay);
    if (info != ctx->streams.end()) {
        au.es_buffer = static_cast<std::uint32_t>(info->second.num);
        (void)ctx->engine->setAudioStream(info->second.num);
    }
    std::uint32_t result = 0u;
    au.pts = ctx->engine->getAudioTimeStamp() + ctx->first_timestamp;
    if (ctx->engine->IsVideoEnd()) {
        m.write_u32(ring_addr + ring::kPacketsAvail, 0u);
        result = kErrMpegNoData;
    }
    if (ctx->atrac_registered && ctx->engine->IsNoAudioData() && !ctx->end_of_audio_reached)
        ctx->end_of_audio_reached = true;
    if (ctx->engine->IsNoAudioData()) result = kErrMpegNoData;
    write_au(m, au_addr, au);
    if (m.is_valid(attr, 4u)) m.write_u32(attr, 0u);
    return k.delay_result(result, 100u);
}

void Mpeg::write_video_image(Context &ctx, std::uint32_t buffer, std::uint32_t frame_width) {
    // MediaEngine::writeVideoImage: frameWidth pixels per line, the video's
    // height, swizzled when the destination is the swizzled VRAM mirror.
    kernel::GuestMemory &m = system_.kernel().memory();
    const bool swizzle = (buffer & 0x3F800000u) == 0x04000000u && (buffer & 0x00200000u) != 0u;
    const int video_height =
        ctx.engine->VideoHeight() > 0 ? ctx.engine->VideoHeight() : ctx.avc_detail_frame_height;
    const auto height = static_cast<std::uint32_t>(std::max(video_height, 0));
    const std::uint32_t lines = swizzle ? (height + 7u) / 8u * 8u : height;
    const std::uint32_t needed =
        frame_width * static_cast<std::uint32_t>(video::VideoPixelModeBytes(ctx.video_pixel_mode)) *
        lines;
    const std::uint32_t valid = clamp_valid(m, buffer, needed);
    std::uint8_t *dest = valid != 0u ? m.translate(buffer, valid) : nullptr;
    if (dest == nullptr) return;
    const int written =
        ctx.engine->writeVideoImage(std::span<std::uint8_t>(dest, valid), swizzle,
                                    static_cast<int>(frame_width), ctx.video_pixel_mode);
    if (written == 0 && !video::HaveVideoDecoder()) {
        // A build without FFmpeg has no image: black, as before F17b.
        m.fill(buffer, 0u, valid);
    }
}

std::uint32_t Mpeg::avc_decode(std::uint32_t mpeg, std::uint32_t au_addr, std::uint32_t frame_width,
                               std::uint32_t buffer_addr, std::uint32_t init_addr) {
    kernel::Kernel &k = system_.kernel();
    kernel::GuestMemory &m = k.memory();
    Context *ctx = context(mpeg);
    if (ctx == nullptr) return kFailure;
    if (frame_width == 0u) {
        frame_width = ctx->default_frame_width != 0u
                          ? ctx->default_frame_width
                          : static_cast<std::uint32_t>(ctx->avc_detail_frame_width);
    }
    Au au = read_au(m, au_addr);
    const std::uint32_t ring_addr = ctx->ringbuffer_addr;
    if (!m.is_valid(ring_addr, ring::kSize)) return kFailure;
    const std::uint32_t buffer = m.is_valid(buffer_addr, 4u) ? m.read_u32(buffer_addr) : 0u;
    if (m.read_u32(ring_addr + ring::kPacketsRead) == 0u || ctx->engine->IsVideoEnd())
        return k.delay_result(kErrMpegAvcDecodeFatal, kAvcEmptyDelay);

    const auto packets = static_cast<std::int32_t>(m.read_u32(ring_addr + ring::kPackets));
    (void)ctx->engine->setVideoStream(static_cast<int>(au.es_buffer));
    if (ctx->engine->stepVideo(ctx->video_pixel_mode)) {
        write_video_image(*ctx, buffer, frame_width);
        ctx->avc_frame_status = 1;
        ++ctx->video_frame_count;
        ++frames_;
    } else {
        ctx->avc_frame_status = 0;
    }
    // libmpeg > 0x0103: avail is what the engine still holds.
    m.write_u32(ring_addr + ring::kPacketsAvail,
                static_cast<std::uint32_t>(packets - ctx->engine->getRemainSize() / 2048));
    au.pts = ctx->engine->getVideoTimeStamp() + ctx->first_timestamp;
    write_au(m, au_addr, au);
    if (ctx->avc_frame_status != 0)
        system_.hle().trace_event("mpeg: video frame %d, pts %lld, to %08X", ctx->video_frame_count,
                                  static_cast<long long>(au.pts), buffer);
    if (m.is_valid(init_addr, 4u)) {
        // Killzone - Liberation expects 1 here (#16727).
        const bool killzone = lib_version_ >= 0x0105 && lib_version_ < 0x010A;
        m.write_u32(init_addr, killzone ? 1u : static_cast<std::uint32_t>(ctx->avc_frame_status));
    }
    ctx->avc_decode_result = kMpegAvcDecodeSuccess;
    return k.delay_result(0u, ctx->video_frame_count <= 1 ? kAvcFirstDelay : kAvcDecodeDelay);
}

std::uint32_t Mpeg::atrac_decode(std::uint32_t mpeg, std::uint32_t au_addr, std::uint32_t buffer) {
    kernel::Kernel &k = system_.kernel();
    kernel::GuestMemory &m = k.memory();
    Context *ctx = context(mpeg);
    if (ctx == nullptr) return kFailure;
    if (!m.is_valid(buffer, 1u)) return kFailure;
    Au au = read_au(m, au_addr);
    (void)ctx->engine->setAudioStream(static_cast<int>(au.es_buffer));
    m.fill(buffer, 0u, clamp_valid(m, buffer, kMpegAtracEsOutputSize));
    // MediaEngine::getAudioSamples: 2048 stereo samples. The PCM goes through
    // a host buffer (the guest one need not be 2-byte aligned), and is lost
    // if the guest buffer is shorter than 0x2000 bytes, like upstream.
    std::vector<std::int16_t> samples(kMpegAtracEsOutputSize / 2u, 0);
    const bool decoded =
        ctx->engine->getAudioSamples(samples) != 0 && m.is_valid(buffer, kMpegAtracEsOutputSize);
    if (decoded) {
        (void)m.write_bytes(buffer, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t *>(samples.data()),
                                        kMpegAtracEsOutputSize));
        ++audio_frames_;
    }
    au.pts = ctx->engine->getAudioTimeStamp() + ctx->first_timestamp;
    write_au(m, au_addr, au);
    if (decoded)
        system_.hle().trace_event("mpeg: audio frame, pts %lld, to %08X",
                                  static_cast<long long>(au.pts), buffer);
    return k.delay_result(0u, kAtracDecodeDelay);
}

void Mpeg::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = system_.kernel();
    GameClock &clock = system_.clock();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("sceMpeg", nid, name, HleKind::kImplemented, std::move(handler));
    };

    def(0x682A619Bu, "sceMpegInit", [this, &k](Ctx &c) {
        // PPSSPP does not return ALREADY_INIT: module load/unload is not hooked.
        initialized_ = true;
        ret(c, k.delay_result(0u, 750u));
    });
    def(0x874624D6u, "sceMpegFinish", [this, &k](Ctx &c) {
        initialized_ = false;
        ret(c, k.delay_result(0u, 250u));
    });
    def(0xD7A29F46u, "sceMpegRingbufferQueryMemSize",
        [](Ctx &c) { ret(c, arg(c, 0) * (104u + 2048u)); });
    def(0x37295ED8u, "sceMpegRingbufferConstruct", [this](Ctx &c) {
        ret(c,
            ringbuffer_construct(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)));
    });
    // Apparently does nothing.
    def(0x13407F13u, "sceMpegRingbufferDestruct", [](Ctx &c) { ret(c, 0u); });
    def(0xC132E22Fu, "sceMpegQueryMemSize",
        [this](Ctx &c) { ret(c, lib_version_ < 0x0105 ? kMpegMemsize0104 : kMpegMemsize0105); });
    def(0xD8C5F121u, "sceMpegCreate",
        [this](Ctx &c) { ret(c, create(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4))); });
    def(0x606A4649u, "sceMpegDelete", [this, &k](Ctx &c) {
        if (context(arg(c, 0)) == nullptr) return ret(c, kFailure);
        contexts_.erase(k.memory().read_u32(arg(c, 0)));
        ret(c, k.delay_result(0u, 40000u));
    });
    def(0x21FF80E4u, "sceMpegQueryStreamOffset", [this, &k](Ctx &c) {
        kernel::GuestMemory &m = k.memory();
        const std::uint32_t buffer = arg(c, 1);
        const std::uint32_t offset_addr = arg(c, 2);
        if (!m.is_valid(buffer, 1u) || !m.is_valid(offset_addr, 4u)) return ret(c, kFailure);
        Context *ctx = context(arg(c, 0));
        if (ctx == nullptr) return ret(c, kFailure);
        // Kinda destructive (PPSSPP): it analyzes and loads the stream.
        analyze(buffer, *ctx);
        std::uint32_t result = 0u;
        if (ctx->magic != kPsmfMagic)
            result = kErrMpegInvalidValue;
        else if (ctx->version < 0)
            result = kErrMpegBadVersion;
        else if ((ctx->offset & 2047u) != 0u || ctx->offset == 0u)
            result = kErrMpegInvalidValue;
        m.write_u32(offset_addr, result == 0u ? ctx->offset : 0u);
        ret(c, result);
    });
    def(0x611E9E11u, "sceMpegQueryStreamSize", [this, &k](Ctx &c) {
        kernel::GuestMemory &m = k.memory();
        const std::uint32_t buffer = arg(c, 0);
        const std::uint32_t size_addr = arg(c, 1);
        if (!m.is_valid(buffer, 1u) || !m.is_valid(size_addr, 4u)) return ret(c, kFailure);
        // A throwaway context that never loads the stream.
        Context ctx;
        analyze(buffer, ctx);
        std::uint32_t result = 0u;
        if (ctx.magic != kPsmfMagic || (ctx.offset & 2047u) != 0u) result = kErrMpegInvalidValue;
        m.write_u32(size_addr, result == 0u ? ctx.stream_size : 0u);
        ret(c, result);
    });
    def(0x42560F23u, "sceMpegRegistStream", [this](Ctx &c) {
        Context *ctx = context(arg(c, 0));
        if (ctx == nullptr) return ret(c, kFailure);
        const auto type = static_cast<int>(arg(c, 1));
        const auto num = static_cast<int>(arg(c, 2));
        switch (type) {
        case kMpegAvcStream:
            ctx->avc_registered = true;
            (void)ctx->engine->addVideoStream(num);
            (void)ctx->engine->setVideoStream(num);
            break;
        case kMpegAudioStream:
        case kMpegAtracStream:
            ctx->atrac_registered = true;
            (void)ctx->engine->setAudioStream(num);
            break;
        case kMpegPcmStream:
            ctx->pcm_registered = true;
            break;
        case kMpegDataStream:
            ctx->data_registered = true;
            break;
        default:
            break;
        }
        const std::uint32_t sid = next_stream_id_++;
        ctx->streams[sid] = StreamInfo{type, num, static_cast<int>(sid), true};
        ret(c, sid);
    });
    def(0x591A4AA2u, "sceMpegUnRegistStream", [this](Ctx &c) {
        Context *ctx = context(arg(c, 0));
        if (ctx == nullptr) return ret(c, kFailure);
        // PPSSPP looks at a fresh StreamInfo (type 0), not the registered one.
        ctx->avc_registered = false;
        ctx->streams[arg(c, 1)] = StreamInfo{};
        ctx->analyzed = false;
        ret(c, 0u);
    });
    def(0xA780CF7Eu, "sceMpegMallocAvcEsBuf", [this](Ctx &c) {
        Context *ctx = context(arg(c, 0));
        if (ctx == nullptr) return ret(c, kFailure);
        // Doesn't actually malloc, just keeps track of a couple of flags.
        for (int i = 0; i < kEsBuffers; ++i) {
            if (!ctx->es_buffers[i]) {
                ctx->es_buffers[i] = true;
                return ret(c, static_cast<std::uint32_t>(i + 1));
            }
        }
        ret(c, 0u);
    });
    def(0xCEB870B1u, "sceMpegFreeAvcEsBuf", [this](Ctx &c) {
        Context *ctx = context(arg(c, 0));
        if (ctx == nullptr) return ret(c, kFailure);
        const auto es_buffer = static_cast<std::int32_t>(arg(c, 1));
        if (es_buffer == 0) return ret(c, kErrMpegInvalidValue);
        if (es_buffer >= 1 && es_buffer <= kEsBuffers) ctx->es_buffers[es_buffer - 1] = false;
        ret(c, 0u);
    });
    def(0x167AFD9Eu, "sceMpegInitAu", [this, &k](Ctx &c) {
        Context *ctx = context(arg(c, 0));
        if (ctx == nullptr) return ret(c, kFailure);
        const std::uint32_t es_buffer = arg(c, 1);
        Au au = read_au(k.memory(), arg(c, 2));
        // The stream number goes in esBuffer later (GetAvcAu, GetAtracAu).
        au.es_buffer = 0u;
        au.pts = 0;
        if (es_buffer >= 1u && es_buffer <= static_cast<std::uint32_t>(kEsBuffers) &&
            ctx->es_buffers[es_buffer - 1u]) {
            // Allocated for AVC.
            au.es_size = kMpegAvcEsSize;
            au.dts = 0;
        } else {
            // Left as ATRAC.
            au.es_size = kMpegAtracEsSize;
            au.dts = kUnknownTimestamp;
        }
        write_au(k.memory(), arg(c, 2), au);
        ret(c, 0u);
    });
    def(0xF8DCB679u, "sceMpegQueryAtracEsSize", [this, &k](Ctx &c) {
        kernel::GuestMemory &m = k.memory();
        if (!m.is_valid(arg(c, 1), 4u) || !m.is_valid(arg(c, 2), 4u)) return ret(c, kFailure);
        if (context(arg(c, 0)) == nullptr) return ret(c, kFailure);
        m.write_u32(arg(c, 1), kMpegAtracEsSize);
        m.write_u32(arg(c, 2), kMpegAtracEsOutputSize);
        ret(c, 0u);
    });
    def(0xB5F6DC87u, "sceMpegRingbufferAvailableSize", [this, &k, &clock](Ctx &c) {
        kernel::GuestMemory &m = k.memory();
        const std::uint32_t ring_addr = arg(c, 0);
        if (!m.is_valid(ring_addr, ring::kSize)) return ret(c, kErrIllegalAddress);
        Context *ctx = context(m.read_u32(ring_addr + ring::kMpeg));
        if (ctx == nullptr) return ret(c, kErrMpegNotYetInit);
        ctx->ringbuffer_addr = ring_addr;
        clock.eat_cycles(2020u);
        ret(c,
            m.read_u32(ring_addr + ring::kPackets) - m.read_u32(ring_addr + ring::kPacketsAvail));
        k.preemption_point();
    });
    def(0xB240A59Eu, "sceMpegRingbufferPut", [this](Ctx &c) {
        const std::uint32_t result = ringbuffer_put(arg(c, 0), static_cast<std::int32_t>(arg(c, 1)),
                                                    static_cast<std::int32_t>(arg(c, 2)));
        ret(c, result);
    });
    def(0xFE246728u, "sceMpegGetAvcAu",
        [this](Ctx &c) { ret(c, get_avc_au(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))); });
    def(0xE1CE83A7u, "sceMpegGetAtracAu",
        [this](Ctx &c) { ret(c, get_atrac_au(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))); });
    def(0x0E3C2E9Du, "sceMpegAvcDecode", [this](Ctx &c) {
        ret(c, avc_decode(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
    });
    def(0x740FCCD1u, "sceMpegAvcDecodeStop", [this, &k](Ctx &c) {
        kernel::GuestMemory &m = k.memory();
        if (!m.is_valid(arg(c, 2), 1u) || !m.is_valid(arg(c, 3), 4u)) return ret(c, kFailure);
        if (context(arg(c, 0)) == nullptr) return ret(c, kFailure);
        // No last frame generated.
        m.write_u32(arg(c, 3), 0u);
        ret(c, 0u);
    });
    def(0x800C44DFu, "sceMpegAtracDecode",
        [this](Ctx &c) { ret(c, atrac_decode(arg(c, 0), arg(c, 1), arg(c, 2))); });
}

} // namespace mpo::hle
