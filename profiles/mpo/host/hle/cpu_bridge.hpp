// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CPU side of mpo_kernel (kernel::HostHooks) on PSPRecomp (F08b).
//
// Every PSP thread runs on its own kernel fiber, and on that fiber it runs its
// own dispatch loop, psprecomp::Runtime::run_until(), on the runtime's single
// cpu(): on_switch() saves the context (and the runtime's per-thread
// execution state) of the thread that leaves and installs the one of the
// thread that resumes. So the native frames of a thread suspended inside an
// HLE call or at a preemption point stay on its own stack and find their own
// registers when they resume; no frame is ever stale.
//
// Guest callbacks (call_guest) run in a nested run_until() on the same fiber
// and return to a host trap, like PPSSPP's MipsCall. The visibility points of
// the recompiled code (the runtime's starvation hook, every
// `preemption_interval` dispatches) call Kernel::preemption_point().
//
// A runtime stop (an unimplemented import, a guest memory fault) is fatal: the
// thread parks itself and asks the kernel to stop, so Kernel::run() returns
// kStopped and the host reads Runtime::stop_reason().

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>

#include "kernel/kernel.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

namespace mpo::hle {

// A fresh thread context, as PPSSPP's PSPThreadContext::reset() leaves it.
// Ported from PPSSPP Core/HLE/sceKernelThread.cpp @ v1.20.4.
void reset_thread_context(psprecomp::AllegrexContext &ctx);

class CpuBridge final : public kernel::HostHooks {
public:
    explicit CpuBridge(psprecomp::Runtime &runtime) : runtime_(runtime) {}
    ~CpuBridge() override;
    CpuBridge(const CpuBridge &) = delete;
    CpuBridge &operator=(const CpuBridge &) = delete;

    // Where guest callbacks return: 8 bytes of guest memory the host owns
    // (`b -1; nop` is written there, so a stray interpreter pass parks on it).
    void set_callback_return(std::uint32_t address);
    [[nodiscard]] std::uint32_t callback_return() const noexcept { return callback_return_; }

    // Installs the runtime's starvation hook: every `interval` dispatches,
    // `on_tick` (host time, deadlines) and then kernel.preemption_point().
    // Process-global, like the hook itself; uninstalled by the destructor.
    void install_preemption(kernel::Kernel &kernel, std::uint64_t interval,
                            std::function<void()> on_tick);

    // Called on the thread's fiber when its entry returns to start.ra, with
    // $v0, before the kernel ends the thread. It may end the thread itself
    // (module_start threads delete themselves) and then never returns.
    void set_thread_return_handler(std::function<void(kernel::Uid, std::uint32_t)> handler) {
        on_thread_return_ = std::move(handler);
    }
    // Called on the thread's fiber before its entry runs.
    void set_thread_start_handler(std::function<void(const kernel::ThreadStart &)> handler) {
        on_thread_start_ = std::move(handler);
    }

    // kernel::HostHooks
    std::uint32_t run_thread(kernel::Kernel &kernel, const kernel::ThreadStart &start) override;
    std::uint32_t call_guest(kernel::Kernel &kernel, kernel::Uid thread, std::uint32_t entry,
                             std::span<const std::uint32_t> args) override;
    void on_switch(kernel::Kernel &kernel, kernel::Uid from, kernel::Uid to) override;
    void on_thread_released(kernel::Kernel &kernel, kernel::Uid thread) override;
    void write_thread_return_stub(kernel::Kernel &kernel, std::uint32_t address) override;

    // Introspection for tests and traces.
    [[nodiscard]] std::uint64_t switches() const noexcept { return switches_; }
    [[nodiscard]] std::uint64_t guest_calls() const noexcept { return guest_calls_; }
    [[nodiscard]] std::size_t saved_contexts() const noexcept { return threads_.size(); }

private:
    struct ThreadCpu {
        psprecomp::AllegrexContext ctx{};
        psprecomp::Runtime::ThreadExecutionState execution{};
    };

    // Runs cpu() until it reaches `return_pc`; a runtime stop parks the thread
    // for good (the runtime stop is fatal).
    void dispatch_until(kernel::Kernel &kernel, std::uint32_t return_pc);
    [[noreturn]] void park(kernel::Kernel &kernel);
    static void starvation_hook(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx);

    psprecomp::Runtime &runtime_;
    std::uint32_t callback_return_ = 0;
    std::unordered_map<kernel::Uid, std::unique_ptr<ThreadCpu>> threads_;
    std::function<void(kernel::Uid, std::uint32_t)> on_thread_return_;
    std::function<void(const kernel::ThreadStart &)> on_thread_start_;
    kernel::Kernel *preempt_kernel_ = nullptr;
    std::function<void()> on_tick_;
    std::uint64_t switches_ = 0;
    std::uint64_t guest_calls_ = 0;
};

} // namespace mpo::hle
