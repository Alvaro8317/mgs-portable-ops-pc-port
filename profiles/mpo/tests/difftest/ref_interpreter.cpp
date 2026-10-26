// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference interpreter for the difftest: one non-branch Allegrex
// instruction at a time over a CpuState. VFPU instructions go to mpo_vfpu.
//
// The ALU, Allegrex and FPU semantics are ported from PPSSPP
// Core/MIPS/MIPSInt.cpp @ v1.20.4 (fa50bb19, GPL-2.0-or-later). Upstream
// relies on what x86 does for out-of-range float-to-int casts (INT_MIN); that
// is written out here so the code has no undefined behaviour.

#include <bit>
#include <cmath>
#include <limits>

#include "difftest/difftest.hpp"
#include "vfpu/vfpu.hpp"
#include "vfpu/vfpu_math.hpp"

namespace mpo::difftest {

namespace {

using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

inline float f_of(u32 bits) {
    return std::bit_cast<float>(bits);
}
inline u32 bits_of(float f) {
    return std::bit_cast<u32>(f);
}

// x86 cvttss2si/cvttsd2si: NaN and out-of-range values give INT_MIN.
inline s32 cvt_trunc(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return std::numeric_limits<s32>::min();
    return static_cast<s32>(v);
}

inline u32 sign_extend16(u32 v) {
    return static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(v)));
}
inline u32 sign_extend8(u32 v) {
    return static_cast<u32>(static_cast<s32>(static_cast<std::int8_t>(v)));
}

u32 vfpu_read32(void *user, u32 address) {
    return static_cast<CpuState *>(user)->read32(address);
}
void vfpu_write32(void *user, u32 address, u32 value) {
    static_cast<CpuState *>(user)->write32(address, value);
}

void set_fpu_cond(CpuState &s, bool value) {
    s.fcr31 = (s.fcr31 & ~(1u << 23)) | (value ? 1u << 23 : 0u);
}

