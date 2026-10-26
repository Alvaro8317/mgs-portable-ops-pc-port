// SPDX-License-Identifier: GPL-3.0-or-later
//
// Object pool, ready queue, scheduler, fibers, timers, kernel-call framing and
// the generic wait/callback machinery.
//
// Ported from PPSSPP Core/HLE/sceKernel.cpp, sceKernelThread.cpp,
// KernelWaitHelpers.h, ThreadQueueList.h and Core/CoreTiming.cpp @ v1.20.4
// (GPL-2.0-or-later). PPSSPP switches threads by swapping one CPU context;
// here every PSP thread owns a fiber and the switch is a yield back to run().

#include "kernel/kernel_impl.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace mpo::kernel {

namespace {

[[noreturn]] void fatal(const char *message) {
    std::fprintf(stderr, "mpo_kernel: %s\n", message);
    std::abort();
}

} // namespace

Kernel::Impl::Impl(Kernel &owner, const KernelConfig &cfg)
    : kernel(owner), config(cfg), mem(*cfg.memory), clock(*cfg.clock), hooks(*cfg.hooks) {
    // __KernelMemoryInit.
    kernel_memory.init(0x08000000u, 0x08400000u - 0x08000000u);
    user_memory.init(0x08800000u, config.user_memory_end - 0x08800000u);
    volatile_memory.init(0x08400000u, 0x08800000u - 0x08400000u);
    // The kernel's user-side system library (Kernel_Library code) sits at the
    // bottom of user memory on real firmware; PPSSPP reserves it too.
    user_memory.alloc_at(0x08800000u, 0x4000u, "usersystemlib");
}

Kernel::Impl::~Impl() {
    // Fibers of exited or blocked threads are discarded without unwinding.
    for (auto &object : pool) {
        if (object && object->type == ObjType::kThread)
            static_cast<Thread *>(object.get())->fiber.reset();
    }
    zombies.clear();
    zombie_fibers.clear();
}

// ---------------------------------------------------------------------------
// Object pool. UIDs are handed out from a counter that only moves forward, and
// the lowest free slot is reused only once the counter runs out (PPSSPP).

Uid Kernel::Impl::create_object(std::unique_ptr<Object> object) {
    int bottom = kInitialNextId;
    if (next_id >= bottom && next_id < kMaxObjects) bottom = next_id++;
    for (int i = bottom; i < kMaxObjects; ++i) {
        if (!pool[static_cast<std::size_t>(i)]) {
            object->uid = i + kHandleOffset;
            pool[static_cast<std::size_t>(i)] = std::move(object);
            return i + kHandleOffset;
        }
    }
    fatal("out of kernel object slots");
}

Object *Kernel::Impl::lookup(Uid uid) const {
    if (uid < kHandleOffset || uid >= kHandleOffset + kMaxObjects) return nullptr;
    return pool[static_cast<std::size_t>(uid - kHandleOffset)].get();
}

std::unique_ptr<Object> Kernel::Impl::take_object(Uid uid) {
    if (lookup(uid) == nullptr) return nullptr;
    return std::move(pool[static_cast<std::size_t>(uid - kHandleOffset)]);
}

std::uint32_t Kernel::Impl::destroy_object(Uid uid, ObjType type, std::uint32_t missing_error) {
    Object *object = lookup(uid);
    if (object == nullptr || object->type != type) return missing_error;
    pool[static_cast<std::size_t>(uid - kHandleOffset)].reset();
    return 0;
}

std::string Kernel::Impl::clip_name(const char *name) {
    std::string out = name != nullptr ? name : "";
    if (out.size() > 31) out.resize(31);
    return out;
}

void Kernel::Impl::write_name(std::uint32_t address, const std::string &name) {
    std::uint8_t bytes[32] = {};
    for (std::size_t i = 0; i < name.size() && i < 31; ++i)
        bytes[i] = static_cast<std::uint8_t>(name[i]);
    mem.write_bytes(address, bytes);
}

