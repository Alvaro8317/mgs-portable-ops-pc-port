// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU difftest harness: random Allegrex states per instruction class, run by
// two executors and compared bit for bit (CLAUDE.md §7).
//
// An executor only has to turn a list of TestCase into final CpuStates. F07a
// ships a reference interpreter (mpo_vfpu + ALU/FPU semantics ported from
// PPSSPP), PPSSPP's own interpreter and an external-process executor (used
// for Unicorn). F07b adds the recompiler's emitter as one more executor: it
// can compile the case list ahead of time (the list is a pure function of
// class, seed and count) and look cases up by TestCase::id.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpo::difftest {

// Guest memory visible to the cases: a small window, enough for lv.q/sv.q and
// lwl/swr around a base register. Accesses outside it read 0, are dropped and
// count as a fault.
inline constexpr std::uint32_t kMemBase = 0x08900000u;
inline constexpr std::uint32_t kMemSize = 512u;
inline constexpr std::uint32_t kInitialPc = 0x08804000u;

struct CpuState {
    std::uint32_t gpr[32]{};
    std::uint32_t hi = 0;
    std::uint32_t lo = 0;
    std::uint32_t fpr[32]{}; // Raw bits.
    // FCR31 including the condition bit 23 (PPSSPP keeps it apart as fpcond).
    std::uint32_t fcr31 = 0;
    // VFPU registers in PPSSPP "voffset" order (see mpo::vfpu::Regs).
    alignas(16) float v[128]{};
    std::uint32_t vfpu_ctrl[16]{};
    // Next PC. A lone branch (bvf/bvt/bvfl/bvtl) leaves its target here when
    // taken, pc + 8 for a likely branch that is not taken, else pc + 4. A
    // kBranch case (branch + delay slot) leaves its target when taken, else
    // pc + 8.
    std::uint32_t pc = kInitialPc;
    std::uint32_t mem_faults = 0;
    std::array<std::uint8_t, kMemSize> mem{};

    [[nodiscard]] std::uint32_t read32(std::uint32_t address);
    void write32(std::uint32_t address, std::uint32_t value);
    [[nodiscard]] std::uint32_t read8(std::uint32_t address);
    void write8(std::uint32_t address, std::uint32_t value);
};

// kBranch (F07b): a branch or jump and its delay slot. Appended last, so the
// per-class seeds of the other classes do not change.
enum class InstrClass { kAlu, kMulDiv, kMem, kAllegrex, kFpu, kVfpu, kVfpuSeq, kBranch };

[[nodiscard]] std::string_view class_name(InstrClass cls);
// Parses "alu", "muldiv", "mem", "allegrex", "fpu", "vfpu", "vfpuseq", "branch".
bool parse_class(std::string_view text, InstrClass *out);
[[nodiscard]] std::span<const InstrClass> all_classes();

inline constexpr int kMaxOps = 4;

struct TestCase {
    std::uint32_t id = 0; // Index in the generated list.
    InstrClass cls = InstrClass::kAlu;
    std::string spec; // Name of the instruction spec that produced it.
    std::array<std::uint32_t, kMaxOps> ops{};
    int num_ops = 0;
    CpuState initial;
};

class Executor {
public:
    virtual ~Executor() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    // Runs every case from its initial state. out[i] receives the final
    // state; supported[i] = 0 when the executor cannot run case i (it is then
    // left out of the comparison and counted as skipped). Returns false, with
    // a message, only when the executor as a whole cannot run.
    virtual bool run(std::span<const TestCase> cases, std::span<CpuState> out,
                     std::span<std::uint8_t> supported, std::string *error) = 0;
};

// Comparison policy.
//
// kExact compares every bit.
//
// kNanEquivalent is kExact except that two NaNs in the same FPR or VFPU lane
// compare equal. Which NaN an add or multiply returns depends on the order in
// which the compiler happens to emit its (commutative) operands, so PPSSPP's
// own interpreter gives different NaN payloads and signs depending on the
// compiler and optimisation level (see docs/sessions/F07a.md). mpo_vfpu pins
// them (vfpu::fadd/fmul); the comparison against PPSSPP cannot.
//
// kMips32 is for plain MIPS32 references (Unicorn/QEMU): NaN-equivalent, and
// only the condition bit of FCR31 is compared, since QEMU also updates the
// cause/flag bits that the Allegrex interpreter in PPSSPP never sets.
enum class Policy { kExact, kNanEquivalent, kMips32 };

