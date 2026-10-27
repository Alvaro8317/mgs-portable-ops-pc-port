// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Dialog/PSPMsgDialog.cpp @ v1.20.4.

#include "savedata/msg_dialog.hpp"

#include "savedata/guest_text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mpo::savedata {

std::string message_for_error_code(std::uint32_t code) {
    const char *text = nullptr;
    switch (code) {
    case error::kSavedataLoadDataBroken:
        text = "Save data was corrupt.";
        break;
    case error::kSavedataLoadNoMs:
    case error::kSavedataRwNoMemstick:
    case error::kSavedataSaveNoMs:
    case error::kSavedataDeleteNoMs:
    case error::kSavedataSizesNoMs:
        text = "Memory stick not inserted.";
        break;
    case error::kSavedataLoadNoData:
    case error::kSavedataRwNoData:
    case error::kSavedataDeleteNoData:
    case error::kSavedataSizesNoData:
        text = "Warning: no save data was found.";
        break;
    case error::kSavedataRwMemstickFull:
    case error::kSavedataSaveMsNoSpace:
        text = "Memory stick full.  Check your storage space.";
        break;
    default:
        break;
    }
    char buffer[96];
    if (text != nullptr) {
        std::snprintf(buffer, sizeof(buffer), "%s (%08x)", text, static_cast<unsigned>(code));
    } else {
        std::snprintf(buffer, sizeof(buffer), "Error code: %08x", static_cast<unsigned>(code));
    }
    return buffer;
}

int MsgDialog::init(kernel::GuestMemory &memory, std::uint32_t address) {
    if (status() != DialogStatus::kNone) return static_cast<int>(error::kInvalidStatus);

    address_ = address;
    if (!memory.is_valid(address, 4)) return 0; // PPSSPP ignores the call.
    // Only the bytes of the request's own version are read (and never more
    // than the largest version; PPSSPP copies whatever size says).
    const std::uint32_t size = memory.read_u32(address);
    const std::uint32_t copied = std::min<std::uint32_t>(size, sizeof(MsgDialogParam));
    request_ = MsgDialogParam{};
    if (const std::uint8_t *host = memory.translate(address, copied); host != nullptr) {
        std::memcpy(&request_, host, copied);
    }

    flags_ = 0;
    if (request_.type == 0 && (request_.error_num & 0x80000000u) == 0) {
        flags_ |= kError;
        request_.result = static_cast<std::int32_t>(error::kMsgDialogErrorCodeInvalid);
    } else if (size == kMsgDialogSizeV2 && request_.type == 1) {
        const std::uint32_t valid =
            msg_option::kTextSound | msg_option::kYesNo | msg_option::kDefaultNo;
        if (((request_.options | valid) ^ valid) != 0) {
            flags_ |= kError;
            request_.result = static_cast<std::int32_t>(error::kMsgDialogBadOption);
        }
    } else if (size == kMsgDialogSizeV3) {
        if ((request_.options & msg_option::kDefaultNo) != 0 &&
            (request_.options & msg_option::kYesNo) == 0) {
            flags_ |= kError;
            request_.result = static_cast<std::int32_t>(error::kMsgDialogBadOption);
        }
        if ((request_.options & ~msg_option::kSupported) != 0) {
            flags_ |= kError;
            request_.result = static_cast<std::int32_t>(error::kMsgDialogBadOption);
        }
    }

    if (flags_ == 0) {
        yes_no_choice_ = 1;
        if (request_.type == 1) flags_ |= kMsg;
        if (request_.type == 0) flags_ |= kErrorMsg;
        if ((request_.options & msg_option::kYesNo) != 0 &&
            (size == kMsgDialogSizeV3 || (size == kMsgDialogSizeV2 && request_.type == 1))) {
            flags_ |= kYesNo;
        }
        if ((request_.options & msg_option::kDefaultNo) != 0) {
            yes_no_choice_ = 0;
            flags_ |= kDefaultNo;
        }
        if ((request_.options & msg_option::kOk) != 0 && size == kMsgDialogSizeV3) {
            yes_no_choice_ = 1;
            flags_ |= kOk;
        }
        if ((flags_ & (kYesNo | kOk)) != 0) flags_ |= kValidButton;
        if (!((request_.options & msg_option::kNoCancel) != 0 && size == kMsgDialogSizeV3)) {
            flags_ |= kCancelButton;
        }
        if ((request_.options & msg_option::kNoSound) != 0) flags_ |= kNoSound;
    }

    if ((flags_ & kErrorMsg) != 0) {
        format_error_code(request_.error_num);
    } else {
        text_ = fixed_string(request_.string, sizeof(request_.string) - 1);
    }

    change_status_init(kInitDelayUs, &request_.common);
    update_buttons({});
    update_common(&request_.common);
    start_fade(true);
    return 0;
}

