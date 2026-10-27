// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_audio_report (F16b) on synthetic signals: the WAV parser (and what it
// rejects), the cuts (a hole of digital silence inside a tone, a repeated
// mixer block), the events, and the matching of an event that sits at an
// arbitrary sample of the reference, at the same level and at half of it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

#include "audio_report.h"

namespace {

namespace rep = mpo::audio::report;

int g_failures = 0;
int g_checks = 0;

void Expect(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
    }
}

constexpr int kRate = 44100;

// A 440 Hz stereo tone of `frames` frames at -12 dBFS.
std::vector<int16_t> Tone(size_t frames) {
    std::vector<int16_t> out(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const auto v = static_cast<int16_t>(8192.0 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    return out;
}

// Deterministic broadband sound with a slow envelope (an LCG through a
// one-pole filter), so the envelope and the waveform both have one best lag.
std::vector<int16_t> Texture(size_t frames, uint32_t seed) {
    std::vector<int16_t> out(frames * 2);
    uint32_t state = seed;
    double low = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        const double white = static_cast<double>(static_cast<int32_t>(state)) / 2147483648.0;
        low = 0.9 * low + 0.1 * white;
        const double t = static_cast<double>(i) / kRate;
        const double env = 0.55 + 0.45 * std::sin(2.0 * std::numbers::pi * 1.3 * t);
        const auto v = static_cast<int16_t>(20000.0 * env * (0.5 * white + low));
        out[2 * i] = v;
        out[2 * i + 1] = static_cast<int16_t>(v / 2);
    }
    return out;
}

std::vector<uint8_t> WavBytes(const std::vector<int16_t> &samples, uint16_t channels,
                              uint16_t bits) {
    std::vector<uint8_t> b;
    const auto put32 = [&b](uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    const auto put16 = [&b](uint16_t v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    };
    const auto data = static_cast<uint32_t>(samples.size() * 2);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(36 + data);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(16);
    put16(1);
    put16(channels);
    put32(kRate);
    put32(static_cast<uint32_t>(kRate * channels * 2));
    put16(static_cast<uint16_t>(channels * 2));
    put16(bits);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(data);
    for (const int16_t s : samples) put16(static_cast<uint16_t>(s));
    return b;
}

void TestParse() {
    const std::vector<int16_t> samples = Tone(1000);
    rep::Pcm pcm;
    std::string error;
    Expect(rep::ParseWav(WavBytes(samples, 2, 16), &pcm, &error), "a 16-bit stereo WAV parses");
    Expect(pcm.rate == kRate && pcm.channels == 2 && pcm.frames() == 1000, "rate and frames");
    Expect(pcm.samples == samples, "the samples come back");
    Expect(std::abs(pcm.mono[250] - samples[500] / 32768.0f) < 1e-6f, "mono downmix");

    std::vector<uint8_t> bad = WavBytes(samples, 2, 16);
    bad[0] = 'X';
    Expect(!rep::ParseWav(bad, &pcm, &error), "not RIFF: rejected");
    Expect(!rep::ParseWav(WavBytes(samples, 2, 8), &pcm, &error), "8-bit: rejected");
    std::vector<uint8_t> truncated = WavBytes(samples, 2, 16);
    truncated.resize(truncated.size() / 2);
    Expect(!rep::ParseWav(truncated, &pcm, &error), "a data chunk past the end: rejected");
    Expect(!rep::ParseWav(std::vector<uint8_t>(8, 0), &pcm, &error), "too short: rejected");
}

void TestCuts() {
    const rep::Options options;
    const rep::Pcm clean = rep::MakePcm(Tone(kRate), 2, kRate);
    const rep::Stats stats = rep::Analyze(clean, options);
    Expect(stats.cuts.empty(), "a clean tone has no cuts");
    Expect(std::abs(stats.rms_dbfs - (20.0 * std::log10(8192.0 / 32768.0) - 3.01)) < 0.1,
           "the tone's RMS level");
    Expect(stats.active_fraction > 0.99, "the tone sounds all along");

    // Three silent mixer blocks at block 300, and block 500 repeated.
    std::vector<int16_t> holed = Tone(kRate);
    std::fill(holed.begin() + 300 * 128, holed.begin() + 303 * 128, int16_t{0});
    std::memcpy(&holed[501 * 128], &holed[500 * 128], 128 * sizeof(int16_t));
    const rep::Stats cut = rep::Analyze(rep::MakePcm(holed, 2, kRate), options);
    Expect(cut.cuts.size() == 2, "a hole and a repeat: two cuts");
    if (cut.cuts.size() == 2) {
        Expect(!cut.cuts[0].repeat && std::abs(cut.cuts[0].at_s - 300.0 * 64 / kRate) < 1e-9 &&
                   std::abs(cut.cuts[0].length_s - 3.0 * 64 / kRate) < 1e-9,
               "the hole: where and how long");
        Expect(cut.cuts[1].repeat && std::abs(cut.cuts[1].at_s - 501.0 * 64 / kRate) < 1e-9,
               "the repeated block");
    }

    // A sound that ends and another that starts: silence, not a cut.
    std::vector<int16_t> two(Tone(kRate / 2));
    std::vector<int16_t> gap(static_cast<size_t>(kRate) / 2 * 2, 0);
    two.insert(two.end(), gap.begin(), gap.end());
    const std::vector<int16_t> tail = Tone(kRate / 2);
    two.insert(two.end(), tail.begin(), tail.end());
    const rep::Pcm events_pcm = rep::MakePcm(two, 2, kRate);
    Expect(rep::Analyze(events_pcm, options).cuts.empty(), "500 ms of silence is not a cut");
    const std::vector<rep::Event> events = rep::FindEvents(events_pcm, options);
    Expect(events.size() == 2, "two events");
    if (events.size() == 2) {
        Expect(events[0].start_s < 0.01 && std::abs(events[0].length_s - 0.5) < 0.01,
               "the first event");
        Expect(std::abs(events[1].start_s - 1.0) < 0.01 &&
                   std::abs(events[1].length_s - 0.5) < 0.01,
               "the second event");
    }
}

void TestMatch() {
    const rep::Options options;
    const std::vector<int16_t> ref_samples = Texture(5 * kRate, 1234u);
    const rep::Pcm ref = rep::MakePcm(ref_samples, 2, kRate);
    constexpr size_t kAt = 103457;    // 2.346 s, not on any frame boundary.
    constexpr size_t kLength = 35280; // 0.8 s
    for (const double gain : {1.0, 0.5}) {
        std::vector<int16_t> test(static_cast<size_t>(kRate) * 2, 0);
        for (size_t i = 0; i < kLength * 2; ++i)
            test.push_back(static_cast<int16_t>(ref_samples[kAt * 2 + i] * gain));
        test.resize(test.size() + static_cast<size_t>(kRate) * 2, 0);
        const rep::Pcm pcm = rep::MakePcm(test, 2, kRate);
        const std::vector<rep::Event> events = rep::FindEvents(pcm, options);
        Expect(events.size() == 1, "one event in the test");
        if (events.empty()) continue;
        const rep::Match m = rep::MatchEvent(pcm, events[0], ref, options);
        // The event starts on its first 5 ms frame (220 samples), a little
        // before the copy: the match starts as much before it in the reference.
        const auto start = static_cast<size_t>(events[0].start_s * kRate + 0.5) / 220 * 220;
        const double expected =
            static_cast<double>(kAt - (static_cast<size_t>(kRate) - start)) / kRate;
        Expect(m.found, "the event is found in the reference");
        Expect(std::abs(m.ref_start_s - expected) < 0.5 / kRate,
               "to the sample (" + std::to_string(m.ref_start_s) + " vs " +
                   std::to_string(expected) + ")");
        // Not 1: the event starts with the end of the silence before the copy.
        Expect(m.correlation > 0.99, "waveform correlation " + std::to_string(m.correlation));
        Expect(m.envelope_correlation > 0.99, "envelope correlation");
        Expect(std::abs(m.level_diff_db - 20.0 * std::log10(gain)) < 0.05,
               "level difference " + std::to_string(m.level_diff_db));
        Expect(m.spectral_distance_db < 0.2 + std::abs(20.0 * std::log10(gain)),
               "spectral distance " + std::to_string(m.spectral_distance_db));
    }

    // Something else entirely does not correlate.
    const rep::Pcm other = rep::MakePcm(Texture(kRate, 999u), 2, kRate);
    const rep::Match none = rep::MatchEvent(other, {0.0, 0.8}, ref, options);
    Expect(none.found && none.correlation < 0.2, "an unrelated sound does not match");
}

} // namespace

int main() {
    TestParse();
    TestCuts();
    TestMatch();
    std::printf("audio_report_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
