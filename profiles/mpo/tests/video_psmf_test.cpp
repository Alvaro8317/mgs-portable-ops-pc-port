// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_video without FFmpeg: PSMF header parsing, the MPEG-PS demuxer (video
// PES and PSP ATRAC3plus private stream), BufferQueue, the GE swizzle and the
// MediaEngine audio path with a fake ATRAC decoder. Synthetic data only.

#include <cstring>
#include <memory>
#include <random>
#include <span>

#include "buffer_queue.h"
#include "media_engine.h"
#include "mpeg_demux.h"
#include "psmf.h"
#include "video_test_util.h"

using namespace mpo::video;
using namespace mpo::video::test;

namespace {

constexpr int64_t kFirstTs = 90000;

struct SyntheticFile {
    std::vector<uint8_t> bytes;
    std::vector<std::vector<uint8_t>> videoPayloads;
    std::vector<int64_t> videoPts;
    std::vector<std::vector<uint8_t>> audioFrames; // whole frames, with the 8-byte header
    std::vector<int64_t> audioPts;                 // per PES (one frame per PES here)
};

// Three video PES (one bigger than a pack) and four audio frames.
SyntheticFile MakeFile(uint8_t audioCode1 = 0x28) {
    SyntheticFile f;
    std::vector<EsPayload> payloads;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> v(i == 1 ? size_t{3000} : size_t{200} + static_cast<size_t>(i) * 50);
        for (size_t j = 0; j < v.size(); ++j) {
            v[j] = static_cast<uint8_t>(static_cast<size_t>(0x40 + i) + j * 7);
        }
        f.videoPayloads.push_back(v);
        f.videoPts.push_back(kFirstTs + i * 3003);
        payloads.push_back({0xE0, 0, kFirstTs + i * 3003, v});
        // Interleave one audio frame after each video PES, plus one at the end.
        std::vector<uint8_t> a = MakeAudioFrame(audioCode1, 0x5C, static_cast<uint8_t>(i * 16));
        f.audioFrames.push_back(a);
        f.audioPts.push_back(kFirstTs + i * 4180);
        payloads.push_back({0xBD, 0, kFirstTs + i * 4180, a});
    }
    std::vector<uint8_t> a = MakeAudioFrame(audioCode1, 0x5C, 0x70);
    f.audioFrames.push_back(a);
    f.audioPts.push_back(kFirstTs + 3 * 4180);
    payloads.push_back({0xBD, 0, kFirstTs + 3 * 4180, a});
    // A second video stream that must be ignored when E0 is selected.
    payloads.push_back({0xE1, 0, kFirstTs, std::vector<uint8_t>(100, 0x99)});

    const std::vector<uint8_t> ps = BuildProgramStream(payloads);
    const std::vector<EpEntry> ep = {{0, 1, static_cast<uint32_t>(kFirstTs), 0},
                                     {0, 2, static_cast<uint32_t>(kFirstTs + 6006), 4096}};
    f.bytes = BuildPsmfHeader(static_cast<uint32_t>(ps.size()), kFirstTs, kFirstTs + 3 * 3003,
                              {{0xE0, 0, 480, 272, 2}, {0xBD, 0, 0, 0, 2}, {0xE1, 0, 480, 272, 2}}, ep);
    f.bytes.insert(f.bytes.end(), ps.begin(), ps.end());
    return f;
}

