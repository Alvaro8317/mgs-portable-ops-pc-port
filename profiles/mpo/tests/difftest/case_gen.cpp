// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instruction specs and random case generation for the difftest harness.
// Encodings follow PPSSPP Core/MIPS/MIPSTables.cpp (v1.20.4).

#include <bit>

#include "difftest/difftest.hpp"

namespace mpo::difftest {

namespace {

// Extra Spec::flags used only by the generator.
constexpr std::uint32_t kSpecExt = 1u << 16; // ext: pos + size <= 32.
constexpr std::uint32_t kSpecIns = 1u << 17; // ins: msb >= lsb.

constexpr std::uint32_t kRs = 0x03E00000u;
constexpr std::uint32_t kRt = 0x001F0000u;
constexpr std::uint32_t kRd = 0x0000F800u;
constexpr std::uint32_t kSa = 0x000007C0u;
constexpr std::uint32_t kImm16 = 0x0000FFFFu;
constexpr std::uint32_t kFt = 0x001F0000u;
constexpr std::uint32_t kFs = 0x0000F800u;
constexpr std::uint32_t kFd = 0x000007C0u;
constexpr std::uint32_t kVd = 0x0000007Fu;
constexpr std::uint32_t kVs = 0x00007F00u;
constexpr std::uint32_t kVt = 0x007F0000u;

struct SizeVariant {
    const char *suffix;
    std::uint32_t bits;
};
constexpr SizeVariant kSizes[4] = {{".s", 0x0000}, {".p", 0x0080}, {".t", 0x8000}, {".q", 0x8080}};

std::vector<Spec> build_specs() {
    std::vector<Spec> specs;
    const auto add = [&specs](std::string name, InstrClass cls, std::uint32_t fixed,
                              std::uint32_t mask, std::uint32_t flags = 0) {
        specs.push_back({std::move(name), cls, fixed, mask, flags});
    };

    // --- ALU (SPECIAL and I-type) ---
    const InstrClass alu = InstrClass::kAlu;
    add("sll", alu, 0x00000000, kRt | kRd | kSa);
    add("srl", alu, 0x00000002, kRt | kRd | kSa);
    add("sra", alu, 0x00000003, kRt | kRd | kSa);
    add("sllv", alu, 0x00000004, kRs | kRt | kRd);
    add("srlv", alu, 0x00000006, kRs | kRt | kRd);
    add("srav", alu, 0x00000007, kRs | kRt | kRd);
    add("add", alu, 0x00000020, kRs | kRt | kRd);
    add("addu", alu, 0x00000021, kRs | kRt | kRd);
    add("sub", alu, 0x00000022, kRs | kRt | kRd);
    add("subu", alu, 0x00000023, kRs | kRt | kRd);
    add("and", alu, 0x00000024, kRs | kRt | kRd);
    add("or", alu, 0x00000025, kRs | kRt | kRd);
    add("xor", alu, 0x00000026, kRs | kRt | kRd);
    add("nor", alu, 0x00000027, kRs | kRt | kRd);
    add("slt", alu, 0x0000002A, kRs | kRt | kRd);
    add("sltu", alu, 0x0000002B, kRs | kRt | kRd);
    add("addi", alu, 0x20000000, kRs | kRt | kImm16);
    add("addiu", alu, 0x24000000, kRs | kRt | kImm16);
    add("slti", alu, 0x28000000, kRs | kRt | kImm16);
    add("sltiu", alu, 0x2C000000, kRs | kRt | kImm16);
    add("andi", alu, 0x30000000, kRs | kRt | kImm16);
    add("ori", alu, 0x34000000, kRs | kRt | kImm16);
    add("xori", alu, 0x38000000, kRs | kRt | kImm16);
    add("lui", alu, 0x3C000000, kRt | kImm16);

    // --- Multiply/divide and HI/LO ---
    const InstrClass md = InstrClass::kMulDiv;
    add("mult", md, 0x00000018, kRs | kRt);
    add("multu", md, 0x00000019, kRs | kRt);
    add("div", md, 0x0000001A, kRs | kRt, kSpecDivide);
    add("divu", md, 0x0000001B, kRs | kRt, kSpecDivide);
    add("mfhi", md, 0x00000010, kRd);
    add("mthi", md, 0x00000011, kRs);
    add("mflo", md, 0x00000012, kRd);
    add("mtlo", md, 0x00000013, kRs);

    // --- Loads and stores (GPR and FPU) ---
    const InstrClass mem = InstrClass::kMem;
    const std::uint32_t mem_mask = kRs | kRt | kImm16;
    add("lb", mem, 0x80000000, mem_mask, kSpecMemory);
    add("lh", mem, 0x84000000, mem_mask, kSpecMemory | kSpecAlign2);
    add("lwl", mem, 0x88000000, mem_mask, kSpecMemory);
    add("lw", mem, 0x8C000000, mem_mask, kSpecMemory | kSpecAlign4);
    add("lbu", mem, 0x90000000, mem_mask, kSpecMemory);
    add("lhu", mem, 0x94000000, mem_mask, kSpecMemory | kSpecAlign2);
    add("lwr", mem, 0x98000000, mem_mask, kSpecMemory);
    add("sb", mem, 0xA0000000, mem_mask, kSpecMemory);
    add("sh", mem, 0xA4000000, mem_mask, kSpecMemory | kSpecAlign2);
    add("swl", mem, 0xA8000000, mem_mask, kSpecMemory);
    add("sw", mem, 0xAC000000, mem_mask, kSpecMemory | kSpecAlign4);
    add("swr", mem, 0xB8000000, mem_mask, kSpecMemory);
    add("lwc1", mem, 0xC4000000, mem_mask, kSpecMemory | kSpecAlign4);
    add("swc1", mem, 0xE4000000, mem_mask, kSpecMemory | kSpecAlign4);

    // --- Allegrex extensions (CLAUDE.md §5) ---
    const InstrClass ax = InstrClass::kAllegrex;
    add("rotr", ax, 0x00200002, kRt | kRd | kSa);
    add("rotrv", ax, 0x00000046, kRs | kRt | kRd);
    add("movz", ax, 0x0000000A, kRs | kRt | kRd);
    add("movn", ax, 0x0000000B, kRs | kRt | kRd);
    add("clz", ax, 0x00000016, kRs | kRd, kSpecNotMips32);
    add("clo", ax, 0x00000017, kRs | kRd, kSpecNotMips32);
    add("max", ax, 0x0000002C, kRs | kRt | kRd, kSpecNotMips32);
    add("min", ax, 0x0000002D, kRs | kRt | kRd, kSpecNotMips32);
    add("madd", ax, 0x0000001C, kRs | kRt, kSpecNotMips32);
    add("maddu", ax, 0x0000001D, kRs | kRt, kSpecNotMips32);
    add("msub", ax, 0x0000002E, kRs | kRt, kSpecNotMips32);
    add("msubu", ax, 0x0000002F, kRs | kRt, kSpecNotMips32);
    add("ext", ax, 0x7C000000, kRs | kRt | kRd | kSa, kSpecExt);
    add("ins", ax, 0x7C000004, kRs | kRt | kRd | kSa, kSpecIns);
    add("wsbh", ax, 0x7C0000A0, kRt | kRd);
    add("wsbw", ax, 0x7C0000E0, kRt | kRd, kSpecNotMips32);
    add("seb", ax, 0x7C000420, kRt | kRd);
    add("bitrev", ax, 0x7C000520, kRt | kRd, kSpecNotMips32);
    add("seh", ax, 0x7C000620, kRt | kRd);

    // --- FPU (single precision only on the Allegrex) ---
    const InstrClass fpu = InstrClass::kFpu;
    add("add.s", fpu, 0x46000000, kFt | kFs | kFd);
    add("sub.s", fpu, 0x46000001, kFt | kFs | kFd);
    add("mul.s", fpu, 0x46000002, kFt | kFs | kFd);
    add("div.s", fpu, 0x46000003, kFt | kFs | kFd);
    add("sqrt.s", fpu, 0x46000004, kFs | kFd);
    add("abs.s", fpu, 0x46000005, kFs | kFd);
    add("mov.s", fpu, 0x46000006, kFs | kFd);
    add("neg.s", fpu, 0x46000007, kFs | kFd);
    add("round.w.s", fpu, 0x4600000C, kFs | kFd);
    add("trunc.w.s", fpu, 0x4600000D, kFs | kFd);
    add("ceil.w.s", fpu, 0x4600000E, kFs | kFd);
    add("floor.w.s", fpu, 0x4600000F, kFs | kFd);
    add("cvt.w.s", fpu, 0x46000024, kFs | kFd);
    add("cvt.s.w", fpu, 0x46800020, kFs | kFd);
    add("c.cond.s", fpu, 0x46000030, kFt | kFs | 0xFu);
    add("mfc1", fpu, 0x44000000, kRt | kFs);
    add("mtc1", fpu, 0x44800000, kRt | kFs);
    add("cfc1", fpu, 0x4440F800, kRt); // FCR31
    add("ctc1", fpu, 0x44C0F800, kRt); // FCR31

    // --- VFPU ---
    const InstrClass vf = InstrClass::kVfpu;
    const auto sized = [&](const char *name, std::uint32_t fixed, std::uint32_t mask,
                           std::uint32_t flags = 0) {
        for (const SizeVariant &size : kSizes) {
            add(std::string(name) + size.suffix, vf, fixed | size.bits, mask, flags);
        }
    };
    const std::uint32_t v3 = kVd | kVs | kVt;
    const std::uint32_t v2 = kVd | kVs;
    // VFPU0/1/3
    sized("vadd", 0x60000000, v3);
    sized("vsub", 0x60800000, v3);
    sized("vsbn", 0x61000000, v3);
    sized("vdiv", 0x63800000, v3);
    sized("vmul", 0x64000000, v3);
    sized("vdot", 0x64800000, v3);
    sized("vscl", 0x65000000, v3);
    sized("vhdp", 0x66000000, v3);
    sized("vcrs", 0x66800000, v3);
    sized("vdet", 0x67000000, v3);
    sized("vcmp", 0x6C000000, kVs | kVt | 0xFu);
    sized("vmin", 0x6D000000, v3);
    sized("vmax", 0x6D800000, v3);
    sized("vscmp", 0x6E800000, v3);
    sized("vsge", 0x6F000000, v3);
    sized("vslt", 0x6F800000, v3);
    // VFPU4
    constexpr std::uint32_t kTab = kSpecNeedsTables;
    sized("vmov", 0xD0000000, v2);
    sized("vabs", 0xD0010000, v2);
    sized("vneg", 0xD0020000, v2);
    sized("vidt", 0xD0030000, kVd);
    sized("vsat0", 0xD0040000, v2);
    sized("vsat1", 0xD0050000, v2);
    sized("vzero", 0xD0060000, kVd);
    sized("vone", 0xD0070000, kVd);
    sized("vrcp", 0xD0100000, v2, kTab);
    sized("vrsq", 0xD0110000, v2);
    sized("vsin", 0xD0120000, v2, kTab);
    sized("vcos", 0xD0130000, v2, kTab);
    sized("vexp2", 0xD0140000, v2, kTab);
    sized("vlog2", 0xD0150000, v2, kTab);
    sized("vsqrt", 0xD0160000, v2);
    sized("vasin", 0xD0170000, v2, kTab);
    sized("vnrcp", 0xD0180000, v2, kTab);
    sized("vnsin", 0xD01A0000, v2, kTab);
    sized("vrexp2", 0xD01C0000, v2, kTab);
    // VFPU7
    sized("vrnds", 0xD0200000, kVd);
    sized("vrndi", 0xD0210000, kVd);
    sized("vrndf1", 0xD0220000, kVd);
    sized("vrndf2", 0xD0230000, kVd);
    sized("vf2h", 0xD0320000, v2);
    sized("vh2f", 0xD0330000, v2);
    sized("vsbz", 0xD0360000, v2);
    sized("vlgb", 0xD0370000, v2);
    sized("vuc2i", 0xD0380000, v2);
    sized("vc2i", 0xD0390000, v2);
    sized("vus2i", 0xD03A0000, v2);
    sized("vs2i", 0xD03B0000, v2);
    sized("vi2uc", 0xD03C0000, v2);
    sized("vi2c", 0xD03D0000, v2);
    sized("vi2us", 0xD03E0000, v2);
    sized("vi2s", 0xD03F0000, v2);
    // VFPU9
    sized("vsrt1", 0xD0400000, v2);
    sized("vsrt2", 0xD0410000, v2);
    sized("vbfy1", 0xD0420000, v2);
    sized("vbfy2", 0xD0430000, v2);
    sized("vocp", 0xD0440000, v2);
    sized("vsocp", 0xD0450000, v2);
    sized("vfad", 0xD0460000, v2);
    sized("vavg", 0xD0470000, v2);
    sized("vsrt3", 0xD0480000, v2);
    sized("vsrt4", 0xD0490000, v2);
    sized("vsgn", 0xD04A0000, v2);
    add("vmfvc", vf, 0xD0500000, kVd | kVs);
    add("vmtvc", vf, 0xD0510000, kVd | kVs);
    sized("vt4444", 0xD0590000, v2);
    sized("vt5551", 0xD05A0000, v2);
    sized("vt5650", 0xD05B0000, v2);
    // VFPU4Jump
    sized("vcst", 0xD0600000, kVd | 0x001F0000u);
    sized("vf2in", 0xD2000000, v2 | 0x001F0000u);
    sized("vf2iz", 0xD2200000, v2 | 0x001F0000u);
    sized("vf2iu", 0xD2400000, v2 | 0x001F0000u);
    sized("vf2id", 0xD2600000, v2 | 0x001F0000u);
    sized("vi2f", 0xD2800000, v2 | 0x001F0000u);
    sized("vcmovt", 0xD2A00000, v2 | 0x00070000u);
    sized("vcmovf", 0xD2A80000, v2 | 0x00070000u);
    sized("vwbn", 0xD3000000, v2 | 0x00FF0000u);
    // VFPU5
    add("vpfxs", vf, 0xDC000000, 0x00FFFFFF);
    add("vpfxt", vf, 0xDD000000, 0x00FFFFFF);
    add("vpfxd", vf, 0xDE000000, 0x00FFFFFF);
    add("viim.s", vf, 0xDF000000, kVt | kImm16);
    add("vfim.s", vf, 0xDF800000, kVt | kImm16);
    // VFPU6 (matrices). vt's top two bits overlap the sub-opcode, all of
    // which select the same instruction.
    for (int i = 1; i < 4; ++i) {
        const SizeVariant &size = kSizes[i];
        add(std::string("vmmul") + size.suffix, vf, 0xF0000000 | size.bits, v3);
        add(std::string("vmscl") + size.suffix, vf, 0xF2000000 | size.bits, v3);
        add(std::string("vmmov") + size.suffix, vf, 0xF3800000 | size.bits, v2);
        add(std::string("vmidt") + size.suffix, vf, 0xF3830000 | size.bits, kVd);
        add(std::string("vmzero") + size.suffix, vf, 0xF3860000 | size.bits, kVd);
        add(std::string("vmone") + size.suffix, vf, 0xF3870000 | size.bits, kVd);
        add(std::string("vrot") + size.suffix, vf, 0xF3A00000 | size.bits, v2 | 0x001F0000u, kTab);
    }
    add("vmmul.q+accurate", vf, 0xF0008080, v3, kSpecMoreAccurateVmmul);
    add("vtfm2.p", vf, 0xF0800080, v3);
    add("vhtfm2.p", vf, 0xF0800000, v3);
    add("vtfm3.t", vf, 0xF1008000, v3);
    add("vhtfm3.t", vf, 0xF1000080, v3);
    add("vtfm4.q", vf, 0xF1808080, v3);
    add("vhtfm4.q", vf, 0xF1808000, v3);
    add("vcrsp.t", vf, 0xF2808000, v3);
    add("vqmul.q", vf, 0xF2808080, v3);
    // COP2 moves.
    add("mfv", vf, 0x48600000, kRt | 0x7Fu);
    add("mfvc", vf, 0x48600080, kRt | 0x0Fu);
    add("mtv", vf, 0x48E00000, kRt | 0x7Fu);
    add("mtvc", vf, 0x48E00080, kRt | 0x0Fu);
    // VFPU loads and stores.
    constexpr std::uint32_t kVmem = kSpecMemory | kSpecVfpuMem;
    add("lv.s", vf, 0xC8000000, kRs | 0x001FFFFF, kVmem | kSpecAlign4);
    add("sv.s", vf, 0xE8000000, kRs | 0x001FFFFF, kVmem | kSpecAlign4);
    add("lv.q", vf, 0xD8000000, kRs | 0x001FFFFD, kVmem | kSpecAlign16);
    add("sv.q", vf, 0xF8000000, kRs | 0x001FFFFD, kVmem | kSpecAlign16);
    add("lvl.q", vf, 0xD4000000, kRs | 0x001FFFFD, kVmem | kSpecAlign4);
    add("lvr.q", vf, 0xD4000002, kRs | 0x001FFFFD, kVmem | kSpecAlign4);
    add("svl.q", vf, 0xF4000000, kRs | 0x001FFFFD, kVmem | kSpecAlign4);
    add("svr.q", vf, 0xF4000002, kRs | 0x001FFFFD, kVmem | kSpecAlign4);
    add("vflush", vf, 0xFC000000, 0x03FFFFFF);
    // COP2 branches on the VFPU condition codes (imm3 in bits 18-20).
    add("bvf", vf, 0x49000000, 0x001CFFFF);
    add("bvt", vf, 0x49010000, 0x001CFFFF);
    add("bvfl", vf, 0x49020000, 0x001CFFFF);
    add("bvtl", vf, 0x49030000, 0x001CFFFF);
    return specs;
}

std::uint32_t random_prefix(Rng &rng, int which) {
    if (rng.chance(55)) return which == 2 ? 0u : 0xE4u;
    return rng.next32() & (which == 2 ? 0x00000FFFu : 0x000FFFFFu);
}

void random_state(Rng &rng, CpuState &s) {
    for (std::uint32_t &reg : s.gpr) reg = rng.random_int_bits();
    s.gpr[0] = 0;
    s.hi = rng.random_int_bits();
    s.lo = rng.random_int_bits();
    for (std::uint32_t &reg : s.fpr) reg = rng.random_float_bits();
    // FCR31: rounding mode, flush bit and condition, as ctc1 would leave them.
    s.fcr31 = rng.next32() & 0x0181FFFFu;
    for (float &reg : s.v) reg = std::bit_cast<float>(rng.random_float_bits());
    s.vfpu_ctrl[0] = random_prefix(rng, 0);
    s.vfpu_ctrl[1] = random_prefix(rng, 1);
    s.vfpu_ctrl[2] = random_prefix(rng, 2);
    s.vfpu_ctrl[3] = rng.next32() & 0x3Fu;
    s.vfpu_ctrl[4] = rng.next32();
    s.vfpu_ctrl[5] = 0;
    s.vfpu_ctrl[6] = 0;
    s.vfpu_ctrl[7] = 0x7772CEABu;
    for (int i = 8; i < 16; ++i) s.vfpu_ctrl[i] = 0x3F800000u | (rng.next32() & 0x000FFFFFu);
    for (std::uint8_t &byte : s.mem) byte = static_cast<std::uint8_t>(rng.next32());
    s.pc = kInitialPc;
    s.mem_faults = 0;
}

std::uint32_t make_op(Rng &rng, const Spec &spec) {
    std::uint32_t op = spec.fixed | (rng.next32() & spec.random_mask);
    if (spec.flags & kSpecExt) {
        const std::uint32_t pos = rng.below(32);
        const std::uint32_t size_minus_one = rng.below(32 - pos);
        op = (op & ~(kRd | kSa)) | (size_minus_one << 11) | (pos << 6);
    } else if (spec.flags & kSpecIns) {
        const std::uint32_t lsb = rng.below(32);
        const std::uint32_t msb = lsb + rng.below(32 - lsb);
        op = (op & ~(kRd | kSa)) | (msb << 11) | (lsb << 6);
    }
    return op;
}

// Points rs at the window so that rs + imm is a valid, aligned address.
void place_memory_access(Rng &rng, const Spec &spec, std::uint32_t &op, CpuState &s) {
    std::uint32_t rs = (op >> 21) & 31;
    if (rs == 0) {
        rs = 1 + rng.below(31);
        op = (op & ~kRs) | (rs << 21);
    }
    std::uint32_t align = 1;
    if (spec.flags & kSpecAlign2) align = 2;
    if (spec.flags & kSpecAlign4) align = 4;
    if (spec.flags & kSpecAlign16) align = 16;
    // Keep 16 bytes of margin on both sides for lwl/lwr and lvl/lvr.q.
    const std::uint32_t slots = (kMemSize - 32) / align;
    const std::uint32_t address = kMemBase + 16 + rng.below(slots) * align;
    const std::uint32_t imm_bits = (spec.flags & kSpecVfpuMem) ? (op & 0xFFFCu) : (op & 0xFFFFu);
    const std::uint32_t imm =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(imm_bits)));
    s.gpr[rs] = address - imm;
}

