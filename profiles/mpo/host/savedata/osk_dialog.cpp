// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Dialog/PSPOskDialog.cpp and PSPOskConstants.cpp
// @ v1.20.4 (English half-width keyboards only; see osk_dialog.hpp).

#include "savedata/osk_dialog.hpp"

#include "savedata/guest_text.hpp"

#include <cstddef>
#include <cstring>
#include <utility>

namespace mpo::savedata {
namespace {

// PPSSPP's oskKeys for OSK_KEYBOARD_LATIN_LOWERCASE and _UPPERCASE.
constexpr char kLatinKeys[2][OskKeyboard::kRows][OskKeyboard::kColumns + 1] = {
    {"1234567890-+", "qwertyuiop[]", "asdfghjkl;@~", "zxcvbnm,./?\\"},
    {"!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"`", "ZXCVBNM<>/?|"},
};

constexpr std::int32_t kAllLatin = osk_input::kLatinLowercase | osk_input::kLatinUppercase |
                                   osk_input::kLatinSymbol | osk_input::kLatinDigit;
// PPSSPP's defaultInputFlagsMap for the two Latin keyboards.
constexpr std::int32_t kDefaultLowercase =
    osk_input::kLatinLowercase | osk_input::kLatinSymbol | osk_input::kLatinDigit;
constexpr std::int32_t kDefaultUppercase = osk_input::kLatinUppercase | osk_input::kLatinSymbol;

// FindValidKeyboard(inputType, 0, ...) restricted to the English keyboards:
// lowercase if its default flags match, else uppercase if they match, else
// lowercase (PPSSPP would try the other languages, then fall back).
OskLayout initial_layout(std::int32_t input_type) {
    if (input_type == 0 || (input_type & kDefaultLowercase) != 0) {
        return OskLayout::kLatinLowercase;
    }
    if ((input_type & kDefaultUppercase) != 0) return OskLayout::kLatinUppercase;
    return OskLayout::kLatinLowercase;
}

} // namespace

OskKeyboard::OskKeyboard(std::int32_t input_type, std::size_t limit, std::u16string initial_text)
    : input_type_(input_type), max_length_(limit), text_(std::move(initial_text)),
      layout_(initial_layout(input_type)) {}

char16_t OskKeyboard::key_at(OskLayout layout, int row, int column) {
    if (row < 0 || row >= kRows || column < 0 || column >= kColumns) return u'\0';
    return static_cast<char16_t>(
        kLatinKeys[layout == OskLayout::kLatinUppercase ? 1 : 0][row][column]);
}

bool OskKeyboard::can_shift() const {
    // IsKeyboardShiftValid: both Latin keyboards allow the same flags.
    return input_type_ == 0 || (input_type_ & kAllLatin) != 0;
}

void OskKeyboard::wrap_selection() {
    constexpr int kKeys = kColumns * kRows;
    selected_ = ((selected_ % kKeys) + kKeys) % kKeys;
}

void OskKeyboard::move_up() {
    selected_ -= kColumns;
    wrap_selection();
}

void OskKeyboard::move_down() {
    selected_ += kColumns;
    wrap_selection();
}

void OskKeyboard::move_left() {
    --selected_;
    if (((selected_ + kColumns) % kColumns) == kColumns - 1) selected_ += kColumns;
    wrap_selection();
}

void OskKeyboard::move_right() {
    ++selected_;
    if ((selected_ % kColumns) == 0) selected_ -= kColumns;
    wrap_selection();
}

void OskKeyboard::press_key() {
    if (text_.size() < max_length_) text_.push_back(selected_key());
}

void OskKeyboard::shift() {
    if (!can_shift()) return;
    layout_ = layout_ == OskLayout::kLatinUppercase ? OskLayout::kLatinLowercase
                                                    : OskLayout::kLatinUppercase;
    // Both layouts are 12x4, so the cursor always stays in range.
}

void OskKeyboard::backspace() {
    if (!text_.empty()) text_.pop_back();
}

void OskKeyboard::space() {
    if (text_.size() < max_length_) text_.push_back(u' ');
}

void OskKeyboard::type(std::u16string_view text) {
    for (const char16_t c : text) {
        if (c < 0x20 || c == 0x7F) continue;
        if (text_.size() >= max_length_) break;
        text_.push_back(c);
    }
}

int OskDialog::init(kernel::GuestMemory &memory, std::uint32_t address) {
    if (status() != DialogStatus::kNone) return static_cast<int>(error::kInvalidStatus);
    const std::uint8_t *host = memory.translate(address, sizeof(OskParams));
    if (host == nullptr) return -1;
    std::memcpy(&params_, host, sizeof(params_));
    if (params_.common.size != sizeof(OskParams)) {
        return static_cast<int>(error::kInvalidParamSize);
    }
    const std::uint8_t *field = memory.translate(params_.fields, sizeof(OskFieldData));
    if (field == nullptr) return -1;
    address_ = address;
    field_address_ = params_.fields;
    std::memcpy(&field_, field, sizeof(field_));
    // Only field 0 is used, as in PPSSPP (which warns for field_count != 1).

    change_status_init(kInitDelayUs, &params_.common);
    description_ = ucs2_to_utf8(read_guest_u16string(memory, field_.desc));
    // FieldMaxLength(): outtextlimit, unless it is 0 or larger than the
    // buffer.
    const std::uint32_t buffer_chars = field_.outtext_length > 0 ? field_.outtext_length - 1 : 0;
    const std::uint32_t max_length =
        (field_.outtext_limit > buffer_chars || field_.outtext_limit == 0) ? buffer_chars
                                                                           : field_.outtext_limit;
    keyboard_ =
        OskKeyboard(field_.input_type, max_length, read_guest_u16string(memory, field_.intext));
    cancelled_ = false;
    cancel_held_ = confirm_held_ = left_held_ = up_held_ = down_held_ = right_held_ = 0;

    // Eat any keys pressed before the dialog started.
    update_buttons({});
    update_common(&params_.common);
    start_fade(true);
    return 0;
}

int OskDialog::update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons) {
    if (status() != DialogStatus::kRunning) return static_cast<int>(error::kInvalidStatus);

    update_buttons(buttons);
    update_common(&params_.common);
    const std::uint32_t confirm = ok_button();
    const std::uint32_t cancel = cancel_button();
    update_fade(anim_speed);

    if (is_button_pressed(button::kUp) ||
        is_button_held(button::kUp, up_held_, kHeldThreshold, kHeldRepeatRate)) {
        keyboard_.move_up();
    } else if (is_button_pressed(button::kDown) ||
               is_button_held(button::kDown, down_held_, kHeldThreshold, kHeldRepeatRate)) {
        keyboard_.move_down();
    } else if (is_button_pressed(button::kLeft) ||
               is_button_held(button::kLeft, left_held_, kHeldThreshold, kHeldRepeatRate)) {
        keyboard_.move_left();
    } else if (is_button_pressed(button::kRight) ||
               is_button_held(button::kRight, right_held_, kHeldThreshold, kHeldRepeatRate)) {
        keyboard_.move_right();
    }

    if (is_button_pressed(confirm) ||
        is_button_held(confirm, confirm_held_, kHeldThreshold, kHeldRepeatRate)) {
        keyboard_.press_key();
    } else if (is_button_pressed(button::kSelect)) {
        keyboard_.shift();
    } else if (is_button_pressed(button::kRTrigger) || is_button_pressed(button::kLTrigger)) {
        // Language cycling: only English is ported.
    } else if (is_button_pressed(cancel) ||
               is_button_held(cancel, cancel_held_, kHeldThreshold, kHeldRepeatRate)) {
        keyboard_.backspace();
    } else if (is_button_pressed(button::kStart)) {
        start_fade(false);
    } else if (is_button_pressed(button::kSquare)) {
        keyboard_.space();
    }

    write_result(memory, cancelled_ ? osk_result::kCancelled : osk_result::kChanged);
    return 0;
}

void OskDialog::write_result(kernel::GuestMemory &memory, std::int32_t field_result) {
    // Only the text and its terminator are written, never the rest of the
    // buffer.
    const std::u16string &text = keyboard_.text();
    std::size_t end = field_.outtext_length;
    if (end > text.size()) end = text.size() + 1;
    for (std::size_t i = 0; i < end; ++i) {
        const char16_t value = (i < keyboard_.max_length() && i < text.size()) ? text[i] : u'\0';
        std::uint8_t *unit =
            memory.translate(field_.outtext + static_cast<std::uint32_t>(2 * i), 2);
        if (unit == nullptr) break;
        unit[0] = static_cast<std::uint8_t>(value & 0xFF);
        unit[1] = static_cast<std::uint8_t>(value >> 8);
    }
    constexpr auto kCommonResult = static_cast<std::uint32_t>(offsetof(DialogCommon, result));
    constexpr auto kFieldResult = static_cast<std::uint32_t>(offsetof(OskFieldData, result));
    memory.write_u32(address_ + kCommonResult, 0);
    memory.write_u32(field_address_ + kFieldResult, static_cast<std::uint32_t>(field_result));
}

int OskDialog::shutdown(bool force) {
    if (status() != DialogStatus::kFinished && !force) {
        return static_cast<int>(error::kInvalidStatus);
    }
    shutdown_base(force);
    if (!force) change_status_shutdown(kShutdownDelayUs, &params_.common);
    return 0;
}

bool OskDialog::host_input_allowed() {
    // Like the pad, the host keyboard is ignored while a fade runs.
    return status() == DialogStatus::kRunning && !is_fading();
}

bool OskDialog::host_type(std::u16string_view text) {
    if (!host_input_allowed()) return false;
    keyboard_.type(text);
    return true;
}

bool OskDialog::host_backspace() {
    if (!host_input_allowed()) return false;
    keyboard_.backspace();
    return true;
}

bool OskDialog::host_finish() {
    if (!host_input_allowed()) return false;
    start_fade(false);
    return true;
}

bool OskDialog::host_cancel() {
    if (!host_input_allowed()) return false;
    cancelled_ = true;
    start_fade(false);
    return true;
}

OskDialogView OskDialog::view() const {
    OskDialogView out;
    out.description = description_;
    out.text = keyboard_.text();
    out.max_length = keyboard_.max_length();
    out.layout = keyboard_.layout();
    out.selected_row = keyboard_.row();
    out.selected_column = keyboard_.column();
    out.can_shift = keyboard_.can_shift();
    out.fade = fade_value();
    out.ok_button = ok_button();
    out.cancel_button = cancel_button();
    return out;
}

} // namespace mpo::savedata
