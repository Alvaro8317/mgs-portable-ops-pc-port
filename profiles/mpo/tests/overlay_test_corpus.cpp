// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for the psp_recomp output of a stage (F13), built into a real
// overlay library by mpo_add_overlay_library for mpo_overlay_library_test.
// Like a generated corpus it registers its functions through the runtime of
// the executable that loads it and reads one of the runtime's fast-path
// globals (the data symbols a Windows import library has to carry).

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

namespace psprecomp {

namespace {

void test_overlay_function(Runtime &, AllegrexContext &ctx) {
    ctx.gpr[2] = 0xC0DEu;
    ctx.gpr[3] = static_cast<std::uint32_t>(g_runtime_starvation_interval_fast);
    ctx.pc = ctx.gpr[31];
}

} // namespace

void register_test_overlay_functions(Runtime &runtime) {
    runtime.register_function(0x08B8F900u, &test_overlay_function, "test_overlay_function");
}

} // namespace psprecomp