void TestHeader() {
    const SyntheticFile f = MakeFile();
    PsmfHeader h;
    VT_CHECK(ParsePsmfHeader(f.bytes, &h) == PsmfError::None);
    VT_CHECK_EQ(h.magic, PSMF_MAGIC);
    VT_CHECK(h.VersionString() == "0015");
    VT_CHECK_EQ(h.streamOffset, 0x800u);
    VT_CHECK_EQ(h.streamSize, f.bytes.size() - 0x800);
    VT_CHECK_EQ(h.presentationStartTime, static_cast<uint32_t>(kFirstTs));
    VT_CHECK_EQ(h.presentationEndTime, static_cast<uint32_t>(kFirstTs + 3 * 3003));
    VT_CHECK_EQ(h.numStreams, 3);
    VT_CHECK_EQ(h.streams.size(), 3u);
    VT_CHECK_EQ(h.streams[0].type, PSMF_AVC_STREAM);
    VT_CHECK_EQ(h.streams[0].videoWidth, 480);
    VT_CHECK_EQ(h.streams[0].videoHeight, 272);
    VT_CHECK_EQ(h.streams[1].type, PSMF_ATRAC_STREAM);
    VT_CHECK_EQ(h.streams[1].audioChannels, 2);
    VT_CHECK_EQ(h.streams[1].audioFrequency, 2);
    VT_CHECK_EQ(h.streams[2].channel, 1);
    VT_CHECK_EQ(h.CountStreams(PSMF_AVC_STREAM), 2);
    VT_CHECK_EQ(h.CountStreams(PSMF_AUDIO_STREAM), 1);
    VT_CHECK_EQ(h.FindStreamWithTypeNumber(PSMF_AVC_STREAM, 1), 2);
    VT_CHECK_EQ(h.FindStreamWithTypeNumber(PSMF_AVC_STREAM, 2), -1);
    VT_CHECK_EQ(h.FindStreamWithTypeChannel(PSMF_AUDIO_STREAM, 0), 1);
    VT_CHECK_EQ(h.EPMap.size(), 2u);
    VT_CHECK_EQ(h.EPMap[1].EPPicOffset, 2);
    VT_CHECK_EQ(h.EPMap[1].EPOffset, 4096);
    VT_CHECK_EQ(h.FindEPWithTimestamp(static_cast<int>(kFirstTs + 6006)), 1); // exact
    VT_CHECK_EQ(h.FindEPWithTimestamp(static_cast<int>(kFirstTs + 5000)), 0); // closest below
    VT_CHECK_EQ(h.FindEPWithTimestamp(static_cast<int>(kFirstTs - 1)), -1);
    VT_CHECK_EQ(GetMpegTimeStamp(f.bytes.data() + PSMF_LAST_TIMESTAMP_OFFSET), kFirstTs + 3 * 3003);

    // A private stream id with high bits set is PCM (upstream's assumption).
    std::vector<uint8_t> pcm = BuildPsmfHeader(0, 0, 0, {{0xBD, 0x12, 0, 0, 1}});
    VT_CHECK(ParsePsmfHeader(pcm, &h) == PsmfError::None);
    VT_CHECK_EQ(h.streams[0].type, PSMF_PCM_STREAM);
    VT_CHECK_EQ(h.streams[0].channel, 2);

    // Errors.
    VT_CHECK(ParsePsmfHeader(std::span<const uint8_t>(f.bytes.data(), 0x81), &h) == PsmfError::TooSmall);
    std::vector<uint8_t> bad = f.bytes;
    bad[0] = 'X';
    VT_CHECK(ParsePsmfHeader(bad, &h) == PsmfError::BadMagic);
    std::vector<uint8_t> many = f.bytes;
    many[0x80] = 0xFF; // 65280+ streams cannot fit
    VT_CHECK(ParsePsmfHeader(std::span<const uint8_t>(many.data(), 0x800), &h) == PsmfError::BadStreamCount);
    // An EP map that runs past the buffer is cut to the entries that fit.
    // (The map comes from the last video stream entry, here 0xE1.)
    std::vector<uint8_t> ep = f.bytes;
    PutU32BE(ep, 0x82 + 2 * 16 + 8, 1000);
    VT_CHECK(ParsePsmfHeader(std::span<const uint8_t>(ep.data(), 0x800), &h) == PsmfError::None);
    VT_CHECK_EQ(h.EPMapEntriesNum, (0x800u - 0x200u) / 10u);
    PutU32BE(ep, 0x82 + 2 * 16 + 4, 0xFFFFFFF0u);
    VT_CHECK(ParsePsmfHeader(std::span<const uint8_t>(ep.data(), 0x800), &h) == PsmfError::None);
    VT_CHECK_EQ(h.EPMap.size(), 0u);
}

