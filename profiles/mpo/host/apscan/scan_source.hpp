// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the APs of a scan come from (F20a). The model asks its source once per
// scan, when the scan completes. The virtual source is the default; a source
// backed by the host's real Wi-Fi scan is the planned extension point.

#pragma once

#include "apscan/access_point.hpp"
#include "apscan/virtual_ap.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace mpo::apscan {

struct ScanRequest {
    std::uint64_t now_us = 0;     // PSP time at which the scan completes
    std::uint64_t scan_index = 0; // 0 for the first scan since init()
};

struct ScanOutcome {
    // 0, or the code the ERROR event carries to the game's handler (for
    // example error::kWlanSwitchOff when the host has no Wi-Fi).
    std::uint32_t error = 0;
    std::vector<AccessPoint> access_points;
};

class ScanSource {
public:
    virtual ~ScanSource() = default;
    // Must not block: a slow backend returns its latest cached results.
    virtual ScanOutcome scan(const ScanRequest &request) = 0;
};

class VirtualApSource final : public ScanSource {
public:
    // `config` must pass validate().
    explicit VirtualApSource(const VirtualApConfig &config) : generator_(config) {}

    ScanOutcome scan(const ScanRequest &request) override;

    [[nodiscard]] const VirtualApGenerator &generator() const { return generator_; }

private:
    VirtualApGenerator generator_;
};

// Extension point for the host's own Wi-Fi scan (WlanAPI on Windows, nl80211
// on Linux), so real APs around the player can be used, as on the PSP. Not
// implemented: it always returns nullptr and the caller falls back to
// VirtualApSource. An implementation has to scan in the background, map RSSI
// to a percent and keep at most kMaxVirtualAps entries.
[[nodiscard]] std::unique_ptr<ScanSource> make_os_scan_source();

} // namespace mpo::apscan
