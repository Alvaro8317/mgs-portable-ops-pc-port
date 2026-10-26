// SPDX-License-Identifier: GPL-3.0-or-later
//
// PPSSPP's own interpreter (MIPSInt.cpp, MIPSIntVFPU.cpp, MIPSVFPUUtils.cpp
// and the MIPSTables.cpp decode tables, v1.20.4, unmodified) as a difftest
// executor. Test-only: it is never linked into the runtime.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "difftest/difftest.hpp"
#include "golden/vfpu_functions.hpp"

namespace mpo::ppsspp_ref {

// Points PPSSPP's VFS at the directory that holds vfpu/*.dat (the parent of
// MPO_VFPU_TABLES_DIR) and checks that every table is readable.
bool init(const std::filesystem::path &vfpu_tables_dir, std::string *error);

std::unique_ptr<difftest::Executor> make_executor();

// PPSSPP's scalar VFPU math (vfpu_sin, vfpu_dot...) for function goldens.
float call(golden::Function fn, const float *args);

} // namespace mpo::ppsspp_ref
