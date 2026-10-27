// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceGe (F11b). Ported from PPSSPP Core/HLE/sceGe.cpp and GPU/GPUCommon.cpp
// @ v1.20.4 (GPL-2.0-or-later): the list states and IDs, the queue order, the
// error codes, the SDK version checks, the GE interrupt and its sub-interrupts,
// the waits of sceGeDrawSync/sceGeListSync and the cycles each call eats.
// Left out: the GE debugger paths (hleSplitSyscallOverGe), the per-list
// interruptsEnabled toggle (always on outside the debugger), the compat flags
// (DrawSyncEatCycles, DrawSyncInstant, IgnoreEnqueue: none is set for MPO)
// and the bone matrix fast path of CALL (it only skips work).

#include <algorithm>
#include <cstring>

#include "hle/ge_manager.hpp"
#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kListIdMagic = 0x35000000u;

constexpr std::uint32_t kErrAlready = 0x80000020u;
constexpr std::uint32_t kErrBusy = 0x80000021u;
constexpr std::uint32_t kErrOutOfMemory = 0x80000022u;
constexpr std::uint32_t kErrPrivRequired = 0x80000023u;
constexpr std::uint32_t kErrBadArgument = 0x80000004u;
constexpr std::uint32_t kErrInvalidId = 0x80000100u;
constexpr std::uint32_t kErrInvalidIndex = 0x80000102u;
constexpr std::uint32_t kErrInvalidPointer = 0x80000103u;
constexpr std::uint32_t kErrInvalidSize = 0x80000104u;
constexpr std::uint32_t kErrInvalidMode = 0x80000107u;
constexpr std::uint32_t kErrInvalidValue = 0x800001FEu;

// PspGeListState.
constexpr std::uint32_t kListCompleted = 0u;
constexpr std::uint32_t kListQueued = 1u;
constexpr std::uint32_t kListDrawing = 2u;
constexpr std::uint32_t kListStalling = 3u;
constexpr std::uint32_t kListPaused = 4u;

constexpr std::uint32_t kSubIntrSignal = 0u; // PSP_GE_SUBINTR_SIGNAL
constexpr std::uint32_t kSubIntrFinish = 1u; // PSP_GE_SUBINTR_FINISH

// GEMatrixType.
constexpr int kMtxBone7 = 7;
constexpr int kMtxWorld = 8;
constexpr int kMtxView = 9;
constexpr int kMtxProjection = 10;
constexpr int kMtxTexgen = 11;

int sub_intr_base(int callback_id) {
    return callback_id * 2;
}

std::uint32_t to_float24(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits >> 8;
}

} // namespace

GeManager::GeManager(System &system, Interrupts &interrupts)
    : system_(system), interrupts_(interrupts), kernel_(system.kernel()),
      memory_(
          // The Runtime's own storage; GuestMemory only hands it out as const.
          {const_cast<std::uint8_t *>(system.runtime().memory().bytes().data()),
           system.runtime().memory().bytes().size()},
          psprecomp::GuestMemory::kPhysicalBase,
          {const_cast<std::uint8_t *>(system.runtime().memory().vram_bytes().data()),
           system.runtime().memory().vram_bytes().size()}),
      ge_(memory_) {
    for (int i = 0; i < kMaxLists; ++i) lists_[static_cast<std::size_t>(i)].id = i;
}

std::uint64_t GeManager::now_ticks() const {
    return kernel_.now_us() * kCpuHz / 1'000'000u;
}

kernel::Kernel::EventId GeManager::schedule_at_ticks(std::uint64_t ticks,
                                                     std::function<void()> event) {
    const std::uint64_t now = now_ticks();
    const std::uint64_t delay = ticks > now ? ticks - now : 0u;
    constexpr std::uint64_t kTicksPerUs = kCpuHz / 1'000'000u;
    return kernel_.schedule_event((delay + kTicksPerUs - 1u) / kTicksPerUs, std::move(event));
}

void GeManager::trace_list_event(const char *what, const List &list) {
    system_.hle().trace_event("ge: list %d %s at %08X (stall %08X)", list.id, what, list.dl.pc,
                              list.dl.stall);
}

// ---- The queue (GPUCommon) ------------------------------------------------

