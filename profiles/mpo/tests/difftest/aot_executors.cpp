// SPDX-License-Identifier: GPL-3.0-or-later

#include "difftest/aot_executors.hpp"

#include <bit>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "difftest/aot_layout.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "recomp/vfpu_executor.hpp"

namespace mpo::difftest {
namespace {

// The final PC an executor reports when a case did not end the way a case
// ends (another stop reason): it can never match the reference.
constexpr std::uint32_t kBadStop = 0xFFFFFFFFu;

void to_context(const CpuState &s, std::uint32_t pc, psprecomp::AllegrexContext &ctx) {
    ctx = psprecomp::AllegrexContext{};
    std::memcpy(ctx.gpr.data(), s.gpr, sizeof(s.gpr));
    ctx.hi = s.hi;
    ctx.lo = s.lo;
    std::memcpy(ctx.fpr.data(), s.fpr, sizeof(s.fpr)); // raw bits, no float round trip
    ctx.fcr31 = s.fcr31;
    std::memcpy(ctx.vfpu.data(), s.v, sizeof(s.v));
    std::memcpy(ctx.vfpu_ctrl.data(), s.vfpu_ctrl, sizeof(s.vfpu_ctrl));
    ctx.pc = pc;
}

void from_context(const psprecomp::AllegrexContext &ctx, CpuState &s) {
    std::memcpy(s.gpr, ctx.gpr.data(), sizeof(s.gpr));
    s.hi = ctx.hi;
    s.lo = ctx.lo;
    std::memcpy(s.fpr, ctx.fpr.data(), sizeof(s.fpr));
    s.fcr31 = ctx.fcr31;
    std::memcpy(s.v, ctx.vfpu.data(), sizeof(s.v));
    std::memcpy(s.vfpu_ctrl, ctx.vfpu_ctrl.data(), sizeof(s.vfpu_ctrl));
    s.mem_faults = 0;
}

void write_window(psprecomp::Runtime &runtime, const CpuState &s) {
    for (std::uint32_t k = 0; k < kMemSize; ++k) runtime.memory().store8(kMemBase + k, s.mem[k]);
}

void read_window(const psprecomp::Runtime &runtime, CpuState &s) {
    for (std::uint32_t k = 0; k < kMemSize; ++k) s.mem[k] = runtime.memory().load8(kMemBase + k);
}

// Finds the slot of a case and checks that it holds the same instructions.
class Layout {
public:
    Layout(std::uint64_t seed, int count) {
        for (const AotCase &c : aot_layout(seed, count))
            by_key_[{static_cast<int>(c.cls), c.id}] = c;
    }

    const AotCase *find(const TestCase &c) const {
        const auto it = by_key_.find({static_cast<int>(c.cls), c.id});
        if (it == by_key_.end() || !it->second.runnable) return nullptr;
        const AotCase &slot = it->second;
        int word = 0;
        for (int k = 0; k < c.num_ops; ++k) {
            if (word >= slot.num_words ||
                slot.words[static_cast<std::size_t>(word)] != c.ops[static_cast<std::size_t>(k)])
                return nullptr;
            word +=
                c.cls != InstrClass::kBranch && is_vfpu_branch(c.ops[static_cast<std::size_t>(k)])
                    ? 2
                    : 1;
        }
        return word == slot.num_words ? &slot : nullptr;
    }

    const std::map<std::pair<int, std::uint32_t>, AotCase> &all() const { return by_key_; }

private:
    std::map<std::pair<int, std::uint32_t>, AotCase> by_key_;
};

class EmitterExecutor final : public Executor {
public:
    EmitterExecutor(std::uint64_t seed, int count) : layout_(seed, count) {
        psprecomp::register_generated_functions(runtime_);
    }

    std::string name() const override { return "emitter"; }

