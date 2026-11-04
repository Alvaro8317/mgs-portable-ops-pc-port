// SPDX-License-Identifier: GPL-3.0-or-later
// FaceLabelForType follows SDL3 @ 3.4.18 (zlib license): src/joystick/
// SDL_gamepad.c, SDL_GetGamepadFaceStyleForGamepadType and
// SDL_GetGamepadButtonLabelForFaceStyle.

#include "prompts.h"

#include <algorithm>
#include <cmath>

#include "input_recording.h"

namespace mpo::frontend {

namespace {

using Kind = PromptLabel::Kind;

PromptLabel Symbol(std::string text) {
    return {Kind::kSymbol, std::move(text)};
}
PromptLabel Word(std::string text) {
    return {Kind::kWord, std::move(text)};
}
PromptLabel Key(std::string text) {
    return {Kind::kKey, std::move(text)};
}

// SDL_GetGamepadFaceStyleForGamepadType.
enum class FaceStyle : std::uint8_t { kAbxy, kAxby, kBayx, kSony };

FaceStyle FaceStyleForType(GamepadType type) {
    switch (type) {
    case GamepadType::kPs3:
    case GamepadType::kPs4:
    case GamepadType::kPs5:
        return FaceStyle::kSony;
    case GamepadType::kSwitchPro:
    case GamepadType::kJoyconLeft:
    case GamepadType::kJoyconRight:
    case GamepadType::kJoyconPair:
        return FaceStyle::kBayx;
    case GamepadType::kGameCube:
        return FaceStyle::kAxby;
    default:
        return FaceStyle::kAbxy;
    }
}

// The pad buttons and axes named per family of gamepads.
struct PadNames {
    const char *start;
    const char *back;
    const char *guide;
    const char *left_shoulder;
    const char *right_shoulder;
    const char *left_trigger;
    const char *right_trigger;
    const char *left_stick;
    const char *right_stick;
};

const PadNames &NamesForType(GamepadType type) {
    static constexpr PadNames kGeneric = {"Start", "Back", "Guide", "LB", "RB",
                                          "LT",    "RT",   "LS",    "RS"};
    static constexpr PadNames kXbox360 = kGeneric;
    static constexpr PadNames kXboxOne = {"Menu", "View", "Guide", "LB", "RB",
                                          "LT",   "RT",   "LS",    "RS"};
    static constexpr PadNames kPs3 = {"Start", "Select", "PS", "L1", "R1", "L2", "R2", "L3", "R3"};
    static constexpr PadNames kPs4 = {"Options", "Share", "PS", "L1", "R1", "L2", "R2", "L3", "R3"};
    static constexpr PadNames kPs5 = {"Options", "Create", "PS", "L1", "R1",
                                      "L2",      "R2",     "L3", "R3"};
    // ASCII minus: the fallback font has no U+2212.
    static constexpr PadNames kSwitch = {"+", "-", "Home", "L", "R", "ZL", "ZR", "LS", "RS"};
    static constexpr PadNames kGameCube = {"Start", "Back", "Guide", "L", "Z",
                                           "L",     "R",    "LS",    "RS"};
    static constexpr PadNames kSteam = {"Menu", "View", "Steam", "L1", "R1",
                                        "L2",   "R2",   "L3",    "R3"};
    switch (type) {
    case GamepadType::kXbox360:
        return kXbox360;
    case GamepadType::kXboxOne:
        return kXboxOne;
    case GamepadType::kPs3:
        return kPs3;
    case GamepadType::kPs4:
        return kPs4;
    case GamepadType::kPs5:
        return kPs5;
    case GamepadType::kSwitchPro:
    case GamepadType::kJoyconLeft:
    case GamepadType::kJoyconRight:
    case GamepadType::kJoyconPair:
        return kSwitch;
    case GamepadType::kGameCube:
        return kGameCube;
    case GamepadType::kSteam:
        return kSteam;
    default:
        return kGeneric;
    }
}

// The gamepad a fixed --prompts style stands for.
GamepadType TypicalPad(PromptStyle style) {
    switch (style) {
    case PromptStyle::kXbox:
        return GamepadType::kXboxOne;
    case PromptStyle::kPlayStation:
        return GamepadType::kPs4;
    case PromptStyle::kNintendo:
        return GamepadType::kSwitchPro;
    default:
        return GamepadType::kStandard;
    }
}

PromptLabel FacePrompt(FaceLabel label) {
    switch (label) {
    case FaceLabel::kA:
        return Key("A");
    case FaceLabel::kB:
        return Key("B");
    case FaceLabel::kX:
        return Key("X");
    case FaceLabel::kY:
        return Key("Y");
    case FaceLabel::kCross:
        return Symbol("×");
    case FaceLabel::kCircle:
        return Symbol("○");
    case FaceLabel::kSquare:
        return Symbol("□");
    case FaceLabel::kTriangle:
        return Symbol("△");
    case FaceLabel::kUnknown:
        break;
    }
    return Key("");
}

PromptLabel PadButtonPrompt(PadButton button, GamepadType type,
                            const std::array<FaceLabel, 4> &face) {
    const PadNames &names = NamesForType(type);
    switch (button) {
    case PadButton::kSouth:
    case PadButton::kEast:
    case PadButton::kWest:
    case PadButton::kNorth: {
        FaceLabel label = face[static_cast<std::size_t>(button)];
        if (label == FaceLabel::kUnknown) label = FaceLabelForType(type, button);
        return FacePrompt(label);
    }
    case PadButton::kBack:
        return Key(names.back);
    case PadButton::kGuide:
        return Key(names.guide);
    case PadButton::kStart:
        return Key(names.start);
    case PadButton::kLeftStick:
        return Key(names.left_stick);
    case PadButton::kRightStick:
        return Key(names.right_stick);
    case PadButton::kLeftShoulder:
        return Key(names.left_shoulder);
    case PadButton::kRightShoulder:
        return Key(names.right_shoulder);
    case PadButton::kDpadUp:
        return Key("Up");
    case PadButton::kDpadDown:
        return Key("Down");
    case PadButton::kDpadLeft:
        return Key("Left");
    case PadButton::kDpadRight:
        return Key("Right");
    }
    // SDL_GAMEPAD_BUTTON_MISC1 (15), the paddles (16-19), the touchpad (20)
    // and MISC2-6 (21-25).
    const int index = static_cast<int>(button);
    if (index == 15) return Key("Misc");
    if (index >= 16 && index <= 19) return Key("P" + std::to_string(index - 15));
    if (index == 20) return Key("Touchpad");
    return Key("Misc " + std::to_string(index - 19));
}

PromptLabel PadAxisPrompt(PadAxis axis, GamepadType type) {
    const PadNames &names = NamesForType(type);
    switch (axis) {
    case PadAxis::kLeftTrigger:
        return Key(names.left_trigger);
    case PadAxis::kRightTrigger:
        return Key(names.right_trigger);
    case PadAxis::kLeftX:
    case PadAxis::kLeftY:
        return Key("Left stick");
    case PadAxis::kRightX:
    case PadAxis::kRightY:
        return Key("Right stick");
    }
    return Key("");
}

// The first binding of `bindings` that presses exactly `mask`, else the first
// that presses it with other buttons.
template <typename Binding>
const Binding *FindBinding(const std::vector<Binding> &bindings, std::uint32_t mask) {
    const Binding *partial = nullptr;
    for (const Binding &binding : bindings) {
        if (binding.target.kind != Target::Kind::kButton) continue;
        if (binding.target.buttons == mask) return &binding;
        if (partial == nullptr && (binding.target.buttons & mask) != 0u) partial = &binding;
    }
    return partial;
}

// A key name the dialogs can draw: one Latin-1 character (upper case) or
// printable ASCII text. Anything else falls back to the built-in names.
bool Drawable(const std::string &name) {
    if (name.empty() || name.size() > 12u) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c >= 0x20u && c < 0x7Fu) continue;
        // One two-byte UTF-8 character up to U+00FF.
        if ((c == 0xC2u || c == 0xC3u) && name.size() == 2u && i == 0u) {
            const auto next = static_cast<unsigned char>(name[1]);
            if ((next & 0xC0u) != 0x80u) return false;
            ++i;
            continue;
        }
        return false;
    }
    return true;
}

