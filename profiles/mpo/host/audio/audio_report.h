// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio_report (F16b): compares the audio the port plays (mpo_boot
// --audio-wav) against a reference recording of PPSSPP ("Dump audio": the
// same 44.1 kHz stereo mix, clamped), without the two runs being in step.
//
// - Stats of each file: length, peak and RMS level, how much of it sounds,
//   and the cuts: runs of digital silence (at least one 64-sample mixer
//   block) inside sound, and mixer blocks repeated exactly (what a starved
//   channel leaves on the PSP mixer, which keeps the previous block).
// - Events: the stretches of the test file that sound, split by 300 ms of
//   silence. Each one is looked up in the reference on its own (the two runs
//   trigger the same sound at different times): first by the RMS envelope
//   (5 ms frames), then to the sample by the waveform. A match reports the
//   waveform correlation, the envelope correlation, the level difference and
//   a spectral distance (mean absolute difference of band levels, in dB).
//
// The input is untrusted: ReadWav checks every chunk and only takes 16-bit
// PCM.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mpo::audio::report {

struct Pcm {
    int rate = 0;
    size_t channels = 0;
    std::vector<int16_t> samples; // Interleaved.
    // The mono downmix ((L + R) / 2), in [-1, 1].
    std::vector<float> mono;

    size_t frames() const { return channels > 0 ? samples.size() / channels : 0; }
    double seconds() const { return rate > 0 ? static_cast<double>(frames()) / rate : 0.0; }
};

bool ParseWav(const std::vector<uint8_t> &bytes, Pcm *out, std::string *error);
bool ReadWav(const std::string &path, Pcm *out, std::string *error);
// Builds a Pcm from interleaved samples (tests).
Pcm MakePcm(std::vector<int16_t> samples, size_t channels, int rate);

struct Options {
    double silence_dbfs = -60.0; // A 5 ms frame below this is silent.
    double event_gap_s = 0.3;    // Silence that splits two events.
    double min_event_s = 0.05;   // Shorter events are ignored.
    double max_event_s = 10.0;   // Longer events are matched on their first seconds.
    size_t block = 64;           // The PSP mixer's block (cuts and repeats).
};

struct Cut {
    double at_s = 0.0;
    double length_s = 0.0;
    bool repeat = false; // A repeated block rather than silence.
};

struct Stats {
    double seconds = 0.0;
    double peak_dbfs = -200.0;
    double rms_dbfs = -200.0;
    double active_fraction = 0.0; // 5 ms frames above silence_dbfs.
    std::vector<Cut> cuts;
};
Stats Analyze(const Pcm &pcm, const Options &options);

struct Event {
    double start_s = 0.0;
    double length_s = 0.0;
};
std::vector<Event> FindEvents(const Pcm &pcm, const Options &options);

struct Match {
    bool found = false;
    double ref_start_s = 0.0;
    double length_s = 0.0;    // What was compared.
    double correlation = 0.0; // Waveform, at the best sample lag.
    double envelope_correlation = 0.0;
    double level_diff_db = 0.0; // Test minus reference (RMS).
    double spectral_distance_db = 0.0;
};
Match MatchEvent(const Pcm &test, const Event &event, const Pcm &ref, const Options &options);

} // namespace mpo::audio::report