// Demuxes everything with the given chunking and checks the outputs.
void CheckDemux(const SyntheticFile &f, size_t chunk, const char *what) {
    MpegDemux demux(1 << 20, 0x800);
    for (size_t pos = 0; pos < f.bytes.size(); pos += chunk) {
        const size_t n = std::min(chunk, f.bytes.size() - pos);
        if (!demux.addStreamData(f.bytes.data() + pos, static_cast<int>(n))) {
            std::fprintf(stderr, "%s: addStreamData failed\n", what);
            ++g_failures;
            return;
        }
        demux.demux(-1, 0xE0);
    }
    demux.demux(-1, 0xE0);

    // Video: payloads concatenated per PES (the big one spans two packs).
    std::vector<uint8_t> got;
    std::vector<int64_t> pts;
    VideoChunk c;
    while (demux.popVideoChunk(&c)) {
        got.insert(got.end(), c.data.begin(), c.data.end());
        if (c.pts >= 0) {
            pts.push_back(c.pts);
        }
    }
    std::vector<uint8_t> want;
    for (const auto &v : f.videoPayloads) {
        want.insert(want.end(), v.begin(), v.end());
    }
    VT_CHECK(got == want);
    VT_CHECK(pts == f.videoPts);
    VT_CHECK_EQ(demux.stats().badPackHeaders, 0u);

    // Audio: one frame at a time, payload without the 8-byte header.
    for (size_t i = 0; i < f.audioFrames.size(); ++i) {
        uint8_t *buf = nullptr;
        int code1 = 0;
        int code2 = 0;
        int64_t apts = 0;
        const int size = demux.getNextAudioFrame(&buf, &code1, &code2, &apts);
        VT_CHECK_EQ(size, static_cast<int>(f.audioFrames[i].size()) - 8);
        if (size > 0) {
            VT_CHECK(std::memcmp(buf, f.audioFrames[i].data() + 8, static_cast<size_t>(size)) == 0);
        }
        VT_CHECK_EQ(code1, f.audioFrames[i][2]);
        VT_CHECK_EQ(code2, 0x5C);
        VT_CHECK_EQ(apts, f.audioPts[i]);
    }
    VT_CHECK_EQ(demux.getNextAudioFrame(nullptr, nullptr, nullptr), 0);
    VT_CHECK_EQ(demux.audioChannel(), 0);
}

void TestDemux() {
    const SyntheticFile f = MakeFile();
    CheckDemux(f, f.bytes.size(), "whole");
    CheckDemux(f, 2048, "packets");
    CheckDemux(f, 7, "7-byte chunks");     // start codes split across chunks
    CheckDemux(f, 333, "333-byte chunks");

    // Header offset larger than the first chunk: the skip carries over.
    MpegDemux demux(1 << 20, 0x800);
    VT_CHECK(demux.addStreamData(f.bytes.data(), 0x100));
    demux.demux(-1, 0xE0);
    VT_CHECK(demux.addStreamData(f.bytes.data() + 0x100, static_cast<int>(f.bytes.size() - 0x100)));
    demux.demux(-1, 0xE0);
    VT_CHECK_EQ(demux.videoQueuedChunks(), 4u);

    // No video selected: nothing queued, audio still works.
    MpegDemux audioOnly(1 << 20, 0x800);
    VT_CHECK(audioOnly.addStreamData(f.bytes.data(), static_cast<int>(f.bytes.size())));
    audioOnly.demux(-1, -1);
    VT_CHECK_EQ(audioOnly.videoQueuedChunks(), 0u);
    VT_CHECK(audioOnly.hasNextAudioFrame(nullptr, nullptr, nullptr, nullptr));

    // A full buffer refuses data instead of overflowing.
    MpegDemux small(4096, 0x800);
    VT_CHECK(small.addStreamData(f.bytes.data(), 4096));
    VT_CHECK(!small.addStreamData(f.bytes.data(), 1));
    VT_CHECK(!small.addStreamData(f.bytes.data(), -1));
}

