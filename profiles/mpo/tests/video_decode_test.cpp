// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_video with FFmpeg: a synthetic PSMF whose H.264 stream is made of
// I_PCM macroblocks goes through the whole path (demuxer, h264 parser,
// decoder, swscale) and the output pixels are checked against the BT.601
// conversion of the samples that were written. Exits 77 (skipped) when the
// build has no H.264 decoder.

#include <cstdlib>
#include <cstring>

#include "media_engine.h"
#include "video_test_util.h"

using namespace mpo::video;
using namespace mpo::video::test;

namespace {

constexpr int64_t kFirstTs = 3003 * 10;
constexpr int kMbW = 2;
constexpr int kMbH = 1;
constexpr int kW = kMbW * 16;
constexpr int kH = kMbH * 16;

// Two frames, two macroblocks each; the second frame swaps the colours.
const PcmColor kA = {100, 150, 110};
const PcmColor kB = {180, 100, 160};

std::vector<uint8_t> MakeVideoFile(int frames, bool withAudioStream) {
    std::vector<EsPayload> payloads;
    for (int i = 0; i < frames; ++i) {
        const std::vector<PcmColor> mbs =
            (i % 2 == 0) ? std::vector<PcmColor>{kA, kB} : std::vector<PcmColor>{kB, kA};
        payloads.push_back(
            {0xE0, 0, kFirstTs + i * 3003, MakePcmAccessUnit(kMbW, kMbH, mbs, i % 2, i == 0)});
    }
    const std::vector<uint8_t> ps = BuildProgramStream(payloads);
    std::vector<PsmfStreamSpec> streams = {{0xE0, 0, kW, kH, 2}};
    if (withAudioStream) {
        streams.push_back({0xBD, 0, 0, 0, 2});
    }
    std::vector<uint8_t> file = BuildPsmfHeader(static_cast<uint32_t>(ps.size()), kFirstTs,
                                                kFirstTs + (frames - 1) * 3003, streams);
    file.insert(file.end(), ps.begin(), ps.end());
    return file;
}

void FeedAll(MediaEngine &engine, const std::vector<uint8_t> &file) {
    for (size_t pos = 2048; pos < file.size(); pos += 2048) {
        VT_CHECK_EQ(engine.addStreamData(file.data() + pos, 2048), 2048);
    }
}

int Near(int a, int b, int tol) {
    return std::abs(a - b) <= tol;
}

// Checks the 8888 output (alpha cleared) at the centre of each macroblock.
void CheckFrame8888(const std::vector<uint8_t> &img, int stride, const PcmColor &left,
                    const PcmColor &right) {
    const PcmColor colors[2] = {left, right};
    for (int mb = 0; mb < 2; ++mb) {
        int r, g, b;
        Bt601ToRgb(colors[mb], &r, &g, &b);
        for (int y = 2; y < kH - 2; y += 5) {
            for (int x = mb * 16 + 4; x < mb * 16 + 12; x += 3) {
                const uint8_t *p =
                    img.data() + (static_cast<size_t>(y) * static_cast<size_t>(stride) +
                                  static_cast<size_t>(x)) *
                                     4;
                if (!Near(p[0], r, 2) || !Near(p[1], g, 2) || !Near(p[2], b, 2) || p[3] != 0) {
                    std::fprintf(stderr, "pixel (%d,%d) = %d,%d,%d,%d, want ~%d,%d,%d,0\n", x, y,
                                 p[0], p[1], p[2], p[3], r, g, b);
                    ++g_failures;
                    return;
                }
            }
        }
    }
}

void TestDecode() {
    const std::vector<uint8_t> file = MakeVideoFile(4, true);
    MediaEngine engine;
    VT_CHECK(engine.loadStream(file.data(), 2048, 64 * 2048));
    FeedAll(engine, file);

    std::vector<uint8_t> img(512 * kH * 4, 0xEE);
    for (int i = 0; i < 4; ++i) {
        VT_CHECK(engine.stepVideo(GE_CMODE_32BIT_ABGR8888));
        VT_CHECK_EQ(engine.VideoWidth(), kW);
        VT_CHECK_EQ(engine.VideoHeight(), kH);
        // Timestamps relative to the first one, one frame apart.
        VT_CHECK_EQ(engine.getVideoTimeStamp(), static_cast<int64_t>(i) * 3003);
        VT_CHECK_EQ(engine.writeVideoImage(img, false, 512, GE_CMODE_32BIT_ABGR8888), 512 * 4 * kH);
        if (i % 2 == 0) {
            CheckFrame8888(img, 512, kA, kB);
        } else {
            CheckFrame8888(img, 512, kB, kA);
        }
        // Past the picture width the destination line is not touched.
        VT_CHECK_EQ(img[(static_cast<size_t>(kW) + 1) * 4], 0xEE);
    }
    VT_CHECK(!engine.stepVideo(GE_CMODE_32BIT_ABGR8888));
    VT_CHECK(engine.IsVideoEnd());
    VT_CHECK_EQ(engine.stats().videoFramesDecoded, 4u);
    VT_CHECK_EQ(engine.stats().videoDecodeErrors, 0u);
    VT_CHECK_EQ(engine.stats().corruptFrames, 0u);
    // No audio in the file.
    VT_CHECK(engine.IsNoAudioData());

    // 16-bit modes: the GE formats with the PSP's cleared top bits.
    const int modes[3] = {GE_CMODE_16BIT_BGR5650, GE_CMODE_16BIT_ABGR5551, GE_CMODE_16BIT_ABGR4444};
    for (int mode : modes) {
        std::vector<uint8_t> out(512 * kH * 2, 0);
        VT_CHECK_EQ(engine.writeVideoImage(out, false, 512, mode), 512 * 2 * kH);
        int r, g, b;
        Bt601ToRgb(kB, &r, &g, &b); // last frame: kB on the left
        uint16_t px;
        std::memcpy(&px, out.data() + (static_cast<size_t>(8) * 512 + 8) * 2, 2);
        int pr = 0, pg = 0, pb = 0;
        if (mode == GE_CMODE_16BIT_BGR5650) {
            pr = (px & 0x1F) * 255 / 31;
            pg = ((px >> 5) & 0x3F) * 255 / 63;
            pb = (px >> 11) * 255 / 31;
        } else if (mode == GE_CMODE_16BIT_ABGR5551) {
            VT_CHECK_EQ(px & 0x8000, 0);
            pr = (px & 0x1F) * 255 / 31;
            pg = ((px >> 5) & 0x1F) * 255 / 31;
            pb = ((px >> 10) & 0x1F) * 255 / 31;
        } else {
            VT_CHECK_EQ(px & 0xF000, 0);
            pr = (px & 0xF) * 255 / 15;
            pg = ((px >> 4) & 0xF) * 255 / 15;
            pb = ((px >> 8) & 0xF) * 255 / 15;
        }
        const int tol = mode == GE_CMODE_16BIT_ABGR4444 ? 18 : 9;
        VT_CHECK(Near(pr, r, tol) && Near(pg, g, tol) && Near(pb, b, tol));
    }

    // Swizzled VRAM destination: same bytes as swizzling the linear image.
    std::vector<uint8_t> linear(512 * kH * 4);
    std::vector<uint8_t> swz(512 * kH * 4);
    VT_CHECK_EQ(engine.writeVideoImage(linear, false, 512, GE_CMODE_32BIT_ABGR8888), 512 * 4 * kH);
    VT_CHECK_EQ(engine.writeVideoImage(swz, true, 512, GE_CMODE_32BIT_ABGR8888), 512 * 4 * kH);
    std::vector<uint8_t> expect(linear.size());
    DoSwizzleTex16(linear.data(), expect.data(), 512 * 4 / 16, kH / 8, 512 * 4);
    VT_CHECK(swz == expect);

    // Range write: the right macroblock only, into an 8-pixel-wide target.
    std::vector<uint8_t> range(8 * 4 * 4, 0xEE);
    VT_CHECK_EQ(
        engine.writeVideoImageWithRange(range, false, 8, GE_CMODE_32BIT_ABGR8888, 20, 4, 8, 4),
        8 * 4 * 4);
    int r, g, b;
    Bt601ToRgb(kA, &r, &g, &b);
    VT_CHECK(Near(range[0], r, 2) && Near(range[1], g, 2) && Near(range[2], b, 2) && range[3] == 0);

    // Bad destinations.
    std::vector<uint8_t> small(100);
    VT_CHECK_EQ(engine.writeVideoImage(small, false, 512, GE_CMODE_32BIT_ABGR8888), 0);
    VT_CHECK_EQ(engine.writeVideoImage(img, false, 2049, GE_CMODE_32BIT_ABGR8888), 0);
    VT_CHECK_EQ(engine.writeVideoImage(img, false, 512, 7), 0);
    VT_CHECK_EQ(
        engine.writeVideoImageWithRange(range, false, 8, GE_CMODE_32BIT_ABGR8888, kW, 0, 8, 4), 0);
}

// Data arrives in small pieces and the decoder must wait for it; frames
// come out once the next access unit (or the end of the stream) shows up.
void TestStreaming() {
    const std::vector<uint8_t> file = MakeVideoFile(3, false);
    MediaEngine engine;
    VT_CHECK(engine.loadStream(file.data(), 2048, 64 * 2048));
    VT_CHECK(!engine.stepVideo(GE_CMODE_32BIT_ABGR8888));
    VT_CHECK(engine.IsVideoEnd());
    int frames = 0;
    for (size_t pos = 2048; pos < file.size(); pos += 512) {
        const int n = static_cast<int>(std::min<size_t>(512, file.size() - pos));
        VT_CHECK_EQ(engine.addStreamData(file.data() + pos, n), n);
        VT_CHECK(!engine.IsVideoEnd());
        while (engine.stepVideo(GE_CMODE_32BIT_ABGR8888)) {
            ++frames;
        }
    }
    VT_CHECK_EQ(frames, 3);
    VT_CHECK_EQ(engine.getVideoTimeStamp(), 2 * 3003);
}

// skipFrame and seekTo (upstream's "step until the timestamp" seek).
void TestSkipAndSeek() {
    const std::vector<uint8_t> file = MakeVideoFile(6, false);
    MediaEngine engine;
    VT_CHECK(engine.loadStream(file.data(), 2048, 64 * 2048));
    FeedAll(engine, file);
    // Upstream returns false when there is no audio to catch up with; the
    // video part of the seek still happens.
    VT_CHECK(!engine.seekTo(4 * 3003, GE_CMODE_32BIT_ABGR8888));
    VT_CHECK_EQ(engine.getVideoTimeStamp(), 3 * 3003);
    VT_CHECK(engine.stepVideo(GE_CMODE_32BIT_ABGR8888, true));
    VT_CHECK_EQ(engine.getVideoTimeStamp(), 4 * 3003);
    VT_CHECK(engine.stepVideo(GE_CMODE_32BIT_ABGR8888));
    std::vector<uint8_t> img(kW * kH * 4);
    VT_CHECK_EQ(engine.writeVideoImage(img, false, kW, GE_CMODE_32BIT_ABGR8888), kW * kH * 4);
    CheckFrame8888(img, kW, kB, kA); // frame 5 is odd: kB on the left
}

// A corrupt slice must not crash the decoder (OUTPUT_CORRUPT keeps going).
void TestCorrupt() {
    std::vector<uint8_t> file = MakeVideoFile(4, false);
    for (size_t i = 0x800 + 2048 + 200; i < file.size(); i += 97) {
        file[i] ^= 0x5A;
    }
    MediaEngine engine;
    VT_CHECK(engine.loadStream(file.data(), 2048, 64 * 2048));
    FeedAll(engine, file);
    int frames = 0;
    while (engine.stepVideo(GE_CMODE_32BIT_ABGR8888) && frames < 100) {
        ++frames;
    }
    VT_CHECK(engine.IsVideoEnd());
}

} // namespace

int main() {
    if (!HaveVideoDecoder()) {
        std::printf("mpo_video_decode_test: skipped (built without FFmpeg)\n");
        return 77;
    }
    std::printf("FFmpeg %s\n", VideoDecoderVersion());
    TestDecode();
    TestStreaming();
    TestSkipAndSeek();
    TestCorrupt();
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_video_decode_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_video_decode_test: all checks passed\n");
    return 0;
}
