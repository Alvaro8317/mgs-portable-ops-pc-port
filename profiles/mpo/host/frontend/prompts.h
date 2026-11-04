// SPDX-License-Identifier: GPL-3.0-or-later
//
// On-screen button prompts that follow the device (F24b).
//
// The game and its dialogs talk about PSP buttons (cross, circle, START...).
// A PromptSet says what to draw for each of them: the PSP's own symbols, the
// labels of a gamepad (A/B/X/Y, the PlayStation shapes, Menu/View, LB/RB...)
// or the names of the keys and mouse buttons the active input mapping binds
// to it ("Space", "Esc", "Right click"). It is built from the mapping, not
// from a fixed table: if the player rebinds cross to Enter, the prompt says
// "Enter".
//
// DeviceTracker remembers the last device the player used: the keyboard and
// mouse (one device), or one gamepad with its SDL_GamepadType and the labels
// of its face buttons. Only real input changes it: a key or mouse button
// press, a gamepad button press, or a gamepad axis pushed past the activity
// threshold. Stick noise under it and mouse motion alone do not (the mouse
// camera moves the d-pad, but a nudge of the desk must not flip the
// prompts).
//
// Kept free of SDL, like input.h: GamepadType and FaceLabel use SDL3's
// numbers (SDL_GamepadType, SDL_GamepadButtonLabel) and FaceLabelForType is
// SDL_GetGamepadButtonLabelForType (checked against SDL in
// mpo_frontend_sdl_test). A set is presentation only: it never reaches the
// game, but it does change the dialog frames, so recordings keep it
// (input_recording.h, the `p` record).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "input.h"

namespace mpo::frontend {

// SDL_GamepadType values.
enum class GamepadType : std::uint8_t {
    kUnknown = 0,
    kStandard = 1,
    kXbox360 = 2,
    kXboxOne = 3,
    kPs3 = 4,
    kPs4 = 5,
    kPs5 = 6,
    kSwitchPro = 7,
    kJoyconLeft = 8,
    kJoyconRight = 9,
    kJoyconPair = 10,
    kGameCube = 11,
    kSteam = 12,
};
inline constexpr int kGamepadTypeCount = 13; // SDL_GAMEPAD_TYPE_COUNT

// SDL_GamepadButtonLabel values: what is printed on a face button.
enum class FaceLabel : std::uint8_t {
    kUnknown = 0,
    kA = 1,
    kB = 2,
    kX = 3,
    kY = 4,
    kCross = 5,
    kCircle = 6,
    kSquare = 7,
    kTriangle = 8,
};

// SDL_GetGamepadButtonLabelForType: the label of a face button (south, east,
// west, north) on a gamepad of `type`; kUnknown for any other button.
[[nodiscard]] FaceLabel FaceLabelForType(GamepadType type, PadButton button);
// The face labels of a gamepad of `type`, in south, east, west, north order.
[[nodiscard]] std::array<FaceLabel, 4> FaceLabelsForType(GamepadType type);

struct Device {
    enum class Kind : std::uint8_t { kNone, kKeyboard, kGamepad };
    Kind kind = Kind::kNone;
    GamepadType type = GamepadType::kUnknown;
    // South, east, west, north (SDL_GetGamepadButtonLabel of the pad).
    std::array<FaceLabel, 4> face{};
    std::uint32_t id = 0; // SDL_JoystickID of a gamepad.
    friend bool operator==(const Device &, const Device &) = default;
};

// Axes count as input from this tilt (or the analog deadzone, if larger).
inline constexpr float kMinAxisActivity = 0.25f;

class DeviceTracker {
public:
    // A key or mouse button went down (repeats are not new presses, motion is
    // not a press).
    void OnKeyboardInput();
    // A gamepad was connected. It becomes the device when nothing was used
    // yet (a game started with a pad plugged in shows its prompts).
    void OnGamepadAdded(std::uint32_t id, GamepadType type, const std::array<FaceLabel, 4> &face);
    // A gamepad went away. If it was the device, the next connected pad
    // takes over, or the keyboard.
    void OnGamepadRemoved(std::uint32_t id);
    // A gamepad button went down.
    void OnGamepadButton(std::uint32_t id);
    // A gamepad axis moved: input only when |value| >= threshold.
    void OnGamepadAxis(std::uint32_t id, float value, float threshold);

    [[nodiscard]] const Device &last() const noexcept { return last_; }
    // Bumped on every change of last(), so a caller can rebuild its prompts
    // only when needed.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    void Set(const Device &device);
    [[nodiscard]] const Device *Find(std::uint32_t id) const;

