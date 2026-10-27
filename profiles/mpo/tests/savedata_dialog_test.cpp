// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_savedata (F18a): the dialog state machine, sceUtilityMsgDialog,
// sceUtilityOsk and the one-dialog-at-a-time rule, on a virtual clock and a
// flat buffer as PSP RAM. Requests are shaped like MPO's (docs/analysis/
// utility.md); no game data is needed.

#include "savedata/msg_dialog.hpp"
#include "savedata/osk_dialog.hpp"
#include "savedata/savedata_store.hpp"
#include "savedata/utility_dialogs.hpp"
#include "savedata_test_util.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace {

using namespace savedata_test;
namespace sd = mpo::savedata;
namespace btn = mpo::savedata::button;
using mpo::kernel::VirtualClock;

constexpr std::uint32_t kMsgAddress = 0x08900000u;
constexpr std::uint32_t kOskAddress = 0x08910000u;
constexpr std::uint32_t kOskField = 0x08910100u;
constexpr std::uint32_t kOskDesc = 0x08910200u;
constexpr std::uint32_t kOskIntext = 0x08910400u;
constexpr std::uint32_t kOskOuttext = 0x08910600u;

int status_of(sd::Dialog &dialog) {
    return static_cast<int>(dialog.status());
}
constexpr int kNone = static_cast<int>(DialogStatus::kNone);
constexpr int kInitialize = static_cast<int>(DialogStatus::kInitialize);
constexpr int kRunning = static_cast<int>(DialogStatus::kRunning);
constexpr int kFinished = static_cast<int>(DialogStatus::kFinished);
constexpr int kShutdown = static_cast<int>(DialogStatus::kShutdown);
constexpr int kInvalidStatus = static_cast<int>(sd::error::kInvalidStatus);

// A V2 request like FUN_08bbd680 in title (type 1, string, options).
sd::MsgDialogParam mpo_msg(std::uint32_t options, const char *text,
                           std::uint32_t size = sd::kMsgDialogSizeV2) {
    sd::MsgDialogParam p{};
    p.common = mpo_common(size, 0, 0x18);
    p.common.graphics_thread = 0x28;
    p.common.font_thread = 0x28;
    p.type = 1;
    std::snprintf(p.string, sizeof(p.string), "%s", text);
    p.options = options;
    return p;
}

sd::MsgDialogParam guest_msg(Guest &guest) {
    return guest.get<sd::MsgDialogParam>(kMsgAddress);
}

