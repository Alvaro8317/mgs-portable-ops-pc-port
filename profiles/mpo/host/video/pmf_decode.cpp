// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_pmf_decode: decodes PSMF videos (.pmf) with mpo_video the way sceMpeg
// would feed them (header first, then 2048-byte packets as the buffer frees
// up, audio pulled up to the video timestamp) and prints one summary line per
// file. Optionally dumps frames as PNG and the audio as a WAV. For local
// analysis of the user's own files; nothing it writes belongs in the repo.
//
// Usage:
//   mpo_pmf_decode [--out DIR] [--frames N,M,...] [--every K] [--wav]
//                  [--ring PACKETS] [--trace N] FILE.pmf...
//
// Exit code: 0 if every file decoded without errors, 1 otherwise, 2 on usage.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "image.hpp"
#include "media_engine.h"
#include "wav_writer.h"

namespace {

constexpr size_t kMaxFileSize = 512u << 20;
constexpr int kPacket = 2048;
constexpr int kDefaultRingPackets = 512; // a typical sceMpeg ring buffer

struct Options {
    std::filesystem::path outDir;
    std::set<int> frames;
    int every = 0;
    bool wav = false;
    int ringPackets = kDefaultRingPackets;
    int trace = 0; // print the engine state every N frames
    std::vector<std::string> files;
};

bool readFile(const std::string &path, std::vector<uint8_t> *out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxFileSize) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(out->data()), static_cast<std::streamsize>(out->size()));
    return static_cast<bool>(in) || in.gcount() == static_cast<std::streamsize>(out->size());
}

uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool decodeFile(const std::string &path, const Options &opt) {
    using mpo::video::MediaEngine;
    std::vector<uint8_t> data;
    if (!readFile(path, &data)) {
        std::printf("%s: cannot read (or larger than %zu bytes)\n", path.c_str(), kMaxFileSize);
        return false;
    }
    const std::string stem = std::filesystem::path(path).stem().string();
    if (data.size() < static_cast<size_t>(kPacket)) {
        std::printf("%s: too small for a PSMF header (%zu bytes)\n", path.c_str(), data.size());
        return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    MediaEngine engine;
    if (!engine.loadStream(data.data(), kPacket, opt.ringPackets * kPacket)) {
        std::printf("%s: loadStream failed\n", path.c_str());
        return false;
    }
    if (!engine.headerValid()) {
        std::printf("%s: bad PSMF header\n", path.c_str());
        return false;
    }
    const mpo::video::PsmfHeader &h = engine.header();
    if (h.streamOffset < static_cast<uint32_t>(kPacket) ||
        static_cast<uint64_t>(h.streamOffset) + h.streamSize > data.size()) {
        std::printf("%s: header stream range 0x%x+0x%x does not fit the file (%zu bytes)\n",
                    path.c_str(), h.streamOffset, h.streamSize, data.size());
        return false;
    }
    const size_t end = static_cast<size_t>(h.streamOffset) + h.streamSize;
    size_t pos = kPacket;

    std::vector<int16_t> pcm;
    std::vector<int16_t> block(2048 * 2);
    uint64_t videoHash = 0xcbf29ce484222325ULL;
    uint64_t audioHash = 0xcbf29ce484222325ULL;
    std::vector<uint8_t> image;
    int frames = 0;
    int dumped = 0;
    int audioBlocks = 0;
    int64_t firstFramePts = -1;
    int64_t lastFramePts = -1;
    int nonMonotonic = 0;
    int stalls = 0;
    std::string dumpError;

    auto refill = [&]() {
        while (pos < end && engine.getRemainSize() >= kPacket) {
            const int chunk = static_cast<int>(std::min<size_t>(kPacket, end - pos));
            if (engine.addStreamData(data.data() + pos, chunk) != chunk) {
                break;
            }
            pos += static_cast<size_t>(chunk);
        }
    };
    auto pullAudio = [&](int64_t untilPts, bool all) {
        while (all || engine.getAudioTimeStamp() + mpo::video::AUDIO_FRAME_DURATION_TS <= untilPts) {
            if (engine.IsNoAudioData()) {
                break;
            }
            std::fill(block.begin(), block.end(), int16_t{0});
            if (engine.getAudioSamples(block) == 0) {
                break;
            }
            ++audioBlocks;
            audioHash = fnv1a(audioHash, reinterpret_cast<const uint8_t *>(block.data()),
                              block.size() * sizeof(int16_t));
            if (opt.wav) {
                pcm.insert(pcm.end(), block.begin(), block.end());
            }
        }
    };

    refill();
    while (true) {
        if (!engine.stepVideo(mpo::video::GE_CMODE_32BIT_ABGR8888)) {
            if (pos < end) {
                const size_t before = pos;
                refill();
                pullAudio(engine.getVideoTimeStamp(), false);
                if (pos == before) {
                    // No room and no frame: drain audio to unblock the demuxer.
                    pullAudio(0, true);
                    refill();
                    if (pos == before && ++stalls > 64) {
                        break;
                    }
                }
                continue;
            }
            break;
        }
        const int64_t vpts = engine.getVideoTimeStamp();
        if (!mpo::video::HaveVideoDecoder() && vpts > engine.getLastTimeStamp()) {
            // Without FFmpeg stepVideo() never ends (upstream too): stop at the header end.
            break;
        }
        if (firstFramePts < 0) {
            firstFramePts = vpts;
        }
        if (lastFramePts >= 0 && vpts <= lastFramePts) {
            ++nonMonotonic;
            if (opt.trace > 0) {
                std::printf("  frame %d: pts %lld after %lld\n", frames, static_cast<long long>(vpts),
                            static_cast<long long>(lastFramePts));
            }
        }
        lastFramePts = vpts;

        const int w = engine.VideoWidth();
        const int hgt = engine.VideoHeight();
        image.assign(static_cast<size_t>(w) * static_cast<size_t>(hgt) * 4, 0);
        const int written = engine.writeVideoImage(image, false, w, mpo::video::GE_CMODE_32BIT_ABGR8888);
        if (written > 0) {
            videoHash = fnv1a(videoHash, image.data(), image.size());
        }
        const bool dump = !opt.outDir.empty() &&
                          (opt.frames.count(frames) != 0 || (opt.every > 0 && frames % opt.every == 0));
        if (dump && written > 0) {
            mpo::ge::replay::Image png;
            png.width = w;
            png.height = hgt;
            png.rgba = image;
            for (size_t i = 3; i < png.rgba.size(); i += 4) {
                png.rgba[i] = 255; // the PSP clears alpha; make the PNG opaque
            }
            char name[64];
            std::snprintf(name, sizeof(name), "_f%05d.png", frames);
            const std::string err = mpo::ge::replay::WritePng(opt.outDir / (stem + name), png);
            if (!err.empty()) {
                dumpError = err;
            } else {
                ++dumped;
            }
        }
        ++frames;
        pullAudio(vpts, false);
        refill();
        if (opt.trace > 0 && frames % opt.trace == 0) {
            const mpo::video::MpegDemux *a = engine.audioDemux();
            const mpo::video::MpegDemux *v = engine.videoDemux();
            std::printf("  f=%d v=%lld a=%lld aq=%d apend=%d vq=%zu vpend=%d noaudio=%d pos=%zu\n",
                        frames, static_cast<long long>(vpts),
                        static_cast<long long>(engine.getAudioTimeStamp()), a->audioQueuedBytes(),
                        a->getPendingSize(), v->videoQueuedBytes(), v->getPendingSize(),
                        engine.IsNoAudioData() ? 1 : 0, pos);
        }
    }
    pullAudio(0, true);
    const auto t1 = std::chrono::steady_clock::now();

    const mpo::video::MediaEngineStats &st = engine.stats();
    const mpo::video::MpegDemuxStats &as = engine.audioDemux()->stats();
    const mpo::video::MpegDemuxStats &vs = engine.videoDemux()->stats();
    if (opt.wav && !opt.outDir.empty() && !pcm.empty()) {
        if (!mpo::audio::WriteWav16((opt.outDir / (stem + ".wav")).string(), pcm, 2, 44100)) {
            dumpError = "cannot write the WAV";
        }
    }

    std::string streams;
    for (const auto &s : h.streams) {
        char buf[64];
        if (s.type == mpo::video::PSMF_AVC_STREAM) {
            std::snprintf(buf, sizeof(buf), "%sv%02X:%dx%d", streams.empty() ? "" : ",", s.streamId,
                          s.videoWidth, s.videoHeight);
        } else {
            std::snprintf(buf, sizeof(buf), "%sa%02X.%d:ch%d", streams.empty() ? "" : ",",
                          s.streamId, s.channel, s.audioChannels);
        }
        streams += buf;
    }
    const double headerSeconds =
        static_cast<double>(h.presentationEndTime - h.presentationStartTime) / 90000.0;
    const double videoSeconds = frames * mpo::video::VIDEO_FRAME_DURATION_TS / 90000.0;
    const double audioSeconds = audioBlocks * 2048 / 44100.0;
    const double wall = std::chrono::duration<double>(t1 - t0).count();
    const uint64_t droppedAudio = as.droppedAudioBytes + st.audioInputDropped;
    const uint64_t badPacks = as.badPackHeaders + vs.badPackHeaders;
    const bool ok = mpo::video::HaveVideoDecoder() && frames > 0 && st.videoDecodeErrors == 0 &&
                    st.corruptFrames == 0 &&
                    st.audioDecodeErrors == 0 && droppedAudio == 0 && vs.droppedVideoBytes == 0 &&
                    badPacks == 0 && pos == end && dumpError.empty();
    std::printf("%s: %s ver=%s off=0x%x streams=%s header=%.3fs frames=%d (%.3fs) "
                "au=%llu pts=[%lld..%lld] nonmono=%d audio=%d blocks (%.3fs, ch=%d, code1=0x%02x) "
                "errors v=%llu corrupt=%llu a=%llu dropped a=%llu v=%llu badpack=%llu "
                "%dx%d dumped=%d vhash=%016llx ahash=%016llx %.2fs%s%s\n",
                stem.c_str(), ok ? "OK" : "FAIL", h.VersionString().c_str(), h.streamOffset,
                streams.c_str(), headerSeconds, frames, videoSeconds,
                static_cast<unsigned long long>(st.videoPacketsSent),
                static_cast<long long>(firstFramePts), static_cast<long long>(lastFramePts),
                nonMonotonic, audioBlocks, audioSeconds, engine.audioChannels(),
                engine.audioHeaderCode1() & 0xFF, static_cast<unsigned long long>(st.videoDecodeErrors),
                static_cast<unsigned long long>(st.corruptFrames),
                static_cast<unsigned long long>(st.audioDecodeErrors),
                static_cast<unsigned long long>(droppedAudio),
                static_cast<unsigned long long>(vs.droppedVideoBytes),
                static_cast<unsigned long long>(badPacks), engine.VideoWidth(),
                engine.VideoHeight(), dumped, static_cast<unsigned long long>(videoHash),
                static_cast<unsigned long long>(audioHash), wall,
                dumpError.empty() ? "" : " dump error: ", dumpError.c_str());
    return ok;
}

int usage() {
    std::fprintf(stderr,
                 "usage: mpo_pmf_decode [--out DIR] [--frames N,M,...] [--every K] [--wav]\n"
                 "                      [--ring PACKETS] [--trace N] FILE.pmf...\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--out" && hasValue) {
            opt.outDir = argv[++i];
        } else if (arg == "--frames" && hasValue) {
            std::string list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty()) {
                    opt.frames.insert(std::atoi(item.c_str()));
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (arg == "--every" && hasValue) {
            opt.every = std::atoi(argv[++i]);
        } else if (arg == "--ring" && hasValue) {
            opt.ringPackets = std::max(16, std::atoi(argv[++i]));
        } else if (arg == "--trace" && hasValue) {
            opt.trace = std::atoi(argv[++i]);
        } else if (arg == "--wav") {
            opt.wav = true;
        } else if (!arg.empty() && arg[0] == '-') {
            return usage();
        } else {
            opt.files.push_back(arg);
        }
    }
    if (opt.files.empty()) {
        return usage();
    }
    if (!opt.outDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt.outDir, ec);
    }
    std::printf("decoder: FFmpeg %s (%s)\n", mpo::video::VideoDecoderVersion(),
                mpo::video::HaveVideoDecoder() ? "h264" : "no video decoder");
    int failed = 0;
    for (const std::string &file : opt.files) {
        if (!decodeFile(file, opt)) {
            ++failed;
        }
    }
    std::printf("%zu files, %d failed\n", opt.files.size(), failed);
    return failed == 0 ? 0 : 1;
}