    std::vector<Device> pads_;
    Device last_;
    std::uint64_t generation_ = 0;
};

// What a set looks like (the --prompts names).
enum class PromptStyle : std::uint8_t {
    kPsp,         // The PSP's symbols and words, as before F24b.
    kXbox,        // Xbox and Steam pads: A B X Y, Menu, View, LB...
    kPlayStation, // PS3/PS4/PS5: the shapes, Options, Share...
    kNintendo,    // Switch and GameCube: B A Y X, +, -, L, ZL...
    kGeneric,     // Other gamepads: SDL's labels (A B X Y), Start, Back...
    kKeyboard,    // Key and mouse button names from the mapping.
};
[[nodiscard]] const char *PromptStyleName(PromptStyle style);
[[nodiscard]] std::optional<PromptStyle> PromptStyleFromName(std::string_view name);
// The style of a gamepad type.
[[nodiscard]] PromptStyle PromptStyleForGamepad(GamepadType type);

// One prompt. A symbol is one glyph drawn where the PSP's symbol goes (the
// four shapes); a word is plain text where the PSP writes "Start"; a key is
// text drawn in a small box, like a key cap (key names, A/B/X/Y, Menu...).
struct PromptLabel {
    enum class Kind : std::uint8_t { kSymbol, kWord, kKey };
    Kind kind = Kind::kKey;
    std::string text; // UTF-8; empty: draw nothing.
    friend bool operator==(const PromptLabel &, const PromptLabel &) = default;
};

// The PSP buttons the prompts cover.
enum class PromptButton : std::uint8_t {
    kCross,
    kCircle,
    kTriangle,
    kSquare,
    kStart,
    kSelect,
    kL,
    kR,
};
inline constexpr int kPromptButtonCount = 8;
// The PSP button mask (ctrl.h) of each, in PromptButton order.
inline constexpr std::array<std::uint32_t, kPromptButtonCount> kPromptButtonMasks = {
    ctrl::kCross, ctrl::kCircle, ctrl::kTriangle, ctrl::kSquare,
    ctrl::kStart, ctrl::kSelect, ctrl::kLTrigger, ctrl::kRTrigger};

struct PromptSet {
    PromptStyle style = PromptStyle::kPsp;
    std::array<PromptLabel, kPromptButtonCount> buttons;
    // The OSK's own keys when the PC keyboard types into it (keyboard sets:
    // Enter finishes, Backspace deletes; see boot_main.cpp). Empty text: the
    // OSK shows the button's prompt.
    PromptLabel osk_finish;
    PromptLabel osk_delete;

    // The PSP's own (what the dialogs drew before F24b).
    static PromptSet Psp();
    // The prompt of a PSP button mask (ctrl.h); an empty key for any other.
    [[nodiscard]] const PromptLabel &For(std::uint32_t psp_button) const;
    friend bool operator==(const PromptSet &, const PromptSet &) = default;
};

// Optional key namer: the name of a scancode on the player's keyboard
// layout, or "" to use the built-in names (KeyName).
using KeyNamer = std::function<std::string(Scancode)>;

// The short built-in name of a key ("Space", "Esc", "Ctrl", "Q", "F1").
[[nodiscard]] std::string KeyName(Scancode key);
// "Left click", "Right click", "Middle click", "Mouse 4", "Mouse 5".
[[nodiscard]] std::string MouseButtonPromptName(MouseButton button);

// The set of `style` for `mapping`. Gamepad styles take the face labels of
// `pad` when given (its kind is kGamepad), or those of the style's usual pad
// (Xbox One, PS4, Switch Pro, a standard pad). kPsp ignores the mapping.
[[nodiscard]] PromptSet BuildPromptSet(PromptStyle style, const InputMapping &mapping,
                                       const Device *pad = nullptr, const KeyNamer &namer = {});
// The set for the device the player used last: keyboard (also for kNone) or
// the gamepad's style.
[[nodiscard]] PromptSet BuildPromptSetForDevice(const Device &device, const InputMapping &mapping,
                                                const KeyNamer &namer = {});

// The `p` record's words after the sample: "<style>" for the PSP set, else
// "<style> <label> x10" (the eight buttons, osk_finish, osk_delete), each
// label its kind letter (s, w, k) followed by its UTF-8 as hex.
[[nodiscard]] std::string FormatPromptSet(const PromptSet &set);
bool ParsePromptSet(std::span<const std::string_view> words, PromptSet *out);

} // namespace mpo::frontend