void test_status_and_delays() {
    VirtualClock clock(1000);
    Guest guest;
    sd::MsgDialog dialog(clock);
    CHECK_EQ(status_of(dialog), kNone);
    guest.put(kMsgAddress, mpo_msg(sd::msg_option::kTextSound | sd::msg_option::kYesNo, "Save?"));
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    CHECK_EQ(status_of(dialog), kInitialize);
    CHECK(dialog.job().has_value());
    CHECK_EQ(dialog.job()->priority, 0x18);
    CHECK_EQ(dialog.next_deadline_us().value_or(0), 1000u + 300'000u);
    // A second InitStart while one runs fails.
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), kInvalidStatus);
    // Update before RUNNING fails.
    CHECK_EQ(dialog.update(guest.memory, 2, {}), kInvalidStatus);
    clock.advance(299'999);
    CHECK_EQ(status_of(dialog), kInitialize);
    clock.advance(1);
    CHECK_EQ(status_of(dialog), kRunning);
    CHECK(!dialog.next_deadline_us().has_value());

    // Buttons are ignored while the dialog fades in (15 updates at speed 2).
    CHECK_EQ(dialog.update(guest.memory, 2, press(btn::kCircle)), 0);
    CHECK_EQ(guest_msg(guest).button_pressed, 0u);
    int fade_updates = 1;
    while (dialog.is_fading()) {
        dialog.update(guest.memory, 2, {});
        ++fade_updates;
    }
    CHECK_EQ(fade_updates, 15);
    CHECK_EQ(dialog.fade_value(), 255u);

    // Yes/No: right selects No, left back to Yes, right again; circle confirms.
    CHECK(dialog.view().yes_selected);
    dialog.update(guest.memory, 2, press(btn::kRight));
    CHECK(!dialog.view().yes_selected);
    dialog.update(guest.memory, 2, press(btn::kLeft));
    CHECK(dialog.view().yes_selected);
    dialog.update(guest.memory, 2, press(btn::kRight));
    dialog.update(guest.memory, 2, press(btn::kCircle));
    CHECK_EQ(guest_msg(guest).button_pressed, 2u);
    CHECK_EQ(run_until_finished(dialog, guest), 15);
    CHECK_EQ(guest_msg(guest).result, 0);
    CHECK_EQ(dialog.update(guest.memory, 2, {}), kInvalidStatus);

    // ShutdownStart: SHUTDOWN for 26 ms, then NONE.
    CHECK_EQ(dialog.shutdown(), 0);
    CHECK_EQ(status_of(dialog), kShutdown);
    clock.advance(25'999);
    CHECK_EQ(status_of(dialog), kShutdown);
    clock.advance(1);
    CHECK_EQ(status_of(dialog), kNone);
    // Shutdown again: not FINISHED.
    CHECK_EQ(dialog.shutdown(), kInvalidStatus);
}

// Runs a message dialog to RUNNING with the fade done.
void start_msg(sd::MsgDialog &dialog, VirtualClock &clock, Guest &guest,
               const sd::MsgDialogParam &request) {
    guest.put(kMsgAddress, request);
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    wait_running(dialog, clock, sd::MsgDialog::kInitDelayUs);
    finish_fade_in(dialog, guest);
}

void close_msg(sd::MsgDialog &dialog, VirtualClock &clock) {
    CHECK_EQ(dialog.shutdown(), 0);
    clock.advance(sd::MsgDialog::kShutdownDelayUs);
    CHECK_EQ(status_of(dialog), kNone);
}

void test_msg_buttons() {
    VirtualClock clock;
    Guest guest;
    sd::MsgDialog dialog(clock);
    using namespace sd::msg_option;

    // DEFAULT_NO (MPO's 0x111) starts on No; circle answers 2.
    start_msg(dialog, clock, guest, mpo_msg(kTextSound | kYesNo | kDefaultNo, "Overwrite?"));
    CHECK(!dialog.view().yes_selected);
    CHECK(dialog.view().has_yes_no);
    dialog.update(guest.memory, 2, press(btn::kCircle));
    CHECK_EQ(guest_msg(guest).button_pressed, 2u);
    run_until_finished(dialog, guest);
    close_msg(dialog, clock);

    // Cancel on a V2 yes/no answers 3.
    start_msg(dialog, clock, guest, mpo_msg(kTextSound | kYesNo, "Quit?"));
    dialog.update(guest.memory, 2, press(btn::kCross));
    CHECK_EQ(guest_msg(guest).button_pressed, 3u);
    run_until_finished(dialog, guest);
    close_msg(dialog, clock);

    // A plain V2 text (MPO's options 0 or 1): only back closes it, with 0.
    start_msg(dialog, clock, guest, mpo_msg(kTextSound, "Saved."));
    CHECK(!dialog.view().has_yes_no);
    CHECK(!dialog.view().shows_ok_button);
    CHECK(dialog.view().shows_back_button);
    dialog.update(guest.memory, 2, press(btn::kCircle)); // no valid button
    CHECK(!dialog.is_fading());
    dialog.update(guest.memory, 2, press(btn::kCross));
    CHECK(dialog.is_fading());
    CHECK_EQ(guest_msg(guest).button_pressed, 0u);
    run_until_finished(dialog, guest);
    CHECK_EQ(dialog.view().text, std::string("Saved."));
    close_msg(dialog, clock);

    // buttonSwap 1: cross confirms, circle goes back.
    auto swapped = mpo_msg(kTextSound | kYesNo, "Swap?");
    swapped.common.button_swap = 1;
    start_msg(dialog, clock, guest, swapped);
    CHECK_EQ(dialog.ok_button(), btn::kCross);
    dialog.update(guest.memory, 2, press(btn::kCross));
    CHECK_EQ(guest_msg(guest).button_pressed, 1u);
    run_until_finished(dialog, guest);
    close_msg(dialog, clock);

    // V3: OK option and captions; NOCANCEL hides back.
    auto v3 = mpo_msg(kOk | kNoCancel, "V3", sd::kMsgDialogSizeV3);
    std::snprintf(v3.okay_button, sizeof(v3.okay_button), "Accept");
    start_msg(dialog, clock, guest, v3);
    CHECK(dialog.view().has_ok);
    CHECK(!dialog.view().shows_back_button);
    CHECK_EQ(dialog.view().ok_caption, std::string("Accept"));
    dialog.update(guest.memory, 2, press(btn::kCross)); // no cancel
    CHECK(!dialog.is_fading());
    dialog.update(guest.memory, 2, press(btn::kCircle));
    CHECK_EQ(guest_msg(guest).button_pressed, 1u);
    run_until_finished(dialog, guest);
    close_msg(dialog, clock);
}

void test_msg_errors_and_abort() {
    VirtualClock clock;
    Guest guest;
    sd::MsgDialog dialog(clock);
    using namespace sd::msg_option;

    // Type 0 with an error code (MPO's case 5): the savedata messages.
    auto code = mpo_msg(0, "");
    code.type = 0;
    code.error_num = sd::error::kSavedataLoadNoData;
    start_msg(dialog, clock, guest, code);
    CHECK(dialog.view().is_error_message);
    CHECK_EQ(dialog.view().text, std::string("Warning: no save data was found. (80110307)"));
    dialog.update(guest.memory, 2, press(btn::kCross));
    run_until_finished(dialog, guest);
    close_msg(dialog, clock);
    CHECK_EQ(sd::message_for_error_code(0x80020001u), std::string("Error code: 80020001"));

    // An "error" without the high bit: finishes at once with the error.
    auto bad_code = mpo_msg(0, "");
    bad_code.type = 0;
    bad_code.error_num = 0x1234;
    guest.put(kMsgAddress, bad_code);
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    wait_running(dialog, clock, sd::MsgDialog::kInitDelayUs);
    CHECK_EQ(dialog.update(guest.memory, 2, {}), 0);
    CHECK_EQ(status_of(dialog), kFinished);
    CHECK_EQ(static_cast<std::uint32_t>(guest_msg(guest).result),
             sd::error::kMsgDialogErrorCodeInvalid);
    close_msg(dialog, clock);

    // V2 accepts only TEXTSOUND, YESNO and DEFAULT_NO.
    guest.put(kMsgAddress, mpo_msg(kOk, "bad"));
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    wait_running(dialog, clock, sd::MsgDialog::kInitDelayUs);
    dialog.update(guest.memory, 2, {});
    CHECK_EQ(status_of(dialog), kFinished);
    CHECK_EQ(static_cast<std::uint32_t>(guest_msg(guest).result), sd::error::kMsgDialogBadOption);
    close_msg(dialog, clock);

    // V3 DEFAULT_NO without YESNO is a bad option too.
    guest.put(kMsgAddress, mpo_msg(kDefaultNo, "bad", sd::kMsgDialogSizeV3));
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    wait_running(dialog, clock, sd::MsgDialog::kInitDelayUs);
    dialog.update(guest.memory, 2, {});
    CHECK_EQ(static_cast<std::uint32_t>(guest_msg(guest).result), sd::error::kMsgDialogBadOption);
    close_msg(dialog, clock);

    // Abort: only while RUNNING, and only on the next Update.
    CHECK_EQ(dialog.abort(), kInvalidStatus);
    start_msg(dialog, clock, guest, mpo_msg(kTextSound, "Connecting..."));
    CHECK_EQ(dialog.abort(), 0);
    CHECK_EQ(status_of(dialog), kRunning);
    dialog.update(guest.memory, 2, {});
    CHECK_EQ(status_of(dialog), kFinished);
    close_msg(dialog, clock);

    // An invalid request address is ignored (PPSSPP returns 0, no status).
    CHECK_EQ(dialog.init(guest.memory, 0x10), 0);
    CHECK_EQ(status_of(dialog), kNone);
    // A size larger than any version reads no more than V3.
    auto huge = mpo_msg(kTextSound, "huge");
    huge.common.size = 0x10000;
    guest.put(kMsgAddress, huge);
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    CHECK_EQ(dialog.request().common.size, 0x10000u);
    wait_running(dialog, clock, sd::MsgDialog::kInitDelayUs);
    finish_fade_in(dialog, guest);
    dialog.update(guest.memory, 2, press(btn::kCross));
    run_until_finished(dialog, guest);
    close_msg(dialog, clock);
}

class CountingLock final : public sd::VolatileMemoryHooks {
public:
    bool lock() override {
        ++locks;
        return available;
    }
    void unlock() override { ++unlocks; }
    bool available = true;
    int locks = 0;
    int unlocks = 0;
};

void test_volatile_and_manual_jobs() {
    VirtualClock clock;
    Guest guest;
    sd::MsgDialog dialog(clock);
    CountingLock lock;
    dialog.set_volatile_hooks(&lock);

    // A busy lock keeps the dialog initializing (the job thread would block).
    lock.available = false;
    guest.put(kMsgAddress, mpo_msg(sd::msg_option::kTextSound, "x"));
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    clock.advance(sd::MsgDialog::kInitDelayUs);
    CHECK_EQ(status_of(dialog), kInitialize);
    CHECK(dialog.job().has_value());
    lock.available = true;
    CHECK_EQ(status_of(dialog), kRunning);
    CHECK_EQ(lock.locks, 2);
    finish_fade_in(dialog, guest);
    dialog.update(guest.memory, 2, press(btn::kCross));
    run_until_finished(dialog, guest);
    CHECK_EQ(lock.unlocks, 0);
    close_msg(dialog, clock);
    // Unlocked once the shutdown ends.
    CHECK_EQ(lock.unlocks, 1);

    // Manual jobs: the HLE's own thread ends them.
    dialog.set_manual_jobs(true);
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    clock.advance(10 * sd::MsgDialog::kInitDelayUs);
    CHECK_EQ(status_of(dialog), kInitialize);
    CHECK(!dialog.next_deadline_us().has_value());
    CHECK_EQ(dialog.complete_job(), 0);
    CHECK_EQ(status_of(dialog), kRunning);
    CHECK_EQ(dialog.complete_job(), -1);
    finish_fade_in(dialog, guest);
    dialog.update(guest.memory, 2, press(btn::kCross));
    run_until_finished(dialog, guest);
    CHECK_EQ(dialog.shutdown(), 0);
    CHECK_EQ(status_of(dialog), kShutdown);
    CHECK_EQ(dialog.complete_job(), 0);
    CHECK_EQ(status_of(dialog), kNone);

    // Forced shutdown: NONE at once, without a job.
    dialog.set_manual_jobs(false);
    CHECK_EQ(dialog.init(guest.memory, kMsgAddress), 0);
    CHECK_EQ(dialog.shutdown(true), 0);
    CHECK_EQ(status_of(dialog), kNone);
}

// --- OSK --------------------------------------------------------------------

// The request of FUN_08bb7ad8/FUN_08bbd430 in title ("INPUT YOUR NAME").
void put_mpo_osk(Guest &guest, std::uint32_t limit = 15, const std::u16string &intext = u"") {
    sd::OskParams params{};
    params.common = mpo_common(sizeof(sd::OskParams), 0, 0x33);
    params.common.graphics_thread = 0x31;
    params.common.font_thread = 0x32;
    params.common.sound_thread = 0x10;
    params.field_count = 1;
    params.fields = kOskField;
    guest.put(kOskAddress, params);
    sd::OskFieldData field{};
    field.unk_00 = 4;
    field.language = 1;
    field.input_type = sd::osk_input::kLatinDigit | sd::osk_input::kLatinUppercase; // 9
    field.lines = 1;
    field.desc = kOskDesc;
    field.intext = kOskIntext;
    field.outtext_length = 0x200;
    field.outtext = kOskOuttext;
    field.outtext_limit = limit;
    guest.put(kOskField, field);
    guest.put_u16string(kOskDesc, u"INPUT YOUR NAME");
    guest.put_u16string(kOskIntext, intext);
    // A sentinel after the output, to check that only text + NUL is written.
    for (std::uint32_t i = 0; i < 0x400; ++i) guest.at(kOskOuttext)[i] = 0xEE;
}

void start_osk(sd::OskDialog &osk, VirtualClock &clock, Guest &guest) {
    CHECK_EQ(osk.init(guest.memory, kOskAddress), 0);
    wait_running(osk, clock, sd::OskDialog::kInitDelayUs);
    finish_fade_in(osk, guest);
}

void close_osk(sd::OskDialog &osk, VirtualClock &clock, Guest &guest) {
    run_until_finished(osk, guest);
    CHECK_EQ(osk.shutdown(), 0);
    clock.advance(sd::OskDialog::kShutdownDelayUs);
    CHECK_EQ(status_of(osk), kNone);
}

void test_osk_keyboard_model() {
    sd::OskKeyboard keyboard(9, 15, u"");
    // Input type 9: PPSSPP keeps the lowercase keyboard (it has the digits).
    CHECK(keyboard.layout() == sd::OskLayout::kLatinLowercase);
    CHECK(keyboard.can_shift());
    CHECK_EQ(keyboard.selected_key(), u'1');
    // Left from column 0 wraps to column 11 of the same row.
    keyboard.move_left();
    CHECK_EQ(keyboard.row(), 0);
    CHECK_EQ(keyboard.column(), 11);
    CHECK_EQ(keyboard.selected_key(), u'+');
    keyboard.move_right();
    CHECK_EQ(keyboard.column(), 0);
    // Up from row 0 wraps to row 3.
    keyboard.move_up();
    CHECK_EQ(keyboard.row(), 3);
    CHECK_EQ(keyboard.selected_key(), u'z');
    keyboard.move_down();
    CHECK_EQ(keyboard.row(), 0);
    keyboard.shift();
    CHECK(keyboard.layout() == sd::OskLayout::kLatinUppercase);
    CHECK_EQ(keyboard.selected_key(), u'!');
    // Only uppercase requested: starts uppercase.
    CHECK(sd::OskKeyboard(sd::osk_input::kLatinUppercase, 4, u"").layout() ==
          sd::OskLayout::kLatinUppercase);
    // Limit and control characters on host typing.
    sd::OskKeyboard small(0, 4, u"AB");
    small.type(u"C\nDEF");
    CHECK(small.text() == u"ABCD");
    small.space();
    CHECK(small.text() == u"ABCD");
    small.backspace();
    small.space();
    CHECK(small.text() == u"ABC ");
    for (int i = 0; i < 10; ++i) small.backspace();
    CHECK(small.text().empty());
    CHECK_EQ(sd::OskKeyboard::key_at(sd::OskLayout::kLatinLowercase, 4, 0), u'\0');
}

void test_osk_pad_sequence() {
    VirtualClock clock;
    Guest guest;
    sd::OskDialog osk(clock);
    put_mpo_osk(guest);
    CHECK_EQ(osk.init(guest.memory, kOskAddress), 0);
    CHECK_EQ(status_of(osk), kInitialize);
    clock.advance(sd::OskDialog::kInitDelayUs - 1);
    CHECK_EQ(status_of(osk), kInitialize);
    clock.advance(1);
    CHECK_EQ(status_of(osk), kRunning);
    CHECK_EQ(osk.view().description, std::string("INPUT YOUR NAME"));
    CHECK_EQ(osk.view().max_length, 15u);
    finish_fade_in(osk, guest);

    // "12wW " then erase the space: circle types, right moves, down moves a
    // row, select shifts, square is a space, cross erases.
    osk.update(guest.memory, 2, press(btn::kCircle));
    osk.update(guest.memory, 2, press(btn::kRight));
    osk.update(guest.memory, 2, press(btn::kCircle));
    osk.update(guest.memory, 2, press(btn::kDown));
    osk.update(guest.memory, 2, press(btn::kCircle));
    osk.update(guest.memory, 2, press(btn::kSelect));
    osk.update(guest.memory, 2, press(btn::kCircle));
    osk.update(guest.memory, 2, press(btn::kSquare));
    CHECK(osk.keyboard().text() == u"12wW ");
    osk.update(guest.memory, 2, press(btn::kCross));
    CHECK(osk.keyboard().text() == u"12wW");
    // The guest sees the text, NUL-terminated, and nothing past what was
    // ever written: "12wW " + NUL took 12 bytes, and erasing does not clear
    // the old terminator (as in PPSSPP).
    CHECK(guest.get_u16string(kOskOuttext) == u"12wW");
    CHECK_EQ(guest.at(kOskOuttext)[12], 0xEE);
    CHECK_EQ(guest.get<std::int32_t>(
                 kOskField + static_cast<std::uint32_t>(offsetof(sd::OskFieldData, result))),
             sd::osk_result::kChanged);
    CHECK_EQ(guest.get<std::int32_t>(
                 kOskAddress + static_cast<std::uint32_t>(offsetof(sd::DialogCommon, result))),
             0);
    // L/R (language cycling) do nothing.
    osk.update(guest.memory, 2, press(btn::kRTrigger));
    CHECK(osk.view().layout == sd::OskLayout::kLatinUppercase);

    // Holding right repeats after 10 frames, then every 5.
    const int column = osk.keyboard().column();
    osk.update(guest.memory, 2, press(btn::kRight)); // the press moves once
    for (int frame = 1; frame <= 20; ++frame) osk.update(guest.memory, 2, hold(btn::kRight));
    // Held frames 10, 15 and 20 repeat: 1 + 3 moves.
    CHECK_EQ(osk.keyboard().column(), (column + 4) % 12);

    // Start finishes.
    osk.update(guest.memory, 2, press(btn::kStart));
    CHECK(osk.is_fading());
    close_osk(osk, clock, guest);
    CHECK(guest.get_u16string(kOskOuttext) == u"12wW");
}

void test_osk_limits_and_host_input() {
    VirtualClock clock;
    Guest guest;
    sd::OskDialog osk(clock);

    // The limit counts characters; the initial text is kept.
    put_mpo_osk(guest, 15, u"SNAKE");
    start_osk(osk, clock, guest);
    CHECK(osk.keyboard().text() == u"SNAKE");
    CHECK(osk.host_type(u"_EATER_123456789"));
    CHECK(osk.keyboard().text() == u"SNAKE_EATER_123");
    osk.update(guest.memory, 2, press(btn::kCircle)); // full: no more keys
    CHECK_EQ(osk.keyboard().text().size(), 15u);
    CHECK(osk.host_backspace());
    osk.update(guest.memory, 2, {});
    CHECK(guest.get_u16string(kOskOuttext) == u"SNAKE_EATER_12");
    CHECK(osk.host_finish());
    CHECK(!osk.host_type(u"late")); // fading out
    close_osk(osk, clock, guest);

    // Cancel (PC extension): field.result = CANCELLED.
    put_mpo_osk(guest);
    start_osk(osk, clock, guest);
    CHECK(osk.host_type(u"BOSS"));
    CHECK(osk.host_cancel());
    osk.update(guest.memory, 2, {});
    CHECK_EQ(guest.get<std::int32_t>(
                 kOskField + static_cast<std::uint32_t>(offsetof(sd::OskFieldData, result))),
             sd::osk_result::kCancelled);
    close_osk(osk, clock, guest);

    // outtextlimit 0 or larger than the buffer: the buffer decides.
    put_mpo_osk(guest, 0);
    auto field = guest.get<sd::OskFieldData>(kOskField);
    field.outtext_length = 5;
    guest.put(kOskField, field);
    start_osk(osk, clock, guest);
    CHECK_EQ(osk.keyboard().max_length(), 4u);
    osk.host_type(u"ABCDEFG");
    osk.update(guest.memory, 2, {});
    CHECK(guest.get_u16string(kOskOuttext) == u"ABCD");
    CHECK_EQ(guest.at(kOskOuttext)[10], 0xEE); // 4 chars + NUL = 10 bytes
    osk.update(guest.memory, 2, press(btn::kStart));
    close_osk(osk, clock, guest);

    // Invalid requests.
    put_mpo_osk(guest);
    auto params = guest.get<sd::OskParams>(kOskAddress);
    params.common.size = 0x3C;
    guest.put(kOskAddress, params);
    CHECK_EQ(osk.init(guest.memory, kOskAddress), static_cast<int>(sd::error::kInvalidParamSize));
    params.common.size = sizeof(sd::OskParams);
    params.fields = 0x10;
    guest.put(kOskAddress, params);
    CHECK_EQ(osk.init(guest.memory, kOskAddress), -1);
    CHECK_EQ(osk.init(guest.memory, 0x20), -1);
    CHECK_EQ(status_of(osk), kNone);
}

// --- UtilityDialogs ---------------------------------------------------------

void test_one_dialog_at_a_time() {
    VirtualClock clock;
    Guest guest;
    const auto root = std::filesystem::temp_directory_path() / "mpo_savedata_unused_root";
    sd::SavedataStore store(root); // never touched: no savedata call here
    sd::UtilityDialogs utility(clock, store);
    constexpr std::int32_t kWrongType = static_cast<std::int32_t>(sd::error::kWrongType);

    // Nothing started: every call of a kind fails with WRONG_TYPE.
    CHECK_EQ(utility.msg_get_status().value, kWrongType);
    CHECK_EQ(utility.osk_update(guest.memory, 2, {}).value, kWrongType);
    CHECK_EQ(utility.savedata_get_status().value, kWrongType);
    CHECK_EQ(utility.savedata_get_status().eat_cycles, 200u);

    guest.put(kMsgAddress, mpo_msg(sd::msg_option::kTextSound, "busy"));
    CHECK_EQ(utility.msg_init_start(guest.memory, kMsgAddress).value, 0);
    CHECK(utility.active());
    CHECK(utility.current_type() == sd::DialogType::kMsg);
    put_mpo_osk(guest);
    CHECK_EQ(utility.osk_init_start(guest.memory, kOskAddress).value, kWrongType);
    CHECK_EQ(utility.msg_get_status().value, kInitialize);
    clock.advance(sd::MsgDialog::kInitDelayUs);
    CHECK_EQ(utility.msg_get_status().value, kRunning);
    // Update: the result, delayed 800 us.
    const auto update = utility.msg_update(guest.memory, 2, {});
    CHECK_EQ(update.value, 0);
    CHECK_EQ(update.delay_us, 800u);
    CHECK_EQ(utility.msg_abort().value, 0);
    utility.msg_update(guest.memory, 2, {});
    CHECK_EQ(utility.msg_get_status().value, kFinished);
    CHECK_EQ(utility.msg_shutdown_start().value, 0);
    CHECK(!utility.active());
    // The next dialog may start while the last one still shuts down.
    CHECK_EQ(utility.msg_get_status().value, kShutdown);
    CHECK_EQ(utility.osk_init_start(guest.memory, kOskAddress).value, 0);
    CHECK(utility.current_type() == sd::DialogType::kOsk);
    CHECK_EQ(utility.msg_get_status().value, kWrongType);
    clock.advance(sd::OskDialog::kInitDelayUs);
    CHECK_EQ(utility.osk_get_status().value, kRunning);
    const auto osk_update = utility.osk_update(guest.memory, 2, {});
    CHECK_EQ(osk_update.delay_us, 0u);
    CHECK_EQ(osk_update.eat_cycles, sd::UtilityDialogs::kOskUpdateCycles);
    // Update of a dialog that is not RUNNING: no delay.
    CHECK_EQ(utility.osk_shutdown_start().value, kInvalidStatus);
    CHECK(!utility.active()); // deactivated anyway, as in PPSSPP
    CHECK_EQ(utility.savedata_update(guest.memory, 2, {}).value, kWrongType);
}

} // namespace

int main() {
    test_status_and_delays();
    test_msg_buttons();
    test_msg_errors_and_abort();
    test_volatile_and_manual_jobs();
    test_osk_keyboard_model();
    test_osk_pad_sequence();
    test_osk_limits_and_host_input();
    test_one_dialog_at_a_time();
    return savedata_test::finish("mpo_savedata_dialog_test");
}