void TestCorruptData() {
    // Random bytes, truncated packets and lying length fields must not read
    // out of bounds (ASan) or loop forever.
    std::mt19937 rng(1234);
    const SyntheticFile f = MakeFile();
    for (int round = 0; round < 300; ++round) {
        std::vector<uint8_t> data = f.bytes;
        const int flips = 1 + static_cast<int>(rng() % 64);
        for (int i = 0; i < flips; ++i) {
            const size_t at = 0x800 + rng() % (data.size() - 0x800);
            data[at] = static_cast<uint8_t>(rng());
        }
        data.resize(0x800 + rng() % (data.size() - 0x800));
        MpegDemux demux(static_cast<int>(data.size()) + 2048, 0x800);
        demux.addStreamData(data.data(), static_cast<int>(data.size()));
        demux.demux(-1, 0xE0);
        VideoChunk c;
        while (demux.popVideoChunk(&c)) {
        }
        for (int i = 0; i < 16; ++i) {
            demux.getNextAudioFrame(nullptr, nullptr, nullptr);
        }
    }
    std::vector<uint8_t> noise(70000);
    for (auto &b : noise) {
        b = static_cast<uint8_t>(rng());
    }
    MpegDemux demux(80000, 0);
    VT_CHECK(demux.addStreamData(noise.data(), static_cast<int>(noise.size())));
    demux.demux(-1, 0xE0);
    PsmfHeader h;
    for (size_t n = 0; n < 0x100; ++n) {
        ParsePsmfHeader(std::span<const uint8_t>(f.bytes.data(), n), &h);
    }
}

void TestBufferQueue() {
    BufferQueue q(16);
    const uint8_t a[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    VT_CHECK(q.push(a, 10, 111));
    uint8_t out[16] = {};
    int64_t pts = 0;
    VT_CHECK_EQ(q.pop_front(out, 6, &pts), 6);
    VT_CHECK_EQ(pts, 111);
    // Wraps around the end of the ring.
    const uint8_t b[10] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    VT_CHECK(q.push(b, 10, 222));
    VT_CHECK(!q.push(b, 3)); // 14 + 3 > 16
    VT_CHECK_EQ(q.getQueueSize(), 14);
    VT_CHECK_EQ(q.get_front(out, 16), 14);
    VT_CHECK_EQ(out[0], 7);
    VT_CHECK_EQ(out[4], 11);
    VT_CHECK_EQ(out[13], 20);
    VT_CHECK_EQ(q.pop_front(nullptr, 4, &pts), 4);
    VT_CHECK_EQ(pts, 0); // no mark inside 7..10
    VT_CHECK_EQ(q.pop_front(out, 100, &pts), 10);
    VT_CHECK_EQ(pts, 222);
    VT_CHECK_EQ(out[9], 20);
    VT_CHECK_EQ(q.getQueueSize(), 0);
}

void TestSwizzle() {
    // 32 bytes per line, 8 lines: two 16x8 blocks.
    std::vector<uint8_t> src(32 * 8);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> dst(src.size());
    DoSwizzleTex16(src.data(), dst.data(), 2, 1, 32);
    // Block 0 = bytes 0..15 of each row, then block 1 = bytes 16..31.
    VT_CHECK_EQ(dst[0], 0);
    VT_CHECK_EQ(dst[16], 32);
    VT_CHECK_EQ(dst[16 * 7 + 15], 32 * 7 + 15);
    VT_CHECK_EQ(dst[128], 16);
    VT_CHECK_EQ(dst[255], 255);
}

// Records what the media engine hands to the ATRAC decoder.
struct FakeAtrac final : mpo::audio::AudioDecoder {
    std::vector<std::vector<uint8_t>> *frames;
    explicit FakeAtrac(std::vector<std::vector<uint8_t>> *f) : frames(f) {}
    bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels,
                int16_t *outbuf, int *outSamples) override {
        frames->emplace_back(inbuf, inbuf + inbytes);
        *inbytesConsumed = inbytes;
        if (outbuf) {
            for (int i = 0; i < 2048 * outputChannels; ++i) {
                outbuf[i] = static_cast<int16_t>(frames->size());
            }
        }
        *outSamples = 2048;
        return true;
    }
    bool IsOK() const override { return true; }
};