void GeManager::pop_queue() {
    if (queue_.empty()) return;
    queue_.pop_front();
    if (queue_.empty()) {
        current_ = -1;
        return;
    }
    const List *cur = current();
    const bool running = cur != nullptr && cur->state == ListState::kRunning;
    current_ = queue_.front();
    if (running) lists_[static_cast<std::size_t>(current_)].state = ListState::kRunning;
}

void GeManager::remove_from_queue(int list_id) {
    queue_.erase(std::remove(queue_.begin(), queue_.end(), list_id), queue_.end());
}

void GeManager::check_draw_sync() {
    if (!queue_.empty()) return;
    for (List &list : lists_) list.state = ListState::kNone;
}

bool GeManager::busy_drawing() {
    const std::uint32_t state = draw_sync(1u);
    if (state == kListDrawing || state == kListStalling) {
        const List *cur = current();
        if (cur != nullptr && cur->state != ListState::kPaused) return true;
    }
    return false;
}

std::uint32_t GeManager::enqueue(std::uint32_t list_pc, std::uint32_t stall, int callback_id,
                                 std::uint32_t opt_param, bool head) {
    if (((list_pc | stall) & 3u) != 0u || !memory_.IsValidRange(list_pc, 4u))
        return kErrInvalidPointer;

    // PspGeListArgs: size, context, numStacks, stackAddr. Below 16 bytes it is
    // the old struct without the stack.
    kernel::GuestMemory &mem = kernel_.memory();
    const bool args_valid = opt_param != 0u && mem.is_valid(opt_param, 16u);
    const std::uint32_t args_size = args_valid ? mem.read_u32(opt_param) : 0u;
    if (args_valid && args_size >= 16u && mem.read_u32(opt_param + 8u) >= 256u)
        return kErrInvalidSize;
    const std::uint32_t stack_addr =
        args_valid && args_size >= 16u ? mem.read_u32(opt_param + 12u) : 0u;

    if (kernel_.compiled_sdk_version() > 0x01FFFFFFu) {
        for (const List &list : lists_) {
            if (list.state == ListState::kNone || list.state == ListState::kCompleted) continue;
            // A list that has not interrupted yet has not shown the CPU its pc.
            if (list.dl.pc == list_pc && !list.pending_interrupt) return kErrBusy;
            if (stack_addr != 0u && list.stack_addr == stack_addr && !list.pending_interrupt)
                return kErrBusy;
        }
    }

    int id = -1;
    const std::uint64_t ticks = now_ticks();
    for (int i = 0; i < kMaxLists; ++i) {
        const int possible = (i + next_list_id_) % kMaxLists;
        const List &candidate = lists_[static_cast<std::size_t>(possible)];
        if (candidate.pending_interrupt) continue;
        if (candidate.state == ListState::kNone) {
            id = possible;
            break;
        }
        if (candidate.state == ListState::kCompleted && candidate.wait_until_ticks < ticks)
            id = possible;
    }
    if (id < 0) return kErrOutOfMemory;
    next_list_id_ = id + 1;

    List &list = lists_[static_cast<std::size_t>(id)];
    list.dl = ge::DisplayList{};
    list.start_pc = list_pc & 0x0FFFFFFFu;
    list.dl.pc = list_pc & 0x0FFFFFFFu;
    list.dl.stall = stall & 0x0FFFFFFFu;
    list.dl.bbox_result = false;
    list.sub_intr_base = std::max(sub_intr_base(callback_id), -1);
    list.interrupted = false;
    list.wait_until_ticks = UINT64_MAX;
    list.started = false;
    list.offset_addr = 0u;
    list.stack_addr = stack_addr;
    const std::uint32_t context = args_valid ? mem.read_u32(opt_param + 4u) : 0u;
    list.context = context != 0u && mem.is_valid(context) ? context : 0u;
    ++stats_.lists;

    bool run = false;
    if (head) {
        if (List *cur = current()) {
            if (cur->state != ListState::kPaused) return kErrInvalidValue;
            cur->state = ListState::kQueued;
            // So it does not pause again.
            cur->dl.signal = ge::SignalBehavior::kNone;
        }
        list.state = ListState::kPaused;
        current_ = id;
        queue_.push_front(id);
    } else if (current() != nullptr) {
        list.state = ListState::kQueued;
        queue_.push_back(id);
    } else {
        list.state = ListState::kRunning;
        current_ = id;
        queue_.push_front(id);
        draw_complete_ticks_ = UINT64_MAX;
        run = true;
    }
    trace_list_event(head ? "enqueued at the head" : "enqueued", list);
    if (run) process_queue();
    return static_cast<std::uint32_t>(id);
}

