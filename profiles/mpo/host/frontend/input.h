// SPDX-License-Identifier: GPL-3.0-or-later
// AnalogConfig and the default gamepad mapping are ported from PPSSPP @
// v1.20.4 (GPL-2.0-or-later): Core/Config.cpp, Core/ControlMapper.cpp and
// Core/KeyMapDefaults.cpp (see input.cpp).
//
// Host input -> PSP controller state. InputTranslator receives abstract
// events (keyboard scancodes, mouse buttons, gamepad buttons and axes), applies a
// configurable InputMapping and produces SceCtrlData. It does not know SDL:
// SdlInput (sdl_input.h) feeds it from SDL events, and the tests feed it
// directly. The sceCtrl HLE (F10b) samples it once per vblank and applies
// the PSP-side rules (sampling mode, latches, buffers).
//
// The physical codes reuse SDL3's numbering, so a mapping file can name keys
// and buttons with SDL's own names: a Scancode is a USB HID keyboard usage
// (= SDL_Scancode), MouseButton = SDL_BUTTON_*, PadButton = SDL_GamepadButton,
// PadAxis = SDL_GamepadAxis.

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
// USB HID keyboard usages (SDL_SCANCODE_*): the ones the default map and the
// tests use.
inline constexpr Scancode kA = 4;
inline constexpr Scancode kD = 7;
inline constexpr Scancode kE = 8;
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
inline constexpr Scancode kEscape = 41;
inline constexpr Scancode kTab = 43;
inline constexpr Scancode kSpace = 44;
inline constexpr Scancode kF1 = 58;
inline constexpr Scancode kF11 = 68;
inline constexpr Scancode kRight = 79;
inline constexpr Scancode kLeft = 80;
inline constexpr Scancode kDown = 81;
inline constexpr Scancode kUp = 82;
inline constexpr Scancode kKpEnter = 88;
inline constexpr Scancode kLeftCtrl = 224;
inline constexpr Scancode kRightCtrl = 228;
inline constexpr Scancode kLeftAlt = 226;
inline constexpr Scancode kRightAlt = 230;
} // namespace scancode

// SDL_BUTTON_* values (SDL numbers mouse buttons from 1).
enum class MouseButton : std::uint8_t {
    kLeft = 1,
    kMiddle = 2,
    kRight = 3,
    kX1 = 4,
    kX2 = 5,
};
inline constexpr int kMouseButtonCount = 6; // 0 is unused

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

struct MouseButtonBinding {
    MouseButton button = MouseButton::kLeft;
    Target target;
    friend bool operator==(const MouseButtonBinding &, const MouseButtonBinding &) = default;
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

// F22: the game's camera turns with the d-pad, so mouse motion can drive it.
// While the mouse is captured (SdlInput), the relative motion adds up and
// every `pixels_per_press` pixels press the d-pad for one poll of the window
// events (one flip): moving faster turns more often. Vertical motion drives
// up/down only when `vertical` is set.
struct MouseCameraConfig {
    bool enabled = true;
    float pixels_per_press = 6.0f;
    bool vertical = false;
    friend bool operator==(const MouseCameraConfig &, const MouseCameraConfig &) = default;
};

// F23a: the gamepad's right stick turns the camera (the d-pad) too. An axis
// engages when its raw value (-1..1, before [analog]'s deadzone) reaches
// `threshold` and lets go when it drops below `threshold - hysteresis`, so a
// stick resting near the threshold does not flicker. While engaged, the axis
// presses its d-pad direction on a share of the polls of the window events
// (one per flip) that grows with the tilt: `min_duty` at the threshold, every
// poll from kPadCameraFullTilt on. The share adds up per poll, so the presses
// depend only on the axis values at each poll. min_duty = 1 is all or
// nothing. X drives left/right; Y drives up/down only when `vertical` is set.
// An axis that the mapping binds in pad_axes is left to that binding.
struct PadCameraConfig {
    bool enabled = true;
    float threshold = 0.5f;
    float hysteresis = 0.15f;
    float min_duty = 0.25f;
    bool vertical = false;
    friend bool operator==(const PadCameraConfig &, const PadCameraConfig &) = default;
};
// The raw tilt from which the right stick presses on every poll.
inline constexpr float kPadCameraFullTilt = 0.9f;
// Share of the polls (0..1] an engaged camera axis presses at `magnitude`
// (the raw tilt, 0..1).
[[nodiscard]] float PadCameraDuty(const PadCameraConfig &config, float magnitude);

struct InputMapping {
    std::vector<KeyBinding> keys;
    std::vector<MouseButtonBinding> mouse_buttons;
    std::vector<PadButtonBinding> pad_buttons;
    std::vector<PadAxisBinding> pad_axes;
    AnalogConfig analog;
    MouseCameraConfig mouse_camera;
    PadCameraConfig pad_camera;

