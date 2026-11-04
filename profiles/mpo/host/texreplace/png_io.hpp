// SPDX-License-Identifier: GPL-3.0-or-later
//
// F24c: PNG files of the texture packs, through SDL3's own PNG support
// (SDL_LoadPNG / SDL_SavePNG, stb_image inside SDL; no other dependency).
// Without SDL3 (MPO_ENABLE_SDL3=OFF or the audio-only build) both fail with
// a message, and the replacement packs cannot be used.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mpo::texreplace {

// Whether this build can read and write PNG.
[[nodiscard]] bool HavePng();

// Any PNG (palette, grey, RGB, with or without alpha, 8 or 16 bits) as RGBA8
// bytes, row-major.
bool LoadPng(const std::filesystem::path &path, int *width, int *height,
             std::vector<std::uint8_t> *rgba, std::string *error);

bool SavePng(const std::filesystem::path &path, int width, int height,
             const std::vector<std::uint8_t> &rgba, std::string *error);

} // namespace mpo::texreplace