PromptLabel KeyPrompt(Scancode key, const KeyNamer &namer) {
    if (namer) {
        std::string name = namer(key);
        if (Drawable(name)) return Key(std::move(name));
    }
    return Key(KeyName(key));
}

PromptSet KeyboardSet(const InputMapping &mapping, const KeyNamer &namer) {
    PromptSet set = PromptSet::Psp();
    set.style = PromptStyle::kKeyboard;
    for (int i = 0; i < kPromptButtonCount; ++i) {
        const std::uint32_t mask = kPromptButtonMasks[static_cast<std::size_t>(i)];
        PromptLabel &label = set.buttons[static_cast<std::size_t>(i)];
        if (const KeyBinding *key = FindBinding(mapping.keys, mask)) {
            label = KeyPrompt(key->key, namer);
        } else if (const MouseButtonBinding *mouse = FindBinding(mapping.mouse_buttons, mask)) {
            label = Key(MouseButtonPromptName(mouse->button));
        }
        // Unbound: the PSP's own symbol or word stays.
    }
    // boot_main.cpp's OSK keyboard: Enter finishes, Backspace deletes.
    set.osk_finish = Key("Enter");
    set.osk_delete = Key("Backspace");
    return set;
}

PromptSet GamepadSet(PromptStyle style, const InputMapping &mapping, GamepadType type,
                     const std::array<FaceLabel, 4> &face) {
    PromptSet set = PromptSet::Psp();
    set.style = style;
    for (int i = 0; i < kPromptButtonCount; ++i) {
        const std::uint32_t mask = kPromptButtonMasks[static_cast<std::size_t>(i)];
        PromptLabel &label = set.buttons[static_cast<std::size_t>(i)];
        if (const PadButtonBinding *button = FindBinding(mapping.pad_buttons, mask)) {
            label = PadButtonPrompt(button->button, type, face);
        } else if (const PadAxisBinding *axis = FindBinding(mapping.pad_axes, mask)) {
            label = PadAxisPrompt(axis->axis, type);
        }
        if (label.text.empty()) label = PromptSet::Psp().buttons[static_cast<std::size_t>(i)];
    }
    return set;
}

