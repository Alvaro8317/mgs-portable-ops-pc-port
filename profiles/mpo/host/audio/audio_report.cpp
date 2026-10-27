// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_report.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>

namespace mpo::audio::report {

namespace {

uint32_t Get32(const std::vector<uint8_t> &b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
uint16_t Get16(const std::vector<uint8_t> &b, size_t at) {
    return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

double ToDb(double power) {
    return power > 0.0 ? 10.0 * std::log10(power) : -200.0;
}

// 5 ms frames.
size_t FrameSize(int rate) {
    return static_cast<size_t>(std::max(1, rate / 200));
}

// RMS of each frame of `mono` (linear, full scale 1).
std::vector<double> Envelope(const float *mono, size_t count, size_t frame) {
    std::vector<double> env(count / frame);
    for (size_t f = 0; f < env.size(); ++f) {
        double sum = 0.0;
        const float *p = mono + f * frame;
        for (size_t i = 0; i < frame; ++i) sum += static_cast<double>(p[i]) * p[i];
        env[f] = std::sqrt(sum / static_cast<double>(frame));
    }
    return env;
}

double Pearson(const double *a, const double *b, size_t n) {
    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma;
        const double db = b[i] - mb;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    return saa > 0.0 && sbb > 0.0 ? sab / std::sqrt(saa * sbb) : 0.0;
}

double WaveCorrelation(const float *a, const float *b, size_t n) {
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sab += static_cast<double>(a[i]) * b[i];
        saa += static_cast<double>(a[i]) * a[i];
        sbb += static_cast<double>(b[i]) * b[i];
    }
    return saa > 0.0 && sbb > 0.0 ? sab / std::sqrt(saa * sbb) : 0.0;
}

void Fft(std::vector<std::complex<double>> &x) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = x[i + k];
                const std::complex<double> v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

constexpr size_t kSpectrumFrame = 2048;
constexpr size_t kBands = 24;

// Band levels (dB) of one Hann-windowed frame, log-spaced from 50 Hz to
// 20 kHz (or Nyquist).
std::vector<double> BandLevels(const float *p, int rate) {
    std::vector<std::complex<double>> x(kSpectrumFrame);
    for (size_t i = 0; i < kSpectrumFrame; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                                 static_cast<double>(kSpectrumFrame - 1));
        x[i] = {p[i] * hann, 0.0};
    }
    Fft(x);
    const double low = 50.0;
    const double high = std::min(20000.0, rate / 2.0);
    std::vector<double> bands(kBands, 0.0);
    for (size_t bin = 1; bin < kSpectrumFrame / 2; ++bin) {
        const double hz = static_cast<double>(bin) * rate / static_cast<double>(kSpectrumFrame);
        if (hz < low || hz >= high) continue;
        const double band = static_cast<double>(kBands) * std::log(hz / low) / std::log(high / low);
        bands[std::min(kBands - 1, static_cast<size_t>(band))] += std::norm(x[bin]);
    }
    for (double &band : bands) band = std::max(-100.0, ToDb(band));
    return bands;
}

} // namespace

bool ParseWav(const std::vector<uint8_t> &bytes, Pcm *out, std::string *error) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        *error = "not a RIFF/WAVE file";
        return false;
    }
    bool have_format = false;
    size_t channels = 0;
    int rate = 0, bits = 0, format = 0;
    size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const uint32_t size = Get32(bytes, at + 4);
        const size_t body = at + 8;
        if (size > bytes.size() - body) {
            *error = "chunk past the end of the file";
            return false;
        }
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0) {
            if (size < 16) {
                *error = "short fmt chunk";
                return false;
            }
            format = Get16(bytes, body);
            channels = Get16(bytes, body + 2);
            rate = static_cast<int>(Get32(bytes, body + 4));
            bits = Get16(bytes, body + 14);
            have_format = true;
        } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
            if (!have_format || format != 1 || bits != 16 || channels < 1 || channels > 8 ||
                rate < 1000 || rate > 384000) {
                *error = "only 16-bit PCM WAV (1-8 channels) is supported";
                return false;
            }
            const size_t count = size / 2 / channels * channels;
            std::vector<int16_t> samples(count);
            for (size_t i = 0; i < count; ++i)
                samples[i] = static_cast<int16_t>(Get16(bytes, body + 2 * i));
            *out = MakePcm(std::move(samples), channels, rate);
            return true;
        }
        at = body + size + (size & 1u);
    }
    *error = "no data chunk";
    return false;
}

