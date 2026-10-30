// SPDX-License-Identifier: GPL-3.0-or-later
//
// The volatile memory lock of sceSuspendForUser, ported from PPSSPP
// Core/HLE/scePower.cpp @ v1.20.4 (KernelVolatileMemLock/Unlock): the 4 MiB
// at 0x08400000, one owner, waiters served in order. The System owns it
// because the sceUtility dialogs (F18b) take it too, as PPSSPP's dialog jobs
// do, and the game waits for it while a dialog runs.

#pragma once

#include <cstdint>
#include <vector>

#include "kernel/kernel.hpp"

namespace mpo::hle {

class VolatileMemory {
public:
    static constexpr std::uint32_t kStart = 0x08400000u;
    static constexpr std::uint32_t kSize = 0x00400000u;
    static constexpr std::uint32_t kInvalidMode = 0x80000107u;
    static constexpr std::uint32_t kInUse = 0x802B0200u; // POWER_VMEM_IN_USE

    struct Waiter {
        kernel::Uid thread{};
        std::uint32_t addr_ptr{};
        std::uint32_t size_ptr{};
    };

    // KernelVolatileMemLock: 0, kInvalidMode or kInUse. Writes the area's
    // address and size to the pointers that are valid.
    std::uint32_t lock(kernel::Kernel &k, std::uint32_t type, std::uint32_t addr_ptr,
                       std::uint32_t size_ptr) {
        if (type != 0u) return kInvalidMode;
        if (locked_) return kInUse;
        if (k.memory().is_valid(addr_ptr, 4u)) k.memory().write_u32(addr_ptr, kStart);
        if (k.memory().is_valid(size_ptr, 4u)) k.memory().write_u32(size_ptr, kSize);
        locked_ = true;
        return 0u;
    }

    // KernelVolatileMemUnlock: 0, kInvalidMode or SEMA_OVF when it was not
    // locked. Hands the lock over to the first waiter still waiting.
    std::uint32_t unlock(kernel::Kernel &k, std::uint32_t type) {
        if (type != 0u) return kInvalidMode;
        if (!locked_) return kernel::error::kSemaOvf;
        locked_ = false;
        while (!waiters_.empty()) {
            const Waiter waiter = waiters_.front();
            waiters_.erase(waiters_.begin());
            if (!k.is_waiting_for(waiter.thread, kernel::WaitType::kVmem, 1)) continue;
            if (lock(k, 0u, waiter.addr_ptr, waiter.size_ptr) == 0u) {
                k.resume_from_wait(waiter.thread, 0u);
                break;
            }
        }
        return 0u;
    }

    void add_waiter(const Waiter &waiter) { waiters_.push_back(waiter); }
    [[nodiscard]] bool locked() const noexcept { return locked_; }

private:
    bool locked_ = false;
    std::vector<Waiter> waiters_;
};

} // namespace mpo::hle
