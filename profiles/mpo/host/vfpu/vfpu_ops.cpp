// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/MIPS/MIPSIntVFPU.cpp and Core/MIPS/MIPSVFPUUtils.cpp
// (ReadVector/WriteVector/ReadMatrix/WriteMatrix, GetVFPUCtrlMask) @ v1.20.4
// (fa50bb19, GPL-2.0-or-later). The decoder follows Core/MIPS/MIPSTables.cpp.
//
// Changes from upstream, none of which alter results for defined behaviour:
// - The state is passed explicitly (Regs) instead of through currentMIPS, and
//   the PC is left to the caller.
// - Every local array is value-initialised. Upstream reads some uninitialised
//   lanes when an odd prefix swizzles past the vector size; the golden
//   generator builds PPSSPP with -ftrivial-auto-var-init=zero so both sides
//   agree on those lanes.
// - Integer views of float lanes use std::bit_cast instead of pointer casts.
// - vscmp computes its NaN-path difference with 32-bit wrap-around, which is
//   what upstream's signed overflow does on every compiler it ships with.
// - Float additions and multiplications go through fadd()/fmul(), which pin
//   the NaN that comes out to what x86 SSE gives with the operands in source
//   order. A plain a + b would let the compiler commute the operands and
//   change NaN payloads between builds (see vfpu_math.hpp).
// - USE_VFPU_DOT and USE_VFPU_SQRT are false upstream; those branches are
//   kept only where a compat flag can enable them (vmmul).

#include "vfpu/vfpu.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "vfpu/vfpu_math.hpp"

namespace mpo::vfpu {
namespace {

using u32 = std::uint32_t;
using s32 = std::int32_t;

constexpr float kInfinity = std::numeric_limits<float>::infinity();

inline int vd_of(u32 op) {
    return static_cast<int>(op & 0x7F);
}
inline int vs_of(u32 op) {
    return static_cast<int>((op >> 8) & 0x7F);
}
inline int vt_of(u32 op) {
    return static_cast<int>((op >> 16) & 0x7F);
}
inline int rs_of(u32 op) {
    return static_cast<int>((op >> 21) & 0x1F);
}
inline int rt_of(u32 op) {
    return static_cast<int>((op >> 16) & 0x1F);
}

inline u32 fbits(float f) {
    return std::bit_cast<u32>(f);
}
inline float bitsf(u32 u) {
    return std::bit_cast<float>(u);
}

// What PPSSPP's (int) casts do on x86 (cvttsd2si): NaN and out-of-range
// values give INT_MIN. In range it is the usual truncation.
inline s32 trunc_to_s32_x86(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return std::numeric_limits<s32>::min();
    return static_cast<s32>(v);
}

inline s32 sign_extend16(u32 value) {
    return static_cast<s32>(static_cast<std::int16_t>(value));
}

inline int num_elements(VectorSize sz) {
    return static_cast<int>(sz);
}

inline u32 swizzle(int x, int y, int z, int w) {
    return u32(x << 0) | u32(y << 2) | u32(z << 4) | u32(w << 6);
}
inline u32 mask4(int x, int y, int z, int w) {
    return u32(x << 0) | u32(y << 1) | u32(z << 2) | u32(w << 3);
}
inline u32 any_swizzle() {
    return 0x000000FF;
}
inline u32 abs_bits(int x, int y, int z, int w) {
    return mask4(x, y, z, w) << 8;
}
inline u32 negate_bits(int x, int y, int z, int w) {
    return mask4(x, y, z, w) << 16;
}

enum class Const { kNone = -1, kZero, kOne, kTwo, kHalf, kThree, kThird, kFourth, kSixth };

inline u32 make_constants(Const x, Const y, Const z, Const w) {
    u32 result = 0;
    const auto add = [&result](Const c, int lane) {
        if (c == Const::kNone) return;
        const u32 value = static_cast<u32>(c);
        // This sets the constant flag and the swizzle/abs flags for the right constant.
        result |= ((value & 3) << (lane * 2)) | ((value & 4) << (6 + lane)) | (1u << (12 + lane));
    };
    add(x, 0);
    add(y, 1);
    add(z, 2);
    add(w, 3);
    return result;
}

inline u32 rewrite_prefix(const Regs &r, int ctrl, u32 remove, u32 add) {
    return (r.ctrl[ctrl] & ~remove) | add;
}

inline bool write_mask(const Regs &r, int i) {
    return ((r.ctrl[kCtrlDPrefix] >> (8 + i)) & 1) != 0;
}

// Preserves NaN in first param, takes sign of equal second param.
inline float nanmax(float f, float cst) {
    return f <= cst ? cst : f;
}
inline float nanmin(float f, float cst) {
    return f >= cst ? cst : f;
}
inline float nanclamp(float f, float lower, float upper) {
    return nanmin(nanmax(f, lower), upper);
}

// s[0]*t[0] + s[1]*t[1] + s[2]*t[2] + s[3]*t[3], left to right.
inline float dot4(const float *s, const float *t) {
    return fadd(fadd(fadd(fmul(s[0], t[0]), fmul(s[1], t[1])), fmul(s[2], t[2])), fmul(s[3], t[3]));
}

void apply_prefix_st(float *r, u32 data, VectorSize size, float invalid = 0.0f) {
    // Check for no prefix.
    if (data == 0xE4) return;

    const int n = num_elements(size);
    float orig[4]{invalid, invalid, invalid, invalid};
    static const float kConstants[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};

    for (int i = 0; i < n; i++) orig[i] = r[i];

    for (int i = 0; i < n; i++) {
        const int regnum = (data >> (i * 2)) & 3;
        const int abs = (data >> (8 + i)) & 1;
        const int negate = (data >> (16 + i)) & 1;
        const int constants = (data >> (12 + i)) & 1;

        if (!constants) {
            r[i] = orig[regnum];
            if (abs) r[i] = bitsf(fbits(r[i]) & 0x7FFFFFFF);
        } else {
            r[i] = kConstants[regnum + (abs << 2)];
        }
        if (negate) r[i] = bitsf(fbits(r[i]) ^ 0x80000000);
    }
}

inline void apply_swizzle_s(const Regs &r, float *v, VectorSize size, float invalid = 0.0f) {
    apply_prefix_st(v, r.ctrl[kCtrlSPrefix], size, invalid);
}

inline void apply_swizzle_t(const Regs &r, float *v, VectorSize size, float invalid = 0.0f) {
    apply_prefix_st(v, r.ctrl[kCtrlTPrefix], size, invalid);
}

void apply_prefix_d(const Regs &r, float *v, VectorSize size, bool only_write_mask = false) {
    const u32 data = r.ctrl[kCtrlDPrefix];
    if (!data || only_write_mask) return;
    const int n = num_elements(size);
    for (int i = 0; i < n; i++) {
        const int sat = (data >> (i * 2)) & 3;
        if (sat == 1)
            v[i] = vfpu_clamp(v[i], 0.0f, 1.0f);
        else if (sat == 3)
            v[i] = vfpu_clamp(v[i], -1.0f, 1.0f);
    }
}

void retain_invalid_swizzle_st(const Regs &r, float *d, VectorSize sz) {
    // Somehow it's like a supernan, maybe wires through to zero? Doesn't
    // apply to all ops.
    const u32 s_prefix = r.ctrl[kCtrlSPrefix];
    const u32 t_prefix = r.ctrl[kCtrlTPrefix];
    const int n = num_elements(sz);
    for (int i = 0; i < n; i++) {
        const int swizzle_s = (s_prefix >> (i + i)) & 3;
        const int swizzle_t = (t_prefix >> (i + i)) & 3;
        const int const_s = (s_prefix >> (12 + i)) & 1;
        const int const_t = (t_prefix >> (12 + i)) & 1;
        if ((swizzle_s >= n && !const_s) || (swizzle_t >= n && !const_t)) d[i] = 0.0f;
    }
}

// The D prefix only applies to the last element (mask and sat).
inline void keep_last_d_prefix(const Regs &r, int last) {
    const u32 lastmask = (r.ctrl[kCtrlDPrefix] & (1 << 8)) << last;
    const u32 lastsat = (r.ctrl[kCtrlDPrefix] & 3) << (last + last);
    r.ctrl[kCtrlDPrefix] = lastmask | lastsat;
}

void get_vector_regs(u32 regs[4], VectorSize n, int vector_reg) {
    const int mtx = (vector_reg >> 2) & 7;
    const int col = vector_reg & 3;
    int row = 0;
    int length = 0;
    int transpose = (vector_reg >> 5) & 1;
    switch (n) {
    case kSingle:
        transpose = 0;
        row = (vector_reg >> 5) & 3;
        length = 1;
        break;
    case kPair:
        row = (vector_reg >> 5) & 2;
        length = 2;
        break;
    case kTriple:
        row = (vector_reg >> 6) & 1;
        length = 3;
        break;
    case kQuad:
        row = (vector_reg >> 5) & 2;
        length = 4;
        break;
    }
    for (int i = 0; i < length; i++) {
        int index = mtx * 4;
        if (transpose)
            index += ((row + i) & 3) + col * 32;
        else
            index += col + ((row + i) & 3) * 32;
        regs[i] = static_cast<u32>(index);
    }
}

bool get_ctrl_mask(int reg, u32 *mask) {
    switch (reg) {
    case kCtrlSPrefix:
    case kCtrlTPrefix:
        *mask = 0x000FFFFF;
        return true;
    case kCtrlDPrefix:
        *mask = 0x00000FFF;
        return true;
    case kCtrlCc:
        *mask = 0x0000003F;
        return true;
    case kCtrlInf4:
        *mask = 0xFFFFFFFF;
        return true;
    case kCtrlRsv5:
    case kCtrlRsv6:
    case kCtrlRev:
        // Don't change anything, these regs are read only.
        return false;
    default:
        if (reg >= kCtrlRcx0 && reg <= kCtrlRcx7) {
            *mask = 0x3FFFFFFF;
            return true;
        }
        return false;
    }
}

const char *size_suffix(VectorSize sz) {
    switch (sz) {
    case kSingle:
        return ".s";
    case kPair:
        return ".p";
    case kTriple:
        return ".t";
    case kQuad:
        return ".q";
    }
    return "";
}

// cst_constants from PPSSPP Core/MIPS/MIPS.cpp, as the bit patterns that
// PPSSPP computes on x86-64 Linux (glibc libm). Upstream evaluates sqrtf and
// logf at startup; fixing the bits keeps the result identical on every host
// libm. Entries 20..31 are zero.
constexpr u32 kCstBits[32] = {
    0x00000000, // 0
    0x7F7FFFFF, // FLT_MAX ("verified on real PSP")
    0x3FB504F3, // sqrt(2)
    0x3F3504F3, // sqrt(1/2)
    0x3F906EBA, // 2/sqrt(pi): 2.0f / sqrtf((float)M_PI), one ulp below the rounded value
    0x3F22F983, // 2/pi
    0x3EA2F983, // 1/pi
    0x3F490FDB, // pi/4
    0x3FC90FDB, // pi/2
    0x40490FDB, // pi
    0x402DF854, // e
    0x3FB8AA3B, // log2(e)
    0x3EDE5BD9, // log10(e)
    0x3F317218, // ln(2)
    0x40135D8E, // ln(10)
    0x40C90FDB, // 2*pi
    0x3F060A92, // pi/6
    0x3E9A209B, // log10(2)
    0x40549A78, // log2(10)
    0x3F5DB3D7, // sqrt(3)/2
};

} // namespace

// --- Register access -------------------------------------------------------

void read_vector(const Regs &r, float *rd, VectorSize size, int reg) {
    int row = 0;
    int length = 0;
    switch (size) {
    case kSingle:
        rd[0] = r.v[voffset(reg)];
        return;
    case kPair:
        row = (reg >> 5) & 2;
        length = 2;
        break;
    case kTriple:
        row = (reg >> 6) & 1;
        length = 3;
        break;
    case kQuad:
        row = (reg >> 5) & 2;
        length = 4;
        break;
    }
    const int transpose = (reg >> 5) & 1;
    const int mtx = ((reg << 2) & 0x70);
    const int col = reg & 3;
    if (transpose) {
        const int base = mtx + col;
        for (int i = 0; i < length; i++) rd[i] = r.v[base + ((row + i) & 3) * 4];
    } else {
        const int base = mtx + col * 4;
        for (int i = 0; i < length; i++) rd[i] = r.v[base + ((row + i) & 3)];
    }
}

void write_vector(const Regs &r, const float *rd, VectorSize size, int reg) {
    int row = 0;
    int length = 0;
    switch (size) {
    case kSingle:
        if (!write_mask(r, 0)) r.v[voffset(reg)] = rd[0];
        return;
    case kPair:
        row = (reg >> 5) & 2;
        length = 2;
        break;
    case kTriple:
        row = (reg >> 6) & 1;
        length = 3;
        break;
    case kQuad:
        row = (reg >> 5) & 2;
        length = 4;
        break;
    }
    const int mtx = ((reg << 2) & 0x70);
    const int col = reg & 3;
    const bool transpose = ((reg >> 5) & 1) != 0;
    if (transpose) {
        const int base = mtx + col;
        for (int i = 0; i < length; i++) {
            if (!write_mask(r, i)) r.v[base + ((row + i) & 3) * 4] = rd[i];
        }
    } else {
        const int base = mtx + col * 4;
        for (int i = 0; i < length; i++) {
            if (!write_mask(r, i)) r.v[base + ((row + i) & 3)] = rd[i];
        }
    }
}

void read_matrix(const Regs &r, float *rd, VectorSize size, int reg) {
    int row = 0;
    int side = 0;
    int transpose = (reg >> 5) & 1;
    switch (size) {
    case kSingle:
        transpose = 0;
        row = (reg >> 5) & 3;
        side = 1;
        break;
    case kPair:
        row = (reg >> 5) & 2;
        side = 2;
        break;
    case kTriple:
        row = (reg >> 6) & 1;
        side = 3;
        break;
    case kQuad:
        row = (reg >> 5) & 2;
        side = 4;
        break;
    }
    const int mtx = (reg >> 2) & 7;
    const int col = reg & 3;
    const float *v = r.v + static_cast<std::size_t>(mtx) * 16;
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            const int index = transpose ? ((row + i) & 3) * 4 + ((col + j) & 3)
                                        : ((col + j) & 3) * 4 + ((row + i) & 3);
            rd[j * 4 + i] = v[index];
        }
    }
}