bool ReadWav(const std::string &path, Pcm *out, std::string *error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "cannot read " + path;
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (!ParseWav(bytes, out, error)) {
        *error = path + ": " + *error;
        return false;
    }
    return true;
}

Pcm MakePcm(std::vector<int16_t> samples, size_t channels, int rate) {
    Pcm pcm;
    pcm.rate = rate;
    pcm.channels = channels;
    pcm.samples = std::move(samples);
    pcm.mono.resize(pcm.frames());
    for (size_t f = 0; f < pcm.mono.size(); ++f) {
        double sum = 0.0;
        for (size_t c = 0; c < channels; ++c) sum += pcm.samples[f * channels + c];
        pcm.mono[f] = static_cast<float>(sum / static_cast<double>(channels) / 32768.0);
    }
    return pcm;
}

Stats Analyze(const Pcm &pcm, const Options &options) {
    Stats stats;
    stats.seconds = pcm.seconds();
    if (pcm.samples.empty()) return stats;
    int peak = 0;
    double power = 0.0;
    for (const int16_t s : pcm.samples) {
        peak = std::max(peak, std::abs(static_cast<int>(s)));
        power += static_cast<double>(s) * s;
    }
    stats.peak_dbfs = peak > 0 ? 20.0 * std::log10(peak / 32768.0) : -200.0;
    stats.rms_dbfs = ToDb(power / static_cast<double>(pcm.samples.size()) / (32768.0 * 32768.0));

    const size_t frame = FrameSize(pcm.rate);
    const std::vector<double> env = Envelope(pcm.mono.data(), pcm.mono.size(), frame);
    size_t active = 0;
    for (const double e : env)
        if (ToDb(e * e) > options.silence_dbfs) ++active;
    stats.active_fraction =
        env.empty() ? 0.0 : static_cast<double>(active) / static_cast<double>(env.size());

    // Cuts, block by block from the start of the file (both the port's dump
    // and PPSSPP's begin on a mixer block).
    const size_t block = options.block * pcm.channels;
    const auto seconds = [&](size_t blocks_from_start) {
        return static_cast<double>(blocks_from_start * options.block) / pcm.rate;
    };
    const size_t blocks = pcm.samples.size() / block;
    const auto block_db = [&](size_t b) {
        double sum = 0.0;
        for (size_t i = 0; i < block; ++i) {
            const double s = pcm.samples[b * block + i] / 32768.0;
            sum += s * s;
        }
        return ToDb(sum / static_cast<double>(block));
    };
    const auto silent = [&](size_t b) {
        for (size_t i = 0; i < block; ++i)
            if (pcm.samples[b * block + i] != 0) return false;
        return true;
    };
    const double loud = options.silence_dbfs + 20.0;
    const size_t max_cut =
        static_cast<size_t>(0.25 * pcm.rate / static_cast<double>(options.block));
    for (size_t b = 1; b < blocks;) {
        if (!silent(b)) {
            if (std::memcmp(&pcm.samples[b * block], &pcm.samples[(b - 1) * block],
                            block * sizeof(int16_t)) == 0 &&
                block_db(b) > options.silence_dbfs) {
                stats.cuts.push_back({seconds(b), seconds(1), true});
            }
            ++b;
            continue;
        }
        size_t end = b;
        while (end < blocks && silent(end)) ++end;
        // Sound, a hole of at most 250 ms, and sound again.
        if (end < blocks && end - b <= max_cut && block_db(b - 1) > loud && block_db(end) > loud) {
            stats.cuts.push_back({seconds(b), seconds(end - b), false});
        }
        b = end;
    }
    return stats;
}

std::vector<Event> FindEvents(const Pcm &pcm, const Options &options) {
    std::vector<Event> events;
    const size_t frame = FrameSize(pcm.rate);
    const std::vector<double> env = Envelope(pcm.mono.data(), pcm.mono.size(), frame);
    const double frame_s = static_cast<double>(frame) / pcm.rate;
    const size_t gap_frames = static_cast<size_t>(options.event_gap_s / frame_s);
    size_t f = 0;
    while (f < env.size()) {
        if (ToDb(env[f] * env[f]) <= options.silence_dbfs) {
            ++f;
            continue;
        }
        const size_t start = f;
        size_t last = f;
        while (f < env.size() && f - last <= gap_frames) {
            if (ToDb(env[f] * env[f]) > options.silence_dbfs) last = f;
            ++f;
        }
        const double length = static_cast<double>(last + 1 - start) * frame_s;
        if (length >= options.min_event_s)
            events.push_back({static_cast<double>(start) * frame_s, length});
    }
    return events;
}

