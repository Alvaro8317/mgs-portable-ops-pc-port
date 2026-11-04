// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdl_input.h"

#include <algorithm>
#include <cstdio>

namespace mpo::frontend {

// The abstract codes in input.h are SDL3's own numbers.
static_assert(kScancodeCount == SDL_SCANCODE_COUNT);
static_assert(scancode::kA == SDL_SCANCODE_A && scancode::kZ == SDL_SCANCODE_Z);
static_assert(scancode::kI == SDL_SCANCODE_I && scancode::kL == SDL_SCANCODE_L);
static_assert(scancode::kReturn == SDL_SCANCODE_RETURN && scancode::kSpace == SDL_SCANCODE_SPACE);
static_assert(scancode::kUp == SDL_SCANCODE_UP && scancode::kRight == SDL_SCANCODE_RIGHT);
static_assert(scancode::kD == SDL_SCANCODE_D && scancode::kE == SDL_SCANCODE_E);
static_assert(scancode::kEscape == SDL_SCANCODE_ESCAPE && scancode::kTab == SDL_SCANCODE_TAB);
static_assert(scancode::kLeftCtrl == SDL_SCANCODE_LCTRL &&
              scancode::kRightCtrl == SDL_SCANCODE_RCTRL);
static_assert(scancode::kLeftAlt == SDL_SCANCODE_LALT && scancode::kRightAlt == SDL_SCANCODE_RALT);
static_assert(scancode::kF1 == SDL_SCANCODE_F1 && scancode::kF11 == SDL_SCANCODE_F11);
static_assert(scancode::kKpEnter == SDL_SCANCODE_KP_ENTER);
static_assert(static_cast<int>(MouseButton::kLeft) == SDL_BUTTON_LEFT);
static_assert(static_cast<int>(MouseButton::kMiddle) == SDL_BUTTON_MIDDLE);
static_assert(static_cast<int>(MouseButton::kRight) == SDL_BUTTON_RIGHT);
static_assert(static_cast<int>(MouseButton::kX2) == SDL_BUTTON_X2);
static_assert(kPadButtonCount == SDL_GAMEPAD_BUTTON_COUNT);
static_assert(static_cast<int>(PadButton::kSouth) == SDL_GAMEPAD_BUTTON_SOUTH);
static_assert(static_cast<int>(PadButton::kBack) == SDL_GAMEPAD_BUTTON_BACK);
static_assert(static_cast<int>(PadButton::kLeftShoulder) == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
static_assert(static_cast<int>(PadButton::kDpadRight) == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert(kPadAxisCount == SDL_GAMEPAD_AXIS_COUNT);
static_assert(static_cast<int>(PadAxis::kLeftY) == SDL_GAMEPAD_AXIS_LEFTY);
static_assert(static_cast<int>(PadAxis::kRightTrigger) == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
static_assert(kGamepadTypeCount == SDL_GAMEPAD_TYPE_COUNT);
static_assert(static_cast<int>(GamepadType::kXboxOne) == SDL_GAMEPAD_TYPE_XBOXONE);
static_assert(static_cast<int>(GamepadType::kPs5) == SDL_GAMEPAD_TYPE_PS5);
static_assert(static_cast<int>(GamepadType::kJoyconPair) ==
              SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR);
static_assert(static_cast<int>(GamepadType::kSteam) == SDL_GAMEPAD_TYPE_STEAM);
static_assert(static_cast<int>(FaceLabel::kA) == SDL_GAMEPAD_BUTTON_LABEL_A);
static_assert(static_cast<int>(FaceLabel::kY) == SDL_GAMEPAD_BUTTON_LABEL_Y);
static_assert(static_cast<int>(FaceLabel::kCross) == SDL_GAMEPAD_BUTTON_LABEL_CROSS);
static_assert(static_cast<int>(FaceLabel::kTriangle) == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE);

const char *MouseButtonName(int button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return "left";
    case SDL_BUTTON_MIDDLE:
        return "middle";
    case SDL_BUTTON_RIGHT:
        return "right";
    case SDL_BUTTON_X1:
        return "x1";
    case SDL_BUTTON_X2:
        return "x2";
    default:
        return "?";
    }
}

int MouseButtonFromName(std::string_view name) {
    for (int button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; ++button) {
        if (name == MouseButtonName(button)) return button;
    }
    return 0;
}

std::string LayoutKeyName(Scancode key) {
    // Letters, digits and punctuation (SDL_SCANCODE_A..SLASH), not Return,
    // Escape, Backspace, Tab or Space (40-44).
    if (key < SDL_SCANCODE_A || key > SDL_SCANCODE_SLASH ||
        (key >= SDL_SCANCODE_RETURN && key <= SDL_SCANCODE_SPACE))
        return {};
    const SDL_Keycode code =
        SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(key), SDL_KMOD_NONE, false);
    if (code == SDLK_UNKNOWN) return {};
    const char *name = SDL_GetKeyName(code);
    return name != nullptr ? std::string(name) : std::string();
}

GamepadType GamepadTypeOf(SDL_Gamepad *pad) {
    const SDL_GamepadType type = SDL_GetGamepadType(pad);
    if (type < 0 || type >= SDL_GAMEPAD_TYPE_COUNT) return GamepadType::kUnknown;
    return static_cast<GamepadType>(type);
}

std::array<FaceLabel, 4> FaceLabelsOf(SDL_Gamepad *pad) {
    std::array<FaceLabel, 4> face{};
    for (int i = 0; i < 4; ++i) {
        const SDL_GamepadButtonLabel label = SDL_GetGamepadButtonLabel(
            pad, static_cast<SDL_GamepadButton>(SDL_GAMEPAD_BUTTON_SOUTH + i));
        if (label > SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN && label <= SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE)
            face[static_cast<std::size_t>(i)] = static_cast<FaceLabel>(label);
    }
    return face;
}

float AxisValueFromSdl(Sint16 value) {
    if (value < 0) return static_cast<float>(value) / 32768.0f;
    return static_cast<float>(value) / 32767.0f;
}

SdlInput::SdlInput(InputTranslator *translator) : translator_(translator) {
    initialised_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!initialised_) error_ = std::string("SDL_InitSubSystem(GAMEPAD): ") + SDL_GetError();
}

SdlInput::~SdlInput() {
    ReleaseMouseCapture();
    for (SDL_Gamepad *pad : gamepads_) SDL_CloseGamepad(pad);
    if (initialised_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void SdlInput::Note(const std::string &text) const {
    if (observer_) observer_(text);
}

void SdlInput::ReleaseMouseCapture() {
    if (!captured_) return;
    captured_ = false;
    if (SDL_Window *window = SDL_GetWindowFromID(captured_window_)) {
        (void)SDL_SetWindowRelativeMouseMode(window, false);
        if (!saved_title_.empty()) (void)SDL_SetWindowTitle(window, saved_title_.c_str());
    }
    captured_window_ = 0;
    saved_title_.clear();
    translator_->ReleaseMouse();
    Note("mouse released");
}

bool SdlInput::HandleWindowShortcut(const SDL_Event &event) {
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) return false;
    const bool down = event.type == SDL_EVENT_KEY_DOWN;
    WindowShortcut shortcut = WindowShortcut::kNone;
    if (!shortcuts_.OnKey(static_cast<Scancode>(event.key.scancode), down, event.key.repeat,
                          (event.key.mod & SDL_KMOD_ALT) != 0, &shortcut))
        return false;
    if (shortcut == WindowShortcut::kToggleFullscreen) {
        SDL_Window *window = SDL_GetWindowFromID(event.key.windowID);
        if (window == nullptr) {
            Note("fullscreen toggle: no window");
            return true;
        }
        const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0;
        // No display mode: borderless at the desktop's resolution (F24a). The
        // swapchain follows with SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED.
        if (SDL_SetWindowFullscreen(window, fullscreen)) {
            ++fullscreen_toggles_;
            Note(fullscreen ? "fullscreen on" : "fullscreen off");
        } else {
            Note(std::string("fullscreen toggle failed: ") + SDL_GetError());
        }
    }
    return true;
}

bool SdlInput::HandleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (HandleWindowShortcut(event)) return true;
        if (event.key.repeat) return true;
        if (down) devices_.OnKeyboardInput();
        if (down && captured_ && event.key.scancode == SDL_SCANCODE_F1) {
            ReleaseMouseCapture();
            return true;
        }
        translator_->OnKey(static_cast<Scancode>(event.key.scancode), down);
        if (observer_) {
            const char *name = SDL_GetScancodeName(event.key.scancode);
            Note(std::string("key ") + (name && *name ? name : "?") + (down ? " down" : " up"));
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        // Taps on a touch screen (Steam Deck) also arrive as mouse clicks;
        // they are not the player's mouse.
        if (event.button.which == SDL_TOUCH_MOUSEID) return false;
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (down) devices_.OnKeyboardInput();
        if (down && !captured_ && translator_->mapping().mouse_camera.enabled) {
            // This click only captures the mouse.
            captured_ = true;
            captured_window_ = event.button.windowID;
            if (SDL_Window *window = SDL_GetWindowFromID(captured_window_)) {
                (void)SDL_SetWindowRelativeMouseMode(window, true);
                const char *title = SDL_GetWindowTitle(window);
                saved_title_ = title ? title : "";
                const std::string captured_title = saved_title_ + " (F1 releases the mouse)";
                (void)SDL_SetWindowTitle(window, captured_title.c_str());
            }
            Note("mouse captured");
            return true;
        }
        if (event.button.button < kMouseButtonCount) {
            translator_->OnMouseButton(static_cast<MouseButton>(event.button.button), down);
        }
        if (observer_) {
            Note("mouse " + std::string(MouseButtonName(event.button.button)) +
                 (down ? " down" : " up"));
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!captured_ || event.motion.which == SDL_TOUCH_MOUSEID) return false;
        translator_->OnMouseMotion(event.motion.xrel, event.motion.yrel);
        return true;
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *pad = SDL_OpenGamepad(event.gdevice.which);
        if (pad == nullptr) {
            Note(std::string("gamepad open failed: ") + SDL_GetError());
            return true;
        }
        gamepads_.push_back(pad);
        devices_.OnGamepadAdded(event.gdevice.which, GamepadTypeOf(pad), FaceLabelsOf(pad));
        const char *name = SDL_GetGamepadName(pad);
        Note(std::string("gamepad added: ") + (name ? name : "?"));
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const auto it = std::find_if(gamepads_.begin(), gamepads_.end(), [&](SDL_Gamepad *pad) {
            return SDL_GetGamepadID(pad) == event.gdevice.which;
        });
        if (it != gamepads_.end()) {
            SDL_CloseGamepad(*it);
            gamepads_.erase(it);
        }
        devices_.OnGamepadRemoved(event.gdevice.which);
        // The removed pad may have been holding buttons or a stick.
        translator_->ReleasePad();
        Note("gamepad removed");
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (down) devices_.OnGamepadButton(event.gbutton.which);
        if (event.gbutton.button < kPadButtonCount) {
            translator_->OnPadButton(static_cast<PadButton>(event.gbutton.button), down);
        }
        if (observer_) {
            const char *name =
                SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event.gbutton.button));
            Note(std::string("pad ") + (name ? name : "?") + (down ? " down" : " up"));
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const float value = AxisValueFromSdl(event.gaxis.value);
        devices_.OnGamepadAxis(event.gaxis.which, value,
                               std::max(translator_->mapping().analog.deadzone, kMinAxisActivity));
        if (event.gaxis.axis < kPadAxisCount) {
            translator_->OnPadAxis(static_cast<PadAxis>(event.gaxis.axis), value);
        }
        if (observer_) {
            const char *name =
                SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(event.gaxis.axis));
            char text[64];
            std::snprintf(text, sizeof(text), "pad axis %s %+.3f", name ? name : "?",
                          static_cast<double>(value));
            Note(text);
        }
        return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Keys and buttons released while unfocused never arrive.
        translator_->ReleaseKeyboard();
        translator_->ReleaseMouse();
        ReleaseMouseCapture();
        shortcuts_.Reset();
        // Gamepads keep working unfocused, but the right stick stops turning
        // the camera until the window is back (F23a).
        translator_->SetPadCameraPaused(true);
        Note("focus lost");
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        translator_->SetPadCameraPaused(false);
        Note("focus gained");
        return true;
    default:
        return false;
    }
}

} // namespace mpo::frontend