void TestMediaEngineAudio(uint8_t code1, int expectChannels) {
    const SyntheticFile f = MakeFile(code1);
    std::vector<std::vector<uint8_t>> decoded;
    int created = 0;
    int createdChannels = 0;
    uint32_t createdCodec = 0;
    MediaEngine engine;
    engine.setAudioDecoderFactory([&](uint32_t codec, int, int channels) {
        ++created;
        createdChannels = channels;
        createdCodec = codec;
        return std::unique_ptr<mpo::audio::AudioDecoder>(new FakeAtrac(&decoded));
    });
    VT_CHECK(engine.loadStream(f.bytes.data(), 2048, 64 * 2048));
    VT_CHECK(engine.headerValid());
    VT_CHECK_EQ(engine.getLastTimeStamp(), 3 * 3003);
    for (size_t pos = 2048; pos < f.bytes.size(); pos += 2048) {
        VT_CHECK_EQ(engine.addStreamData(f.bytes.data() + pos, 2048), 2048);
    }
    VT_CHECK(!engine.IsNoAudioData());
    std::vector<int16_t> pcm(4096);
    for (size_t i = 0; i < f.audioFrames.size(); ++i) {
        VT_CHECK_EQ(engine.getAudioSamples(pcm), 0x2000);
        VT_CHECK_EQ(pcm[0], static_cast<int16_t>(i + 1));
        // getAudioTimeStamp() is the pts of the frame just returned.
        VT_CHECK_EQ(engine.getAudioTimeStamp(), f.audioPts[i] - kFirstTs);
    }
    VT_CHECK(engine.IsNoAudioData());
    VT_CHECK_EQ(engine.getAudioSamples(pcm), 0);
    VT_CHECK_EQ(created, 1);
    VT_CHECK_EQ(createdChannels, expectChannels);
    VT_CHECK_EQ(createdCodec, 0x1000u);
    VT_CHECK_EQ(decoded.size(), f.audioFrames.size());
    for (size_t i = 0; i < decoded.size() && i < f.audioFrames.size(); ++i) {
        VT_CHECK(decoded[i] == std::vector<uint8_t>(f.audioFrames[i].begin() + 8, f.audioFrames[i].end()));
    }
    // A too-small output buffer still consumes the frame (upstream logs it).
    VT_CHECK(engine.loadStream(f.bytes.data(), 2048, 64 * 2048));
    for (size_t pos = 2048; pos < f.bytes.size(); pos += 2048) {
        engine.addStreamData(f.bytes.data() + pos, 2048);
    }
    std::vector<int16_t> tiny(16, 7);
    VT_CHECK_EQ(engine.getAudioSamples(tiny), 0x2000);
    VT_CHECK_EQ(tiny[0], 7);
}

void TestMediaEngineEdges() {
    MediaEngine engine;
    VT_CHECK(!engine.loadStream(nullptr, 0, 0));
    std::vector<uint8_t> tiny(0x40, 0);
    VT_CHECK(!engine.loadStream(tiny.data(), static_cast<int>(tiny.size()), 4096));
    VT_CHECK_EQ(engine.getAudioTimeStamp(), -1);
    std::vector<int16_t> pcm(4096);
    VT_CHECK_EQ(engine.getAudioSamples(pcm), 0);
    VT_CHECK(engine.IsNoAudioData());
    // A bad magic still loads (upstream lets FFmpeg probe), with stream 0xE0.
    SyntheticFile f = MakeFile();
    f.bytes[0] = 'X';
    VT_CHECK(engine.loadStream(f.bytes.data(), 2048, 64 * 2048));
    VT_CHECK(!engine.headerValid());
    VT_CHECK(engine.setVideoStream(0));
    VT_CHECK(!engine.setVideoStream(1));
    VT_CHECK(engine.addVideoStream(1));
    VT_CHECK(engine.setVideoStream(1));
    // Without a decoded frame nothing is written.
    std::vector<uint8_t> out(512 * 272 * 4);
    VT_CHECK_EQ(engine.writeVideoImage(out, false, 512, GE_CMODE_32BIT_ABGR8888), 0);
    VT_CHECK_EQ(engine.writeVideoImage(out, false, 4096, GE_CMODE_32BIT_ABGR8888), 0);
}

} // namespace

int main() {
    TestHeader();
    TestDemux();
    TestCorruptData();
    TestBufferQueue();
    TestSwizzle();
    TestMediaEngineAudio(0x28, 2);
    TestMediaEngineAudio(0x24, 1); // mono header code
    TestMediaEngineEdges();
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_video_psmf_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_video_psmf_test: all checks passed\n");
    return 0;
}
