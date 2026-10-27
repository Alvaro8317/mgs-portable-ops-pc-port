// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/HLE/sceUtility.cpp @ v1.20.4 (see
// utility_dialogs.hpp).

#include "savedata/utility_dialogs.hpp"

namespace mpo::savedata {

UtilityResult UtilityDialogs::wrong_type() {
    return {static_cast<std::int32_t>(error::kWrongType), 0, 0};
}

void UtilityDialogs::activate(DialogType type) noexcept {
    if (!active_) {
        type_ = type;
        active_ = true;
    }
}

void UtilityDialogs::set_volatile_hooks(VolatileMemoryHooks *hooks) noexcept {
    savedata_.set_volatile_hooks(hooks);
    msg_.set_volatile_hooks(hooks);
    osk_.set_volatile_hooks(hooks);
}

// --- Savedata ---------------------------------------------------------------

UtilityResult UtilityDialogs::savedata_init_start(kernel::GuestMemory &memory,
                                                  std::uint32_t address) {
    if (!can_start(DialogType::kSavedata)) return wrong_type();
    activate(DialogType::kSavedata);
    return {savedata_.init(memory, address), 0, 0};
}

UtilityResult UtilityDialogs::savedata_shutdown_start() {
    if (type_ != DialogType::kSavedata) return wrong_type();
    active_ = false;
    return {savedata_.shutdown(), 0, kSavedataShutdownCycles};
}

UtilityResult UtilityDialogs::savedata_update(kernel::GuestMemory &memory, int anim_speed,
                                              const ButtonState &buttons) {
    if (type_ != DialogType::kSavedata) return wrong_type();
    const int result = savedata_.update(memory, anim_speed, buttons);
    return {result, result >= 0 ? kSavedataUpdateDelayUs : 0u, 0};
}

UtilityResult UtilityDialogs::savedata_get_status() {
    if (type_ != DialogType::kSavedata) {
        return {static_cast<std::int32_t>(error::kWrongType), 0, kSavedataGetStatusCycles};
    }
    return {static_cast<std::int32_t>(savedata_.status()), 0, kSavedataGetStatusCycles};
}

// --- MsgDialog --------------------------------------------------------------

UtilityResult UtilityDialogs::msg_init_start(kernel::GuestMemory &memory, std::uint32_t address) {
    if (!can_start(DialogType::kMsg)) return wrong_type();
    activate(DialogType::kMsg);
    return {msg_.init(memory, address), 0, 0};
}

UtilityResult UtilityDialogs::msg_shutdown_start() {
    if (type_ != DialogType::kMsg) return wrong_type();
    active_ = false;
    return {msg_.shutdown(), 0, 0};
}

UtilityResult UtilityDialogs::msg_update(kernel::GuestMemory &memory, int anim_speed,
                                         const ButtonState &buttons) {
    if (type_ != DialogType::kMsg) return wrong_type();
    const int result = msg_.update(memory, anim_speed, buttons);
    return {result, result >= 0 ? kMsgUpdateDelayUs : 0u, 0};
}

UtilityResult UtilityDialogs::msg_get_status() {
    if (type_ != DialogType::kMsg) return wrong_type();
    return {static_cast<std::int32_t>(msg_.status()), 0, 0};
}

UtilityResult UtilityDialogs::msg_abort() {
    if (type_ != DialogType::kMsg) return wrong_type();
    return {msg_.abort(), 0, 0};
}

// --- OSK --------------------------------------------------------------------

UtilityResult UtilityDialogs::osk_init_start(kernel::GuestMemory &memory, std::uint32_t address) {
    if (!can_start(DialogType::kOsk)) return wrong_type();
    activate(DialogType::kOsk);
    return {osk_.init(memory, address), 0, 0};
}

UtilityResult UtilityDialogs::osk_shutdown_start() {
    if (type_ != DialogType::kOsk) return wrong_type();
    active_ = false;
    return {osk_.shutdown(), 0, 0};
}

UtilityResult UtilityDialogs::osk_update(kernel::GuestMemory &memory, int anim_speed,
                                         const ButtonState &buttons) {
    if (type_ != DialogType::kOsk) return wrong_type();
    return {osk_.update(memory, anim_speed, buttons), 0, kOskUpdateCycles};
}

UtilityResult UtilityDialogs::osk_get_status() {
    if (type_ != DialogType::kOsk) return wrong_type();
    return {static_cast<std::int32_t>(osk_.status()), 0, 0};
}

} // namespace mpo::savedata
