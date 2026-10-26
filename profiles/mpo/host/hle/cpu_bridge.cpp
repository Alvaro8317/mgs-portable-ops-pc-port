// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle/cpu_bridge.hpp"

#include <algorithm>
#include <bit>

namespace mpo::hle {

namespace {

constexpr std::uint32_t kMipsBranchSelf = 0x1000FFFFu; // b -1
constexpr std::uint32_t kMipsNop = 0u;
constexpr std::uint32_t kDeadBeef = 0xDEADBEEFu;
constexpr std::uint32_t kSignalingNan = 0x7F800001u;

// The bridge whose kernel the starvation hook preempts (the hook is a plain
// function pointer).
CpuBridge *g_preempting = nullptr;

} // namespace

void reset_thread_context(psprecomp::AllegrexContext &ctx) {
    ctx = psprecomp::AllegrexContext{};
    for (std::size_t i = 0; i < ctx.gpr.size(); ++i) ctx.gpr[i] = kDeadBeef;
    ctx.gpr[0] = 0u;
    for (float &f : ctx.fpr) f = std::bit_cast<float>(kSignalingNan);
    for (float &v : ctx.vfpu) v = std::bit_cast<float>(kSignalingNan);
    // VFPU control registers: S/T prefixes neutral, D prefix neutral, CC,
    // INF4, two reserved, REV and the eight RCX seeds of the random unit.
    ctx.vfpu_ctrl = {0xE4u,       0xE4u,       0u,          0x3Fu,       0u,          0u,
                     0u,          0x7772CEABu, 0x3F800001u, 0x3F800002u, 0x3F800004u, 0x3F800008u,
                     0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u};
    ctx.fcr31 = 0x00000E00u;
    ctx.hi = kDeadBeef;
    ctx.lo = kDeadBeef;
}

CpuBridge::~CpuBridge() {
    if (g_preempting == this) {
        psprecomp::set_runtime_starvation_hook(nullptr, 0u);
        g_preempting = nullptr;
    }
}

void CpuBridge::set_callback_return(std::uint32_t address) {
    callback_return_ = address;
    runtime_.memory().store32(address, kMipsBranchSelf);
    runtime_.memory().store32(address + 4u, kMipsNop);
}

void CpuBridge::install_preemption(kernel::Kernel &kernel, std::uint64_t interval,
                                   std::function<void()> on_tick) {
    preempt_kernel_ = &kernel;
    on_tick_ = std::move(on_tick);
    g_preempting = this;
    psprecomp::set_runtime_starvation_hook(&CpuBridge::starvation_hook, interval);
}

void CpuBridge::starvation_hook(psprecomp::Runtime &, psprecomp::AllegrexContext &) {
    CpuBridge *self = g_preempting;
    if (self == nullptr || self->preempt_kernel_ == nullptr) return;
    if (self->on_tick_) self->on_tick_();
    self->preempt_kernel_->preemption_point();
}

void CpuBridge::park(kernel::Kernel &kernel) {
    // Every resume hands control straight back to Kernel::run(), which then
    // returns kStopped; the host reads the runtime's stop reason.
    for (;;) {
        kernel.request_stop();
        kernel.preemption_point();
    }
}

void CpuBridge::dispatch_until(kernel::Kernel &kernel, std::uint32_t return_pc) {
    if (runtime_.run_until(return_pc) != psprecomp::Runtime::RunUntilResult::Returned) park(kernel);
}

std::uint32_t CpuBridge::run_thread(kernel::Kernel &kernel, const kernel::ThreadStart &start) {
    // on_switch() installed this thread's slot; the entry starts from a reset
    // context with the registers __KernelStartThread sets.
    psprecomp::AllegrexContext &cpu = runtime_.cpu();
    reset_thread_context(cpu);
    cpu.gpr[4] = start.a0;
    cpu.gpr[5] = start.a1;
    cpu.gpr[26] = start.k0;
    cpu.gpr[28] = start.gp;
    cpu.gpr[29] = start.sp;
    cpu.gpr[30] = start.fp;
    cpu.gpr[31] = start.ra;
    cpu.pc = start.entry;
    if (on_thread_start_) on_thread_start_(start);
    dispatch_until(kernel, start.ra);
    const std::uint32_t v0 = runtime_.cpu().gpr[2];
    if (on_thread_return_) on_thread_return_(start.thread, v0);
    return v0;
}

std::uint32_t CpuBridge::call_guest(kernel::Kernel &kernel, kernel::Uid, std::uint32_t entry,
                                    std::span<const std::uint32_t> args) {
    ++guest_calls_;
    psprecomp::AllegrexContext &cpu = runtime_.cpu();
    // A plain copy on this fiber's stack: nothing here owns memory, so the
    // frame may be abandoned if the callback exits its thread.
    const psprecomp::AllegrexContext saved = cpu;
    const std::size_t count = std::min<std::size_t>(args.size(), 6u);
    for (std::size_t i = 0; i < count; ++i) cpu.gpr[4u + i] = args[i];
    cpu.gpr[31] = callback_return_;
    cpu.pc = entry;
    dispatch_until(kernel, callback_return_);
    const std::uint32_t v0 = runtime_.cpu().gpr[2];
    runtime_.cpu() = saved;
    return v0;
}

void CpuBridge::on_switch(kernel::Kernel &, kernel::Uid from, kernel::Uid to) {
    ++switches_;
    const psprecomp::Runtime::ThreadExecutionState left =
        runtime_.exchange_thread_execution_state({});
    if (from != 0) {
        const auto it = threads_.find(from);
        if (it != threads_.end()) {
            it->second->ctx = runtime_.cpu();
            it->second->execution = left;
        }
    }
    std::unique_ptr<ThreadCpu> &slot = threads_[to];
    if (!slot) {
        slot = std::make_unique<ThreadCpu>();
        reset_thread_context(slot->ctx);
    }
    runtime_.cpu() = slot->ctx;
    (void)runtime_.exchange_thread_execution_state(slot->execution);
}

void CpuBridge::on_thread_released(kernel::Kernel &, kernel::Uid thread) {
    threads_.erase(thread);
}

void CpuBridge::write_thread_return_stub(kernel::Kernel &kernel, std::uint32_t address) {
    // The dispatch loop returns as soon as the pc reaches this address; the
    // self-branch only matters to an interpreter that runs past it.
    kernel.memory().write_u32(address, kMipsBranchSelf);
    kernel.memory().write_u32(address + 4u, kMipsNop);
}

} // namespace mpo::hle
