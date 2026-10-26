// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal state of mpo_kernel. Not part of the API F08b uses.
//
// The object model, wait bookkeeping and scheduling rules are ported from
// PPSSPP Core/HLE/sceKernel.h, sceKernelThread.cpp, KernelWaitHelpers.h and
// ThreadQueueList.h @ v1.20.4 (GPL-2.0-or-later); the context switch itself is
// new (fibers instead of swapping a single CPU context).

#pragma once

#include "kernel/fiber.hpp"
#include "kernel/kernel.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mpo::kernel {

enum class ObjType {
    kThread,
    kCallback,
    kSema,
    kEventFlag,
    kMutex,
    kFpl,
    kVpl,
    kPartitionBlock,
};

struct Object {
    explicit Object(ObjType t) : type(t) {}
    virtual ~Object() = default;
    ObjType type;
    Uid uid{};
    std::string name; // Truncated to 31 characters, like KERNELOBJECT_MAX_NAME_LENGTH.
};

// The wait state saved while a thread runs callbacks (PPSSPP ActionAfterMipsCall).
struct SavedWaitState {
    std::uint32_t status{};
    WaitType wait_type{WaitType::kNone};
    Uid wait_id{};
    std::uint32_t wait_value{};
    std::uint32_t timeout_ptr{};
    bool processing_callbacks{};
};

// A wait paused for a callback (PPSSPP pausedWaits; the key is always the
// thread because currentCallbackId is never set in PPSSPP).
struct PausedWait {
    bool active{};
    std::uint64_t deadline{}; // 0 means "no timeout", as in PPSSPP.
    // Per-type wait data removed from the object's list: event flag bits/mode/
    // out address, or the FPL/VPL output address.
    std::uint32_t data0{};
    std::uint32_t data1{};
    std::uint32_t data2{};
};

struct CallbackCall {
    Uid callback{};
    std::uint32_t entry{};
    std::array<std::uint32_t, 3> args{};
};

struct Thread final : Object {
    static constexpr ObjType kType = ObjType::kThread;
    static constexpr std::uint32_t kMissing = error::kUnknownThid;
    Thread() : Object(kType) {}

    // NativeThread fields.
    std::uint32_t attr{};
    std::uint32_t status{};
    std::uint32_t entry{};
    std::uint32_t initial_stack{};
    std::uint32_t stack_size{};
    std::uint32_t gp{};
    std::int32_t initial_priority{};
    std::int32_t current_priority{};
    WaitType wait_type{WaitType::kNone};
    Uid wait_id{};
    std::int32_t wakeup_count{};
    std::uint32_t exit_status{};
    std::uint64_t run_for_clocks{};
    std::int32_t num_interrupt_preempts{};
    std::int32_t num_thread_preempts{};
    std::int32_t num_releases{};

    // ThreadWaitInfo.
    std::uint32_t wait_value{};
    std::uint32_t timeout_ptr{};

    std::uint32_t stack_start{};
    std::uint32_t stack_end{};
    BlockAllocator *stack_allocator{};
    Uid module_id{};

    std::vector<Uid> callbacks;
    std::vector<Uid> waiting_threads; // Threads in sceKernelWaitThreadEnd on this one.
    bool processing_callbacks{};

    // Callbacks scheduled on this thread but not run yet (PPSSPP pendingMipsCalls).
    bool callback_pending{};
    bool pending_resched_after{};
    SavedWaitState pending_saved;
    CallbackCall pending_call;
    int callback_depth{};

    PausedWait paused;

    ThreadStart start;
    std::uint32_t ret{}; // $v0 as the kernel last set it for this thread.
    std::unique_ptr<Fiber> fiber;
    void *host_data{};

    [[nodiscard]] bool is_running() const { return (status & thread_status::kRunning) != 0; }
    [[nodiscard]] bool is_ready() const { return (status & thread_status::kReady) != 0; }
    [[nodiscard]] bool is_waiting() const { return (status & thread_status::kWait) != 0; }
    [[nodiscard]] bool is_suspended() const { return (status & thread_status::kSuspend) != 0; }
    [[nodiscard]] bool is_stopped() const { return (status & thread_status::kDormant) != 0; }
    [[nodiscard]] bool is_waiting_for(WaitType kind, Uid id) const {
        return is_waiting() && wait_type == kind && wait_id == id;
    }
    [[nodiscard]] Uid wait_id_for(WaitType kind) const { return wait_type == kind ? wait_id : 0; }
};