std::uint32_t GeManager::dequeue(int list_id) {
    if (list_id < 0 || list_id >= kMaxLists) return kErrInvalidId;
    List &list = lists_[static_cast<std::size_t>(list_id)];
    if (list.state == ListState::kNone) return kErrInvalidId;
    if (list.started) return kErrBusy;
    list.state = ListState::kNone;
    if (!queue_.empty() && list_id == queue_.front())
        pop_queue();
    else
        remove_from_queue(list_id);
    list.wait_until_ticks = 0u;
    (void)trigger_wait(SyncType::kList, list_id);
    check_draw_sync();
    return 0u;
}

std::uint32_t GeManager::update_stall(int list_id, std::uint32_t stall) {
    if (list_id < 0 || list_id >= kMaxLists) return kErrInvalidId;
    List &list = lists_[static_cast<std::size_t>(list_id)];
    if (list.state == ListState::kNone) return kErrInvalidId;
    if (list.state == ListState::kCompleted) return kErrAlready;
    list.dl.stall = stall & 0x0FFFFFFFu;
    process_queue();
    return 0u;
}

std::uint32_t GeManager::continue_lists() {
    List *cur = current();
    if (cur == nullptr) return 0u;
    const bool new_sdk = kernel_.compiled_sdk_version() >= 0x02000000u;
    if (cur->state == ListState::kPaused) {
        if (!is_break_) {
            cur->state = ListState::kRunning;
            cur->dl.signal = ge::SignalBehavior::kNone;
            // We have a list now, so it's not complete.
            draw_complete_ticks_ = UINT64_MAX;
        } else {
            cur->state = ListState::kQueued;
            cur->dl.signal = ge::SignalBehavior::kNone;
        }
    } else if (cur->state == ListState::kRunning) {
        return new_sdk ? kErrAlready : 0xFFFFFFFFu;
    } else {
        return new_sdk ? kErrBadArgument : 0xFFFFFFFFu;
    }
    process_queue();
    return 0u;
}

std::uint32_t GeManager::break_lists(std::uint32_t mode) {
    if (mode > 1u) return kErrInvalidMode;
    List *cur = current();
    if (cur == nullptr) return kErrAlready;
    if (mode == 1u) {
        // Clear the queue.
        queue_.clear();
        for (List &list : lists_) {
            list.state = ListState::kNone;
            list.dl.signal = ge::SignalBehavior::kNone;
        }
        next_list_id_ = 0;
        current_ = -1;
        return 0u;
    }
    const std::uint32_t sdk = kernel_.compiled_sdk_version();
    if (cur->state == ListState::kNone || cur->state == ListState::kCompleted)
        return sdk >= 0x02000000u ? kErrBadArgument : 0xFFFFFFFFu;
    if (cur->state == ListState::kPaused) {
        if (sdk > 0x02000010u && cur->dl.signal != ge::SignalBehavior::kHandlerPause)
            return kErrAlready;
        return kErrBusy;
    }
    if (cur->state == ListState::kQueued) {
        cur->state = ListState::kPaused;
        return static_cast<std::uint32_t>(cur->id);
    }
    if (cur->dl.signal == ge::SignalBehavior::kSync) cur->dl.pc += 8u;
    cur->interrupted = true;
    cur->state = ListState::kPaused;
    cur->dl.signal = ge::SignalBehavior::kHandlerSuspend;
    is_break_ = true;
    return static_cast<std::uint32_t>(cur->id);
}

std::uint32_t GeManager::draw_sync(std::uint32_t mode) {
    if (mode > 1u) return kErrInvalidMode;
    if (mode == 0u) {
        if (!kernel_.dispatch_enabled()) return kernel::error::kCanNotWait;
        if (kernel_.in_interrupt()) return kernel::error::kIllegalContext;
        if (draw_complete_ticks_ > now_ticks()) return wait(SyncType::kDraw, 1);
        for (List &list : lists_)
            if (list.state == ListState::kCompleted) list.state = ListState::kNone;
        return 0u;
    }
    // If there's no current list, it must be complete.
    const List *top = nullptr;
    for (const int i : queue_) {
        if (lists_[static_cast<std::size_t>(i)].state != ListState::kCompleted) {
            top = &lists_[static_cast<std::size_t>(i)];
            break;
        }
    }
    if (top == nullptr) return kListCompleted;
    const List *cur = current();
    if (cur != nullptr && cur->dl.pc == cur->dl.stall) return kListStalling;
    return kListDrawing;
}

