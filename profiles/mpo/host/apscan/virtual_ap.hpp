// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seeded virtual access points for the AP Scan (F20a).
//
// MPO turns each AP into a recruit with a seed built from BSSID[2..5] and the
// SSID bytes (docs/analysis/apscan.md), so the same AP always gives the same
// soldier. The generator therefore keeps the AP set stable for a (seed,
// epoch) pair: the set only changes when the epoch does, either because
// `rotation_period_us` of PSP time went by or because `epoch_offset` changed
// (a "go somewhere else" control for the player). Each scan adds a small,
// deterministic jitter to the signal, like a real scan does.
//
// Everything is a pure function of the config and its inputs: no host RNG, no
// floating point, no std:: distributions (their output varies across standard
// libraries), so the APs are the same on every compiler and platform.

#pragma once

#include "apscan/access_point.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mpo::apscan {

inline constexpr std::uint32_t kMaxVirtualAps = 32;
inline constexpr std::uint8_t kMaxJitter = 20;
inline constexpr std::uint64_t kDefaultRotationPeriodUs = 24ull * 60 * 60 * 1000 * 1000;

struct VirtualApConfig {
    std::uint64_t seed = 0;
    // APs per scan, 1 to kMaxVirtualAps. MPO reads at most 16 (its ID list
    // buffer is 0x80 bytes).
    std::uint32_t count = 8;
    // Base signal range, in percent. MPO skips an AP at 9 or less.
    std::uint8_t min_strength = 30;
    std::uint8_t max_strength = 90;
    // Per-scan variation, +/- this many points (0 to kMaxJitter).
    std::uint8_t jitter = 3;
    // PSP time an AP set stays visible; 0 keeps one set forever. The default
    // is the 24 h per-AP cooldown players report (docs/RESEARCH.md).
    std::uint64_t rotation_period_us = kDefaultRotationPeriodUs;
    // Added to the epoch, so the AP set can be changed by hand.
    std::int64_t epoch_offset = 0;
};

// Empty if `config` is usable, otherwise why not.
[[nodiscard]] std::string validate(const VirtualApConfig &config);

class VirtualApGenerator {
public:
    // `config` must pass validate().
    explicit VirtualApGenerator(const VirtualApConfig &config);

    [[nodiscard]] const VirtualApConfig &config() const { return config_; }

    // Epoch in effect at `now_us` (PSP microseconds), offset included.
    [[nodiscard]] std::int64_t epoch_at(std::uint64_t now_us) const;

    // The APs of `epoch` with their base signal. BSSIDs and SSIDs are unique
    // within the set.
    [[nodiscard]] std::vector<AccessPoint> access_points(std::int64_t epoch) const;

    // What scan number `scan_index` sees at `now_us`: the APs of that epoch
    // with the per-scan jitter applied.
    [[nodiscard]] std::vector<AccessPoint> scan(std::uint64_t now_us,
                                                std::uint64_t scan_index) const;

private:
    VirtualApConfig config_;
};

} // namespace mpo::apscan
