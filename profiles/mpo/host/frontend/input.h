// SPDX-License-Identifier: GPL-3.0-or-later
// AnalogConfig and the default mapping are ported from PPSSPP @ v1.20.4
// (GPL-2.0-or-later): Core/Config.cpp, Core/ControlMapper.cpp and
// Core/KeyMapDefaults.cpp (see input.cpp).
//
// Host input -> PSP controller state. InputTranslator receives abstract
// events (keyboard scancodes, gamepad buttons and axes), applies a
// configurable InputMapping and produces SceCtrlData. It does not know SDL:
// SdlInput (sdl_input.h) feeds it from SDL events, and the tests feed it
// directly. The sceCtrl HLE (F10b) samples it once per vblank and applies
// the PSP-side rules (sampling mode, latches, buffers).
//
// The physical codes reuse SDL3's numbering, so a mapping file can name keys
// and buttons with SDL's own names: a Scancode is a USB HID keyboard usage
// (= SDL_Scancode), PadButton = SDL_GamepadButton, PadAxis = SDL_GamepadAxis.

#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

#include "ctrl.h"

namespace mpo::frontend {

using Scancode = std::uint16_t;
inline constexpr int kScancodeCount = 512; // SDL_SCANCODE_COUNT

namespace scancode {
// USB HID keyboard usages (SDL_SCANCODE_*), the ones the default map uses.
inline constexpr Scancode kA = 4;
inline constexpr Scancode kI = 12;
inline constexpr Scancode kJ = 13;
inline constexpr Scancode kK = 14;
inline constexpr Scancode kL = 15;
inline constexpr Scancode kQ = 20;
inline constexpr Scancode kS = 22;
inline constexpr Scancode kV = 25;
inline constexpr Scancode kW = 26;
inline constexpr Scancode kX = 27;
inline constexpr Scancode kZ = 29;
inline constexpr Scancode kReturn = 40;
inline constexpr Scancode kSpace = 44;
inline constexpr Scancode kRight = 79;
inline constexpr Scancode kLeft = 80;
inline constexpr Scancode kDown = 81;
inline constexpr Scancode kUp = 82;
} // namespace scancode

// SDL_GamepadButton values (positional names: south is A on Xbox, cross on
// PlayStation).
enum class PadButton : std::uint8_t {
    kSouth = 0,
    kEast = 1,
    kWest = 2,
    kNorth = 3,
    kBack = 4,
    kGuide = 5,
    kStart = 6,
    kLeftStick = 7,
    kRightStick = 8,
    kLeftShoulder = 9,
    kRightShoulder = 10,
    kDpadUp = 11,
    kDpadDown = 12,
    kDpadLeft = 13,
    kDpadRight = 14,
};
inline constexpr int kPadButtonCount = 26; // SDL_GAMEPAD_BUTTON_COUNT

// SDL_GamepadAxis values. Sticks go -1..1 with Y growing downwards; triggers
// go 0..1.
enum class PadAxis : std::uint8_t {
    kLeftX = 0,
    kLeftY = 1,
    kRightX = 2,
    kRightY = 3,
    kLeftTrigger = 4,
    kRightTrigger = 5,
};
inline constexpr int kPadAxisCount = 6; // SDL_GAMEPAD_AXIS_COUNT

// What a physical input drives: PSP buttons, or one direction of an analog
// stick axis (like PPSSPP's VIRTKEY_AXIS_{X,Y}_{MIN,MAX}). Directions follow
// the PSP data: +1 on Y is down.
struct Target {
    enum class Kind : std::uint8_t { kButton, kAnalog };
    Kind kind = Kind::kButton;
    std::uint32_t buttons = 0;
    std::uint8_t stick = 0;
    std::uint8_t axis = 0;
    std::int8_t direction = 0;

    static Target Button(std::uint32_t buttons) { return {Kind::kButton, buttons, 0, 0, 0}; }
    static Target Analog(int stick, int axis, int direction) {
        return {Kind::kAnalog, 0, static_cast<std::uint8_t>(stick), static_cast<std::uint8_t>(axis),
                static_cast<std::int8_t>(direction < 0 ? -1 : 1)};
    }
    friend bool operator==(const Target &, const Target &) = default;
};

struct KeyBinding {
    Scancode key = 0;
    Target target;
    friend bool operator==(const KeyBinding &, const KeyBinding &) = default;
};

struct PadButtonBinding {
    PadButton button = PadButton::kSouth;
    Target target;
    friend bool operator==(const PadButtonBinding &, const PadButtonBinding &) = default;
};

// One half of a gamepad axis: the part of the value with sign `sign` (+1 or
// -1), as a magnitude 0..1. Driving an analog target, the magnitude goes
// through; driving buttons, they are pressed while it is >= the threshold.
struct PadAxisBinding {
    PadAxis axis = PadAxis::kLeftX;
    std::int8_t sign = 1;
    Target target;
    friend bool operator==(const PadAxisBinding &, const PadAxisBinding &) = default;
};

// Ported from PPSSPP Core/ControlMapper.cpp (MapAxisValue, ConvertAnalogStick)
// and Core/Config.cpp defaults @ v1.20.4 (GPL-2.0-or-later).
struct AnalogConfig {
    float deadzone = 0.15f;              // fAnalogDeadzone
    float inverse_deadzone = 0.0f;       // fAnalogInverseDeadzone
    float sensitivity = 1.1f;            // fAnalogSensitivity
    bool circular = false;               // bAnalogIsCircular
    float axis_button_threshold = 0.75f; // fAnalogTriggerThreshold / fAnalogStickThreshold
    friend bool operator==(const AnalogConfig &, const AnalogConfig &) = default;
};

struct InputMapping {
    std::vector<KeyBinding> keys;
    std::vector<PadButtonBinding> pad_buttons;
    std::vector<PadAxisBinding> pad_axes;
    AnalogConfig analog;

    // PPSSPP's defaults: defaultQwertyKeyboardKeyMap for the keyboard and
    // defaultXInputKeyMap for gamepads, plus the triggers as L/R.
    static InputMapping Default();
    friend bool operator==(const InputMapping &, const InputMapping &) = default;
};

// PPSSPP's stick shaping: deadzone, inverse deadzone and sensitivity on the
// stick's norm (square or circular). Inputs and outputs are -1..1.
void ConvertAnalogStick(const AnalogConfig &config, float x, float y, float *out_x, float *out_y);
// __CtrlSetAnalogX/Y: -1..1 (Y down) -> 0..255, 1.0 -> 255, 0 -> 128.
[[nodiscard]] std::uint8_t AnalogToByte(float value);

class InputTranslator {
public:
    explicit InputTranslator(InputMapping mapping = InputMapping::Default());

    void SetMapping(InputMapping mapping);
    [[nodiscard]] const InputMapping &mapping() const { return mapping_; }

    // Key repeat must be filtered by the caller (repeats are not new presses).
    void OnKey(Scancode key, bool down);
    void OnPadButton(PadButton button, bool down);
    // Sticks: -1..1 (Y down), triggers: 0..1. Values are clamped.
    void OnPadAxis(PadAxis axis, float value);
    // Everything up and centred (focus lost, gamepad removed).
    void ReleaseKeyboard();
    void ReleasePad();

    // Current state; the timestamp is the caller's (sceCtrl uses microseconds).
    [[nodiscard]] SceCtrlData Sample(std::uint32_t timestamp = 0) const;

private:
    InputMapping mapping_;
    std::bitset<kScancodeCount> keys_down_;
    std::bitset<kPadButtonCount> pad_down_;
    std::array<float, kPadAxisCount> pad_axes_{};
};

} // namespace mpo::frontend
