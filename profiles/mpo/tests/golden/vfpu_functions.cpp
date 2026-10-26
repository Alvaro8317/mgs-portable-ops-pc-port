// SPDX-License-Identifier: GPL-3.0-or-later

#include "golden/vfpu_functions.hpp"

#include <bit>

#include "difftest/difftest.hpp"
#include "vfpu/vfpu_math.hpp"

namespace mpo::golden {

std::span<const FunctionInfo> all_functions() {
    static constexpr FunctionInfo kFunctions[] = {
        {Function::kSin, "vfpu_sin", 1},     {Function::kCos, "vfpu_cos", 1},
        {Function::kAsin, "vfpu_asin", 1},   {Function::kSqrt, "vfpu_sqrt", 1},
        {Function::kRsqrt, "vfpu_rsqrt", 1}, {Function::kExp2, "vfpu_exp2", 1},
        {Function::kRexp2, "vfpu_rexp2", 1}, {Function::kLog2, "vfpu_log2", 1},
        {Function::kRcp, "vfpu_rcp", 1},     {Function::kDot, "vfpu_dot", 8},
    };
    return kFunctions;
}

namespace {

float uniform(difftest::Rng &rng, float lo, float hi) {
    return lo + (hi - lo) * (static_cast<float>(rng.next32() >> 8) / 16777216.0f);
}

float domain_value(Function fn, difftest::Rng &rng) {
    switch (fn) {
    case Function::kSin:
    case Function::kCos:
        return uniform(rng, -8.0f, 8.0f);
    case Function::kAsin:
        return uniform(rng, -1.0625f, 1.0625f);
    case Function::kExp2:
    case Function::kRexp2:
        return uniform(rng, -132.0f, 132.0f);
    case Function::kDot:
        return std::bit_cast<float>(rng.random_float_bits());
    default:
        // Positive normals across the whole exponent range.
        return std::bit_cast<float>(((1 + rng.below(254)) << 23) | (rng.next32() & 0x007FFFFFu));
    }
}

} // namespace

std::vector<float> make_inputs(const FunctionInfo &info, std::uint64_t seed, int count) {
    difftest::Rng rng(seed);
    std::vector<float> inputs;
    inputs.reserve(static_cast<std::size_t>(count) * static_cast<std::size_t>(info.arity));
    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < info.arity; ++k) {
            float value = 0.0f;
            switch (i % 3) {
            case 0:
                value = std::bit_cast<float>(rng.next32());
                break;
            case 1:
                value = domain_value(info.fn, rng);
                break;
            default: {
                // Sweep: walk all 2^32 patterns in `count / 3` strides with a
                // random low part, so every exponent and sign is visited.
                const std::uint64_t step = (0x100000000ull * 3) / static_cast<std::uint64_t>(count);
                const std::uint32_t base =
                    static_cast<std::uint32_t>((static_cast<std::uint64_t>(i / 3) * step));
                value = std::bit_cast<float>(base ^ (rng.next32() & 0x00000FFFu));
                break;
            }
            }
            inputs.push_back(value);
        }
    }
    return inputs;
}

float call_mpo(Function fn, const float *args) {
    switch (fn) {
    case Function::kSin:
        return vfpu::vfpu_sin(args[0]);
    case Function::kCos:
        return vfpu::vfpu_cos(args[0]);
    case Function::kAsin:
        return vfpu::vfpu_asin(args[0]);
    case Function::kSqrt:
        return vfpu::vfpu_sqrt(args[0]);
    case Function::kRsqrt:
        return vfpu::vfpu_rsqrt(args[0]);
    case Function::kExp2:
        return vfpu::vfpu_exp2(args[0]);
    case Function::kRexp2:
        return vfpu::vfpu_rexp2(args[0]);
    case Function::kLog2:
        return vfpu::vfpu_log2(args[0]);
    case Function::kRcp:
        return vfpu::vfpu_rcp(args[0]);
    case Function::kDot:
        return vfpu::vfpu_dot(args, args + 4);
    }
    return 0.0f;
}

} // namespace mpo::golden
