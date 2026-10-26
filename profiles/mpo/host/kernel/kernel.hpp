// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_kernel: the PSP kernel objects of ThreadManForUser / SysMemUserForUser
// on host fibers (F08a). A PSP thread is a minicoro fiber with its own host
// stack; one host thread runs the single-core, preemptive-priority scheduler
// and switches fibers only at kernel calls and at visibility points.
//
// Semantics (wake order, error codes, timeouts, callbacks, allocation
// placement) follow PPSSPP Core/HLE/sceKernel*.cpp @ v1.20.4; the session note
// docs/sessions/F08a.md lists every place where it deliberately differs.
//
// Every method named after a PSP call takes the call's arguments as the guest
// passes them (guest addresses as uint32) and returns the raw $v0 value, so an
// HLE wrapper only moves registers. This library registers no NID: that is
// F08b's job. Calls that may block or reschedule must be made from the fiber
// of the current PSP thread; the rest may also be made from the host side
// (bootstrap, tests, other HLE modules).

#pragma once

#include "kernel/block_allocator.hpp"
#include "kernel/clock.hpp"
#include "kernel/error_codes.hpp"
#include "kernel/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mpo::kernel {

using Uid = std::int32_t;

enum class WaitType : std::int32_t {
    kNone = 0,
    kSleep = 1,
    kDelay = 2,
    kSema = 3,
    kEventFlag = 4,
    kMbx = 5,
    kVpl = 6,
    kFpl = 7,
    kMsgPipe = 8,
    kThreadEnd = 9,
    kAudioChannel = 10,
    kUmd = 11,
    kVblank = 12,
    kMutex = 13,
    kLwMutex = 14,
    kCtrl = 15,
    kIo = 16,
    kGeDrawSync = 17,
    kGeListSync = 18,
    kModule = 19,
    kHleDelay = 20,
    kTlspl = 21,
    kVmem = 22,
    kAsyncIo = 23,
};

namespace thread_status {
inline constexpr std::uint32_t kRunning = 1;
inline constexpr std::uint32_t kReady = 2;
inline constexpr std::uint32_t kWait = 4;
inline constexpr std::uint32_t kSuspend = 8;
inline constexpr std::uint32_t kDormant = 16;
inline constexpr std::uint32_t kDead = 32;
} // namespace thread_status

namespace thread_attr {
inline constexpr std::uint32_t kKernel = 0x00001000u;
inline constexpr std::uint32_t kVfpu = 0x00004000u;
inline constexpr std::uint32_t kNoFillStack = 0x00100000u;
inline constexpr std::uint32_t kClearStack = 0x00200000u;
inline constexpr std::uint32_t kLowStack = 0x00400000u;
inline constexpr std::uint32_t kUser = 0x80000000u;
} // namespace thread_attr

// SysMem block types for alloc_partition_memory().
namespace memblock_type {
inline constexpr std::uint32_t kLow = 0;
inline constexpr std::uint32_t kHigh = 1;
inline constexpr std::uint32_t kAddr = 2;
inline constexpr std::uint32_t kLowAligned = 3;
inline constexpr std::uint32_t kHighAligned = 4;
} // namespace memblock_type

// Initial guest register state of a thread that is about to run its entry.
// $ra points at the 64 bytes the kernel reserves under the arguments: the
// runner treats a return to $ra as the thread returning (PPSSPP writes its
// THREADRETURN syscall there; see HostHooks::write_thread_return_stub).
struct ThreadStart {
    Uid thread{};
    std::uint32_t entry{};
    std::uint32_t gp{};
    std::uint32_t sp{};
    std::uint32_t k0{};
    std::uint32_t fp{};
    std::uint32_t ra{};
    std::uint32_t a0{};
    std::uint32_t a1{};
    std::uint32_t attr{};
};

class Kernel;

// What the kernel needs from the CPU side (F08b: PSPRecomp; tests: host
// lambdas). run_thread and call_guest run on the PSP thread's fiber.
class HostHooks {
public:
    virtual ~HostHooks() = default;
    // Runs the thread from its entry until it returns to start.ra; returns $v0
    // (the exit status). A thread that calls exit/terminate never returns here.
    virtual std::uint32_t run_thread(Kernel &kernel, const ThreadStart &start) = 0;
    // Runs a guest function (a callback) on the current thread with up to six
    // arguments and returns its $v0. The caller's guest registers must be
    // preserved, like PPSSPP's MipsCall does.
    virtual std::uint32_t call_guest(Kernel &kernel, Uid thread, std::uint32_t entry,
                                     std::span<const std::uint32_t> args) = 0;
    // On the scheduler stack, right before the fiber of `to` resumes. `from` is
    // the thread whose fiber ran last (0 if none). F08b swaps CPU contexts here.
    virtual void on_switch(Kernel &, Uid /*from*/, Uid /*to*/) {}
    // The thread's fiber is being discarded (deleted or restarted thread).
    virtual void on_thread_released(Kernel &, Uid) {}
    // Fills the first 8 bytes at `address` (start.ra) with the return trap.
    virtual void write_thread_return_stub(Kernel &, std::uint32_t /*address*/) {}
};

