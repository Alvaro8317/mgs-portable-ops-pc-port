// SPDX-License-Identifier: GPL-3.0-or-later
//
// Semaphores, event flags and mutexes.
//
// Ported from PPSSPP Core/HLE/sceKernelSemaphore.cpp, sceKernelEventFlag.cpp
// and sceKernelMutex.cpp @ v1.20.4 (GPL-2.0-or-later). Lightweight mutexes are
// not ported: no module of the game imports them.

#include "kernel/kernel_impl.hpp"

#include <algorithm>

namespace mpo::kernel {

namespace {

constexpr std::uint32_t kSemaAttrPriority = 0x100;
constexpr std::uint32_t kEventWaitMultiple = 0x200;
constexpr std::uint32_t kEventWaitOr = 0x01;
constexpr std::uint32_t kEventWaitClearAll = 0x10;
constexpr std::uint32_t kEventWaitClear = 0x20;
constexpr std::uint32_t kEventWaitKnown = kEventWaitClear | kEventWaitClearAll | kEventWaitOr;
constexpr std::uint32_t kMutexAttrPriority = 0x100;
constexpr std::uint32_t kMutexAttrAllowRecursive = 0x200;

} // namespace

namespace {

bool thread_sort_priority(const Kernel::Impl &k, Uid a, Uid b) {
    const Thread *ta = k.thread(a);
    const Thread *tb = k.thread(b);
    const std::int32_t pa = ta != nullptr ? ta->current_priority : 0;
    const std::int32_t pb = tb != nullptr ? tb->current_priority : 0;
    return pa < pb;
}

void write_native_header(Kernel::Impl &k, std::uint32_t address, std::uint32_t size,
                         const std::string &name) {
    k.mem.write_u32(address, size);
    k.write_name(address + 4, name);
}

} // namespace

// ---------------------------------------------------------------------------
// Semaphores.

bool Kernel::Impl::unlock_sema_for_thread(Semaphore &s, Uid thread_uid, std::uint32_t result,
                                          bool &woke) {
    if (!verify_wait(thread_uid, WaitType::kSema, s.uid)) return true;
    Thread *t = thread(thread_uid);
    // If result is an error code, the thread is just let go.
    if (result == 0) {
        const auto wanted = static_cast<std::int32_t>(t->wait_value);
        if (wanted > s.current_count) return false;
        s.current_count -= wanted;
    }
    const std::uint32_t timeout_ptr = t->timeout_ptr;
    if (timeout_ptr != 0) {
        std::int64_t left = unschedule(TimerKind::kSemaTimeout, thread_uid);
        if (left < 0) left = 0;
        write_timeout_left(timeout_ptr, left);
    }
    resume_from_wait(thread_uid, result);
    woke = true;
    return true;
}

bool Kernel::Impl::clear_sema_threads(Semaphore &s, std::uint32_t reason) {
    bool woke = false;
    const std::vector<Uid> waiting = s.waiting;
    for (Uid uid : waiting) unlock_sema_for_thread(s, uid, reason, woke);
    s.waiting.clear();
    return woke;
}

void Kernel::Impl::sema_timeout(Uid thread_uid) {
    const Thread *t = thread(thread_uid);
    const Uid uid = t != nullptr ? t->wait_id_for(WaitType::kSema) : 0;
    wait_exec_timeout<Semaphore>(thread_uid, WaitType::kSema);
    // In FIFO mode, the timeout may have cleared the way for the next thread.
    Semaphore *s = get<Semaphore>(uid);
    if (s != nullptr && (s->attr & kSemaAttrPriority) == 0) {
        bool woke = false;
        while (!s->waiting.empty() && unlock_sema_for_thread(*s, s->waiting.front(), 0, woke)) {
            s->waiting.erase(s->waiting.begin());
        }
    }
}

std::uint32_t Kernel::create_sema(const char *name, std::uint32_t attr, std::int32_t initial,
                                  std::int32_t max, std::uint32_t option_addr) {
    Impl &k = *impl_;
    if (name == nullptr) return error::kError;
    if (attr >= 0x200) return error::kIllegalAttr;
    auto s = std::make_unique<Semaphore>();
    s->name = Impl::clip_name(name);
    s->attr = attr;
    s->initial_count = initial;
    s->current_count = initial;
    s->max_count = max;
    const Uid uid = k.create_object(std::move(s));
    // Many games pass garbage options; they have none, so they never fail.
    (void)option_addr;
    return static_cast<std::uint32_t>(uid);
}

std::uint32_t Kernel::delete_sema(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Semaphore *s = k.get<Semaphore>(uid, error);
    if (s == nullptr) return error;
    CallScope call(k);
    if (k.clear_sema_threads(*s, error::kWaitDelete)) k.request_resched();
    return call.finish(k.destroy_object(uid, ObjType::kSema, error::kUnknownSemid));
}

std::uint32_t Kernel::signal_sema(Uid uid, std::int32_t count) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Semaphore *s = k.get<Semaphore>(uid, error);
    if (s == nullptr) return error;
    if (s->current_count + count - static_cast<std::int32_t>(s->waiting.size()) > s->max_count) {
        return error::kSemaOvf;
    }
    CallScope call(k);
    s->current_count += count;
    if ((s->attr & kSemaAttrPriority) != 0) {
        std::stable_sort(s->waiting.begin(), s->waiting.end(),
                         [&k](Uid a, Uid b) { return thread_sort_priority(k, a, b); });
    }
    bool woke = false;
    for (std::size_t i = 0; i < s->waiting.size();) {
        if (k.unlock_sema_for_thread(*s, s->waiting[i], 0, woke)) {
            s->waiting.erase(s->waiting.begin() + static_cast<std::ptrdiff_t>(i));
            i = 0; // PPSSPP restarts from the front after each removal.
        } else {
            ++i;
        }
    }
    if (woke) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::Impl::wait_sema_impl(Uid uid, std::int32_t count, std::uint32_t timeout_ptr,
                                           bool callbacks) {
    if (!dispatch_allowed()) return error::kCanNotWait;
    if (in_interrupt) return error::kIllegalContext;
    if (count <= 0) return error::kIllegalCount;
    std::uint32_t error = 0;
    Semaphore *s = get<Semaphore>(uid, error);
    if (s == nullptr) return error;
    if (count > s->max_count) return error::kIllegalCount;
    // With callbacks ready the thread always waits, and wakes after them.
    const bool has_callbacks = callbacks && current_has_ready_callbacks();
    if (s->current_count >= count && s->waiting.empty() && !has_callbacks) {
        s->current_count -= count;
        return 0;
    }
    if (!on_current_fiber()) return error::kIllegalContext;
    CallScope call(*this);
    const Uid cur = current->uid;
    // It may be looping on timeouts (still listed): no duplicates.
    if (std::find(s->waiting.begin(), s->waiting.end(), cur) == s->waiting.end())
        s->waiting.push_back(cur);
    if (timeout_ptr != 0) {
        auto micro = static_cast<std::int32_t>(mem.read_u32(timeout_ptr));
        // This is how the hardware seems to time things.
        if (micro <= 3) {
            micro = 24;
        } else if (micro <= 249) {
            micro = 245;
        }
        schedule(TimerKind::kSemaTimeout, cur, static_cast<std::uint64_t>(micro));
    }
    wait_current(WaitType::kSema, uid, static_cast<std::uint32_t>(count), timeout_ptr, callbacks);
    return call.finish(0);
}

std::uint32_t Kernel::wait_sema(Uid uid, std::int32_t count, std::uint32_t timeout_addr) {
    return impl_->wait_sema_impl(uid, count, timeout_addr, false);
}

std::uint32_t Kernel::wait_sema_cb(Uid uid, std::int32_t count, std::uint32_t timeout_addr) {
    return impl_->wait_sema_impl(uid, count, timeout_addr, true);
}

std::uint32_t Kernel::poll_sema(Uid uid, std::int32_t count) {
    Impl &k = *impl_;
    if (count <= 0) return error::kIllegalCount;
    std::uint32_t error = 0;
    Semaphore *s = k.get<Semaphore>(uid, error);
    if (s == nullptr) return error;
    if (s->current_count >= count && s->waiting.empty()) {
        s->current_count -= count;
        return 0;
    }
    return error::kSemaZero;
}

std::uint32_t Kernel::cancel_sema(Uid uid, std::int32_t new_count,
                                  std::uint32_t num_wait_threads_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Semaphore *s = k.get<Semaphore>(uid, error);
    if (s == nullptr) return error;
    if (new_count > s->max_count) return error::kIllegalCount;
    CallScope call(k);
    s->num_wait_threads = static_cast<std::int32_t>(s->waiting.size());
    if (k.mem.is_valid(num_wait_threads_addr)) {
        k.mem.write_u32(num_wait_threads_addr, static_cast<std::uint32_t>(s->num_wait_threads));
    }
    s->current_count = new_count < 0 ? s->initial_count : new_count;
    if (k.clear_sema_threads(*s, error::kWaitCancel)) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::refer_sema_status(Uid uid, std::uint32_t info_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Semaphore *s = k.get<Semaphore>(uid, error);
    if (s == nullptr) return error;
    if (!k.mem.is_valid(info_addr, 56)) return 0xFFFFFFFFu;
    cleanup_waiting(k, WaitType::kSema, uid, s->waiting);
    s->num_wait_threads = static_cast<std::int32_t>(s->waiting.size());
    if (k.mem.read_u32(info_addr) != 0) {
        write_native_header(k, info_addr, 56, s->name);
        k.mem.write_u32(info_addr + 36, s->attr);
        k.mem.write_u32(info_addr + 40, static_cast<std::uint32_t>(s->initial_count));
        k.mem.write_u32(info_addr + 44, static_cast<std::uint32_t>(s->current_count));
        k.mem.write_u32(info_addr + 48, static_cast<std::uint32_t>(s->max_count));
        k.mem.write_u32(info_addr + 52, static_cast<std::uint32_t>(s->num_wait_threads));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Event flags.

namespace {

bool event_flag_matches(std::uint32_t pattern, std::uint32_t bits, std::uint32_t mode) {
    if ((mode & kEventWaitOr) != 0) return (bits & pattern) != 0;
    return (bits & pattern) == bits;
}

} // namespace

bool Kernel::Impl::apply_event_flag_match(EventFlag &e, std::uint32_t bits, std::uint32_t mode,
                                          std::uint32_t out_addr) {
    if (!event_flag_matches(e.current_pattern, bits, mode)) return false;
    if (mem.is_valid(out_addr)) mem.write_u32(out_addr, e.current_pattern);
    if ((mode & kEventWaitClear) != 0) e.current_pattern &= ~bits;
    if ((mode & kEventWaitClearAll) != 0) e.current_pattern = 0;
    return true;
}

bool Kernel::Impl::unlock_event_flag_for_thread(EventFlag &e, EventFlagWaiter &waiter,
                                                std::uint32_t result, bool &woke) {
    if (!verify_wait(waiter.thread, WaitType::kEventFlag, e.uid)) return true;
    if (result == 0) {
        if (!apply_event_flag_match(e, waiter.bits, waiter.mode, waiter.out_addr)) return false;
    } else if (mem.is_valid(waiter.out_addr)) {
        // Bailing out: report the current pattern.
        mem.write_u32(waiter.out_addr, e.current_pattern);
    }
    const std::uint32_t timeout_ptr = thread(waiter.thread)->timeout_ptr;
    if (timeout_ptr != 0) {
        const std::int64_t left = unschedule(TimerKind::kEventFlagTimeout, waiter.thread);
        write_timeout_left(timeout_ptr, left);
    }
    resume_from_wait(waiter.thread, result);
    woke = true;
    return true;
}

bool Kernel::Impl::clear_event_flag_threads(EventFlag &e, std::uint32_t reason) {
    bool woke = false;
    std::vector<EventFlagWaiter> waiting = e.waiting;
    for (auto &waiter : waiting) unlock_event_flag_for_thread(e, waiter, reason, woke);
    e.waiting.clear();
    return woke;
}

void Kernel::Impl::event_flag_timeout(Uid thread_uid) {
    // Still needs to write the result pointer of the wait.
    const Thread *t = thread(thread_uid);
    if (t == nullptr) return;
    EventFlag *e = get<EventFlag>(t->wait_id_for(WaitType::kEventFlag));
    if (e == nullptr) return;
    if (t->timeout_ptr != 0) mem.write_u32(t->timeout_ptr, 0);
    for (auto &waiter : e->waiting) {
        if (waiter.thread == thread_uid) {
            // Stays listed until it runs, so a delete still reports DELETE.
            bool woke = false;
            EventFlagWaiter copy = waiter;
            unlock_event_flag_for_thread(*e, copy, error::kWaitTimeout, woke);
            break;
        }
    }
}

std::uint32_t Kernel::create_event_flag(const char *name, std::uint32_t attr,
                                        std::uint32_t initial_pattern, std::uint32_t option_addr) {
    Impl &k = *impl_;
    if (name == nullptr) return error::kError;
    if ((attr & 0x100) != 0 || attr >= 0x300) return error::kIllegalAttr;
    auto e = std::make_unique<EventFlag>();
    e->name = Impl::clip_name(name);
    e->attr = attr;
    e->initial_pattern = initial_pattern;
    e->current_pattern = initial_pattern;
    (void)option_addr;
    return static_cast<std::uint32_t>(k.create_object(std::move(e)));
}

std::uint32_t Kernel::cancel_event_flag(Uid uid, std::uint32_t pattern,
                                        std::uint32_t num_wait_threads_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    EventFlag *e = k.get<EventFlag>(uid, error);
    if (e == nullptr) return error;
    CallScope call(k);
    e->num_wait_threads = static_cast<std::int32_t>(e->waiting.size());
    if (k.mem.is_valid(num_wait_threads_addr)) {
        k.mem.write_u32(num_wait_threads_addr, static_cast<std::uint32_t>(e->num_wait_threads));
    }
    e->current_pattern = pattern;
    if (k.clear_event_flag_threads(*e, error::kWaitCancel)) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::clear_event_flag(Uid uid, std::uint32_t bits) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    EventFlag *e = k.get<EventFlag>(uid, error);
    if (e == nullptr) return error;
    // Clearing can't wake anyone.
    e->current_pattern &= bits;
    return 0;
}

std::uint32_t Kernel::delete_event_flag(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    EventFlag *e = k.get<EventFlag>(uid, error);
    if (e == nullptr) return error;
    CallScope call(k);
    if (k.clear_event_flag_threads(*e, error::kWaitDelete)) k.request_resched();
    return call.finish(k.destroy_object(uid, ObjType::kEventFlag, error::kUnknownEvfid));
}

std::uint32_t Kernel::set_event_flag(Uid uid, std::uint32_t bits) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    EventFlag *e = k.get<EventFlag>(uid, error);
    if (e == nullptr) return error;
    CallScope call(k);
    bool woke = false;
    e->current_pattern |= bits;
    for (std::size_t i = 0; i < e->waiting.size();) {
        EventFlagWaiter waiter = e->waiting[i];
        if (k.unlock_event_flag_for_thread(*e, waiter, 0, woke)) {
            // Try the one that moved into this place next.
            e->waiting.erase(e->waiting.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (woke) k.request_resched();
    return call.finish(0);
}

namespace {

std::uint32_t event_flag_wait(Kernel::Impl &k, Uid uid, std::uint32_t bits, std::uint32_t mode,
                              std::uint32_t out_bits_addr, std::uint32_t timeout_addr,
                              bool callbacks) {
    if (k.in_interrupt) return error::kIllegalContext;
    if ((mode & ~kEventWaitKnown) != 0) return error::kIllegalMode;
    // Waiting on 0 would wait forever.
    if (bits == 0) return error::kEvfIlpat;
    if (!k.dispatch_allowed()) return error::kCanNotWait;
    std::uint32_t error = 0;
    EventFlag *e = k.get<EventFlag>(uid, error);
    if (e == nullptr) return error;
    CallScope call(k);
    // With callbacks, only check here: CLEAR/CLEARALL apply once they're done.
    bool do_wait = false;
    bool callback_wait = false;
    if (callbacks) {
        do_wait = !event_flag_matches(e->current_pattern, bits, mode);
        if (k.current_has_ready_callbacks()) {
            do_wait = true;
            callback_wait = true;
        }
    } else {
        do_wait = !k.apply_event_flag_match(*e, bits, mode, out_bits_addr);
    }
    if (!do_wait) {
        if (callbacks) {
            k.apply_event_flag_match(*e, bits, mode, out_bits_addr);
            k.request_current_callbacks();
        }
        return call.finish(0);
    }
    if (!k.on_current_fiber()) return call.finish(error::kIllegalContext);
    const Uid cur = k.current->uid;
    // Left listed after a timeout? Remove it, or outBits could go to the wrong place.
    e->waiting.erase(std::remove_if(e->waiting.begin(), e->waiting.end(),
                                    [cur](const EventFlagWaiter &w) { return w.thread == cur; }),
                     e->waiting.end());
    std::uint32_t timeout = 0xFFFFFFFFu;
    if (k.mem.is_valid(timeout_addr)) timeout = k.mem.read_u32(timeout_addr);
    if (!e->waiting.empty() && (e->attr & kEventWaitMultiple) == 0)
        return call.finish(error::kEvfMulti);
    // If < 5ms, sometimes hardware doesn't write this, but it's unpredictable.
    e->waiting.push_back(EventFlagWaiter{cur, bits, mode, timeout == 0 ? 0u : out_bits_addr});
    if (timeout_addr != 0) {
        auto micro = static_cast<std::int32_t>(k.mem.read_u32(timeout_addr));
        if (micro <= 1) {
            micro = 25;
        } else if (micro <= 209) {
            micro = 240;
        }
        k.schedule(TimerKind::kEventFlagTimeout, cur, static_cast<std::uint64_t>(micro));
    }
    if (callback_wait) {
        k.wait_callbacks_current(WaitType::kEventFlag, uid, 0, timeout_addr);
    } else {
        k.wait_current(WaitType::kEventFlag, uid, 0, timeout_addr, callbacks);
    }
    return call.finish(0);
}

} // namespace

std::uint32_t Kernel::wait_event_flag(Uid uid, std::uint32_t bits, std::uint32_t wait_mode,
                                      std::uint32_t out_bits_addr, std::uint32_t timeout_addr) {
    return event_flag_wait(*impl_, uid, bits, wait_mode, out_bits_addr, timeout_addr, false);
}

std::uint32_t Kernel::wait_event_flag_cb(Uid uid, std::uint32_t bits, std::uint32_t wait_mode,
                                         std::uint32_t out_bits_addr, std::uint32_t timeout_addr) {
    return event_flag_wait(*impl_, uid, bits, wait_mode, out_bits_addr, timeout_addr, true);
}

std::uint32_t Kernel::poll_event_flag(Uid uid, std::uint32_t bits, std::uint32_t wait_mode,
                                      std::uint32_t out_bits_addr) {
    Impl &k = *impl_;
    if ((wait_mode & ~kEventWaitKnown) != 0) return error::kIllegalMode;
    // Poll also fails when CLEAR and CLEARALL are used together.
    if ((wait_mode & kEventWaitClear) != 0 && (wait_mode & kEventWaitClearAll) != 0)
        return error::kIllegalMode;
    if (bits == 0) return error::kEvfIlpat;
    std::uint32_t error = 0;
    EventFlag *e = k.get<EventFlag>(uid, error);
    if (e == nullptr) return error;
    if (k.apply_event_flag_match(*e, bits, wait_mode, out_bits_addr)) return 0;
    if (k.mem.is_valid(out_bits_addr)) k.mem.write_u32(out_bits_addr, e->current_pattern);
    if (!e->waiting.empty() && (e->attr & kEventWaitMultiple) == 0) return error::kEvfMulti;
    return error::kEvfCond;
}

std::uint32_t Kernel::refer_event_flag_status(Uid uid, std::uint32_t info_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    EventFlag *e = k.get<EventFlag>(uid, error);
    if (e == nullptr) return error;
    if (!k.mem.is_valid(info_addr, 52)) return 0xFFFFFFFFu;
    cleanup_waiting(k, WaitType::kEventFlag, uid, e->waiting);
    e->num_wait_threads = static_cast<std::int32_t>(e->waiting.size());
    if (k.mem.read_u32(info_addr) != 0) {
        write_native_header(k, info_addr, 52, e->name);
        k.mem.write_u32(info_addr + 36, e->attr);
        k.mem.write_u32(info_addr + 40, e->initial_pattern);
        k.mem.write_u32(info_addr + 44, e->current_pattern);
        k.mem.write_u32(info_addr + 48, static_cast<std::uint32_t>(e->num_wait_threads));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Mutexes.

void Kernel::Impl::mutex_acquire_lock(Mutex &m, std::int32_t count, Uid thread_uid) {
    mutex_held_locks.emplace(thread_uid, m.uid);
    m.lock_level = count;
    m.lock_thread = thread_uid;
}

void Kernel::Impl::mutex_erase_lock(Mutex &m) {
    if (m.lock_thread != -1) {
        auto range = mutex_held_locks.equal_range(m.lock_thread);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == m.uid) {
                mutex_held_locks.erase(it);
                break;
            }
        }
    }
    m.lock_thread = -1;
}

bool Kernel::Impl::unlock_mutex_for_thread(Mutex &m, Uid thread_uid, std::uint32_t result) {
    if (!verify_wait(thread_uid, WaitType::kMutex, m.uid)) return false;
    Thread *t = thread(thread_uid);
    if (result == 0) mutex_acquire_lock(m, static_cast<std::int32_t>(t->wait_value), thread_uid);
    if (t->timeout_ptr != 0) {
        const std::int64_t left = unschedule(TimerKind::kMutexTimeout, thread_uid);
        write_timeout_left(t->timeout_ptr, left);
    }
    resume_from_wait(thread_uid, result);
    return true;
}

bool Kernel::Impl::lock_mutex_check(Mutex *m, std::int32_t count, std::uint32_t &error) {
    if (error != 0) return false;
    const bool recursive = (m->attr & kMutexAttrAllowRecursive) != 0;
    const Uid cur = current != nullptr ? current->uid : 0;
    if (count <= 0) {
        error = error::kIllegalCount;
    } else if (count > 1 && !recursive) {
        error = error::kIllegalCount;
    } else if (static_cast<std::int64_t>(count) + m->lock_level > 0x7FFFFFFF) {
        // Two positive ints always overflow to negative.
        error = error::kMutexLockOverflow;
    } else if (m->lock_thread == cur) {
        // Only a recursive mutex can re-lock.
        if (recursive) return true;
        error = error::kMutexAlreadyLocked;
    } else if (m->lock_level == 0) {
        return true;
    }
    return false;
}

bool Kernel::Impl::lock_mutex(Mutex *m, std::int32_t count, std::uint32_t &error) {
    if (!lock_mutex_check(m, count, error)) return false;
    const Uid cur = current != nullptr ? current->uid : 0;
    if (m->lock_level == 0) {
        mutex_acquire_lock(*m, count, cur);
        return true;
    }
    if (m->lock_thread == cur) {
        m->lock_level += count;
        return true;
    }
    return false;
}

bool Kernel::Impl::unlock_mutex(Mutex &m) {
    mutex_erase_lock(m);
    bool woke = false;
    while (!woke && !m.waiting.empty()) {
        auto it = m.waiting.begin();
        if ((m.attr & kMutexAttrPriority) != 0) {
            std::int32_t best = 0x7FFFFFFF;
            for (auto i = m.waiting.begin(); i != m.waiting.end(); ++i) {
                const Thread *t = thread(*i);
                const std::int32_t priority = t != nullptr ? t->current_priority : 0;
                if (priority < best) {
                    best = priority;
                    it = i;
                }
            }
        }
        woke |= unlock_mutex_for_thread(m, *it, 0);
        m.waiting.erase(it);
    }
    if (!woke) m.lock_thread = -1;
    return woke;
}

void Kernel::Impl::mutex_thread_end(Uid thread_uid) {
    // If it was waiting on a mutex, it isn't anymore.
    if (const Thread *t = thread(thread_uid)) {
        if (Mutex *m = get<Mutex>(t->wait_id_for(WaitType::kMutex))) {
            m->waiting.erase(std::remove(m->waiting.begin(), m->waiting.end(), thread_uid),
                             m->waiting.end());
        }
    }
    // Unlock every mutex it held.
    std::vector<Uid> held;
    auto range = mutex_held_locks.equal_range(thread_uid);
    for (auto it = range.first; it != range.second; ++it) held.push_back(it->second);
    for (Uid mutex_uid : held) {
        if (Mutex *m = get<Mutex>(mutex_uid)) {
            m->lock_level = 0;
            unlock_mutex(*m);
        }
    }
}

void Kernel::Impl::set_mutex_timeout(std::uint32_t timeout_ptr) {
    if (timeout_ptr == 0 || current == nullptr) return;
    auto micro = static_cast<std::int32_t>(mem.read_u32(timeout_ptr));
    if (micro <= 3) {
        micro = 25;
    } else if (micro <= 249) {
        micro = 250;
    }
    schedule(TimerKind::kMutexTimeout, current->uid, static_cast<std::uint64_t>(micro));
}

std::uint32_t Kernel::create_mutex(const char *name, std::uint32_t attr, std::int32_t initial_count,
                                   std::uint32_t option_addr) {
    Impl &k = *impl_;
    if (name == nullptr) return error::kError;
    if ((attr & ~0xBFFu) != 0) return error::kIllegalAttr;
    if (initial_count < 0) return error::kIllegalCount;
    if ((attr & kMutexAttrAllowRecursive) == 0 && initial_count > 1) return error::kIllegalCount;
    auto owned = std::make_unique<Mutex>();
    Mutex *m = owned.get();
    m->name = Impl::clip_name(name);
    m->attr = attr;
    m->initial_count = initial_count;
    const Uid uid = k.create_object(std::move(owned));
    if (initial_count == 0) {
        m->lock_level = 0;
        m->lock_thread = -1;
    } else {
        k.mutex_acquire_lock(*m, initial_count, k.current != nullptr ? k.current->uid : 0);
    }
    (void)option_addr;
    return static_cast<std::uint32_t>(uid);
}

std::uint32_t Kernel::delete_mutex(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Mutex *m = k.get<Mutex>(uid, error);
    if (m == nullptr) return error;
    CallScope call(k);
    bool woke = false;
    for (Uid waiting : m->waiting)
        woke |= k.unlock_mutex_for_thread(*m, waiting, error::kWaitDelete);
    if (m->lock_thread != -1) k.mutex_erase_lock(*m);
    m->waiting.clear();
    if (woke) k.request_resched();
    return call.finish(k.destroy_object(uid, ObjType::kMutex, error::kMutexNoSuchMutex));
}

std::uint32_t Kernel::lock_mutex(Uid uid, std::int32_t count, std::uint32_t timeout_addr) {
    Impl &k = *impl_;
    if (!k.dispatch_allowed()) return error::kCanNotWait;
    if (k.in_interrupt) return error::kIllegalContext;
    std::uint32_t error = 0;
    Mutex *m = k.get<Mutex>(uid, error);
    if (k.lock_mutex(m, count, error)) return 0;
    if (error != 0) return error;
    if (!k.on_current_fiber()) return error::kIllegalContext;
    CallScope call(k);
    const Uid cur = k.current->uid;
    if (std::find(m->waiting.begin(), m->waiting.end(), cur) == m->waiting.end())
        m->waiting.push_back(cur);
    k.set_mutex_timeout(timeout_addr);
    k.wait_current(WaitType::kMutex, uid, static_cast<std::uint32_t>(count), timeout_addr, false);
    // The wait overwrites this.
    return call.finish(0);
}

std::uint32_t Kernel::lock_mutex_cb(Uid uid, std::int32_t count, std::uint32_t timeout_addr) {
    Impl &k = *impl_;
    if (!k.dispatch_allowed()) return error::kCanNotWait;
    if (k.in_interrupt) return error::kIllegalContext;
    std::uint32_t error = 0;
    Mutex *m = k.get<Mutex>(uid, error);
    if (!k.lock_mutex_check(m, count, error)) {
        if (error != 0) return error;
        if (!k.on_current_fiber()) return error::kIllegalContext;
        CallScope call(k);
        const Uid cur = k.current->uid;
        if (std::find(m->waiting.begin(), m->waiting.end(), cur) == m->waiting.end())
            m->waiting.push_back(cur);
        k.set_mutex_timeout(timeout_addr);
        k.wait_current(WaitType::kMutex, uid, static_cast<std::uint32_t>(count), timeout_addr,
                       true);
        return call.finish(0);
    }
    CallScope call(k);
    if (k.current_has_ready_callbacks()) {
        // Might actually end up having to wait, so set the timeout.
        k.set_mutex_timeout(timeout_addr);
        k.wait_callbacks_current(WaitType::kMutex, uid, static_cast<std::uint32_t>(count),
                                 timeout_addr);
    } else {
        k.lock_mutex(m, count, error);
    }
    return call.finish(0);
}

std::uint32_t Kernel::try_lock_mutex(Uid uid, std::int32_t count) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Mutex *m = k.get<Mutex>(uid, error);
    if (k.lock_mutex(m, count, error)) return 0;
    if (error != 0) return error;
    return error::kMutexTrylockFailed;
}

std::uint32_t Kernel::unlock_mutex(Uid uid, std::int32_t count) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Mutex *m = k.get<Mutex>(uid, error);
    if (m == nullptr) return error;
    if (count <= 0) return error::kIllegalCount;
    if ((m->attr & kMutexAttrAllowRecursive) == 0 && count > 1) return error::kIllegalCount;
    const Uid cur = k.current != nullptr ? k.current->uid : 0;
    if (m->lock_level == 0 || m->lock_thread != cur) return error::kMutexNotLocked;
    if (m->lock_level < count) return error::kMutexUnlockUnderflow;
    CallScope call(k);
    m->lock_level -= count;
    if (m->lock_level == 0 && k.unlock_mutex(*m)) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::cancel_mutex(Uid uid, std::int32_t count,
                                   std::uint32_t num_wait_threads_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Mutex *m = k.get<Mutex>(uid, error);
    if (m == nullptr) return error;
    const bool lockable = count <= 0 || k.lock_mutex_check(m, count, error);
    if (!lockable && error != 0 && error != error::kMutexLockOverflow &&
        error != error::kMutexAlreadyLocked) {
        return error;
    }
    CallScope call(k);
    // Drop threads no longer waiting first, so the count is right.
    cleanup_waiting(k, WaitType::kMutex, uid, m->waiting);
    if (k.mem.is_valid(num_wait_threads_addr)) {
        k.mem.write_u32(num_wait_threads_addr, static_cast<std::uint32_t>(m->waiting.size()));
    }
    bool woke = false;
    for (Uid waiting : m->waiting)
        woke |= k.unlock_mutex_for_thread(*m, waiting, error::kWaitCancel);
    if (m->lock_thread != -1) k.mutex_erase_lock(*m);
    m->waiting.clear();
    if (count <= 0) {
        m->lock_level = 0;
        m->lock_thread = -1;
    } else {
        k.mutex_acquire_lock(*m, count, k.current != nullptr ? k.current->uid : 0);
    }
    if (woke) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::refer_mutex_status(Uid uid, std::uint32_t info_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Mutex *m = k.get<Mutex>(uid, error);
    if (m == nullptr) return error;
    if (!k.mem.is_valid(info_addr, 56)) return 0xFFFFFFFFu;
    // Don't write if the size is 0.
    if (k.mem.read_u32(info_addr) != 0) {
        cleanup_waiting(k, WaitType::kMutex, uid, m->waiting);
        m->num_wait_threads = static_cast<std::int32_t>(m->waiting.size());
        write_native_header(k, info_addr, 56, m->name);
        k.mem.write_u32(info_addr + 36, m->attr);
        k.mem.write_u32(info_addr + 40, static_cast<std::uint32_t>(m->initial_count));
        k.mem.write_u32(info_addr + 44, static_cast<std::uint32_t>(m->lock_level));
        k.mem.write_u32(info_addr + 48, static_cast<std::uint32_t>(m->lock_thread));
        k.mem.write_u32(info_addr + 52, static_cast<std::uint32_t>(m->num_wait_threads));
    }
    return 0;
}

} // namespace mpo::kernel
