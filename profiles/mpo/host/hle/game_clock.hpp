// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's time (F09): the kernel clock and the RTC.
//
// GameClock is PPSSPP's CoreTiming as far as the HLE can see it: virtual time
// that only the guest's work moves (dispatches at the visibility points, the
// cycles an HLE call "eats", hleEatCycles) and that jumps to the next event
// when every thread waits. It never reads the host clock, so a run is
// reproducible. With `realtime`, a jump waits until the host has caught up
// (PPSSPP's frame throttling), so the game runs at the PSP's speed instead of
// as fast as possible.
//
// Rtc is the wall clock of sceRtc and Libc time: a base taken once (the host's
// time at boot, or a fixed one for traces) plus the virtual time, like
// PPSSPP's rtcBaseTime + CoreTiming::GetGlobalTimeUs(). The local time zone is
// a fixed offset taken at boot.

#pragma once

#include <chrono>
#include <cstdint>

#include "kernel/clock.hpp"

namespace mpo::hle {

// The PSP's CPU clock (CoreTiming CPU_HZ).
inline constexpr std::uint64_t kCpuHz = 222'000'000u;

class GameClock final : public kernel::Clock {
public:
    [[nodiscard]] std::uint64_t now_us() const override { return now_; }
    void wait_until(std::uint64_t deadline_us) override;

    void advance(std::uint64_t delta_us) { now_ += delta_us; }
    // hleEatCycles: the time an HLE call takes on the PSP.
    void eat_cycles(std::uint64_t cycles);
    void set_realtime(bool realtime);
    [[nodiscard]] bool realtime() const noexcept { return realtime_; }

private:
    std::uint64_t now_ = 0;
    std::uint64_t cycle_remainder_ = 0;
    bool realtime_ = false;
    std::chrono::steady_clock::time_point host_start_{};
    std::uint64_t virtual_start_ = 0;
};

class Rtc {
public:
    // Seconds since the Unix epoch at boot (UTC) and the local offset.
    Rtc(std::int64_t base_unix_seconds, std::int64_t utc_offset_seconds)
        : base_(base_unix_seconds), utc_offset_(utc_offset_seconds) {}
    // The host's time and time zone, now.
    static Rtc from_host();

    [[nodiscard]] std::int64_t base_seconds() const noexcept { return base_; }
    [[nodiscard]] std::int64_t utc_offset() const noexcept { return utc_offset_; }
    // Microseconds since the Unix epoch, UTC, at virtual time `now_us`.
    [[nodiscard]] std::int64_t unix_us(std::uint64_t now_us) const {
        return base_ * 1'000'000 + static_cast<std::int64_t>(now_us);
    }
    // sceRtc ticks: microseconds since 0001-01-01, UTC.
    [[nodiscard]] std::uint64_t ticks(std::uint64_t now_us) const {
        return static_cast<std::uint64_t>(unix_us(now_us)) + kUnixEpochTicks;
    }

    // rtcMagicOffset: 1970-01-01 in sceRtc ticks.
    static constexpr std::uint64_t kUnixEpochTicks = 62135596800000000ull;

private:
    std::int64_t base_;
    std::int64_t utc_offset_;
};

} // namespace mpo::hle
