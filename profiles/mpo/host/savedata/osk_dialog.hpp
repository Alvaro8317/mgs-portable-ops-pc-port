// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUtilityOsk without the HLE and without drawing (F18a).
// Ported from PPSSPP Core/Dialog/PSPOskDialog.cpp and PSPOskConstants.cpp
// @ v1.20.4.
//
// Two parts:
//   - OskKeyboard: the keyboard model (layout, cursor, UTF-16 text and length
//     limit), with no PSP structures, so a renderer or a test drives it;
//   - OskDialog: the dialog state machine on top of it. It reads field 0 of
//     the request at InitStart, applies the pad on every Update (the
//     navigation, repeat timings and actions of PPSSPP) and writes the text,
//     common.result = 0 and field.result after every Update, like PPSSPP.
//
// Only PPSSPP's English half-width keyboards (lowercase and uppercase) are
// ported: MPO US asks for input type LATIN_DIGIT | LATIN_UPPERCASE and turns
// any character >= 0x80 into '?' (docs/analysis/utility.md), so the Japanese,
// Korean, Russian and full-width keyboards would only produce '?'. L and R,
// which cycle those languages in PPSSPP, do nothing here.
//
// PC extensions (not in PPSSPP's PSP-pad path): the host keyboard can type
// text, erase, finish or cancel through host_*() between Updates, like
// PPSSPP's native-keyboard bypass. Cancel writes field.result = CANCELLED,
// which MPO checks for [?: the PSP's own OSK has no cancel in PPSSPP].

#pragma once

#include "kernel/guest_memory.hpp"
#include "savedata/dialog.hpp"
#include "savedata/psp_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mpo::savedata {

enum class OskLayout : std::uint8_t { kLatinLowercase, kLatinUppercase };

class OskKeyboard {
public:
    static constexpr int kColumns = 12;
    static constexpr int kRows = 4;

    OskKeyboard() = default;
    // `input_type` is the field's SceUtilityOskInputType mask; `max_length`
    // the number of characters the field takes.
    OskKeyboard(std::int32_t input_type, std::size_t limit, std::u16string initial_text);

    [[nodiscard]] static char16_t key_at(OskLayout layout, int row, int column);
    [[nodiscard]] char16_t selected_key() const { return key_at(layout_, row(), column()); }

    [[nodiscard]] OskLayout layout() const noexcept { return layout_; }
    [[nodiscard]] int selected() const noexcept { return selected_; }
    [[nodiscard]] int row() const noexcept { return selected_ / kColumns; }
    [[nodiscard]] int column() const noexcept { return selected_ % kColumns; }
    [[nodiscard]] const std::u16string &text() const noexcept { return text_; }
    [[nodiscard]] std::size_t max_length() const noexcept { return max_length_; }
    // Whether Select may switch case for this input type.
    [[nodiscard]] bool can_shift() const;

    void move_up();
    void move_down();
    void move_left();
    void move_right();
    // Confirm on the cursor: appends the key if the text has room.
    void press_key();
    // Select: switches case when allowed, keeping the cursor in range.
    void shift();
    // Cancel button: erases the last character.
    void backspace();
    // Square: a space, if the text has room.
    void space();
    // Host keyboard text, character by character, up to max_length; control
    // characters are dropped.
    void type(std::u16string_view text);

private:
    void wrap_selection();

    std::int32_t input_type_ = 0;
    std::size_t max_length_ = 0;
    std::u16string text_;
    OskLayout layout_ = OskLayout::kLatinLowercase;
    int selected_ = 0;
};

struct OskDialogView {
    std::string description; // UTF-8, from field.desc
    std::u16string text;
    std::size_t max_length = 0;
    OskLayout layout = OskLayout::kLatinLowercase;
    int selected_row = 0;
    int selected_column = 0;
    bool can_shift = false;
    std::uint32_t fade = 0;
    std::uint32_t ok_button = button::kCircle;
    std::uint32_t cancel_button = button::kCross;
};

class OskDialog final : public Dialog {
public:
    // PPSSPP's OSK_INIT_DELAY_US and OSK_SHUTDOWN_DELAY_US.
    static constexpr std::int64_t kInitDelayUs = 300'000;
    static constexpr std::int64_t kShutdownDelayUs = 40'000;
    // PPSSPP's repeat timings for the OSK (frames).
    static constexpr int kHeldThreshold = 10;
    static constexpr int kHeldRepeatRate = 5;

    explicit OskDialog(kernel::Clock &clock) : Dialog(DialogType::kOsk, clock) {}

    // sceUtilityOskInitStart: 0, error::kInvalidStatus, error::kInvalidParamSize
    // or -1 (invalid request or field pointers, where PPSSPP also gives -1).
    int init(kernel::GuestMemory &memory, std::uint32_t address);
    // sceUtilityOskUpdate.
    int update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons);
    // sceUtilityOskShutdownStart.
    int shutdown(bool force = false);

    // Host keyboard, while RUNNING and not fading out; returns false when
    // ignored. The text reaches the guest on the next Update.
    bool host_type(std::u16string_view text);
    bool host_backspace();
    bool host_finish();
    bool host_cancel();

    [[nodiscard]] const OskKeyboard &keyboard() const noexcept { return keyboard_; }
    [[nodiscard]] OskDialogView view() const;

private:
    bool host_input_allowed();
    void write_result(kernel::GuestMemory &memory, std::int32_t field_result);

    std::uint32_t address_ = 0;
    std::uint32_t field_address_ = 0;
    OskParams params_{};
    OskFieldData field_{};
    std::string description_;
    OskKeyboard keyboard_;
    bool cancelled_ = false;

    int cancel_held_ = 0;
    int confirm_held_ = 0;
    int left_held_ = 0;
    int up_held_ = 0;
    int down_held_ = 0;
    int right_held_ = 0;
};

} // namespace mpo::savedata