bool step_special(CpuState &s, u32 op) {
    u32 *r = s.gpr;
    const u32 rs = (op >> 21) & 31;
    const u32 rt = (op >> 16) & 31;
    const u32 rd = (op >> 11) & 31;
    const u32 sa = (op >> 6) & 31;
    switch (op & 63) {
    case 0: // sll
    case 2: // srl / rotr
    case 3: // sra
    case 4: // sllv
    case 6: // srlv / rotrv
    case 7: // srav
        if (rd == 0) return true;
        switch (op & 63) {
        case 0:
            r[rd] = r[rt] << sa;
            break;
        case 2:
            if (rs == 0)
                r[rd] = r[rt] >> sa;
            else if (rs == 1)
                r[rd] = std::rotr(r[rt], static_cast<int>(sa));
            else
                return false;
            break;
        case 3:
            r[rd] = static_cast<u32>(static_cast<s32>(r[rt]) >> sa);
            break;
        case 4:
            r[rd] = r[rt] << (r[rs] & 0x1F);
            break;
        case 6:
            if (sa == 0)
                r[rd] = r[rt] >> (r[rs] & 0x1F);
            else if (sa == 1)
                r[rd] = std::rotr(r[rt], static_cast<int>(r[rs] & 0x1F));
            else
                return false;
            break;
        case 7:
            r[rd] = static_cast<u32>(static_cast<s32>(r[rt]) >> (r[rs] & 0x1F));
            break;
        }
        return true;
    case 10: // movz
    case 11: // movn
    case 32: // add
    case 33: // addu
    case 34: // sub
    case 35: // subu
    case 36: // and
    case 37: // or
    case 38: // xor
    case 39: // nor
    case 42: // slt
    case 43: // sltu
    case 44: // max
    case 45: // min
        if (rd == 0) return true;
        switch (op & 63) {
        case 10:
            if (r[rt] == 0) r[rd] = r[rs];
            break;
        case 11:
            if (r[rt] != 0) r[rd] = r[rs];
            break;
        case 32:
        case 33:
            r[rd] = r[rs] + r[rt];
            break; // add: no overflow exception upstream
        case 34:
        case 35:
            r[rd] = r[rs] - r[rt];
            break;
        case 36:
            r[rd] = r[rs] & r[rt];
            break;
        case 37:
            r[rd] = r[rs] | r[rt];
            break;
        case 38:
            r[rd] = r[rs] ^ r[rt];
            break;
        case 39:
            r[rd] = ~(r[rs] | r[rt]);
            break;
        case 42:
            r[rd] = static_cast<s32>(r[rs]) < static_cast<s32>(r[rt]);
            break;
        case 43:
            r[rd] = r[rs] < r[rt];
            break;
        case 44:
            r[rd] = static_cast<s32>(r[rs]) > static_cast<s32>(r[rt]) ? r[rs] : r[rt];
            break;
        case 45:
            r[rd] = static_cast<s32>(r[rs]) < static_cast<s32>(r[rt]) ? r[rs] : r[rt];
            break;
        }
        return true;
    case 22: // clz
    case 23: // clo
        if (rd == 0) return true;
        r[rd] = static_cast<u32>(std::countl_zero((op & 63) == 22 ? r[rs] : ~r[rs]));
        return true;
    case 16: // mfhi
        if (rd != 0) r[rd] = s.hi;
        return true;
    case 17:
        s.hi = r[rs];
        return true; // mthi
    case 18:         // mflo
        if (rd != 0) r[rd] = s.lo;
        return true;
    case 19:
        s.lo = r[rs];
        return true; // mtlo
    case 24: {       // mult
        const u64 result = static_cast<u64>(static_cast<s64>(static_cast<s32>(r[rs])) *
                                            static_cast<s64>(static_cast<s32>(r[rt])));
        s.lo = static_cast<u32>(result);
        s.hi = static_cast<u32>(result >> 32);
        return true;
    }
    case 25: { // multu
        const u64 result = static_cast<u64>(r[rs]) * static_cast<u64>(r[rt]);
        s.lo = static_cast<u32>(result);
        s.hi = static_cast<u32>(result >> 32);
        return true;
    }
    case 28:   // madd
    case 29:   // maddu
    case 46:   // msub
    case 47: { // msubu
        const u64 orig = static_cast<u64>(s.lo) | (static_cast<u64>(s.hi) << 32);
        u64 result = 0;
        switch (op & 63) {
        case 28:
            result = orig + static_cast<u64>(static_cast<s64>(static_cast<s32>(r[rs])) *
                                             static_cast<s64>(static_cast<s32>(r[rt])));
            break;
        case 29:
            result = orig + static_cast<u64>(r[rs]) * static_cast<u64>(r[rt]);
            break;
        case 46:
            result = orig - static_cast<u64>(static_cast<s64>(static_cast<s32>(r[rs])) *
                                             static_cast<s64>(static_cast<s32>(r[rt])));
            break;
        case 47:
            result = orig - static_cast<u64>(r[rs]) * static_cast<u64>(r[rt]);
            break;
        }
        s.lo = static_cast<u32>(result);
        s.hi = static_cast<u32>(result >> 32);
        return true;
    }
    case 26: { // div
        const s32 a = static_cast<s32>(r[rs]);
        const s32 b = static_cast<s32>(r[rt]);
        if (a == std::numeric_limits<s32>::min() && b == -1) {
            s.lo = 0x80000000u;
            s.hi = 0xFFFFFFFFu;
        } else if (b != 0) {
            s.lo = static_cast<u32>(a / b);
            s.hi = static_cast<u32>(a % b);
        } else {
            s.lo = a < 0 ? 1u : 0xFFFFFFFFu;
            s.hi = static_cast<u32>(a);
        }
        return true;
    }
    case 27: { // divu
        const u32 a = r[rs];
        const u32 b = r[rt];
        if (b != 0) {
            s.lo = a / b;
            s.hi = a % b;
        } else {
            s.lo = a <= 0xFFFF ? 0xFFFFu : 0xFFFFFFFFu;
            s.hi = a;
        }
        return true;
    }
    default:
        return false;
    }
}

bool step_itype(CpuState &s, u32 op) {
    u32 *r = s.gpr;
    const u32 rs = (op >> 21) & 31;
    const u32 rt = (op >> 16) & 31;
    const u32 uimm = op & 0xFFFF;
    const u32 suimm = sign_extend16(op);
    if (rt == 0) return true;
    switch (op >> 26) {
    case 8:
    case 9:
        r[rt] = r[rs] + suimm;
        break; // addi/addiu
    case 10:
        r[rt] = static_cast<s32>(r[rs]) < static_cast<s32>(suimm);
        break;
    case 11:
        r[rt] = r[rs] < suimm;
        break;
    case 12:
        r[rt] = r[rs] & uimm;
        break;
    case 13:
        r[rt] = r[rs] | uimm;
        break;
    case 14:
        r[rt] = r[rs] ^ uimm;
        break;
    case 15:
        r[rt] = uimm << 16;
        break;
    default:
        return false;
    }
    return true;
}

