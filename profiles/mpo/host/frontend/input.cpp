// SPDX-License-Identifier: GPL-3.0-or-later
// Partly ported from PPSSPP @ v1.20.4 (GPL-2.0-or-later):
// - Core/ControlMapper.cpp: MapAxisValue, ConvertAnalogStick and the way
//   virtual axis keys add up (UpdatePSPState: sum, clamp to 0..1, max - min);
// - Core/HLE/sceCtrl.cpp: __CtrlSetAnalogX/Y scaling;
// - Core/KeyMapDefaults.cpp: defaultQwertyKeyboardKeyMap, defaultXInputKeyMap.

#include "input.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mpo::frontend {
namespace {

float MapAxisValue(const AnalogConfig &config, float v) {
    const float sign = v >= 0.0f ? 1.0f : -1.0f;
    // Deadzone.
    v = std::clamp((std::fabs(v) - config.deadzone) / (1.0f - config.deadzone), 0.0f, 1.0f);
    // Sensitivity and inverse deadzone.
    if (v != 0.0f) {
        v = std::clamp(config.inverse_deadzone + v * (config.sensitivity - config.inverse_deadzone),
                       0.0f, 1.0f);
    }
    return sign * v;
}

} // namespace

void ConvertAnalogStick(const AnalogConfig &config, float x, float y, float *out_x, float *out_y) {
    float norm = std::max(std::fabs(x), std::fabs(y));
    if (norm == 0.0f) {
        *out_x = x;
        *out_y = y;
        return;
    }
    if (config.circular) {
        const float new_norm = std::sqrt(x * x + y * y);
        const float factor = new_norm / norm;
        x *= factor;
        y *= factor;
        norm = new_norm;
    }
    const float mapped_norm = MapAxisValue(config, norm);
    *out_x = std::clamp(x / norm * mapped_norm, -1.0f, 1.0f);
    *out_y = std::clamp(y / norm * mapped_norm, -1.0f, 1.0f);
}

std::uint8_t AnalogToByte(float value) {
    const int scaled = static_cast<int>(std::ceil(value * 127.5f + 127.5f));
    return static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
}

InputMapping InputMapping::Default() {
    using ctrl::kAxisX;
    using ctrl::kAxisY;
    using ctrl::kStickLeft;
    InputMapping m;
    // defaultQwertyKeyboardKeyMap. PPSSPP's I/K are AXIS_Y_MAX/MIN with Y up;
    // here Y grows downwards, so I is -1.
    m.keys = {
        {scancode::kA, Target::Button(ctrl::kSquare)},
        {scancode::kS, Target::Button(ctrl::kTriangle)},
        {scancode::kX, Target::Button(ctrl::kCircle)},
        {scancode::kZ, Target::Button(ctrl::kCross)},
        {scancode::kQ, Target::Button(ctrl::kLTrigger)},
        {scancode::kW, Target::Button(ctrl::kRTrigger)},
        {scancode::kSpace, Target::Button(ctrl::kStart)},
        {scancode::kV, Target::Button(ctrl::kSelect)},
        {scancode::kReturn, Target::Button(ctrl::kSelect)},
        {scancode::kUp, Target::Button(ctrl::kUp)},
        {scancode::kDown, Target::Button(ctrl::kDown)},
        {scancode::kLeft, Target::Button(ctrl::kLeft)},
        {scancode::kRight, Target::Button(ctrl::kRight)},
        {scancode::kI, Target::Analog(kStickLeft, kAxisY, -1)},
        {scancode::kK, Target::Analog(kStickLeft, kAxisY, +1)},
        {scancode::kJ, Target::Analog(kStickLeft, kAxisX, -1)},
        {scancode::kL, Target::Analog(kStickLeft, kAxisX, +1)},
    };
    // defaultXInputKeyMap (positional: south = A/cross).
    m.pad_buttons = {
        {PadButton::kSouth, Target::Button(ctrl::kCross)},
        {PadButton::kEast, Target::Button(ctrl::kCircle)},
        {PadButton::kWest, Target::Button(ctrl::kSquare)},
        {PadButton::kNorth, Target::Button(ctrl::kTriangle)},
        {PadButton::kDpadUp, Target::Button(ctrl::kUp)},
        {PadButton::kDpadDown, Target::Button(ctrl::kDown)},
        {PadButton::kDpadLeft, Target::Button(ctrl::kLeft)},
        {PadButton::kDpadRight, Target::Button(ctrl::kRight)},
        {PadButton::kStart, Target::Button(ctrl::kStart)},
        {PadButton::kBack, Target::Button(ctrl::kSelect)},
        {PadButton::kLeftShoulder, Target::Button(ctrl::kLTrigger)},
        {PadButton::kRightShoulder, Target::Button(ctrl::kRTrigger)},
    };
    m.pad_axes = {
        {PadAxis::kLeftX, -1, Target::Analog(kStickLeft, kAxisX, -1)},
        {PadAxis::kLeftX, +1, Target::Analog(kStickLeft, kAxisX, +1)},
        {PadAxis::kLeftY, -1, Target::Analog(kStickLeft, kAxisY, -1)},
        {PadAxis::kLeftY, +1, Target::Analog(kStickLeft, kAxisY, +1)},
        // Not in PPSSPP's map (it uses them for pause and fast-forward).
        {PadAxis::kLeftTrigger, +1, Target::Button(ctrl::kLTrigger)},
        {PadAxis::kRightTrigger, +1, Target::Button(ctrl::kRTrigger)},
    };
    return m;
}

