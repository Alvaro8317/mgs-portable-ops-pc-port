// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_audio_report (F16b): the port's audio against PPSSPP's (audio_report.h).
//
//   mpo_audio_report <test.wav> [--ref <reference.wav>]... [--min-envelope 0.9]
//                    [--max-level-db 1.5] [--max-spectral-db 3] [--max-event-s 10]
//                    [--silence-db -60]
//
// Prints the stats of every file and, for each sound event of the test file,
// its best match among the references. Exit code 0 when the test file has no
// cuts and, with references, every event matches within the thresholds; 1
// otherwise; 2 on a usage or read error.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio_report.h"

namespace {

namespace rep = mpo::audio::report;

void PrintStats(const char *role, const std::string &path, const rep::Stats &stats) {
    int silent_cuts = 0;
    int repeats = 0;
    for (const rep::Cut &cut : stats.cuts) (cut.repeat ? repeats : silent_cuts)++;
    std::printf("%s %s: %.2f s, peak %.1f dBFS, rms %.1f dBFS, %.1f%% sounding, %d cuts, %d "
                "repeated blocks\n",
                role, path.c_str(), stats.seconds, stats.peak_dbfs, stats.rms_dbfs,
                100.0 * stats.active_fraction, silent_cuts, repeats);
    int shown = 0;
    for (const rep::Cut &cut : stats.cuts) {
        if (++shown > 10) {
            std::printf("    ...\n");
            break;
        }
        std::printf("    %s at %.3f s (%.1f ms)\n", cut.repeat ? "repeated block" : "cut", cut.at_s,
                    1000.0 * cut.length_s);
    }
}

} // namespace

int main(int argc, char **argv) {
    std::string test_path;
    std::vector<std::string> refs;
    rep::Options options;
    double min_envelope = 0.9;
    double max_level_db = 1.5;
    double max_spectral_db = 3.0;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const bool has_value = i + 1 < argc;
        if (option == "--ref" && has_value) {
            refs.push_back(argv[++i]);
        } else if (option == "--min-envelope" && has_value) {
            min_envelope = std::atof(argv[++i]);
        } else if (option == "--max-level-db" && has_value) {
            max_level_db = std::atof(argv[++i]);
        } else if (option == "--max-spectral-db" && has_value) {
            max_spectral_db = std::atof(argv[++i]);
        } else if (option == "--max-event-s" && has_value) {
            options.max_event_s = std::atof(argv[++i]);
        } else if (option == "--silence-db" && has_value) {
            options.silence_dbfs = std::atof(argv[++i]);
        } else if (option.rfind("--", 0) != 0 && test_path.empty()) {
            test_path = option;
        } else {
            std::fprintf(stderr, "usage: mpo_audio_report <test.wav> [--ref <ref.wav>]... "
                                 "(see audio_report_main.cpp)\n");
            return 2;
        }
    }
    if (test_path.empty()) {
        std::fprintf(stderr, "mpo_audio_report: no test file\n");
        return 2;
    }

    std::string error;
    rep::Pcm test;
    if (!rep::ReadWav(test_path, &test, &error)) {
        std::fprintf(stderr, "mpo_audio_report: %s\n", error.c_str());
        return 2;
    }
    const rep::Stats test_stats = rep::Analyze(test, options);
    PrintStats("test", test_path, test_stats);
    bool ok = test_stats.cuts.empty();

    std::vector<rep::Pcm> ref_pcm(refs.size());
    for (size_t r = 0; r < refs.size(); ++r) {
        if (!rep::ReadWav(refs[r], &ref_pcm[r], &error)) {
            std::fprintf(stderr, "mpo_audio_report: %s\n", error.c_str());
            return 2;
        }
        PrintStats("ref ", refs[r], rep::Analyze(ref_pcm[r], options));
    }

    const std::vector<rep::Event> events = rep::FindEvents(test, options);
    std::printf("\n%zu events in the test file\n", events.size());
    int matched = 0;
    for (const rep::Event &event : events) {
        std::printf("  event at %8.3f s, %6.3f s:", event.start_s, event.length_s);
        if (refs.empty()) {
            std::printf("\n");
            continue;
        }
        rep::Match best;
        size_t best_ref = 0;
        for (size_t r = 0; r < refs.size(); ++r) {
            const rep::Match m = rep::MatchEvent(test, event, ref_pcm[r], options);
            if (m.found && (!best.found || m.correlation > best.correlation)) {
                best = m;
                best_ref = r;
            }
        }
        if (!best.found) {
            std::printf(" no match (reference too short?)\n");
            continue;
        }
        const bool pass = best.envelope_correlation >= min_envelope &&
                          std::abs(best.level_diff_db) <= max_level_db &&
                          best.spectral_distance_db <= max_spectral_db;
        if (pass) ++matched;
        std::printf(" ref %zu at %8.3f s (%.2f s): waveform %.3f, envelope %.3f, level %+.2f dB, "
                    "spectrum %.2f dB  %s\n",
                    best_ref, best.ref_start_s, best.length_s, best.correlation,
                    best.envelope_correlation, best.level_diff_db, best.spectral_distance_db,
                    pass ? "match" : "DIFFERENT");
    }
    if (!refs.empty()) {
        std::printf("\n%d of %zu events match (envelope >= %.2f, |level| <= %.1f dB, "
                    "spectrum <= %.1f dB)\n",
                    matched, events.size(), min_envelope, max_level_db, max_spectral_db);
        ok = ok && matched == static_cast<int>(events.size());
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
