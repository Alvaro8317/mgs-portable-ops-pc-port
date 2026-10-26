// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vfpu: state and semantics of the Allegrex VFPU, ported bit-exact from
// the PPSSPP interpreter.
//
// Ported from PPSSPP Core/MIPS/MIPSIntVFPU.cpp, Core/MIPS/MIPSVFPUUtils.h and
// Core/MIPS/MIPSVFPUUtils.cpp @ v1.20.4 (fa50bb19, GPL-2.0-or-later).
//
// The library is a narrow, game-independent API (docs/PLAN.md, "olas"): it
// only sees a view of the VFPU registers, the GPRs it is told about and a
// memory callback. It never registers HLE functions.
//
// Every op_* function executes one already-decoded VFPU instruction exactly
// like PPSSPP's Int_* counterpart, except that the PC is not touched: the
// caller (interpreter or generated code) owns control flow.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mpo::vfpu {

inline constexpr int kNumRegs = 128;
inline constexpr int kNumCtrl = 16;

// VFPU control registers (mfvc/mtvc 128 + index).
enum Ctrl : int {
    kCtrlSPrefix = 0,
    kCtrlTPrefix = 1,
    kCtrlDPrefix = 2,
    kCtrlCc = 3,
    kCtrlInf4 = 4,
    kCtrlRsv5 = 5,
    kCtrlRsv6 = 6,
    kCtrlRev = 7,
    kCtrlRcx0 = 8,
    kCtrlRcx7 = 15,
};

// View of the 128 VFPU registers and the 16 control registers.
//
// v[] uses PPSSPP's "voffset" order, which is also the order of
// psprecomp::AllegrexContext::vfpu: the encoded register
// 0b0RRMMMCC (row R, matrix M, column C) lives at v[M * 16 + C * 4 + R].
// A view lets the runtime point at its own context without copies.
struct Regs {
    float *v;
    std::uint32_t *ctrl;
};

// Owning storage, initialised like PPSSPP's MIPSState::Reset().
struct State {
    alignas(16) float v[kNumRegs]{};
    std::uint32_t ctrl[kNumCtrl]{};

    State() { reset(); }
    void reset();
    [[nodiscard]] Regs regs() { return {v, ctrl}; }
};

// Index into Regs::v of an encoded single register (0..127).
[[nodiscard]] constexpr int voffset(int reg) {
    return ((reg >> 2) & 7) * 16 + (reg & 3) * 4 + ((reg >> 5) & 3);
}

// Guest memory for lv/sv. Addresses are full 32-bit guest addresses.
// lv.q/sv.q never fault here: the caller decides what an invalid address does.
struct Memory {
    void *user = nullptr;
    std::uint32_t (*read32)(void *user, std::uint32_t address) = nullptr;
    void (*write32)(void *user, std::uint32_t address, std::uint32_t value) = nullptr;
};

// PPSSPP per-game compatibility flags that change VFPU results. Both are off
// for Metal Gear Solid: Portable Ops in PPSSPP v1.20.4 (assets/compat.ini).
struct Options {
    bool more_accurate_vmmul = false;
};

enum class Status {
    kOk,
    kNotVfpu, // The word is not a VFPU instruction this library executes.
    kBranch,  // bvf/bvt/bvfl/bvtl: use branch_taken() instead.
    kInvalid, // VFPU opcode space, but PPSSPP's table marks it invalid.
};

// Executes one VFPU instruction. gpr points at the 32 GPRs (mfv/mtv/lv/sv).
Status execute(Regs r, std::uint32_t op, std::uint32_t *gpr, const Memory &memory,
               const Options &options = {});

// Returns the PPSSPP mnemonic with its size suffix (e.g. "vadd.q", "vtfm4.q",
// "lv.s"), or an empty string for words execute() would not accept.
[[nodiscard]] std::string mnemonic(std::uint32_t op);

// bvf/bvt/bvfl/bvtl: whether the branch is taken for the current CC.
[[nodiscard]] bool branch_taken(const Regs &r, std::uint32_t op);

// --- Lookup tables for the transcendental helpers -------------------------
//
// vsin/vcos/vrsq/vsqrt/vasin/vexp2/vlog2/vrcp (and vrot) need PPSSPP's
// assets/vfpu/*.dat tables. They are fetched at configure time (see
// third_party/ppsspp_vfpu_tables) and must be loaded once before use. Calling
// one of those helpers without tables aborts with a message: there is no
// silent approximate fallback.
bool load_tables(const std::filesystem::path &directory, std::string *error);
[[nodiscard]] bool tables_loaded();

// --- Register access (PPSSPP ReadVector/WriteVector/ReadMatrix/WriteMatrix) -