void write_matrix(const Regs &r, const float *rd, VectorSize size, int reg) {
    const int mtx = (reg >> 2) & 7;
    const int col = reg & 3;
    int row = 0;
    int side = 0;
    int transpose = (reg >> 5) & 1;
    switch (size) {
    case kSingle:
        transpose = 0;
        row = (reg >> 5) & 3;
        side = 1;
        break;
    case kPair:
        row = (reg >> 5) & 2;
        side = 2;
        break;
    case kTriple:
        row = (reg >> 6) & 1;
        side = 3;
        break;
    case kQuad:
        row = (reg >> 5) & 2;
        side = 4;
        break;
    }
    // Upstream reports "Write mask used with vfpu matrix instruction" here.
    float *v = r.v + static_cast<std::size_t>(mtx) * 16;
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            if (j != side - 1 || !write_mask(r, i)) {
                const int index = transpose ? ((row + i) & 3) * 4 + ((col + j) & 3)
                                            : ((col + j) & 3) * 4 + ((row + i) & 3);
                v[index] = rd[j * 4 + i];
            }
        }
    }
}

// --- Instructions ----------------------------------------------------------

void op_vpfx(const Regs &r, u32 op) {
    u32 data = op & 0x000FFFFF;
    const int regnum = (op >> 24) & 3;
    if (regnum == kCtrlDPrefix) data &= 0x00000FFF;
    r.ctrl[kCtrlSPrefix + regnum] = data;
}

void op_svq(const Regs &r, u32 op, const u32 *gpr, const Memory &memory) {
    const s32 imm = sign_extend16(op & 0xFFFC);
    const int vt = static_cast<int>(((op >> 16) & 0x1F) | ((op & 1) << 5));
    const u32 addr = gpr[rs_of(op)] + static_cast<u32>(imm);

    switch (op >> 26) {
    case 53: { // lvl.q/lvr.q
        float d[4]{};
        read_vector(r, d, kQuad, vt);
        const int offset = (addr >> 2) & 3;
        if ((op & 2) == 0) {
            // It's an LVL
            for (int i = 0; i < offset + 1; i++)
                d[3 - i] = bitsf(memory.read32(memory.user, addr - 4 * static_cast<u32>(i)));
        } else {
            // It's an LVR
            for (int i = 0; i < (3 - offset) + 1; i++)
                d[i] = bitsf(memory.read32(memory.user, addr + 4 * static_cast<u32>(i)));
        }
        write_vector(r, d, kQuad, vt);
        break;
    }
    case 54: { // lv.q
        float d[4]{};
        for (int i = 0; i < 4; i++)
            d[i] = bitsf(memory.read32(memory.user, addr + 4 * static_cast<u32>(i)));
        write_vector(r, d, kQuad, vt);
        break;
    }
    case 61: { // svl.q/svr.q
        float d[4]{};
        read_vector(r, d, kQuad, vt);
        const int offset = (addr >> 2) & 3;
        if ((op & 2) == 0) {
            // It's an SVL
            for (int i = 0; i < offset + 1; i++)
                memory.write32(memory.user, addr - static_cast<u32>(i) * 4, fbits(d[3 - i]));
        } else {
            // It's an SVR
            for (int i = 0; i < (3 - offset) + 1; i++)
                memory.write32(memory.user, addr + 4 * static_cast<u32>(i), fbits(d[i]));
        }
        break;
    }
    case 62: { // sv.q
        float d[4]{};
        read_vector(r, d, kQuad, vt);
        for (int i = 0; i < 4; i++)
            memory.write32(memory.user, addr + 4 * static_cast<u32>(i), fbits(d[i]));
        break;
    }
    default:
        break;
    }
}

void op_sv(const Regs &r, u32 op, const u32 *gpr, const Memory &memory) {
    const s32 imm = sign_extend16(op & 0xFFFC);
    const int vt = static_cast<int>(((op >> 16) & 0x1F) | ((op & 3) << 5));
    const u32 addr = gpr[rs_of(op)] + static_cast<u32>(imm);
    switch (op >> 26) {
    case 50: // lv.s
        r.v[voffset(vt)] = bitsf(memory.read32(memory.user, addr));
        break;
    case 58: // sv.s
        memory.write32(memory.user, addr, fbits(r.v[voffset(vt)]));
        break;
    default:
        break;
    }
}

