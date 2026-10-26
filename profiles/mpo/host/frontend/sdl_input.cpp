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
static_assert(kPadButtonCount == SDL_GAMEPAD_BUTTON_COUNT);
static_assert(static_cast<int>(PadButton::kSouth) == SDL_GAMEPAD_BUTTON_SOUTH);
static_assert(static_cast<int>(PadButton::kBack) == SDL_GAMEPAD_BUTTON_BACK);
static_assert(static_cast<int>(PadButton::kLeftShoulder) == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
static_assert(static_cast<int>(PadButton::kDpadRight) == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert(kPadAxisCount == SDL_GAMEPAD_AXIS_COUNT);
static_assert(static_cast<int>(PadAxis::kLeftY) == SDL_GAMEPAD_AXIS_LEFTY);
static_assert(static_cast<int>(PadAxis::kRightTrigger) == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

float AxisValueFromSdl(Sint16 value) {
    if (value < 0) return static_cast<float>(value) / 32768.0f;
    return static_cast<float>(value) / 32767.0f;
}

SdlInput::SdlInput(InputTranslator *translator) : translator_(translator) {
    initialised_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!initialised_) error_ = std::string("SDL_InitSubSystem(GAMEPAD): ") + SDL_GetError();
}

SdlInput::~SdlInput() {
    for (SDL_Gamepad *pad : gamepads_) SDL_CloseGamepad(pad);
    if (initialised_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void SdlInput::Note(const std::string &text) const {
    if (observer_) observer_(text);
}

bool SdlInput::HandleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (event.key.repeat) return true;
        translator_->OnKey(static_cast<Scancode>(event.key.scancode), down);
        if (observer_) {
            const char *name = SDL_GetScancodeName(event.key.scancode);
            Note(std::string("key ") + (name && *name ? name : "?") + (down ? " down" : " up"));
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *pad = SDL_OpenGamepad(event.gdevice.which);
        if (pad == nullptr) {
            Note(std::string("gamepad open failed: ") + SDL_GetError());
            return true;
        }
        gamepads_.push_back(pad);
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
        // The removed pad may have been holding buttons or a stick.
        translator_->ReleasePad();
        Note("gamepad removed");
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
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
        // Keys released while unfocused never arrive.
        translator_->ReleaseKeyboard();
        Note("focus lost");
        return true;
    default:
        return false;
    }
}

} // namespace mpo::frontend