enum VectorSize { kSingle = 1, kPair = 2, kTriple = 3, kQuad = 4 };

[[nodiscard]] constexpr VectorSize vec_size(std::uint32_t op) {
    return static_cast<VectorSize>(((op >> 7) & 1) + ((op >> 14) & 2) + 1);
}

void read_vector(const Regs &r, float *rd, VectorSize size, int reg);
// Honours the write mask of the D prefix, like PPSSPP.
void write_vector(const Regs &r, const float *rd, VectorSize size, int reg);
void read_matrix(const Regs &r, float *rd, VectorSize size, int reg);
void write_matrix(const Regs &r, const float *rd, VectorSize size, int reg);

// Resets the S/T/D prefixes after a prefix-consuming instruction.
inline void eat_prefixes(const Regs &r) {
    r.ctrl[kCtrlSPrefix] = 0xE4;
    r.ctrl[kCtrlTPrefix] = 0xE4;
    r.ctrl[kCtrlDPrefix] = 0;
}

// --- Instructions (one per PPSSPP Int_* handler) ---------------------------

void op_vpfx(const Regs &r, std::uint32_t op);
void op_sv(const Regs &r, std::uint32_t op, const std::uint32_t *gpr, const Memory &memory);
void op_svq(const Regs &r, std::uint32_t op, const std::uint32_t *gpr, const Memory &memory);
void op_mftv(const Regs &r, std::uint32_t op, std::uint32_t *gpr);
void op_vmfvc(const Regs &r, std::uint32_t op);
void op_vmtvc(const Regs &r, std::uint32_t op);
void op_vmatrix_init(const Regs &r, std::uint32_t op);
void op_vvector_init(const Regs &r, std::uint32_t op);
void op_viim(const Regs &r, std::uint32_t op);
void op_vidt(const Regs &r, std::uint32_t op);
void op_vmmul(const Regs &r, std::uint32_t op, bool more_accurate_vmmul);
void op_vmscl(const Regs &r, std::uint32_t op);
void op_vmmov(const Regs &r, std::uint32_t op);
void op_vflush(const Regs &r, std::uint32_t op);
void op_vv2op(const Regs &r, std::uint32_t op);
void op_vocp(const Regs &r, std::uint32_t op);
void op_vsocp(const Regs &r, std::uint32_t op);
void op_vsgn(const Regs &r, std::uint32_t op);
void op_vf2i(const Regs &r, std::uint32_t op);
void op_vi2f(const Regs &r, std::uint32_t op);
void op_vh2f(const Regs &r, std::uint32_t op);
void op_vf2h(const Regs &r, std::uint32_t op);
void op_vx2i(const Regs &r, std::uint32_t op);
void op_vi2x(const Regs &r, std::uint32_t op);
void op_color_conv(const Regs &r, std::uint32_t op);
void op_vdot(const Regs &r, std::uint32_t op);
void op_vhdp(const Regs &r, std::uint32_t op);
void op_vbfy(const Regs &r, std::uint32_t op);
void op_vsrt1(const Regs &r, std::uint32_t op);
void op_vsrt2(const Regs &r, std::uint32_t op);
void op_vsrt3(const Regs &r, std::uint32_t op);
void op_vsrt4(const Regs &r, std::uint32_t op);
void op_vcrs(const Regs &r, std::uint32_t op);
void op_vdet(const Regs &r, std::uint32_t op);
void op_vfad(const Regs &r, std::uint32_t op);
void op_vavg(const Regs &r, std::uint32_t op);
void op_vscl(const Regs &r, std::uint32_t op);
void op_vrnds(const Regs &r, std::uint32_t op);
void op_vrndx(const Regs &r, std::uint32_t op);
void op_vrot(const Regs &r, std::uint32_t op);
void op_vtfm(const Regs &r, std::uint32_t op);
void op_vcst(const Regs &r, std::uint32_t op);
void op_vcmp(const Regs &r, std::uint32_t op);
void op_vminmax(const Regs &r, std::uint32_t op);
void op_vscmp(const Regs &r, std::uint32_t op);
void op_vsge(const Regs &r, std::uint32_t op);
void op_vslt(const Regs &r, std::uint32_t op);
void op_vcmov(const Regs &r, std::uint32_t op);
void op_vec_do3(const Regs &r, std::uint32_t op);
void op_cross_quat(const Regs &r, std::uint32_t op);
void op_vlgb(const Regs &r, std::uint32_t op);
void op_vwbn(const Regs &r, std::uint32_t op);
void op_vsbn(const Regs &r, std::uint32_t op);
void op_vsbz(const Regs &r, std::uint32_t op);

} // namespace mpo::vfpu