// ---------------------------------------------------------------------------
// Ready queue (ThreadQueueList): one FIFO per priority, lower is better.

void Kernel::Impl::ready_remove(int priority, Uid uid) {
    auto &queue = ready[static_cast<std::size_t>(priority)];
    auto it = std::find(queue.begin(), queue.end(), uid);
    if (it != queue.end()) queue.erase(it);
}

Uid Kernel::Impl::ready_pop_first() {
    for (auto &queue : ready) {
        if (!queue.empty()) {
            const Uid uid = queue.front();
            queue.pop_front();
            return uid;
        }
    }
    return 0;
}

Uid Kernel::Impl::ready_pop_first_better(int priority) {
    for (int p = 0; p < priority && p < static_cast<int>(ready.size()); ++p) {
        auto &queue = ready[static_cast<std::size_t>(p)];
        if (!queue.empty()) {
            const Uid uid = queue.front();
            queue.pop_front();
            return uid;
        }
    }
    return 0;
}

void Kernel::Impl::change_ready_state(Thread &t, bool is_ready) {
    const int priority = t.current_priority;
    if (t.is_ready()) {
        if (!is_ready) ready_remove(priority, t.uid);
    } else if (is_ready) {
        // A thread that loses the CPU keeps its place at the head of its queue.
        if (t.is_running()) {
            ready_push_front(priority, t.uid);
        } else {
            ready_push_back(priority, t.uid);
        }
        t.status = thread_status::kReady;
    }
}

void Kernel::Impl::change_thread_state(Thread &t, std::uint32_t new_status) {
    if (t.status == new_status) return;
    if (!dispatch_enabled && &t == current && new_status != thread_status::kRunning) return;
    change_ready_state(t, (new_status & thread_status::kReady) != 0);
    t.status = new_status;
}

Thread *Kernel::Impl::next_thread() {
    Uid best = 0;
    Thread *cur = current;
    if (cur != nullptr && cur->is_running()) {
        best = ready_pop_first_better(cur->current_priority);
        if (best != 0) change_ready_state(*cur, true);
    } else {
        best = ready_pop_first();
    }
    return best != 0 ? thread(best) : nullptr;
}

void Kernel::Impl::switch_context(Thread *target) {
    Thread *cur = current;
    if (cur != nullptr && cur->is_running()) change_ready_state(*cur, true);
    if (target != nullptr) {
        current = target;
        change_ready_state(*target, false);
        target->status = (target->status | thread_status::kRunning) & ~thread_status::kReady;
        // No longer waiting: from here on a stale wake-up can't match it.
        target->wait_type = WaitType::kNone;
        target->wait_id = 0;
    } else {
        current = nullptr;
    }
}

void Kernel::Impl::reschedule_core() {
    check_callbacks();
    advance_events();
    if (in_interrupt || !dispatch_allowed()) return;
    Thread *next = next_thread();
    if (next != nullptr) {
        switch_context(next);
    } else if (current != nullptr && !current->is_running()) {
        // PPSSPP switches to its idle thread here; we have none, so nothing is
        // current until a thread becomes ready.
        current = nullptr;
    }
}

void Kernel::Impl::reschedule_state(bool do_callbacks) {
    Thread *t = current;
    if (do_callbacks && t != nullptr) t->processing_callbacks = true;
    reschedule_core();
    if (do_callbacks && t != nullptr && t == current && t->is_running())
        t->processing_callbacks = false;
}

void Kernel::Impl::block_until_running(Thread &self) {
    for (;;) {
        if (current != &self || !self.is_running()) {
            if (!self.fiber) fatal("blocking outside a thread fiber");
            self.fiber->yield();
            continue;
        }
        if (self.callback_pending) {
            if (run_pending_callbacks(self)) {
                reschedule_core();
            } else if (current == &self && self.is_ready()) {
                // PPSSPP keeps executing a thread whose wait ended inside the
                // callback's end handler when no reschedule was asked for.
                switch_context(&self);
            }
            continue;
        }
        return;
    }
}

