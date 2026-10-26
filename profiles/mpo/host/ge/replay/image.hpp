// SPDX-License-Identifier: GPL-3.0-or-later
//
// Framebuffer readback and a minimal PNG writer (stored deflate blocks, no
// compression library) for local analysis output.

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "memory_view.hpp"

namespace mpo::ge::replay {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4
};

// Reads a framebuffer from VRAM (address with or without the 0x04000000
// base, mirrors folded) in GE format 0-3 (565, 5551, 4444, 8888) and
// converts it to RGBA8. With `opaque`, alpha is forced to 255 (the alpha
// bits of a framebuffer are stencil, not transparency).
Image ReadFramebuffer(const MemoryView &mem, std::uint32_t addr, int stride, std::uint32_t format,
                      int width, int height, bool opaque);

// Encodes RGBA8 as PNG (colour type 6, 8 bits).
std::vector<std::uint8_t> EncodePng(const Image &image);
// Returns an empty string on success.
std::string WritePng(const std::filesystem::path &path, const Image &image);

} // namespace mpo::ge::replay