std::uint32_t GeManager::list_sync(int list_id, std::uint32_t mode) {
    if (list_id < 0 || list_id >= kMaxLists) return kErrInvalidId;
    if (mode > 1u) return kErrInvalidMode;
    const List &list = lists_[static_cast<std::size_t>(list_id)];
    if (mode == 1u) {
        switch (list.state) {
        case ListState::kQueued:
            return list.interrupted ? kListPaused : kListQueued;
        case ListState::kRunning:
            return list.dl.pc == list.dl.stall ? kListStalling : kListDrawing;
        case ListState::kCompleted:
            return kListCompleted;
        case ListState::kPaused:
            return kListPaused;
        default:
            return kErrInvalidId;
        }
    }
    if (!kernel_.dispatch_enabled()) return kernel::error::kCanNotWait;
    if (kernel_.in_interrupt()) return kernel::error::kIllegalContext;
    if (list.wait_until_ticks > now_ticks()) return wait(SyncType::kList, list_id);
    return kListCompleted;
}

void GeManager::process_queue() {
    starting_ticks_ = now_ticks();
    cycles_executed_ = 0u;
    while (!queue_.empty()) {
        const int index = queue_.front();
        List &list = lists_[static_cast<std::size_t>(index)];
        if (list.state == ListState::kPaused) return;
        if (list.state == ListState::kNone) {
            // PPSSPP's workaround for Crazy Taxi (#19894).
            remove_from_queue(index);
            return;
        }
        current_ = index;
        if (!list.started && list.context != 0u) save_context(list.context);
        list.started = true;
        ge_.SetOffsetAddr(list.offset_addr);
        if (!memory_.IsValidRange(list.dl.pc, 4u)) return;
        cycles_executed_ += 60u;
        list.state = ListState::kRunning;
        list.interrupted = false;

        const ge::RunResult result = ge_.Run(list.dl);
        ++stats_.runs;
        stats_.commands += result.commands;
        cycles_executed_ += result.cycles;
        switch (result.status) {
        case ge::RunStatus::kFinished:
            ++stats_.finishes;
            finish_list(list);
            break;
        case ge::RunStatus::kPaused:
            // FINISH after a SIGNAL with "pause": the handler runs now.
            ++stats_.finishes;
            list.state = ListState::kPaused;
            trigger_interrupt(list);
            break;
        case ge::RunStatus::kSignal:
            ++stats_.signals;
            // Before SDK 2.00.0010, sceGeListSync returns "paused".
            if (result.behavior == ge::SignalBehavior::kHandlerSuspend &&
                kernel_.compiled_sdk_version() <= 0x02000010u)
                list.state = ListState::kPaused;
            trigger_interrupt(list);
            break;
        case ge::RunStatus::kError:
            ++stats_.errors;
            trace_list_event("stopped by an error", list);
            break;
        case ge::RunStatus::kStalled:
        case ge::RunStatus::kBudget:
            break;
        }
        list.offset_addr = ge_.offset_addr();
        switch (result.status) {
        case ge::RunStatus::kFinished:
        case ge::RunStatus::kError:
            break;
        default:
            // Stalled: resume this list later. Interrupted: the interrupt
            // runs the queue again when it ends.
            return;
        }
        // Another list could have taken the spot meanwhile.
        if (list.state != ListState::kQueued) remove_from_queue(index);
    }
    current_ = -1;
    draw_complete_ticks_ = starting_ticks_ + cycles_executed_;
    busy_ticks_ = std::max(busy_ticks_, draw_complete_ticks_);
    trigger_sync(SyncType::kDraw, 1, draw_complete_ticks_);
}

void GeManager::finish_list(List &list) {
    // The list is done: restore the context now, before the next list runs.
    if (list.started && list.context != 0u) {
        restore_context(list.context);
        // Don't restore it again.
        list.started = false;
    }
    trace_list_event("finished", list);
    // With the per-list interrupts off (only PPSSPP's debugger does that) the
    // list would complete here and wake its ListSync waiters at the
    // finishing tick; the interrupt does both instead.
    trigger_interrupt(list);
}

