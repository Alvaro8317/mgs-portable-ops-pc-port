// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input mapping file (TOML). Each PSP input lists the host inputs that drive
// it, by SDL name:
//
//   [keyboard]                    # SDL key names (SDL_GetScancodeFromName)
//   cross = ["Z"]
//   select = ["V", "Return"]
//   analog_up = ["I"]
//
//   [gamepad]                     # SDL gamepad names; axes with a sign
//   cross = ["a"]                 # "a" is the bottom face button
//   l = ["leftshoulder", "+lefttrigger"]
//   analog_left = ["-leftx"]
//
//   [analog]
//   deadzone = 0.15
//
// PSP inputs: up down left right cross circle square triangle l r start
// select home, analog_{up,down,left,right} (the stick). A section that is
// present replaces that device's default bindings; a missing one keeps them.
// Unknown names are errors, so a typo never drops a binding silently.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "input.h"

namespace mpo::frontend {

// Starts from `base` (normally InputMapping::Default()) and applies the file.
bool ParseInputMapping(std::string_view toml_text, const InputMapping &base, InputMapping *out,
                       std::string *error);
bool LoadInputMapping(const std::filesystem::path &path, const InputMapping &base,
                      InputMapping *out, std::string *error);

// The mapping as a file ParseInputMapping reads back to the same bindings.
[[nodiscard]] std::string FormatInputMapping(const InputMapping &mapping);

} // namespace mpo::frontend
