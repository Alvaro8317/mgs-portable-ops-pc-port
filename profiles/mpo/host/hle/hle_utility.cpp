// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/HLE/sceUtility.cpp, sceImpose.cpp and the drawing
// of Core/Dialog/PSPDialog.cpp, PSPMsgDialog.cpp, PSPOskDialog.cpp and
// PSPSaveDialog.cpp @ v1.20.4 (see utility.hpp).

#include "hle/utility.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <vector>

#include "hle/hle_args.hpp"
#include "hle/system.hpp"
#include "savedata/guest_text.hpp"

namespace mpo::hle {

namespace {

namespace sd = mpo::savedata;

// The SDK version MPO's module_start declares (docs/analysis/mgp_main.md).
constexpr std::uint32_t kErrInvalidSystemParam = 0x80110103u;
constexpr std::uint32_t kErrStringTooLong = 0x80110104u;

constexpr std::uint32_t kLanguageEnglish = 1u;
constexpr std::uint32_t kButtonCross = 1u;
// PPSSPP's default nickname (g_Config.sNickName).
constexpr const char *kNickname = "PPSSPP";

// PSPDialog.cpp / PSPSaveDialog.cpp / PSPMsgDialog.cpp FONT_SCALE.
constexpr float kDialogFontScale = 0.55f;
constexpr float kMsgFontScale = 0.65f;

std::string utf16_to_utf8(std::u16string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c >= 0xD800u && c < 0xDC00u && i + 1 < text.size() && text[i + 1] >= 0xDC00u &&
            text[i + 1] < 0xE000u) {
            c = 0x10000u + ((c - 0xD800u) << 10u) + (text[i + 1] - 0xDC00u);
            ++i;
        }
        if (c < 0x80u) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (c >> 6u)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else if (c < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (c >> 12u)));
            out.push_back(static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (c >> 18u)));
            out.push_back(static_cast<char>(0x80u | ((c >> 12u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        }
    }
    return out;
}

std::u16string utf8_to_utf16(const std::string &text) {
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t code = 0;
        std::size_t length = 1;
        if (c < 0x80u) {
            code = c;
        } else if ((c & 0xE0u) == 0xC0u) {
            code = c & 0x1Fu;
            length = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            code = c & 0x0Fu;
            length = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            code = c & 0x07u;
            length = 4;
        } else {
            ++i;
            continue;
        }
        if (i + length > text.size()) break;
        for (std::size_t k = 1; k < length; ++k)
            code = (code << 6u) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        i += length;
        if (code >= 0x10000u) {
            code -= 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u + (code >> 10u)));
            out.push_back(static_cast<char16_t>(0xDC00u + (code & 0x3FFu)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

// The button symbols, drawn as text (PPSSPP draws atlas images).
const char *button_symbol(std::uint32_t button) {
    switch (button) {
    case sd::button::kCircle:
        return "○";
    case sd::button::kCross:
        return "×";
    case sd::button::kTriangle:
        return "△";
    case sd::button::kSquare:
        return "□";
    default:
        return "";
    }
}

const char *mode_name(std::uint32_t mode) {
    static constexpr const char *kNames[] = {
        "AUTOLOAD",   "AUTOSAVE",       "LOAD",     "SAVE",           "LISTLOAD", "LISTSAVE",
        "LISTDELETE", "LISTALLDELETE",  "SIZES",    "AUTODELETE",     "DELETE",   "LIST",
        "FILES",      "MAKEDATASECURE", "MAKEDATA", "READDATASECURE", "READDATA", "WRITEDATASECURE",
        "WRITEDATA",  "ERASESECURE",    "ERASE",    "DELETEDATA",     "GETSIZE"};
    return mode < std::size(kNames) ? kNames[mode] : "?";
}

// FormatSaveDate and FormatSaveHourMin with the system settings this HLE
// reports: YYYY/MM/DD and 24 hours.
std::string format_date_time(const std::tm &t) {
    char text[48];
    std::snprintf(text, sizeof(text), "%04d/%02d/%02d   %02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
                  t.tm_mday, t.tm_hour, t.tm_min);
    return text;
}

std::tm local_time(std::time_t seconds) {
    std::tm out{};
#if defined(_WIN32)
    (void)localtime_s(&out, &seconds);
#else
    (void)localtime_r(&seconds, &out);
#endif
    return out;
}

} // namespace

// The dialog jobs take the volatile memory lock as PPSSPP's job thread does
// (sceKernelVolatileMemLock(0, 0, 0)), and clear the area when they give it
// back (PSPDialog::FinishVolatile).
class Utility::VolatileHooks final : public sd::VolatileMemoryHooks {
public:
    explicit VolatileHooks(System &system) : system_(system) {}
    bool lock() override {
        const bool ok = system_.volatile_memory().lock(system_.kernel(), 0u, 0u, 0u) == 0u;
        system_.hle().trace_event("utility: volatile memory %s",
                                  ok ? "locked" : "busy, the dialog waits");
        return ok;
    }
    void unlock() override {
        if (system_.volatile_memory().unlock(system_.kernel(), 0u) != 0u) return;
        system_.kernel().memory().fill(VolatileMemory::kStart, 0u, VolatileMemory::kSize);
        system_.hle().trace_event("utility: volatile memory unlocked");
    }

private:
    System &system_;
};

Utility::Utility(System &system) : system_(system), ppge_(system) {
    sd::StoreConfig config;
    config.compiled_sdk_version = 0x02080010u;
    // Without a cipher nothing is encrypted; the flag only matters with one.
    store_ = std::make_unique<sd::SavedataStore>(system.config().ms0_root, nullptr, config);
    dialogs_ = std::make_unique<sd::UtilityDialogs>(system.clock(), *store_);
    hooks_ = std::make_unique<VolatileHooks>(system);
    dialogs_->set_volatile_hooks(hooks_.get());
}

Utility::~Utility() = default;

std::uint32_t Utility::apply(const sd::UtilityResult &result) {
    if (result.eat_cycles != 0u) system_.clock().eat_cycles(result.eat_cycles);
    const auto value = static_cast<std::uint32_t>(result.value);
    if (result.delay_us != 0u) return system_.kernel().delay_result(value, result.delay_us);
    return value;
}

sd::ButtonState Utility::buttons() {
    sd::ButtonState state;
    state.held = system_.ctrl().peek_buttons();
    state.pressed = system_.ctrl().take_dialog_make();
    return state;
}

// ---- Calls -------------------------------------------------------------------

std::uint32_t Utility::savedata_init_start(std::uint32_t address) {
    const sd::UtilityResult r = dialogs_->savedata_init_start(system_.kernel().memory(), address);
    if (r.value == 0) {
        ++stats_.savedata_runs;
        const sd::SavedataParam &p = dialogs_->savedata().request();
        system_.hle().trace_event(
            "utility: savedata %s (%u), game '%s', save '%s', file '%s', data %u bytes",
            mode_name(p.mode), p.mode, sd::fixed_string(p.game_name, sizeof(p.game_name)).c_str(),
            sd::fixed_string(p.save_name, sizeof(p.save_name)).c_str(),
            sd::fixed_string(p.file_name, sizeof(p.file_name)).c_str(), p.data_size);
        if (const auto mode = dialogs_->savedata().unsupported_mode()) {
            system_.runtime().stop(std::string("sceUtilitySavedataInitStart: mode ") +
                                   mode_name(*mode) + " is not implemented");
        }
    }
    return apply(r);
}

std::uint32_t Utility::savedata_update(int anim_speed) {
    kernel::GuestMemory &m = system_.kernel().memory();
    const sd::DialogStatus before = dialogs_->savedata().status();
    const sd::UtilityResult r = dialogs_->savedata_update(m, anim_speed, buttons());
    trace_store();
    if (r.value >= 0) draw_save();
    if (before == sd::DialogStatus::kRunning &&
        dialogs_->savedata().status() == sd::DialogStatus::kFinished)
        finish_savedata();
    return apply(r);
}

void Utility::finish_savedata() {
    const sd::SavedataParam &p = dialogs_->savedata().request();
    const std::int32_t result = p.common.result;
    system_.hle().trace_event("utility: savedata %s finished, result %08X, save '%s', data %u",
                              mode_name(p.mode), static_cast<std::uint32_t>(result),
                              sd::fixed_string(p.save_name, sizeof(p.save_name)).c_str(),
                              p.data_size);
    if (result == 1) { // SCE_UTILITY_DIALOG_RESULT_CANCEL
        ++stats_.cancelled;
    } else if (result != 0) {
        ++stats_.failures;
        if (store_->needs_cipher())
            std::fprintf(stderr,
                         "mpo: the save is encrypted (from a PSP or PPSSPP); convert it with "
                         "the installer first\n");
    }
}

// Traces what the store did in this call: a dialog can load, save or delete
// more than once before it finishes (LISTALLDELETE goes back to its list).
void Utility::trace_store() {
    const sd::SavedataStore::Activity &a = store_->activity();
    const std::uint64_t operations = a.loads + a.saves + a.removes;
    if (operations == store_operations_) return;
    store_operations_ = operations;
    system_.hle().trace_event("utility: store: %llu loads, %llu saves, %llu deletes; last '%s'",
                              static_cast<unsigned long long>(a.loads),
                              static_cast<unsigned long long>(a.saves),
                              static_cast<unsigned long long>(a.removes), a.last_dir.c_str());
}

std::uint32_t Utility::osk_init_start(std::uint32_t address) {
    const sd::UtilityResult r = dialogs_->osk_init_start(system_.kernel().memory(), address);
    if (r.value == 0) {
        ++stats_.osk_runs;
        const sd::OskDialogView v = dialogs_->osk().view();
        system_.hle().trace_event("utility: osk '%s', limit %zu", v.description.c_str(),
                                  v.max_length);
        osk_script_pending_ = !osk_script_.empty();
    }
    return apply(r);
}

std::uint32_t Utility::osk_update(int anim_speed) {
    kernel::GuestMemory &m = system_.kernel().memory();
    sd::OskDialog &osk = dialogs_->osk();
    // The keyboard is ignored until the fade-in ends, so the script waits.
    if (osk_script_pending_ && osk.host_type(utf8_to_utf16(osk_script_.front()))) {
        osk_script_pending_ = false;
        (void)osk.host_finish();
        system_.hle().trace_event("utility: osk typed '%s' (script)", osk_script_.front().c_str());
        osk_script_.pop_front();
    }
    const sd::UtilityResult r = dialogs_->osk_update(m, anim_speed, buttons());
    if (r.value >= 0) draw_osk();
    return apply(r);
}

std::uint32_t Utility::msg_init_start(std::uint32_t address) {
    const sd::UtilityResult r = dialogs_->msg_init_start(system_.kernel().memory(), address);
    if (r.value == 0) {
        ++stats_.msg_runs;
        system_.hle().trace_event("utility: message '%s'", dialogs_->msg().view().text.c_str());
    }
    return apply(r);
}

std::uint32_t Utility::msg_update(int anim_speed) {
    kernel::GuestMemory &m = system_.kernel().memory();
    const sd::UtilityResult r = dialogs_->msg_update(m, anim_speed, buttons());
    if (r.value >= 0) draw_msg();
    return apply(r);
}

// ---- The PC keyboard ---------------------------------------------------------

bool Utility::osk_takes_text() {
    return dialogs_->active() && dialogs_->current_type() == sd::DialogType::kOsk &&
           dialogs_->osk().status() == sd::DialogStatus::kRunning;
}

bool Utility::host_type(const std::string &utf8) {
    return osk_takes_text() && dialogs_->osk().host_type(utf8_to_utf16(utf8));
}
bool Utility::host_backspace() {
    return osk_takes_text() && dialogs_->osk().host_backspace();
}
bool Utility::host_finish() {
    return osk_takes_text() && dialogs_->osk().host_finish();
}
bool Utility::host_cancel() {
    return osk_takes_text() && dialogs_->osk().host_cancel();
}

// ---- Drawing -------------------------------------------------------------------

void Utility::start_draw(std::uint32_t fade) {
    // PSPDialog::StartDraw.
    (void)ppge_.begin();
    ppge_.draw_rect(0.0f, 0.0f, 480.0f, 272.0f, Ppge::faded(0x20000000u, fade));
}

// PSPDialog::DisplayButtons.
void Utility::display_buttons(bool ok, bool cancel, std::uint32_t fade, std::uint32_t ok_button,
                              std::uint32_t cancel_button, bool button_swap,
                              const std::string &ok_caption, const std::string &back_caption) {
    Ppge::Style text;
    text.align = Ppge::kLeft;
    text.scale = kDialogFontScale;
    text.color = Ppge::faded(0xFFFFFFFFu, fade);
    text.shadow = true;
    text.shadow_color = Ppge::faded(0x80000000u, fade);
    Ppge::Style symbol = text;
    symbol.align = Ppge::kCenter;
    float x1 = 183.5f;
    float x2 = 261.5f;
    if (button_swap) std::swap(x1, x2);
    if (ok) {
        ppge_.draw_text(button_symbol(ok_button), x2 + 5.75f, 261.75f, symbol);
        ppge_.draw_text(ok_caption.empty() ? "Enter" : ok_caption, x2 + 14.5f, 252.0f, text);
    }
    if (cancel) {
        ppge_.draw_text(button_symbol(cancel_button), x1 + 5.75f, 261.75f, symbol);
        ppge_.draw_text(back_caption.empty() ? "Back" : back_caption, x1 + 14.5f, 252.0f, text);
    }
}

// PSPMsgDialog::Update's drawing and DisplayMessage.
void Utility::draw_msg() {
    const sd::MsgDialogView v = dialogs_->msg().view();
    const std::uint32_t fade = v.fade;
    start_draw(fade);
    ppge_.draw_rect(0.0f, 0.0f, 480.0f, 272.0f, Ppge::faded(0xC0C8B2ACu, fade));

    Ppge::Style button_style;
    button_style.align = Ppge::kCenter;
    button_style.scale = kMsgFontScale;
    button_style.color = Ppge::faded(0xFFFFFFFFu, fade);
    button_style.shadow = true;
    button_style.shadow_color = Ppge::faded(0x80000000u, fade);
    Ppge::Style message_style = button_style;
    message_style.align = Ppge::kHCenter;

    if (!v.text.empty()) {
        constexpr float kWrapWidth = 340.0f;
        float total_height = 0.0f;
        ppge_.measure_text(nullptr, &total_height, v.text, kMsgFontScale, Ppge::kWrapWord,
                           kWrapWidth);
        // The PSP shows about 8 lines at a time.
        const float visible_height = std::min(total_height, 175.0f);
        const float h2 = visible_height / 2.0f;
        float center_y = 135.0f;
        float sy = center_y - h2 - 15.0f;
        float ey = center_y + h2 + 20.0f;
        float button_y = center_y + h2 + 5.0f;
        const auto selection_box = [&](float x) {
            constexpr float kW = 15.0f;
            constexpr float kH = 8.0f;
            ppge_.draw_rect(x - kW, button_y - kH, x + kW, button_y + kH,
                            Ppge::faded(0x6DCFCFCFu, fade));
            center_y -= kH + 5.0f;
            sy -= kH + 5.0f;
            ey = button_y + kH * 2.0f + 5.0f;
        };
        if (v.has_yes_no) {
            selection_box(v.yes_selected ? 204.0f : 273.0f);
            ppge_.draw_text("Yes", 203.0f, button_y - 1.0f, button_style);
            ppge_.draw_text("No", 272.0f, button_y - 1.0f, button_style);
            button_y += 8.0f + 5.0f;
        }
        if (v.has_ok) {
            selection_box(240.0f);
            ppge_.draw_text("OK", 239.0f, button_y - 1.0f, button_style);
            button_y += 8.0f + 5.0f;
        }
        ppge_.scissor(0, static_cast<int>(center_y - h2 - 2.0f), 480,
                      static_cast<int>(center_y + h2 + 2.0f));
        ppge_.draw_text_wrapped(v.text, 240.0f, center_y - h2, kWrapWidth, 0.0f, message_style);
        ppge_.scissor_reset();
        ppge_.draw_rect(40.0f, sy, 440.0f, sy + 1.0f, Ppge::faded(0xFFFFFFFFu, fade));
        ppge_.draw_rect(40.0f, ey, 440.0f, ey + 1.0f, Ppge::faded(0xFFFFFFFFu, fade));
    }
    const bool swap = v.ok_button == sd::button::kCross;
    if (v.shows_ok_button)
        display_buttons(true, false, fade, v.ok_button, v.cancel_button, swap, v.ok_caption);
    if (v.shows_back_button)
        display_buttons(false, true, fade, v.ok_button, v.cancel_button, swap, {}, v.back_caption);
    ppge_.end();
    ++stats_.drawn_frames;
}

// PSPOskDialog::Update's drawing and RenderKeyboard.
void Utility::draw_osk() {
    const sd::OskDialogView v = dialogs_->osk().view();
    const std::uint32_t fade = v.fade;
    start_draw(fade);
    ppge_.draw_rect(0.0f, 0.0f, 480.0f, 272.0f, Ppge::faded(0x63636363u, fade));

    const auto style = [fade](std::uint32_t align, float scale) {
        Ppge::Style s;
        s.align = align;
        s.scale = scale;
        s.color = Ppge::faded(0xFFFFFFFFu, fade);
        s.shadow = true;
        s.shadow_color = Ppge::faded(0x80000000u, fade);
        return s;
    };
    constexpr std::size_t kFieldDrawMax = 16;
    const std::size_t draw_limit = std::min(kFieldDrawMax, v.max_length);
    const float keyboard_left = (480.0f - 24.0f * sd::OskKeyboard::kColumns) / 2.0f;
    constexpr float kCharacterWidth = 12.0f;
    const float preview_left = (480.0f - 12.0f * static_cast<float>(draw_limit)) / 2.0f;
    const float title_x = (480.0f - 0.5f * static_cast<float>(draw_limit)) / 2.0f;

    ppge_.draw_text(v.description, title_x, 20.0f, style(Ppge::kCenter, 0.5f));
    const Ppge::Style text_style = style(Ppge::kHCenter, 0.5f);
    const Ppge::Style key_style = style(Ppge::kHCenter, 0.6f);
    Ppge::Style selected_style = key_style;
    selected_style.color = Ppge::faded(0xFF3060FFu, fade);

    // The text, its last draw_limit characters, then underscores.
    const std::size_t first = v.text.size() > draw_limit ? v.text.size() - draw_limit : 0;
    for (std::size_t i = 0; i < draw_limit; ++i) {
        const std::size_t index = first + i;
        const float x = preview_left + static_cast<float>(i) * kCharacterWidth;
        if (index < v.text.size())
            ppge_.draw_text(utf16_to_utf8(std::u16string(1, v.text[index])), x, 40.0f, text_style);
        else
            ppge_.draw_text("_", x, 40.0f, text_style);
    }
    for (int row = 0; row < sd::OskKeyboard::kRows; ++row) {
        for (int col = 0; col < sd::OskKeyboard::kColumns; ++col) {
            const char16_t key = sd::OskKeyboard::key_at(v.layout, row, col);
            const std::string label = utf16_to_utf8(std::u16string(1, key));
            const float x =
                keyboard_left + 25.0f * static_cast<float>(col) + kCharacterWidth / 2.0f;
            const float y = 70.0f + 25.0f * static_cast<float>(row);
            if (row == v.selected_row && col == v.selected_column) {
                ppge_.draw_text(label, x, y, selected_style);
                ppge_.draw_text("_", x, y, key_style);
            } else {
                ppge_.draw_text(label, x, y, key_style);
            }
        }
    }

    // The guide at the bottom.
    const Ppge::Style action = style(Ppge::kLeft, 0.5f);
    const Ppge::Style guide = style(Ppge::kLeft, 0.6f);
    Ppge::Style symbol = guide;
    symbol.align = Ppge::kCenter;
    ppge_.draw_text(button_symbol(sd::button::kSquare), 373.0f, 230.0f, symbol);
    ppge_.draw_text("Space", 390.0f, 222.0f, action);
    ppge_.draw_text(button_symbol(v.ok_button), 53.0f, 230.0f, symbol);
    ppge_.draw_text(button_symbol(v.cancel_button), 53.0f, 255.0f, symbol);
    ppge_.draw_text("Select", 75.0f, 222.0f, action);
    ppge_.draw_text("Delete", 75.0f, 247.0f, action);
    ppge_.draw_text("Start", 135.0f, 220.0f, guide);
    ppge_.draw_text("Finish", 185.0f, 222.0f, action);
    if (v.can_shift) {
        ppge_.draw_text("Select", 135.0f, 245.0f, guide);
        ppge_.draw_text("Shift", 185.0f, 247.0f, action);
    }
    ppge_.end();
    ++stats_.drawn_frames;
}

// PSPSaveDialog::Update's drawing.
void Utility::draw_save() {
    const sd::SaveDialogView v = dialogs_->savedata().view();
    if (v.screen == sd::SaveScreen::kNone) return;
    const std::uint32_t fade = v.fade;
    const auto style = [fade](std::uint32_t align, float scale) {
        Ppge::Style s;
        s.align = align;
        s.scale = scale;
        s.color = Ppge::faded(0xFFFFFFFFu, fade);
        s.shadow = true;
        s.shadow_color = Ppge::faded(0x80000000u, fade);
        return s;
    };
    const auto mtime = [](const sd::SaveFileInfo &info) {
        if (!info.modified) return std::tm{};
        // file_clock::to_sys is missing in MSVC's STL: go through now() of both
        // clocks instead.
        const auto system_time =
            std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                *info.modified - std::filesystem::file_time_type::clock::now());
        return local_time(std::chrono::system_clock::to_time_t(system_time));
    };
    const sd::SaveFileInfo *current =
        v.selected >= 0 && v.selected < static_cast<int>(v.entries.size())
            ? &v.entries[static_cast<std::size_t>(v.selected)]
            : nullptr;
    const sd::SavedataParam &request = dialogs_->savedata().request();

    start_draw(fade);
    // DisplaySaveIcon: no PNG decoder here, so the placeholder PPSSPP draws
    // without a texture.
    const auto save_icon = [&] {
        ppge_.draw_rect(27.0f, 97.0f, 27.0f + 144.0f, 97.0f + 80.0f,
                        Ppge::faded(0x88666666u, fade));
    };
    // DisplaySaveDataInfo2.
    const auto save_info2 = [&](bool new_data) {
        std::tm when{};
        std::string title;
        std::uint64_t size = 0;
        if (new_data || current == nullptr || current->size == 0) {
            const std::uint64_t now = system_.kernel().now_us();
            when = local_time(static_cast<std::time_t>(system_.rtc().unix_us(now) / 1000000));
            title = sd::fixed_string(request.sfo_param.savedata_title,
                                     sizeof(request.sfo_param.savedata_title));
            size = request.data_size;
        } else {
            when = mtime(*current);
            title = current->save_title;
            size = current->size;
        }
        char line[64];
        std::snprintf(line, sizeof(line), "\n%s\n%llu KB", format_date_time(when).c_str(),
                      static_cast<unsigned long long>(size / 1024u));
        ppge_.draw_text_wrapped(title + line, 8.0f, 200.0f, 0.0f, 0.0f, style(Ppge::kLeft, 0.5f));
    };
    // PSPSaveDialog::DisplayMessage.
    const auto message = [&](const std::string &text, bool yes_no) {
        const Ppge::Style text_style = style(Ppge::kCenter, kDialogFontScale);
        constexpr float kWrapWidth = 254.0f;
        float y = 136.0f;
        float h = 0.0f;
        ppge_.measure_text(nullptr, &h, text, kDialogFontScale, Ppge::kWrapWord, kWrapWidth);
        float h2 = h / 2.0f;
        if (yes_no) {
            const char *choice = v.yes_selected ? "Yes" : "No";
            const float x = v.yes_selected ? 302.0f : 366.0f;
            float w = 0.0f;
            ppge_.measure_text(&w, &h, choice, kDialogFontScale);
            w = w / 2.0f + 5.5f;
            h /= 2.0f;
            const float y2 = y + h2 + 4.0f;
            h2 += h + 4.0f;
            y = 132.0f - h;
            ppge_.draw_rect(x - w, y2 - h, x + w, y2 + h, Ppge::faded(0x40C0C0C0u, fade));
            ppge_.draw_text("Yes", 302.0f, y2, text_style);
            ppge_.draw_text("No", 366.0f, y2, text_style);
        }
        ppge_.draw_text_wrapped(text, 334.0f, y, kWrapWidth, 0.0f, text_style);
        const float sy = 122.0f - h2;
        const float ey = 150.0f + h2;
        ppge_.draw_rect(202.0f, sy, 466.0f, sy + 1.0f, Ppge::faded(0xFFFFFFFFu, fade));
        ppge_.draw_rect(202.0f, ey, 466.0f, ey + 1.0f, Ppge::faded(0xFFFFFFFFu, fade));
    };

    switch (v.screen) {
    case sd::SaveScreen::kSaveListChoice:
    case sd::SaveScreen::kLoadListChoice:
    case sd::SaveScreen::kDeleteListChoice: {
        // DisplaySaveList.
        for (int i = 0; i < static_cast<int>(v.entries.size()); ++i) {
            float w = 81.0f;
            float h = 45.0f;
            float x = 58.5f;
            float y = 97.0f;
            if (i == v.selected) {
                w = 144.0f;
                h = 80.0f;
                x = 27.0f;
            }
            if (i < v.selected)
                y -= 13.0f + 45.0f * static_cast<float>(v.selected - i);
            else if (i > v.selected)
                y += 48.0f + 45.0f * static_cast<float>(i - v.selected);
            if (y > 472.0f || y < -200.0f) continue;
            ppge_.draw_rect(x, y, x + w, y + h, 0x88666666u);
            if (i == v.selected) {
                constexpr float kB = 1.2f;
                const std::uint32_t bc = Ppge::faded(0xD0FFFFFFu, fade);
                ppge_.draw_rect(x - kB, y - kB, x + w + kB, y, bc);
                ppge_.draw_rect(x - kB, y, x, y + h, bc);
                ppge_.draw_rect(x - kB, y + h, x + w + kB, y + h + kB, bc);
                ppge_.draw_rect(x + w, y, x + w + kB, y + h, bc);
            }
            // The slot's name inside its box, which PPSSPP leaves to the icon.
            const sd::SaveFileInfo &entry = v.entries[static_cast<std::size_t>(i)];
            ppge_.draw_text(entry.save_name, x + w / 2.0f, y + h / 2.0f,
                            style(Ppge::kCenter, 0.5f));
        }
        // DisplaySaveDataInfo1.
        if (current != nullptr) {
            const Ppge::Style save_title_style = style(Ppge::kLeft, kDialogFontScale);
            if (current->broken) {
                ppge_.draw_text("Corrupted Data", 180.0f, 136.0f, style(Ppge::kVCenter, 0.6f));
                ppge_.draw_text(current->title, 175.0f, 159.0f, save_title_style);
            } else if (current->size == 0) {
                ppge_.draw_text("NEW DATA", 180.0f, 136.0f, style(Ppge::kVCenter, 0.6f));
            } else {
                ppge_.draw_rect(180.0f, 136.0f, 480.0f, 137.0f, Ppge::faded(0xFFFFFFFFu, fade));
                Ppge::Style title_style = style(Ppge::kBottom, 0.6f);
                title_style.color = Ppge::faded(0xFFC0C0C0u, fade);
                char line[64];
                std::snprintf(line, sizeof(line), "%s  %llu KB",
                              format_date_time(mtime(*current)).c_str(),
                              static_cast<unsigned long long>(current->size / 1024u));
                ppge_.draw_text(current->title, 180.0f, 136.0f, title_style);
                ppge_.draw_text(line, 180.0f, 137.0f, style(Ppge::kLeft, 0.5f));
                ppge_.draw_text(current->save_title, 175.0f, 159.0f, save_title_style);
                ppge_.draw_text_wrapped(current->save_detail, 175.0f, 181.0f, 480.0f - 175.0f,
                                        250.0f - 181.0f, style(Ppge::kLeft, 0.5f));
            }
        }
        break;
    }
    case sd::SaveScreen::kLoadNoData:
    case sd::SaveScreen::kDeleteNoData:
        message(v.message, false);
        break;
    default:
        save_icon();
        save_info2(v.screen == sd::SaveScreen::kSaveConfirm ||
                   v.screen == sd::SaveScreen::kSaveSaving ||
                   v.screen == sd::SaveScreen::kSaveFailed);
        message(v.message, v.has_yes_no);
        break;
    }
    const bool swap = v.ok_button == sd::button::kCross;
    if (v.shows_ok_button || v.shows_back_button)
        display_buttons(v.shows_ok_button, v.shows_back_button, fade, v.ok_button, v.cancel_button,
                        swap);
    // DisplayBanner.
    if (v.banner != sd::SaveBanner::kNone) {
        ppge_.draw_rect(0.0f, 0.0f, 480.0f, 23.0f, Ppge::faded(0x65636358u, fade));
        Ppge::Style banner = style(Ppge::kVCenter, 0.6f);
        banner.shadow = false;
        const char *title = v.banner == sd::SaveBanner::kSave   ? "Save"
                            : v.banner == sd::SaveBanner::kLoad ? "Load"
                                                                : "Delete";
        ppge_.draw_rect(10.0f, 6.0f, 22.0f, 18.0f, Ppge::faded(0xFFFFFFFFu, fade));
        ppge_.draw_text(title, 30.0f, 11.0f, banner);
    }
    ppge_.end();
    ++stats_.drawn_frames;
}

// ---- NIDs ----------------------------------------------------------------------

void Utility::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = system_.kernel();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("sceUtility", nid, name, HleKind::kImplemented, std::move(handler));
    };
    sd::UtilityDialogs &d = *dialogs_;

    // sceUtilitySavedata.
    def(0x50C4CD57u, "sceUtilitySavedataInitStart",
        [this](Ctx &c) { ret(c, savedata_init_start(arg(c, 0))); });
    def(0x9790B33Cu, "sceUtilitySavedataShutdownStart",
        [this, &d](Ctx &c) { ret(c, apply(d.savedata_shutdown_start())); });
    def(0xD4B95FFBu, "sceUtilitySavedataUpdate",
        [this](Ctx &c) { ret(c, savedata_update(static_cast<std::int32_t>(arg(c, 0)))); });
    def(0x8874DBE0u, "sceUtilitySavedataGetStatus",
        [this, &d](Ctx &c) { ret(c, apply(d.savedata_get_status())); });

    // sceUtilityOsk.
    def(0xF6269B82u, "sceUtilityOskInitStart",
        [this](Ctx &c) { ret(c, osk_init_start(arg(c, 0))); });
    def(0x3DFAEBA9u, "sceUtilityOskShutdownStart",
        [this, &d](Ctx &c) { ret(c, apply(d.osk_shutdown_start())); });
    def(0x4B85C861u, "sceUtilityOskUpdate",
        [this](Ctx &c) { ret(c, osk_update(static_cast<std::int32_t>(arg(c, 0)))); });
    def(0xF3F76017u, "sceUtilityOskGetStatus",
        [this, &d](Ctx &c) { ret(c, apply(d.osk_get_status())); });

    // sceUtilityMsgDialog.
    def(0x2AD8E239u, "sceUtilityMsgDialogInitStart",
        [this](Ctx &c) { ret(c, msg_init_start(arg(c, 0))); });
    def(0x67AF3428u, "sceUtilityMsgDialogShutdownStart",
        [this, &d](Ctx &c) { ret(c, apply(d.msg_shutdown_start())); });
    def(0x95FC253Bu, "sceUtilityMsgDialogUpdate",
        [this](Ctx &c) { ret(c, msg_update(static_cast<std::int32_t>(arg(c, 0)))); });
    def(0x9A1C91D7u, "sceUtilityMsgDialogGetStatus",
        [this, &d](Ctx &c) { ret(c, apply(d.msg_get_status())); });
    def(0x4DB1E739u, "sceUtilityMsgDialogAbort",
        [this, &d](Ctx &c) { ret(c, apply(d.msg_abort())); });

    // System parameters: PPSSPP's defaults (English, Cross confirms).
    def(0xA5DA2406u, "sceUtilityGetSystemParamInt", [&k](Ctx &c) {
        std::uint32_t value = 0;
        switch (arg(c, 0)) {
        case 2: // Ad hoc channel: automatic.
            k.memory().write_u32(arg(c, 1), 0u);
            return ret(c, 0x800ADF4u);
        case 3:  // WLAN power save: off.
        case 4:  // Date format: YYYYMMDD.
        case 5:  // Time format: 24 h.
        case 6:  // Time zone: 0.
        case 7:  // Daylight savings: standard.
        case 10: // Parental level: 0.
            value = 0u;
            break;
        case 8:
            value = kLanguageEnglish;
            break;
        case 9:
            value = kButtonCross;
            break;
        default:
            return ret(c, kErrInvalidSystemParam);
        }
        k.memory().write_u32(arg(c, 1), value);
        ret(c, 0u);
    });
    def(0x34B78343u, "sceUtilityGetSystemParamString", [&k](Ctx &c) {
        const std::uint32_t dest = arg(c, 1);
        const auto size = static_cast<std::int32_t>(arg(c, 2));
        if (size < 0 || !k.memory().is_valid(dest, static_cast<std::uint32_t>(size)))
            return ret(c, 0xFFFFFFFFu);
        if (arg(c, 0) != 1u) return ret(c, kErrInvalidSystemParam); // NICKNAME
        const std::string nickname = kNickname;
        if (size <= static_cast<std::int32_t>(nickname.size())) return ret(c, kErrStringTooLong);
        // strncpy: the name, then zeros up to the size.
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size), 0u);
        std::copy(nickname.begin(), nickname.end(), bytes.begin());
        (void)k.memory().write_bytes(dest, bytes);
        ret(c, 0u);
    });
    t.add("sceImpose", 0x36AA6E91u, "sceImposeSetLanguageMode", HleKind::kImplemented, [](Ctx &c) {
        // The language is the system's; a different request is ignored,
        // as in PPSSPP.
        ret(c, 0u);
    });
}

} // namespace mpo::hle