struct Callback final : Object {
    static constexpr ObjType kType = ObjType::kCallback;
    static constexpr std::uint32_t kMissing = error::kUnknownCbid;
    Callback() : Object(kType) {}
    Uid thread{};
    std::uint32_t entry{};
    std::uint32_t common_arg{};
    std::int32_t notify_count{};
    std::uint32_t notify_arg{};
};

struct Semaphore final : Object {
    static constexpr ObjType kType = ObjType::kSema;
    static constexpr std::uint32_t kMissing = error::kUnknownSemid;
    Semaphore() : Object(kType) {}
    std::uint32_t attr{};
    std::int32_t initial_count{};
    std::int32_t current_count{};
    std::int32_t max_count{};
    std::int32_t num_wait_threads{};
    std::vector<Uid> waiting;
};

struct EventFlagWaiter {
    Uid thread{};
    std::uint32_t bits{};
    std::uint32_t mode{};
    std::uint32_t out_addr{};
};

struct EventFlag final : Object {
    static constexpr ObjType kType = ObjType::kEventFlag;
    static constexpr std::uint32_t kMissing = error::kUnknownEvfid;
    EventFlag() : Object(kType) {}
    std::uint32_t attr{};
    std::uint32_t initial_pattern{};
    std::uint32_t current_pattern{};
    std::int32_t num_wait_threads{};
    std::vector<EventFlagWaiter> waiting;
};

struct Mutex final : Object {
    static constexpr ObjType kType = ObjType::kMutex;
    static constexpr std::uint32_t kMissing = error::kMutexNoSuchMutex;
    Mutex() : Object(kType) {}
    std::uint32_t attr{};
    std::int32_t initial_count{};
    std::int32_t lock_level{};
    Uid lock_thread{-1};
    std::int32_t num_wait_threads{};
    std::vector<Uid> waiting;
};

struct PoolWaiter {
    Uid thread{};
    std::uint32_t addr_ptr{};
};

struct Fpl final : Object {
    static constexpr ObjType kType = ObjType::kFpl;
    static constexpr std::uint32_t kMissing = error::kUnknownFplid;
    Fpl() : Object(kType) {}
    std::uint32_t attr{};
    std::int32_t block_size{};
    std::int32_t num_blocks{};
    std::int32_t num_free_blocks{};
    std::int32_t num_wait_threads{};
    std::vector<bool> blocks;
    std::uint32_t address{};
    std::int32_t aligned_size{};
    std::int32_t next_block{};
    BlockAllocator *allocator{};
    std::vector<PoolWaiter> waiting;
};

struct Vpl final : Object {
    static constexpr ObjType kType = ObjType::kVpl;
    static constexpr std::uint32_t kMissing = error::kUnknownVplid;
    Vpl() : Object(kType) {}
    std::uint32_t attr{};
    std::int32_t pool_size{};
    std::int32_t free_size{};
    std::int32_t num_wait_threads{};
    std::uint32_t address{}; // Pool data, after the 0x20-byte header.
    std::uint32_t header{};  // SceKernelVplHeader in guest memory.
    BlockAllocator *allocator{};
    std::vector<PoolWaiter> waiting;
};

struct PartitionBlock final : Object {
    static constexpr ObjType kType = ObjType::kPartitionBlock;
    static constexpr std::uint32_t kMissing = error::kUnknownUid;
    PartitionBlock() : Object(kType) {}
    BlockAllocator *allocator{};
    std::uint32_t address{};
};

enum class TimerKind {
    kWakeup, // Delay end (PPSSPP eventScheduledWakeup).
    kThreadEndTimeout,
    kSemaTimeout,
    kEventFlagTimeout,
    kMutexTimeout,
    kFplTimeout,
    kVplTimeout,
    kHleDelay,
    kHost,
};

struct TimerEvent {
    TimerKind kind{};
    Uid thread{};
    Kernel::EventId id{};
    std::function<void()> host;
};

// Flags a kernel call leaves for the end of the call (PPSSPP hleAfterSyscall).
struct AfterCall {
    bool resched{};
    bool resched_callbacks{};
    bool current_callbacks{};
    bool wait_callbacks_now{}; // __KernelWaitCallbacksCurThread
};

