// SPDX-License-Identifier: GPL-3.0-or-later

#include "difftest/aot_layout.hpp"

#include <algorithm>
#include <cstdint>

namespace mpo::difftest {
namespace {

std::uint32_t branch_target(std::uint32_t pc, std::uint32_t op) {
    const auto offset = static_cast<std::int32_t>(static_cast<std::int16_t>(op & 0xFFFFu)) * 4;
    return pc + 4u + static_cast<std::uint32_t>(offset);
}

bool is_likely(std::uint32_t op) {
    return ((op >> 17) & 1u) != 0u;
}

// PSPRecomp raises its arithmetic-overflow stop where a MIPS CPU would trap
// (add, sub) and does not decode addi at all; PPSSPP, and so the reference,
// wraps around. mgp_main uses none of the three (its corpus has no
// arithmetic_overflow and no undecoded word), so the AOT executors leave
// those cases out instead of the framework changing its diagnostic.
bool framework_traps(const TestCase &c) {
    if (c.num_ops != 1) return false;
    const std::uint32_t op = c.ops[0];
    if (c.spec == "addi") return true;
    if (c.spec != "add" && c.spec != "sub") return false;
    const auto a =
        static_cast<std::int64_t>(static_cast<std::int32_t>(c.initial.gpr[(op >> 21) & 31]));
    const auto b =
        static_cast<std::int64_t>(static_cast<std::int32_t>(c.initial.gpr[(op >> 16) & 31]));
    const std::int64_t result = c.spec == "add" ? a + b : a - b;
    return result < INT32_MIN || result > INT32_MAX;
}

} // namespace

std::uint64_t class_seed(std::uint64_t seed, InstrClass cls) {
    return seed + static_cast<std::uint64_t>(cls);
}

bool is_vfpu_branch(std::uint32_t op) {
    return (op >> 26) == 0x12u && ((op >> 21) & 0x1Fu) == 0x08u;
}

std::vector<AotCase> aot_layout(std::uint64_t seed, int count) {
    std::vector<AotCase> layout;
    std::uint32_t address = kAotCodeBase;
    for (const InstrClass cls : all_classes()) {
        const std::vector<TestCase> cases = generate_cases(cls, class_seed(seed, cls), count);
        for (const TestCase &c : cases) {
            if (address + kAotSlotBytes > kAotCodeLimit) return {};
            AotCase slot;
            slot.cls = cls;
            slot.id = c.id;
            slot.address = address;
            const Spec *spec = find_spec(c.spec);
            if (cls == InstrClass::kBranch && spec != nullptr) {
                const std::uint32_t op = c.ops[0];
                slot.branch = 0;
                slot.absolute = (spec->flags & kSpecAbsolute) != 0u;
                if (spec->flags & kSpecLink)
                    slot.link = spec->name == "jalr" ? (op >> 11) & 31u : 31u;
                if (spec->name == "j" || spec->name == "jal") {
                    slot.target = (address & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
                } else if (spec->name == "jr" || spec->name == "jalr") {
                    slot.target = c.initial.gpr[(op >> 21) & 31u];
                } else {
                    slot.target = branch_target(address, op);
                }
            }
            for (int k = 0; k < c.num_ops; ++k) {
                const std::uint32_t op = c.ops[static_cast<std::size_t>(k)];
                slot.words[static_cast<std::size_t>(slot.num_words++)] = op;
                if (cls != InstrClass::kBranch && is_vfpu_branch(op)) {
                    if (k != c.num_ops - 1) slot.runnable = false;
                    slot.branch = k;
                    slot.target = branch_target(address + static_cast<std::uint32_t>(k) * 4u, op);
                    slot.words[static_cast<std::size_t>(slot.num_words++)] = 0u; // nop delay slot
                }
            }
            if (spec != nullptr && (spec->flags & kSpecMoreAccurateVmmul) != 0u)
                slot.runnable = false;
            if (framework_traps(c)) slot.runnable = false;
            layout.push_back(slot);
            address += kAotSlotBytes;
        }
    }
    // A taken branch must leave the case for an address that is not code, so
    // the run stops exactly there (the runtime looks addresses up without
    // their mirror bits). For a lone VFPU branch a target of pc + 8 is
    // ambiguous: the fall-through of a non-likely branch ends there too.
    for (AotCase &slot : layout) {
        if (slot.branch < 0 || !slot.runnable) continue;
        const std::uint32_t pc = slot.address + static_cast<std::uint32_t>(slot.branch) * 4u;
        const std::uint32_t target = slot.target & 0x1FFFFFFFu;
        if (slot.cls != InstrClass::kBranch && target == pc + 8u) {
            slot.runnable = false;
            continue;
        }
        const auto next = std::upper_bound(
            layout.begin(), layout.end(), target,
            [](std::uint32_t value, const AotCase &c) { return value < c.address; });
        if (next != layout.begin()) {
            const AotCase &owner = *std::prev(next);
            if (target < owner.address + static_cast<std::uint32_t>(owner.num_words) * 4u)
                slot.runnable = false;
        }
    }
    return layout;
}

void reference_link(const AotCase &c, CpuState &s) {
    if (c.link != 0 && s.gpr[c.link] == c.address + 8u) s.gpr[c.link] = kInitialPc + 8u;
}

std::uint32_t reference_pc(const AotCase &c, std::uint32_t final_pc) {
    std::uint32_t pc = final_pc;
    // A jump (always taken) ends at its absolute target.
    if (c.absolute) return final_pc;
    if (c.cls != InstrClass::kBranch && c.branch >= 0) {
        const std::uint32_t branch_pc = c.address + static_cast<std::uint32_t>(c.branch) * 4u;
        const std::uint32_t op = c.words[static_cast<std::size_t>(c.branch)];
        if (!is_likely(op) && final_pc == branch_pc + 8u) pc = branch_pc + 4u;
    }
    return pc - c.address + kInitialPc;
}

} // namespace mpo::difftest
