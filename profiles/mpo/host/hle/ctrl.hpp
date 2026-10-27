// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceCtrl (F10b), ported from PPSSPP Core/HLE/sceCtrl.cpp @ v1.20.4.
//
// The controller is sampled when each vblank ends (or on a timer, with
// sceCtrlSetSamplingCycle): the host's state goes into a ring of 64
// SceCtrlData buffers and updates the latch (make, break, press, release).
// In digital mode the sticks read as centred. sceCtrlReadBuffer* returns the
// buffers sampled since the last read (at most n), and blocks until the next
// sample when there are none; sceCtrlPeekBuffer* returns the last n without
// consuming them. Games only see CTRL_MASK_USER.
//
// The host's state comes from a Sampler: mpo_boot feeds it from the
// InputTranslator of F10a (SDL) and from scripted presses. Without one,
// nothing is pressed.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "ctrl.h"
#include "kernel/kernel.hpp"

namespace mpo::hle {

class Display;
class System;

class Ctrl {
public:
    static constexpr std::uint32_t kBuffers = 64u; // NUM_CTRL_BUFFERS

    // The host's controller at the PSP time `timestamp` (microseconds).
    using Sampler = std::function<frontend::SceCtrlData(std::uint64_t timestamp)>;

    Ctrl(System &system, Display &display);

    void set_sampler(Sampler sampler) { sampler_ = std::move(sampler); }
    void register_hle();

    [[nodiscard]] bool analog() const noexcept { return analog_; }
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    // __CtrlReadLatch: the buttons pressed since the last call (sceUtility
    // dialogs, F18b).
    std::uint32_t take_dialog_make();

private:
    struct Latch {
        std::uint32_t make{};
        std::uint32_t brk{};
        std::uint32_t press{};
        std::uint32_t release{};
    };
    struct Waiter {
        kernel::Uid thread{};
        std::uint32_t data{};
        bool negative{};
    };

    void sample();       // __CtrlDoSample
    void update_latch(); // __CtrlUpdateLatch
    std::uint32_t read_single(std::uint32_t data, bool negative);
    std::uint32_t read_buffer(std::uint32_t data, std::uint32_t count, bool negative, bool peek);
    void trace_read(std::uint32_t data, std::uint32_t done, bool negative);
    std::uint32_t write_latch(std::uint32_t address);
    std::uint32_t set_sampling_cycle(std::uint32_t cycle);
    void schedule_timer();

    System &system_;
    kernel::Kernel &kernel_;
    Sampler sampler_;
    bool analog_ = false;
    std::array<frontend::SceCtrlData, kBuffers> buffers_{};
    std::uint32_t buffer_ = 1u; // Next buffer to fill.
    std::uint32_t read_ = 0u;   // Next buffer to read.
    std::uint32_t old_buttons_ = 0u;
    std::uint32_t latch_buffers_ = 0u;
    Latch latch_{0u, 0u, 0u, 0xFFFFFFFFu}; // Everything starts released.
    std::uint32_t dialog_make_ = 0u;
    std::int32_t idle_reset_ = -1;
    std::int32_t idle_back_ = -1;
    std::uint32_t cycle_ = 0u; // 0: sample at each vblank end.
    kernel::Kernel::EventId timer_ = 0u;
    bool timer_scheduled_ = false;
    std::vector<Waiter> waiting_;
    std::uint64_t samples_ = 0u;
};

} // namespace mpo::hle