void GeManager::trigger_interrupt(List &list) {
    // Ge::Run stops past the END.
    const std::uint32_t pc = list.dl.pc - 4u;
    pending_.push_back({list.id, pc, memory_.Read32(pc - 4u) >> 24});
    list.pending_interrupt = true;
    (void)schedule_at_ticks(starting_ticks_ + cycles_executed_, [this] {
        // __GeExecuteInterrupt: PSP_INTR_IMMEDIATE, not only if enabled.
        interrupts_.post([this] { run_interrupt(); });
    });
}

// ---- The GE interrupt (GeIntrHandler) --------------------------------------

void GeManager::run_interrupt() {
    if (pending_.empty()) return;
    const PendingInterrupt intr = pending_.front();
    List &list = lists_[static_cast<std::size_t>(intr.list_id)];
    ++stats_.interrupts;
    const std::uint32_t sdk = kernel_.compiled_sdk_version();

    int sub = -1;
    if (list.sub_intr_base >= 0) {
        switch (list.dl.signal) {
        case ge::SignalBehavior::kSync:
        case ge::SignalBehavior::kJump:
        case ge::SignalBehavior::kCall:
        case ge::SignalBehavior::kRet:
            break;
        case ge::SignalBehavior::kHandlerPause:
            if (intr.cmd == ge::kCmdFinish)
                sub = list.sub_intr_base | static_cast<int>(kSubIntrSignal);
            break;
        default:
            sub = list.sub_intr_base |
                  static_cast<int>(intr.cmd == ge::kCmdSignal ? kSubIntrSignal : kSubIntrFinish);
            break;
        }
    }
    // The list is complete once the interrupt starts, not before another
    // interrupt finishes.
    if (list.dl.signal != ge::SignalBehavior::kHandlerPause && intr.cmd == ge::kCmdFinish)
        list.state = ListState::kCompleted;

    const bool suspend_old_sdk =
        list.dl.signal == ge::SignalBehavior::kHandlerSuspend && sdk <= 0x02000010u;
    const std::uint32_t a2 = sdk <= 0x02000010u ? 0u : intr.pc + 4u;
    if (sub >= 0 && interrupts_.run_sub(kGeInterrupt, static_cast<std::uint32_t>(sub),
                                        list.dl.sub_intr_token & 0xFFFFu, a2)) {
        // handleResult.
        ++stats_.handlers;
        if (system_.runtime().stopped()) return;
        pending_.pop_front();
        if (list.dl.signal == ge::SignalBehavior::kHandlerSuspend && sdk <= 0x02000010u &&
            list.state != ListState::kNone && list.state != ListState::kCompleted)
            list.state = ListState::kQueued;
    } else {
        if (suspend_old_sdk && list.state != ListState::kNone &&
            list.state != ListState::kCompleted)
            list.state = ListState::kQueued;
        pending_.pop_front();
    }
    interrupt_end(intr.list_id);
    process_queue();
}

void GeManager::interrupt_end(int list_id) {
    is_break_ = false;
    List &list = lists_[static_cast<std::size_t>(list_id)];
    list.pending_interrupt = false;
    // Unless the signal handler could change it?
    if (list.state != ListState::kCompleted && list.state != ListState::kNone) return;
    if (list.started && list.context != 0u) restore_context(list.context);
    list.wait_until_ticks = 0u;
    (void)trigger_wait(SyncType::kList, list_id);
    // Make sure the list isn't still queued since it's now completed.
    if (!queue_.empty()) {
        if (list_id == queue_.front())
            pop_queue();
        else
            remove_from_queue(list_id);
    }
}

// ---- Syncs and waits ---------------------------------------------------------

void GeManager::trigger_sync(SyncType type, int list_id, std::uint64_t at_ticks) {
    if (type == SyncType::kDraw) {
        // A later draw sync wins over an earlier one.
        if (draw_sync_scheduled_ && kernel_.unschedule_event(draw_sync_event_) >= 0 &&
            draw_sync_at_ticks_ > at_ticks)
            at_ticks = draw_sync_at_ticks_;
        draw_sync_at_ticks_ = at_ticks;
        draw_sync_scheduled_ = true;
        draw_sync_event_ = schedule_at_ticks(at_ticks, [this] {
            draw_sync_scheduled_ = false;
            sync_end(SyncType::kDraw, trigger_wait(SyncType::kDraw, 1));
        });
        return;
    }
    (void)schedule_at_ticks(at_ticks, [this, list_id] {
        sync_end(SyncType::kList, trigger_wait(SyncType::kList, list_id));
    });
}