struct KernelConfig {
    GuestMemory *memory = nullptr;
    Clock *clock = nullptr;
    HostHooks *hooks = nullptr;
    // Host stack reserved per PSP thread (lazily committed). Recompiled code
    // nests deeply: Yakumo needs 64 MiB (CLAUDE.md §3).
    std::size_t fiber_stack_size = 64u * 1024u * 1024u;
    // End of user RAM: 0x0A000000 on a PSP-1000 (32 MiB).
    std::uint32_t user_memory_end = 0x0A000000u;
};

enum class RunResult {
    kIdle,     // Nothing can run and no timer is pending (all threads blocked or gone).
    kStopped,  // request_stop() was called.
    kDeadline, // The clock reached the run() deadline.
};

// Read-only snapshot for tests, traces and debuggers.
struct ThreadInfo {
    Uid uid{};
    std::string name;
    std::uint32_t status{};
    std::int32_t current_priority{};
    std::int32_t initial_priority{};
    WaitType wait_type{};
    Uid wait_id{};
    std::int32_t wakeup_count{};
    std::uint32_t exit_status{};
    std::uint32_t attr{};
    std::uint32_t entry{};
    std::uint32_t initial_stack{};
    std::uint32_t stack_size{};
    std::uint32_t gp{};
    std::int32_t num_releases{};
};

class Kernel {
public:
    explicit Kernel(const KernelConfig &config);
    ~Kernel();
    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;

    // ---- Scheduler -----------------------------------------------------
    // Runs PSP threads on this host thread until nothing can run, a stop is
    // requested, or the clock passes `until_us`. Must not be called from a fiber.
    RunResult run(std::uint64_t until_us = std::numeric_limits<std::uint64_t>::max());
    void request_stop();
    // Visibility point from guest code on the current thread: fires due timers
    // and switches if a better thread became ready (PPSSPP: CoreTiming events
    // between basic blocks).
    void preemption_point();
    [[nodiscard]] Uid current_thread() const;
    [[nodiscard]] std::uint64_t now_us() const;

    // Interrupt and dispatch state (sceKernelCpuSuspendIntr & co.). Interrupt
    // handlers themselves are F09's; set_in_interrupt() marks their extent.
    std::uint32_t cpu_suspend_intr();
    void cpu_resume_intr(std::uint32_t enable);
    [[nodiscard]] bool interrupts_enabled() const;
    void set_in_interrupt(bool in_interrupt);
    std::uint32_t suspend_dispatch_thread();
    std::uint32_t resume_dispatch_thread(std::uint32_t enabled);
    // Whether the HLE caller should get SCE_KERNEL_ERROR_CAN_NOT_WAIT /
    // ILLEGAL_CONTEXT for a NID flagged HLE_NOT_DISPATCH_SUSPENDED /
    // HLE_NOT_IN_INTERRUPT (PPSSPP HLE.cpp). The waiting calls below already
    // apply the flags PPSSPP gives them.
    [[nodiscard]] bool dispatch_enabled() const;
    [[nodiscard]] bool in_interrupt() const;

    // ---- Threads -------------------------------------------------------
    std::uint32_t create_thread(const char *name, std::uint32_t entry, std::uint32_t priority,
                                std::uint32_t stack_size, std::uint32_t attr,
                                std::uint32_t option_addr);
    // Creation on behalf of a module (bootstrap, module_start threads).
    std::uint32_t create_thread_for_module(const char *name, Uid module_id, std::uint32_t gp,
                                           std::uint32_t entry, std::uint32_t priority,
                                           std::uint32_t stack_size, std::uint32_t attr,
                                           bool allow_kernel);
    std::uint32_t start_thread(Uid thread, std::uint32_t arg_size, std::uint32_t arg_block,
                               bool force_args = false);
    std::uint32_t exit_thread(std::uint32_t exit_status);
    std::uint32_t exit_delete_thread(std::uint32_t exit_status);
    std::uint32_t delete_thread(Uid thread);
    std::uint32_t terminate_thread(Uid thread);
    std::uint32_t terminate_delete_thread(Uid thread);
    std::uint32_t get_thread_id();
    std::uint32_t get_thread_current_priority();
    std::uint32_t change_current_thread_attr(std::uint32_t clear_attr, std::uint32_t set_attr);
    std::uint32_t change_thread_priority(Uid thread, std::uint32_t priority);
    std::uint32_t rotate_thread_ready_queue(std::uint32_t priority);
    std::uint32_t delay_thread(std::uint32_t usec);
    std::uint32_t delay_thread_cb(std::uint32_t usec);
    std::uint32_t delay_sys_clock_thread(std::uint32_t sysclock_addr);
    std::uint32_t delay_sys_clock_thread_cb(std::uint32_t sysclock_addr);
    std::uint32_t sleep_thread();
    std::uint32_t sleep_thread_cb();
    std::uint32_t wakeup_thread(Uid thread);
    std::uint32_t cancel_wakeup_thread(Uid thread);
    std::uint32_t wait_thread_end(Uid thread, std::uint32_t timeout_addr);
    std::uint32_t wait_thread_end_cb(Uid thread, std::uint32_t timeout_addr);
    std::uint32_t release_wait_thread(Uid thread);
    std::uint32_t suspend_thread(Uid thread);
    std::uint32_t resume_thread(Uid thread);
    std::uint32_t refer_thread_status(Uid thread, std::uint32_t status_addr);
    std::uint32_t get_thread_exit_status(Uid thread);
    std::uint64_t get_system_time_wide();

