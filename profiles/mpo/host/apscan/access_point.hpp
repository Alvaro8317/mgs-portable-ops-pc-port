// SPDX-License-Identifier: GPL-3.0-or-later
//
// One access point as a scan reports it (F20a).

#pragma once

#include "apscan/apctl_types.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace mpo::apscan {

struct AccessPoint {
    std::array<std::uint8_t, kBssidSize> bssid{};
    // 1 to 32 bytes. MPO copies it with strncpy, so a NUL ends it early.
    std::string ssid;
    std::uint8_t channel = 1;
    // Percent, 0 to 100. MPO ignores an AP at 9 or less and ranks the rest by
    // this value rounded down to a multiple of 5.
    std::uint8_t strength = 0;
    SecurityType security = SecurityType::kNone;

    friend bool operator==(const AccessPoint &, const AccessPoint &) = default;
};

} // namespace mpo::apscan