bool step_mem(CpuState &s, u32 op) {
    u32 *r = s.gpr;
    const u32 rs = (op >> 21) & 31;
    const u32 rt = (op >> 16) & 31;
    const u32 addr = r[rs] + sign_extend16(op);
    // Don't load anything into $zr (stores still happen).
    if (((op >> 29) & 1) == 0 && rt == 0) return true;
    switch (op >> 26) {
    case 32:
        r[rt] = sign_extend8(s.read8(addr));
        break; // lb
    case 33:   // lh
        r[rt] = sign_extend16(s.read8(addr) | (s.read8(addr + 1) << 8));
        break;
    case 35:
        r[rt] = s.read32(addr);
        break; // lw
    case 36:
        r[rt] = s.read8(addr);
        break; // lbu
    case 37:
        r[rt] = s.read8(addr) | (s.read8(addr + 1) << 8);
        break; // lhu
    case 40:
        s.write8(addr, r[rt]);
        break; // sb
    case 41:   // sh
        s.write8(addr, r[rt]);
        s.write8(addr + 1, r[rt] >> 8);
        break;
    case 43:
        s.write32(addr, r[rt]);
        break; // sw
    case 34: { // lwl
        const u32 shift = (addr & 3) * 8;
        const u32 mem = s.read32(addr & 0xFFFFFFFC);
        r[rt] = (r[rt] & (0x00FFFFFFu >> shift)) | (mem << (24 - shift));
        break;
    }
    case 38: { // lwr
        const u32 shift = (addr & 3) * 8;
        const u32 mem = s.read32(addr & 0xFFFFFFFC);
        r[rt] = (r[rt] & (0xFFFFFF00u << (24 - shift))) | (mem >> shift);
        break;
    }
    case 42: { // swl
        const u32 shift = (addr & 3) * 8;
        const u32 mem = s.read32(addr & 0xFFFFFFFC);
        s.write32(addr & 0xFFFFFFFC, (r[rt] >> (24 - shift)) | (mem & (0xFFFFFF00u << shift)));
        break;
    }
    case 46: { // swr
        const u32 shift = (addr & 3) * 8;
        const u32 mem = s.read32(addr & 0xFFFFFFFC);
        s.write32(addr & 0xFFFFFFFC, (r[rt] << shift) | (mem & (0x00FFFFFFu >> (24 - shift))));
        break;
    }
    default:
        return false;
    }
    return true;
}