bool GeManager::trigger_wait(SyncType type, int list_id) {
    std::vector<kernel::Uid> &waiters =
        type == SyncType::kDraw ? draw_waiters_ : list_waiters_[list_id];
    const kernel::WaitType wait_type =
        type == SyncType::kDraw ? kernel::WaitType::kGeDrawSync : kernel::WaitType::kGeListSync;
    bool woke = false;
    for (const kernel::Uid thread : waiters) {
        if (!kernel_.is_waiting_for(thread, wait_type, list_id)) continue;
        kernel_.wake_from_wait(thread, 0u);
        woke = true;
    }
    waiters.clear();
    return woke;
}

void GeManager::sync_end(SyncType type, bool woke_threads) {
    if (type != SyncType::kDraw || !woke_threads) return;
    for (List &list : lists_)
        if (list.state == ListState::kCompleted) list.state = ListState::kNone;
}

std::uint32_t GeManager::wait(SyncType type, int list_id) {
    if (type == SyncType::kDraw) {
        draw_waiters_.push_back(kernel_.current_thread());
        return kernel_.wait_current_thread(kernel::WaitType::kGeDrawSync, list_id, 0u, 0u, false);
    }
    list_waiters_[list_id].push_back(kernel_.current_thread());
    return kernel_.wait_current_thread(kernel::WaitType::kGeListSync, list_id, 0u, 0u, false);
}

// ---- Context and callbacks -----------------------------------------------------

void GeManager::save_context(std::uint32_t address) {
    kernel::GuestMemory &mem = kernel_.memory();
    if (!mem.is_valid(address, ge::kContextWords * 4u)) return;
    std::array<std::uint32_t, ge::kContextWords> words{};
    ge_.SaveContext(words);
    for (std::size_t i = 0; i < words.size(); ++i)
        mem.write_u32(address + static_cast<std::uint32_t>(i * 4u), words[i]);
}

void GeManager::restore_context(std::uint32_t address) {
    kernel::GuestMemory &mem = kernel_.memory();
    if (!mem.is_valid(address, ge::kContextWords * 4u)) return;
    std::array<std::uint32_t, ge::kContextWords> words{};
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = mem.read_u32(address + static_cast<std::uint32_t>(i * 4u));
    ge_.RestoreContext(words);
}

std::uint32_t GeManager::set_callback(std::uint32_t struct_addr) {
    int id = -1;
    for (int i = 0; i < kMaxCallbacks; ++i) {
        if (!used_callbacks_[static_cast<std::size_t>(i)]) {
            id = i;
            break;
        }
    }
    if (id < 0) return kErrOutOfMemory;
    used_callbacks_[static_cast<std::size_t>(id)] = true;
    Callback data;
    kernel::GuestMemory &mem = kernel_.memory();
    if (mem.is_valid(struct_addr, 16u)) {
        data.signal_func = mem.read_u32(struct_addr);
        data.signal_arg = mem.read_u32(struct_addr + 4u);
        data.finish_func = mem.read_u32(struct_addr + 8u);
        data.finish_arg = mem.read_u32(struct_addr + 12u);
    }
    callbacks_[static_cast<std::size_t>(id)] = data;
    const auto base = static_cast<std::uint32_t>(sub_intr_base(id));
    // PPSSPP ignores what these return too.
    if (data.finish_func != 0u) {
        (void)interrupts_.register_sub(kGeInterrupt, base | kSubIntrFinish, data.finish_func,
                                       data.finish_arg);
        (void)interrupts_.enable_sub(kGeInterrupt, base | kSubIntrFinish);
    }
    if (data.signal_func != 0u) {
        (void)interrupts_.register_sub(kGeInterrupt, base | kSubIntrSignal, data.signal_func,
                                       data.signal_arg);
        (void)interrupts_.enable_sub(kGeInterrupt, base | kSubIntrSignal);
    }
    system_.hle().trace_event("ge: callback %d: signal %08X(%08X) finish %08X(%08X)", id,
                              data.signal_func, data.signal_arg, data.finish_func, data.finish_arg);
    return static_cast<std::uint32_t>(id);
}