void MsgDialog::format_error_code(std::uint32_t code) {
    text_ = message_for_error_code(code);
}

int MsgDialog::update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons) {
    if (status() != DialogStatus::kRunning) return static_cast<int>(error::kInvalidStatus);

    if ((flags_ & (kError | kAbort)) != 0) {
        change_status(DialogStatus::kFinished, 0);
    } else {
        update_buttons(buttons);
        update_common(&request_.common);
        update_fade(anim_speed);

        // DisplayMessage: left/right move the yes/no selection.
        if ((flags_ & kYesNo) != 0) {
            if (is_button_pressed(button::kLeft) && yes_no_choice_ == 0) {
                yes_no_choice_ = 1;
            } else if (is_button_pressed(button::kRight) && yes_no_choice_ == 1) {
                yes_no_choice_ = 0;
            }
        }

        const std::uint32_t size = request_.common.size;
        if (is_button_pressed(cancel_button()) && (flags_ & kCancelButton) != 0) {
            if (size == kMsgDialogSizeV3 || (size == kMsgDialogSizeV2 && (flags_ & kYesNo) != 0)) {
                request_.button_pressed = 3;
            } else {
                request_.button_pressed = 0;
            }
            start_fade(false);
        } else if (is_button_pressed(ok_button()) && (flags_ & kValidButton) != 0) {
            request_.button_pressed = yes_no_choice_ == 0 ? 2u : 1u;
            start_fade(false);
        }
        request_.result = 0;
    }

    const std::uint32_t size = std::min<std::uint32_t>(request_.common.size, sizeof(request_));
    memory.write_bytes(address_, {reinterpret_cast<const std::uint8_t *>(&request_), size});
    return 0;
}

int MsgDialog::abort() {
    if (status() != DialogStatus::kRunning) return static_cast<int>(error::kInvalidStatus);
    // The status only changes on the next Update.
    flags_ |= kAbort;
    return 0;
}

int MsgDialog::shutdown(bool force) {
    if (status() != DialogStatus::kFinished && !force) {
        return static_cast<int>(error::kInvalidStatus);
    }
    shutdown_base(force);
    if (!force) change_status_shutdown(kShutdownDelayUs, &request_.common);
    return 0;
}

MsgDialogView MsgDialog::view() const {
    MsgDialogView out;
    out.text = text_;
    out.is_error_message = (flags_ & kErrorMsg) != 0;
    out.has_yes_no = (flags_ & kYesNo) != 0;
    out.yes_selected = yes_no_choice_ != 0;
    out.has_ok = (flags_ & kOk) != 0;
    out.shows_ok_button = (flags_ & (kOk | kValidButton)) != 0;
    out.shows_back_button = (flags_ & kCancelButton) != 0;
    if (request_.common.size == kMsgDialogSizeV3) {
        out.ok_caption = fixed_string(request_.okay_button, sizeof(request_.okay_button));
        out.back_caption = fixed_string(request_.cancel_button, sizeof(request_.cancel_button));
    }
    out.sound = (flags_ & kNoSound) == 0;
    out.fade = fade_value();
    out.ok_button = ok_button();
    out.cancel_button = cancel_button();
    return out;
}

} // namespace mpo::savedata
