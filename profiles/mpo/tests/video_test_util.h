// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic PSMF files for the mpo_video tests: a PSMF header, MPEG-2
// program-stream packs with video PES and PSP-style ATRAC3plus private-stream
// PES, and a tiny H.264 stream made of I_PCM macroblocks (the decoded pixels
// are exactly the samples written, so the expected output is known).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace mpo::video::test {

inline int g_failures = 0;

#define VT_CHECK(cond)                                                                             \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++mpo::video::test::g_failures;                                                        \
        }                                                                                          \
    } while (0)

#define VT_CHECK_EQ(a, b)                                                                          \
    do {                                                                                           \
        const auto vt_a = (a);                                                                     \
        const auto vt_b = (b);                                                                     \
        if (!(vt_a == vt_b)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__,    \
                         __LINE__, #a, #b, static_cast<long long>(vt_a),                           \
                         static_cast<long long>(vt_b));                                            \
            ++mpo::video::test::g_failures;                                                        \
        }                                                                                          \
    } while (0)

inline void PutU32BE(std::vector<uint8_t> &v, size_t off, uint32_t x) {
    v[off] = static_cast<uint8_t>(x >> 24);
    v[off + 1] = static_cast<uint8_t>(x >> 16);
    v[off + 2] = static_cast<uint8_t>(x >> 8);
    v[off + 3] = static_cast<uint8_t>(x);
}

inline void PutTs(std::vector<uint8_t> &v, size_t off, int64_t ts) {
    for (int i = 0; i < 6; ++i) {
        // getMpegTimeStamp: buf[0] << 36 | buf[1] << 32 | buf[2] << 24 | ...
        static const int shifts[6] = {36, 32, 24, 16, 8, 0};
        v[off + static_cast<size_t>(i)] = static_cast<uint8_t>(ts >> shifts[i]);
    }
}

struct PsmfStreamSpec {
    int streamId = 0xE0; // 0xE0..0xEF video, 0xBD audio
    int privateId = 0;   // audio: channel (low nibble), 0x00 = ATRAC3plus
    int width = 480;     // video
    int height = 272;
    int channels = 2; // audio
    uint32_t epMapOffset = 0;
    uint32_t epMapEntries = 0;
};

struct EpEntry {
    int index = 0;
    int picOffset = 0;
    uint32_t pts = 0;
    uint32_t offset = 0;
};

// 0x800-byte PSMF header.
inline std::vector<uint8_t> BuildPsmfHeader(uint32_t streamSize, int64_t firstTs, int64_t lastTs,
                                            const std::vector<PsmfStreamSpec> &streams,
                                            const std::vector<EpEntry> &ep = {},
                                            uint32_t epOffset = 0x200) {
    std::vector<uint8_t> h(0x800, 0);
    h[0] = 'P';
    h[1] = 'S';
    h[2] = 'M';
    h[3] = 'F';
    h[4] = '0';
    h[5] = '0';
    h[6] = '1';
    h[7] = '5';
    PutU32BE(h, 0x8, 0x800);
    PutU32BE(h, 0xC, streamSize);
    PutU32BE(h, 0x50, 0x4E);
    PutTs(h, 0x54, firstTs);
    PutTs(h, 0x5A, lastTs);
    h[0x80] = static_cast<uint8_t>(streams.size() >> 8);
    h[0x81] = static_cast<uint8_t>(streams.size());
    for (size_t i = 0; i < streams.size(); ++i) {
        const size_t e = 0x82 + i * 16;
        const PsmfStreamSpec &s = streams[i];
        h[e] = static_cast<uint8_t>(s.streamId);
        h[e + 1] = static_cast<uint8_t>(s.privateId);
        if ((s.streamId & 0xE0) == 0xE0) {
            const bool withEp = !ep.empty();
            PutU32BE(h, e + 4, withEp ? epOffset : s.epMapOffset);
            PutU32BE(h, e + 8, withEp ? static_cast<uint32_t>(ep.size()) : s.epMapEntries);
            h[e + 12] = static_cast<uint8_t>(s.width / 16);
            h[e + 13] = static_cast<uint8_t>(s.height / 16);
        } else {
            h[e + 14] = static_cast<uint8_t>(s.channels);
            h[e + 15] = 2;
        }
    }
    for (size_t i = 0; i < ep.size(); ++i) {
        const size_t e = epOffset + i * 10;
        h[e] = static_cast<uint8_t>(ep[i].index);
        h[e + 1] = static_cast<uint8_t>(ep[i].picOffset);
        PutU32BE(h, e + 2, ep[i].pts);
        PutU32BE(h, e + 6, ep[i].offset);
    }
    return h;
}

// One elementary-stream payload to put in PES packets.
struct EsPayload {
    int streamId = 0xE0;  // 0xE0.. or 0xBD
    int audioChannel = 0; // private stream 1 channel byte
    int64_t pts = -1;     // -1: no PTS
    std::vector<uint8_t> data;
};

