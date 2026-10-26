// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guest layout of the difftest cases for the ahead-of-time executors (F07b).
//
// The emitter executor cannot compile a case when it is asked to run it: the
// cases are recompiled by psp_recomp at build time, like a game. Because the
// case list is a pure function of (class, seed, count), the build tool
// (mpo_aot_casegen) and the executors compute the same layout from the same
// two numbers: every case of every class gets its own small slot of guest
// code, the classes one after another in all_classes() order.
//
// A slot holds the case's instructions and, after a VFPU branch, a nop as its
// delay slot. The words between slots are not code (the hints only declare the
// slots as executable), so control that leaves a case reaches an address with
// no generated entry and the runtime stops there: that address is the case's
// final PC.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "difftest/difftest.hpp"

namespace mpo::difftest {

// Above the 512-byte data window at kMemBase, so slots never overlap it, and
// inside the 24 MiB of PSP user RAM.
inline constexpr std::uint32_t kAotCodeBase = 0x08A00000u;
inline constexpr std::uint32_t kAotSlotBytes = 0x20u;
inline constexpr std::uint32_t kAotCodeLimit = 0x0A000000u;
inline constexpr int kAotMaxWords = kMaxOps + 1;

struct AotCase {
    InstrClass cls = InstrClass::kAlu;
    std::uint32_t id = 0;
    std::uint32_t address = 0;
    std::array<std::uint32_t, kAotMaxWords> words{};
    int num_words = 0;
    // Index of the branch among the ops, or -1: a lone VFPU branch
    // (bvf/bvt/bvfl/bvtl, its nop delay slot added here) or the branch of a
    // kBranch case (whose second op is its delay slot).
    int branch = -1;
    // kBranch: the target is absolute (j, jal, jr, jalr), so the final PC is
    // not relative to the slot.
    bool absolute = false;
    // The register a branch-and-link writes the return address to, or 0. The
    // executors report slot + 8 there as kInitialPc + 8.
    std::uint32_t link = 0;
    // The address a taken branch goes to (before the delay slot runs).
    std::uint32_t target = 0;
    // False when the AOT executors cannot reproduce the reference's view of
    // the case: a branch that is not the last op, a branch whose target is
    // inside some slot (it would run other code) or ambiguous, a spec that
    // needs a PPSSPP compat flag the runtime does not set, or an add/sub that
    // overflows or an addi (PSPRecomp stops on them, PPSSPP wraps).
    bool runnable = true;
};

// The seed generate_cases() gets for one class, as mpo_difftest uses it.
[[nodiscard]] std::uint64_t class_seed(std::uint64_t seed, InstrClass cls);

// Lays out generate_cases(cls, class_seed(seed, cls), count) for every class.
// Fails (returns an empty list) if the slots do not fit below kAotCodeLimit.
[[nodiscard]] std::vector<AotCase> aot_layout(std::uint64_t seed, int count);

// True if the word is bvf/bvt/bvfl/bvtl.
[[nodiscard]] bool is_vfpu_branch(std::uint32_t op);

// Rewrites the return address of a branch-and-link case into the reference's
// view (kInitialPc + 8), if the delay slot left it in place.
void reference_link(const AotCase &c, CpuState &s);

// Translates a final PC observed at `slot_address` into the reference's view
// (cases start at kInitialPc). A not-taken, non-likely VFPU branch leaves the
// reference at pc + 4 (its delay slot is not part of the case), while the
// generated code has already run the nop delay slot.
[[nodiscard]] std::uint32_t reference_pc(const AotCase &c, std::uint32_t final_pc);

} // namespace mpo::difftest