void Kernel::Impl::fiber_main(Uid uid) {
    Thread *t = thread(uid);
    if (t == nullptr) fatal("fiber started for a missing thread");
    const ThreadStart start = t->start;
    // A thread can be scheduled with callbacks already pending (rare: they were
    // notified between start and first run). Run them before the entry.
    block_until_running(*t);
    const std::uint32_t exit_status = hooks.run_thread(kernel, start);
    thread_returned(exit_status);
}

void Kernel::Impl::resume_fiber(Thread &t) {
    if (!t.fiber) {
        const Uid uid = t.uid;
        t.fiber = Fiber::create(config.fiber_stack_size, [this, uid] { fiber_main(uid); });
        if (!t.fiber) fatal("cannot reserve a fiber stack");
    }
    if (last_resumed != t.uid) {
        hooks.on_switch(kernel, last_resumed, t.uid);
        last_resumed = t.uid;
    }
    fiber_thread = &t;
    t.fiber->resume();
    fiber_thread = nullptr;
}

void Kernel::Impl::release_fiber(Thread &t) {
    if (!t.fiber) return;
    if (t.fiber->running()) {
        zombie_fibers.push_back(std::move(t.fiber));
    } else {
        t.fiber.reset();
    }
    if (last_resumed == t.uid) last_resumed = 0;
    hooks.on_thread_released(kernel, t.uid);
}

void Kernel::Impl::destroy_zombies() {
    // Only called on the host stack, so none of these fibers is running.
    zombies.clear();
    zombie_fibers.clear();
}

// ---------------------------------------------------------------------------
// Kernel-call framing (PPSSPP hleFinishSyscall).

std::uint32_t Kernel::Impl::finish_call(std::uint32_t result) {
    // Inside a timer handler the outer reschedule_core() picks the next thread.
    if (in_event_dispatch > 0) {
        after = AfterCall{};
        return result;
    }
    Thread *self = fiber_thread;
    if (self != nullptr) self->ret = result;
    const AfterCall flags = after;
    after = AfterCall{};
    if (flags.wait_callbacks_now) force_callbacks();
    if (flags.current_callbacks && !flags.resched_callbacks) force_callbacks();
    if (flags.resched_callbacks) {
        reschedule_state(true);
    } else if (flags.resched) {
        reschedule_state(false);
    }
    if (self == nullptr) return result;
    block_until_running(*self);
    return self->ret;
}

// ---------------------------------------------------------------------------
// Waits.

bool Kernel::Impl::wait_current(WaitType type, Uid wait_id, std::uint32_t wait_value,
                                std::uint32_t timeout_ptr, bool process_callbacks) {
    if (!dispatch_enabled) return false; // PPSSPP: "Ignoring wait, dispatching disabled".
    Thread *t = current;
    if (t == nullptr) return false;
    t->wait_id = wait_id;
    t->wait_type = type;
    change_thread_state(*t, thread_status::kWait | (t->status & thread_status::kSuspend));
    ++t->num_releases;
    t->wait_value = wait_value;
    t->timeout_ptr = timeout_ptr;
    request_resched(process_callbacks);
    return true;
}

bool Kernel::Impl::wait_callbacks_current(WaitType type, Uid wait_id, std::uint32_t wait_value,
                                          std::uint32_t timeout_ptr) {
    if (!dispatch_enabled) return false;
    Thread *t = current;
    if (t == nullptr) return false;
    t->wait_id = wait_id;
    t->wait_type = type;
    change_thread_state(*t, thread_status::kWait | (t->status & thread_status::kSuspend));
    ++t->num_releases;
    t->wait_value = wait_value;
    t->timeout_ptr = timeout_ptr;
    after.wait_callbacks_now = true;
    return true;
}