    bool run(std::span<const TestCase> cases, std::span<CpuState> out,
             std::span<std::uint8_t> supported, std::string *) override {
        mpo::recomp::install_vfpu_executor();
        // A case ends where control leaves its slot: an address with no
        // generated entry. Interpreting the gap instead would hide that.
        const bool interpreter = psprecomp::interpreter_fallback_enabled();
        psprecomp::set_interpreter_fallback_enabled(false);
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const TestCase &c = cases[i];
            const AotCase *slot = layout_.find(c);
            supported[i] = slot != nullptr ? 1 : 0;
            if (slot == nullptr) continue;
            to_context(c.initial, slot->address, runtime_.cpu());
            write_window(runtime_, c.initial);
            runtime_.run(slot->address, 16u);
            CpuState &s = out[i];
            s = c.initial;
            from_context(runtime_.cpu(), s);
            read_window(runtime_, s);
            const std::string &reason = runtime_.stop_reason();
            const bool clean = reason.rfind("No recompiled function registered at ", 0) == 0 ||
                               reason.find("invalid internal function entry") != std::string::npos;
            s.pc = clean ? reference_pc(*slot, runtime_.cpu().pc) : kBadStop;
            reference_link(*slot, s);
            if (!clean && bad_stops_++ < 5)
                std::fprintf(stderr, "emitter: case %s #%u stopped with: %s\n", c.spec.c_str(),
                             c.id, reason.c_str());
        }
        psprecomp::set_interpreter_fallback_enabled(interpreter);
        return true;
    }

private:
    Layout layout_;
    psprecomp::Runtime runtime_;
    int bad_stops_ = 0;
};

class InterpreterExecutor final : public Executor {
public:
    InterpreterExecutor(std::uint64_t seed, int count, bool vfpu_executor)
        : layout_(seed, count), vfpu_executor_(vfpu_executor) {
        for (const auto &[key, slot] : layout_.all()) {
            for (int k = 0; k < slot.num_words; ++k)
                runtime_.memory().store32(slot.address + static_cast<std::uint32_t>(k) * 4u,
                                          slot.words[static_cast<std::size_t>(k)]);
        }
    }

    std::string name() const override { return vfpu_executor_ ? "interp" : "interp-native"; }

    bool run(std::span<const TestCase> cases, std::span<CpuState> out,
             std::span<std::uint8_t> supported, std::string *) override {
        if (vfpu_executor_)
            mpo::recomp::install_vfpu_executor();
        else
            psprecomp::set_vfpu_executor(nullptr);
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const TestCase &c = cases[i];
            const AotCase *slot = layout_.find(c);
            supported[i] = slot != nullptr ? 1 : 0;
            if (slot == nullptr) continue;
            psprecomp::AllegrexContext ctx;
            to_context(c.initial, slot->address, ctx);
            write_window(runtime_, c.initial);
            // A branch and its delay slot count as two; stopping one past the
            // branch also covers a likely branch that annuls its delay slot.
            const std::uint64_t budget = slot->branch >= 0
                                             ? static_cast<std::uint64_t>(slot->branch) + 1u
                                             : static_cast<std::uint64_t>(slot->num_words);
            const psprecomp::InterpreterExit exit =
                psprecomp::interpret_allegrex(runtime_, ctx, budget);
            CpuState &s = out[i];
            s = c.initial;
            from_context(ctx, s);
            read_window(runtime_, s);
            s.pc =
                exit == psprecomp::InterpreterExit::Budget ? reference_pc(*slot, ctx.pc) : kBadStop;
            reference_link(*slot, s);
            if (exit != psprecomp::InterpreterExit::Budget && bad_stops_++ < 5)
                std::fprintf(stderr, "%s: case %s #%u stopped with: %s\n", name().c_str(),
                             c.spec.c_str(), c.id, runtime_.stop_reason().c_str());
        }
        mpo::recomp::install_vfpu_executor();
        return true;
    }

private:
    Layout layout_;
    bool vfpu_executor_;
    psprecomp::Runtime runtime_;
    int bad_stops_ = 0;
};

} // namespace

std::unique_ptr<Executor> make_emitter_executor(std::uint64_t seed, int count) {
    return std::make_unique<EmitterExecutor>(seed, count);
}

std::unique_ptr<Executor> make_interpreter_executor(std::uint64_t seed, int count,
                                                    bool vfpu_executor) {
    return std::make_unique<InterpreterExecutor>(seed, count, vfpu_executor);
}

} // namespace mpo::difftest
