// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle/game_clock.hpp"

#include <ctime>
#include <thread>

#include "io/file_system.hpp"

namespace mpo::hle {

void GameClock::wait_until(std::uint64_t deadline_us) {
    if (deadline_us <= now_) return;
    if (realtime_) {
        const auto target = host_start_ + std::chrono::microseconds(deadline_us - virtual_start_);
        std::this_thread::sleep_until(target);
    }
    now_ = deadline_us;
}

void GameClock::eat_cycles(std::uint64_t cycles) {
    cycle_remainder_ += cycles * 1'000'000u;
    now_ += cycle_remainder_ / kCpuHz;
    cycle_remainder_ %= kCpuHz;
}

void GameClock::set_realtime(bool realtime) {
    realtime_ = realtime;
    host_start_ = std::chrono::steady_clock::now();
    virtual_start_ = now_;
}

Rtc Rtc::from_host() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    // The local wall time read as if it were UTC, minus the real UTC time.
    const std::int64_t local_seconds =
        io::days_from_civil(local.tm_year + 1900, static_cast<std::uint32_t>(local.tm_mon + 1),
                            static_cast<std::uint32_t>(local.tm_mday)) *
            86400 +
        local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    const auto utc = static_cast<std::int64_t>(now);
    return Rtc(utc, local_seconds - utc);
}

} // namespace mpo::hle
