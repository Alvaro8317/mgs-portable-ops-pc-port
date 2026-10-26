// SPDX-License-Identifier: GPL-3.0-or-later

#include "recomp/vfpu_executor.hpp"

#include <string>

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "vfpu/vfpu.hpp"

namespace mpo::recomp {
namespace {

struct MemoryAccess {
    psprecomp::GuestMemory *memory = nullptr;
    bool fault = false;
    std::uint32_t fault_address = 0;
};

std::uint32_t read32(void *user, std::uint32_t address) {
    auto *access = static_cast<MemoryAccess *>(user);
    if (!access->memory->contains(address, 4u)) {
        if (!access->fault) access->fault_address = address;
        access->fault = true;
        return 0u;
    }
    return access->memory->load32(address);
}

void write32(void *user, std::uint32_t address, std::uint32_t value) {
    auto *access = static_cast<MemoryAccess *>(user);
    if (!access->memory->contains(address, 4u)) {
        if (!access->fault) access->fault_address = address;
        access->fault = true;
        return;
    }
    access->memory->store32(address, value);
}

} // namespace

bool execute_vfpu(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, std::uint32_t pc,
                  std::uint32_t word) {
    // AllegrexContext::vfpu uses PPSSPP's voffset order, like mpo::vfpu::Regs.
    static_assert(sizeof(ctx.vfpu) == sizeof(float) * mpo::vfpu::kNumRegs);
    static_assert(sizeof(ctx.vfpu_ctrl) == sizeof(std::uint32_t) * mpo::vfpu::kNumCtrl);
    MemoryAccess access{&runtime.memory()};
    const mpo::vfpu::Memory memory{&access, &read32, &write32};
    const mpo::vfpu::Regs regs{ctx.vfpu.data(), ctx.vfpu_ctrl.data()};
    const mpo::vfpu::Status status = mpo::vfpu::execute(regs, word, ctx.gpr.data(), memory);
    // mfv/viim never target $zero in valid code, but keep the invariant.
    ctx.gpr[0] = 0u;
    if (status != mpo::vfpu::Status::kOk) return false;
    if (access.fault) {
        ctx.pc = pc;
        runtime.stop("VFPU access outside guest memory at " +
                     psprecomp::hex32(access.fault_address) + " by " + psprecomp::hex32(word) +
                     " at " + psprecomp::hex32(pc));
        return false;
    }
    return true;
}

void install_vfpu_executor() {
    psprecomp::set_vfpu_executor(&execute_vfpu);
}

} // namespace mpo::recomp
