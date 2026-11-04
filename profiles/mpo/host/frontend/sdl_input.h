// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL3 events -> InputTranslator. Keyboard keys are bound by scancode (the
// physical position, so the default map works on AZERTY/QWERTZ too), mouse
// buttons by number (clicks synthesized from touches are ignored), and every
// connected gamepad feeds the same PSP controller: buttons add up, and for
// each axis the last event wins.
//
// Mouse camera (F22, InputMapping::mouse_camera): a click in the window
// captures the mouse (relative mode: the cursor hides and motion is no longer
// limited by the window) and is not a button press; while captured, motion
// goes to InputTranslator::OnMouseMotion. F1 or losing the focus releases it.
// Uncaptured motion is left to the caller, so the menus never move by
// accident.
//
// Right-stick camera (F23a, InputMapping::pad_camera): the stick's values go
// to the translator like any axis, and InputTranslator::EndPoll turns them
// into d-pad presses. Losing the focus pauses it (gaining it resumes it);
// removing a gamepad releases it with the rest of the pad.
//
// Last device (F24b, prompts.h): every key or mouse button press, gamepad
// button press and gamepad axis past the activity threshold (the analog
// deadzone, at least kMinAxisActivity) goes to a DeviceTracker, with the
// pad's SDL_GetGamepadType and the labels of its face buttons
// (SDL_GetGamepadButtonLabel). Mouse motion does not.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "input.h"
#include "prompts.h"

namespace mpo::frontend {

class SdlInput {
public:
    // Initialises SDL's gamepad subsystem (reference counted by SDL). The
    // translator must outlive this object.
    explicit SdlInput(InputTranslator *translator);
    ~SdlInput();
    SdlInput(const SdlInput &) = delete;
    SdlInput &operator=(const SdlInput &) = delete;

    [[nodiscard]] bool ok() const { return initialised_; }
    [[nodiscard]] const std::string &error() const { return error_; }

    // Feeds one event. Returns true when it was keyboard, mouse button,
    // gamepad or focus input (focus lost or gained, consumed here); other
    // events are left to the caller.
    bool HandleEvent(const SDL_Event &event);

    [[nodiscard]] int gamepad_count() const { return static_cast<int>(gamepads_.size()); }

    // Call after each round of HandleEvent (one poll of the window events).
    void EndPoll() { translator_->EndPoll(); }
    [[nodiscard]] bool mouse_captured() const { return captured_; }
    // The device the player used last (F24b). Keys the caller takes for
    // itself (the OSK's text entry) are reported with
    // devices().OnKeyboardInput().
    [[nodiscard]] DeviceTracker &devices() noexcept { return devices_; }
    [[nodiscard]] const DeviceTracker &devices() const noexcept { return devices_; }
    void ReleaseMouseCapture();

    // Called with a short description ("key Z down", "mouse left down",
    // "pad a down",
    // "pad axis leftx +0.500", "gamepad added: <name>") of each event it uses.
    using Observer = std::function<void(std::string_view)>;
    void SetObserver(Observer observer) { observer_ = std::move(observer); }

private:
    void Note(const std::string &text) const;

    InputTranslator *translator_;
    bool initialised_ = false;
    std::string error_;
    std::vector<SDL_Gamepad *> gamepads_;
    DeviceTracker devices_;
    Observer observer_;
    bool captured_ = false;
    SDL_WindowID captured_window_ = 0;
    std::string saved_title_;
};

// Mouse button names of the mapping file: "left", "middle", "right", "x1" and
// "x2" (SDL_BUTTON_*). MouseButtonFromName returns 0 for an unknown name.
[[nodiscard]] const char *MouseButtonName(int button);
[[nodiscard]] int MouseButtonFromName(std::string_view name);

// F24b: names keys on the player's keyboard layout (SDL_GetKeyFromScancode
// and SDL_GetKeyName) for the keys that print a character (letters, digits,
// punctuation), so a key bound by position shows the letter on its cap; ""
// for the others, which keep the built-in names (KeyName).
[[nodiscard]] std::string LayoutKeyName(Scancode key);

// SDL_GetGamepadType and SDL_GetGamepadButtonLabel of an open gamepad.
[[nodiscard]] GamepadType GamepadTypeOf(SDL_Gamepad *pad);
[[nodiscard]] std::array<FaceLabel, 4> FaceLabelsOf(SDL_Gamepad *pad);

// SDL's 16-bit axis value to -1..1 (like PPSSPP: -32768 -> -1, 32767 -> 1).
[[nodiscard]] float AxisValueFromSdl(Sint16 value);

} // namespace mpo::frontend