char KindLetter(Kind kind) {
    switch (kind) {
    case Kind::kSymbol:
        return 's';
    case Kind::kWord:
        return 'w';
    case Kind::kKey:
        return 'k';
    }
    return 'k';
}

bool ParseLabel(std::string_view word, PromptLabel *out) {
    if (word.empty()) return false;
    PromptLabel label;
    switch (word[0]) {
    case 's':
        label.kind = Kind::kSymbol;
        break;
    case 'w':
        label.kind = Kind::kWord;
        break;
    case 'k':
        label.kind = Kind::kKey;
        break;
    default:
        return false;
    }
    if (!FromHex(word.substr(1), &label.text)) return false;
    *out = std::move(label);
    return true;
}

} // namespace

FaceLabel FaceLabelForType(GamepadType type, PadButton button) {
    static constexpr FaceLabel kLabels[4][4] = {
        // South, east, west, north.
        {FaceLabel::kA, FaceLabel::kB, FaceLabel::kX, FaceLabel::kY}, // ABXY
        {FaceLabel::kA, FaceLabel::kX, FaceLabel::kB, FaceLabel::kY}, // AXBY
        {FaceLabel::kB, FaceLabel::kA, FaceLabel::kY, FaceLabel::kX}, // BAYX
        {FaceLabel::kCross, FaceLabel::kCircle, FaceLabel::kSquare,   // Sony
         FaceLabel::kTriangle},
    };
    const auto index = static_cast<std::size_t>(button);
    if (index >= 4u) return FaceLabel::kUnknown;
    return kLabels[static_cast<std::size_t>(FaceStyleForType(type))][index];
}

std::array<FaceLabel, 4> FaceLabelsForType(GamepadType type) {
    return {FaceLabelForType(type, PadButton::kSouth), FaceLabelForType(type, PadButton::kEast),
            FaceLabelForType(type, PadButton::kWest), FaceLabelForType(type, PadButton::kNorth)};
}

// ---- DeviceTracker ----

void DeviceTracker::Set(const Device &device) {
    if (device == last_) return;
    last_ = device;
    ++generation_;
}

const Device *DeviceTracker::Find(std::uint32_t id) const {
    for (const Device &pad : pads_)
        if (pad.id == id) return &pad;
    return nullptr;
}

