// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal 16-bit PCM WAV writer for tools and tests.

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace mpo::audio {

inline bool WriteWav16(const std::string &path, const std::vector<int16_t> &samples, int channels,
                       int sampleRate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    auto put32 = [&out](uint32_t v) {
        const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                           static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        out.write(b, 4);
    };
    auto put16 = [&out](uint16_t v) {
        const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
        out.write(b, 2);
    };
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    put32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1); // PCM
    put16(static_cast<uint16_t>(channels));
    put32(static_cast<uint32_t>(sampleRate));
    put32(static_cast<uint32_t>(sampleRate * channels * 2));
    put16(static_cast<uint16_t>(channels * 2));
    put16(16);
    out.write("data", 4);
    put32(dataBytes);
    for (int16_t s : samples) {
        put16(static_cast<uint16_t>(s));
    }
    return static_cast<bool>(out);
}

} // namespace mpo::audio