struct Kernel::Impl {
    Impl(Kernel &owner, const KernelConfig &config);
    ~Impl();

    Kernel &kernel;
    KernelConfig config;
    GuestMemory &mem;
    Clock &clock;
    HostHooks &hooks;

    // ---- Object pool (PPSSPP KernelObjectPool) ----
    static constexpr int kMaxObjects = 4096;
    static constexpr int kHandleOffset = 0x100;
    static constexpr int kInitialNextId = 0x10;
    std::array<std::unique_ptr<Object>, kMaxObjects> pool;
    int next_id = kInitialNextId;

    Uid create_object(std::unique_ptr<Object> object);
    Object *lookup(Uid uid) const;
    template <class T>
    T *get(Uid uid, std::uint32_t &error) const {
        Object *object = lookup(uid);
        if (object == nullptr || object->type != T::kType) {
            error = T::kMissing;
            return nullptr;
        }
        error = 0;
        return static_cast<T *>(object);
    }
    template <class T>
    T *get(Uid uid) const {
        std::uint32_t ignored = 0;
        return get<T>(uid, ignored);
    }
    std::unique_ptr<Object> take_object(Uid uid);
    std::uint32_t destroy_object(Uid uid, ObjType type, std::uint32_t missing_error);

    // ---- Threads and scheduling ----
    std::array<std::deque<Uid>, 128> ready;
    std::vector<Uid> thread_queue; // All threads, in creation order.
    Thread *current{};
    Thread *fiber_thread{}; // Thread whose fiber is executing (nullptr on the host stack).
    Uid last_resumed{};
    std::vector<std::unique_ptr<Thread>> zombies;
    std::vector<std::unique_ptr<Fiber>> zombie_fibers;
    bool dispatch_enabled = true;
    bool interrupts_enabled = true;
    bool in_interrupt = false;
    bool stop_requested = false;
    int in_event_dispatch = 0;
    int ready_callbacks_count = 0;
    int in_callback_count = 0;
    Uid exit_callback = 0;
    AfterCall after;

    Thread *thread(Uid uid) const { return get<Thread>(uid); }
    void ready_push_front(int priority, Uid uid) {
        ready[static_cast<std::size_t>(priority)].push_front(uid);
    }
    void ready_push_back(int priority, Uid uid) {
        ready[static_cast<std::size_t>(priority)].push_back(uid);
    }
    void ready_remove(int priority, Uid uid);
    Uid ready_pop_first();
    Uid ready_pop_first_better(int priority);

    void change_ready_state(Thread &t, bool is_ready);
    void change_thread_state(Thread &t, std::uint32_t new_status);
    bool dispatch_allowed() const { return dispatch_enabled && interrupts_enabled; }
    // Blocking calls need to run on the current thread's own fiber.
    bool on_current_fiber() const {
        return fiber_thread != nullptr && fiber_thread == current && in_event_dispatch == 0;
    }
    Thread *next_thread();
    void switch_context(Thread *target);
    // PPSSPP __KernelReSchedule: callbacks, timers, then maybe switch. State only.
    void reschedule_core();
    void reschedule_state(bool do_callbacks);
    // On a thread's fiber: yields until this thread runs again, running any
    // callbacks scheduled on it.
    void block_until_running(Thread &self);
    void resume_fiber(Thread &t);
    void fiber_main(Uid uid);
    void release_fiber(Thread &t);
    void destroy_zombies();

    // ---- Kernel call framing ----
    std::uint32_t finish_call(std::uint32_t result);
    void request_resched() { after.resched = true; }
    void request_resched(bool callbacks) {
        after.resched = true;
        if (callbacks) after.resched_callbacks = true;
    }
    void request_current_callbacks() { after.current_callbacks = true; }

    // ---- Waits ----
    // PPSSPP __KernelWaitCurThread. Returns false if the wait was refused
    // (dispatch disabled or no current thread).
    bool wait_current(WaitType type, Uid wait_id, std::uint32_t wait_value,
                      std::uint32_t timeout_ptr, bool process_callbacks);
    bool wait_callbacks_current(WaitType type, Uid wait_id, std::uint32_t wait_value,
                                std::uint32_t timeout_ptr);
    void resume_from_wait(Uid thread, std::uint32_t result);
    bool verify_wait(Uid thread, WaitType type, Uid id) const;
    void wait_begin_callback(Thread &t);
    void wait_end_callback(Thread &t);
    void write_timeout_left(std::uint32_t timeout_ptr, std::int64_t left_us);