    // ---- Callbacks -----------------------------------------------------
    std::uint32_t create_callback(const char *name, std::uint32_t entry, std::uint32_t common_arg);
    std::uint32_t delete_callback(Uid callback);
    std::uint32_t notify_callback(Uid callback, std::uint32_t notify_arg);
    std::uint32_t cancel_callback(Uid callback);
    std::uint32_t get_callback_count(Uid callback);
    std::uint32_t refer_callback_status(Uid callback, std::uint32_t status_addr);
    std::uint32_t check_callback();
    // LoadExecForUser: stores the callback the host notifies on "exit game".
    std::uint32_t register_exit_callback(Uid callback);
    [[nodiscard]] Uid exit_callback() const;

    // ---- Semaphores ----------------------------------------------------
    std::uint32_t create_sema(const char *name, std::uint32_t attr, std::int32_t initial,
                              std::int32_t max, std::uint32_t option_addr);
    std::uint32_t delete_sema(Uid sema);
    std::uint32_t signal_sema(Uid sema, std::int32_t count);
    std::uint32_t wait_sema(Uid sema, std::int32_t count, std::uint32_t timeout_addr);
    std::uint32_t wait_sema_cb(Uid sema, std::int32_t count, std::uint32_t timeout_addr);
    std::uint32_t poll_sema(Uid sema, std::int32_t count);
    std::uint32_t cancel_sema(Uid sema, std::int32_t new_count,
                              std::uint32_t num_wait_threads_addr);
    std::uint32_t refer_sema_status(Uid sema, std::uint32_t info_addr);

    // ---- Event flags ---------------------------------------------------
    std::uint32_t create_event_flag(const char *name, std::uint32_t attr,
                                    std::uint32_t initial_pattern, std::uint32_t option_addr);
    std::uint32_t delete_event_flag(Uid flag);
    std::uint32_t set_event_flag(Uid flag, std::uint32_t bits);
    std::uint32_t clear_event_flag(Uid flag, std::uint32_t bits);
    std::uint32_t wait_event_flag(Uid flag, std::uint32_t bits, std::uint32_t wait_mode,
                                  std::uint32_t out_bits_addr, std::uint32_t timeout_addr);
    std::uint32_t wait_event_flag_cb(Uid flag, std::uint32_t bits, std::uint32_t wait_mode,
                                     std::uint32_t out_bits_addr, std::uint32_t timeout_addr);
    std::uint32_t poll_event_flag(Uid flag, std::uint32_t bits, std::uint32_t wait_mode,
                                  std::uint32_t out_bits_addr);
    std::uint32_t cancel_event_flag(Uid flag, std::uint32_t pattern,
                                    std::uint32_t num_wait_threads_addr);
    std::uint32_t refer_event_flag_status(Uid flag, std::uint32_t info_addr);

    // ---- Mutexes -------------------------------------------------------
    std::uint32_t create_mutex(const char *name, std::uint32_t attr, std::int32_t initial_count,
                               std::uint32_t option_addr);
    std::uint32_t delete_mutex(Uid mutex);
    std::uint32_t lock_mutex(Uid mutex, std::int32_t count, std::uint32_t timeout_addr);
    std::uint32_t lock_mutex_cb(Uid mutex, std::int32_t count, std::uint32_t timeout_addr);
    std::uint32_t try_lock_mutex(Uid mutex, std::int32_t count);
    std::uint32_t unlock_mutex(Uid mutex, std::int32_t count);
    std::uint32_t cancel_mutex(Uid mutex, std::int32_t count, std::uint32_t num_wait_threads_addr);
    std::uint32_t refer_mutex_status(Uid mutex, std::uint32_t info_addr);

