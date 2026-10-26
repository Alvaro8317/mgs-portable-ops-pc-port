// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_config.h"

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

namespace mpo::frontend {
namespace {

struct NamedTarget {
    std::string_view name;
    Target target;
};

const std::vector<NamedTarget> &TargetNames() {
    static const std::vector<NamedTarget> kNames = {
        {"up", Target::Button(ctrl::kUp)},
        {"down", Target::Button(ctrl::kDown)},
        {"left", Target::Button(ctrl::kLeft)},
        {"right", Target::Button(ctrl::kRight)},
        {"cross", Target::Button(ctrl::kCross)},
        {"circle", Target::Button(ctrl::kCircle)},
        {"square", Target::Button(ctrl::kSquare)},
        {"triangle", Target::Button(ctrl::kTriangle)},
        {"l", Target::Button(ctrl::kLTrigger)},
        {"r", Target::Button(ctrl::kRTrigger)},
        {"start", Target::Button(ctrl::kStart)},
        {"select", Target::Button(ctrl::kSelect)},
        {"home", Target::Button(ctrl::kHome)},
        {"analog_up", Target::Analog(ctrl::kStickLeft, ctrl::kAxisY, -1)},
        {"analog_down", Target::Analog(ctrl::kStickLeft, ctrl::kAxisY, +1)},
        {"analog_left", Target::Analog(ctrl::kStickLeft, ctrl::kAxisX, -1)},
        {"analog_right", Target::Analog(ctrl::kStickLeft, ctrl::kAxisX, +1)},
    };
    return kNames;
}

const Target *FindTarget(std::string_view name) {
    for (const NamedTarget &t : TargetNames()) {
        if (t.name == name) return &t.target;
    }
    return nullptr;
}

// A string or an array of strings.
bool StringList(const toml::node &node, std::vector<std::string> *out) {
    out->clear();
    if (const toml::value<std::string> *s = node.as_string()) {
        out->push_back(s->get());
        return true;
    }
    const toml::array *array = node.as_array();
    if (array == nullptr) return false;
    for (const toml::node &item : *array) {
        const toml::value<std::string> *s = item.as_string();
        if (s == nullptr) return false;
        out->push_back(s->get());
    }
    return true;
}

bool ParseKeyboard(const toml::table &table, InputMapping *m, std::string *error) {
    m->keys.clear();
    std::vector<std::string> names;
    for (const auto &[key, node] : table) {
        const Target *target = FindTarget(key.str());
        if (target == nullptr) {
            *error = "[keyboard]: unknown PSP input '" + std::string(key.str()) + "'";
            return false;
        }
        if (!StringList(node, &names)) {
            *error = "[keyboard] " + std::string(key.str()) + ": expected a list of key names";
            return false;
        }
        for (const std::string &name : names) {
            const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
            if (code == SDL_SCANCODE_UNKNOWN) {
                *error = "[keyboard] " + std::string(key.str()) + ": unknown key '" + name + "'";
                return false;
            }
            m->keys.push_back({static_cast<Scancode>(code), *target});
        }
    }
    return true;
}

bool ParseGamepad(const toml::table &table, InputMapping *m, std::string *error) {
    m->pad_buttons.clear();
    m->pad_axes.clear();
    std::vector<std::string> names;
    for (const auto &[key, node] : table) {
        const std::string psp(key.str());
        const Target *target = FindTarget(psp);
        if (target == nullptr) {
            *error = "[gamepad]: unknown PSP input '" + psp + "'";
            return false;
        }
        if (!StringList(node, &names)) {
            *error = "[gamepad] " + psp + ": expected a list of gamepad inputs";
            return false;
        }
        for (const std::string &name : names) {
            if (!name.empty() && (name[0] == '+' || name[0] == '-')) {
                const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name.c_str() + 1);
                if (axis == SDL_GAMEPAD_AXIS_INVALID) {
                    *error = "[gamepad] " + psp + ": unknown axis '" + name + "'";
                    return false;
                }
                m->pad_axes.push_back({static_cast<PadAxis>(axis),
                                       static_cast<std::int8_t>(name[0] == '+' ? 1 : -1), *target});
                continue;
            }
            const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
            if (button == SDL_GAMEPAD_BUTTON_INVALID) {
                if (SDL_GetGamepadAxisFromString(name.c_str()) != SDL_GAMEPAD_AXIS_INVALID) {
                    *error = "[gamepad] " + psp + ": axis '" + name + "' needs a direction ('+" +
                             name + "' or '-" + name + "')";
                } else {
                    *error = "[gamepad] " + psp + ": unknown button '" + name + "'";
                }
                return false;
            }
            m->pad_buttons.push_back({static_cast<PadButton>(button), *target});
        }
    }
    return true;
}

bool ParseAnalog(const toml::table &table, AnalogConfig *a, std::string *error) {
    for (const auto &[key, node] : table) {
        const std::string name(key.str());
        if (name == "circular") {
            // as_boolean, not value<bool>: toml++ would turn 1 into true.
            const toml::value<bool> *v = node.as_boolean();
            if (v == nullptr) {
                *error = "[analog] circular: expected true or false";
                return false;
            }
            a->circular = v->get();
            continue;
        }
        float *field = nullptr;
        float lo = 0.0f;
        float hi = 1.0f;
        if (name == "deadzone") {
            field = &a->deadzone;
            hi = 0.99f;
        } else if (name == "inverse_deadzone") {
            field = &a->inverse_deadzone;
        } else if (name == "sensitivity") {
            field = &a->sensitivity;
            lo = 0.01f;
            hi = 5.0f;
        } else if (name == "axis_button_threshold") {
            field = &a->axis_button_threshold;
            lo = 0.01f;
        } else {
            *error = "[analog]: unknown setting '" + name + "'";
            return false;
        }
        const auto v = node.is_number() ? node.value<double>() : std::nullopt;
        if (!v || *v < lo || *v > hi) {
            char text[128];
            std::snprintf(text, sizeof(text), "[analog] %s: expected a number in [%g, %g]",
                          name.c_str(), static_cast<double>(lo), static_cast<double>(hi));
            *error = text;
            return false;
        }
        *field = static_cast<float>(*v);
    }
    return true;
}

std::string Quoted(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string FloatText(float v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(v));
    std::string s(text);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

} // namespace

bool ParseInputMapping(std::string_view toml_text, const InputMapping &base, InputMapping *out,
                       std::string *error) {
    std::string local_error;
    if (error == nullptr) error = &local_error;
    InputMapping m = base;
    toml::parse_result result = toml::parse(toml_text);
    if (!result) {
        std::ostringstream message;
        message << result.error();
        *error = message.str();
        return false;
    }
    for (const auto &[key, node] : result.table()) {
        const std::string section(key.str());
        const toml::table *table = node.as_table();
        if (table == nullptr) {
            *error = "'" + section + "' must be a table ([" + section + "])";
            return false;
        }
        bool ok = false;
        if (section == "keyboard") {
            ok = ParseKeyboard(*table, &m, error);
        } else if (section == "gamepad") {
            ok = ParseGamepad(*table, &m, error);
        } else if (section == "analog") {
            ok = ParseAnalog(*table, &m.analog, error);
        } else {
            *error = "unknown section [" + section + "]";
        }
        if (!ok) return false;
    }
    *out = std::move(m);
    return true;
}

bool LoadInputMapping(const std::filesystem::path &path, const InputMapping &base,
                      InputMapping *out, std::string *error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    if (!ParseInputMapping(text.str(), base, out, error)) {
        if (error) *error = path.string() + ": " + *error;
        return false;
    }
    return true;
}

std::string FormatInputMapping(const InputMapping &mapping) {
    std::string out = "[keyboard]\n";
    for (const NamedTarget &t : TargetNames()) {
        std::vector<std::string> names;
        for (const KeyBinding &b : mapping.keys) {
            if (b.target != t.target) continue;
            const char *name = SDL_GetScancodeName(static_cast<SDL_Scancode>(b.key));
            if (name && *name) names.push_back(Quoted(name));
        }
        if (names.empty()) continue;
        out += std::string(t.name) + " = [";
        for (std::size_t i = 0; i < names.size(); ++i) out += (i ? ", " : "") + names[i];
        out += "]\n";
    }
    out += "\n[gamepad]\n";
    for (const NamedTarget &t : TargetNames()) {
        std::vector<std::string> names;
        for (const PadButtonBinding &b : mapping.pad_buttons) {
            if (b.target != t.target) continue;
            const char *name =
                SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b.button));
            if (name) names.push_back(Quoted(name));
        }
        for (const PadAxisBinding &b : mapping.pad_axes) {
            if (b.target != t.target) continue;
            const char *name = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(b.axis));
            if (name) names.push_back(Quoted(std::string(b.sign > 0 ? "+" : "-") + name));
        }
        if (names.empty()) continue;
        out += std::string(t.name) + " = [";
        for (std::size_t i = 0; i < names.size(); ++i) out += (i ? ", " : "") + names[i];
        out += "]\n";
    }
    const AnalogConfig &a = mapping.analog;
    out += "\n[analog]\n";
    out += "deadzone = " + FloatText(a.deadzone) + "\n";
    out += "inverse_deadzone = " + FloatText(a.inverse_deadzone) + "\n";
    out += "sensitivity = " + FloatText(a.sensitivity) + "\n";
    out += std::string("circular = ") + (a.circular ? "true" : "false") + "\n";
    out += "axis_button_threshold = " + FloatText(a.axis_button_threshold) + "\n";
    return out;
}

} // namespace mpo::frontend