inline void AppendPts(std::vector<uint8_t> &out, int64_t pts) {
    out.push_back(static_cast<uint8_t>(0x21 | ((pts >> 29) & 0x0E)));
    out.push_back(static_cast<uint8_t>(pts >> 22));
    out.push_back(static_cast<uint8_t>(((pts >> 14) & 0xFE) | 1));
    out.push_back(static_cast<uint8_t>(pts >> 7));
    out.push_back(static_cast<uint8_t>(((pts << 1) & 0xFE) | 1));
}

// Lays the payloads out as 2048-byte packs: pack header, PES packets (split
// across packs when needed; only the first piece carries the PTS) and a
// padding packet to fill each pack.
inline std::vector<uint8_t> BuildProgramStream(const std::vector<EsPayload> &payloads) {
    constexpr size_t kPack = 2048;
    static const uint8_t packHeader[14] = {0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x05,
                                           0x3D, 0x1D, 0x11, 0x01, 0x86, 0xA3, 0xF8};
    std::vector<uint8_t> out;
    for (const EsPayload &p : payloads) {
        size_t pos = 0;
        bool first = true;
        while (pos < p.data.size() || (first && p.data.empty())) {
            std::vector<uint8_t> pack(packHeader, packHeader + 14);
            std::vector<uint8_t> pesHeader;
            const bool withPts = first && p.pts >= 0;
            pesHeader.push_back(0x81);
            pesHeader.push_back(withPts ? 0x80 : 0x00);
            pesHeader.push_back(withPts ? 5 : 0);
            if (withPts) {
                AppendPts(pesHeader, p.pts);
            }
            if (p.streamId == 0xBD) {
                // PSP private stream: channel byte + 3 more header bytes.
                pesHeader.push_back(static_cast<uint8_t>(p.audioChannel));
                pesHeader.push_back(0x00);
                pesHeader.push_back(0x00);
                pesHeader.push_back(0x00);
            }
            // 4 start code + 2 length, and leave room for a padding packet (6+).
            const size_t room = kPack - pack.size() - 6 - pesHeader.size() - 8;
            const size_t take = std::min(room, p.data.size() - pos);
            const size_t pesLen = pesHeader.size() + take;
            pack.push_back(0x00);
            pack.push_back(0x00);
            pack.push_back(0x01);
            pack.push_back(static_cast<uint8_t>(p.streamId));
            pack.push_back(static_cast<uint8_t>(pesLen >> 8));
            pack.push_back(static_cast<uint8_t>(pesLen));
            pack.insert(pack.end(), pesHeader.begin(), pesHeader.end());
            pack.insert(pack.end(), p.data.begin() + static_cast<std::ptrdiff_t>(pos),
                        p.data.begin() + static_cast<std::ptrdiff_t>(pos + take));
            pos += take;
            first = false;
            // Padding stream to the end of the pack.
            const size_t padLen = kPack - pack.size() - 6;
            pack.push_back(0x00);
            pack.push_back(0x00);
            pack.push_back(0x01);
            pack.push_back(0xBE);
            pack.push_back(static_cast<uint8_t>(padLen >> 8));
            pack.push_back(static_cast<uint8_t>(padLen));
            pack.insert(pack.end(), padLen, 0xFF);
            out.insert(out.end(), pack.begin(), pack.end());
            if (p.data.empty()) {
                break;
            }
        }
    }
    return out;
}

// An ATRAC3plus frame as the PSP muxes it: 0x0F 0xD0 code1 code2 + 4 bytes,
// then frameSize - 8 payload bytes, with frameSize = ((code1 & 3) << 8 |
// code2 * 8) + 0x10.
inline std::vector<uint8_t> MakeAudioFrame(uint8_t code1, uint8_t code2, uint8_t fill) {
    const int frameSize = (((code1 & 0x03) << 8) | (code2 * 8)) + 0x10;
    std::vector<uint8_t> f(static_cast<size_t>(frameSize), 0);
    f[0] = 0x0F;
    f[1] = 0xD0;
    f[2] = code1;
    f[3] = code2;
    for (size_t i = 8; i < f.size(); ++i) {
        f[i] = static_cast<uint8_t>(fill + i);
        if (f[i] == 0x0F) {
            f[i] = 0x10; // never a fake frame header inside the payload
        }
    }
    return f;
}

// --- H.264 (baseline, CAVLC, I_PCM macroblocks) ------------------------------

