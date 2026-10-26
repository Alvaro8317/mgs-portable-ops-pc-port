// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vfpu as the PSPRecomp VFPU executor (docs/analysis/psp_analyze.md §9,
// gap 14).
//
// The corpus is generated with `psp_recomp --auto --vfpu-executor`, so every
// VFPU word (everything but bvf/bvt/bvfl/bvtl) reaches execute_vfpu() below,
// which runs it with mpo_vfpu: the PPSSPP v1.20.4 semantics, bit for bit
// (docs/sessions/F07a.md). The interpreter fallback uses the same executor.

#pragma once

#include <cstdint>

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

namespace mpo::recomp {

// Runs one VFPU word on ctx through mpo::vfpu::execute(). lv/sv go through
// runtime.memory(); an access outside guest memory stops the runtime. Returns
// false (the runtime then reports the word as unsupported) for a word that
// mpo_vfpu does not execute. The VFPU tables must be loaded
// (mpo::vfpu::load_tables) before a transcendental op runs.
bool execute_vfpu(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, std::uint32_t pc,
                  std::uint32_t word);

// psprecomp::set_vfpu_executor(&execute_vfpu). Process-global.
void install_vfpu_executor();

} // namespace mpo::recomp
