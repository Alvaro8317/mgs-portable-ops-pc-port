// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kernel NIDs (F08b): ThreadManForUser, SysMemUserForUser,
// Kernel_Library, LoadExecForUser, the cache and GPI calls of UtilsForUser,
// sceSuspendForUser and StdioForUser, plus ModuleMgrForUser and
// sceUtilityLoad(Av)Module (the module loader).
//
// Each wrapper only moves registers: mpo_kernel (F08a) holds the semantics of
// PPSSPP Core/HLE/sceKernel*.cpp @ v1.20.4. The few calls implemented here
// (printf, volatile memory, the module manager glue) are ported from the same
// tag; the source file is named at each one.

#include <bit>
#include <cstdio>
#include <string>
#include <vector>

#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kInvalidMode = 0x80000107u;
constexpr std::uint32_t kPowerVmemInUse = 0x802B0200u;
constexpr std::uint32_t kCanNotWait = kernel::error::kCanNotWait;
constexpr std::uint32_t kIllegalContext = kernel::error::kIllegalContext;

// Ported from PPSSPP Core/HLE/sceKernelMemory.cpp @ v1.20.4 (sceKernelPrintf):
// %%, %s, %d/%i/%x/%X/%u, %08x/%08X and %p, arguments from $a1 on.
std::string format_printf(kernel::Kernel &k, const Ctx &ctx, const std::string &format) {
    std::string result;
    int param = 1;
    char text[24];
    for (std::size_t i = 0, n = format.size(); i < n;) {
        const std::size_t next = format.find('%', i);
        if (next == std::string::npos) {
            result += format.substr(i);
            break;
        }
        result += format.substr(i, next - i);
        i = next + 1u;
        if (i >= n) break;
        const char c = format[i];
        if (c == '%') {
            result += '%';
            ++i;
        } else if (c == 's') {
            const GuestString s = guest_string(k, arg(ctx, param++));
            result += s.present ? s.text : "(null)";
            ++i;
        } else if (c == 'd' || c == 'i' || c == 'x' || c == 'X' || c == 'u') {
            if (c == 'd' || c == 'i')
                std::snprintf(text, sizeof(text), "%d", static_cast<int>(arg(ctx, param++)));
            else
                std::snprintf(text, sizeof(text), c == 'u' ? "%u" : (c == 'x' ? "%x" : "%X"),
                              arg(ctx, param++));
            result += text;
            ++i;
        } else if (c == '0' && i + 2u < n && format[i + 1u] == '8' &&
                   (format[i + 2u] == 'x' || format[i + 2u] == 'X')) {
            std::snprintf(text, sizeof(text), format[i + 2u] == 'x' ? "%08x" : "%08X",
                          arg(ctx, param++));
            result += text;
            i += 3u;
        } else if (c == 'p') {
            std::snprintf(text, sizeof(text), "%08x", arg(ctx, param++));
            result += text;
            ++i;
        } else {
            // Unsupported conversion: print the rest raw, like PPSSPP.
            result += format.substr(next);
            break;
        }
    }
    return result;
}

// sceSuspendForUser volatile memory lock (PPSSPP Core/HLE/scePower.cpp
// @ v1.20.4): 0x08400000, 4 MiB, one owner, waiters served in order.
struct VolatileMemory {
    bool locked{};
    struct Waiter {
        kernel::Uid thread;
        std::uint32_t addr_ptr;
        std::uint32_t size_ptr;
    };
    std::vector<Waiter> waiters;
};

std::uint32_t volatile_lock(kernel::Kernel &k, VolatileMemory &vm, std::uint32_t type,
                            std::uint32_t addr_ptr, std::uint32_t size_ptr) {
    if (type != 0u) return kInvalidMode;
    if (vm.locked) return kPowerVmemInUse;
    if (k.memory().is_valid(addr_ptr, 4u)) k.memory().write_u32(addr_ptr, 0x08400000u);
    if (k.memory().is_valid(size_ptr, 4u)) k.memory().write_u32(size_ptr, 0x00400000u);
    vm.locked = true;
    return 0u;
}