TestCase make_case(Rng &rng, const Spec &spec, std::uint32_t id) {
    TestCase c;
    c.id = id;
    c.cls = spec.cls;
    c.spec = spec.name;
    random_state(rng, c.initial);
    std::uint32_t op = make_op(rng, spec);
    if (spec.flags & kSpecMemory) place_memory_access(rng, spec, op, c.initial);
    c.ops[0] = op;
    c.num_ops = 1;
    return c;
}

} // namespace

const std::vector<Spec> &all_specs() {
    static const std::vector<Spec> specs = build_specs();
    return specs;
}

std::vector<const Spec *> specs_of(InstrClass cls) {
    std::vector<const Spec *> out;
    for (const Spec &spec : all_specs()) {
        if (spec.cls == cls) out.push_back(&spec);
    }
    return out;
}

const Spec *find_spec(std::string_view name) {
    for (const Spec &spec : all_specs()) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

std::vector<TestCase> generate_cases_for_spec(const Spec &spec, std::uint64_t seed, int count) {
    Rng rng(seed);
    std::vector<TestCase> cases;
    cases.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        cases.push_back(make_case(rng, spec, static_cast<std::uint32_t>(i)));
    return cases;
}

std::vector<TestCase> generate_cases(InstrClass cls, std::uint64_t seed, int count) {
    Rng rng(seed);
    std::vector<TestCase> cases;
    cases.reserve(static_cast<std::size_t>(count));
    if (cls == InstrClass::kVfpuSeq) {
        // One to three vpfx* followed by an arithmetic VFPU op: the prefixes
        // come from real instructions instead of the initial state.
        static const char *const kPrefixes[3] = {"vpfxs", "vpfxt", "vpfxd"};
        std::vector<const Spec *> targets;
        for (const Spec *spec : specs_of(InstrClass::kVfpu)) {
            if (spec->flags & (kSpecMemory | kSpecMoreAccurateVmmul)) continue;
            if (spec->name.rfind("vpfx", 0) == 0 || spec->name.rfind("m", 0) == 0) continue;
            if (spec->name.rfind("bv", 0) == 0) continue;
            targets.push_back(spec);
        }
        for (int i = 0; i < count; ++i) {
            const Spec &target = *targets[rng.below(static_cast<std::uint32_t>(targets.size()))];
            TestCase c = make_case(rng, target, static_cast<std::uint32_t>(i));
            c.cls = InstrClass::kVfpuSeq;
            c.spec = "seq:" + target.name;
            const std::uint32_t op = c.ops[0];
            c.num_ops = 0;
            for (int p = 0; p < 3; ++p) {
                if (!rng.chance(60)) continue;
                const Spec &prefix = *find_spec(kPrefixes[p]);
                c.ops[static_cast<std::size_t>(c.num_ops++)] = make_op(rng, prefix);
            }
            c.ops[static_cast<std::size_t>(c.num_ops++)] = op;
            cases.push_back(std::move(c));
        }
        return cases;
    }
    const std::vector<const Spec *> specs = specs_of(cls);
    for (int i = 0; i < count; ++i) {
        const Spec &spec = *specs[static_cast<std::size_t>(i) % specs.size()];
        cases.push_back(make_case(rng, spec, static_cast<std::uint32_t>(i)));
    }
    return cases;
}

} // namespace mpo::difftest