    // ---- Thread lifecycle ----
    Thread *create_thread_object(Uid &uid, Uid module_id, std::uint32_t gp, const char *name,
                                 std::uint32_t entry, std::int32_t priority,
                                 std::uint32_t stack_size, std::uint32_t attr);
    std::uint32_t create_thread_checked(const char *name, Uid module_id, std::uint32_t gp,
                                        std::uint32_t entry, std::uint32_t priority,
                                        std::uint32_t stack_size, std::uint32_t attr,
                                        std::uint32_t option_addr, bool allow_kernel);
    void reset_thread(Thread &t, std::int32_t lowest_priority);
    void fill_stack(Thread &t);
    void free_stack(Thread &t);
    void stop_thread(Thread &t, std::uint32_t exit_status);
    std::uint32_t delete_thread_internal(Uid uid, std::uint32_t exit_status);
    void thread_end_listeners(Thread &t);
    void thread_returned(std::uint32_t exit_status);
    std::uint32_t delay_thread_impl(std::uint64_t usec, bool callbacks);
    std::uint32_t sleep_thread_impl(bool callbacks);
    std::uint32_t wait_thread_end_impl(Uid uid, std::uint32_t timeout_ptr, bool callbacks);

    // ---- Callbacks ----
    bool current_has_ready_callbacks() const;
    std::optional<CallbackCall> take_ready_callback(Thread &t);
    bool check_thread_callbacks(Thread &t, bool force);
    void start_callback_on_thread(Thread &t, const CallbackCall &call, bool resched_after);
    bool check_callbacks();
    bool force_callbacks();
    // Returns whether the caller must reschedule afterwards.
    bool run_pending_callbacks(Thread &t);
    void destroy_callback(Callback &cb);
    void notify_callback(Uid uid, std::uint32_t notify_arg);

    // ---- Timers (PPSSPP CoreTiming) ----
    std::map<std::pair<std::uint64_t, std::uint64_t>, TimerEvent> events;
    std::uint64_t event_seq = 0;
    Kernel::EventId next_event_id = 1;
    void schedule(TimerKind kind, Uid thread, std::uint64_t delay_us);
    // Removes every matching event; returns the time left of the last match
    // (0 if none), like CoreTiming::UnscheduleEvent.
    std::int64_t unschedule(TimerKind kind, Uid thread);
    void advance_events();
    void fire(TimerEvent &event);
    std::uint64_t now() const { return clock.now_us(); }

    // Timeout handlers.
    // HLEKernel::WaitExecTimeout. The thread stays in the object's list: if
    // the object is deleted before the thread runs, it gets DELETE instead.
    template <class T>
    void wait_exec_timeout(Uid thread_uid, WaitType type) {
        const Thread *t = thread(thread_uid);
        if (t == nullptr) return;
        const Uid uid = t->wait_id_for(type);
        const std::uint32_t timeout_ptr = t->timeout_ptr;
        if (uid == 0 || get<T>(uid) == nullptr) return;
        if (timeout_ptr != 0) mem.write_u32(timeout_ptr, 0);
        resume_from_wait(thread_uid, error::kWaitTimeout);
    }
    void sema_timeout(Uid thread);
    void event_flag_timeout(Uid thread);
    void vpl_timeout(Uid thread);
    void wakeup_timeout(Uid thread);
    void hle_delay_timeout(Uid thread);

