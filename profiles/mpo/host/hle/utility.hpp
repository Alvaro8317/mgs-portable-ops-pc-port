// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUtility's dialogs and system parameters (F18b): the NIDs of
// sceUtilitySavedata*, sceUtilityOsk* and sceUtilityMsgDialog* over
// mpo_savedata (F18a), drawn with PPGe; sceUtilityGetSystemParamInt/String
// and sceImposeSetLanguageMode. Ported from PPSSPP Core/HLE/sceUtility.cpp,
// sceImpose.cpp and the drawing of Core/Dialog/PSPDialog.cpp,
// PSPMsgDialog.cpp, PSPOskDialog.cpp and PSPSaveDialog.cpp @ v1.20.4.
//
// - ms0:/ of the IO and the savedata root are the same folder
//   (SystemConfig::ms0_root): the game also lists ms0:/PSP/SAVEDATA itself.
// - Saves are written without encryption: the runtime holds no keys (F02).
//   Converting to and from the PSP's mode 5 is the installer's job.
// - The dialog jobs (init and shutdown) are deadlines on the game clock that
//   GetStatus completes, and they take the volatile memory lock like
//   PPSSPP's job thread; the game waits for it meanwhile.
// - Buttons: what is held (__CtrlPeekButtons) and pressed since the last
//   Update (__CtrlReadLatch), as PPSSPP's PSPDialog::UpdateButtons.
// - The PC keyboard types into the OSK (host_type...), and scripted text
//   (queue_osk_text) fills the next OSKs for unattended runs.

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "hle/ppge.hpp"
#include "savedata/savedata_store.hpp"
#include "savedata/utility_dialogs.hpp"

namespace mpo::hle {

class System;

class Utility {
public:
    struct Stats {
        std::uint64_t savedata_runs = 0; // InitStart that succeeded
        std::uint64_t osk_runs = 0;
        std::uint64_t msg_runs = 0;
        std::uint64_t drawn_frames = 0; // Updates that drew a dialog
        std::uint64_t cancelled = 0;    // Savedata dialogs the player left with Back
        std::uint64_t failures = 0;     // ... that finished with an error
    };

    explicit Utility(System &system);
    ~Utility();
    Utility(const Utility &) = delete;
    Utility &operator=(const Utility &) = delete;

    void register_hle();

    [[nodiscard]] savedata::SavedataStore &store() noexcept { return *store_; }
    [[nodiscard]] savedata::UtilityDialogs &dialogs() noexcept { return *dialogs_; }
    [[nodiscard]] Ppge &ppge() noexcept { return ppge_; }
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }

    // The OSK is on screen and takes text from the PC keyboard.
    [[nodiscard]] bool osk_takes_text();
    // The PC keyboard (UTF-8 text, Backspace, Enter, Escape). False when the
    // OSK is not running.
    bool host_type(const std::string &utf8);
    bool host_backspace();
    bool host_finish();
    bool host_cancel();
    // Types `text` and finishes the next OSK that runs (mpo_boot --osk-text).
    void queue_osk_text(std::string text) { osk_script_.push_back(std::move(text)); }

    // The calls, as the HLE wrappers see them (also for tests): the value of
    // $v0 after the timing PPSSPP gives each call.
    std::uint32_t savedata_init_start(std::uint32_t address);
    std::uint32_t savedata_update(int anim_speed);
    std::uint32_t osk_init_start(std::uint32_t address);
    std::uint32_t osk_update(int anim_speed);
    std::uint32_t msg_init_start(std::uint32_t address);
    std::uint32_t msg_update(int anim_speed);

private:
    class VolatileHooks;

    std::uint32_t apply(const savedata::UtilityResult &result);
    savedata::ButtonState buttons();
    void draw_msg();
    void draw_osk();
    void draw_save();
    // PSPDialog's helpers.
    void start_draw(std::uint32_t fade);
    void display_buttons(bool ok, bool cancel, std::uint32_t fade, std::uint32_t ok_button,
                         std::uint32_t cancel_button, bool button_swap,
                         const std::string &ok_caption = {}, const std::string &back_caption = {});
    void finish_savedata();
    void trace_store();

    System &system_;
    std::unique_ptr<savedata::SavedataStore> store_;
    std::unique_ptr<savedata::UtilityDialogs> dialogs_;
    std::unique_ptr<VolatileHooks> hooks_;
    Ppge ppge_;
    std::deque<std::string> osk_script_;
    bool osk_script_pending_ = false;
    std::uint64_t store_operations_ = 0;
    Stats stats_;
};

} // namespace mpo::hle