bool step_special3(CpuState &s, u32 op) {
    u32 *r = s.gpr;
    const u32 rs = (op >> 21) & 31;
    const u32 rt = (op >> 16) & 31;
    const u32 rd = (op >> 11) & 31;
    const u32 pos = (op >> 6) & 31;
    switch (op & 63) {
    case 0x00: { // ext
        if (rt == 0) return true;
        const u32 size = rd + 1;
        const u32 sourcemask = 0xFFFFFFFFu >> (32 - size);
        r[rt] = (r[rs] >> pos) & sourcemask;
        return true;
    }
    case 0x04: { // ins
        if (rt == 0) return true;
        const u32 size = (rd + 1) - pos;
        const u32 sourcemask = 0xFFFFFFFFu >> (32 - size);
        const u32 destmask = sourcemask << pos;
        r[rt] = (r[rt] & ~destmask) | ((r[rs] & sourcemask) << pos);
        return true;
    }
    case 0x18:
    case 0x20: // ALLEGREX0, selected by bits 6-10.
        switch ((op >> 6) & 31) {
        case 2: // wsbh
        case 3: // wsbw
            if (rd == 0) return true;
            switch (op & 0x3FF) {
            case 0xA0:
                r[rd] = ((r[rt] & 0xFF00FF00) >> 8) | ((r[rt] & 0x00FF00FF) << 8);
                break;
            case 0xE0:
                r[rd] = (r[rt] >> 24) | ((r[rt] >> 8) & 0xFF00) | ((r[rt] << 8) & 0xFF0000) |
                        (r[rt] << 24);
                break;
            default:
                break; // Upstream asserts and does nothing (the 0x18 forms).
            }
            return true;
        case 16: // seb
            if (rd != 0) r[rd] = sign_extend8(r[rt]);
            return true;
        case 20: // bitrev
            if (rd != 0) {
                u32 tmp = 0;
                for (int i = 0; i < 32; i++) {
                    if (r[rt] & (1u << i)) tmp |= (0x80000000u >> i);
                }
                r[rd] = tmp;
            }
            return true;
        case 24: // seh
            if (rd != 0) r[rd] = sign_extend16(r[rt]);
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

s32 nan_or_inf_to_int(float f) {
    return vfpu::my_isinf(f) && f < 0.0f ? std::numeric_limits<s32>::min()
                                         : std::numeric_limits<s32>::max();
}

bool step_cop1(CpuState &s, u32 op) {
    u32 *r = s.gpr;
    const u32 rt = (op >> 16) & 31;
    const u32 ft = (op >> 16) & 31;
    const u32 fs = (op >> 11) & 31;
    const u32 fd = (op >> 6) & 31;
    const auto F = [&s](u32 i) { return f_of(s.fpr[i]); };
    const auto set_f = [&s](u32 i, float v) { s.fpr[i] = bits_of(v); };
    const auto set_i = [&s](u32 i, s32 v) { s.fpr[i] = static_cast<u32>(v); };

    switch ((op >> 21) & 31) {
    case 0: // mfc1
        if (rt != 0) r[rt] = s.fpr[fs];
        return true;
    case 2: // cfc1
        if (rt != 0) {
            if (fs == 31)
                r[rt] = s.fcr31;
            else if (fs == 0)
                r[rt] = 0x00003351; // MIPSState::FCR0_VALUE
            else
                r[rt] = 0;
        }
        return true;
    case 4:
        s.fpr[fs] = r[rt];
        return true; // mtc1
    case 6:          // ctc1
        if (fs == 31) s.fcr31 = r[rt] & 0x0181FFFF;
        return true;
    case 20: // W format
        if ((op & 63) == 32) {
            set_f(fd, static_cast<float>(static_cast<s32>(s.fpr[fs]))); // cvt.s.w
            return true;
        }
        return false;
    case 16:
        break; // S format, below.
    default:
        return false;
    }

    const u32 funct = op & 63;
    switch (funct) {
    case 0:
        set_f(fd, vfpu::fadd(F(fs), F(ft)));
        return true; // add.s
    case 1:
        set_f(fd, F(fs) - F(ft));
        return true; // sub.s
    case 2:          // mul.s
        if ((vfpu::my_isinf(F(fs)) && F(ft) == 0.0f) || (vfpu::my_isinf(F(ft)) && F(fs) == 0.0f)) {
            // Must be positive NAN, see PPSSPP #12519.
            s.fpr[fd] = 0x7FC00000;
        } else {
            set_f(fd, vfpu::fmul(F(fs), F(ft)));
        }
        return true;
    case 3:
        set_f(fd, F(fs) / F(ft));
        return true; // div.s
    case 4:
        set_f(fd, std::sqrt(F(fs)));
        return true; // sqrt.s
    case 5:
        set_f(fd, std::fabs(F(fs)));
        return true; // abs.s
    case 6:
        s.fpr[fd] = s.fpr[fs];
        return true; // mov.s
    case 7:
        set_f(fd, -F(fs));
        return true; // neg.s
    case 12:         // round.w.s
    case 13:         // trunc.w.s
    case 14:         // ceil.w.s
    case 15:         // floor.w.s
        if (vfpu::my_isnanorinf(F(fs))) {
            set_i(fd, nan_or_inf_to_int(F(fs)));
            return true;
        }
        switch (funct) {
        case 12:
            set_i(fd, cvt_trunc(std::floor(F(fs) + 0.5f)));
            break;
        case 13:
            if (F(fs) >= 0.0f) {
                s32 value = cvt_trunc(std::floor(F(fs)));
                // Overflow, but it was positive.
                if (value == std::numeric_limits<s32>::min())
                    value = std::numeric_limits<s32>::max();
                set_i(fd, value);
            } else {
                // Overflow happens to be the right value anyway.
                set_i(fd, cvt_trunc(std::ceil(F(fs))));
            }
            break;
        case 14:
            set_i(fd, cvt_trunc(std::ceil(F(fs))));
            break;
        case 15:
            set_i(fd, cvt_trunc(std::floor(F(fs))));
            break;
        }
        return true;
    case 36: // cvt.w.s
        if (vfpu::my_isnanorinf(F(fs))) {
            set_i(fd, nan_or_inf_to_int(F(fs)));
            return true;
        }
        switch (s.fcr31 & 3) {
        case 0:
            set_i(fd, cvt_trunc(vfpu::round_ieee_754(F(fs))));
            break; // RINT_0
        case 1:
            set_i(fd, cvt_trunc(F(fs)));
            break; // CAST_1
        case 2:
            set_i(fd, cvt_trunc(std::ceil(F(fs))));
            break; // CEIL_2
        case 3:
            set_i(fd, cvt_trunc(std::floor(F(fs))));
            break; // FLOOR_3
        }
        return true;
    default:
        break;
    }
    if (funct >= 48) { // c.cond.s
        const float a = F(fs);
        const float b = F(ft);
        const bool unordered = vfpu::my_isnan(a) || vfpu::my_isnan(b);
        bool cond = false;
        switch (op & 0xF) {
        case 0:
        case 8:
            cond = false;
            break; // f, sf
        case 1:
        case 9:
            cond = unordered;
            break; // un, ngle
        case 2:
        case 10:
            cond = !unordered && a == b;
            break; // eq, seq
        case 3:
        case 11:
            cond = a == b || unordered;
            break; // ueq, ngl
        case 4:
        case 12:
            cond = a < b;
            break; // olt, lt
        case 5:
        case 13:
            cond = a < b || unordered;
            break; // ult, nge
        case 6:
        case 14:
            cond = a <= b;
            break; // ole, le
        case 7:
        case 15:
            cond = a <= b || unordered;
            break; // ule, ngt
        }
        set_fpu_cond(s, cond);
        return true;
    }
    return false;
}

bool step_fpu_mem(CpuState &s, u32 op) {
    const u32 ft = (op >> 16) & 31;
    const u32 addr = s.gpr[(op >> 21) & 31] + sign_extend16(op);
    if ((op >> 26) == 49) {
        s.fpr[ft] = s.read32(addr); // lwc1
    } else {
        s.write32(addr, s.fpr[ft]); // swc1
    }
    return true;
}

} // namespace

// Executes one instruction; false if it is not supported here.
static bool reference_step(CpuState &s, u32 op, const vfpu::Options &options) {
    bool ok = false;
    switch (op >> 26) {
    case 0:
        ok = step_special(s, op);
        break;
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        ok = step_itype(s, op);
        break;
    case 17:
        ok = step_cop1(s, op);
        break;
    case 31:
        ok = step_special3(s, op);
        break;
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
    case 40:
    case 41:
    case 42:
    case 43:
    case 46:
        ok = step_mem(s, op);
        break;
    case 49:
    case 57:
        ok = step_fpu_mem(s, op);
        break;
    default: {
        const vfpu::Memory memory{&s, vfpu_read32, vfpu_write32};
        const vfpu::Regs regs{s.v, s.vfpu_ctrl};
        const vfpu::Status status = vfpu::execute(regs, op, s.gpr, memory, options);
        if (status == vfpu::Status::kBranch) {
            // bvf/bvt/bvfl/bvtl (PPSSPP Int_VBranch): leave the next PC.
            const u32 target = s.pc + 4 + (sign_extend16(op) << 2);
            const bool likely = ((op >> 17) & 1) != 0;
            s.pc = vfpu::branch_taken(regs, op) ? target : s.pc + (likely ? 8u : 4u);
            return true;
        }
        ok = status == vfpu::Status::kOk;
        break;
    }
    }
    if (ok) s.pc += 4;
    return ok;
}

namespace {

class ReferenceInterpreter final : public Executor {
public:
    std::string name() const override { return "mpo"; }

    bool run(std::span<const TestCase> cases, std::span<CpuState> out,
             std::span<std::uint8_t> supported, std::string *) override {
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const TestCase &c = cases[i];
            vfpu::Options options;
            const Spec *spec = find_spec(c.spec);
            options.more_accurate_vmmul = spec && (spec->flags & kSpecMoreAccurateVmmul);
            out[i] = c.initial;
            supported[i] = 1;
            for (int k = 0; k < c.num_ops; ++k) {
                if (!reference_step(out[i], c.ops[static_cast<std::size_t>(k)], options)) {
                    supported[i] = 0;
                    break;
                }
            }
        }
        return true;
    }
};

} // namespace

std::unique_ptr<Executor> make_reference_interpreter() {
    return std::make_unique<ReferenceInterpreter>();
}

} // namespace mpo::difftest