    // ---- Semaphores / event flags / mutexes ----
    bool unlock_sema_for_thread(Semaphore &s, Uid thread, std::uint32_t result, bool &woke);
    bool clear_sema_threads(Semaphore &s, std::uint32_t reason);
    std::uint32_t wait_sema_impl(Uid id, std::int32_t count, std::uint32_t timeout_ptr,
                                 bool callbacks);
    bool unlock_event_flag_for_thread(EventFlag &e, EventFlagWaiter &waiter, std::uint32_t result,
                                      bool &woke);
    bool apply_event_flag_match(EventFlag &e, std::uint32_t bits, std::uint32_t mode,
                                std::uint32_t out_addr);
    bool clear_event_flag_threads(EventFlag &e, std::uint32_t reason);
    std::multimap<Uid, Uid> mutex_held_locks;
    void mutex_acquire_lock(Mutex &m, std::int32_t count, Uid thread);
    void mutex_erase_lock(Mutex &m);
    bool unlock_mutex_for_thread(Mutex &m, Uid thread, std::uint32_t result);
    bool lock_mutex_check(Mutex *m, std::int32_t count, std::uint32_t &error);
    bool lock_mutex(Mutex *m, std::int32_t count, std::uint32_t &error);
    bool unlock_mutex(Mutex &m);
    void mutex_thread_end(Uid thread);
    void set_mutex_timeout(std::uint32_t timeout_ptr);

    // ---- Memory ----
    BlockAllocator user_memory{256};
    BlockAllocator kernel_memory{256};
    BlockAllocator volatile_memory{256};
    bool kernel_mode = false;
    std::uint32_t sdk_version = 0;
    std::uint32_t compiler_version = 0;
    bool has_sdk_version = false;
    BlockAllocator *allocator_from_id(std::uint32_t partition);
    bool unlock_fpl_for_thread(Fpl &fpl, PoolWaiter &waiter, std::uint32_t result, bool &woke);
    bool clear_fpl_threads(Fpl &fpl, std::uint32_t reason);
    void sort_fpl_threads(Fpl &fpl);
    int fpl_allocate_block(Fpl &fpl);
    void set_fpl_timeout(std::uint32_t timeout_ptr);
    bool unlock_vpl_for_thread(Vpl &vpl, PoolWaiter &waiter, std::uint32_t result, bool &woke);
    bool clear_vpl_threads(Vpl &vpl, std::uint32_t reason);
    void sort_vpl_threads(Vpl &vpl);
    bool allocate_vpl_impl(Uid uid, std::uint32_t size, std::uint32_t addr_ptr,
                           std::uint32_t &error, bool trying);
    std::uint32_t allocate_vpl_call(Uid uid, std::uint32_t size, std::uint32_t addr_ptr,
                                    std::uint32_t timeout_ptr, bool callbacks);
    std::uint32_t allocate_fpl_call(Uid uid, std::uint32_t block_ptr_addr,
                                    std::uint32_t timeout_ptr, bool callbacks);
    // SceKernelVplHeader, kept in guest memory like PPSSPP does.
    void vpl_header_init(std::uint32_t header, std::uint32_t size);
    std::uint32_t vpl_header_allocate(std::uint32_t header, std::uint32_t size);
    bool vpl_header_free(std::uint32_t header, std::uint32_t ptr);
    std::uint32_t vpl_header_free_size(std::uint32_t header);
    std::uint32_t delay_result_impl(std::uint32_t result, std::uint32_t usec);

    static std::string clip_name(const char *name);
    void write_name(std::uint32_t address, const std::string &name);
};

inline Uid waiter_thread(Uid uid) {
    return uid;
}
inline Uid waiter_thread(const EventFlagWaiter &w) {
    return w.thread;
}
inline Uid waiter_thread(const PoolWaiter &w) {
    return w.thread;
}

// HLEKernel::CleanupWaitingThreads: drops threads that stopped waiting. Like
// PPSSPP it swaps the last entry into the hole, so the order changes.
template <class T>
void cleanup_waiting(const Kernel::Impl &k, WaitType type, Uid uid, std::vector<T> &list) {
    std::size_t size = list.size();
    for (std::size_t i = 0; i < size;) {
        if (!k.verify_wait(waiter_thread(list[i]), type, uid)) {
            if (--size != i) std::swap(list[i], list[size]);
        } else {
            ++i;
        }
    }
    list.resize(size);
}

// Ties a public kernel call to the AfterCall flags; nested calls (from guest
// callbacks) keep their own flags.
class CallScope {
public:
    explicit CallScope(Kernel::Impl &k) : k_(k), saved_(k.after) { k.after = AfterCall{}; }
    ~CallScope() { k_.after = saved_; }
    CallScope(const CallScope &) = delete;
    CallScope &operator=(const CallScope &) = delete;
    std::uint32_t finish(std::uint32_t result) { return k_.finish_call(result); }

private:
    Kernel::Impl &k_;
    AfterCall saved_;
};

} // namespace mpo::kernel
