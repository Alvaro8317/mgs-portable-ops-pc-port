// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sceUtility entry points of the dialogs, without NIDs (F18a; F20b adds
// Netconf and the HTML viewer).
// Ported from PPSSPP Core/HLE/sceUtility.cpp @ v1.20.4.
//
// One dialog runs at a time: InitStart of another kind while one is active
// fails with error::kWrongType, and Update, GetStatus, ShutdownStart and
// Abort fail with it when the last dialog started is of another kind.
// ShutdownStart deactivates at once, so the next dialog can start while the
// previous one still reports SHUTDOWN.
//
// Each call returns the value for $v0 and the timing PPSSPP gives it, which
// the HLE (F18b) applies:
//   - delay_us: hleDelayResult, the calling thread waits (and others run);
//   - eat_cycles: hleEatCycles, CPU time consumed without a reschedule (at
//     the PSP's 222 MHz).
// The F18b wrappers must not keep iterators or owning objects alive across
// the delay: the thread may exit there (docs/sessions/F08b.md).

#pragma once

#include "kernel/clock.hpp"
#include "kernel/guest_memory.hpp"
#include "savedata/msg_dialog.hpp"
#include "savedata/net_dialog.hpp"
#include "savedata/osk_dialog.hpp"
#include "savedata/psp_types.hpp"
#include "savedata/save_dialog.hpp"
#include "savedata/savedata_store.hpp"

#include <cstdint>

namespace mpo::savedata {

struct UtilityResult {
    std::int32_t value = 0;
    std::uint32_t delay_us = 0;
    std::uint64_t eat_cycles = 0;
};

class UtilityDialogs {
public:
    // PPSSPP's timings.
    static constexpr std::uint32_t kSavedataUpdateDelayUs = 300;
    static constexpr std::uint32_t kMsgUpdateDelayUs = 800;
    static constexpr std::uint64_t kSavedataShutdownCycles = 30'000;
    static constexpr std::uint64_t kSavedataGetStatusCycles = 200;
    // msToCycles(0.7315 + 0.1): a vblank plus slack (PPSSPP #12044).
    static constexpr std::uint64_t kOskUpdateCycles = 184'593;

    UtilityDialogs(kernel::Clock &clock, SavedataStore &store);

    UtilityResult savedata_init_start(kernel::GuestMemory &memory, std::uint32_t address);
    UtilityResult savedata_shutdown_start();
    UtilityResult savedata_update(kernel::GuestMemory &memory, int anim_speed,
                                  const ButtonState &buttons);
    UtilityResult savedata_get_status();

    UtilityResult msg_init_start(kernel::GuestMemory &memory, std::uint32_t address);
    UtilityResult msg_shutdown_start();
    UtilityResult msg_update(kernel::GuestMemory &memory, int anim_speed,
                             const ButtonState &buttons);
    UtilityResult msg_get_status();
    UtilityResult msg_abort();

    UtilityResult osk_init_start(kernel::GuestMemory &memory, std::uint32_t address);
    UtilityResult osk_shutdown_start();
    UtilityResult osk_update(kernel::GuestMemory &memory, int anim_speed,
                             const ButtonState &buttons);
    UtilityResult osk_get_status();

    // F20b: the network dialogs (net_dialog.hpp). PPSSPP gives their Update
    // and GetStatus no delay.
    UtilityResult net_init_start(DialogType type, kernel::GuestMemory &memory,
                                 std::uint32_t address);
    UtilityResult net_shutdown_start(DialogType type);
    UtilityResult net_update(DialogType type, kernel::GuestMemory &memory, int anim_speed,
                             const ButtonState &buttons);
    UtilityResult net_get_status(DialogType type);

    [[nodiscard]] DialogType current_type() const noexcept { return type_; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    SaveDialog &savedata() noexcept { return savedata_; }
    MsgDialog &msg() noexcept { return msg_; }
    OskDialog &osk() noexcept { return osk_; }
    // kNetconf or kHtmlViewer.
    NetDialog &net(DialogType type) noexcept {
        return type == DialogType::kHtmlViewer ? html_viewer_ : netconf_;
    }

    void set_volatile_hooks(VolatileMemoryHooks *hooks) noexcept;

private:
    bool can_start(DialogType type) const noexcept { return !active_ || type_ == type; }
    void activate(DialogType type) noexcept;
    static UtilityResult wrong_type();

    SaveDialog savedata_;
    MsgDialog msg_;
    OskDialog osk_;
    NetDialog netconf_;
    NetDialog html_viewer_;
    DialogType type_ = DialogType::kNone;
    bool active_ = false;
};

} // namespace mpo::savedata
