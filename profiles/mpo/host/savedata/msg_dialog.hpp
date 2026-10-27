// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUtilityMsgDialog without the HLE and without drawing (F18a).
// Ported from PPSSPP Core/Dialog/PSPMsgDialog.cpp @ v1.20.4.
//
// The model reads the request from guest memory at InitStart, runs the
// buttons on every Update and writes the request back (common.size bytes)
// after every Update, like PPSSPP. What to draw is in view(); the renderer
// (F18b) owns layout, fonts and scrolling.
//
// MPO (docs/analysis/utility.md) uses V2 requests (580 bytes): type 1 with a
// string and options 0, TEXTSOUND, TEXTSOUND|YESNO or TEXTSOUND|YESNO|
// DEFAULT_NO, and type 0 with an error code. It calls Update(2) every frame
// while RUNNING and reads buttonPressed and common.result after NONE.

#pragma once

#include "kernel/guest_memory.hpp"
#include "savedata/dialog.hpp"
#include "savedata/psp_types.hpp"

#include <cstdint>
#include <string>

namespace mpo::savedata {

struct MsgDialogView {
    // UTF-8 text: the request's string, or the message for an error code.
    std::string text;
    bool is_error_message = false;
    bool has_yes_no = false;
    // The yes/no selection: true when "Yes" is highlighted.
    bool yes_selected = true;
    bool has_ok = false;            // an "OK" choice in the box (V3 OPTION_OK)
    bool shows_ok_button = false;   // the confirm-button hint at the bottom
    bool shows_back_button = false; // the cancel-button hint
    // V3 captions for those hints (empty: use the default text).
    std::string ok_caption;
    std::string back_caption;
    bool sound = true;
    std::uint32_t fade = 0; // 0-255
    std::uint32_t ok_button = button::kCircle;
    std::uint32_t cancel_button = button::kCross;
};

class MsgDialog final : public Dialog {
public:
    // PPSSPP's MSG_INIT_DELAY_US and MSG_SHUTDOWN_DELAY_US (a PSP takes
    // about 700 ms to init; PPSSPP shortens it).
    static constexpr std::int64_t kInitDelayUs = 300'000;
    static constexpr std::int64_t kShutdownDelayUs = 26'000;

    explicit MsgDialog(kernel::Clock &clock) : Dialog(DialogType::kMsg, clock) {}

    // sceUtilityMsgDialogInitStart. Returns 0 or a firmware error.
    int init(kernel::GuestMemory &memory, std::uint32_t address);
    // sceUtilityMsgDialogUpdate.
    int update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons);
    // sceUtilityMsgDialogAbort: only while RUNNING; takes effect on the next
    // Update.
    int abort();
    // sceUtilityMsgDialogShutdownStart.
    int shutdown(bool force = false);

    [[nodiscard]] MsgDialogView view() const;
    // The request as the model holds it (written back after each Update).
    [[nodiscard]] const MsgDialogParam &request() const noexcept { return request_; }

private:
    enum Flags : std::uint32_t {
        kMsg = 0x1,
        kErrorMsg = 0x2,
        kYesNo = 0x4,
        kDefaultNo = 0x8,
        kOk = 0x10,
        kValidButton = 0x20,
        kCancelButton = 0x40,
        kNoSound = 0x80,
        kError = 0x100,
        kAbort = 0x200,
    };

    void format_error_code(std::uint32_t code);

    std::uint32_t flags_ = 0;
    MsgDialogParam request_{};
    std::uint32_t address_ = 0;
    std::string text_;
    int yes_no_choice_ = 1; // 1: yes, 0: no
};

// The English text PPSSPP shows for a savedata error code (or the generic
// "Error code: xxxxxxxx").
std::string message_for_error_code(std::uint32_t code);

} // namespace mpo::savedata