void op_vmatrix_init(const Regs &r, u32 op) {
    static const float kIdt[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const float kZero[16] = {};
    static const float kOne[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    const float *m = nullptr;
    switch ((op >> 16) & 0xF) {
    case 3:
        m = kIdt;
        break; // vmidt
    case 6:
        m = kZero;
        break; // vmzero
    case 7:
        m = kOne;
        break; // vmone
    default:
        eat_prefixes(r);
        return;
    }

    // The S prefix generates constants, but only for the final (possibly transposed) row.
    if (r.ctrl[kCtrlSPrefix] & 0xF0F00) {
        float prefixed[16];
        std::memcpy(prefixed, m, sizeof(prefixed));

        const int off = static_cast<int>(sz) - 1;
        const u32 remove = any_swizzle();
        u32 add = 0;
        switch ((op >> 16) & 0xF) {
        case 3: {
            const Const x = off == 0 ? Const::kOne : Const::kZero;
            const Const y = off == 1 ? Const::kOne : Const::kZero;
            const Const z = off == 2 ? Const::kOne : Const::kZero;
            const Const w = off == 3 ? Const::kOne : Const::kZero;
            add = make_constants(x, y, z, w);
            break;
        }
        case 6:
            add = make_constants(Const::kZero, Const::kZero, Const::kZero, Const::kZero);
            break;
        case 7:
            add = make_constants(Const::kOne, Const::kOne, Const::kOne, Const::kOne);
            break;
        default:
            break;
        }
        apply_prefix_st(&prefixed[off * 4], rewrite_prefix(r, kCtrlSPrefix, remove, add), kQuad);
        write_matrix(r, prefixed, sz, vd);
    } else {
        // Write mask applies to the final (maybe transposed) row. Sat causes hang.
        write_matrix(r, m, sz, vd);
    }
    eat_prefixes(r);
}

void op_vvector_init(const Regs &r, u32 op) {
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    float d[4]{};
    Const constant = Const::kZero;
    switch ((op >> 16) & 0xF) {
    case 6:
        constant = Const::kZero;
        break; // vzero
    case 7:
        constant = Const::kOne;
        break; // vone
    default:
        eat_prefixes(r);
        return;
    }
    // The S prefix generates constants, but negate is still respected.
    const u32 add = make_constants(constant, constant, constant, constant);
    apply_prefix_st(d, rewrite_prefix(r, kCtrlSPrefix, any_swizzle(), add), sz);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_viim(const Regs &r, u32 op) {
    const int vt = vt_of(op);
    const s32 imm = sign_extend16(op & 0xFFFF);
    const std::uint16_t uimm16 = static_cast<std::uint16_t>(op & 0xFFFF);
    float f[1]{};
    const int type = (op >> 23) & 7;
    if (type == 6) {
        f[0] = static_cast<float>(imm); // viim
    } else if (type == 7) {
        f[0] = float16_to_float32(uimm16); // vfim
    } else {
        f[0] = 0;
    }
    apply_prefix_d(r, f, kSingle);
    write_vector(r, f, kSingle, vt);
    eat_prefixes(r);
}

void op_vidt(const Regs &r, u32 op) {
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    float f[4]{};
    // The S prefix generates constants, but negate is still respected.
    const int offmask = sz == kQuad || sz == kTriple ? 3 : 1;
    const int off = vd & offmask;
    // If it's a pair, the identity starts in a different position.
    const Const x = off == (0 & offmask) ? Const::kOne : Const::kZero;
    const Const y = off == (1 & offmask) ? Const::kOne : Const::kZero;
    const Const z = off == (2 & offmask) ? Const::kOne : Const::kZero;
    const Const w = off == (3 & offmask) ? Const::kOne : Const::kZero;
    apply_prefix_st(f, rewrite_prefix(r, kCtrlSPrefix, any_swizzle(), make_constants(x, y, z, w)),
                    sz);
    apply_prefix_d(r, f, sz);
    write_vector(r, f, sz, vd);
    eat_prefixes(r);
}

void op_vmmul(const Regs &r, u32 op, bool more_accurate_vmmul) {
    float s[16]{}, t[16]{}, d[16]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    const int n = static_cast<int>(sz);

    read_matrix(r, s, sz, vs);
    read_matrix(r, t, sz, vt);

    for (int a = 0; a < n; a++) {
        for (int b = 0; b < n; b++) {
            float sum = 0.0f;
            if (a == n - 1 && b == n - 1) {
                // S and T prefixes work on the final (or maybe first, in reverse?) dot.
                apply_swizzle_s(r, &s[b * 4], kQuad);
                apply_swizzle_t(r, &t[a * 4], kQuad);
            }
            if (more_accurate_vmmul) {
                u32 bits = fbits(vfpu_dot(&s[b * 4], &t[a * 4]));
                if (my_isnan(bitsf(bits))) {
                    bits = 0x7F800001;
                } else if ((bits & 0x7F800000) == 0) {
                    bits &= 0xFF800000;
                }
                sum = bitsf(bits);
            } else {
                const int count = (a == n - 1 && b == n - 1) ? 4 : n;
                for (int c = 0; c < count; c++) sum = fadd(sum, fmul(s[b * 4 + c], t[a * 4 + c]));
            }
            d[a * 4 + b] = sum;
        }
    }

    // The D prefix applies ONLY to the final element, but sat does work.
    keep_last_d_prefix(r, n - 1);
    apply_prefix_d(r, &d[4 * (n - 1)], kQuad, false);
    write_matrix(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vmscl(const Regs &r, u32 op) {
    float s[16]{}, t[4]{}, d[16]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    const int n = static_cast<int>(sz);

    read_matrix(r, s, sz, vs);
    read_vector(r, t, kSingle, vt);

    for (int a = 0; a < n - 1; a++) {
        for (int b = 0; b < n; b++) d[a * 4 + b] = fmul(s[a * 4 + b], t[0]);
    }

    // S prefix applies to the last row.
    apply_swizzle_s(r, &s[(n - 1) * 4], kQuad);
    // T prefix applies only for the last row, and is used per element.
    // This is like vscl, but instead of zzzz it uses xxxx.
    const int tlane = (vt >> 5) & 3;
    t[tlane] = t[0];
    apply_prefix_st(
        t, rewrite_prefix(r, kCtrlTPrefix, any_swizzle(), swizzle(tlane, tlane, tlane, tlane)),
        kQuad);

    for (int b = 0; b < n; b++) d[(n - 1) * 4 + b] = fmul(s[(n - 1) * 4 + b], t[b]);

    // The D prefix is applied to the last row.
    apply_prefix_d(r, &d[(n - 1) * 4], kQuad);
    write_matrix(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vmmov(const Regs &r, u32 op) {
    float s[16]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_matrix(r, s, sz, vs);
    // S and D prefixes are applied to the last row.
    const int off = static_cast<int>(sz) - 1;
    apply_swizzle_s(r, &s[off * 4], kQuad);
    apply_prefix_d(r, &s[off * 4], kQuad);
    write_matrix(r, s, sz, vd);
    eat_prefixes(r);
}

void op_vflush(const Regs &r, u32 op) {
    // Anything with 0xFC000000 is a nop, but only 0xFFFF0000 retains prefixes.
    if ((op & 0xFFFF0000) != 0xFFFF0000) eat_prefixes(r);
}

void op_vv2op(const Regs &r, u32 op) {
    float s[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int optype = (op >> 16) & 0x1F;
    const VectorSize sz = vec_size(op);
    const int n = static_cast<int>(sz);
    read_vector(r, s, sz, vs);
    // Some of these are prefix hacks (affects constants, etc.)
    switch (optype) {
    case 1:
        apply_prefix_st(s, rewrite_prefix(r, kCtrlSPrefix, 0, abs_bits(1, 1, 1, 1)), sz);
        break;
    case 2:
        apply_prefix_st(s, rewrite_prefix(r, kCtrlSPrefix, 0, negate_bits(1, 1, 1, 1)), sz);
        break;
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
        // Similar to vdiv. Some of the behavior using the invalid constant is iffy.
        apply_swizzle_s(r, &s[n - 1], kSingle, kInfinity);
        break;
    case 24:
    case 26:
        // Similar to above, but also ignores negate.
        apply_prefix_st(&s[n - 1], rewrite_prefix(r, kCtrlSPrefix, negate_bits(1, 0, 0, 0), 0),
                        kSingle, -kInfinity);
        break;
    case 28:
        // Similar to above, but also ignores negate.
        apply_prefix_st(&s[n - 1], rewrite_prefix(r, kCtrlSPrefix, negate_bits(1, 0, 0, 0), 0),
                        kSingle, kInfinity);
        break;
    default:
        apply_swizzle_s(r, s, sz);
        break;
    }
    for (int i = 0; i < n; i++) {
        switch (optype) {
        case 0:
            d[i] = s[i];
            break; // vmov
        case 1:
            d[i] = s[i];
            break; // vabs (prefix)
        case 2:
            d[i] = s[i];
            break; // vneg (prefix)
        // vsat0 changes -0.0 to +0.0, both retain NAN.
        case 4:
            if (s[i] <= 0)
                d[i] = 0;
            else
                d[i] = s[i] > 1.0f ? 1.0f : s[i];
            break; // vsat0
        case 5:
            if (s[i] < -1.0f)
                d[i] = -1.0f;
            else
                d[i] = s[i] > 1.0f ? 1.0f : s[i];
            break; // vsat1
        case 16:
            d[i] = vfpu_rcp(s[i]);
            break; // vrcp
        case 17:
            d[i] = ppsspp_vrsq(s[i]);
            break; // vrsq (USE_VFPU_SQRT is false)
        case 18:
            d[i] = vfpu_sin(s[i]);
            break; // vsin
        case 19:
            d[i] = vfpu_cos(s[i]);
            break; // vcos
        case 20:
            d[i] = vfpu_exp2(s[i]);
            break; // vexp2
        case 21:
            d[i] = vfpu_log2(s[i]);
            break; // vlog2
        case 22:
            d[i] = ppsspp_vsqrt(s[i]);
            break; // vsqrt (USE_VFPU_SQRT is false)
        case 23:
            d[i] = vfpu_asin(s[i]);
            break; // vasin
        case 24:
            d[i] = -vfpu_rcp(s[i]);
            break; // vnrcp
        case 26:
            d[i] = -vfpu_sin(s[i]);
            break; // vnsin
        case 28:
            d[i] = vfpu_rexp2(s[i]);
            break; // vrexp2
        default:
            break;
        }
    }
    // vsat1 is a prefix hack, so 0:1 doesn't apply. Others don't process sat at all.
    switch (optype) {
    case 5:
        apply_prefix_d(r, d, sz, true);
        break;
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 26:
    case 28:
        // Only the last element gets the mask applied.
        keep_last_d_prefix(r, n - 1);
        apply_prefix_d(r, d, sz);
        break;
    default:
        apply_prefix_d(r, d, sz);
    }
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vocp(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);

    // S prefix forces the negate flags.
    apply_prefix_st(s, rewrite_prefix(r, kCtrlSPrefix, 0, negate_bits(1, 1, 1, 1)), sz);

    // T prefix forces constants on and regnum to 1. That means negate still
    // works, and abs activates a different constant.
    apply_prefix_st(
        t,
        rewrite_prefix(r, kCtrlTPrefix, any_swizzle(),
                       make_constants(Const::kOne, Const::kOne, Const::kOne, Const::kOne)),
        sz);

    for (int i = 0; i < num_elements(sz); i++) {
        // Always positive NaN. Note that s is always negated from the registers.
        d[i] = my_isnan(s[i]) ? std::fabs(s[i]) : fadd(t[i], s[i]);
    }
    retain_invalid_swizzle_st(r, d, sz);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vsocp(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    VectorSize out_size = kQuad;
    if (sz == kSingle) out_size = kPair; // GetDoubleVectorSizeSafe; triple/quad -> quad.
    read_vector(r, s, sz, vs);

    // S prefix forces negate in even/odd and xxyy swizzle. abs works, and
    // applies to final position (not source.)
    apply_prefix_st(s,
                    rewrite_prefix(r, kCtrlSPrefix, any_swizzle() | negate_bits(1, 1, 1, 1),
                                   swizzle(0, 0, 1, 1) | negate_bits(1, 0, 1, 0)),
                    out_size);

    // T prefix forces constants on and regnum to 1, 0, 1, 0.
    apply_prefix_st(
        t,
        rewrite_prefix(r, kCtrlTPrefix, any_swizzle(),
                       make_constants(Const::kOne, Const::kZero, Const::kOne, Const::kZero)),
        out_size);

    // Essentially D prefix saturation is forced.
    d[0] = nanclamp(fadd(t[0], s[0]), 0.0f, 1.0f);
    d[1] = nanclamp(fadd(t[1], s[1]), 0.0f, 1.0f);
    if (out_size == kQuad) {
        d[2] = nanclamp(fadd(t[2], s[2]), 0.0f, 1.0f);
        d[3] = nanclamp(fadd(t[3], s[3]), 0.0f, 1.0f);
    }
    apply_prefix_d(r, d, sz, true);
    write_vector(r, d, out_size, vd);
    eat_prefixes(r);
}

void op_vsgn(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);

    // Not sure who would do this, but using abs/neg allows a compare against 3 or -3.
    apply_prefix_st(
        t,
        rewrite_prefix(r, kCtrlTPrefix, any_swizzle(),
                       make_constants(Const::kZero, Const::kZero, Const::kZero, Const::kZero)),
        sz);

    const int n = num_elements(sz);
    if (n < 4) {
        // Compare with a swizzled value out of bounds always produces 0.
        std::memcpy(&s[n], &t[n], sizeof(float) * static_cast<std::size_t>(4 - n));
    }
    apply_swizzle_s(r, s, kQuad);

    for (int i = 0; i < n; i++) {
        const float diff = s[i] - t[i];
        // To handle NaNs correctly, we do this with integer hackery
        const u32 val = fbits(diff);
        if (val == 0 || val == 0x80000000)
            d[i] = 0.0f;
        else if ((val >> 31) == 0)
            d[i] = 1.0f;
        else
            d[i] = -1.0f;
    }
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vf2i(const Regs &r, u32 op) {
    float s[4]{};
    u32 d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int imm = (op >> 16) & 0x1F;
    const float mult = static_cast<float>(1UL << imm);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    // Negate, abs, and constants apply as you'd expect to the bits.
    apply_swizzle_s(r, s, sz);
    for (int i = 0; i < num_elements(sz); i++) {
        if (my_isnan(s[i])) {
            d[i] = 0x7FFFFFFF;
            continue;
        }
        const double sv = s[i] * mult; // (float)0x7fffffff == (float)0x80000000
        // Cap/floor it to 0x7fffffff / 0x80000000
        if (sv > static_cast<double>(0x7FFFFFFF)) {
            d[i] = 0x7FFFFFFF;
        } else if (sv <= static_cast<double>(std::numeric_limits<s32>::min())) {
            d[i] = 0x80000000;
        } else {
            switch ((op >> 21) & 0x1F) {
            // round_ieee_754 can round up to 2^31 through its float temporary.
            case 16:
                d[i] = static_cast<u32>(trunc_to_s32_x86(round_ieee_754(sv)));
                break; // n
            case 17:   // z
                d[i] = static_cast<u32>(s[i] >= 0 ? trunc_to_s32_x86(std::floor(sv))
                                                  : trunc_to_s32_x86(std::ceil(sv)));
                break;
            case 18:
                d[i] = static_cast<u32>(trunc_to_s32_x86(std::ceil(sv)));
                break; // u
            case 19:
                d[i] = static_cast<u32>(trunc_to_s32_x86(std::floor(sv)));
                break; // d
            default:
                d[i] = 0x7FFFFFFF;
                break;
            }
        }
    }
    // Does not apply sat, but does apply mask.
    float out[4];
    for (int i = 0; i < 4; i++) out[i] = bitsf(d[i]);
    apply_prefix_d(r, out, sz, true);
    write_vector(r, out, sz, vd);
    eat_prefixes(r);
}

void op_vi2f(const Regs &r, u32 op) {
    float s[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int imm = (op >> 16) & 0x1F;
    const float mult = 1.0f / static_cast<float>(1UL << imm);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    // Negate, abs, and constants apply as you'd expect to the bits.
    apply_swizzle_s(r, s, sz);
    for (int i = 0; i < num_elements(sz); i++) {
        d[i] = static_cast<float>(static_cast<s32>(fbits(s[i]))) * mult;
    }
    // Sat and mask apply normally.
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vh2f(const Regs &r, u32 op) {
    float s[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);

    VectorSize outsize = kPair;
    if (sz == kSingle) {
        outsize = kPair;
        d[0] = expand_half(static_cast<std::uint16_t>(fbits(s[0]) & 0xFFFF));
        d[1] = expand_half(static_cast<std::uint16_t>(fbits(s[0]) >> 16));
    } else {
        // All other sizes are treated the same.
        outsize = kQuad;
        d[0] = expand_half(static_cast<std::uint16_t>(fbits(s[0]) & 0xFFFF));
        d[1] = expand_half(static_cast<std::uint16_t>(fbits(s[0]) >> 16));
        d[2] = expand_half(static_cast<std::uint16_t>(fbits(s[1]) & 0xFFFF));
        d[3] = expand_half(static_cast<std::uint16_t>(fbits(s[1]) >> 16));
    }
    apply_prefix_d(r, d, outsize);
    write_vector(r, d, outsize, vd);
    eat_prefixes(r);
}

void op_vf2h(const Regs &r, u32 op) {
    float s[4]{};
    u32 d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    // Swizzle can cause V_Single to properly write both components.
    apply_swizzle_s(r, s, kQuad);
    // Negate should not actually apply to invalid swizzle.
    retain_invalid_swizzle_st(r, s, kQuad);

    VectorSize outsize = kSingle;
    switch (sz) {
    case kSingle:
    case kPair:
        outsize = kSingle;
        d[0] = shrink_to_half(s[0]) | (static_cast<u32>(shrink_to_half(s[1])) << 16);
        break;
    case kTriple:
    case kQuad:
        outsize = kPair;
        d[0] = shrink_to_half(s[0]) | (static_cast<u32>(shrink_to_half(s[1])) << 16);
        d[1] = shrink_to_half(s[2]) | (static_cast<u32>(shrink_to_half(s[3])) << 16);
        break;
    }
    float out[4];
    for (int i = 0; i < 4; i++) out[i] = bitsf(d[i]);
    apply_prefix_d(r, out, outsize);
    write_vector(r, out, outsize, vd);
    eat_prefixes(r);
}

void op_vx2i(const Regs &r, u32 op) {
    float sf[4]{};
    u32 d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    VectorSize sz = vec_size(op);
    VectorSize oz = sz;
    read_vector(r, sf, sz, vs);
    apply_swizzle_s(r, sf, sz);
    u32 s[4];
    for (int i = 0; i < 4; i++) s[i] = fbits(sf[i]);

    // TODO upstream: invalid swizzle seems to reuse last output.
    switch ((op >> 16) & 3) {
    case 0: { // vuc2i
        // Quad is the only option. This converts 8-bit unsigned to 31-bit
        // signed, swizzling to saturate.
        u32 value = s[0];
        for (int i = 0; i < 4; i++) {
            d[i] = static_cast<u32>(static_cast<u32>(value & 0xFF) * 0x01010101UL) >> 1;
            value >>= 8;
        }
        oz = kQuad;
        break;
    }
    case 1: { // vc2i
        // Unlike vuc2i, the source and destination are signed so there is no shift.
        const u32 value = s[0];
        d[0] = (value & 0xFF) << 24;
        d[1] = (value & 0xFF00) << 16;
        d[2] = (value & 0xFF0000) << 8;
        d[3] = (value & 0xFF000000);
        oz = kQuad;
        break;
    }
    case 2: // vus2i
    case 3: // vs2i
    {
        // Note: vus2i skips swizzle such that 0xFFFF -> 0x7FFF8000 unlike vuc2i.
        const bool is_signed = ((op >> 16) & 3) == 3;
        oz = kPair;
        if (sz == kQuad || sz == kTriple) sz = kPair;
        if (sz == kPair) oz = kQuad;
        for (int i = 0; i < num_elements(sz); i++) {
            const u32 value = s[i];
            if (is_signed) {
                d[i * 2] = (value & 0xFFFF) << 16;
                d[i * 2 + 1] = value & 0xFFFF0000;
            } else {
                d[i * 2] = (value & 0xFFFF) << 15;
                d[i * 2 + 1] = (value & 0xFFFF0000) >> 1;
            }
        }
        break;
    }
    }

    // Saturation does in fact apply.
    float out[4];
    for (int i = 0; i < 4; i++) out[i] = bitsf(d[i]);
    apply_prefix_d(r, out, oz);
    write_vector(r, out, oz, vd);
    eat_prefixes(r);
}

void op_vi2x(const Regs &r, u32 op) {
    float sf[4]{};
    u32 d[2]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    VectorSize oz = kSingle;
    read_vector(r, sf, sz, vs);
    // Negate, const, etc. apply as expected.
    apply_swizzle_s(r, sf, kQuad);
    s32 s[4];
    for (int i = 0; i < 4; i++) s[i] = static_cast<s32>(fbits(sf[i]));

    switch ((op >> 16) & 3) {
    case 0: // vi2uc
        for (int i = 0; i < 4; i++) {
            int v = s[i];
            if (v < 0) v = 0;
            v >>= 23;
            d[0] |= (static_cast<u32>(v) & 0xFF) << (i * 8);
        }
        oz = kSingle;
        break;
    case 1: // vi2c
        for (int i = 0; i < 4; i++) {
            const u32 v = static_cast<u32>(s[i]);
            d[0] |= (v >> 24) << (i * 8);
        }
        oz = kSingle;
        break;
    case 2: { // vi2us
        const int elems = (num_elements(sz) + 1) / 2;
        for (int i = 0; i < elems; i++) {
            int low = s[i * 2];
            int high = s[i * 2 + 1];
            if (low < 0) low = 0;
            if (high < 0) high = 0;
            low >>= 15;
            high >>= 15;
            d[i] = static_cast<u32>(low) | (static_cast<u32>(high) << 16);
        }
        oz = (sz == kQuad || sz == kTriple) ? kPair : kSingle;
        break;
    }
    case 3: { // vi2s
        const int elems = (num_elements(sz) + 1) / 2;
        for (int i = 0; i < elems; i++) {
            u32 low = static_cast<u32>(s[i * 2]);
            u32 high = static_cast<u32>(s[i * 2 + 1]);
            low >>= 16;
            high >>= 16;
            d[i] = low | (high << 16);
        }
        oz = (sz == kQuad || sz == kTriple) ? kPair : kSingle;
        break;
    }
    }
    // D prefix applies as expected.
    float out[2] = {bitsf(d[0]), bitsf(d[1])};
    apply_prefix_d(r, out, oz);
    write_vector(r, out, oz, vd);
    eat_prefixes(r);
}

void op_color_conv(const Regs &r, u32 op) {
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    float sf[4]{};
    const VectorSize isz = vec_size(op);
    read_vector(r, sf, kQuad, vs);
    apply_swizzle_s(r, sf, kQuad);
    std::uint16_t colors[4];
    // TODO upstream: invalid swizzle values almost seem to use the last value
    // converted in a previous execution of these ops.
    for (int i = 0; i < 4; i++) {
        const u32 in = fbits(sf[i]);
        u32 col = 0;
        switch ((op >> 16) & 3) {
        case 1: { // 4444
            const u32 a = ((in >> 24) & 0xFF) >> 4;
            const u32 b = ((in >> 16) & 0xFF) >> 4;
            const u32 g = ((in >> 8) & 0xFF) >> 4;
            const u32 rr = ((in) & 0xFF) >> 4;
            col = (a << 12) | (b << 8) | (g << 4) | (rr);
            break;
        }
        case 2: { // 5551
            const u32 a = ((in >> 24) & 0xFF) >> 7;
            const u32 b = ((in >> 16) & 0xFF) >> 3;
            const u32 g = ((in >> 8) & 0xFF) >> 3;
            const u32 rr = ((in) & 0xFF) >> 3;
            col = (a << 15) | (b << 10) | (g << 5) | (rr);
            break;
        }
        case 3: { // 565
            const u32 b = ((in >> 16) & 0xFF) >> 3;
            const u32 g = ((in >> 8) & 0xFF) >> 2;
            const u32 rr = ((in) & 0xFF) >> 3;
            col = (b << 11) | (g << 5) | (rr);
            break;
        }
        default:
            break;
        }
        colors[i] = static_cast<std::uint16_t>(col);
    }
    float ov[2] = {bitsf(static_cast<u32>(colors[0]) | (static_cast<u32>(colors[1]) << 16)),
                   bitsf(static_cast<u32>(colors[2]) | (static_cast<u32>(colors[3]) << 16))};
    apply_prefix_d(r, ov, kPair);
    write_vector(r, ov, isz == kSingle ? kSingle : kPair, vd);
    eat_prefixes(r);
}

void op_vdot(const Regs &r, u32 op) {
    float s[4]{}, t[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, kQuad);
    read_vector(r, t, sz, vt);
    apply_swizzle_t(r, t, kQuad);

    float d = ppsspp_dot4(s, t);

    apply_prefix_d(r, &d, kSingle);
    write_vector(r, &d, kSingle, vd);
    eat_prefixes(r);
}

void op_vhdp(const Regs &r, u32 op) {
    float s[4]{}, t[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    read_vector(r, t, sz, vt);
    apply_swizzle_t(r, t, kQuad);

    // S prefix forces constant 1 for the last element (w for quad.)
    // Otherwise it is the same as vdot.
    u32 remove = 0;
    u32 add = 0;
    if (sz == kQuad) {
        remove = swizzle(0, 0, 0, 3);
        add = make_constants(Const::kNone, Const::kNone, Const::kNone, Const::kOne);
    } else if (sz == kTriple) {
        remove = swizzle(0, 0, 3, 0);
        add = make_constants(Const::kNone, Const::kNone, Const::kOne, Const::kNone);
    } else if (sz == kPair) {
        remove = swizzle(0, 3, 0, 0);
        add = make_constants(Const::kNone, Const::kOne, Const::kNone, Const::kNone);
    } else {
        remove = swizzle(3, 0, 0, 0);
        add = make_constants(Const::kOne, Const::kNone, Const::kNone, Const::kNone);
    }
    apply_prefix_st(s, rewrite_prefix(r, kCtrlSPrefix, remove, add), kQuad);

    float sum = 0.0f;
    for (int i = 0; i < 4; i++) sum = fadd(sum, fmul(s[i], t[i]));
    float d = my_isnan(sum) ? std::fabs(sum) : sum;
    apply_prefix_d(r, &d, kSingle);
    write_vector(r, &d, kSingle, vd);
    eat_prefixes(r);
}

void op_vbfy(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    read_vector(r, t, sz, vs);

    if (op & 0x10000) {
        // vbfy2: S prefix forces the negate flags (so z and w are negative.)
        apply_prefix_st(s, rewrite_prefix(r, kCtrlSPrefix, 0, negate_bits(0, 0, 1, 1)), sz);
        // T prefix forces swizzle (zwxy.)
        apply_prefix_st(t, rewrite_prefix(r, kCtrlTPrefix, any_swizzle(), swizzle(2, 3, 0, 1)), sz);
    } else {
        // vbfy1: S prefix forces the negate flags (so y and w are negative.)
        apply_prefix_st(s, rewrite_prefix(r, kCtrlSPrefix, 0, negate_bits(0, 1, 0, 1)), sz);
        // T prefix forces swizzle (yxwz.)
        apply_prefix_st(t, rewrite_prefix(r, kCtrlTPrefix, any_swizzle(), swizzle(1, 0, 3, 2)), sz);
    }

    d[0] = fadd(s[0], t[0]);
    d[1] = fadd(s[1], t[1]);
    d[2] = fadd(s[2], t[2]);
    d[3] = fadd(s[3], t[3]);

    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

namespace {

// vsrt1..4 share everything but the forced T swizzle and the min/max pattern.
void vsrt_common(const Regs &r, u32 op, u32 t_swizzle, const bool use_max[4]) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);
    read_vector(r, t, sz, vs);

    apply_prefix_st(t, rewrite_prefix(r, kCtrlTPrefix, swizzle(3, 3, 3, 3), t_swizzle), sz);

    // TODO upstream: may mishandle NAN / negative zero / etc.
    for (int i = 0; i < 4; i++) d[i] = use_max[i] ? std::max(s[i], t[i]) : std::min(s[i], t[i]);
    retain_invalid_swizzle_st(r, d, sz);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

} // namespace

void op_vsrt1(const Regs &r, u32 op) {
    // T is force swizzled to yxwz from S.
    static const bool kMax[4] = {false, true, false, true};
    vsrt_common(r, op, swizzle(1, 0, 3, 2), kMax);
}

void op_vsrt2(const Regs &r, u32 op) {
    // T is force swizzled to wzyx from S.
    static const bool kMax[4] = {false, false, true, true};
    vsrt_common(r, op, swizzle(3, 2, 1, 0), kMax);
}

void op_vsrt3(const Regs &r, u32 op) {
    static const bool kMax[4] = {true, false, true, false};
    vsrt_common(r, op, swizzle(1, 0, 3, 2), kMax);
}

void op_vsrt4(const Regs &r, u32 op) {
    static const bool kMax[4] = {true, true, false, false};
    vsrt_common(r, op, swizzle(3, 2, 1, 0), kMax);
}

void op_vcrs(const Regs &r, u32 op) {
    // Half a cross product.
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    read_vector(r, t, sz, vt);

    // S prefix forces swizzle (yzx?.)
    apply_prefix_st(s, rewrite_prefix(r, kCtrlSPrefix, swizzle(3, 3, 3, 0), swizzle(1, 2, 0, 0)),
                    sz);
    // T prefix forces swizzle (zxy?.)
    apply_prefix_st(t, rewrite_prefix(r, kCtrlTPrefix, swizzle(3, 3, 3, 0), swizzle(2, 0, 1, 0)),
                    sz);

    d[0] = fmul(s[0], t[0]);
    d[1] = fmul(s[1], t[1]);
    d[2] = fmul(s[2], t[2]);
    d[3] = fmul(s[3], t[3]);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vdet(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    // This is normally V_Pair. Unfilled s/t values are treated as zero.
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, kQuad);
    read_vector(r, t, sz, vt);

    // T prefix forces swizzle for x and y (yx??.) No forced negation here.
    apply_prefix_st(t, rewrite_prefix(r, kCtrlTPrefix, swizzle(3, 3, 0, 0), swizzle(1, 0, 0, 0)),
                    kQuad);

    d[0] = fmul(s[0], t[0]) - fmul(s[1], t[1]);
    d[0] = fadd(d[0], fadd(fmul(s[2], t[2]), fmul(s[3], t[3])));

    apply_prefix_d(r, d, kSingle);
    write_vector(r, d, kSingle, vd);
    eat_prefixes(r);
}

void op_vfad(const Regs &r, u32 op) {
    float s[4]{}, t[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, kQuad);

    // T prefix generates constants, but abs can change the constant.
    apply_prefix_st(
        t,
        rewrite_prefix(r, kCtrlTPrefix, any_swizzle(),
                       make_constants(Const::kOne, Const::kOne, Const::kOne, Const::kOne)),
        kQuad);

    float d = 0.0f;
    for (int i = 0; i < 4; i++) d = fadd(d, fmul(s[i], t[i]));
    apply_prefix_d(r, &d, kSingle);
    write_vector(r, &d, kSingle, vd);
    eat_prefixes(r);
}

void op_vavg(const Regs &r, u32 op) {
    float s[4]{}, t[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, kQuad);

    // T prefix generates constants, but supports negate.
    const u32 remove = any_swizzle() | abs_bits(1, 1, 1, 1);
    u32 add = 0;
    if (sz == kSingle)
        add = make_constants(Const::kZero, Const::kZero, Const::kZero, Const::kZero);
    else if (sz == kPair)
        add = make_constants(Const::kHalf, Const::kHalf, Const::kHalf, Const::kHalf);
    else if (sz == kTriple)
        add = make_constants(Const::kThird, Const::kThird, Const::kThird, Const::kThird);
    else
        add = make_constants(Const::kFourth, Const::kFourth, Const::kFourth, Const::kFourth);
    apply_prefix_st(t, rewrite_prefix(r, kCtrlTPrefix, remove, add), kQuad);

    float d = 0.0f;
    for (int i = 0; i < 4; i++) d = fadd(d, fmul(s[i], t[i]));
    apply_prefix_d(r, &d, kSingle);
    write_vector(r, &d, kSingle, vd);
    eat_prefixes(r);
}

void op_vscl(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);

    // T prefix forces swizzle (zzzz for some reason, so we force V_Quad.)
    const int tlane = (vt >> 5) & 3;
    t[tlane] = r.v[voffset(vt)];
    apply_prefix_st(
        t, rewrite_prefix(r, kCtrlTPrefix, any_swizzle(), swizzle(tlane, tlane, tlane, tlane)),
        kQuad);

    const int n = num_elements(sz);
    for (int i = 0; i < n; i++) d[i] = fmul(s[i], t[i]);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vrnds(const Regs &r, u32 op) {
    const int vd = vd_of(op);
    float seed = r.v[voffset(vd)];
    // Swizzles apply a constant value, constants/abs/neg work to vary the seed.
    apply_swizzle_s(r, &seed, kSingle);
    vrnd_init(fbits(seed), r.ctrl + kCtrlRcx0);
    eat_prefixes(r);
}

void op_vrndx(const Regs &r, u32 op) {
    u32 d[4]{};
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    const int n = num_elements(sz);
    // Values are written in backwards order.
    for (int i = n - 1; i >= 0; i--) {
        switch ((op >> 16) & 0x1F) {
        case 1:
            d[i] = vrnd_generate(r.ctrl + kCtrlRcx0);
            break; // vrndi
        case 2:    // vrndf1 (>= 1, < 2)
            d[i] = 0x3F800000 | (vrnd_generate(r.ctrl + kCtrlRcx0) & 0x007FFFFF);
            break;
        case 3: // vrndf2 (>= 2, < 4)
            d[i] = 0x40000000 | (vrnd_generate(r.ctrl + kCtrlRcx0) & 0x007FFFFF);
            break;
        default:
            break;
        }
    }
    // D prefix is broken and applies to the last element only (mask and sat.)
    keep_last_d_prefix(r, n - 1);
    float out[4];
    for (int i = 0; i < 4; i++) out[i] = bitsf(d[i]);
    apply_prefix_d(r, out, sz);
    write_vector(r, out, sz, vd);
    eat_prefixes(r);
}

// Generates one line of a rotation matrix around one of the three axes.
void op_vrot(const Regs &r, u32 op) {
    float d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int imm = (op >> 16) & 0x1F;
    const VectorSize sz = vec_size(op);
    const bool neg_sin = (imm & 0x10) != 0;
    const int sine_lane = (imm >> 2) & 3;
    const int cosine_lane = imm & 3;

    float sine = 0.0f;
    float cosine = 0.0f;
    if (r.ctrl[kCtrlSPrefix] == 0x000E4) {
        vfpu_sincos(r.v[voffset(vs)], sine, cosine);
        if (neg_sin) sine = -sine;
    } else {
        // Swizzle on S is a bit odd here, but generally only applies to sine.
        float s[4]{};
        read_vector(r, s, kSingle, vs);
        // We apply neg_sin later, not here. This handles zero a bit better.
        apply_prefix_st(
            s, rewrite_prefix(r, kCtrlSPrefix, negate_bits(1, 0, 0, 0), negate_bits(0, 0, 0, 0)),
            kSingle);

        // Cosine ignores all prefixes, so take the original.
        cosine = vfpu_cos(r.v[voffset(vs)]);
        sine = vfpu_sin(s[0]);

        if (neg_sin) sine = -sine;
        retain_invalid_swizzle_st(r, &sine, kSingle);
    }

    if (sine_lane == cosine_lane) {
        for (int i = 0; i < 4; i++) d[i] = sine;
    } else {
        d[sine_lane] = sine;
    }

    if (((vd >> 2) & 7) == ((vs >> 2) & 7)) {
        u32 dregs[4]{};
        get_vector_regs(dregs, sz, vd);
        // Calculate cosine based on sine/zero result.
        bool written = false;
        for (int i = 0; i < 4; i++) {
            if (static_cast<u32>(vs) == dregs[i]) {
                d[cosine_lane] = vfpu_cos(d[i]);
                written = true;
                break;
            }
        }
        if (!written) d[cosine_lane] = cosine;
    } else {
        d[cosine_lane] = cosine;
    }

    // D prefix works, just not for the cosine lane.
    const u32 remove = (3u << cosine_lane) | (1u << (8 + cosine_lane));
    r.ctrl[kCtrlDPrefix] &= 0xFFFFF ^ remove;
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vtfm(const Regs &r, u32 op) {
    float s[16]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const int ins = (op >> 23) & 3;

    const VectorSize sz = static_cast<VectorSize>(ins + 1);
    const int n = num_elements(vec_size(op));

    const int tn = std::min(n, ins + 1);
    read_matrix(r, s, sz, vs);
    read_vector(r, t, sz, vt);

    for (int i = 0; i < ins; i++) {
        d[i] = fmul(s[i * 4], t[0]);
        for (int k = 1; k < tn; k++) d[i] = fadd(d[i], fmul(s[i * 4 + k], t[k]));
        if (ins >= n) d[i] = fadd(d[i], s[i * 4 + ins]);
    }

    // S and T prefixes apply for the final row only. The T prefix is used to
    // apply zero/one constants, but abs still changes it.
    apply_swizzle_s(r, &s[ins * 4], kQuad);
    Const x = Const::kNone;
    Const y = n < 2 ? Const::kZero : Const::kNone;
    Const z = n < 3 ? Const::kZero : Const::kNone;
    Const w = n < 4 ? Const::kZero : Const::kNone;
    if (ins >= n) {
        if (ins == 1)
            y = Const::kOne;
        else if (ins == 2)
            z = Const::kOne;
        else if (ins == 3)
            w = Const::kOne;
    }
    const u32 remove = swizzle(0, n < 2 ? 3 : 0, n < 3 ? 3 : 0, n < 4 ? 3 : 0);
    apply_prefix_st(t, rewrite_prefix(r, kCtrlTPrefix, remove, make_constants(x, y, z, w)), kQuad);

    // Really this is the operation all rows probably use (with constant wiring.)
    d[ins] = fmul(s[ins * 4], t[0]);
    for (int k = 1; k < 4; k++) d[ins] = fadd(d[ins], fmul(s[ins * 4 + k], t[k]));

    // D prefix applies to the last element only.
    keep_last_d_prefix(r, ins);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_mftv(const Regs &r, u32 op, u32 *gpr) {
    const int imm = op & 0xFF;
    const int rt = rt_of(op);
    switch ((op >> 21) & 0x1F) {
    case 3: // mfv / mfvc
        // rt = 0, imm = 255 appears to be used as a CPU interlock by some games.
        if (rt != 0) {
            if (imm < 128) {
                gpr[rt] = fbits(r.v[voffset(imm)]);
            } else if (imm < 128 + kNumCtrl) { // mfvc
                gpr[rt] = r.ctrl[imm - 128];
            }
        }
        break;
    case 7: // mtv
        if (imm < 128) {
            r.v[voffset(imm)] = bitsf(gpr[rt]);
        } else if (imm < 128 + kNumCtrl) { // mtvc
            u32 mask = 0;
            if (get_ctrl_mask(imm - 128, &mask)) r.ctrl[imm - 128] = gpr[rt] & mask;
        }
        break;
    default:
        break;
    }
}

void op_vmfvc(const Regs &r, u32 op) {
    const int vd = vd_of(op);
    const int imm = (op >> 8) & 0x7F;
    r.v[voffset(vd)] = bitsf(imm < kNumCtrl ? r.ctrl[imm] : 0u);
}

void op_vmtvc(const Regs &r, u32 op) {
    const int vs = vs_of(op);
    const int imm = op & 0x7F;
    if (imm < kNumCtrl) {
        u32 mask = 0;
        if (get_ctrl_mask(imm, &mask)) r.ctrl[imm] = fbits(r.v[voffset(vs)]) & mask;
    }
}

void op_vcst(const Regs &r, u32 op) {
    const int con_num = (op >> 16) & 0x1F;
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    const float c = bitsf(kCstBits[con_num]);
    float temp[4] = {c, c, c, c};
    apply_prefix_d(r, temp, sz);
    write_vector(r, temp, sz, vd);
    eat_prefixes(r);
}

void op_vcmp(const Regs &r, u32 op) {
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const int cond = op & 0xF;
    const VectorSize sz = vec_size(op);
    const int n = num_elements(sz);
    float s[4]{}, t[4]{};
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);
    read_vector(r, t, sz, vt);
    apply_swizzle_t(r, t, sz);
    u32 cc = 0;
    u32 or_val = 0;
    u32 and_val = 1;
    u32 affected_bits = (1 << 4) | (1 << 5); // 4 and 5
    for (int i = 0; i < n; i++) {
        u32 c = 0;
        // These set c to 0 or 1, nothing else.
        switch (cond) {
        case 0:
            c = 0;
            break; // VC_FL
        case 1:
            c = s[i] == t[i];
            break; // VC_EQ
        case 2:
            c = s[i] < t[i];
            break; // VC_LT
        case 3:
            c = s[i] <= t[i];
            break; // VC_LE
        case 4:
            c = 1;
            break; // VC_TR
        case 5:
            c = s[i] != t[i];
            break; // VC_NE
        case 6:
            c = s[i] >= t[i];
            break; // VC_GE
        case 7:
            c = s[i] > t[i];
            break; // VC_GT
        case 8:
            c = s[i] == 0.0f || s[i] == -0.0f;
            break; // VC_EZ
        case 9:
            c = my_isnan(s[i]);
            break; // VC_EN
        case 10:
            c = my_isinf(s[i]);
            break; // VC_EI
        case 11:
            c = my_isnanorinf(s[i]);
            break; // VC_ES
        case 12:
            c = s[i] != 0;
            break; // VC_NZ
        case 13:
            c = !my_isnan(s[i]);
            break; // VC_NN
        case 14:
            c = !my_isinf(s[i]);
            break; // VC_NI
        case 15:
            c = !(my_isnanorinf(s[i]));
            break; // VC_NS
        default:
            break;
        }
        cc |= (c << i);
        or_val |= c;
        and_val &= c;
        affected_bits |= 1u << i;
    }
    // Use masking to only change the affected bits.
    r.ctrl[kCtrlCc] = (r.ctrl[kCtrlCc] & ~affected_bits) |
                      ((cc | (or_val << 4) | (and_val << 5)) & affected_bits);
    eat_prefixes(r);
}

void op_vminmax(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vt = vt_of(op);
    const int vs = vs_of(op);
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    const int n = num_elements(sz);

    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);
    read_vector(r, t, sz, vt);
    apply_swizzle_t(r, t, sz);

    // If both are zero, take t's sign.
    // Otherwise: -NAN < -INF < real < INF < NAN (higher mantissa is farther from 0.)
    const auto si = [&](int i) { return static_cast<s32>(fbits(s[i])); };
    const auto ti = [&](int i) { return static_cast<s32>(fbits(t[i])); };
    switch ((op >> 23) & 3) {
    case 2: // vmin
        for (int i = 0; i < n; i++) {
            if (my_isnanorinf(s[i]) || my_isnanorinf(t[i])) {
                // If both are negative, we flip the comparison (not two's compliment.)
                if (si(i) < 0 && ti(i) < 0) {
                    // If at least one side is NAN, we take the highest mantissa bits.
                    d[i] = bitsf(static_cast<u32>(std::max(ti(i), si(i))));
                } else {
                    // Otherwise, we take the lowest value (negative or lowest mantissa.)
                    d[i] = bitsf(static_cast<u32>(std::min(ti(i), si(i))));
                }
            } else {
                d[i] = std::min(t[i], s[i]);
            }
        }
        break;
    case 3: // vmax
        for (int i = 0; i < n; i++) {
            // This is the same logic as vmin, just reversed.
            if (my_isnanorinf(s[i]) || my_isnanorinf(t[i])) {
                if (si(i) < 0 && ti(i) < 0) {
                    d[i] = bitsf(static_cast<u32>(std::min(ti(i), si(i))));
                } else {
                    d[i] = bitsf(static_cast<u32>(std::max(ti(i), si(i))));
                }
            } else {
                d[i] = std::max(t[i], s[i]);
            }
        }
        break;
    default:
        eat_prefixes(r);
        return;
    }
    retain_invalid_swizzle_st(r, d, sz);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vscmp(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vt = vt_of(op);
    const int vs = vs_of(op);
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);
    read_vector(r, t, sz, vt);
    apply_swizzle_t(r, t, sz);
    const int n = num_elements(sz);
    for (int i = 0; i < n; i++) {
        const float a = s[i] - t[i];
        if (my_isnan(a)) {
            // NAN/INF are treated as just larger numbers, as in vmin/vmax.
            const u32 s_magnitude = fbits(s[i]) & 0x7FFFFFFF;
            const u32 t_magnitude = fbits(t[i]) & 0x7FFFFFFF;
            const u32 s_value = static_cast<s32>(fbits(s[i])) < 0 ? 0u - s_magnitude : s_magnitude;
            const u32 t_value = static_cast<s32>(fbits(t[i])) < 0 ? 0u - t_magnitude : t_magnitude;
            const s32 b = static_cast<s32>(s_value - t_value);
            d[i] = static_cast<float>((0 < b) - (b < 0));
        } else {
            d[i] = static_cast<float>((0.0f < a) - (a < 0.0f));
        }
    }
    retain_invalid_swizzle_st(r, d, sz);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

namespace {

void vset_common(const Regs &r, u32 op, bool greater_equal) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vt = vt_of(op);
    const int vs = vs_of(op);
    const int vd = vd_of(op);
    const VectorSize sz = vec_size(op);
    const int n = num_elements(sz);
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);
    read_vector(r, t, sz, vt);
    apply_swizzle_t(r, t, sz);
    for (int i = 0; i < n; i++) {
        if (my_isnan(s[i]) || my_isnan(t[i]))
            d[i] = 0.0f;
        else if (greater_equal)
            d[i] = s[i] >= t[i] ? 1.0f : 0.0f;
        else
            d[i] = s[i] < t[i] ? 1.0f : 0.0f;
    }
    retain_invalid_swizzle_st(r, d, sz);
    // The clamp cannot matter, so skip it.
    apply_prefix_d(r, d, sz, true);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

} // namespace

void op_vsge(const Regs &r, u32 op) {
    vset_common(r, op, true);
}
void op_vslt(const Regs &r, u32 op) {
    vset_common(r, op, false);
}

void op_vcmov(const Regs &r, u32 op) {
    const int vs = vs_of(op);
    const int vd = vd_of(op);
    const u32 tf = (op >> 19) & 1;
    const int imm3 = (op >> 16) & 7;
    const VectorSize sz = vec_size(op);
    const int n = num_elements(sz);
    float s[4]{}, d[4]{};
    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);
    // Not only is D read (as T), but the T prefix applies to it.
    read_vector(r, d, sz, vd);
    apply_swizzle_t(r, d, sz);

    const u32 cc = r.ctrl[kCtrlCc];
    if (imm3 < 6) {
        if (((cc >> imm3) & 1) == (tf ^ 1u)) {
            for (int i = 0; i < n; i++) d[i] = s[i];
        }
    } else if (imm3 == 6) {
        for (int i = 0; i < n; i++) {
            if (((cc >> i) & 1) == (tf ^ 1u)) d[i] = s[i];
        }
    }
    // imm3 == 7: upstream reports "Bad Imm3 in cmov" and keeps d.
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vec_do3(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);

    int optype = -1;
    switch (op >> 26) {
    case 24: // VFPU0
        switch ((op >> 23) & 7) {
        case 0:
            optype = 0;
            break;
        case 1:
            optype = 1;
            break;
        case 7:
            optype = 7;
            break;
        default:
            break;
        }
        break;
    case 25: // VFPU1
        if (((op >> 23) & 7) == 0) optype = 8;
        break;
    default:
        break;
    }

    const int n = num_elements(sz);
    read_vector(r, s, sz, vs);
    read_vector(r, t, sz, vt);
    if (optype != 7) {
        apply_swizzle_s(r, s, sz);
        apply_swizzle_t(r, t, sz);
    } else {
        // The prefix handling of S/T is a bit odd, probably the HW doesn't do
        // it in parallel. The X prefix is applied to the last element in sz.
        apply_swizzle_s(r, &s[n - 1], kSingle, -kInfinity);
        apply_swizzle_t(r, &t[n - 1], kSingle, -kInfinity);
    }

    for (int i = 0; i < n; i++) {
        switch (optype) {
        case 0:
            d[i] = fadd(s[i], t[i]);
            break; // vadd
        case 1:
            d[i] = s[i] - t[i];
            break; // vsub
        case 7:
            d[i] = s[i] / t[i];
            break; // vdiv
        case 8:
            d[i] = fmul(s[i], t[i]);
            break; // vmul
        default:
            break;
        }
    }

    // For vdiv only, the D prefix only applies mask (and like S/T, x applied to last.)
    if (optype == 7) {
        keep_last_d_prefix(r, n - 1);
        apply_prefix_d(r, d, sz);
    } else {
        retain_invalid_swizzle_st(r, d, sz);
        apply_prefix_d(r, d, sz);
    }
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_cross_quat(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);
    const int n = num_elements(sz);
    read_vector(r, s, sz, vs);
    read_vector(r, t, sz, vt);

    const u32 remove = any_swizzle() | negate_bits(1, 1, 1, 1);
    switch (sz) {
    case kTriple: { // vcrsp.t
        d[0] = fmul(s[1], t[2]) - fmul(s[2], t[1]);
        d[1] = fmul(s[2], t[0]) - fmul(s[0], t[2]);
        // T prefix forces swizzle and negate, can be used to have weird constants.
        apply_prefix_st(
            t,
            rewrite_prefix(r, kCtrlTPrefix, remove, swizzle(1, 0, 3, 2) | negate_bits(0, 1, 0, 0)),
            kQuad);
        apply_swizzle_s(r, s, kQuad);
        d[2] = dot4(s, t);
        break;
    }
    case kQuad: { // vqmul.q
        d[0] = fadd(fadd(fmul(s[0], t[3]), fmul(s[1], t[2])) - fmul(s[2], t[1]), fmul(s[3], t[0]));
        d[1] = fadd(fadd(fadd(fmul(-s[0], t[2]), fmul(s[1], t[3])), fmul(s[2], t[0])),
                    fmul(s[3], t[1]));
        d[2] = fadd(fadd(fmul(s[0], t[1]) - fmul(s[1], t[0]), fmul(s[2], t[3])), fmul(s[3], t[2]));
        // T prefix forces swizzle and negate, can be used to have weird constants.
        apply_prefix_st(
            t,
            rewrite_prefix(r, kCtrlTPrefix, remove, swizzle(0, 1, 2, 3) | negate_bits(1, 1, 1, 0)),
            kQuad);
        apply_swizzle_s(r, s, sz);
        d[3] = dot4(s, t);
        break;
    }
    case kPair:
        // t swizzles invalid so the multiply is always zero.
        d[0] = 0;
        apply_prefix_st(
            t,
            rewrite_prefix(r, kCtrlTPrefix, remove, swizzle(0, 0, 0, 0) | negate_bits(0, 0, 0, 0)),
            kQuad);
        apply_swizzle_s(r, s, kQuad);
        // It's possible to populate a value by swizzling s[2].
        d[1] = fmul(s[2], t[2]);
        break;
    case kSingle:
        // t swizzles invalid so the multiply is always zero.
        d[0] = 0;
        break;
    }

    // D prefix applies to the last element only (mask and sat) for pair and larger.
    if (sz != kSingle) {
        keep_last_d_prefix(r, n - 1);
        apply_prefix_d(r, d, sz);
    } else {
        // Single always seems to write out zero.
        r.ctrl[kCtrlDPrefix] = 0;
    }
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vlgb(const Regs &r, u32 op) {
    // Vector log binary (extract exponent).
    float s[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);

    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);

    const int exp = static_cast<int>((fbits(s[0]) & 0x7F800000) >> 23);
    if (exp == 0xFF) {
        d[0] = s[0];
    } else if (exp == 0) {
        d[0] = -kInfinity;
    } else {
        d[0] = static_cast<float>(exp - 127);
    }
    // If sz is greater than V_Single, the rest are copied unchanged.
    for (int i = 1; i < num_elements(sz); ++i) d[i] = s[i];

    retain_invalid_swizzle_st(r, d, sz);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vwbn(const Regs &r, u32 op) {
    float s[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);
    const std::uint8_t exp = static_cast<std::uint8_t>((op >> 16) & 0xFF);

    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);

    const u32 s0 = fbits(s[0]);
    const u32 sigbit = s0 & 0x80000000;
    const u32 prev_exp = (s0 & 0x7F800000) >> 23;
    u32 mantissa = (s0 & 0x007FFFFF) | 0x00800000;
    if (prev_exp != 0xFF && prev_exp != 0) {
        if (exp > prev_exp) {
            const std::int8_t shift = static_cast<std::int8_t>((exp - prev_exp) & 0xF);
            mantissa = mantissa >> shift;
        } else {
            const std::int8_t shift = static_cast<std::int8_t>((prev_exp - exp) & 0xF);
            mantissa = mantissa << shift;
        }
        d[0] = bitsf(sigbit | (mantissa & 0x007FFFFF) | (static_cast<u32>(exp) << 23));
    } else {
        d[0] = bitsf(s0 | (static_cast<u32>(exp) << 23));
    }
    // If sz is greater than V_Single, the rest are copied unchanged.
    for (int i = 1; i < num_elements(sz); ++i) d[i] = s[i];

    retain_invalid_swizzle_st(r, d, sz);
    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vsbn(const Regs &r, u32 op) {
    float s[4]{}, t[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const int vt = vt_of(op);
    const VectorSize sz = vec_size(op);

    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);
    read_vector(r, t, sz, vt);
    apply_swizzle_t(r, t, sz);
    // Swizzle does apply to the value read as an integer.
    const std::uint8_t exp = static_cast<std::uint8_t>(127u + fbits(t[0]));

    // Simply replace the exponent bits.
    const u32 s0 = fbits(s[0]);
    const u32 prev = s0 & 0x7F800000;
    if (prev != 0 && prev != 0x7F800000) {
        d[0] = bitsf((s0 & ~0x7F800000u) | (static_cast<u32>(exp) << 23));
    } else {
        d[0] = s[0];
    }
    // If sz is greater than V_Single, the rest are copied unchanged.
    for (int i = 1; i < num_elements(sz); ++i) d[i] = s[i];

    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

void op_vsbz(const Regs &r, u32 op) {
    // Vector scale by zero (set exp to 0 to extract mantissa).
    float s[4]{}, d[4]{};
    const int vd = vd_of(op);
    const int vs = vs_of(op);
    const VectorSize sz = vec_size(op);

    read_vector(r, s, sz, vs);
    apply_swizzle_s(r, s, sz);

    // NAN and denormals pass through.
    const u32 s0 = fbits(s[0]);
    if (my_isnan(s[0]) || (s0 & 0x7F800000) == 0) {
        d[0] = s[0];
    } else {
        d[0] = bitsf((127u << 23) | (s0 & 0x007FFFFF));
    }
    // If sz is greater than V_Single, the rest are copied unchanged.
    for (int i = 1; i < num_elements(sz); ++i) d[i] = s[i];

    apply_prefix_d(r, d, sz);
    write_vector(r, d, sz, vd);
    eat_prefixes(r);
}

// --- Decoder (Core/MIPS/MIPSTables.cpp) -------------------------------------

bool branch_taken(const Regs &r, u32 op) {
    const int imm3 = (op >> 18) & 7;
    const u32 val = (r.ctrl[kCtrlCc] >> imm3) & 1;
    switch ((op >> 16) & 3) {
    case 0: // bvf
    case 2: // bvfl
        return val == 0;
    default: // bvt, bvtl
        return val != 0;
    }
}

namespace {

enum class Handler {
    kNone,
    kInvalid,
    kBranch,
    kVpfx,
    kSv,
    kSvq,
    kMftv,
    kVmfvc,
    kVmtvc,
    kVmatrixInit,
    kVvectorInit,
    kViim,
    kVidt,
    kVmmul,
    kVmscl,
    kVmmov,
    kVflush,
    kVv2op,
    kVocp,
    kVsocp,
    kVsgn,
    kVf2i,
    kVi2f,
    kVh2f,
    kVf2h,
    kVx2i,
    kVi2x,
    kColorConv,
    kVdot,
    kVhdp,
    kVbfy,
    kVsrt1,
    kVsrt2,
    kVsrt3,
    kVsrt4,
    kVcrs,
    kVdet,
    kVfad,
    kVavg,
    kVscl,
    kVrnds,
    kVrndx,
    kVrot,
    kVtfm,
    kVcst,
    kVcmp,
    kVminmax,
    kVscmp,
    kVsge,
    kVslt,
    kVcmov,
    kVecDo3,
    kCrossQuat,
    kVlgb,
    kVwbn,
    kVsbn,
    kVsbz,
};

struct Decoded {
    Handler handler = Handler::kNone;
    const char *name = "";
    bool sized = false; // Append the .s/.p/.t/.q suffix to the name.
};

Decoded decode(u32 op) {
    const auto ok = [](Handler h, const char *name, bool sized = true) {
        return Decoded{h, name, sized};
    };
    const Decoded invalid{Handler::kInvalid, "", false};
    switch (op >> 26) {
    case 18: { // COP2
        switch ((op >> 21) & 0x1F) {
        case 3:
            return ok(Handler::kMftv, (op & 0xFF) >= 128 ? "mfvc" : "mfv", false);
        case 7:
            return ok(Handler::kMftv, (op & 0xFF) >= 128 ? "mtvc" : "mtv", false);
        case 8: {
            static const char *const kNames[4] = {"bvf", "bvt", "bvfl", "bvtl"};
            return ok(Handler::kBranch, kNames[(op >> 16) & 3], false);
        }
        default:
            return invalid; // mfc2/cfc2/mtc2/ctc2 and "??": no interpreter upstream.
        }
    }
    case 24: // VFPU0
        switch ((op >> 23) & 7) {
        case 0:
            return ok(Handler::kVecDo3, "vadd");
        case 1:
            return ok(Handler::kVecDo3, "vsub");
        case 2:
            return ok(Handler::kVsbn, "vsbn");
        case 7:
            return ok(Handler::kVecDo3, "vdiv");
        default:
            return invalid;
        }
    case 25: // VFPU1
        switch ((op >> 23) & 7) {
        case 0:
            return ok(Handler::kVecDo3, "vmul");
        case 1:
            return ok(Handler::kVdot, "vdot");
        case 2:
            return ok(Handler::kVscl, "vscl");
        case 4:
            return ok(Handler::kVhdp, "vhdp");
        case 5:
            return ok(Handler::kVcrs, "vcrs");
        case 6:
            return ok(Handler::kVdet, "vdet");
        default:
            return invalid;
        }
    case 27: // VFPU3
        switch ((op >> 23) & 7) {
        case 0:
            return ok(Handler::kVcmp, "vcmp");
        case 2:
            return ok(Handler::kVminmax, "vmin");
        case 3:
            return ok(Handler::kVminmax, "vmax");
        case 5:
            return ok(Handler::kVscmp, "vscmp");
        case 6:
            return ok(Handler::kVsge, "vsge");
        case 7:
            return ok(Handler::kVslt, "vslt");
        default:
            return invalid;
        }
    case 50:
        return ok(Handler::kSv, "lv.s", false);
    case 53:
        return ok(Handler::kSvq, (op & 2) ? "lvr.q" : "lvl.q", false);
    case 54:
        return ok(Handler::kSvq, "lv.q", false);
    case 58:
        return ok(Handler::kSv, "sv.s", false);
    case 61:
        return ok(Handler::kSvq, (op & 2) ? "svr.q" : "svl.q", false);
    case 62:
        return ok(Handler::kSvq, "sv.q", false);
    case 52: { // VFPU4Jump
        const u32 sub = (op >> 21) & 0x1F;
        if (sub == 0) { // VFPU4
            switch ((op >> 16) & 0x1F) {
            case 0:
                return ok(Handler::kVv2op, "vmov");
            case 1:
                return ok(Handler::kVv2op, "vabs");
            case 2:
                return ok(Handler::kVv2op, "vneg");
            case 3:
                return ok(Handler::kVidt, "vidt");
            case 4:
                return ok(Handler::kVv2op, "vsat0");
            case 5:
                return ok(Handler::kVv2op, "vsat1");
            case 6:
                return ok(Handler::kVvectorInit, "vzero");
            case 7:
                return ok(Handler::kVvectorInit, "vone");
            case 16:
                return ok(Handler::kVv2op, "vrcp");
            case 17:
                return ok(Handler::kVv2op, "vrsq");
            case 18:
                return ok(Handler::kVv2op, "vsin");
            case 19:
                return ok(Handler::kVv2op, "vcos");
            case 20:
                return ok(Handler::kVv2op, "vexp2");
            case 21:
                return ok(Handler::kVv2op, "vlog2");
            case 22:
                return ok(Handler::kVv2op, "vsqrt");
            case 23:
                return ok(Handler::kVv2op, "vasin");
            case 24:
                return ok(Handler::kVv2op, "vnrcp");
            case 26:
                return ok(Handler::kVv2op, "vnsin");
            case 28:
                return ok(Handler::kVv2op, "vrexp2");
            default:
                return invalid;
            }
        }
        if (sub == 1) { // VFPU7
            switch ((op >> 16) & 0x1F) {
            case 0:
                return ok(Handler::kVrnds, "vrnds");
            case 1:
                return ok(Handler::kVrndx, "vrndi");
            case 2:
                return ok(Handler::kVrndx, "vrndf1");
            case 3:
                return ok(Handler::kVrndx, "vrndf2");
            case 18:
                return ok(Handler::kVf2h, "vf2h");
            case 19:
                return ok(Handler::kVh2f, "vh2f");
            case 22:
                return ok(Handler::kVsbz, "vsbz");
            case 23:
                return ok(Handler::kVlgb, "vlgb");
            case 24:
                return ok(Handler::kVx2i, "vuc2i");
            case 25:
                return ok(Handler::kVx2i, "vc2i");
            case 26:
                return ok(Handler::kVx2i, "vus2i");
            case 27:
                return ok(Handler::kVx2i, "vs2i");
            case 28:
                return ok(Handler::kVi2x, "vi2uc");
            case 29:
                return ok(Handler::kVi2x, "vi2c");
            case 30:
                return ok(Handler::kVi2x, "vi2us");
            case 31:
                return ok(Handler::kVi2x, "vi2s");
            default:
                return invalid;
            }
        }
        if (sub == 2) { // VFPU9
            switch ((op >> 16) & 0x1F) {
            case 0:
                return ok(Handler::kVsrt1, "vsrt1");
            case 1:
                return ok(Handler::kVsrt2, "vsrt2");
            case 2:
                return ok(Handler::kVbfy, "vbfy1");
            case 3:
                return ok(Handler::kVbfy, "vbfy2");
            case 4:
                return ok(Handler::kVocp, "vocp");
            case 5:
                return ok(Handler::kVsocp, "vsocp");
            case 6:
                return ok(Handler::kVfad, "vfad");
            case 7:
                return ok(Handler::kVavg, "vavg");
            case 8:
                return ok(Handler::kVsrt3, "vsrt3");
            case 9:
                return ok(Handler::kVsrt4, "vsrt4");
            case 10:
                return ok(Handler::kVsgn, "vsgn");
            case 16:
                return ok(Handler::kVmfvc, "vmfvc", false);
            case 17:
                return ok(Handler::kVmtvc, "vmtvc", false);
            case 25:
                return ok(Handler::kColorConv, "vt4444");
            case 26:
                return ok(Handler::kColorConv, "vt5551");
            case 27:
                return ok(Handler::kColorConv, "vt5650");
            default:
                return invalid;
            }
        }
        if (sub == 3) return ok(Handler::kVcst, "vcst");
        switch (sub) {
        case 16:
            return ok(Handler::kVf2i, "vf2in");
        case 17:
            return ok(Handler::kVf2i, "vf2iz");
        case 18:
            return ok(Handler::kVf2i, "vf2iu");
        case 19:
            return ok(Handler::kVf2i, "vf2id");
        case 20:
            return ok(Handler::kVi2f, "vi2f");
        case 21:
            return ok(Handler::kVcmov, ((op >> 19) & 1) ? "vcmovf" : "vcmovt");
        default:
            break;
        }
        if (sub >= 24) return ok(Handler::kVwbn, "vwbn");
        return invalid;
    }
    case 55: // VFPU5
        switch ((op >> 23) & 7) {
        case 0:
        case 1:
            return ok(Handler::kVpfx, "vpfxs", false);
        case 2:
        case 3:
            return ok(Handler::kVpfx, "vpfxt", false);
        case 4:
        case 5:
            return ok(Handler::kVpfx, "vpfxd", false);
        case 6:
            return ok(Handler::kViim, "viim.s", false);
        default:
            return ok(Handler::kViim, "vfim.s", false);
        }
    case 60: { // VFPU6
        const u32 sub = (op >> 21) & 0x1F;
        if (sub < 4) return ok(Handler::kVmmul, "vmmul");
        if (sub < 16) {
            // v(h)tfm2/3/4: homogeneous when the vector is one shorter than
            // the matrix. The suffix is the matrix (and destination) size.
            static const char *const kTfm[3] = {"vtfm2.p", "vtfm3.t", "vtfm4.q"};
            static const char *const kHtfm[3] = {"vhtfm2.p", "vhtfm3.t", "vhtfm4.q"};
            static const char *const kOdd[3] = {"vtfm2?", "vtfm3?", "vtfm4?"};
            const int ins = static_cast<int>((op >> 23) & 3);
            const int n = static_cast<int>(vec_size(op));
            const char *name = n == ins + 1 ? kTfm[ins - 1]
                               : n == ins   ? kHtfm[ins - 1]
                                            : kOdd[ins - 1];
            return ok(Handler::kVtfm, name, false);
        }
        if (sub < 20) return ok(Handler::kVmscl, "vmscl");
        if (sub < 24) {
            switch (vec_size(op)) {
            case kTriple:
                return ok(Handler::kCrossQuat, "vcrsp.t", false);
            case kQuad:
                return ok(Handler::kCrossQuat, "vqmul.q", false);
            default:
                return ok(Handler::kCrossQuat, "vcrsp");
            }
        }
        if (sub == 28) { // VFPUMatrixSet1
            switch ((op >> 16) & 0xF) {
            case 0:
                return ok(Handler::kVmmov, "vmmov");
            case 3:
                return ok(Handler::kVmatrixInit, "vmidt");
            case 6:
                return ok(Handler::kVmatrixInit, "vmzero");
            case 7:
                return ok(Handler::kVmatrixInit, "vmone");
            default:
                return invalid;
            }
        }
        if (sub == 29) return ok(Handler::kVrot, "vrot");
        return invalid;
    }
    case 63: // vflush/vnop/vsync
        return ok(Handler::kVflush, "vflush", false);
    default:
        return Decoded{};
    }
}

} // namespace

std::string mnemonic(u32 op) {
    const Decoded d = decode(op);
    if (d.handler == Handler::kNone || d.handler == Handler::kInvalid) return {};
    std::string name = d.name;
    if (d.sized) name += size_suffix(vec_size(op));
    return name;
}

Status execute(Regs r, u32 op, u32 *gpr, const Memory &memory, const Options &options) {
    const Decoded d = decode(op);
    switch (d.handler) {
    case Handler::kNone:
        return Status::kNotVfpu;
    case Handler::kInvalid:
        return Status::kInvalid;
    case Handler::kBranch:
        return Status::kBranch;
    case Handler::kVpfx:
        op_vpfx(r, op);
        break;
    case Handler::kSv:
        op_sv(r, op, gpr, memory);
        break;
    case Handler::kSvq:
        op_svq(r, op, gpr, memory);
        break;
    case Handler::kMftv:
        op_mftv(r, op, gpr);
        break;
    case Handler::kVmfvc:
        op_vmfvc(r, op);
        break;
    case Handler::kVmtvc:
        op_vmtvc(r, op);
        break;
    case Handler::kVmatrixInit:
        op_vmatrix_init(r, op);
        break;
    case Handler::kVvectorInit:
        op_vvector_init(r, op);
        break;
    case Handler::kViim:
        op_viim(r, op);
        break;
    case Handler::kVidt:
        op_vidt(r, op);
        break;
    case Handler::kVmmul:
        op_vmmul(r, op, options.more_accurate_vmmul);
        break;
    case Handler::kVmscl:
        op_vmscl(r, op);
        break;
    case Handler::kVmmov:
        op_vmmov(r, op);
        break;
    case Handler::kVflush:
        op_vflush(r, op);
        break;
    case Handler::kVv2op:
        op_vv2op(r, op);
        break;
    case Handler::kVocp:
        op_vocp(r, op);
        break;
    case Handler::kVsocp:
        op_vsocp(r, op);
        break;
    case Handler::kVsgn:
        op_vsgn(r, op);
        break;
    case Handler::kVf2i:
        op_vf2i(r, op);
        break;
    case Handler::kVi2f:
        op_vi2f(r, op);
        break;
    case Handler::kVh2f:
        op_vh2f(r, op);
        break;
    case Handler::kVf2h:
        op_vf2h(r, op);
        break;
    case Handler::kVx2i:
        op_vx2i(r, op);
        break;
    case Handler::kVi2x:
        op_vi2x(r, op);
        break;
    case Handler::kColorConv:
        op_color_conv(r, op);
        break;
    case Handler::kVdot:
        op_vdot(r, op);
        break;
    case Handler::kVhdp:
        op_vhdp(r, op);
        break;
    case Handler::kVbfy:
        op_vbfy(r, op);
        break;
    case Handler::kVsrt1:
        op_vsrt1(r, op);
        break;
    case Handler::kVsrt2:
        op_vsrt2(r, op);
        break;
    case Handler::kVsrt3:
        op_vsrt3(r, op);
        break;
    case Handler::kVsrt4:
        op_vsrt4(r, op);
        break;
    case Handler::kVcrs:
        op_vcrs(r, op);
        break;
    case Handler::kVdet:
        op_vdet(r, op);
        break;
    case Handler::kVfad:
        op_vfad(r, op);
        break;
    case Handler::kVavg:
        op_vavg(r, op);
        break;
    case Handler::kVscl:
        op_vscl(r, op);
        break;
    case Handler::kVrnds:
        op_vrnds(r, op);
        break;
    case Handler::kVrndx:
        op_vrndx(r, op);
        break;
    case Handler::kVrot:
        op_vrot(r, op);
        break;
    case Handler::kVtfm:
        op_vtfm(r, op);
        break;
    case Handler::kVcst:
        op_vcst(r, op);
        break;
    case Handler::kVcmp:
        op_vcmp(r, op);
        break;
    case Handler::kVminmax:
        op_vminmax(r, op);
        break;
    case Handler::kVscmp:
        op_vscmp(r, op);
        break;
    case Handler::kVsge:
        op_vsge(r, op);
        break;
    case Handler::kVslt:
        op_vslt(r, op);
        break;
    case Handler::kVcmov:
        op_vcmov(r, op);
        break;
    case Handler::kVecDo3:
        op_vec_do3(r, op);
        break;
    case Handler::kCrossQuat:
        op_cross_quat(r, op);
        break;
    case Handler::kVlgb:
        op_vlgb(r, op);
        break;
    case Handler::kVwbn:
        op_vwbn(r, op);
        break;
    case Handler::kVsbn:
        op_vsbn(r, op);
        break;
    case Handler::kVsbz:
        op_vsbz(r, op);
        break;
    }
    return Status::kOk;
}

} // namespace mpo::vfpu