void DeviceTracker::OnKeyboardInput() {
    Device keyboard;
    keyboard.kind = Device::Kind::kKeyboard;
    Set(keyboard);
}

void DeviceTracker::OnGamepadAdded(std::uint32_t id, GamepadType type,
                                   const std::array<FaceLabel, 4> &face) {
    OnGamepadRemoved(id); // A re-added id replaces the old entry.
    Device pad;
    pad.kind = Device::Kind::kGamepad;
    pad.type = type;
    pad.face = face;
    pad.id = id;
    pads_.push_back(pad);
    if (last_.kind == Device::Kind::kNone) Set(pad);
}

void DeviceTracker::OnGamepadRemoved(std::uint32_t id) {
    const auto it =
        std::find_if(pads_.begin(), pads_.end(), [id](const Device &pad) { return pad.id == id; });
    if (it == pads_.end()) return;
    pads_.erase(it);
    if (last_.kind != Device::Kind::kGamepad || last_.id != id) return;
    if (!pads_.empty()) {
        Set(pads_.front());
    } else {
        OnKeyboardInput();
    }
}

void DeviceTracker::OnGamepadButton(std::uint32_t id) {
    if (const Device *pad = Find(id)) Set(*pad);
}

void DeviceTracker::OnGamepadAxis(std::uint32_t id, float value, float threshold) {
    if (std::isnan(value) || std::fabs(value) < threshold) return;
    OnGamepadButton(id);
}

// ---- Styles and labels ----

const char *PromptStyleName(PromptStyle style) {
    switch (style) {
    case PromptStyle::kPsp:
        return "psp";
    case PromptStyle::kXbox:
        return "xbox";
    case PromptStyle::kPlayStation:
        return "playstation";
    case PromptStyle::kNintendo:
        return "nintendo";
    case PromptStyle::kGeneric:
        return "generic";
    case PromptStyle::kKeyboard:
        return "keyboard";
    }
    return "?";
}

std::optional<PromptStyle> PromptStyleFromName(std::string_view name) {
    for (const PromptStyle style :
         {PromptStyle::kPsp, PromptStyle::kXbox, PromptStyle::kPlayStation, PromptStyle::kNintendo,
          PromptStyle::kGeneric, PromptStyle::kKeyboard}) {
        if (name == PromptStyleName(style)) return style;
    }
    return std::nullopt;
}

PromptStyle PromptStyleForGamepad(GamepadType type) {
    switch (type) {
    case GamepadType::kXbox360:
    case GamepadType::kXboxOne:
    case GamepadType::kSteam:
        return PromptStyle::kXbox;
    case GamepadType::kPs3:
    case GamepadType::kPs4:
    case GamepadType::kPs5:
        return PromptStyle::kPlayStation;
    case GamepadType::kSwitchPro:
    case GamepadType::kJoyconLeft:
    case GamepadType::kJoyconRight:
    case GamepadType::kJoyconPair:
    case GamepadType::kGameCube:
        return PromptStyle::kNintendo;
    default:
        return PromptStyle::kGeneric;
    }
}

PromptSet PromptSet::Psp() {
    PromptSet set;
    set.style = PromptStyle::kPsp;
    set.buttons = {Symbol("×"),   Symbol("○"),    Symbol("△"), Symbol("□"),
                   Word("Start"), Word("Select"), Word("L"),   Word("R")};
    return set;
}

const PromptLabel &PromptSet::For(std::uint32_t psp_button) const {
    static const PromptLabel kNone;
    for (std::size_t i = 0; i < kPromptButtonMasks.size(); ++i)
        if (kPromptButtonMasks[i] == psp_button) return buttons[i];
    return kNone;
}

