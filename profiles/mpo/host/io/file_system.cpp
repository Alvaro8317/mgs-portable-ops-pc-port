// SPDX-License-Identifier: GPL-3.0-or-later
//
// Date helpers. days_from_civil and civil_from_days are Howard Hinnant's
// public-domain algorithms (http://howardhinnant.github.io/date_algorithms.html),
// as ported in PPSSPP Core/HLE/sceRtc.cpp @ v1.20.4.

#include "io/file_system.hpp"

namespace mpo::io {

std::int64_t days_from_civil(std::int64_t year, std::uint32_t month, std::uint32_t day) {
    year -= month <= 2u ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<std::uint32_t>(year - era * 400); // [0, 399]
    const std::uint32_t doy = (153u * (month > 2u ? month - 3u : month + 9u) + 2u) / 5u + day - 1u;
    const std::uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy; // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t days, std::int64_t &year, std::uint32_t &month,
                     std::uint32_t &day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<std::uint32_t>(days - era * 146097); // [0, 146096]
    const std::uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); // [0, 365]
    const std::uint32_t mp = (5u * doy + 2u) / 153u;                      // [0, 11]
    day = doy - (153u * mp + 2u) / 5u + 1u;
    month = mp < 10u ? mp + 3u : mp - 9u;
    year = y + (month <= 2u ? 1 : 0);
}

DateTime date_time_from_unix(std::int64_t seconds, std::uint32_t microsecond) {
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    std::int64_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;
    civil_from_days(days, year, month, day);
    DateTime out;
    out.year = static_cast<std::uint16_t>(year);
    out.month = static_cast<std::uint16_t>(month);
    out.day = static_cast<std::uint16_t>(day);
    out.hour = static_cast<std::uint16_t>(rest / 3600);
    out.minute = static_cast<std::uint16_t>(rest / 60 % 60);
    out.second = static_cast<std::uint16_t>(rest % 60);
    out.microsecond = microsecond;
    return out;
}

} // namespace mpo::io