struct Mismatch {
    std::string field; // e.g. "gpr[4]", "v[S013]", "vfpu_ctrl[3]", "mem[0x10]"
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

std::vector<Mismatch> compare(const CpuState &a, const CpuState &b, Policy policy);

// 64-bit FNV-1a over everything compare() looks at. With canonical_nan, every
// NaN in an FPR or VFPU lane is hashed as 0x7FC00000 (see kNanEquivalent).
[[nodiscard]] std::uint64_t digest(const CpuState &state, bool canonical_nan = false);
void digest_append(std::uint64_t *hash, const void *data, std::size_t size);
inline constexpr std::uint64_t kDigestSeed = 0xCBF29CE484222325ull;

// --- Case generation -------------------------------------------------------

struct Spec {
    std::string name;
    InstrClass cls;
    std::uint32_t fixed;       // Bits that select the instruction.
    std::uint32_t random_mask; // Register/immediate fields filled at random.
    std::uint32_t flags;
};

// Spec::flags
enum SpecFlags : std::uint32_t {
    kSpecMemory = 1u << 0,            // Effective address rs+imm16 must hit the window.
    kSpecAlign16 = 1u << 1,           // ... aligned to 16 (lv.q/sv.q).
    kSpecAlign4 = 1u << 2,            // ... aligned to 4.
    kSpecAlign2 = 1u << 3,            // ... aligned to 2.
    kSpecVfpuMem = 1u << 4,           // VFPU memory op: imm is op & 0xFFFC.
    kSpecNotMips32 = 1u << 5,         // Allegrex-only encoding (Unicorn cannot run it as is).
    kSpecDivide = 1u << 6,            // div/divu: MIPS32 leaves x/0 unpredictable.
    kSpecNeedsTables = 1u << 7,       // VFPU op that uses the PPSSPP lookup tables.
    kSpecMoreAccurateVmmul = 1u << 8, // Run with PPSSPP's MoreAccurateVMMUL compat flag.
    kSpecBranch = 1u << 9,            // Branch/jump: the case is the branch and its delay slot.
    kSpecLink = 1u << 10,             // ... that writes a return address (jal, jalr, b*al).
    kSpecAbsolute = 1u << 11,         // ... whose target is absolute (j, jal, jr, jalr).
};

[[nodiscard]] const std::vector<Spec> &all_specs();
[[nodiscard]] std::vector<const Spec *> specs_of(InstrClass cls);
[[nodiscard]] const Spec *find_spec(std::string_view name);

// Deterministic on every platform (own PRNG, no <random> distributions).
std::vector<TestCase> generate_cases(InstrClass cls, std::uint64_t seed, int count);
std::vector<TestCase> generate_cases_for_spec(const Spec &spec, std::uint64_t seed, int count);

class Rng {
public:
    explicit Rng(std::uint64_t seed);
    std::uint64_t next();
    std::uint32_t next32() { return static_cast<std::uint32_t>(next() >> 32); }
    std::uint32_t below(std::uint32_t bound); // [0, bound)
    bool chance(std::uint32_t percent) { return below(100) < percent; }
    std::uint32_t random_float_bits();
    std::uint32_t random_int_bits();

private:
    std::uint64_t s_[4];
};

// --- Executors -------------------------------------------------------------

// mpo reference interpreter: ALU/Allegrex/FPU semantics ported from PPSSPP
// MIPSInt.cpp plus mpo_vfpu. Needs the VFPU tables for the transcendental ops.
std::unique_ptr<Executor> make_reference_interpreter();

// Wraps another executor and flips one bit of the result of every n-th case
// of the given spec (or of every spec if empty). Used to prove the harness
// catches differences.
std::unique_ptr<Executor> make_fault_injector(std::unique_ptr<Executor> inner, std::string spec,
                                              int every);

// Runs `command <cases file> <results file>` and reads the results. See
// external_executor.cpp for the text protocol.
std::unique_ptr<Executor> make_external_executor(std::string name, std::string command);

// --- Running and reporting -------------------------------------------------

struct RunReport {
    int cases = 0;
    int compared = 0;
    int skipped = 0;
    int mismatched = 0;
    int nan_only = 0;                 // Equal under the policy, but not bit for bit (NaN payloads).
    std::vector<std::string> details; // First few mismatches, human readable.
};

// Runs both executors over the cases and compares. Returns false if an
// executor failed to run (error set).
bool run_difftest(Executor &a, Executor &b, std::span<const TestCase> cases, Policy policy,
                  int max_details, RunReport *report, std::string *error);

std::string describe_case(const TestCase &c);

} // namespace mpo::difftest
