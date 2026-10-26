// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glue that runs PPSSPP's unmodified interpreter sources inside the difftest:
// the globals MIPSState/voffset/cst_constants (MIPS.cpp is not compiled), the
// guest memory, the VFS for the VFPU tables and no-op stand-ins for the parts
// of PPSSPP the interpreter only touches for logging or the JIT.
//
// cst_constants is copied from PPSSPP Core/MIPS/MIPS.cpp @ v1.20.4
// (GPL-2.0-or-later).

#define _USE_MATH_DEFINES

#include "ppsspp_ref/ppsspp_ref.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include "Common/File/VFS/VFS.h"
#include "Common/StringUtils.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/MemMap.h"
#include "Core/Reporting.h"
#include "Core/System.h"

// From the extracted MIPSTables.cpp.
extern bool g_mpo_ref_unsupported;

// --- PPSSPP globals normally defined in MIPS.cpp ----------------------------

MIPSState mipsr4k;
MIPSState *currentMIPS = &mipsr4k;
MIPSDebugInterface *currentDebugMIPS = nullptr;
u8 voffset[128];
u8 fromvoffset[128];

#ifndef M_LOG2E
#error "M_* constants are required (copied expressions from MIPS.cpp)"
#endif

const float cst_constants[32] = {
    0,
    std::numeric_limits<float>::max(), // all these are verified on real PSP
    sqrtf(2.0f),
    sqrtf(0.5f),
    2.0f / sqrtf((float)M_PI),
    2.0f / (float)M_PI,
    1.0f / (float)M_PI,
    (float)M_PI / 4,
    (float)M_PI / 2,
    (float)M_PI,
    (float)M_E,
    (float)M_LOG2E,
    (float)M_LOG10E,
    (float)M_LN2,
    (float)M_LN10,
    2 * (float)M_PI,
    (float)M_PI / 6,
    log10f(2.0f),
    logf(10.0f) / logf(2.0f),
    sqrtf(3.0f) / 2.0f,
};

MIPSState::MIPSState() {}
MIPSState::~MIPSState() {}

namespace MIPSComp {
JitInterface *jit = nullptr;
}

// --- Guest memory -----------------------------------------------------------

namespace {

mpo::difftest::CpuState *g_state = nullptr;
std::filesystem::path g_vfs_root;
MpoCoreParameterShim g_core_parameter;

} // namespace

namespace Memory {

u8 Read_U8(u32 address) {
    return static_cast<u8>(g_state->read8(address));
}
u16 Read_U16(u32 address) {
    return static_cast<u16>(g_state->read8(address) | (g_state->read8(address + 1) << 8));
}
u32 Read_U32(u32 address) {
    return g_state->read32(address);
}
float Read_Float(u32 address) {
    const u32 bits = g_state->read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
void Write_U8(u8 data, u32 address) {
    g_state->write8(address, data);
}
void Write_U16(u16 data, u32 address) {
    g_state->write8(address, data);
    g_state->write8(address + 1, static_cast<u32>(data >> 8));
}
void Write_U32(u32 data, u32 address) {
    g_state->write32(address, data);
}
void Write_Float(float data, u32 address) {
    u32 bits;
    std::memcpy(&bits, &data, sizeof(bits));
    g_state->write32(address, bits);
}

namespace {

u8 *window(u32 address, u32 size) {
    using mpo::difftest::kMemBase;
    using mpo::difftest::kMemSize;
    if (address < kMemBase || address - kMemBase > kMemSize - size) {
        g_state->mem_faults += size / 4;
        return nullptr;
    }
    return g_state->mem.data() + (address - kMemBase);
}

} // namespace

const u8 *GetPointerRange(u32 address, u32 size) {
    return window(address, size);
}
u8 *GetPointerWriteRange(u32 address, u32 size) {
    return window(address, size);
}
bool IsValidAddress(u32) {
    return true;
}
Opcode Read_Instruction(u32, bool) {
    return Opcode(0);
}
Opcode Read_Opcode_JIT(u32) {
    return Opcode(0);
}

} // namespace Memory

// --- Stand-ins --------------------------------------------------------------

void Core_ExecException(u32, u32, ExecExceptionType) {}
void Core_BreakException(u32) {}
void CallSyscall(MIPSOpcode) {}
const ReplacementTableEntry *GetReplacementFunc(size_t) {
    return nullptr;
}
std::string MIPSDisasmAt(u32) {
    return {};
}
MpoCoreParameterShim &PSP_CoreParameter() {
    return g_core_parameter;
}

namespace Reporting {
void ReportMessage(const char *, ...) {}
} // namespace Reporting

std::string StringFromFormat(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

void truncate_cpy(char *dest, std::size_t dest_size, const char *src) {
    std::snprintf(dest, dest_size, "%s", src);
}

MpoVfsShim g_VFS;

uint8_t *MpoVfsShim::ReadFile(const char *filename, size_t *size) {
    const std::filesystem::path path = g_vfs_root / filename;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "mpo_ppsspp_ref: missing VFPU table %s\n", path.string().c_str());
        std::abort();
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto *data = new uint8_t[bytes.size()];
    std::memcpy(data, bytes.data(), bytes.size());
    *size = bytes.size();
    return data;
}

// --- Executor -----------------------------------------------------------------

namespace mpo::ppsspp_ref {

bool init(const std::filesystem::path &vfpu_tables_dir, std::string *error) {
    // MIPSState::MIPSState(): the voffset reordering.
    int i = 0;
    for (int m = 0; m < 8; m++) {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) voffset[m * 4 + x * 32 + y] = static_cast<u8>(i++);
        }
    }
    for (int k = 0; k < 128; k++) fromvoffset[voffset[k]] = static_cast<u8>(k);

