// SPDX-License-Identifier: GPL-3.0-or-later
//
// Time source for the kernel, in PSP microseconds. It is injected so tests run
// on a virtual clock that only moves when every thread is blocked; F09 decides
// how the real game clock relates to host time and executed work.

#pragma once

#include <cstdint>

namespace mpo::kernel {

class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual std::uint64_t now_us() const = 0;
    // Called by the scheduler when no thread can run and the next timer is at
    // `deadline_us`. Must return with now_us() >= deadline_us.
    virtual void wait_until(std::uint64_t deadline_us) = 0;
};

// Deterministic clock: time stands still unless the kernel idles or a test
// advances it.
class VirtualClock final : public Clock {
public:
    explicit VirtualClock(std::uint64_t start_us = 0) : now_(start_us) {}
    [[nodiscard]] std::uint64_t now_us() const override { return now_; }
    void wait_until(std::uint64_t deadline_us) override {
        if (deadline_us > now_) now_ = deadline_us;
    }
    void advance(std::uint64_t delta_us) { now_ += delta_us; }

private:
    std::uint64_t now_;
};

} // namespace mpo::kernel
