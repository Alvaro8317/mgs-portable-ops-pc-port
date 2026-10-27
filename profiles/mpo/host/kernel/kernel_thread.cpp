// SPDX-License-Identifier: GPL-3.0-or-later
//
// Threads, delays, sleep/wakeup, callbacks, interrupt/dispatch state and the
// services other HLE modules use.
//
// Ported from PPSSPP Core/HLE/sceKernelThread.cpp, sceKernelInterrupt.cpp,
// sceKernelTime.cpp and HLE.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "kernel/kernel_impl.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mpo::kernel {

namespace {

constexpr std::uint32_t kThreadAttrUserMask = 0xF8F060FFu;
constexpr std::uint32_t kThreadAttrUserErase = 0x78800000u;
constexpr std::uint32_t kThreadInfoSize = 104;
constexpr std::uint32_t kThreadInfoSizeAfter260 = 108;
constexpr std::uint32_t kSdk308 = 0x03080000u;

// Encodings PPSSPP writes under a starting thread's arguments.
constexpr std::uint32_t kMipsBranchSelf = 0x1000FFFFu; // b -1
constexpr std::uint32_t kMipsNop = 0;

std::int64_t delay_thread_us(std::uint64_t usec) {
    if (usec < 200) return 210;
    if (usec > 0x8000000000000000ULL) usec -= 0x8000000000000000ULL;
    if (usec > 0x0010000000000000ULL) usec >>= 12;
    // It never wakes up right away.
    return static_cast<std::int64_t>(usec + 10);
}

void put_u32(std::uint8_t *bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

} // namespace

Kernel::Kernel(const KernelConfig &config) {
    if (config.memory == nullptr || config.clock == nullptr || config.hooks == nullptr) {
        std::fprintf(stderr, "mpo_kernel: KernelConfig needs memory, clock and hooks\n");
        std::abort();
    }
    impl_ = std::make_unique<Impl>(*this, config);
}

Kernel::~Kernel() = default;

Uid Kernel::current_thread() const {
    return impl_->current != nullptr ? impl_->current->uid : 0;
}

std::uint64_t Kernel::now_us() const {
    return impl_->now();
}

GuestMemory &Kernel::memory() const {
    return impl_->mem;
}

// ---------------------------------------------------------------------------
// Interrupt and dispatch state.

std::uint32_t Kernel::cpu_suspend_intr() {
    Impl &k = *impl_;
    if (k.interrupts_enabled) {
        k.interrupts_enabled = false;
        return 1;
    }
    return 0;
}

void Kernel::cpu_resume_intr(std::uint32_t enable) {
    Impl &k = *impl_;
    CallScope call(k);
    if (enable != 0) {
        k.interrupts_enabled = true;
        if (k.on_interrupts_resumed) k.on_interrupts_resumed();
        k.request_resched();
    } else {
        k.interrupts_enabled = false;
    }
    call.finish(0);
}

bool Kernel::interrupts_enabled() const {
    return impl_->interrupts_enabled;
}

void Kernel::set_interrupts_resumed_hook(std::function<void()> hook) {
    impl_->on_interrupts_resumed = std::move(hook);
}

void Kernel::set_in_interrupt(bool in_interrupt) {
    impl_->in_interrupt = in_interrupt;
}

bool Kernel::dispatch_enabled() const {
    return impl_->dispatch_allowed();
}

bool Kernel::in_interrupt() const {
    return impl_->in_interrupt;
}

std::uint32_t Kernel::suspend_dispatch_thread() {
    Impl &k = *impl_;
    if (k.in_interrupt) return error::kIllegalContext;
    if (!k.interrupts_enabled) return error::kCpuDi;
    const bool old = k.dispatch_enabled;
    k.dispatch_enabled = false;
    return old ? 1u : 0u;
}

std::uint32_t Kernel::resume_dispatch_thread(std::uint32_t enabled) {
    Impl &k = *impl_;
    if (k.in_interrupt) return error::kIllegalContext;
    if (!k.interrupts_enabled) return error::kCpuDi;
    CallScope call(k);
    k.dispatch_enabled = enabled != 0;
    k.request_resched();
    return call.finish(0);
}

// ---------------------------------------------------------------------------
// Thread objects.

Thread *Kernel::Impl::create_thread_object(Uid &uid, Uid module_id, std::uint32_t gp,
                                           const char *name, std::uint32_t entry,
                                           std::int32_t priority, std::uint32_t stack_size,
                                           std::uint32_t attr) {
    auto owned = std::make_unique<Thread>();
    Thread *t = owned.get();
    uid = create_object(std::move(owned));
    thread_queue.push_back(uid);

    t->entry = entry;
    // The firmware adds 0xFF to the attributes on create.
    t->attr = attr | 0xFFu;
    t->initial_priority = t->current_priority = priority;
    t->stack_size = stack_size;
    t->status = thread_status::kDormant;
    t->exit_status = error::kDormant;
    t->wait_type = WaitType::kNone;
    t->gp = gp;
    t->module_id = module_id;
    t->name = clip_name(name);

    // PSPThread::AllocateStack: from the top of the partition unless LOW_STACK.
    t->stack_allocator = (t->attr & thread_attr::kKernel) != 0 ? &kernel_memory : &user_memory;
    const bool from_top = (t->attr & thread_attr::kLowStack) == 0;
    std::uint32_t size = stack_size;
    const std::string tag = "stack/" + t->name;
    const std::uint32_t start = t->stack_allocator->alloc(size, from_top, tag.c_str());
    if (start == BlockAllocator::kFailed) {
        t->stack_start = 0;
        t->initial_stack = 0;
    } else {
        t->stack_start = start;
        t->initial_stack = start;
        t->stack_size = size;
    }
    return t;
}

void Kernel::Impl::fill_stack(Thread &t) {
    if ((t.attr & thread_attr::kNoFillStack) == 0) mem.fill(t.stack_start, 0xFF, t.stack_size);
    std::uint32_t sp = t.stack_start + t.stack_size;
    t.stack_end = sp;
    // The k0 section is 256 bytes at the top of the stack.
    sp -= 256;
    const std::uint32_t k0 = sp;
    mem.fill(k0, 0, 0x100);
    mem.write_u32(k0 + 0xC0, static_cast<std::uint32_t>(t.uid));
    mem.write_u32(k0 + 0xC8, t.initial_stack);
    mem.write_u32(k0 + 0xF8, 0xFFFFFFFFu);
    mem.write_u32(k0 + 0xFC, 0xFFFFFFFFu);
    mem.write_u32(t.initial_stack, static_cast<std::uint32_t>(t.uid));
    t.start.sp = sp;
    t.start.k0 = k0;
}

void Kernel::Impl::free_stack(Thread &t) {
    if (t.stack_start == 0) return;
    if ((t.attr & thread_attr::kClearStack) != 0 && t.initial_stack != 0) {
        mem.fill(t.initial_stack, 0, t.stack_size);
    }
    t.stack_allocator->free(t.stack_start);
    t.stack_start = 0;
}

void Kernel::Impl::reset_thread(Thread &t, std::int32_t lowest_priority) {
    // A restarted thread gets a fresh fiber; the old one is discarded.
    release_fiber(t);
    t.start = ThreadStart{};
    t.start.thread = t.uid;
    t.start.entry = t.entry;
    t.start.attr = t.attr;
    // If the thread would be better than lowest_priority, reset to its initial.
    if (t.current_priority < lowest_priority) t.current_priority = t.initial_priority;
    t.wait_type = WaitType::kNone;
    t.wait_id = 0;
    t.wait_value = 0;
    t.timeout_ptr = 0;
    t.exit_status = error::kNotDormant;
    t.processing_callbacks = false;
    t.callback_pending = false;
    t.callback_depth = 0;
    t.paused = PausedWait{};
    t.start.gp = t.gp;
    fill_stack(t);
}

std::uint32_t Kernel::Impl::create_thread_checked(const char *name, Uid module_id, std::uint32_t gp,
                                                  std::uint32_t entry, std::uint32_t priority,
                                                  std::uint32_t stack_size, std::uint32_t attr,
                                                  std::uint32_t option_addr, bool allow_kernel) {
    (void)option_addr; // PPSSPP ignores the options too.
    if (name == nullptr) return error::kError;
    for (int i = 0; i < 4 && name[i] != 0; ++i) {
        if (static_cast<std::uint8_t>(name[i]) >= 0xF0) {
            name = "badThreadName";
            break;
        }
    }
    if (stack_size < 0x200) return error::kIllegalStackSize;
    if (priority < 0x08 || priority > 0x77) return error::kIllegalPriority;
    if (!mem.is_valid(entry) && entry != 0) return error::kIllegalAddr;
    if ((attr & ~kThreadAttrUserMask) != 0 && !allow_kernel) return error::kIllegalAttr;
    attr &= ~kThreadAttrUserErase;
    if ((attr & thread_attr::kKernel) == 0) {
        if (allow_kernel && (attr & thread_attr::kUser) == 0) {
            attr |= thread_attr::kKernel;
        } else {
            attr |= thread_attr::kUser;
        }
    }
    Uid uid = 0;
    Thread *t = create_thread_object(uid, module_id, gp, name, entry,
                                     static_cast<std::int32_t>(priority), stack_size, attr);
    // PPSSPP leaves the object (and its UID) behind when the stack can't be had.
    if (t->stack_start == 0) return error::kNoMemory;
    // Creating a thread resumes dispatch automatically.
    dispatch_enabled = true;
    request_resched();
    return static_cast<std::uint32_t>(uid);
}

std::uint32_t Kernel::create_thread(const char *name, std::uint32_t entry, std::uint32_t priority,
                                    std::uint32_t stack_size, std::uint32_t attr,
                                    std::uint32_t option_addr) {
    Impl &k = *impl_;
    if (k.in_interrupt) return error::kIllegalContext;
    CallScope call(k);
    const Thread *cur = k.current;
    const bool allow_kernel =
        k.kernel_mode || (cur != nullptr && (cur->attr & thread_attr::kKernel) != 0);
    const Uid module = cur != nullptr ? cur->module_id : 0;
    const std::uint32_t gp = cur != nullptr ? cur->gp : 0;
    return call.finish(k.create_thread_checked(name, module, gp, entry, priority, stack_size, attr,
                                               option_addr, allow_kernel));
}

std::uint32_t Kernel::create_thread_for_module(const char *name, Uid module_id, std::uint32_t gp,
                                               std::uint32_t entry, std::uint32_t priority,
                                               std::uint32_t stack_size, std::uint32_t attr,
                                               bool allow_kernel) {
    Impl &k = *impl_;
    CallScope call(k);
    return call.finish(k.create_thread_checked(name, module_id, gp, entry, priority, stack_size,
                                               attr, 0, allow_kernel));
}

std::uint32_t Kernel::start_thread(Uid uid, std::uint32_t arg_size, std::uint32_t arg_block,
                                   bool force_args) {
    Impl &k = *impl_;
    if (k.in_interrupt) return error::kIllegalContext;
    if (uid == 0) return error::kIllegalThid;
    if (static_cast<std::int32_t>(arg_size) < 0 || (arg_block & 0x80000000u) != 0)
        return error::kIllegalAddr;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (t->status != thread_status::kDormant) return error::kNotDormant;

    CallScope call(k);
    Thread *cur = k.current;
    k.reset_thread(*t, cur != nullptr ? cur->current_priority : 0);
    std::uint32_t sp = t->start.sp;
    if (force_args) {
        t->start.a0 = arg_size;
        t->start.a1 = arg_block;
    } else if (arg_block != 0 && arg_size > 0) {
        // Room for the arguments, always 0x10 aligned.
        sp -= (arg_size + 0xFu) & ~0xFu;
        t->start.a0 = arg_size;
        t->start.a1 = sp;
        if (k.mem.is_valid(arg_block)) k.mem.copy(sp, arg_block, arg_size);
    } else {
        t->start.a0 = 0;
        t->start.a1 = 0;
    }
    // The firmware eats 64 more bytes; the return trap sits at their bottom.
    sp -= 64;
    k.hooks.write_thread_return_stub(*this, sp);
    k.mem.write_u32(sp + 8, kMipsBranchSelf);
    k.mem.write_u32(sp + 12, kMipsNop);
    t->start.sp = sp;
    t->start.ra = sp;
    t->start.fp = sp;

    // Smaller is better: only switch if the new thread is better.
    if (cur != nullptr && cur->is_running() && cur->current_priority > t->current_priority &&
        k.interrupts_enabled) {
        k.change_ready_state(*cur, true);
        k.request_resched();
    }
    // Starting a worse (or equal) thread resumes dispatch, oddly.
    if (cur != nullptr && cur->current_priority <= t->current_priority) k.dispatch_enabled = true;
    k.change_ready_state(*t, true);
    return call.finish(0);
}

void Kernel::Impl::thread_end_listeners(Thread &t) {
    // __KernelCancelWakeup, __KernelCancelThreadEndTimeout, mutex release.
    // Every other wait timer of the thread goes too: PPSSPP leaves them armed,
    // and a stale one could time out a later wait of a restarted thread.
    for (auto it = events.begin(); it != events.end();) {
        if (it->second.kind != TimerKind::kHost && it->second.thread == t.uid) {
            it = events.erase(it);
        } else {
            ++it;
        }
    }
    mutex_thread_end(t.uid);
}

void Kernel::Impl::stop_thread(Thread &t, std::uint32_t exit_status) {
    change_ready_state(t, false);
    t.exit_status = exit_status;
    t.status = thread_status::kDormant;
    // A thread stopped inside a callback never returns from it.
    in_callback_count -= t.callback_depth;
    t.callback_depth = 0;
    t.callback_pending = false;
    thread_end_listeners(t);
    for (Uid waiting : t.waiting_threads) {
        Thread *w = thread(waiting);
        if (w == nullptr) continue;
        const std::uint32_t timeout_ptr = w->timeout_ptr;
        if (verify_wait(waiting, WaitType::kThreadEnd, t.uid)) {
            const std::int64_t left = unschedule(TimerKind::kThreadEndTimeout, waiting);
            if (timeout_ptr != 0) write_timeout_left(timeout_ptr, left);
            resume_from_wait(waiting, exit_status);
        }
    }
    t.waiting_threads.clear();
    // Stopped threads are never waiting.
    t.wait_type = WaitType::kNone;
    t.wait_id = 0;
}

std::uint32_t Kernel::Impl::delete_thread_internal(Uid uid, std::uint32_t exit_status) {
    Thread *t = thread(uid);
    if (t == nullptr) return error::kUnknownThid;
    stop_thread(*t, exit_status);
    ready_remove(t->current_priority, uid);
    thread_queue.erase(std::remove(thread_queue.begin(), thread_queue.end(), uid),
                       thread_queue.end());
    if (current == t) current = nullptr;
    // Callbacks are deleted with their thread.
    for (Uid cb_uid : t->callbacks) {
        if (Callback *cb = get<Callback>(cb_uid)) {
            if (cb->notify_count != 0) --ready_callbacks_count;
            destroy_object(cb_uid, ObjType::kCallback, error::kUnknownCbid);
        }
    }
    t->callbacks.clear();
    free_stack(*t);
    t->status = thread_status::kDead;
    std::unique_ptr<Object> owned = take_object(uid);
    auto *dead = static_cast<Thread *>(owned.release());
    std::unique_ptr<Thread> dead_thread(dead);
    if (fiber_thread == dead) {
        // Deleting itself: its fiber is the one running this code.
        if (last_resumed == uid) last_resumed = 0;
        hooks.on_thread_released(kernel, uid);
        zombies.push_back(std::move(dead_thread));
    } else {
        release_fiber(*dead);
    }
    return 0;
}

void Kernel::Impl::thread_returned(std::uint32_t exit_status) {
    Thread *t = fiber_thread;
    if (t == nullptr) return;
    stop_thread(*t, exit_status);
    reschedule_core();
}

std::uint32_t Kernel::exit_thread(std::uint32_t exit_status) {
    Impl &k = *impl_;
    if (!k.dispatch_allowed() && k.sdk_version >= kSdk308 && k.has_sdk_version)
        return error::kCanNotWait;
    Thread *t = k.current;
    if (t == nullptr) return 0;
    CallScope call(k);
    if (static_cast<std::int32_t>(exit_status) < 0) exit_status = error::kIllegalArgument;
    k.stop_thread(*t, exit_status);
    k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::exit_delete_thread(std::uint32_t exit_status) {
    Impl &k = *impl_;
    if (!k.dispatch_allowed() && k.sdk_version >= kSdk308 && k.has_sdk_version)
        return error::kCanNotWait;
    Thread *t = k.current;
    if (t == nullptr) return 0;
    CallScope call(k);
    k.delete_thread_internal(t->uid, exit_status);
    k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::delete_thread(Uid uid) {
    Impl &k = *impl_;
    if (uid == 0 || (k.current != nullptr && uid == k.current->uid)) return error::kNotDormant;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (!t->is_stopped()) return error::kNotDormant;
    CallScope call(k);
    return call.finish(k.delete_thread_internal(uid, error::kThreadTerminated));
}

std::uint32_t Kernel::terminate_delete_thread(Uid uid) {
    Impl &k = *impl_;
    if (uid == 0 || (k.current != nullptr && uid == k.current->uid)) return error::kIllegalThid;
    if (!k.dispatch_allowed() && k.sdk_version >= kSdk308 && k.has_sdk_version)
        return error::kCanNotWait;
    std::uint32_t error = 0;
    if (k.get<Thread>(uid, error) == nullptr) return error;
    CallScope call(k);
    return call.finish(k.delete_thread_internal(uid, error::kThreadTerminated));
}

std::uint32_t Kernel::terminate_thread(Uid uid) {
    Impl &k = *impl_;
    if (k.in_interrupt && k.sdk_version >= kSdk308 && k.has_sdk_version)
        return error::kIllegalContext;
    if (!k.dispatch_allowed() && k.sdk_version >= kSdk308 && k.has_sdk_version)
        return error::kCanNotWait;
    if (uid == 0 || (k.current != nullptr && uid == k.current->uid)) return error::kIllegalThid;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (t->is_stopped()) return error::kDormant;
    CallScope call(k);
    k.stop_thread(*t, error::kThreadTerminated);
    // On terminate the priority is reset; on exit it isn't always.
    t->current_priority = t->initial_priority;
    return call.finish(0);
}

std::uint32_t Kernel::get_thread_id() {
    Impl &k = *impl_;
    if (k.in_interrupt) return error::kIllegalContext;
    return k.current != nullptr ? static_cast<std::uint32_t>(k.current->uid) : 0u;
}

std::uint32_t Kernel::get_thread_current_priority() {
    Impl &k = *impl_;
    return k.current != nullptr ? static_cast<std::uint32_t>(k.current->current_priority) : 0u;
}

std::uint32_t Kernel::change_current_thread_attr(std::uint32_t clear_attr, std::uint32_t set_attr) {
    Impl &k = *impl_;
    if (((clear_attr | set_attr) & ~thread_attr::kVfpu) != 0) return error::kIllegalAttr;
    if (k.current == nullptr) return 0xFFFFFFFFu;
    k.current->attr = (k.current->attr & ~clear_attr) | set_attr;
    return 0;
}

std::uint32_t Kernel::change_thread_priority(Uid uid, std::uint32_t priority) {
    Impl &k = *impl_;
    if (uid == 0 && k.current != nullptr) uid = k.current->uid;
    // 0 means the current (running) thread's priority, not the target's.
    if (priority == 0 && k.current != nullptr)
        priority = static_cast<std::uint32_t>(k.current->current_priority);
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (t->is_stopped()) return error::kDormant;
    if (priority < 0x08 || priority > 0x77) return error::kIllegalPriority;
    CallScope call(k);
    k.ready_remove(t->current_priority, uid);
    t->current_priority = static_cast<std::int32_t>(priority);
    if (t->is_running()) t->status = (t->status & ~thread_status::kRunning) | thread_status::kReady;
    if (t->is_ready()) k.ready_push_back(t->current_priority, uid);
    k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::rotate_thread_ready_queue(std::uint32_t priority) {
    Impl &k = *impl_;
    Thread *cur = k.current;
    if (cur == nullptr) return error::kIllegalContext;
    if (priority == 0) priority = static_cast<std::uint32_t>(cur->current_priority);
    if (priority <= 0x07 || priority > 0x77) return error::kIllegalPriority;
    CallScope call(k);
    auto &queue = k.ready[priority];
    if (!queue.empty()) {
        if (cur->current_priority == static_cast<std::int32_t>(priority)) {
            // Yield to everyone else at this priority.
            queue.push_back(cur->uid);
            cur->status = (cur->status & ~thread_status::kRunning) | thread_status::kReady;
        } else if (queue.size() > 1) {
            queue.push_back(queue.front());
            queue.pop_front();
        }
    }
    k.request_resched();
    return call.finish(0);
}

// ---------------------------------------------------------------------------
// Delays, sleep and wakeup.

std::uint32_t Kernel::Impl::delay_thread_impl(std::uint64_t usec, bool callbacks) {
    if (!dispatch_allowed()) return error::kCanNotWait;
    if (in_interrupt) return error::kIllegalContext;
    if (!on_current_fiber()) return error::kIllegalContext;
    CallScope call(*this);
    const Uid cur = current->uid;
    schedule(TimerKind::kWakeup, cur, static_cast<std::uint64_t>(delay_thread_us(usec)));
    wait_current(WaitType::kDelay, cur, 0, 0, callbacks);
    return call.finish(0);
}

std::uint32_t Kernel::delay_thread(std::uint32_t usec) {
    return impl_->delay_thread_impl(usec, false);
}

std::uint32_t Kernel::delay_thread_cb(std::uint32_t usec) {
    return impl_->delay_thread_impl(usec, true);
}

std::uint32_t Kernel::delay_sys_clock_thread(std::uint32_t sysclock_addr) {
    Impl &k = *impl_;
    if (!k.mem.is_valid(sysclock_addr, 8)) return error::kIllegalAddress;
    const std::uint64_t usec =
        k.mem.read_u32(sysclock_addr) |
        (static_cast<std::uint64_t>(k.mem.read_u32(sysclock_addr + 4)) << 32);
    return k.delay_thread_impl(usec, false);
}

std::uint32_t Kernel::delay_sys_clock_thread_cb(std::uint32_t sysclock_addr) {
    Impl &k = *impl_;
    if (!k.mem.is_valid(sysclock_addr, 8)) return error::kIllegalAddress;
    const std::uint64_t usec =
        k.mem.read_u32(sysclock_addr) |
        (static_cast<std::uint64_t>(k.mem.read_u32(sysclock_addr + 4)) << 32);
    return k.delay_thread_impl(usec, true);
}

std::uint32_t Kernel::Impl::sleep_thread_impl(bool callbacks) {
    if (!dispatch_allowed()) return error::kCanNotWait;
    if (in_interrupt) return error::kIllegalContext;
    if (!on_current_fiber()) return error::kIllegalContext;
    CallScope call(*this);
    Thread *t = current;
    if (t->wakeup_count > 0) {
        --t->wakeup_count;
        return call.finish(0);
    }
    wait_current(WaitType::kSleep, 0, 0, 0, callbacks);
    return call.finish(0);
}

std::uint32_t Kernel::sleep_thread() {
    return impl_->sleep_thread_impl(false);
}

std::uint32_t Kernel::sleep_thread_cb() {
    return impl_->sleep_thread_impl(true);
}

std::uint32_t Kernel::wakeup_thread(Uid uid) {
    Impl &k = *impl_;
    if (k.current != nullptr && uid == k.current->uid) return error::kIllegalThid;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    CallScope call(k);
    if (!t->is_waiting_for(WaitType::kSleep, 0)) {
        ++t->wakeup_count;
    } else {
        k.resume_from_wait(uid, 0);
        k.request_resched();
    }
    return call.finish(0);
}

std::uint32_t Kernel::cancel_wakeup_thread(Uid uid) {
    Impl &k = *impl_;
    if (uid == 0 && k.current != nullptr) uid = k.current->uid;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    const std::int32_t count = t->wakeup_count;
    t->wakeup_count = 0;
    return static_cast<std::uint32_t>(count);
}

std::uint32_t Kernel::Impl::wait_thread_end_impl(Uid uid, std::uint32_t timeout_ptr,
                                                 bool callbacks) {
    if (uid == 0 || (current != nullptr && uid == current->uid)) return error::kIllegalThid;
    if (!dispatch_allowed()) return error::kCanNotWait;
    if (in_interrupt) return error::kIllegalContext;
    std::uint32_t error = 0;
    Thread *t = get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (!on_current_fiber()) return error::kIllegalContext;
    CallScope call(*this);
    const Uid cur = current->uid;
    if (t->status != thread_status::kDormant) {
        if (mem.is_valid(timeout_ptr))
            schedule(TimerKind::kThreadEndTimeout, cur, mem.read_u32(timeout_ptr));
        if (std::find(t->waiting_threads.begin(), t->waiting_threads.end(), cur) ==
            t->waiting_threads.end()) {
            t->waiting_threads.push_back(cur);
        }
        wait_current(WaitType::kThreadEnd, uid, 0, timeout_ptr, callbacks);
    } else if (callbacks) {
        request_current_callbacks();
    }
    return call.finish(t->exit_status);
}

std::uint32_t Kernel::wait_thread_end(Uid uid, std::uint32_t timeout_addr) {
    return impl_->wait_thread_end_impl(uid, timeout_addr, false);
}

std::uint32_t Kernel::wait_thread_end_cb(Uid uid, std::uint32_t timeout_addr) {
    return impl_->wait_thread_end_impl(uid, timeout_addr, true);
}

std::uint32_t Kernel::release_wait_thread(Uid uid) {
    Impl &k = *impl_;
    if (uid == 0 || (k.current != nullptr && uid == k.current->uid)) return error::kIllegalThid;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (!t->is_waiting()) return error::kNotWait;
    if (t->wait_type == WaitType::kHleDelay || t->wait_type == WaitType::kModule)
        return error::kNotWait;
    CallScope call(k);
    k.resume_from_wait(uid, error::kReleaseWait);
    k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::suspend_thread(Uid uid) {
    Impl &k = *impl_;
    if (uid == 0 || (k.current != nullptr && uid == k.current->uid)) return error::kIllegalThid;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (t->is_stopped()) return error::kDormant;
    if (t->is_suspended()) return error::kSuspend;
    if (t->is_ready()) k.change_ready_state(*t, false);
    t->status = (t->status & ~thread_status::kReady) | thread_status::kSuspend;
    return 0;
}

std::uint32_t Kernel::resume_thread(Uid uid) {
    Impl &k = *impl_;
    if (uid == 0 || (k.current != nullptr && uid == k.current->uid)) return error::kIllegalThid;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (!t->is_suspended()) return error::kNotSuspend;
    CallScope call(k);
    t->status &= ~thread_status::kSuspend;
    // If it was dormant, waiting, etc. before, its ready state stays.
    if (t->status == 0) k.change_ready_state(*t, true);
    k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::refer_thread_status(Uid uid, std::uint32_t status_addr) {
    Impl &k = *impl_;
    if (uid == 0 && k.current != nullptr) uid = k.current->uid;
    CallScope call(k);
    k.request_resched();
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return call.finish(error);
    const std::uint32_t wanted = k.mem.read_u32(status_addr);

    std::uint8_t native[kThreadInfoSize] = {};
    const bool after_260 = k.has_sdk_version && k.sdk_version > 0x02060010u;
    if (after_260 && wanted > kThreadInfoSizeAfter260) return call.finish(error::kIllegalSize);
    put_u32(native, 0, after_260 ? kThreadInfoSizeAfter260 : kThreadInfoSize);
    for (std::size_t i = 0; i < t->name.size() && i < 31; ++i)
        native[4 + i] = static_cast<std::uint8_t>(t->name[i]);
    put_u32(native, 36, t->attr);
    put_u32(native, 40, t->status);
    put_u32(native, 44, t->entry);
    put_u32(native, 48, t->initial_stack);
    put_u32(native, 52, t->stack_size);
    put_u32(native, 56, t->gp);
    put_u32(native, 60, static_cast<std::uint32_t>(t->initial_priority));
    put_u32(native, 64, static_cast<std::uint32_t>(t->current_priority));
    put_u32(native, 68, static_cast<std::uint32_t>(t->wait_type));
    put_u32(native, 72, static_cast<std::uint32_t>(t->wait_id));
    put_u32(native, 76, static_cast<std::uint32_t>(t->wakeup_count));
    put_u32(native, 80, t->exit_status);
    put_u32(native, 84, static_cast<std::uint32_t>(t->run_for_clocks));
    put_u32(native, 88, static_cast<std::uint32_t>(t->run_for_clocks >> 32));
    put_u32(native, 92, static_cast<std::uint32_t>(t->num_interrupt_preempts));
    put_u32(native, 96, static_cast<std::uint32_t>(t->num_thread_preempts));
    put_u32(native, 100, static_cast<std::uint32_t>(t->num_releases));

    const std::uint32_t copy = std::min(wanted, kThreadInfoSize);
    if (copy != 0) k.mem.write_bytes(status_addr, std::span<const std::uint8_t>(native, copy));
    if (after_260 && wanted > kThreadInfoSize)
        k.mem.fill(status_addr + kThreadInfoSize, 0, wanted - kThreadInfoSize);
    return call.finish(0);
}

std::uint32_t Kernel::get_thread_exit_status(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Thread *t = k.get<Thread>(uid, error);
    if (t == nullptr) return error;
    if (t->status == thread_status::kDormant) return t->exit_status;
    return error::kNotDormant;
}

std::uint64_t Kernel::get_system_time_wide() {
    Impl &k = *impl_;
    CallScope call(k);
    const std::uint64_t now = k.now();
    k.request_resched();
    call.finish(0);
    return now;
}

// ---------------------------------------------------------------------------
// Callbacks.

std::uint32_t Kernel::create_callback(const char *name, std::uint32_t entry,
                                      std::uint32_t common_arg) {
    Impl &k = *impl_;
    if (name == nullptr) return error::kError;
    if ((entry & 0xF0000000u) != 0) return error::kIllegalAddr;
    auto cb = std::make_unique<Callback>();
    cb->name = Impl::clip_name(name);
    cb->entry = entry;
    cb->thread = k.current != nullptr ? k.current->uid : 0;
    cb->common_arg = common_arg;
    const Uid uid = k.create_object(std::move(cb));
    if (k.current != nullptr) k.current->callbacks.push_back(uid);
    return static_cast<std::uint32_t>(uid);
}

std::uint32_t Kernel::delete_callback(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Callback *cb = k.get<Callback>(uid, error);
    if (cb == nullptr) return error;
    k.destroy_callback(*cb);
    return 0;
}

std::uint32_t Kernel::notify_callback(Uid uid, std::uint32_t notify_arg) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    if (k.get<Callback>(uid, error) == nullptr) return error;
    k.notify_callback(uid, notify_arg);
    return 0;
}

std::uint32_t Kernel::cancel_callback(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Callback *cb = k.get<Callback>(uid, error);
    if (cb == nullptr) return error;
    // PPSSPP resets only the argument, not the count.
    cb->notify_arg = 0;
    return 0;
}

std::uint32_t Kernel::get_callback_count(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Callback *cb = k.get<Callback>(uid, error);
    if (cb == nullptr) return error;
    return static_cast<std::uint32_t>(cb->notify_count);
}

std::uint32_t Kernel::refer_callback_status(Uid uid, std::uint32_t status_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Callback *cb = k.get<Callback>(uid, error);
    if (cb == nullptr) return error;
    if (k.mem.is_valid(status_addr, 56) && k.mem.read_u32(status_addr) != 0) {
        k.mem.write_u32(status_addr, 56);
        k.write_name(status_addr + 4, cb->name);
        k.mem.write_u32(status_addr + 36, static_cast<std::uint32_t>(cb->thread));
        k.mem.write_u32(status_addr + 40, cb->entry);
        k.mem.write_u32(status_addr + 44, cb->common_arg);
        k.mem.write_u32(status_addr + 48, static_cast<std::uint32_t>(cb->notify_count));
        k.mem.write_u32(status_addr + 52, cb->notify_arg);
    }
    return 0;
}

std::uint32_t Kernel::check_callback() {
    Impl &k = *impl_;
    CallScope call(k);
    const bool processed = k.force_callbacks();
    return call.finish(processed ? 1u : 0u);
}

std::uint32_t Kernel::register_exit_callback(Uid uid) {
    Impl &k = *impl_;
    if (k.get<Callback>(uid) == nullptr) {
        if (k.has_sdk_version && k.sdk_version >= 0x3090500u) return error::kIllegalArgument;
        return 0;
    }
    k.exit_callback = uid;
    return 0;
}

Uid Kernel::exit_callback() const {
    return impl_->exit_callback;
}

void Kernel::notify_callback_from_host(Uid uid, std::uint32_t notify_arg) {
    impl_->notify_callback(uid, notify_arg);
}

// ---------------------------------------------------------------------------
// Services for other HLE modules.

std::uint32_t Kernel::wait_current_thread(WaitType type, Uid wait_id, std::uint32_t wait_value,
                                          std::uint32_t timeout_addr, bool process_callbacks,
                                          std::uint32_t provisional_result) {
    Impl &k = *impl_;
    if (!k.on_current_fiber()) return error::kIllegalContext;
    CallScope call(k);
    k.wait_current(type, wait_id, wait_value, timeout_addr, process_callbacks);
    return call.finish(provisional_result);
}

void Kernel::resume_from_wait(Uid uid, std::uint32_t result) {
    Impl &k = *impl_;
    CallScope call(k);
    k.resume_from_wait(uid, result);
    k.request_resched();
    call.finish(0);
}

void Kernel::wake_from_wait(Uid uid, std::uint32_t result) {
    impl_->resume_from_wait(uid, result);
}

bool Kernel::is_waiting_for(Uid uid, WaitType type, Uid wait_id) const {
    const Thread *t = impl_->thread(uid);
    return t != nullptr && t->is_waiting_for(type, wait_id);
}

std::uint32_t Kernel::Impl::delay_result_impl(std::uint32_t result, std::uint32_t usec) {
    if (!dispatch_allowed() || current == nullptr) return result;
    schedule(TimerKind::kHleDelay, current->uid, usec);
    wait_current(WaitType::kHleDelay, 1, result, 0, false);
    return result;
}

std::uint32_t Kernel::delay_result(std::uint32_t result, std::uint32_t usec) {
    Impl &k = *impl_;
    if (!k.on_current_fiber()) return result;
    CallScope call(k);
    return call.finish(k.delay_result_impl(result, usec));
}

Kernel::EventId Kernel::schedule_event(std::uint64_t delay_us, std::function<void()> callback) {
    Impl &k = *impl_;
    TimerEvent event;
    event.kind = TimerKind::kHost;
    event.id = k.next_event_id++;
    event.host = std::move(callback);
    const EventId id = event.id;
    k.events.emplace(std::make_pair(k.now() + delay_us, k.event_seq++), std::move(event));
    return id;
}

std::int64_t Kernel::unschedule_event(EventId id) {
    Impl &k = *impl_;
    for (auto it = k.events.begin(); it != k.events.end(); ++it) {
        if (it->second.kind == TimerKind::kHost && it->second.id == id) {
            const std::int64_t left =
                static_cast<std::int64_t>(it->first.first) - static_cast<std::int64_t>(k.now());
            k.events.erase(it);
            return left;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Introspection.

bool Kernel::thread_info(Uid uid, ThreadInfo &out) const {
    const Thread *t = impl_->thread(uid);
    if (t == nullptr) return false;
    out.uid = t->uid;
    out.name = t->name;
    out.status = t->status;
    out.current_priority = t->current_priority;
    out.initial_priority = t->initial_priority;
    out.wait_type = t->wait_type;
    out.wait_id = t->wait_id;
    out.wakeup_count = t->wakeup_count;
    out.exit_status = t->exit_status;
    out.attr = t->attr;
    out.entry = t->entry;
    out.initial_stack = t->initial_stack;
    out.stack_size = t->stack_size;
    out.gp = t->gp;
    out.num_releases = t->num_releases;
    out.module_id = t->module_id;
    return true;
}

Uid Kernel::create_host_object(const char *name) {
    auto object = std::make_unique<HostObject>();
    object->name = Impl::clip_name(name);
    return impl_->create_object(std::move(object));
}

bool Kernel::delete_host_object(Uid uid) {
    Impl &k = *impl_;
    const Object *object = k.lookup(uid);
    if (object == nullptr || object->type != ObjType::kHost) return false;
    k.take_object(uid);
    return true;
}

bool Kernel::is_host_object(Uid uid) const {
    const Object *object = impl_->lookup(uid);
    return object != nullptr && object->type == ObjType::kHost;
}

std::vector<Uid> Kernel::thread_list() const {
    return impl_->thread_queue;
}

std::vector<Uid> Kernel::ready_queue() const {
    std::vector<Uid> out;
    for (const auto &queue : impl_->ready) out.insert(out.end(), queue.begin(), queue.end());
    return out;
}

void Kernel::set_thread_host_data(Uid uid, void *data) {
    if (Thread *t = impl_->thread(uid)) t->host_data = data;
}

void *Kernel::thread_host_data(Uid uid) const {
    const Thread *t = impl_->thread(uid);
    return t != nullptr ? t->host_data : nullptr;
}

} // namespace mpo::kernel