std::uint32_t GeManager::unset_callback(std::uint32_t callback_id) {
    if (callback_id >= static_cast<std::uint32_t>(kMaxCallbacks)) return kErrInvalidId;
    if (used_callbacks_[callback_id]) {
        const auto base = static_cast<std::uint32_t>(sub_intr_base(static_cast<int>(callback_id)));
        (void)interrupts_.release_sub(kGeInterrupt, base | kSubIntrFinish);
        (void)interrupts_.release_sub(kGeInterrupt, base | kSubIntrSignal);
    }
    used_callbacks_[callback_id] = false;
    return 0u;
}

// ---- NIDs ------------------------------------------------------------------------

void GeManager::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = kernel_;
    GameClock &clock = system_.clock();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("sceGe_user", nid, name, HleKind::kImplemented, std::move(handler));
    };
    const auto with_magic = [](std::uint32_t id) {
        return static_cast<std::int32_t>(id) >= 0 ? id ^ kListIdMagic : id;
    };
    const auto list_id = [](std::uint32_t id) { return static_cast<int>(id ^ kListIdMagic); };

    // Some games spam this, like MediEvil.
    def(0xE47E40E4u, "sceGeEdramGetAddr", [&clock](Ctx &c) {
        clock.eat_cycles(150u);
        ret(c, 0x04000000u);
    });
    def(0x1F6752ADu, "sceGeEdramGetSize", [](Ctx &c) { ret(c, 0x00200000u); });
    def(0xAB49E76Au, "sceGeListEnQueue", [this, &k, &clock, with_magic](Ctx &c) {
        const std::uint32_t id =
            enqueue(arg(c, 0), arg(c, 1), static_cast<std::int32_t>(arg(c, 2)), arg(c, 3), false);
        clock.eat_cycles(490u);
        ret(c, with_magic(id));
        k.preemption_point();
    });
    def(0x1C0D95A6u, "sceGeListEnQueueHead", [this, &k, &clock, with_magic](Ctx &c) {
        const std::uint32_t id =
            enqueue(arg(c, 0), arg(c, 1), static_cast<std::int32_t>(arg(c, 2)), arg(c, 3), true);
        clock.eat_cycles(480u);
        ret(c, with_magic(id));
        k.preemption_point();
    });
    def(0x5FB86AB0u, "sceGeListDeQueue", [this, &k, list_id](Ctx &c) {
        ret(c, dequeue(list_id(arg(c, 0))));
        k.preemption_point();
    });
    def(0xE0D68148u, "sceGeListUpdateStallAddr", [this, &k, &clock, list_id](Ctx &c) {
        // Before the update: it may cause an interrupt (FF Type-0).
        clock.eat_cycles(190u);
        ret(c, update_stall(list_id(arg(c, 0)), arg(c, 1)));
        k.preemption_point();
    });
    def(0x03444EB4u, "sceGeListSync", [this, &clock, list_id](Ctx &c) {
        clock.eat_cycles(220u);
        ret(c, list_sync(list_id(arg(c, 0)), arg(c, 1)));
    });
    def(0xB287BD61u, "sceGeDrawSync", [this, &clock](Ctx &c) {
        clock.eat_cycles(1240u);
        ret(c, draw_sync(arg(c, 0)));
    });
    def(0xB448EC0Du, "sceGeBreak", [this](Ctx &c) {
        const std::uint32_t mode = arg(c, 0);
        const std::uint32_t unknown = arg(c, 1);
        if (mode > 1u) return ret(c, kErrInvalidMode);
        if (static_cast<std::int32_t>(unknown) < 0 || static_cast<std::int32_t>(unknown + 16u) < 0)
            return ret(c, kErrPrivRequired);
        const std::uint32_t result = break_lists(mode);
        ret(c,
            static_cast<std::int32_t>(result) >= 0 && mode == 0u ? result ^ kListIdMagic : result);
    });
    def(0x4C06E472u, "sceGeContinue", [this, &k, &clock](Ctx &c) {
        const std::uint32_t result = continue_lists();
        clock.eat_cycles(220u);
        ret(c, result);
        k.preemption_point();
    });
    def(0xA4FC06A4u, "sceGeSetCallback", [this](Ctx &c) { ret(c, set_callback(arg(c, 0))); });
    def(0x05DB22CEu, "sceGeUnsetCallback", [this](Ctx &c) { ret(c, unset_callback(arg(c, 0))); });
    def(0x438A385Au, "sceGeSaveContext", [this](Ctx &c) {
        // The real error code.
        if (busy_drawing()) return ret(c, 0xFFFFFFFFu);
        save_context(arg(c, 0));
        ret(c, 0u);
    });
    def(0x0BF608FBu, "sceGeRestoreContext", [this](Ctx &c) {
        if (busy_drawing()) return ret(c, kErrBusy);
        restore_context(arg(c, 0));
        ret(c, 0u);
    });
    def(0x57C8945Bu, "sceGeGetMtx", [this, &k](Ctx &c) {
        const auto type = static_cast<std::int32_t>(arg(c, 0));
        const std::uint32_t ptr = arg(c, 1);
        const std::uint32_t size = type == kMtxProjection ? 16u : 12u;
        if (!k.memory().is_valid(ptr, size * 4u)) return ret(c, 0xFFFFFFFFu);
        // The matrices as the GE uses them (GPUCommon::GetMatrix24).
        const ge::GeState &state = ge_.state();
        const float *matrix = nullptr;
        if (type >= 0 && type <= kMtxBone7)
            matrix = state.bone_matrix.data() + static_cast<std::size_t>(type) * 12u;
        else if (type == kMtxTexgen)
            matrix = state.tgen_matrix.data();
        else if (type == kMtxWorld)
            matrix = state.world_matrix.data();
        else if (type == kMtxView)
            matrix = state.view_matrix.data();
        else if (type == kMtxProjection)
            matrix = state.proj_matrix.data();
        if (matrix == nullptr) return ret(c, kErrInvalidIndex);
        for (std::uint32_t i = 0; i < size; ++i)
            k.memory().write_u32(ptr + i * 4u, to_float24(matrix[i]));
        ret(c, 0u);
    });
    def(0xDC93CFEFu, "sceGeGetCmd", [this](Ctx &c) {
        const auto cmd = static_cast<std::int32_t>(arg(c, 0));
        if (cmd < 0 || cmd >= 256) return ret(c, kErrInvalidIndex);
        // Does not mask away the high bits, but matrix data does not read back.
        std::uint32_t value = ge_.state().cmdmem[static_cast<std::size_t>(cmd)];
        switch (cmd) {
        case ge::kCmdBoneMatrixData:
        case ge::kCmdWorldMatrixData:
        case ge::kCmdViewMatrixData:
        case ge::kCmdProjMatrixData:
        case ge::kCmdTgenMatrixData:
            value &= 0xFF000000u;
            break;
        case ge::kCmdBoneMatrixNumber:
            value &= 0xFF00007Fu;
            break;
        case ge::kCmdWorldMatrixNumber:
        case ge::kCmdViewMatrixNumber:
        case ge::kCmdProjMatrixNumber:
        case ge::kCmdTgenMatrixNumber:
            value &= 0xFF00000Fu;
            break;
        default:
            break;
        }
        ret(c, value);
    });
    def(0xE66CB92Eu, "sceGeGetStack", [this, &k](Ctx &c) {
        const List *cur = current();
        // Seems like it doesn't return an error code.
        if (cur == nullptr) return ret(c, 0u);
        const auto index = static_cast<std::int32_t>(arg(c, 0));
        if (cur->dl.stack_ptr <= index) return ret(c, kErrInvalidIndex);
        const std::uint32_t ptr = arg(c, 1);
        if (index >= 0 && k.memory().is_valid(ptr, 32u)) {
            const auto &entry = cur->dl.stack[static_cast<std::size_t>(index)];
            // Not really sure what most of these values are.
            k.memory().write_u32(ptr, 0u);
            k.memory().write_u32(ptr + 4u, entry.pc + 4u);
            k.memory().write_u32(ptr + 8u, entry.offset_addr);
            k.memory().write_u32(ptr + 28u, entry.base_addr);
        }
        ret(c, static_cast<std::uint32_t>(cur->dl.stack_ptr));
    });
    def(0xB77905EAu, "sceGeEdramSetAddrTranslation", [this](Ctx &c) {
        const std::uint32_t value = arg(c, 0);
        const bool outside_range = value != 0u && (value < 0x200u || value > 0x1000u);
        const bool not_power_of_two = (value & (value - 1u)) != 0u;
        if (outside_range || not_power_of_two) return ret(c, kErrInvalidValue);
        const std::uint32_t old = ge_.edram_addr_translation();
        ge_.SetEdramAddrTranslation(value);
        ret(c, old);
    });
}

} // namespace mpo::hle
