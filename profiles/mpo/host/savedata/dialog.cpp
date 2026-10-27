// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Dialog/PSPDialog.cpp @ v1.20.4 and the dialog job
// thread of Core/HLE/sceUtility.cpp @ v1.20.4 (see dialog.hpp).

#include "savedata/dialog.hpp"

#include <algorithm>

namespace mpo::savedata {

DialogStatus Dialog::status() {
    apply_due();
    const DialogStatus reported = status_;
    if (use_auto_status()) {
        if (status_ == DialogStatus::kShutdown) status_ = DialogStatus::kNone;
        if (status_ == DialogStatus::kInitialize) status_ = DialogStatus::kRunning;
    }
    return reported;
}

std::optional<std::uint64_t> Dialog::next_deadline_us() const {
    std::optional<std::uint64_t> next = pending_deadline_us_;
    if (job_ && !manual_jobs_ && (!next || job_->deadline_us < *next)) next = job_->deadline_us;
    return next;
}

void Dialog::apply_due() {
    const std::uint64_t now = clock_.now_us();
    if (job_ && !manual_jobs_ && now >= job_->deadline_us) complete_job();
    if (pending_deadline_us_ && now >= *pending_deadline_us_) {
        bool allowed = true;
        if (pending_status_ == DialogStatus::kNone && status_ == DialogStatus::kShutdown) {
            finish_volatile();
        } else if (pending_status_ == DialogStatus::kRunning &&
                   status_ == DialogStatus::kInitialize) {
            if (!volatile_locked_) {
                volatile_locked_ = volatile_hooks_ == nullptr || volatile_hooks_->lock();
                allowed = volatile_locked_;
            }
        }
        if (allowed) {
            status_ = pending_status_;
            pending_deadline_us_.reset();
        }
    }
}

int Dialog::complete_job() {
    if (!job_) return -1;
    if (job_->kind == DialogJobKind::kInit) {
        if (status_ != DialogStatus::kInitialize) {
            job_.reset();
            return -1;
        }
        // The job thread locks the volatile memory before it works; if the
        // lock is busy it would block, so the job stays in flight.
        if (!volatile_locked_) {
            if (volatile_hooks_ != nullptr && !volatile_hooks_->lock()) return 1;
            volatile_locked_ = true;
        }
        job_.reset();
        change_status(DialogStatus::kRunning, 0);
        return 0;
    }
    if (status_ != DialogStatus::kShutdown) {
        job_.reset();
        return -1;
    }
    job_.reset();
    change_status(DialogStatus::kNone, 0);
    return 0;
}

void Dialog::change_status(DialogStatus new_status, std::int64_t delay_us) {
    if (delay_us <= 0) {
        if (new_status == DialogStatus::kNone && status_ == DialogStatus::kShutdown) {
            finish_volatile();
        } else if (new_status == DialogStatus::kRunning && status_ == DialogStatus::kInitialize) {
            if (!volatile_locked_) {
                volatile_locked_ = volatile_hooks_ == nullptr || volatile_hooks_->lock();
            }
        }
        status_ = new_status;
        pending_status_ = new_status;
        pending_deadline_us_.reset();
    } else {
        pending_status_ = new_status;
        pending_deadline_us_ = clock_.now_us() + static_cast<std::uint64_t>(delay_us);
    }
}

void Dialog::change_status_init(std::int64_t delay_us, const DialogCommon *common) {
    change_status(DialogStatus::kInitialize, 0);
    if (common != nullptr) {
        job_ = DialogJob{DialogJobKind::kInit,
                         clock_.now_us() +
                             static_cast<std::uint64_t>(std::max<std::int64_t>(delay_us, 0)),
                         common->access_thread};
    } else {
        change_status(DialogStatus::kRunning, delay_us);
    }
}

void Dialog::change_status_shutdown(std::int64_t delay_us, const DialogCommon *common) {
    // A shutdown right after a skipped start does not run the job.
    const bool skip_job = status_ == DialogStatus::kNone && pending_status_ == DialogStatus::kNone;
    change_status(DialogStatus::kShutdown, 0);
    if (common != nullptr && !skip_job) {
        job_ = DialogJob{DialogJobKind::kShutdown,
                         clock_.now_us() +
                             static_cast<std::uint64_t>(std::max<std::int64_t>(delay_us, 0)),
                         common->access_thread};
    } else {
        change_status(DialogStatus::kNone, delay_us);
    }
}

void Dialog::shutdown_base(bool force) {
    change_status(force ? DialogStatus::kNone : DialogStatus::kShutdown, 0);
}

void Dialog::finish_volatile() {
    if (!volatile_locked_) return;
    if (volatile_hooks_ != nullptr) volatile_hooks_->unlock();
    volatile_locked_ = false;
}

void Dialog::update_common(const DialogCommon *common) {
    ok_button_ = button::kCircle;
    cancel_button_ = button::kCross;
    if (common != nullptr && common->button_swap == 1) {
        ok_button_ = button::kCross;
        cancel_button_ = button::kCircle;
    }
}

bool Dialog::is_button_held(std::uint32_t button, int &frames_held, int threshold,
                            int repeat_rate) const noexcept {
    if (!fading_ && (buttons_.held & button) != 0) {
        ++frames_held;
    } else {
        frames_held = 0;
        return false;
    }
    // Held for dialog purposes after `threshold` frames, then repeats.
    return frames_held >= threshold && repeat_rate > 0 && (frames_held % repeat_rate) == 0;
}

void Dialog::start_fade(bool fade_in) noexcept {
    fading_ = true;
    fade_ticks_ = 0;
    fade_in_ = fade_in;
}

void Dialog::update_fade(int anim_speed) {
    if (!fading_) return;
    // Integer ticks instead of PPSSPP's float accumulation of 1/30 s, so the
    // number of updates a fade takes does not depend on rounding.
    fade_ticks_ = std::min(fade_ticks_ + std::max(anim_speed, 0), kFadeTicks);
    if (fade_ticks_ < kFadeTicks) {
        const auto level = static_cast<std::uint32_t>(fade_ticks_ * 255 / kFadeTicks);
        fade_value_ = fade_in_ ? level : 255u - level;
    } else {
        fade_value_ = fade_in_ ? 255u : 0u;
        fading_ = false;
        if (!fade_in_) finish_fade_out();
    }
}

void Dialog::finish_fade_out() {
    change_status(DialogStatus::kFinished, 0);
}

} // namespace mpo::savedata