void Kernel::Impl::resume_from_wait(Uid uid, std::uint32_t result) {
    Thread *t = thread(uid);
    if (t == nullptr) return;
    t->status &= ~thread_status::kWait;
    if ((t->status & (thread_status::kWait | thread_status::kSuspend | thread_status::kDormant |
                      thread_status::kDead)) == 0) {
        change_ready_state(*t, true);
    }
    // Non-waiting threads do not process callbacks.
    t->processing_callbacks = false;
    t->ret = result;
}

bool Kernel::Impl::verify_wait(Uid uid, WaitType type, Uid id) const {
    // Like PPSSPP this checks waitType/waitID only, not the WAIT bit: a thread
    // that timed out keeps matching until it actually runs.
    const Thread *t = thread(uid);
    return t != nullptr && t->wait_id_for(type) == id;
}

void Kernel::Impl::write_timeout_left(std::uint32_t timeout_ptr, std::int64_t left_us) {
    if (timeout_ptr == 0) return;
    mem.write_u32(timeout_ptr, static_cast<std::uint32_t>(left_us));
}

namespace {

template <class T>
bool remove_waiter(std::vector<T> &list, Uid thread) {
    const auto before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [thread](const T &w) { return w.thread == thread; }),
               list.end());
    return list.size() != before;
}

bool remove_waiter(std::vector<Uid> &list, Uid thread) {
    const auto before = list.size();
    list.erase(std::remove(list.begin(), list.end(), thread), list.end());
    return list.size() != before;
}

} // namespace

void Kernel::Impl::wait_begin_callback(Thread &t) {
    // HLEKernel::WaitBeginCallback for each wait type that has one.
    if (t.paused.active) return; // Two callbacks in a row.
    const bool has_timeout = t.timeout_ptr != 0;
    auto pause = [&](TimerKind timer, bool do_timeout) {
        std::uint64_t deadline = 0;
        if (do_timeout)
            deadline = static_cast<std::uint64_t>(static_cast<std::int64_t>(now()) +
                                                  unschedule(timer, t.uid));
        t.paused = PausedWait{};
        t.paused.active = true;
        t.paused.deadline = deadline;
    };
    switch (t.wait_type) {
    case WaitType::kDelay:
        if (t.wait_id == t.uid) pause(TimerKind::kWakeup, true);
        break;
    case WaitType::kSema:
        if (auto *s = get<Semaphore>(t.wait_id)) {
            pause(TimerKind::kSemaTimeout, has_timeout);
            remove_waiter(s->waiting, t.uid);
        }
        break;
    case WaitType::kMutex:
        if (auto *m = get<Mutex>(t.wait_id)) {
            pause(TimerKind::kMutexTimeout, has_timeout);
            remove_waiter(m->waiting, t.uid);
        }
        break;
    case WaitType::kThreadEnd:
        if (auto *target = get<Thread>(t.wait_id)) {
            pause(TimerKind::kThreadEndTimeout, has_timeout);
            remove_waiter(target->waiting_threads, t.uid);
        }
        break;
    case WaitType::kEventFlag:
        if (auto *e = get<EventFlag>(t.wait_id)) {
            auto it = std::find_if(e->waiting.begin(), e->waiting.end(),
                                   [&](const EventFlagWaiter &w) { return w.thread == t.uid; });
            if (it == e->waiting.end()) break; // WAIT_CB_BAD_WAIT_DATA: nothing paused.
            const EventFlagWaiter waiter = *it;
            e->waiting.erase(it);
            pause(TimerKind::kEventFlagTimeout, has_timeout);
            t.paused.data0 = waiter.bits;
            t.paused.data1 = waiter.mode;
            t.paused.data2 = waiter.out_addr;
        }
        break;
    case WaitType::kFpl:
    case WaitType::kVpl: {
        std::vector<PoolWaiter> *list = nullptr;
        if (t.wait_type == WaitType::kFpl) {
            if (auto *f = get<Fpl>(t.wait_id)) list = &f->waiting;
        } else if (auto *v = get<Vpl>(t.wait_id)) {
            list = &v->waiting;
        }
        if (list == nullptr) break;
        auto it = std::find_if(list->begin(), list->end(),
                               [&](const PoolWaiter &w) { return w.thread == t.uid; });
        if (it == list->end()) break;
        const PoolWaiter waiter = *it;
        list->erase(it);
        pause(t.wait_type == WaitType::kFpl ? TimerKind::kFplTimeout : TimerKind::kVplTimeout,
              has_timeout);
        t.paused.data0 = waiter.addr_ptr;
        break;
    }
    default:
        // Sleep has nothing to pause; other wait types belong to other modules.
        break;
    }
}

