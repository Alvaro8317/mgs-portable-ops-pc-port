// SPDX-License-Identifier: GPL-3.0-or-later
//
// Common state machine of the sceUtility dialogs (F18a).
// Ported from PPSSPP Core/Dialog/PSPDialog.cpp @ v1.20.4 (status changes,
// pending delays, fade, buttons) and the dialog job thread of
// Core/HLE/sceUtility.cpp @ v1.20.4 (UtilityDialogInitialize/Shutdown).
//
// Status flow, as in PPSSPP:
//   InitStart  -> INITIALIZE at once, RUNNING when the init job ends;
//   Update     -> the dialog runs; when it is done it fades out -> FINISHED;
//   ShutdownStart (only from FINISHED) -> SHUTDOWN, NONE when the shutdown
//                 job ends.
// PPSSPP runs each job as a guest helper thread ("ScePafJob", priority
// DialogCommon::access_thread) that locks the volatile memory, works for the
// dialog's delay and then finishes the transition. Here a job is a deadline on
// the injected kernel::Clock: by default status() completes it once due, so
// the dialog works without threads; with set_manual_jobs(true) the HLE (F18b)
// runs its own job thread and calls complete_job().
//
// The model never calls guest code. Nothing here keeps iterators or owning
// objects alive across a call into the host hooks.

#pragma once

#include "kernel/clock.hpp"
#include "savedata/psp_types.hpp"

#include <cstdint>
#include <optional>

namespace mpo::savedata {

// Pad state for one Update, from sceCtrl: `held` is the current mask
// (__CtrlPeekButtons in PPSSPP), `pressed` the buttons pressed since the
// previous read (__CtrlReadLatch).
struct ButtonState {
    std::uint32_t held = 0;
    std::uint32_t pressed = 0;
};

// sceKernelVolatileMemLock/Unlock, as the dialog jobs use them. Optional: a
// dialog without hooks behaves as if the lock always succeeds.
class VolatileMemoryHooks {
public:
    virtual ~VolatileMemoryHooks() = default;
    // True when the lock was taken (sceKernelVolatileMemLock(0, 0, 0) == 0).
    virtual bool lock() = 0;
    // Releases the lock. PPSSPP then clears the volatile area to mimic the
    // firmware using it; that is the hook's choice.
    virtual void unlock() = 0;
};

enum class DialogJobKind : std::uint8_t { kInit, kShutdown };

struct DialogJob {
    DialogJobKind kind = DialogJobKind::kInit;
    std::uint64_t deadline_us = 0;
    std::int32_t priority = 0; // DialogCommon::access_thread
};

class Dialog {
public:
    Dialog(DialogType kind, kernel::Clock &clock) : type_(kind), clock_(clock) {}
    virtual ~Dialog() = default;
    Dialog(const Dialog &) = delete;
    Dialog &operator=(const Dialog &) = delete;

    [[nodiscard]] DialogType type() const noexcept { return type_; }

    // sceUtility*GetStatus: applies the pending change (and the job, unless
    // jobs are manual) that is due, then returns the status.
    DialogStatus status();

    // The earliest time at which status() can change on its own, if any.
    [[nodiscard]] std::optional<std::uint64_t> next_deadline_us() const;

    // The init or shutdown job in flight, if any.
    [[nodiscard]] const std::optional<DialogJob> &job() const noexcept { return job_; }
    void set_manual_jobs(bool manual) noexcept { manual_jobs_ = manual; }
    // Ends the job in flight now (PPSSPP's __UtilityInitDialog and
    // __UtilityFinishDialog). Returns 0, or -1 when the status no longer
    // matches the job.
    int complete_job();

    void set_volatile_hooks(VolatileMemoryHooks *hooks) noexcept { volatile_hooks_ = hooks; }

    // Fade level for the renderer, 0 (invisible) to 255.
    [[nodiscard]] std::uint32_t fade_value() const noexcept { return fade_value_; }
    [[nodiscard]] bool is_fading() const noexcept { return fading_; }
    // The buttons that confirm and cancel, after DialogCommon::button_swap.
    [[nodiscard]] std::uint32_t ok_button() const noexcept { return ok_button_; }
    [[nodiscard]] std::uint32_t cancel_button() const noexcept { return cancel_button_; }

    // Fade duration in Update ticks at animSpeed 1 (PPSSPP: FADE_TIME 1.0 s
    // advanced by animSpeed / 30 per update).
    static constexpr int kFadeTicks = 30;

protected:
    // Base of the dialogs' Shutdown(force): FINISHED -> SHUTDOWN (or NONE when
    // forced), at once.
    void shutdown_base(bool force);

    void change_status(DialogStatus new_status, std::int64_t delay_us);
    void change_status_init(std::int64_t delay_us, const DialogCommon *common);
    void change_status_shutdown(std::int64_t delay_us, const DialogCommon *common);
    [[nodiscard]] DialogStatus read_status() const noexcept { return status_; }
    [[nodiscard]] DialogStatus pending_status() const noexcept { return pending_status_; }

    // PPSSPP's UseAutoStatus(): INITIALIZE and SHUTDOWN are reported once and
    // then skipped. Off for every dialog MPO uses.
    [[nodiscard]] virtual bool use_auto_status() const { return false; }

    void update_common(const DialogCommon *common);
    void update_buttons(const ButtonState &buttons) noexcept { buttons_ = buttons; }
    [[nodiscard]] bool is_button_pressed(std::uint32_t button) const noexcept {
        return !fading_ && (buttons_.pressed & button) != 0;
    }
    bool is_button_held(std::uint32_t button, int &frames_held, int threshold = 30,
                        int repeat_rate = 10) const noexcept;

    void start_fade(bool fade_in) noexcept;
    void update_fade(int anim_speed);
    virtual void finish_fade_out();

    kernel::Clock &clock() noexcept { return clock_; }

private:
    void finish_volatile();
    void apply_due();

    DialogType type_;
    kernel::Clock &clock_;
    DialogStatus status_ = DialogStatus::kNone;
    DialogStatus pending_status_ = DialogStatus::kNone;
    std::optional<std::uint64_t> pending_deadline_us_;
    std::optional<DialogJob> job_;
    bool manual_jobs_ = false;
    VolatileMemoryHooks *volatile_hooks_ = nullptr;
    bool volatile_locked_ = false;

    ButtonState buttons_;
    std::uint32_t ok_button_ = button::kCircle;
    std::uint32_t cancel_button_ = button::kCross;

    int fade_ticks_ = 0;
    bool fading_ = false;
    bool fade_in_ = false;
    std::uint32_t fade_value_ = 0;
};

} // namespace mpo::savedata
