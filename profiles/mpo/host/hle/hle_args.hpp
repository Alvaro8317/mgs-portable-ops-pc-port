// SPDX-License-Identifier: GPL-3.0-or-later
//
// Register helpers for HLE handlers: arguments, results and guest strings.

#pragma once

#include <cstdint>
#include <string>

#include "kernel/kernel.hpp"
#include "psprecomp/allegrex_context.hpp"

namespace mpo::hle {

using Ctx = psprecomp::AllegrexContext;

// $a0..$a3, $t0..$t3: the eight register arguments of the PSP ABI.
inline std::uint32_t arg(const Ctx &ctx, int index) {
    return ctx.gpr[static_cast<std::size_t>(index < 4 ? 4 + index : 8 + (index - 4))];
}
// A 64-bit argument in an aligned register pair ($a2:$a3, $t0:$t1).
inline std::uint64_t arg64(const Ctx &ctx, int low_index) {
    return static_cast<std::uint64_t>(arg(ctx, low_index)) |
           (static_cast<std::uint64_t>(arg(ctx, low_index + 1)) << 32u);
}
inline void ret(Ctx &ctx, std::uint32_t value) {
    ctx.gpr[2] = value;
}
inline void ret64(Ctx &ctx, std::uint64_t value) {
    ctx.gpr[2] = static_cast<std::uint32_t>(value);
    ctx.gpr[3] = static_cast<std::uint32_t>(value >> 32u);
}

// A guest C string argument; `present` false means a null or bad pointer.
struct GuestString {
    std::string text;
    bool present{};
    [[nodiscard]] const char *c_str() const { return present ? text.c_str() : nullptr; }
};
inline GuestString guest_string(kernel::Kernel &k, std::uint32_t address) {
    if (address == 0u || !k.memory().is_valid(address)) return {};
    return {k.memory().read_c_string(address), true};
}

} // namespace mpo::hle