InputTranslator::InputTranslator(InputMapping mapping) : mapping_(std::move(mapping)) {}

void InputTranslator::SetMapping(InputMapping mapping) {
    mapping_ = std::move(mapping);
}

void InputTranslator::OnKey(Scancode key, bool down) {
    if (key < kScancodeCount) keys_down_.set(key, down);
}

void InputTranslator::OnPadButton(PadButton button, bool down) {
    const auto index = static_cast<std::size_t>(button);
    if (index < pad_down_.size()) pad_down_.set(index, down);
}

void InputTranslator::OnPadAxis(PadAxis axis, float value) {
    const auto index = static_cast<std::size_t>(axis);
    if (index >= pad_axes_.size()) return;
    if (std::isnan(value)) value = 0.0f;
    pad_axes_[index] = std::clamp(value, -1.0f, 1.0f);
}

void InputTranslator::ReleaseKeyboard() {
    keys_down_.reset();
}

void InputTranslator::ReleasePad() {
    pad_down_.reset();
    pad_axes_.fill(0.0f);
}

SceCtrlData InputTranslator::Sample(std::uint32_t timestamp) const {
    std::uint32_t buttons = 0;
    // half[stick][axis][0: negative, 1: positive], summed like PPSSPP's
    // virtual keys and clamped to 0..1 afterwards.
    float half[2][2][2] = {};
    const auto apply = [&](const Target &target, float magnitude, bool from_axis) {
        if (magnitude <= 0.0f) return;
        if (target.kind == Target::Kind::kButton) {
            if (!from_axis || magnitude >= mapping_.analog.axis_button_threshold) {
                buttons |= target.buttons;
            }
            return;
        }
        if (target.stick > 1 || target.axis > 1) return;
        half[target.stick][target.axis][target.direction > 0 ? 1 : 0] += magnitude;
    };

    for (const KeyBinding &b : mapping_.keys) {
        if (b.key < kScancodeCount && keys_down_.test(b.key)) apply(b.target, 1.0f, false);
    }
    for (const PadButtonBinding &b : mapping_.pad_buttons) {
        const auto index = static_cast<std::size_t>(b.button);
        if (index < pad_down_.size() && pad_down_.test(index)) apply(b.target, 1.0f, false);
    }
    for (const PadAxisBinding &b : mapping_.pad_axes) {
        const auto index = static_cast<std::size_t>(b.axis);
        if (index >= pad_axes_.size()) continue;
        apply(b.target, pad_axes_[index] * static_cast<float>(b.sign), true);
    }

    SceCtrlData data;
    data.timestamp = timestamp;
    data.buttons = buttons & ctrl::kMaskUser;
    for (int stick = 0; stick < 2; ++stick) {
        float v[2];
        for (int axis = 0; axis < 2; ++axis) {
            v[axis] = std::clamp(half[stick][axis][1], 0.0f, 1.0f) -
                      std::clamp(half[stick][axis][0], 0.0f, 1.0f);
        }
        float x = 0.0f;
        float y = 0.0f;
        ConvertAnalogStick(mapping_.analog, v[0], v[1], &x, &y);
        data.analog[stick][ctrl::kAxisX] = AnalogToByte(x);
        data.analog[stick][ctrl::kAxisY] = AnalogToByte(y);
    }
    return data;
}

} // namespace mpo::frontend