std::string KeyName(Scancode key) {
    // USB HID usages (SDL_Scancode).
    if (key >= 4u && key <= 29u) return std::string(1, static_cast<char>('A' + (key - 4u)));
    if (key >= 30u && key <= 38u) return std::string(1, static_cast<char>('1' + (key - 30u)));
    if (key >= 58u && key <= 69u) return "F" + std::to_string(key - 57u);
    if (key >= 89u && key <= 97u) return "Num " + std::to_string(key - 88u);
    switch (key) {
    case 39:
        return "0";
    case 40:
        return "Enter";
    case 41:
        return "Esc";
    case 42:
        return "Backspace";
    case 43:
        return "Tab";
    case 44:
        return "Space";
    case 45:
        return "-";
    case 46:
        return "=";
    case 47:
        return "[";
    case 48:
        return "]";
    case 49:
        return "\\";
    case 51:
        return ";";
    case 52:
        return "'";
    case 53:
        return "`";
    case 54:
        return ",";
    case 55:
        return ".";
    case 56:
        return "/";
    case 57:
        return "Caps Lock";
    case 73:
        return "Insert";
    case 74:
        return "Home";
    case 75:
        return "Page Up";
    case 76:
        return "Delete";
    case 77:
        return "End";
    case 78:
        return "Page Down";
    case 79:
        return "Right";
    case 80:
        return "Left";
    case 81:
        return "Down";
    case 82:
        return "Up";
    case 84:
        return "Num /";
    case 85:
        return "Num *";
    case 86:
        return "Num -";
    case 87:
        return "Num +";
    case 88:
        return "Num Enter";
    case 98:
        return "Num 0";
    case 99:
        return "Num .";
    case 224:
    case 228:
        return "Ctrl";
    case 225:
    case 229:
        return "Shift";
    case 226:
    case 230:
        return "Alt";
    case 227:
    case 231:
        return "Super";
    default:
        return "Key " + std::to_string(key);
    }
}

std::string MouseButtonPromptName(MouseButton button) {
    switch (button) {
    case MouseButton::kLeft:
        return "Left click";
    case MouseButton::kMiddle:
        return "Middle click";
    case MouseButton::kRight:
        return "Right click";
    case MouseButton::kX1:
        return "Mouse 4";
    case MouseButton::kX2:
        return "Mouse 5";
    }
    return "Mouse";
}

PromptSet BuildPromptSet(PromptStyle style, const InputMapping &mapping, const Device *pad,
                         const KeyNamer &namer) {
    switch (style) {
    case PromptStyle::kPsp:
        return PromptSet::Psp();
    case PromptStyle::kKeyboard:
        return KeyboardSet(mapping, namer);
    default:
        break;
    }
    if (pad != nullptr && pad->kind == Device::Kind::kGamepad)
        return GamepadSet(style, mapping, pad->type, pad->face);
    const GamepadType type = TypicalPad(style);
    return GamepadSet(style, mapping, type, FaceLabelsForType(type));
}

PromptSet BuildPromptSetForDevice(const Device &device, const InputMapping &mapping,
                                  const KeyNamer &namer) {
    if (device.kind != Device::Kind::kGamepad)
        return BuildPromptSet(PromptStyle::kKeyboard, mapping, nullptr, namer);
    return BuildPromptSet(PromptStyleForGamepad(device.type), mapping, &device, namer);
}

// ---- The `p` record ----

std::string FormatPromptSet(const PromptSet &set) {
    std::string out = PromptStyleName(set.style);
    if (set == PromptSet::Psp()) return out;
    const auto add = [&out](const PromptLabel &label) {
        out += ' ';
        out += KindLetter(label.kind);
        out += ToHex(label.text);
    };
    for (const PromptLabel &label : set.buttons) add(label);
    add(set.osk_finish);
    add(set.osk_delete);
    return out;
}

bool ParsePromptSet(std::span<const std::string_view> words, PromptSet *out) {
    if (words.empty()) return false;
    const std::optional<PromptStyle> style = PromptStyleFromName(words[0]);
    if (!style) return false;
    PromptSet set = PromptSet::Psp();
    set.style = *style;
    if (words.size() == 1u) {
        // Only the PSP set is written without its labels.
        if (*style != PromptStyle::kPsp) return false;
        *out = std::move(set);
        return true;
    }
    if (words.size() != 1u + kPromptButtonCount + 2u) return false;
    for (std::size_t i = 0; i < set.buttons.size(); ++i)
        if (!ParseLabel(words[1u + i], &set.buttons[i])) return false;
    if (!ParseLabel(words[1u + kPromptButtonCount], &set.osk_finish) ||
        !ParseLabel(words[2u + kPromptButtonCount], &set.osk_delete))
        return false;
    *out = std::move(set);
    return true;
}

} // namespace mpo::frontend