    // ---- Fixed-length pools (FPL) ----------------------------------------
    std::uint32_t create_fpl(const char *name, std::uint32_t partition, std::uint32_t attr,
                             std::uint32_t block_size, std::uint32_t num_blocks,
                             std::uint32_t option_addr);
    std::uint32_t delete_fpl(Uid fpl);
    std::uint32_t allocate_fpl(Uid fpl, std::uint32_t block_addr_out, std::uint32_t timeout_addr);
    std::uint32_t allocate_fpl_cb(Uid fpl, std::uint32_t block_addr_out,
                                  std::uint32_t timeout_addr);
    std::uint32_t try_allocate_fpl(Uid fpl, std::uint32_t block_addr_out);
    std::uint32_t free_fpl(Uid fpl, std::uint32_t block_addr);
    std::uint32_t cancel_fpl(Uid fpl, std::uint32_t num_wait_threads_addr);
    std::uint32_t refer_fpl_status(Uid fpl, std::uint32_t status_addr);

    // ---- Variable-length pools (VPL) -------------------------------------
    std::uint32_t create_vpl(const char *name, std::uint32_t partition, std::uint32_t attr,
                             std::uint32_t size, std::uint32_t option_addr);
    std::uint32_t delete_vpl(Uid vpl);
    std::uint32_t allocate_vpl(Uid vpl, std::uint32_t size, std::uint32_t addr_out,
                               std::uint32_t timeout_addr);
    std::uint32_t allocate_vpl_cb(Uid vpl, std::uint32_t size, std::uint32_t addr_out,
                                  std::uint32_t timeout_addr);
    std::uint32_t try_allocate_vpl(Uid vpl, std::uint32_t size, std::uint32_t addr_out);
    std::uint32_t free_vpl(Uid vpl, std::uint32_t addr);
    std::uint32_t cancel_vpl(Uid vpl, std::uint32_t num_wait_threads_addr);
    std::uint32_t refer_vpl_status(Uid vpl, std::uint32_t info_addr);

    // ---- SysMem ----------------------------------------------------------
    std::uint32_t alloc_partition_memory(std::uint32_t partition, const char *name,
                                         std::uint32_t type, std::uint32_t size,
                                         std::uint32_t addr);
    std::uint32_t free_partition_memory(Uid block);
    std::uint32_t get_block_head_addr(Uid block);
    std::uint32_t max_free_mem_size();
    std::uint32_t total_free_mem_size();
    std::uint32_t set_compiled_sdk_version(std::uint32_t version);
    std::uint32_t set_compiler_version(std::uint32_t version);
    [[nodiscard]] std::uint32_t compiled_sdk_version() const;
    // Partitions as PPSSPP lays them out (user 0x08800000.., kernel
    // 0x08000000-0x08400000, volatile 0x08400000-0x08800000), for the module
    // loader (F08b) and sceKernelVolatileMem*.
    BlockAllocator &user_memory();
    BlockAllocator &kernel_memory();
    BlockAllocator &volatile_memory();
    // User mode by default; partitions 1/3/4/8/10 need kernel mode.
    void set_kernel_mode(bool kernel_mode);

    // ---- Services for other HLE modules (F08b/F09) ------------------------
    // Puts the current thread in a wait the caller resolves later with
    // resume_from_wait() (vblank, IO, UMD...). Returns the final $v0.
    std::uint32_t wait_current_thread(WaitType type, Uid wait_id, std::uint32_t wait_value,
                                      std::uint32_t timeout_addr, bool process_callbacks,
                                      std::uint32_t provisional_result = 0);
    void resume_from_wait(Uid thread, std::uint32_t result);
    [[nodiscard]] bool is_waiting_for(Uid thread, WaitType type, Uid wait_id) const;
    // hleDelayResult: returns `result` to the current thread after `usec`.
    std::uint32_t delay_result(std::uint32_t result, std::uint32_t usec);
    // Host timer on the kernel clock (vblank, async IO completion...). The
    // callback runs on the scheduler path with no PSP thread switch inside it.
    using EventId = std::uint64_t;
    EventId schedule_event(std::uint64_t delay_us, std::function<void()> callback);
    // Returns the microseconds that were left, or -1 if it already fired.
    std::int64_t unschedule_event(EventId event);
    // Notifies a callback from host code (power, UMD, exit), like PPSSPP's
    // __KernelNotifyCallback.
    void notify_callback_from_host(Uid callback, std::uint32_t notify_arg);

    // ---- Introspection -----------------------------------------------------
    [[nodiscard]] bool thread_info(Uid thread, ThreadInfo &out) const;
    [[nodiscard]] std::vector<Uid> thread_list() const;
    [[nodiscard]] std::vector<Uid> ready_queue() const;
    void set_thread_host_data(Uid thread, void *data);
    [[nodiscard]] void *thread_host_data(Uid thread) const;
    [[nodiscard]] GuestMemory &memory() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mpo::kernel
