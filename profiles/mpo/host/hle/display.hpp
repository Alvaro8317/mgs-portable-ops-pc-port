// SPDX-License-Identifier: GPL-3.0-or-later
//
// The vblank and its users (F09): sceDisplay and InterruptManager.
//
// Display runs PPSSPP's frame timeline (Core/HLE/sceDisplay.cpp and
// Core/HW/Display.cpp @ v1.20.4): frames of 1001/60 ms (59.94 Hz), the vblank
// starts 0.7315 ms before the end of each one. Entering the vblank counts the
// frame (vcount, accumulated hcount), runs the vblank interrupt handlers,
// wakes the threads in sceDisplayWaitVblank* and latches the framebuffer
// that sceDisplaySetFrameBuf queued for the next frame. Each flip goes to
// set_flip_handler() (F10b presents it), and leaving the vblank runs the
// vblank-end handler (sceCtrl samples there, like PPSSPP's
// DisplayFireVblankEnd).
//
// Interrupts keeps the sub-interrupt handlers (sceKernelRegisterSubIntrHandler)
// of each interrupt and runs the enabled ones, in sub-interrupt order, inline
// in the kernel event that raises them (CpuBridge::run_interrupt), with the
// kernel in interrupt context. Like PPSSPP's idle-thread context they get the
// boot module's $gp and a stack of their own. The vblank is only raised when
// interrupts are enabled (PSP_INTR_ONLY_IF_ENABLED); an interrupt with a host
// handler of its own (the GE, F11b) is posted instead: it waits in a queue
// until interrupts are enabled and no other one runs, like PPSSPP's
// pendingInterrupts.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "kernel/kernel.hpp"

namespace mpo::hle {

class System;

inline constexpr std::uint32_t kGeInterrupt = 25;     // PSP_GE_INTR
inline constexpr std::uint32_t kVblankInterrupt = 30; // PSP_VBLANK_INTR
inline constexpr std::uint32_t kInterruptCount = 67;  // PSP_NUMBER_INTERRUPTS
inline constexpr std::uint32_t kSubInterruptCount = 32;

class Interrupts {
public:
    explicit Interrupts(System &system);

    std::uint32_t register_sub(std::uint32_t intr, std::uint32_t sub, std::uint32_t handler,
                               std::uint32_t handler_arg);
    std::uint32_t release_sub(std::uint32_t intr, std::uint32_t sub);
    std::uint32_t enable_sub(std::uint32_t intr, std::uint32_t sub);
    std::uint32_t disable_sub(std::uint32_t intr, std::uint32_t sub);
    // __TriggerInterrupt(PSP_INTR_IMMEDIATE | PSP_INTR_ONLY_IF_ENABLED, intr,
    // PSP_INTR_SUB_ALL), from a kernel event or the host.
    void trigger(std::uint32_t intr);
    // __TriggerInterrupt(PSP_INTR_IMMEDIATE, intr, PSP_INTR_SUB_NONE) for an
    // interrupt with a host handler (PPSSPP's IntrHandler subclasses): queues
    // `run`, which runs in interrupt context once interrupts are enabled and
    // no other one runs, now if possible. `run` picks the sub-interrupt and
    // calls run_sub() itself.
    void post(std::function<void()> run);
    // __RunOnePendingInterrupt until the queue is empty or interrupts are off.
    void run_pending();
    // Runs the handler of `sub` with $a0 = a0, $a1 = its argument, $a2 = a2,
    // from a posted handler. False when no handler is registered there
    // (enabled or not, like IntrHandler::get).
    bool run_sub(std::uint32_t intr, std::uint32_t sub, std::uint32_t a0, std::uint32_t a2);
    // Where the handlers run: the boot module's $gp and the top of their stack.
    void set_context(std::uint32_t gp, std::uint32_t sp) {
        gp_ = gp;
        sp_ = sp;
    }
    void set_gp(std::uint32_t gp) { gp_ = gp; }
    [[nodiscard]] std::uint64_t handled() const noexcept { return handled_; }

    void register_hle();

private:
    struct Sub {
        std::uint32_t handler{};
        std::uint32_t arg{};
        bool enabled{};
    };

    System &system_;
    std::array<std::map<std::uint32_t, Sub>, kInterruptCount> handlers_{};
    std::deque<std::function<void()>> pending_;
    std::uint32_t gp_ = 0;
    std::uint32_t sp_ = 0;
    std::uint64_t handled_ = 0;
};

struct Framebuffer {
    std::uint32_t top_address = 0x04000000u;
    std::uint32_t stride = 512u;
    std::uint32_t format = 3u; // GE_FORMAT_8888
};

class Display {
public:
    Display(System &system, Interrupts &interrupts);

    // Schedules the first vblank (PPSSPP __DisplayInit).
    void start();
    void register_hle();

    [[nodiscard]] std::uint32_t vcount() const noexcept { return vcount_; }
    [[nodiscard]] bool in_vblank() const noexcept { return in_vblank_; }
    [[nodiscard]] const Framebuffer &framebuffer() const noexcept { return framebuf_; }
    [[nodiscard]] std::uint64_t flips() const noexcept { return flips_; }
    // Called on every flip with the framebuffer to show (F10b presents it).
    void set_flip_handler(std::function<void(const Framebuffer &)> handler) {
        on_flip_ = std::move(handler);
    }
    // Called when each vblank ends (DisplayFireVblankEnd: __CtrlVblank).
    void set_vblank_end_handler(std::function<void()> handler) {
        on_vblank_end_ = std::move(handler);
    }

    // 1001/60 ms and the vblank's 0.7315 ms, in nanoseconds.
    static constexpr std::uint64_t kFrameNumeratorNs = 1'001'000'000ull; // / 60
    static constexpr std::uint64_t kVblankNs = 731'500u;

private:
    void schedule_at(std::uint64_t target_ns, std::function<void()> event);
    void enter_vblank();
    void leave_vblank();
    void flip();
    [[nodiscard]] std::uint32_t current_hcount() const;
    [[nodiscard]] std::uint32_t accumulated_hcount() const;
    // __DisplayWaitForVblanks.
    std::uint32_t wait_vblanks(std::uint32_t vblanks, bool callbacks);
    std::uint32_t set_framebuf(std::uint32_t top, std::uint32_t stride, std::uint32_t format,
                               std::uint32_t sync);

    struct Waiter {
        kernel::Uid thread{};
        std::uint32_t vblanks{};
    };

    System &system_;
    Interrupts &interrupts_;
    kernel::Kernel &kernel_;
    std::uint64_t frame_ = 0; // Frames started (vblanks entered).
    std::uint32_t vcount_ = 0;
    std::uint32_t hcount_base_ = 0;
    bool in_vblank_ = false;
    std::uint64_t frame_start_us_ = 0;
    std::vector<Waiter> waiting_;
    std::map<kernel::Uid, std::uint32_t> paused_; // Thread -> vcount that unblocks it.
    Framebuffer framebuf_;
    Framebuffer latched_;
    bool latched_pending_ = false;
    bool flipped_this_frame_ = false;
    bool has_set_mode_ = false;
    std::uint32_t mode_ = 0;
    std::uint32_t width_ = 480;
    std::uint32_t height_ = 272;
    std::uint32_t brightness_ = 84;
    std::uint32_t resume_mode_ = 0;
    std::uint32_t hold_mode_ = 0;
    std::uint64_t flips_ = 0;
    std::function<void(const Framebuffer &)> on_flip_;
    std::function<void()> on_vblank_end_;
};

} // namespace mpo::hle