class BitWriter {
public:
    void bit(uint32_t b) {
        cur_ = static_cast<uint8_t>((static_cast<uint32_t>(cur_) << 1) | (b & 1));
        if (++n_ == 8) {
            bytes_.push_back(cur_);
            cur_ = 0;
            n_ = 0;
        }
    }
    void bits(uint32_t v, int count) {
        for (int i = count - 1; i >= 0; --i) {
            bit((v >> i) & 1);
        }
    }
    void ue(uint32_t v) {
        const uint32_t x = v + 1;
        int len = 0;
        while ((x >> len) > 1) {
            ++len;
        }
        bits(0, len);
        bits(x, len + 1);
    }
    void se(int32_t v) {
        ue(v <= 0 ? static_cast<uint32_t>(-2 * v) : static_cast<uint32_t>(2 * v - 1));
    }
    bool aligned() const { return n_ == 0; }
    void alignZero() {
        while (n_ != 0) {
            bit(0);
        }
    }
    void trailing() {
        bit(1);
        alignZero();
    }
    const std::vector<uint8_t> &bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    uint8_t cur_ = 0;
    int n_ = 0;
};

// Start code + NAL header + RBSP with emulation prevention.
inline void AppendNal(std::vector<uint8_t> &out, int refIdc, int type,
                      const std::vector<uint8_t> &rbsp) {
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
    out.push_back(static_cast<uint8_t>((refIdc << 5) | type));
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

struct PcmColor {
    uint8_t y, cb, cr;
};

// One access unit: (SPS, PPS,) IDR slice with one I_PCM macroblock per entry
// of `mbs` (raster order) for a picture of mbW x mbH macroblocks.
inline std::vector<uint8_t> MakePcmAccessUnit(int mbW, int mbH, const std::vector<PcmColor> &mbs,
                                              int idrPicId, bool withParamSets) {
    std::vector<uint8_t> au;
    if (withParamSets) {
        BitWriter sps;
        sps.bits(66, 8); // profile_idc: baseline
        sps.bits(0, 8);  // constraint flags
        sps.bits(30, 8); // level_idc
        sps.ue(0);       // seq_parameter_set_id
        sps.ue(0);       // log2_max_frame_num_minus4
        sps.ue(2);       // pic_order_cnt_type
        sps.ue(1);       // max_num_ref_frames
        sps.bit(0);      // gaps_in_frame_num_value_allowed_flag
        sps.ue(static_cast<uint32_t>(mbW - 1));
        sps.ue(static_cast<uint32_t>(mbH - 1));
        sps.bit(1); // frame_mbs_only_flag
        sps.bit(1); // direct_8x8_inference_flag
        sps.bit(0); // frame_cropping_flag
        sps.bit(0); // vui_parameters_present_flag
        sps.trailing();
        AppendNal(au, 3, 7, sps.bytes());

        BitWriter pps;
        pps.ue(0);      // pic_parameter_set_id
        pps.ue(0);      // seq_parameter_set_id
        pps.bit(0);     // entropy_coding_mode_flag (CAVLC)
        pps.bit(0);     // bottom_field_pic_order_in_frame_present_flag
        pps.ue(0);      // num_slice_groups_minus1
        pps.ue(0);      // num_ref_idx_l0_default_active_minus1
        pps.ue(0);      // num_ref_idx_l1_default_active_minus1
        pps.bit(0);     // weighted_pred_flag
        pps.bits(0, 2); // weighted_bipred_idc
        pps.se(0);      // pic_init_qp_minus26
        pps.se(0);      // pic_init_qs_minus26
        pps.se(0);      // chroma_qp_index_offset
        pps.bit(1);     // deblocking_filter_control_present_flag
        pps.bit(0);     // constrained_intra_pred_flag
        pps.bit(0);     // redundant_pic_cnt_present_flag
        pps.trailing();
        AppendNal(au, 3, 8, pps.bytes());
    }
    BitWriter s;
    s.ue(0);      // first_mb_in_slice
    s.ue(7);      // slice_type: I (all slices)
    s.ue(0);      // pic_parameter_set_id
    s.bits(0, 4); // frame_num (log2_max_frame_num = 4)
    s.ue(static_cast<uint32_t>(idrPicId));
    s.bit(0); // no_output_of_prior_pics_flag
    s.bit(0); // long_term_reference_flag
    s.se(0);  // slice_qp_delta
    s.ue(1);  // disable_deblocking_filter_idc
    for (const PcmColor &c : mbs) {
        s.ue(25); // mb_type: I_PCM
        s.alignZero();
        for (int i = 0; i < 256; ++i) {
            s.bits(c.y, 8);
        }
        for (int i = 0; i < 64; ++i) {
            s.bits(c.cb, 8);
        }
        for (int i = 0; i < 64; ++i) {
            s.bits(c.cr, 8);
        }
    }
    s.trailing();
    AppendNal(au, 3, 5, s.bytes());
    return au;
}

// BT.601 limited range, as swscale with srcRange = 0 does it.
inline void Bt601ToRgb(PcmColor c, int *r, int *g, int *b) {
    const double y = 1.164383 * (c.y - 16);
    const double cb = c.cb - 128.0;
    const double cr = c.cr - 128.0;
    auto clamp = [](double v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<int>(v + 0.5)); };
    *r = clamp(y + 1.596027 * cr);
    *g = clamp(y - 0.391762 * cb - 0.812968 * cr);
    *b = clamp(y + 2.017232 * cb);
}

} // namespace mpo::video::test