    g_vfs_root = vfpu_tables_dir.parent_path();
    std::error_code ec;
    if (!std::filesystem::exists(vfpu_tables_dir / "vfpu_sin_lut8192.dat", ec)) {
        if (error) *error = "no VFPU tables in " + vfpu_tables_dir.string();
        return false;
    }
    return true;
}

namespace {

class PpssppExecutor final : public difftest::Executor {
public:
    std::string name() const override { return "ppsspp"; }

    bool run(std::span<const difftest::TestCase> cases, std::span<difftest::CpuState> out,
             std::span<std::uint8_t> supported, std::string *) override {
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const difftest::TestCase &c = cases[i];
            const difftest::Spec *spec = difftest::find_spec(c.spec);
            g_core_parameter.compat.flags_.MoreAccurateVMMUL =
                spec && (spec->flags & difftest::kSpecMoreAccurateVmmul);
            out[i] = c.initial;
            supported[i] = 1;
            for (int k = 0; k < c.num_ops && supported[i]; ++k) {
                if (!step(out[i], c.ops[static_cast<std::size_t>(k)])) supported[i] = 0;
            }
        }
        return true;
    }

private:
    static bool step(difftest::CpuState &s, u32 op) {
        MIPSState &m = mipsr4k;
        std::memcpy(m.r, s.gpr, sizeof(m.r));
        std::memcpy(m.fi, s.fpr, sizeof(m.fi));
        static_assert(sizeof(m.v) == sizeof(s.v));
        std::memcpy(m.v, s.v, sizeof(m.v));
        std::memcpy(m.vfpuCtrl, s.vfpu_ctrl, sizeof(m.vfpuCtrl));
        m.hi = s.hi;
        m.lo = s.lo;
        m.fcr31 = s.fcr31 & ~(1u << 23);
        m.fpcond = (s.fcr31 >> 23) & 1;
        m.pc = s.pc;
        m.nextPC = s.pc + 4;
        m.inDelaySlot = false;

        g_state = &s;
        g_mpo_ref_unsupported = false;
        MIPSInterpret(MIPSOpcode(op));
        g_state = nullptr;
        if (g_mpo_ref_unsupported) return false;

        std::memcpy(s.gpr, m.r, sizeof(m.r));
        std::memcpy(s.fpr, m.fi, sizeof(m.fi));
        std::memcpy(s.v, m.v, sizeof(m.v));
        std::memcpy(s.vfpu_ctrl, m.vfpuCtrl, sizeof(m.vfpuCtrl));
        s.hi = m.hi;
        s.lo = m.lo;
        s.fcr31 = (m.fcr31 & ~(1u << 23)) | ((m.fpcond & 1) << 23);
        // A taken branch leaves its target in nextPC (DelayBranchTo).
        s.pc = m.inDelaySlot ? m.nextPC : m.pc;
        return true;
    }
};

} // namespace

std::unique_ptr<difftest::Executor> make_executor() {
    return std::make_unique<PpssppExecutor>();
}

float call(golden::Function fn, const float *args) {
    switch (fn) {
    case golden::Function::kSin:
        return vfpu_sin(args[0]);
    case golden::Function::kCos:
        return vfpu_cos(args[0]);
    case golden::Function::kAsin:
        return vfpu_asin(args[0]);
    case golden::Function::kSqrt:
        return vfpu_sqrt(args[0]);
    case golden::Function::kRsqrt:
        return vfpu_rsqrt(args[0]);
    case golden::Function::kExp2:
        return vfpu_exp2(args[0]);
    case golden::Function::kRexp2:
        return vfpu_rexp2(args[0]);
    case golden::Function::kLog2:
        return vfpu_log2(args[0]);
    case golden::Function::kRcp:
        return vfpu_rcp(args[0]);
    case golden::Function::kDot:
        return vfpu_dot(args, args + 4);
    }
    return 0.0f;
}

} // namespace mpo::ppsspp_ref
