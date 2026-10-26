// SPDX-License-Identifier: GPL-3.0-or-later
//
// Function-level golden vectors: the scalar VFPU math (vfpu_sin, vfpu_dot...)
// over deterministic inputs, shared by the generator (PPSSPP side) and the
// test (mpo_vfpu side).

#pragma once

#include <bit>
#include <cstdint>
#include <span>
#include <vector>

namespace mpo::golden {

enum class Function {
    kSin,
    kCos,
    kAsin,
    kSqrt,
    kRsqrt,
    kExp2,
    kRexp2,
    kLog2,
    kRcp,
    kDot,
};

struct FunctionInfo {
    Function fn;
    const char *name; // PPSSPP's function name.
    int arity;        // Floats per call.
};

std::span<const FunctionInfo> all_functions();

// count calls' worth of inputs (count * arity floats), deterministic on every
// platform: a third any bit pattern, a third in the function's useful domain
// and a third sweeping the whole exponent range.
std::vector<float> make_inputs(const FunctionInfo &info, std::uint64_t seed, int count);

// mpo_vfpu's implementation.
float call_mpo(Function fn, const float *args);

// Digest of the result bits of fn over the inputs, using call.
template <typename Call>
std::uint64_t digest_function(const FunctionInfo &info, const std::vector<float> &inputs,
                              Call call) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i + static_cast<std::size_t>(info.arity) <= inputs.size();
         i += static_cast<std::size_t>(info.arity)) {
        const float result = call(info.fn, &inputs[i]);
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(result);
        for (int b = 0; b < 4; ++b) {
            hash ^= (bits >> (8 * b)) & 0xFF;
            hash *= 0x100000001B3ull;
        }
    }
    return hash;
}

} // namespace mpo::golden
