// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Dialog/PSPNetconfDialog.cpp @ v1.20.4 (see
// net_dialog.hpp).

#include "savedata/net_dialog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace mpo::savedata {

namespace {

// SceUtilityNetconfParam::netAction, right after the common part.
constexpr std::size_t kNetActionOffset = sizeof(DialogCommon);

} // namespace

int NetDialog::init(kernel::GuestMemory &memory, std::uint32_t address) {
    if (status() != DialogStatus::kNone) return static_cast<int>(error::kInvalidStatus);
    address_ = address;
    request_.assign(sizeof(DialogCommon), 0u);
    if (memory.is_valid(address, 4)) {
        const std::uint32_t size = std::min(memory.read_u32(address), kMaxRequestSize);
        request_.assign(std::max<std::size_t>(size, sizeof(DialogCommon)), 0u);
        if (const std::uint8_t *host = memory.translate(address, size); host != nullptr)
            std::memcpy(request_.data(), host, size);
    }
    const DialogCommon c = common();
    change_status_init(kInitDelayUs, &c);
    update_buttons({});
    update_common(&c);
    start_fade(true);
    return 0;
}

int NetDialog::update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons) {
    if (status() != DialogStatus::kRunning) return static_cast<int>(error::kInvalidStatus);
    update_buttons(buttons);
    const DialogCommon c = common();
    update_common(&c);
    update_fade(anim_speed);
    if (is_button_pressed(cancel_button())) {
        start_fade(false);
        change_status(DialogStatus::kFinished, kShutdownDelayUs);
        set_result(kDialogResultAbort);
    }
    if (status() == DialogStatus::kFinished || pending_status() == DialogStatus::kFinished) {
        const std::uint32_t size =
            std::min<std::uint32_t>(common().size, static_cast<std::uint32_t>(request_.size()));
        memory.write_bytes(address_, {request_.data(), size});
    }
    return 0;
}

int NetDialog::shutdown(bool force) {
    if (status() != DialogStatus::kFinished && !force)
        return static_cast<int>(error::kInvalidStatus);
    shutdown_base(force);
    if (!force) {
        const DialogCommon c = common();
        change_status_shutdown(kShutdownDelayUs, &c);
    }
    return 0;
}

NetDialogView NetDialog::view() const {
    NetDialogView out;
    out.title = title_;
    out.message = message_;
    out.fade = fade_value();
    out.cancel_button = cancel_button();
    return out;
}

std::int32_t NetDialog::net_action() const {
    std::int32_t action = 0;
    if (request_.size() >= kNetActionOffset + sizeof(action))
        std::memcpy(&action, request_.data() + kNetActionOffset, sizeof(action));
    return action;
}

std::int32_t NetDialog::result() const {
    return common().result;
}

DialogCommon NetDialog::common() const {
    DialogCommon c{};
    std::memcpy(&c, request_.data(), std::min(sizeof(c), request_.size()));
    return c;
}

void NetDialog::set_result(std::int32_t result) {
    std::memcpy(request_.data() + offsetof(DialogCommon, result), &result, sizeof(result));
}

} // namespace mpo::savedata