void Kernel::Impl::wait_end_callback(Thread &t) {
    // HLEKernel::WaitEndCallback and the per-type end functions.
    const PausedWait paused = t.paused;
    t.paused = PausedWait{};
    const std::uint32_t timeout_ptr = t.timeout_ptr;
    bool woke = false;

    if (t.wait_type == WaitType::kSleep) {
        if (t.wakeup_count > 0) {
            --t.wakeup_count;
            resume_from_wait(t.uid, 0);
        }
        return;
    }
    if (t.wait_type == WaitType::kDelay) {
        if (!paused.active) {
            resume_from_wait(t.uid, 0);
            return;
        }
        const std::int64_t left =
            static_cast<std::int64_t>(paused.deadline) - static_cast<std::int64_t>(now());
        if (left < 0) {
            resume_from_wait(t.uid, 0);
        } else {
            schedule(TimerKind::kWakeup, t.uid, static_cast<std::uint64_t>(left));
        }
        return;
    }

    TimerKind timer{};
    bool object_alive = false;
    switch (t.wait_type) {
    case WaitType::kSema:
        timer = TimerKind::kSemaTimeout;
        object_alive = get<Semaphore>(t.wait_id) != nullptr;
        break;
    case WaitType::kMutex:
        timer = TimerKind::kMutexTimeout;
        object_alive = get<Mutex>(t.wait_id) != nullptr;
        break;
    case WaitType::kThreadEnd:
        timer = TimerKind::kThreadEndTimeout;
        object_alive = get<Thread>(t.wait_id) != nullptr;
        break;
    case WaitType::kEventFlag:
        timer = TimerKind::kEventFlagTimeout;
        object_alive = get<EventFlag>(t.wait_id) != nullptr;
        break;
    case WaitType::kFpl:
        timer = TimerKind::kFplTimeout;
        object_alive = get<Fpl>(t.wait_id) != nullptr;
        break;
    case WaitType::kVpl:
        timer = TimerKind::kVplTimeout;
        object_alive = get<Vpl>(t.wait_id) != nullptr;
        break;
    default:
        return;
    }

    if (!object_alive || !paused.active) {
        // Deleted while the callback ran: PPSSPP reports the full time as used.
        if (timeout_ptr != 0) mem.write_u32(timeout_ptr, 0);
        resume_from_wait(t.uid, error::kWaitDelete);
        return;
    }

    // Try to finish the wait first.
    bool unlocked = false;
    switch (t.wait_type) {
    case WaitType::kSema:
        unlocked = unlock_sema_for_thread(*get<Semaphore>(t.wait_id), t.uid, 0, woke);
        break;
    case WaitType::kMutex: {
        Mutex &m = *get<Mutex>(t.wait_id);
        unlocked = m.lock_thread == -1 && unlock_mutex_for_thread(m, t.uid, 0);
        break;
    }
    case WaitType::kThreadEnd: {
        Thread &target = *get<Thread>(t.wait_id);
        if (!verify_wait(t.uid, WaitType::kThreadEnd, target.uid)) {
            unlocked = true;
        } else if (target.status == thread_status::kDormant) {
            const std::int64_t left = unschedule(TimerKind::kThreadEndTimeout, t.uid);
            if (timeout_ptr != 0) write_timeout_left(timeout_ptr, left);
            resume_from_wait(t.uid, target.exit_status);
            unlocked = true;
        }
        break;
    }
    case WaitType::kEventFlag: {
        EventFlagWaiter waiter{t.uid, paused.data0, paused.data1, paused.data2};
        unlocked = unlock_event_flag_for_thread(*get<EventFlag>(t.wait_id), waiter, 0, woke);
        break;
    }
    case WaitType::kFpl: {
        PoolWaiter waiter{t.uid, paused.data0};
        unlocked = unlock_fpl_for_thread(*get<Fpl>(t.wait_id), waiter, 0, woke);
        break;
    }
    case WaitType::kVpl: {
        PoolWaiter waiter{t.uid, paused.data0};
        unlocked = unlock_vpl_for_thread(*get<Vpl>(t.wait_id), waiter, 0, woke);
        break;
    }
    default:
        break;
    }
    if (unlocked) return;

    // Only check the timeout if it couldn't finish.
    const std::int64_t left =
        static_cast<std::int64_t>(paused.deadline) - static_cast<std::int64_t>(now());
    if (left < 0 && paused.deadline != 0) {
        if (timeout_ptr != 0) mem.write_u32(timeout_ptr, 0);
        resume_from_wait(t.uid, error::kWaitTimeout);
        return;
    }
    if (timeout_ptr != 0) schedule(timer, t.uid, static_cast<std::uint64_t>(left < 0 ? 0 : left));
    // Resume waiting, at the end of the list.
    switch (t.wait_type) {
    case WaitType::kSema:
        get<Semaphore>(t.wait_id)->waiting.push_back(t.uid);
        break;
    case WaitType::kMutex:
        get<Mutex>(t.wait_id)->waiting.push_back(t.uid);
        break;
    case WaitType::kThreadEnd:
        get<Thread>(t.wait_id)->waiting_threads.push_back(t.uid);
        break;
    case WaitType::kEventFlag:
        get<EventFlag>(t.wait_id)->waiting.push_back(
            EventFlagWaiter{t.uid, paused.data0, paused.data1, paused.data2});
        break;
    case WaitType::kFpl:
        get<Fpl>(t.wait_id)->waiting.push_back(PoolWaiter{t.uid, paused.data0});
        break;
    case WaitType::kVpl:
        get<Vpl>(t.wait_id)->waiting.push_back(PoolWaiter{t.uid, paused.data0});
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Callbacks.

bool Kernel::Impl::current_has_ready_callbacks() const {
    if (ready_callbacks_count == 0 || current == nullptr) return false;
    for (Uid id : current->callbacks) {
        const Callback *cb = get<Callback>(id);
        if (cb != nullptr && cb->notify_count != 0) return true;
    }
    return false;
}

std::optional<CallbackCall> Kernel::Impl::take_ready_callback(Thread &t) {
    for (Uid id : t.callbacks) {
        Callback *cb = get<Callback>(id);
        if (cb != nullptr && cb->notify_count != 0) {
            CallbackCall call;
            call.callback = id;
            call.entry = cb->entry;
            call.args = {static_cast<std::uint32_t>(cb->notify_count), cb->notify_arg,
                         cb->common_arg};
            cb->notify_count = 0;
            cb->notify_arg = 0;
            --ready_callbacks_count;
            return call;
        }
    }
    return std::nullopt;
}

void Kernel::Impl::start_callback_on_thread(Thread &t, const CallbackCall &call,
                                            bool resched_after) {
    // __KernelCallAddress: save the wait, pause it, make the thread ready.
    SavedWaitState saved;
    saved.status = t.status;
    saved.wait_type = t.wait_type;
    saved.wait_id = t.wait_id;
    saved.wait_value = t.wait_value;
    saved.timeout_ptr = t.timeout_ptr;
    saved.processing_callbacks = t.processing_callbacks;
    if (t.wait_type != WaitType::kNone) {
        wait_begin_callback(t);
        t.wait_type = WaitType::kNone;
    }
    change_thread_state(t, thread_status::kReady);
    t.callback_pending = true;
    t.pending_resched_after = resched_after;
    t.pending_saved = saved;
    t.pending_call = call;
}

bool Kernel::Impl::check_thread_callbacks(Thread &t, bool force) {
    if (!t.processing_callbacks && !force) return false;
    // Already has a call queued, or is inside one: the running callback loop
    // picks up whatever became ready.
    if (t.callback_pending || t.callback_depth > 0) return false;
    std::optional<CallbackCall> call = take_ready_callback(t);
    if (!call) return false;
    start_callback_on_thread(t, *call, !force);
    return true;
}

bool Kernel::Impl::check_callbacks() {
    if (ready_callbacks_count == 0) return false;
    if (in_interrupt || !dispatch_allowed() || in_callback_count != 0) return false;
    bool processed = false;
    const std::vector<Uid> threads = thread_queue;
    for (Uid uid : threads) {
        Thread *t = thread(uid);
        if (t != nullptr && check_thread_callbacks(*t, false)) processed = true;
    }
    return processed;
}

bool Kernel::Impl::force_callbacks() {
    if (ready_callbacks_count == 0) return false;
    Thread *cur = current;
    if (cur == nullptr) return false;
    if (!check_thread_callbacks(*cur, true)) return false;
    // Runs right away on the current thread (it is the caller).
    change_thread_state(*cur, thread_status::kRunning);
    return true;
}

bool Kernel::Impl::run_pending_callbacks(Thread &t) {
    t.callback_pending = false;
    const SavedWaitState saved = t.pending_saved;
    const bool resched_after = t.pending_resched_after;
    CallbackCall call = t.pending_call;
    ++t.callback_depth;
    ++in_callback_count;
    for (;;) {
        const std::uint32_t saved_ret = t.ret;
        const std::uint32_t v0 = hooks.call_guest(kernel, t.uid, call.entry, call.args);
        t.ret = saved_ret;
        // ActionAfterCallback: queue the next ready one, then drop this one if
        // it returned non-zero.
        std::optional<CallbackCall> next = take_ready_callback(t);
        if (v0 != 0) {
            if (Callback *cb = get<Callback>(call.callback)) destroy_callback(*cb);
        }
        if (!next) break;
        call = *next;
    }
    --t.callback_depth;
    --in_callback_count;

    // ActionAfterMipsCall: put the wait back, unless the thread was stopped.
    if ((t.status & (thread_status::kDead | thread_status::kDormant)) == 0) {
        change_ready_state(t, (saved.status & thread_status::kReady) != 0);
        t.status = saved.status;
    }
    t.wait_type = saved.wait_type;
    t.wait_id = saved.wait_id;
    t.wait_value = saved.wait_value;
    t.timeout_ptr = saved.timeout_ptr;
    t.processing_callbacks = saved.processing_callbacks;
    if (t.wait_type != WaitType::kNone) wait_end_callback(t);

    const bool runnable = (t.status & (thread_status::kReady | thread_status::kRunning)) != 0;
    return resched_after || !runnable;
}

void Kernel::Impl::destroy_callback(Callback &cb) {
    if (Thread *owner = thread(cb.thread)) remove_waiter(owner->callbacks, cb.uid);
    if (cb.notify_count != 0) --ready_callbacks_count;
    destroy_object(cb.uid, ObjType::kCallback, error::kUnknownCbid);
}

void Kernel::Impl::notify_callback(Uid uid, std::uint32_t notify_arg) {
    Callback *cb = get<Callback>(uid);
    if (cb == nullptr) return;
    if (cb->notify_count == 0) ++ready_callbacks_count;
    ++cb->notify_count;
    cb->notify_arg = notify_arg;
}

// ---------------------------------------------------------------------------
// Timers.

void Kernel::Impl::schedule(TimerKind kind, Uid thread_uid, std::uint64_t delay_us) {
    TimerEvent event;
    event.kind = kind;
    event.thread = thread_uid;
    events.emplace(std::make_pair(now() + delay_us, event_seq++), std::move(event));
}

std::int64_t Kernel::Impl::unschedule(TimerKind kind, Uid thread_uid) {
    std::int64_t left = 0;
    for (auto it = events.begin(); it != events.end();) {
        if (it->second.kind == kind && it->second.thread == thread_uid) {
            left = static_cast<std::int64_t>(it->first.first) - static_cast<std::int64_t>(now());
            it = events.erase(it);
        } else {
            ++it;
        }
    }
    return left;
}

void Kernel::Impl::advance_events() {
    while (!events.empty()) {
        auto it = events.begin();
        if (it->first.first > now()) break;
        TimerEvent event = std::move(it->second);
        events.erase(it);
        ++in_event_dispatch;
        fire(event);
        --in_event_dispatch;
    }
}

void Kernel::Impl::fire(TimerEvent &event) {
    switch (event.kind) {
    case TimerKind::kWakeup:
        wakeup_timeout(event.thread);
        break;
    case TimerKind::kThreadEndTimeout:
        wait_exec_timeout<Thread>(event.thread, WaitType::kThreadEnd);
        break;
    case TimerKind::kSemaTimeout:
        sema_timeout(event.thread);
        break;
    case TimerKind::kEventFlagTimeout:
        event_flag_timeout(event.thread);
        break;
    case TimerKind::kMutexTimeout:
        wait_exec_timeout<Mutex>(event.thread, WaitType::kMutex);
        break;
    case TimerKind::kFplTimeout:
        wait_exec_timeout<Fpl>(event.thread, WaitType::kFpl);
        break;
    case TimerKind::kVplTimeout:
        vpl_timeout(event.thread);
        break;
    case TimerKind::kHleDelay:
        hle_delay_timeout(event.thread);
        break;
    case TimerKind::kHost:
        if (event.host) event.host();
        break;
    }
}

void Kernel::Impl::wakeup_timeout(Uid thread_uid) {
    const Thread *t = thread(thread_uid);
    if (t != nullptr && t->wait_id_for(WaitType::kDelay) == thread_uid)
        resume_from_wait(thread_uid, 0);
}

void Kernel::Impl::hle_delay_timeout(Uid thread_uid) {
    const Thread *t = thread(thread_uid);
    if (t != nullptr && t->wait_id_for(WaitType::kHleDelay) == 1)
        resume_from_wait(thread_uid, t->wait_value);
}

// ---------------------------------------------------------------------------
// Scheduler loop (the host side of the fibers).

RunResult Kernel::run(std::uint64_t until_us) {
    Impl &k = *impl_;
    if (k.fiber_thread != nullptr) fatal("Kernel::run() called from a thread fiber");
    k.stop_requested = false;
    for (;;) {
        k.destroy_zombies();
        if (k.stop_requested) return RunResult::kStopped;
        if (k.now() >= until_us) return RunResult::kDeadline;
        Thread *t = k.current;
        if (t != nullptr && t->is_running()) {
            k.resume_fiber(*t);
            continue;
        }
        // The idle thread (__KernelIdle): fire what is due, then reschedule,
        // which also picks up callbacks those timers notified.
        k.advance_events();
        k.reschedule_core();
        if (k.current != nullptr && k.current->is_running()) continue;
        if (k.events.empty()) return RunResult::kIdle;
        const std::uint64_t next = k.events.begin()->first.first;
        if (next >= until_us) {
            k.clock.wait_until(until_us);
            return RunResult::kDeadline;
        }
        k.clock.wait_until(next);
    }
}

void Kernel::request_stop() {
    impl_->stop_requested = true;
}

void Kernel::preemption_point() {
    Impl &k = *impl_;
    Thread *self = k.fiber_thread;
    k.reschedule_core();
    if (self == nullptr) return;
    // A stop request hands control back to run(); the thread stays current and
    // continues from here on the next run().
    if (k.stop_requested && k.current == self && self->fiber) self->fiber->yield();
    k.block_until_running(*self);
}

} // namespace mpo::kernel