Match MatchEvent(const Pcm &test, const Event &event, const Pcm &ref, const Options &options) {
    Match match;
    if (test.rate != ref.rate || test.rate <= 0) return match;
    const size_t frame = FrameSize(test.rate);
    const size_t start = static_cast<size_t>(event.start_s * test.rate) / frame * frame;
    size_t count = static_cast<size_t>(std::min(event.length_s, options.max_event_s) * test.rate);
    count = std::min(count, test.mono.size() - std::min(start, test.mono.size()));
    count = count / frame * frame;
    if (count < frame * 4 || ref.mono.size() < count) return match;
    const float *seg = test.mono.data() + start;
    const std::vector<double> seg_env = Envelope(seg, count, frame);
    const std::vector<double> ref_env = Envelope(ref.mono.data(), ref.mono.size(), frame);
    if (ref_env.size() < seg_env.size()) return match;

    // Coarse: the envelope, frame by frame; keep the three best lags apart.
    struct Candidate {
        double score;
        size_t lag;
    };
    std::vector<Candidate> best;
    for (size_t lag = 0; lag + seg_env.size() <= ref_env.size(); ++lag) {
        const double score = Pearson(seg_env.data(), ref_env.data() + lag, seg_env.size());
        bool merged = false;
        for (Candidate &c : best) {
            if ((lag > c.lag ? lag - c.lag : c.lag - lag) < seg_env.size()) {
                if (score > c.score) c = {score, lag};
                merged = true;
                break;
            }
        }
        if (!merged) best.push_back({score, lag});
        std::sort(best.begin(), best.end(),
                  [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
        if (best.size() > 3) best.resize(3);
    }

    // Fine: the waveform, sample by sample around each candidate.
    for (const Candidate &c : best) {
        const size_t center = c.lag * frame;
        const size_t first = center > frame ? center - frame : 0;
        for (size_t lag = first; lag <= center + frame; ++lag) {
            if (lag + count > ref.mono.size()) break;
            const double corr = WaveCorrelation(seg, ref.mono.data() + lag, count);
            if (!match.found || corr > match.correlation) {
                match.found = true;
                match.correlation = corr;
                match.ref_start_s = static_cast<double>(lag) / ref.rate;
                match.envelope_correlation = c.score;
            }
        }
    }
    if (!match.found) return match;
    match.length_s = static_cast<double>(count) / test.rate;
    const float *r = ref.mono.data() + static_cast<size_t>(match.ref_start_s * ref.rate + 0.5);

    double pt = 0.0, pr = 0.0;
    for (size_t i = 0; i < count; ++i) {
        pt += static_cast<double>(seg[i]) * seg[i];
        pr += static_cast<double>(r[i]) * r[i];
    }
    match.level_diff_db = ToDb(pt) - ToDb(pr);

    double distance = 0.0;
    int frames = 0;
    for (size_t at = 0; at + kSpectrumFrame <= count; at += kSpectrumFrame) {
        double et = 0.0, er = 0.0;
        for (size_t i = 0; i < kSpectrumFrame; ++i) {
            et += static_cast<double>(seg[at + i]) * seg[at + i];
            er += static_cast<double>(r[at + i]) * r[at + i];
        }
        if (ToDb(et / kSpectrumFrame) <= options.silence_dbfs &&
            ToDb(er / kSpectrumFrame) <= options.silence_dbfs)
            continue;
        const std::vector<double> bt = BandLevels(seg + at, test.rate);
        const std::vector<double> br = BandLevels(r + at, ref.rate);
        double sum = 0.0;
        for (size_t b = 0; b < kBands; ++b) sum += std::abs(bt[b] - br[b]);
        distance += sum / static_cast<double>(kBands);
        ++frames;
    }
    match.spectral_distance_db = frames > 0 ? distance / frames : 0.0;
    return match;
}

} // namespace mpo::audio::report