    // Keyboard and mouse: the PC layout chosen in F22 (WASD for the stick,
    // the arrows for the d-pad, Space = cross, mouse right/left = circle/square,
    // Ctrl = triangle, Q/E = L/R, Esc or Enter = start, Tab = select).
    // Gamepads: PPSSPP's defaultXInputKeyMap, plus the triggers as L/R.
    static InputMapping Default();
    friend bool operator==(const InputMapping &, const InputMapping &) = default;
};

// PPSSPP's stick shaping: deadzone, inverse deadzone and sensitivity on the
// stick's norm (square or circular). Inputs and outputs are -1..1.
void ConvertAnalogStick(const AnalogConfig &config, float x, float y, float *out_x, float *out_y);
// __CtrlSetAnalogX/Y: -1..1 (Y down) -> 0..255, 1.0 -> 255, 0 -> 128.
[[nodiscard]] std::uint8_t AnalogToByte(float value);

// F25: the window's own shortcuts. F11, or Alt+Enter (either Enter key),
// toggles fullscreen. They belong to the window, not to the game: the
// frontend takes them before the OSK and the mapping see the key, so Alt+Enter
// never presses Start (Enter) and whatever input.toml binds to F11 does not
// fire. Alt alone, and Enter without Alt, still go to the game.
enum class WindowShortcut : std::uint8_t { kNone, kToggleFullscreen };
[[nodiscard]] WindowShortcut WindowShortcutFor(Scancode key, bool alt_held);

// Filters the shortcuts out of the key events. A shortcut's press is taken,
// and so are its key's repeats and release: the game sees nothing of it. A key
// that was already held when Alt went down (Enter as Start) keeps going to the
// game, repeats and release included.
class WindowShortcutFilter {
public:
    // True when the event belongs to a shortcut and must not reach the game.
    // `*shortcut` is the action to run (only on the press; kNone otherwise).
    bool OnKey(Scancode key, bool down, bool repeat, bool alt_held, WindowShortcut *shortcut);
    // The releases of held shortcut keys never arrive (the window lost the
    // focus): forget them.
    void Reset() { held_.reset(); }

private:
    std::bitset<kScancodeCount> held_;
};

class InputTranslator {
public:
    explicit InputTranslator(InputMapping mapping = InputMapping::Default());

    void SetMapping(InputMapping mapping);
    [[nodiscard]] const InputMapping &mapping() const { return mapping_; }

    // Key repeat must be filtered by the caller (repeats are not new presses).
    void OnKey(Scancode key, bool down);
    void OnMouseButton(MouseButton button, bool down);
    // Relative mouse motion in pixels (x right, y down), for the camera.
    void OnMouseMotion(float dx, float dy);
    // End of one poll of the window events: turns the mouse motion since the
    // last poll and the right stick (PadCameraConfig) into d-pad presses that
    // last until the next poll.
    void EndPoll();
    void OnPadButton(PadButton button, bool down);
    // Sticks: -1..1 (Y down), triggers: 0..1. Values are clamped.
    void OnPadAxis(PadAxis axis, float value);
    // Everything up and centred (focus lost, gamepad removed). Focus loss
    // releases the keyboard and the mouse.
    void ReleaseKeyboard();
    void ReleaseMouse();
    // Also releases the right-stick camera.
    void ReleasePad();
    // While paused (the window lost the focus), the right stick does not turn
    // the camera; pausing lets its presses go and forgets its state. The
    // other gamepad inputs keep working (background gamepad events).
    void SetPadCameraPaused(bool paused);
    [[nodiscard]] bool pad_camera_paused() const { return pad_camera_paused_; }

    // Current state; the timestamp is the caller's (sceCtrl uses microseconds).
    [[nodiscard]] SceCtrlData Sample(std::uint32_t timestamp = 0) const;

private:
    // One right-stick axis of the camera: the engaged direction (0: not
    // engaged) and the share of a press carried to the next poll.
    struct PadCameraAxis {
        std::int8_t direction = 0;
        float phase = 0.0f;
    };

    void EndPollMouseCamera();
    void EndPollPadCamera();
    void ReleasePadCamera();

    InputMapping mapping_;
    std::bitset<kScancodeCount> keys_down_;
    std::bitset<kMouseButtonCount> mouse_down_;
    float camera_motion_[2]{};
    bool camera_moved_ = false;
    std::uint32_t camera_buttons_ = 0;
    std::array<PadCameraAxis, 2> pad_camera_{};
    std::uint32_t pad_camera_buttons_ = 0;
    bool pad_camera_paused_ = false;
    std::bitset<kPadButtonCount> pad_down_;
    std::array<float, kPadAxisCount> pad_axes_{};
};

} // namespace mpo::frontend
