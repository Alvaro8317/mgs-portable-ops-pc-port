// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceGe (F11b): the display list queue of the GE over mpo_ge, ported from
// PPSSPP Core/HLE/sceGe.cpp and the queue half of GPU/GPUCommon.cpp
// @ v1.20.4 (EnqueueList, DequeueList, UpdateStall, Continue, Break,
// DrawSync, ListSync, ProcessDLQueue, Execute_End's interrupts and syncs,
// InterruptStart/End and SyncEnd).
//
// Like PPSSPP without a GE debugger, a list runs to its end, stall or signal
// synchronously inside the call that starts it (sceGeListEnQueue,
// sceGeListUpdateStallAddr, sceGeContinue, the end of a GE interrupt), and
// the time the GE would take is only accounted: ProcessDLQueue adds up the
// cycles of what it ran (ge::RunResult::cycles) from the moment it started,
// and the waits (sceGeDrawSync, sceGeListSync) and the interrupts are kernel
// events at that time.
//
// Every SIGNAL with a handler and every FINISH posts the GE interrupt (25).
// Its host handler (GeIntrHandler) runs one sub-interrupt of the list's
// callback, 2·id | 0 for a signal and 2·id | 1 for a finish, with
// $a0 = token, $a1 = the callback's argument and $a2 = the END's pc + 4, and
// then ends the interrupt: a finished list is completed, its ListSync waiters
// wake up and the queue runs again.
//
// F14b: the draws go to ge().SetDrawBackend's renderer (softgpu by default).
// A renderer that keeps the framebuffers elsewhere (the Vulkan one) sets an
// after-run hook, which runs when each ge::Ge::Run returns, before any guest
// code: there it writes back to VRAM what it drew, so the CPU sees VRAM as
// softgpu would leave it.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "ge.hpp"
#include "kernel/kernel.hpp"
#include "memory_view.hpp"

namespace mpo::hle {

class Interrupts;
class System;

class GeManager {
public:
    static constexpr int kMaxLists = 64;     // DisplayListMaxCount
    static constexpr int kMaxCallbacks = 16; // ge_callback_data

    // PSP_GE_DL_STATE_*.
    enum class ListState : std::uint8_t {
        kNone = 0,
        kQueued = 1,
        kRunning = 2,
        kCompleted = 3,
        kPaused = 4,
    };

    struct Stats {
        std::uint64_t lists = 0;      // Lists enqueued.
        std::uint64_t runs = 0;       // ge::Ge::Run calls.
        std::uint64_t commands = 0;   // GE commands executed.
        std::uint64_t finishes = 0;   // FINISH + END reached.
        std::uint64_t signals = 0;    // SIGNAL + END that interrupt.
        std::uint64_t interrupts = 0; // GE interrupts run.
        std::uint64_t handlers = 0;   // ... that called a guest handler.
        std::uint64_t errors = 0;     // Lists stopped by a bad address.
    };

    GeManager(System &system, Interrupts &interrupts);
    GeManager(const GeManager &) = delete;
    GeManager &operator=(const GeManager &) = delete;

    void register_hle();

    [[nodiscard]] ge::Ge &ge() noexcept { return ge_; }
    // Called after every ge::Ge::Run (F14b). Empty: nothing.
    void set_after_run(std::function<void()> hook) { after_run_ = std::move(hook); }
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }

    // The calls, as the HLE wrappers see them (also for tests). List IDs are
    // the internal ones (0..63), without LIST_ID_MAGIC.
    std::uint32_t enqueue(std::uint32_t list_pc, std::uint32_t stall, int callback_id,
                          std::uint32_t opt_param, bool head);
    std::uint32_t dequeue(int list_id);
    std::uint32_t update_stall(int list_id, std::uint32_t stall);
    std::uint32_t list_sync(int list_id, std::uint32_t mode);
    std::uint32_t draw_sync(std::uint32_t mode);
    std::uint32_t continue_lists();
    std::uint32_t break_lists(std::uint32_t mode);
    std::uint32_t set_callback(std::uint32_t struct_addr);
    std::uint32_t unset_callback(std::uint32_t callback_id);

private:
    enum class SyncType : std::uint8_t { kDraw, kList };

    struct List {
        ge::DisplayList dl;
        int id = 0;
        std::uint32_t start_pc = 0;
        ListState state = ListState::kNone;
        int sub_intr_base = -1;
        bool interrupted = false;
        bool pending_interrupt = false;
        bool started = false;
        std::uint64_t wait_until_ticks = 0;
        std::uint32_t context = 0;
        std::uint32_t stack_addr = 0;
        std::uint32_t offset_addr = 0;
    };

    // ge_pending_cb.
    struct PendingInterrupt {
        int list_id = 0;
        std::uint32_t pc = 0;  // The END's address.
        std::uint32_t cmd = 0; // SIGNAL or FINISH.
    };

    struct Callback {
        std::uint32_t signal_func = 0;
        std::uint32_t signal_arg = 0;
        std::uint32_t finish_func = 0;
        std::uint32_t finish_arg = 0;
    };

    [[nodiscard]] std::uint64_t now_ticks() const;
    // Schedules `event` at `ticks` of the CPU clock (the next microsecond).
    kernel::Kernel::EventId schedule_at_ticks(std::uint64_t ticks, std::function<void()> event);

    void process_queue(); // ProcessDLQueue
    void pop_queue();     // PopDLQueue
    void remove_from_queue(int list_id);
    void check_draw_sync(); // CheckDrawSync
    void finish_list(List &list);
    void trigger_interrupt(List &list);
    void run_interrupt(); // GeIntrHandler::run + handleResult
    void interrupt_end(int list_id);
    void trigger_sync(SyncType type, int list_id, std::uint64_t at_ticks);
    bool trigger_wait(SyncType type, int list_id); // __GeTriggerWait
    void sync_end(SyncType type, bool woke_threads);
    std::uint32_t wait(SyncType type, int list_id);
    void save_context(std::uint32_t address);
    void restore_context(std::uint32_t address);
    [[nodiscard]] List *current() {
        return current_ < 0 ? nullptr : &lists_[static_cast<std::size_t>(current_)];
    }
    [[nodiscard]] bool busy_drawing();
    void trace_list_event(const char *what, const List &list);

    System &system_;
    Interrupts &interrupts_;
    kernel::Kernel &kernel_;
    ge::MemoryView memory_;
    ge::Ge ge_;
    std::array<List, kMaxLists> lists_{};
    std::deque<int> queue_;
    int current_ = -1;
    int next_list_id_ = 0;
    bool is_break_ = false;
    std::uint64_t starting_ticks_ = 0;
    std::uint64_t cycles_executed_ = 0;
    std::uint64_t draw_complete_ticks_ = 0;
    std::uint64_t busy_ticks_ = 0;
    kernel::Kernel::EventId draw_sync_event_ = 0;
    std::uint64_t draw_sync_at_ticks_ = 0;
    bool draw_sync_scheduled_ = false;
    std::deque<PendingInterrupt> pending_;
    std::array<Callback, kMaxCallbacks> callbacks_{};
    std::array<bool, kMaxCallbacks> used_callbacks_{};
    std::vector<kernel::Uid> draw_waiters_;
    std::map<int, std::vector<kernel::Uid>> list_waiters_;
    std::function<void()> after_run_;
    Stats stats_;
};

} // namespace mpo::hle
