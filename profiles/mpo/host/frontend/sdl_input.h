// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL3 events -> InputTranslator. Keyboard keys are bound by scancode (the
// physical position, so the default map works on AZERTY/QWERTZ too), and
// every connected gamepad feeds the same PSP controller: buttons add up, and
// for each axis the last event wins.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "input.h"

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

    // Feeds one event. Returns true when it was keyboard, gamepad or focus
    // input (consumed here); other events are left to the caller.
    bool HandleEvent(const SDL_Event &event);

    [[nodiscard]] int gamepad_count() const { return static_cast<int>(gamepads_.size()); }

    // Called with a short description ("key Z down", "pad a down",
    // "pad axis leftx +0.500", "gamepad added: <name>") of each event it uses.
    using Observer = std::function<void(std::string_view)>;
    void SetObserver(Observer observer) { observer_ = std::move(observer); }

private:
    void Note(const std::string &text) const;

    InputTranslator *translator_;
    bool initialised_ = false;
    std::string error_;
    std::vector<SDL_Gamepad *> gamepads_;
    Observer observer_;
};

// SDL's 16-bit axis value to -1..1 (like PPSSPP: -32768 -> -1, 32767 -> 1).
[[nodiscard]] float AxisValueFromSdl(Sint16 value);

} // namespace mpo::frontend