void register_threadman(System &system) {
    kernel::Kernel &k = system.kernel();
    HleTable &t = system.hle();
    const std::string lib = "ThreadManForUser";
    const auto def = [&t, &lib](std::uint32_t nid, const char *name, HleTable::Handler handler,
                                bool may_not_return = false) {
        t.add(lib, nid, name, HleKind::kImplemented, std::move(handler), may_not_return);
    };
    // Threads.
    def(0x446D8DE6u, "sceKernelCreateThread", [&k](Ctx &c) {
        const GuestString name = guest_string(k, arg(c, 0));
        ret(c,
            k.create_thread(name.c_str(), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)));
    });
    def(0xF475845Du, "sceKernelStartThread", [&k](Ctx &c) {
        ret(c, k.start_thread(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1), arg(c, 2)));
    });
    def(
        0xAA73C935u, "sceKernelExitThread", [&k](Ctx &c) { ret(c, k.exit_thread(arg(c, 0))); },
        true);
    def(
        0x809CE29Bu, "sceKernelExitDeleteThread",
        [&k](Ctx &c) { ret(c, k.exit_delete_thread(arg(c, 0))); }, true);
    def(0x9FA03CD3u, "sceKernelDeleteThread",
        [&k](Ctx &c) { ret(c, k.delete_thread(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0x616403BAu, "sceKernelTerminateThread",
        [&k](Ctx &c) { ret(c, k.terminate_thread(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0x293B45B8u, "sceKernelGetThreadId", [&k](Ctx &c) { ret(c, k.get_thread_id()); });
    def(0xEA748E31u, "sceKernelChangeCurrentThreadAttr",
        [&k](Ctx &c) { ret(c, k.change_current_thread_attr(arg(c, 0), arg(c, 1))); });
    def(0x17C1684Eu, "sceKernelReferThreadStatus", [&k](Ctx &c) {
        ret(c, k.refer_thread_status(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1)));
    });
    def(0x3B183E26u, "sceKernelGetThreadExitStatus",
        [&k](Ctx &c) { ret(c, k.get_thread_exit_status(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0xCEADEB47u, "sceKernelDelayThread", [&k](Ctx &c) { ret(c, k.delay_thread(arg(c, 0))); });
    def(0x68DA9E36u, "sceKernelDelayThreadCB",
        [&k](Ctx &c) { ret(c, k.delay_thread_cb(arg(c, 0))); });
    def(0x82826F70u, "sceKernelSleepThreadCB", [&k](Ctx &c) { ret(c, k.sleep_thread_cb()); });
    def(0xD59EAD2Fu, "sceKernelWakeupThread",
        [&k](Ctx &c) { ret(c, k.wakeup_thread(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0xFCCFAD26u, "sceKernelCancelWakeupThread",
        [&k](Ctx &c) { ret(c, k.cancel_wakeup_thread(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0x2C34E053u, "sceKernelReleaseWaitThread",
        [&k](Ctx &c) { ret(c, k.release_wait_thread(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0x278C0DF5u, "sceKernelWaitThreadEnd", [&k](Ctx &c) {
        ret(c, k.wait_thread_end(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1)));
    });
    def(0x840E8133u, "sceKernelWaitThreadEndCB", [&k](Ctx &c) {
        ret(c, k.wait_thread_end_cb(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1)));
    });
    def(0x82BC5777u, "sceKernelGetSystemTimeWide",
        [&k](Ctx &c) { ret64(c, k.get_system_time_wide()); });
    // Callbacks.
    def(0xE81CAF8Fu, "sceKernelCreateCallback", [&k](Ctx &c) {
        const GuestString name = guest_string(k, arg(c, 0));
        ret(c, k.create_callback(name.c_str(), arg(c, 1), arg(c, 2)));
    });
    // Semaphores.
    def(0xD6DA4BA1u, "sceKernelCreateSema", [&k](Ctx &c) {
        const GuestString name = guest_string(k, arg(c, 0));
        ret(c, k.create_sema(name.c_str(), arg(c, 1), static_cast<std::int32_t>(arg(c, 2)),
                             static_cast<std::int32_t>(arg(c, 3)), arg(c, 4)));
    });
    def(0x28B6489Cu, "sceKernelDeleteSema",
        [&k](Ctx &c) { ret(c, k.delete_sema(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0x3F53E640u, "sceKernelSignalSema", [&k](Ctx &c) {
        ret(c, k.signal_sema(static_cast<kernel::Uid>(arg(c, 0)),
                             static_cast<std::int32_t>(arg(c, 1))));
    });
    def(0x4E3A1105u, "sceKernelWaitSema", [&k](Ctx &c) {
        ret(c, k.wait_sema(static_cast<kernel::Uid>(arg(c, 0)),
                           static_cast<std::int32_t>(arg(c, 1)), arg(c, 2)));
    });
    def(0x6D212BACu, "sceKernelWaitSemaCB", [&k](Ctx &c) {
        ret(c, k.wait_sema_cb(static_cast<kernel::Uid>(arg(c, 0)),
                              static_cast<std::int32_t>(arg(c, 1)), arg(c, 2)));
    });
    def(0x58B1F937u, "sceKernelPollSema", [&k](Ctx &c) {
        ret(c,
            k.poll_sema(static_cast<kernel::Uid>(arg(c, 0)), static_cast<std::int32_t>(arg(c, 1))));
    });
    // Event flags.
    def(0x55C20A00u, "sceKernelCreateEventFlag", [&k](Ctx &c) {
        const GuestString name = guest_string(k, arg(c, 0));
        ret(c, k.create_event_flag(name.c_str(), arg(c, 1), arg(c, 2), arg(c, 3)));
    });
    def(0xEF9E4C70u, "sceKernelDeleteEventFlag",
        [&k](Ctx &c) { ret(c, k.delete_event_flag(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0x1FB15A32u, "sceKernelSetEventFlag",
        [&k](Ctx &c) { ret(c, k.set_event_flag(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1))); });
    def(0x812346E4u, "sceKernelClearEventFlag", [&k](Ctx &c) {
        ret(c, k.clear_event_flag(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1)));
    });
    def(0x402FCF22u, "sceKernelWaitEventFlag", [&k](Ctx &c) {
        ret(c, k.wait_event_flag(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1), arg(c, 2),
                                 arg(c, 3), arg(c, 4)));
    });
    def(0x328C546Au, "sceKernelWaitEventFlagCB", [&k](Ctx &c) {
        ret(c, k.wait_event_flag_cb(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1), arg(c, 2),
                                    arg(c, 3), arg(c, 4)));
    });
    // Fixed-length pools.
    def(0xC07BB470u, "sceKernelCreateFpl", [&k](Ctx &c) {
        const GuestString name = guest_string(k, arg(c, 0));
        ret(c, k.create_fpl(name.c_str(), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)));
    });
    def(0xED1410E0u, "sceKernelDeleteFpl",
        [&k](Ctx &c) { ret(c, k.delete_fpl(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0xD979E9BFu, "sceKernelAllocateFpl", [&k](Ctx &c) {
        ret(c, k.allocate_fpl(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1), arg(c, 2)));
    });
    def(0xF6414A71u, "sceKernelFreeFpl",
        [&k](Ctx &c) { ret(c, k.free_fpl(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1))); });
    // Variable-length pools.
    def(0x56C039B5u, "sceKernelCreateVpl", [&k](Ctx &c) {
        const GuestString name = guest_string(k, arg(c, 0));
        ret(c, k.create_vpl(name.c_str(), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
    });
    def(0x89B3D48Cu, "sceKernelDeleteVpl",
        [&k](Ctx &c) { ret(c, k.delete_vpl(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(0xAF36D708u, "sceKernelTryAllocateVpl", [&k](Ctx &c) {
        ret(c, k.try_allocate_vpl(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1), arg(c, 2)));
    });
    def(0xB736E9FFu, "sceKernelFreeVpl",
        [&k](Ctx &c) { ret(c, k.free_vpl(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1))); });
}

void register_sysmem_and_friends(System &system) {
    kernel::Kernel &k = system.kernel();
    HleTable &t = system.hle();
    const auto def = [&t](const char *lib, std::uint32_t nid, const char *name,
                          HleTable::Handler handler, bool may_not_return = false) {
        t.add(lib, nid, name, HleKind::kImplemented, std::move(handler), may_not_return);
    };
    // SysMemUserForUser.
    def("SysMemUserForUser", 0x7591C7DBu, "sceKernelSetCompiledSdkVersion",
        [&k](Ctx &c) { ret(c, k.set_compiled_sdk_version(arg(c, 0))); });
    def("SysMemUserForUser", 0xF77D77CBu, "sceKernelSetCompilerVersion",
        [&k](Ctx &c) { ret(c, k.set_compiler_version(arg(c, 0))); });
    def("SysMemUserForUser", 0x237DBD4Fu, "sceKernelAllocPartitionMemory", [&k](Ctx &c) {
        const GuestString name = guest_string(k, arg(c, 1));
        ret(c, k.alloc_partition_memory(arg(c, 0), name.c_str(), arg(c, 2), arg(c, 3), arg(c, 4)));
    });
    def("SysMemUserForUser", 0xB6D61D02u, "sceKernelFreePartitionMemory",
        [&k](Ctx &c) { ret(c, k.free_partition_memory(static_cast<kernel::Uid>(arg(c, 0)))); });
    def("SysMemUserForUser", 0x9D9A5BA1u, "sceKernelGetBlockHeadAddr",
        [&k](Ctx &c) { ret(c, k.get_block_head_addr(static_cast<kernel::Uid>(arg(c, 0)))); });
    def("SysMemUserForUser", 0xA291F107u, "sceKernelMaxFreeMemSize",
        [&k](Ctx &c) { ret(c, k.max_free_mem_size()); });
    def("SysMemUserForUser", 0xF919F628u, "sceKernelTotalFreeMemSize",
        [&k](Ctx &c) { ret(c, k.total_free_mem_size()); });
    def("SysMemUserForUser", 0x13A5ABEFu, "sceKernelPrintf", [&k, &system](Ctx &c) {
        const GuestString format = guest_string(k, arg(c, 0));
        if (!format.present) {
            ret(c, 0xFFFFFFFFu);
            return;
        }
        std::string text = format_printf(k, c, format.text);
        if (!text.empty() && text.back() == '\n') text.pop_back();
        std::fprintf(stderr, "[guest printf] %s\n", text.c_str());
        system.hle().trace_event("printf: %s", text.c_str());
        ret(c, 0u);
    });
    // Kernel_Library.
    def("Kernel_Library", 0x092968F4u, "sceKernelCpuSuspendIntr",
        [&k](Ctx &c) { ret(c, k.cpu_suspend_intr()); });
    def("Kernel_Library", 0x5F10D406u, "sceKernelCpuResumeIntr", [&k](Ctx &c) {
        k.cpu_resume_intr(arg(c, 0));
        ret(c, 0u);
    });
    // LoadExecForUser.
    def("LoadExecForUser", 0x4AC57943u, "sceKernelRegisterExitCallback",
        [&k](Ctx &c) { ret(c, k.register_exit_callback(static_cast<kernel::Uid>(arg(c, 0)))); });
    def(
        "LoadExecForUser", 0x05572A5Fu, "sceKernelExitGame",
        [&system](Ctx &) { system.runtime().stop("the game called sceKernelExitGame"); }, true);
    // UtilsForUser: the data cache is the host's, the code is recompiled
    // ahead of time (CLAUDE.md §5: no self-modifying code), so these are the
    // no-ops PPSSPP makes of them; GPI (development switches) reads 0. The
    // game writes back the cache right after linking a stage with its .rlc,
    // which is when an installed overlay can be checked (F13).
    for (const auto &[nid, name] :
         {std::pair{0x79D1C3FAu, "sceKernelDcacheWritebackAll"},
          std::pair{0x3EE30821u, "sceKernelDcacheWritebackRange"},
          std::pair{0x34B9FA9Eu, "sceKernelDcacheWritebackInvalidateRange"},
          std::pair{0xC2DF770Eu, "sceKernelIcacheInvalidateRange"}}) {
        def("UtilsForUser", nid, name, [&system](Ctx &c) {
            system.overlays().verify();
            ret(c, 0u);
        });
    }
    def("UtilsForUser", 0x37FB5C42u, "sceKernelGetGPI", [](Ctx &c) { ret(c, 0u); });
    // sceSuspendForUser (scePower.cpp).
    def("sceSuspendForUser", 0x090CCB3Fu, "sceKernelPowerTick", [](Ctx &c) { ret(c, 0u); });
    def("sceSuspendForUser", 0xEADB1BD7u, "sceKernelPowerLock",
        [](Ctx &c) { ret(c, arg(c, 0) == 0u ? 0u : kInvalidMode); });
    def("sceSuspendForUser", 0x3AEE7261u, "sceKernelPowerUnlock",
        [](Ctx &c) { ret(c, arg(c, 0) == 0u ? 0u : kInvalidMode); });
    auto volatile_memory = std::make_shared<VolatileMemory>();
    def("sceSuspendForUser", 0x3E0271D3u, "sceKernelVolatileMemLock",
        [&k, volatile_memory](Ctx &c) {
            const std::uint32_t type = arg(c, 0);
            const std::uint32_t addr_ptr = arg(c, 1);
            const std::uint32_t size_ptr = arg(c, 2);
            std::uint32_t error = 0;
            if (!k.dispatch_enabled()) {
                error = kCanNotWait;
            } else if (k.in_interrupt()) {
                error = kIllegalContext;
            } else {
                error = volatile_lock(k, *volatile_memory, type, addr_ptr, size_ptr);
            }
            if (error == kPowerVmemInUse) {
                volatile_memory->waiters.push_back({k.current_thread(), addr_ptr, size_ptr});
                ret(c, k.wait_current_thread(kernel::WaitType::kVmem, 1, 0u, 0u, false, 0u));
                return;
            }
            if (error == kCanNotWait) {
                k.memory().write_u32(addr_ptr, 0x08400000u);
                k.memory().write_u32(size_ptr, 0x00400000u);
            }
            ret(c, error);
        });
    def("sceSuspendForUser", 0xA14F40B2u, "sceKernelVolatileMemTryLock",
        [&k, volatile_memory](Ctx &c) {
            ret(c, volatile_lock(k, *volatile_memory, arg(c, 0), arg(c, 1), arg(c, 2)));
        });
    def("sceSuspendForUser", 0xA569E425u, "sceKernelVolatileMemUnlock",
        [&k, volatile_memory](Ctx &c) {
            if (arg(c, 0) != 0u) {
                ret(c, kInvalidMode);
                return;
            }
            if (!volatile_memory->locked) {
                ret(c, kernel::error::kSemaOvf);
                return;
            }
            volatile_memory->locked = false;
            // Hand it over to the first waiter still waiting.
            while (!volatile_memory->waiters.empty()) {
                const VolatileMemory::Waiter waiter = volatile_memory->waiters.front();
                volatile_memory->waiters.erase(volatile_memory->waiters.begin());
                if (!k.is_waiting_for(waiter.thread, kernel::WaitType::kVmem, 1)) continue;
                if (volatile_lock(k, *volatile_memory, 0u, waiter.addr_ptr, waiter.size_ptr) ==
                    0u) {
                    k.resume_from_wait(waiter.thread, 0u);
                    break;
                }
            }
            ret(c, 0u);
        });
    // StdioForUser (sceIo.cpp: PSP_STDIN 0, PSP_STDOUT 1, PSP_STDERR 2).
    def("StdioForUser", 0x172D316Eu, "sceKernelStdin", [](Ctx &c) { ret(c, 0u); });
    def("StdioForUser", 0xA6BAB2E9u, "sceKernelStdout", [](Ctx &c) { ret(c, 1u); });
    def("StdioForUser", 0xF78BA90Au, "sceKernelStderr", [](Ctx &c) { ret(c, 2u); });
}

} // namespace

void register_kernel_hle(System &system) {
    register_threadman(system);
    register_sysmem_and_friends(system);
}

void register_module_hle(System &system) {
    kernel::Kernel &k = system.kernel();
    HleTable &t = system.hle();
    ModuleManager &modules = system.modules();
    const auto def = [&t](const char *lib, std::uint32_t nid, const char *name,
                          HleTable::Handler handler) {
        t.add(lib, nid, name, HleKind::kImplemented, std::move(handler));
    };
    // ModuleMgrForUser (sceKernelModule.cpp).
    def("ModuleMgrForUser", 0xB7F46618u, "sceKernelLoadModuleByID", [&system, &k](Ctx &c) {
        // The id is a file descriptor; the module is the rest of the file
        // (PPSSPP reads from the current position to the end).
        std::vector<std::uint8_t> data;
        std::string path;
        if (const std::uint32_t result = system.io().read_rest(arg(c, 0), data, path); result != 0u)
            return ret(c, result);
        // SceKernelLMOption: size, mpidtext, mpiddata, flags, position (u8 at +16).
        const std::uint32_t option = arg(c, 2);
        const bool from_top =
            option != 0u && k.memory().read_u8(option + 16u) == 1u; // PSP_SMEM_High
        std::string error;
        const std::uint32_t uid = system.modules().load(data, path, 0u, from_top, &error);
        if (static_cast<std::int32_t>(uid) < 0)
            std::fprintf(stderr, "mpo: sceKernelLoadModuleByID: %s\n", error.c_str());
        ret(c, uid);
    });
    def("ModuleMgrForUser", 0x50F0C1ECu, "sceKernelStartModule", [&modules](Ctx &c) {
        ret(c, modules.start_module(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1), arg(c, 2),
                                    arg(c, 3), arg(c, 4)));
    });
    def("ModuleMgrForUser", 0xD1FF982Au, "sceKernelStopModule", [&modules](Ctx &c) {
        ret(c, modules.stop_module(static_cast<kernel::Uid>(arg(c, 0)), arg(c, 1), arg(c, 2),
                                   arg(c, 3), arg(c, 4)));
    });
    def("ModuleMgrForUser", 0x2E0911AAu, "sceKernelUnloadModule",
        [&modules](Ctx &c) { ret(c, modules.unload_module(static_cast<kernel::Uid>(arg(c, 0)))); });
    def("ModuleMgrForUser", 0xF0A26395u, "sceKernelGetModuleId",
        [&modules](Ctx &c) { ret(c, modules.get_module_id()); });
    def("ModuleMgrForUser", 0xD8B73127u, "sceKernelGetModuleIdByAddress",
        [&modules](Ctx &c) { ret(c, modules.get_module_id_by_address(arg(c, 0))); });
    // sceUtility: only the module loader part (sceUtility.cpp). PPSSPP delays
    // the result by 25 ms (av) and hands the memory out first fit.
    def("sceUtility", 0xC629AF26u, "sceUtilityLoadAvModule", [&system, &k](Ctx &c) {
        const std::uint32_t module = arg(c, 0);
        const std::uint32_t result =
            module > 7u ? 0x80110F01u : system.load_utility_module(0x300u | module, true);
        ret(c, k.delay_result(result, 25000u));
    });
    def("sceUtility", 0xF7D8D092u, "sceUtilityUnloadAvModule", [&system, &k](Ctx &c) {
        const std::uint32_t module = arg(c, 0);
        const std::uint32_t result =
            module > 7u ? 0x80110F01u : system.unload_utility_module(0x300u | module, true);
        ret(c, k.delay_result(result, 800u));
    });
}

} // namespace mpo::hle
