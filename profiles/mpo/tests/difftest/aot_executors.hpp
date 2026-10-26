// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difftest executors over PSPRecomp (F07b): the code that psp_recomp emitted
// for the cases at build time, and the framework's interpreter fallback.
// Both run with mpo_vfpu as the VFPU executor (profiles/mpo/host/recomp), the
// way the game runs.

#pragma once

#include <cstdint>
#include <memory>

#include "difftest/difftest.hpp"

namespace mpo::difftest {

// The generated corpus linked into this binary (psp_recomp --auto
// --vfpu-executor over mpo_aot_casegen's output for the same seed and count).
// Cases that are not in that corpus come back unsupported.
std::unique_ptr<Executor> make_emitter_executor(std::uint64_t seed, int count);

// psprecomp::interpret_allegrex over the same layout. With vfpu_executor
// false, the interpreter keeps the framework's own VFPU lowering (to measure
// it; the game never runs that way).
std::unique_ptr<Executor> make_interpreter_executor(std::uint64_t seed, int count,
                                                    bool vfpu_executor);

} // namespace mpo::difftest
