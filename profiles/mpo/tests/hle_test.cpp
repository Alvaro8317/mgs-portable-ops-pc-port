// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_hle tests (F08b), without game data: small Allegrex programs assembled
// into guest memory and run by PSPRecomp's interpreter fallback on the HLE
// kernel, one fiber and one dispatch loop per PSP thread. Imports go through
// stub wrappers exactly like the generated ones (invoke_import at the stub
// address, return to $ra).
//
// Covered: thread creation and switching with private register contexts,
// semaphores, delays, callbacks run on their thread's fiber (call_guest),
// preemption at the runtime's visibility points, a thread exit that abandons
// its fiber, the visible failure of an unimplemented import (stop and
// continue policies, histogram), the module loader (a synthetic PRX: first
// fit placement, relocation, exports linked as imports, imports covered,
// module_start threads with and without waiting, unload), a fake firmware
// module and the memory adapter; F09: the vblank and a vblank interrupt
// handler, IO on a synthetic ISO (async on the helper thread, sync, listing)
// and the RTC; F10b: sceCtrl (sampling at the vblank end, blocking reads,
// peek, latch, negative reads, analog mode); F11b: sceGe (a list with a block
// transfer into VRAM, a signal and a finish with their sub-interrupt
// handlers, ListSync and DrawSync, a stalled list), sceMpeg (PSMF header,
// ring buffer and its read callback) and the binding of the import stubs of
// a module loaded without recompiled code; F16b: sceAudio (a blocking output
// that waits for the mixer, a busy channel, the samples in the mix),
// sceSasCore (the delay after a mix) and sceAtrac3plus (no ID before
// sceUtilityLoadAvModule loads the module).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "hle/system.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"
#include "synthetic_disc.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const auto check_a = (a);                                                                  \
        const auto check_b = (b);                                                                  \
        if (!(check_a == check_b)) {                                                               \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (0x%llX vs 0x%llX)\n",          \
                         __FILE__, __LINE__, #a, #b, static_cast<unsigned long long>(check_a),     \
                         static_cast<unsigned long long>(check_b));                                \
        }                                                                                          \
    } while (0)

using Ctx = psprecomp::AllegrexContext;
namespace mk = mpo::kernel;
namespace mh = mpo::hle;

// Guest layout of the tests.
constexpr std::uint32_t kCode = 0x08A00000u;
constexpr std::uint32_t kStubs = 0x08A70000u;
constexpr std::uint32_t kData = 0x08A80000u;
constexpr std::uint32_t kStrings = 0x08A90000u;

// Registers.
constexpr int zero = 0, v0 = 2, a0 = 4, a1 = 5, a2 = 6, a3 = 7, t0 = 8, t1 = 9, t2 = 10, s0 = 16,
              s1 = 17, s2 = 18, s6 = 22, s7 = 23, t9 = 25, ra = 31;

// ---- Import stubs ------------------------------------------------------------

struct StubImport {
    const char *library;
    std::uint32_t nid;
};
enum Stub : std::uint32_t {
    kCreateSema,
    kCreateThread,
    kStartThread,
    kWaitSema,
    kSignalSema,
    kDelayThread,
    kWaitThreadEnd,
    kCreateCallback,
    kDelayThreadCB,
    kExitThread,
    kFake,
    kStartModule,
    kTestExport,
    kGetModuleIdByAddress,
    kUnloadModule,
    // F09.
    kIoOpen,
    kIoOpenAsync,
    kIoRead,
    kIoReadAsync,
    kIoWaitAsyncCB,
    kIoPollAsync,
    kIoLseek32,
    kIoClose,
    kIoDopen,
    kIoDread,
    kIoDclose,
    kGetSystemTimeWide,
    kWaitVblankStartCB,
    kGetVcount,
    kRegisterSubIntr,
    kEnableSubIntr,
    kRtcGetCurrentTick,
    kLibcTime,
    kRtcGetClockLocal,
    // F10b.
    kCtrlSetSamplingMode,
    kCtrlReadBufferPositive,
    kCtrlPeekBufferPositive,
    kCtrlReadBufferNegative,
    kCtrlReadLatch,
    // F11b.
    kGeEdramGetAddr,
    kGeSetCallback,
    kGeListEnQueue,
    kGeListSync,
    kGeDrawSync,
    kGeListUpdateStallAddr,
    kMpegInit,
    kMpegRingbufferQueryMemSize,
    kMpegRingbufferConstruct,
    kMpegQueryMemSize,
    kMpegCreate,
    kMpegQueryStreamOffset,
    kMpegQueryStreamSize,
    kMpegRegistStream,
    kMpegRingbufferAvailableSize,
    kMpegRingbufferPut,
    // F16b.
    kAudioChReserve,
    kAudioOutputPannedBlocking,
    kAudioOutputPanned,
    kAudioGetChannelRestLength,
    kSasInit,
    kSasCoreWithMix,
    kUtilityLoadAvModule,
    kAtracGetAtracID,
    kStubCount,
};
constexpr StubImport kStubImports[kStubCount] = {
    {"ThreadManForUser", 0xD6DA4BA1u}, {"ThreadManForUser", 0x446D8DE6u},
    {"ThreadManForUser", 0xF475845Du}, {"ThreadManForUser", 0x4E3A1105u},
    {"ThreadManForUser", 0x3F53E640u}, {"ThreadManForUser", 0xCEADEB47u},
    {"ThreadManForUser", 0x278C0DF5u}, {"ThreadManForUser", 0xE81CAF8Fu},
    {"ThreadManForUser", 0x68DA9E36u}, {"ThreadManForUser", 0xAA73C935u},
    {"sceFake", 0x12345678u},          {"ModuleMgrForUser", 0x50F0C1ECu},
    {"testlib", 0x11112222u},          {"ModuleMgrForUser", 0xD8B73127u},
    {"ModuleMgrForUser", 0x2E0911AAu}, {"IoFileMgrForUser", 0x109F50BCu},
    {"IoFileMgrForUser", 0x89AA9906u}, {"IoFileMgrForUser", 0x6A638D83u},
    {"IoFileMgrForUser", 0xA0B5A7C2u}, {"IoFileMgrForUser", 0x35DBD746u},
    {"IoFileMgrForUser", 0x3251EA56u}, {"IoFileMgrForUser", 0x68963324u},
    {"IoFileMgrForUser", 0x810C4BC3u}, {"IoFileMgrForUser", 0xB29DDF9Cu},
    {"IoFileMgrForUser", 0xE3EB004Cu}, {"IoFileMgrForUser", 0xEB092469u},
    {"ThreadManForUser", 0x82BC5777u}, {"sceDisplay", 0x46F186C3u},
    {"sceDisplay", 0x9C6EAAD7u},       {"InterruptManager", 0xCA04A2B9u},
    {"InterruptManager", 0xFB8E22ECu}, {"sceRtc", 0x3F7AD767u},
    {"UtilsForUser", 0x27CC57F0u},     {"sceRtc", 0xE7C27D1Bu},
    {"sceCtrl", 0x1F4011E6u},          {"sceCtrl", 0x1F803938u},
    {"sceCtrl", 0x3A622550u},          {"sceCtrl", 0x60B81F86u},
    {"sceCtrl", 0x0B588501u},          {"sceGe_user", 0xE47E40E4u},
    {"sceGe_user", 0xA4FC06A4u},       {"sceGe_user", 0xAB49E76Au},
    {"sceGe_user", 0x03444EB4u},       {"sceGe_user", 0xB287BD61u},
    {"sceGe_user", 0xE0D68148u},       {"sceMpeg", 0x682A619Bu},
    {"sceMpeg", 0xD7A29F46u},          {"sceMpeg", 0x37295ED8u},
    {"sceMpeg", 0xC132E22Fu},          {"sceMpeg", 0xD8C5F121u},
    {"sceMpeg", 0x21FF80E4u},          {"sceMpeg", 0x611E9E11u},
    {"sceMpeg", 0x42560F23u},          {"sceMpeg", 0xB5F6DC87u},
    {"sceMpeg", 0xB240A59Eu},          {"sceAudio", 0x5EC81C55u},
    {"sceAudio", 0x13F592BCu},         {"sceAudio", 0xE2D56B2Du},
    {"sceAudio", 0xB011922Fu},         {"sceSasCore", 0x42778A9Fu},
    {"sceSasCore", 0x50A14DFCu},       {"sceUtility", 0xC629AF26u},
    {"sceAtrac3plus", 0x780F88D1u},
};
constexpr std::uint32_t stub(Stub s) {
    return kStubs + 8u * static_cast<std::uint32_t>(s);
}

// What psp_recomp's import wrappers do (generated_registry.cpp): one slot
// per stub, through invoke_import_cached. Not the uncached invoke_import: it
// keeps two iterators of the runtime's HLE maps alive across the call, and a
// call that never returns (ExitThread) abandons them on the fiber's stack.
// With checked iterators (MSVC Debug, _GLIBCXX_DEBUG) the maps keep a list of
// their iterators, and destroying them later walks into the freed stack.
void import_wrapper(psprecomp::Runtime &rt, Ctx &ctx) {
    const std::uint32_t import_pc = ctx.pc;
    const std::uint32_t return_address = ctx.gpr[31];
    const std::uint32_t slot = (import_pc - kStubs) / 8u;
    const StubImport &import = kStubImports[slot];
    rt.invoke_import_cached(slot, import.library, import.nid, ctx);
    if (!rt.stopped() && ctx.pc == import_pc) ctx.pc = return_address;
}

void register_stubs(psprecomp::Runtime &rt) {
    for (std::uint32_t i = 0; i < kStubCount; ++i)
        rt.register_function(stub(static_cast<Stub>(i)), &import_wrapper, "import_test");
}

// ---- A tiny Allegrex assembler -------------------------------------------------

class Asm {
public:
    Asm(psprecomp::Runtime &rt, std::uint32_t base) : rt_(rt), pc_(base) {}
    [[nodiscard]] std::uint32_t here() const { return pc_; }
    void emit(std::uint32_t word) {
        rt_.memory().store32(pc_, word);
        pc_ += 4u;
    }
    static std::uint32_t r(int rs, int rt, int rd, std::uint32_t funct) {
        return (static_cast<std::uint32_t>(rs) << 21u) | (static_cast<std::uint32_t>(rt) << 16u) |
               (static_cast<std::uint32_t>(rd) << 11u) | funct;
    }
    static std::uint32_t i(std::uint32_t op, int rs, int rt, std::uint32_t imm) {
        return (op << 26u) | (static_cast<std::uint32_t>(rs) << 21u) |
               (static_cast<std::uint32_t>(rt) << 16u) | (imm & 0xFFFFu);
    }
    void li(int reg, std::uint32_t value) {
        emit(i(0x0Fu, zero, reg, value >> 16u));   // lui
        emit(i(0x0Du, reg, reg, value & 0xFFFFu)); // ori
    }
    void move(int rd, int rs) { emit(r(rs, zero, rd, 0x21u)); } // addu
    void addu(int rd, int rs, int rt) { emit(r(rs, rt, rd, 0x21u)); }
    void addiu(int rt, int rs, std::int32_t imm) {
        emit(i(0x09u, rs, rt, static_cast<std::uint32_t>(imm)));
    }
    void sw(int rt, std::int32_t offset, int base) {
        emit(i(0x2Bu, base, rt, static_cast<std::uint32_t>(offset)));
    }
    void lw(int rt, std::int32_t offset, int base) {
        emit(i(0x23u, base, rt, static_cast<std::uint32_t>(offset)));
    }
    void call(std::uint32_t target) {
        emit((0x03u << 26u) | ((target >> 2u) & 0x03FFFFFFu)); // jal
        emit(0u);
    }
    void call(Stub s) { call(stub(s)); }
    // A function that calls imports keeps its $ra in $s6 (no HLE call
    // touches the callee-saved registers).
    void enter() { move(s6, ra); }
    void leave() {
        move(ra, s6);
        ret();
    }
    void ret() {
        emit(r(ra, zero, zero, 0x08u)); // jr ra
        emit(0u);
    }
    void beq(int rs, int rt, std::uint32_t target) {
        emit(i(0x04u, rs, rt, (target - (pc_ + 4u)) / 4u));
        emit(0u);
    }

private:
    psprecomp::Runtime &rt_;
    std::uint32_t pc_;
};

void put_string(psprecomp::Runtime &rt, std::uint32_t address, const char *text) {
    for (std::size_t k = 0; k <= std::strlen(text); ++k)
        rt.memory().store8(address + static_cast<std::uint32_t>(k),
                           static_cast<std::uint8_t>(text[k]));
}

// A PSP thread started from the host, as the boot does.
mk::Uid start_thread(mh::System &system, const char *name, std::uint32_t entry,
                     std::uint32_t priority) {
    const std::uint32_t uid = system.kernel().create_thread_for_module(name, 0, 0u, entry, priority,
                                                                       0x4000u, 0x80000000u, false);
    CHECK(static_cast<std::int32_t>(uid) > 0);
    CHECK_EQ(system.kernel().start_thread(static_cast<mk::Uid>(uid), 0u, 0u), 0u);
    return static_cast<mk::Uid>(uid);
}

struct Fixture {
    psprecomp::Runtime runtime{32u * 1024u * 1024u};
    mh::SystemConfig config;
    std::unique_ptr<mh::System> system;
    explicit Fixture(mh::SystemConfig cfg = {}, bool vblank = false) : config(std::move(cfg)) {
        config.fiber_stack_size = 1u << 20u;
        // Most scenarios expect the kernel to go idle: no 59.94 Hz vblank.
        config.vblank = vblank;
        register_stubs(runtime);
        system = std::make_unique<mh::System>(runtime, config);
    }
    std::uint64_t dword(std::uint32_t address) {
        return std::uint64_t{word(address)} | (std::uint64_t{word(address + 4u)} << 32u);
    }
    std::uint32_t word(std::uint32_t address) { return runtime.memory().load32(address); }
};

// ---- Tests ---------------------------------------------------------------------

// Two threads with their own registers: the better worker preempts at
// StartThread, signals, sleeps; the main thread waits, then joins it.
void test_threads_and_semaphores() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    put_string(rt, kStrings, "sema");
    put_string(rt, kStrings + 0x10u, "worker");
    rt.memory().store32(kData + 8u, 0xFFFFFFFFu);

    Asm worker(rt, kCode + 0x400u);
    const std::uint32_t worker_entry = worker.here();
    worker.enter();
    worker.li(s7, kData);
    worker.li(s0, 0x2222u);
    worker.lw(s1, 0, a1); // The sema UID, copied to this thread's stack.
    worker.li(t9, 0xAAAAu);
    worker.sw(t9, 20, s7);
    worker.lw(t9, 8, s7); // main has not returned from WaitSema yet.
    worker.sw(t9, 24, s7);
    worker.move(a0, s1);
    worker.li(a1, 1u);
    worker.call(kSignalSema);
    worker.li(a0, 1000u);
    worker.call(kDelayThread);
    worker.sw(s0, 28, s7);
    worker.li(v0, 0x55u);
    worker.leave();

    Asm main(rt, kCode);
    const std::uint32_t main_entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.li(s0, 0x1111u);
    main.li(a0, kStrings);
    main.li(a1, 0u);
    main.li(a2, 0u);
    main.li(a3, 1u);
    main.li(t0, 0u);
    main.call(kCreateSema);
    main.move(s1, v0);
    main.sw(s1, 0, s7);
    main.li(a0, kStrings + 0x10u);
    main.li(a1, worker_entry);
    main.li(a2, 0x10u);
    main.li(a3, 0x4000u);
    main.li(t0, 0u);
    main.li(t1, 0u);
    main.call(kCreateThread);
    main.move(s2, v0);
    main.move(a0, s2);
    main.li(a1, 4u);
    main.move(a2, s7); // Argument block: the sema UID at kData.
    main.call(kStartThread);
    main.move(a0, s1);
    main.li(a1, 1u);
    main.li(a2, 0u);
    main.call(kWaitSema);
    main.sw(v0, 8, s7);
    main.sw(s0, 12, s7);
    main.move(a0, s2);
    main.li(a1, 0u);
    main.call(kWaitThreadEnd);
    main.sw(v0, 16, s7);
    main.li(v0, 0x99u);
    main.leave();

    const mk::Uid main_uid = start_thread(*f.system, "main", main_entry, 0x20u);
    CHECK(f.system->run(10'000'000u) == mk::RunResult::kIdle);
    CHECK(!rt.stopped());
    CHECK_EQ(f.word(kData + 20u), 0xAAAAu);     // The worker ran first...
    CHECK_EQ(f.word(kData + 24u), 0xFFFFFFFFu); // ...before main's WaitSema returned.
    CHECK_EQ(f.word(kData + 8u), 0u);           // WaitSema succeeded.
    CHECK_EQ(f.word(kData + 12u), 0x1111u);     // main's s0 survived the worker's.
    CHECK_EQ(f.word(kData + 28u), 0x2222u);     // and the worker's survived main's.
    CHECK_EQ(f.word(kData + 16u), 0x55u);       // WaitThreadEnd: the worker's exit status.
    CHECK_EQ(f.system->kernel().get_thread_exit_status(main_uid), 0x99u);
    CHECK(f.system->kernel().now_us() >= 1000u);
    CHECK(f.system->cpu().switches() >= 3u);
    const auto *wait = f.system->hle().find("ThreadManForUser", 0x4E3A1105u);
    CHECK(wait != nullptr && wait->calls == 1u && wait->kind == mh::HleKind::kImplemented);
}

// A callback notified by the host runs on its thread's fiber during
// DelayThreadCB, with its arguments, and the thread finds its registers again.
void test_callbacks() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    put_string(rt, kStrings, "cb");
    Asm cb(rt, kCode + 0x400u);
    const std::uint32_t cb_entry = cb.here();
    cb.li(t0, kData);
    cb.sw(a0, 12, t0);
    cb.sw(a1, 16, t0);
    cb.sw(a2, 20, t0);
    cb.li(v0, 0u);
    cb.ret();

    Asm main(rt, kCode);
    const std::uint32_t entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.li(s0, 0x3333u);
    main.li(a0, kStrings);
    main.li(a1, cb_entry);
    main.li(a2, 0x77u);
    main.call(kCreateCallback);
    main.sw(v0, 0, s7);
    main.li(a0, 5000u);
    main.call(kDelayThreadCB);
    main.sw(v0, 4, s7);
    main.sw(s0, 8, s7);
    main.leave();

    mh::System &system = *f.system;
    system.kernel().schedule_event(1000u, [&system, &rt] {
        system.kernel().notify_callback_from_host(static_cast<mk::Uid>(rt.memory().load32(kData)),
                                                  0x42u);
    });
    (void)start_thread(system, "main", entry, 0x20u);
    CHECK(system.run(10'000'000u) == mk::RunResult::kIdle);
    CHECK(!rt.stopped());
    CHECK_EQ(f.word(kData + 12u), 1u);    // notifyCount
    CHECK_EQ(f.word(kData + 16u), 0x42u); // notifyArg
    CHECK_EQ(f.word(kData + 20u), 0x77u); // common
    CHECK_EQ(f.word(kData + 4u), 0u);
    CHECK_EQ(f.word(kData + 8u), 0x3333u);
    CHECK_EQ(system.cpu().guest_calls(), 1u);
    CHECK(system.kernel().now_us() >= 5000u);
}

// A better thread whose delay ends while a worse one spins in guest code takes
// the CPU at the next visibility point (the runtime's starvation hook).
void test_preemption_from_guest_loop() {
    mh::SystemConfig config;
    config.preemption_interval = 1u;
    config.us_per_preemption = 50u;
    Fixture f(config);
    psprecomp::Runtime &rt = f.runtime;

    Asm low(rt, kCode);
    const std::uint32_t low_entry = low.here();
    low.li(s7, kData);
    const std::uint32_t loop = low.here();
    low.lw(t0, 0, s7);
    low.addiu(t0, t0, 1);
    low.sw(t0, 0, s7);
    low.lw(t1, 4, s7);
    low.beq(t1, zero, loop);
    low.ret();

    Asm high(rt, kCode + 0x400u);
    const std::uint32_t high_entry = high.here();
    high.enter();
    high.li(s7, kData);
    high.li(a0, 2000u);
    high.call(kDelayThread);
    high.lw(t0, 0, s7);
    high.sw(t0, 8, s7);
    high.li(t1, 1u);
    high.sw(t1, 4, s7);
    high.leave();

    (void)start_thread(*f.system, "high", high_entry, 0x20u);
    (void)start_thread(*f.system, "low", low_entry, 0x30u);
    CHECK(f.system->run(10'000'000u) == mk::RunResult::kIdle);
    CHECK(!rt.stopped());
    CHECK(f.word(kData + 8u) > 0u);   // low ran before high woke...
    CHECK_EQ(f.word(kData + 4u), 1u); // ...and high stopped it.
    CHECK(f.word(kData) >= f.word(kData + 8u));
    CHECK(f.system->kernel().now_us() >= 2000u);
}

// ExitThread never returns: its fiber is abandoned (no leak under ASan), the
// thread can start again, and deleting it drops its saved context.
void test_exit_and_restart() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    Asm code(rt, kCode);
    const std::uint32_t entry = code.here();
    code.enter();
    code.li(s7, kData);
    code.lw(t0, 0, s7);
    code.addiu(t0, t0, 1);
    code.sw(t0, 0, s7);
    code.li(a0, 3u);
    code.call(kExitThread);
    code.li(t0, 0xDEADu);
    code.sw(t0, 4, s7);
    code.leave();

    mk::Kernel &k = f.system->kernel();
    const mk::Uid uid = start_thread(*f.system, "exiter", entry, 0x20u);
    CHECK(f.system->run(10'000'000u) == mk::RunResult::kIdle);
    CHECK_EQ(f.word(kData), 1u);
    CHECK_EQ(f.word(kData + 4u), 0u);
    CHECK_EQ(k.get_thread_exit_status(uid), 3u);
    CHECK_EQ(k.start_thread(uid, 0u, 0u), 0u);
    CHECK(f.system->run(10'000'000u) == mk::RunResult::kIdle);
    CHECK_EQ(f.word(kData), 2u);
    CHECK_EQ(f.word(kData + 4u), 0u);
    const std::size_t contexts = f.system->cpu().saved_contexts();
    CHECK(contexts >= 1u);
    CHECK_EQ(k.delete_thread(uid), 0u);
    CHECK_EQ(f.system->cpu().saved_contexts(), contexts - 1u);
}

// An import without HLE stops the runtime with its name, stub and caller
// (default policy), or returns 0 and is counted (kReturnZero).
void test_unimplemented_imports() {
    {
        Fixture f;
        psprecomp::Runtime &rt = f.runtime;
        f.system->hle().cover_imports({psprecomp::PspImport{"sceFake", 0x12345678u, stub(kFake)}});
        Asm code(rt, kCode);
        const std::uint32_t entry = code.here();
        code.enter();
        code.li(s7, kData);
        code.call(kFake);
        const std::uint32_t caller = code.here() - 8u;
        code.li(t0, 1u);
        code.sw(t0, 0, s7);
        code.leave();
        (void)start_thread(*f.system, "caller", entry, 0x20u);
        CHECK(f.system->run(10'000'000u) == mk::RunResult::kStopped);
        CHECK(rt.stopped());
        const std::string &reason = rt.stop_reason();
        char caller_text[16];
        std::snprintf(caller_text, sizeof(caller_text), "%08X", caller);
        CHECK(reason.find("unimplemented HLE import sceFake::sceFake_12345678") !=
              std::string::npos);
        CHECK(reason.find("08A70050") != std::string::npos); // the stub
        CHECK(reason.find(caller_text) != std::string::npos);
        CHECK_EQ(f.word(kData), 0u);
        // Stopped is final: another run stops again at once.
        CHECK(f.system->run(10'000'000u) == mk::RunResult::kStopped);
    }
    {
        mh::SystemConfig config;
        config.unimplemented = mh::UnimplementedPolicy::kReturnZero;
        Fixture f(config);
        psprecomp::Runtime &rt = f.runtime;
        f.system->hle().add_nid_name("sceFake", 0x12345678u, "sceFakeCall");
        f.system->hle().cover_imports({psprecomp::PspImport{"sceFake", 0x12345678u, stub(kFake)}});
        Asm code(rt, kCode);
        const std::uint32_t entry = code.here();
        code.enter();
        code.li(s7, kData);
        code.li(v0, 0x1234u);
        code.call(kFake);
        code.sw(v0, 4, s7);
        code.li(t0, 1u);
        code.sw(t0, 0, s7);
        code.leave();
        (void)start_thread(*f.system, "caller", entry, 0x20u);
        CHECK(f.system->run(10'000'000u) == mk::RunResult::kIdle);
        CHECK(!rt.stopped());
        CHECK_EQ(f.word(kData), 1u);
        CHECK_EQ(f.word(kData + 4u), 0u);
        const auto *entry_info = f.system->hle().find("sceFake", 0x12345678u);
        CHECK(entry_info != nullptr && entry_info->kind == mh::HleKind::kUnimplemented &&
              entry_info->calls == 1u && entry_info->name == "sceFakeCall");
        CHECK(!f.system->hle().histogram().empty() &&
              f.system->hle().histogram().front()->calls >= 1u);
    }
}

// ---- A synthetic PRX -----------------------------------------------------------

void put32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    if (bytes.size() < offset + 4u) bytes.resize(offset + 4u);
    for (std::size_t k = 0; k < 4u; ++k)
        bytes[offset + k] = static_cast<std::uint8_t>(value >> (8u * k));
}
void put16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    if (bytes.size() < offset + 2u) bytes.resize(offset + 2u);
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

// One PT_LOAD at vaddr 0 (code, export and import tables, sceModuleInfo), a
// PSP relocation section of R_MIPS_32 entries for every pointer in it.
//   0x00 module_start: v0 = 7
//   0x0C testlib 0x11112222: v0 = a0 + 100
std::vector<std::uint8_t> synthetic_prx() {
    std::vector<std::uint8_t> seg(0x180u, 0u);
    const auto word = [&seg](std::uint32_t offset, std::uint32_t value) {
        put32(seg, offset, value);
    };
    word(0x00u, Asm::i(0x09u, zero, v0, 7u));
    word(0x04u, Asm::r(ra, zero, zero, 0x08u));
    word(0x08u, 0u);
    word(0x0Cu, Asm::i(0x09u, a0, v0, 100u));
    word(0x10u, Asm::r(ra, zero, zero, 0x08u));
    word(0x14u, 0u);
    std::memcpy(seg.data() + 0x40u, "testlib", 8u);
    std::memcpy(seg.data() + 0x50u, "ThreadManForUser", 17u);
    // syslib: module_start and module_start_thread_parameter.
    word(0x80u, 0xD632ACDBu);
    word(0x84u, 0x0F7C276Cu);
    word(0x88u, 0x00u);
    word(0x8Cu, 0xA0u);
    word(0xA0u, 3u);
    word(0xA4u, 0x18u);
    word(0xA8u, 0x2000u);
    word(0xACu, 0u);
    // testlib: one function.
    word(0xB0u, 0x11112222u);
    word(0xB4u, 0x0Cu);
    // .lib.ent: syslib, testlib.
    word(0xC0u, 0u);
    put16(seg, 0xC4u, 0u);
    put16(seg, 0xC6u, 0x8000u);
    word(0xC8u, 4u | (1u << 8u) | (1u << 16u));
    word(0xCCu, 0x80u);
    word(0xD0u, 0x40u);
    put16(seg, 0xD4u, 0x11u);
    put16(seg, 0xD6u, 0x0001u);
    word(0xD8u, 4u | (0u << 8u) | (1u << 16u));
    word(0xDCu, 0xB0u);
    // .lib.stub: ThreadManForUser, two functions (one implemented, one not).
    word(0xE0u, 0x50u);
    put16(seg, 0xE4u, 0x11u);
    put16(seg, 0xE6u, 0x4001u);
    word(0xE8u, 5u | (0u << 8u) | (2u << 16u));
    word(0xECu, 0x100u);
    word(0xF0u, 0x110u);
    word(0x100u, 0x293B45B8u);
    word(0x104u, 0xDEADBE01u);
    // sceModuleInfo.
    put16(seg, 0x140u, 0u);
    put16(seg, 0x142u, 0x0101u);
    std::memcpy(seg.data() + 0x144u, "SyntheticModule", 16u);
    word(0x160u, 0x8000u);
    word(0x164u, 0xC0u);
    word(0x168u, 0xE0u);
    word(0x16Cu, 0xE0u);
    word(0x170u, 0xF4u);
    const std::uint32_t relocated[] = {0x88u, 0x8Cu, 0xB4u,  0xCCu,  0xD0u,  0xDCu,  0xE0u,
                                       0xECu, 0xF0u, 0x160u, 0x164u, 0x168u, 0x16Cu, 0x170u};

    std::vector<std::uint8_t> elf(0x100u, 0u);
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1;
    elf[5] = 1;
    elf[6] = 1;
    put16(elf, 16u, 0xFFA0u);
    put16(elf, 18u, 8u);
    put32(elf, 20u, 1u);
    put32(elf, 24u, 0u); // e_entry: module_start
    put32(elf, 28u, 52u);
    put16(elf, 40u, 52u);
    put16(elf, 42u, 32u);
    put16(elf, 44u, 1u);
    put16(elf, 46u, 40u);
    // PT_LOAD.
    put32(elf, 52u, 1u);
    put32(elf, 56u, 0x100u);
    put32(elf, 60u, 0u);
    put32(elf, 64u, 0x140u);
    put32(elf, 68u, 0x180u);
    put32(elf, 72u, 0x200u);
    put32(elf, 76u, 7u);
    put32(elf, 80u, 16u);
    elf.insert(elf.end(), seg.begin(), seg.end());
    const std::size_t rel_offset = elf.size();
    for (const std::uint32_t offset : relocated) {
        put32(elf, elf.size(), offset);
        put32(elf, elf.size(), 2u); // R_MIPS_32, segment 0 relative to segment 0.
    }
    const char names[] = "\0.text\0.rodata.sceModuleInfo\0.rel.sceStub\0.shstrtab\0";
    const std::size_t names_offset = elf.size();
    elf.insert(elf.end(), names, names + sizeof(names));
    while (elf.size() % 4u != 0u) elf.push_back(0u);
    const std::size_t sh_offset = elf.size();
    const auto section = [&elf](std::uint32_t name, std::uint32_t type, std::uint32_t flags,
                                std::uint32_t address, std::size_t offset, std::size_t size) {
        const std::size_t at = elf.size();
        elf.resize(at + 40u, 0u);
        put32(elf, at, name);
        put32(elf, at + 4u, type);
        put32(elf, at + 8u, flags);
        put32(elf, at + 12u, address);
        put32(elf, at + 16u, static_cast<std::uint32_t>(offset));
        put32(elf, at + 20u, static_cast<std::uint32_t>(size));
        put32(elf, at + 32u, 4u);
    };
    section(0u, 0u, 0u, 0u, 0u, 0u);
    section(1u, 1u, 6u, 0u, 0x100u, 0x40u);
    section(7u, 1u, 2u, 0x140u, 0x100u + 0x140u, 52u);
    section(29u, 0x700000A0u, 0u, 0u, rel_offset, sizeof(relocated) * 2u);
    section(42u, 3u, 0u, 0u, names_offset, sizeof(names));
    put32(elf, 32u, static_cast<std::uint32_t>(sh_offset));
    put16(elf, 48u, 5u);
    put16(elf, 50u, 4u);
    return elf;
}

std::vector<std::uint8_t> fake_firmware_module() {
    std::vector<std::uint8_t> psp(0x150u, 0u);
    std::memcpy(psp.data(), "~PSP", 4u);
    std::memcpy(psp.data() + 0x0Au, "sceFont_Library", 16u);
    put16(psp, 0x3Cu, 0x10u);
    put16(psp, 0x3Eu, 0x10u);
    put32(psp, 0x54u, 0x8750u);
    put32(psp, 0x58u, 0x238u);
    return psp;
}

void test_module_loader() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    mh::System &system = *f.system;
    mh::ModuleManager &modules = system.modules();
    const std::vector<std::uint8_t> prx = synthetic_prx();
    std::string error;
    const std::uint32_t uid = modules.load(prx, "disc0:/test.prx", 0u, false, &error);
    CHECK(static_cast<std::int32_t>(uid) > 0);
    if (static_cast<std::int32_t>(uid) <= 0) {
        std::fprintf(stderr, "load: %s\n", error.c_str());
        return;
    }
    const mh::Module *module = modules.find(static_cast<mk::Uid>(uid));
    CHECK(module != nullptr);
    // First fit above the 0x4000 bytes PPSSPP keeps at the bottom of user RAM.
    CHECK_EQ(module->base, 0x08804000u);
    CHECK_EQ(module->size, 0x200u);
    CHECK(module->name == "SyntheticModule");
    CHECK_EQ(module->gp, 0x08804000u + 0x8000u);
    CHECK_EQ(module->start_func, 0x08804000u);
    CHECK_EQ(module->start_thread_priority, 0x18u);
    CHECK_EQ(module->start_thread_stack_size, 0x2000u);
    CHECK_EQ(module->exports.size(), 1u);
    CHECK_EQ(module->imports.size(), 2u);
    const auto *linked = system.hle().find("testlib", 0x11112222u);
    CHECK(linked != nullptr && linked->kind == mh::HleKind::kLink);
    const auto *thread_id = system.hle().find("ThreadManForUser", 0x293B45B8u);
    CHECK(thread_id != nullptr && thread_id->kind == mh::HleKind::kImplemented);
    const auto *missing = system.hle().find("ThreadManForUser", 0xDEADBE01u);
    CHECK(missing != nullptr && missing->kind == mh::HleKind::kUnimplemented);
    CHECK_EQ(modules.get_module_id_by_address(0x08804010u), uid);
    // Its import stubs (jr ra; nop) have no recompiled code: the loader binds
    // them to the HLE (F11b).
    CHECK_EQ(module->bound_stubs.size(), 2u);
    CHECK(rt.has_function(0x08804110u));
    CHECK(rt.has_function(0x08804118u));
    // A second copy, started with a worse priority than its caller: the
    // caller waits for module_start.
    const std::uint32_t uid2 = modules.load(prx, "disc0:/test2.prx", 0u, false, &error);
    CHECK(static_cast<std::int32_t>(uid2) > 0);
    const mh::Module *module2 = modules.find(static_cast<mk::Uid>(uid2));
    CHECK(module2 != nullptr && module2->base == 0x08804200u);
    // SceKernelSMOption: size 20, mpidstack 0, stacksize 0, priority 0x30, attr 0.
    rt.memory().store32(kData + 0x100u, 20u);
    rt.memory().store32(kData + 0x10Cu, 0x30u);

    Asm code(rt, kCode);
    const std::uint32_t entry = code.here();
    code.enter();
    code.li(s7, kData);
    code.li(a0, uid);
    code.li(a1, 0u);
    code.li(a2, 0u);
    code.li(a3, kData + 0x40u); // status
    code.li(t0, 0u);
    code.call(kStartModule);
    code.sw(v0, 0, s7);
    code.li(a0, uid2);
    code.li(a1, 0u);
    code.li(a2, 0u);
    code.li(a3, kData + 0x44u);
    code.li(t0, kData + 0x100u);
    code.call(kStartModule);
    code.sw(v0, 4, s7);
    code.li(a0, 5u);
    code.call(kTestExport); // Linked: jumps into the module's export.
    code.sw(v0, 8, s7);
    code.call(0x08804110u); // The module's own stub: sceKernelGetThreadId.
    code.sw(v0, 16, s7);
    code.li(a0, uid);
    code.call(kUnloadModule);
    code.sw(v0, 12, s7);
    code.leave();
    rt.memory().store32(kData + 0x40u, 0xFFFFFFFFu);
    rt.memory().store32(kData + 0x44u, 0xFFFFFFFFu);
    const mk::Uid loader = start_thread(system, "loader", entry, 0x20u);
    CHECK(system.run(10'000'000u) == mk::RunResult::kIdle);
    CHECK(!rt.stopped());
    CHECK_EQ(f.word(kData), uid);
    CHECK_EQ(f.word(kData + 0x40u), 7u);
    CHECK_EQ(f.word(kData + 4u), uid2);
    CHECK_EQ(f.word(kData + 0x44u), 7u);
    CHECK_EQ(f.word(kData + 8u), 105u);
    CHECK_EQ(f.word(kData + 12u), uid);
    CHECK_EQ(f.word(kData + 16u), static_cast<std::uint32_t>(loader));
    CHECK(modules.find(static_cast<mk::Uid>(uid)) == nullptr);
    // Unloading unbinds them.
    CHECK(!rt.has_function(0x08804110u));
    CHECK(modules.find(static_cast<mk::Uid>(uid2)) != nullptr);
    // Its memory is free again: the next load lands there.
    const std::uint32_t uid3 = modules.load(prx, "disc0:/test3.prx", 0u, false, &error);
    const mh::Module *module3 = modules.find(static_cast<mk::Uid>(uid3));
    CHECK(module3 != nullptr && module3->base == 0x08804000u);
    CHECK(rt.has_function(0x08804110u));
    // module_start threads delete themselves (PPSSPP's NID_MODULERETURN).
    for (const mk::Uid thread : system.kernel().thread_list()) {
        mk::ThreadInfo info;
        CHECK(system.kernel().thread_info(thread, info));
        CHECK(info.name != "SyntheticModule");
    }

    // A firmware module PPSSPP replaces with HLE: memory reserved from the
    // ~PSP header, nothing to run.
    const std::uint32_t font =
        modules.load(fake_firmware_module(), "disc0:/libfont.prx", 0u, false, &error);
    const mh::Module *fake = modules.find(static_cast<mk::Uid>(font));
    CHECK(fake != nullptr && fake->fake && fake->name == "sceFont_Library");
    CHECK(fake != nullptr && fake->size == 0x8988u);
    CHECK_EQ(modules.start_module(static_cast<mk::Uid>(font), 0u, 0u, 0u, 0u), font);
    // Garbage is rejected with a PSP error, not a crash.
    const std::vector<std::uint8_t> junk(64u, 0xEEu);
    CHECK_EQ(modules.load(junk, "disc0:/junk.prx", 0u, false, &error), 0x80020148u);
}

// F13: a stage overlay corpus for the synthetic PRX. Its install function
// stands in for psp_recomp output: it registers the module's entry and the
// wrapper of one import stub, so the loader only binds the other one.
int g_overlay_installs = 0;

void overlay_entry(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.gpr[2] = 0x5A5Au;
    ctx.pc = ctx.gpr[31];
}

void install_synthetic_overlay(psprecomp::Runtime &runtime) {
    ++g_overlay_installs;
    runtime.register_function(0x08804000u, &overlay_entry, "overlay_entry");
    runtime.register_function(0x08804110u, &overlay_entry, "overlay_import_wrapper");
}

std::string guest_sha256(psprecomp::Runtime &rt, std::uint32_t address, std::uint32_t size) {
    return psprecomp::sha256_bytes(
        std::span<const std::uint8_t>(rt.memory().raw_pointer(address, size), size));
}

void test_overlays() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    mh::System &system = *f.system;
    mh::ModuleManager &modules = system.modules();
    mh::OverlayManager &overlays = system.overlays();
    const std::vector<std::uint8_t> prx = synthetic_prx();
    std::string error;

    // The code as loaded, then as "the game" leaves it after its .rlc: one
    // word of the text patched (what the corpus is generated from).
    constexpr std::uint32_t kBase = 0x08804000u;
    constexpr std::uint32_t kText = 0x20u;
    constexpr std::uint32_t kLinkedWord = 0x0000000Du; // An ori with another immediate.
    const std::uint32_t probe = modules.load(prx, "disc0:/probe.prx", 0u, false, &error);
    CHECK(static_cast<std::int32_t>(probe) > 0);
    // No corpus at all: the module counts as interpreted.
    CHECK_EQ(overlays.stats().size(), 1u);
    CHECK_EQ(overlays.stats()[0].interpreted, 1u);
    CHECK_EQ(overlays.stats()[0].base, kBase);
    CHECK(overlays.stats()[0].name.starts_with("SyntheticModule@"));
    const std::uint32_t original = rt.memory().load32(kBase + 0x0Cu);
    rt.memory().store32(kBase + 0x0Cu, (original & 0xFFFF0000u) | kLinkedWord);
    const std::string linked_text = guest_sha256(rt, kBase, kText);
    CHECK_EQ(modules.unload_module(static_cast<mk::Uid>(probe)), probe);

    const std::string prx_sha256 = psprecomp::sha256_bytes(prx);
    const mh::OverlayModuleInfo info{mh::kOverlayAbiVersion,
                                     static_cast<std::uint32_t>(sizeof(psprecomp::Runtime)),
                                     static_cast<std::uint32_t>(sizeof(psprecomp::AllegrexContext)),
                                     "synthetic",
                                     kBase,
                                     0x200u,
                                     prx_sha256.c_str(),
                                     0u,
                                     kText,
                                     linked_text.c_str()};
    overlays.add_corpus(info, &install_synthetic_overlay, "");
    CHECK_EQ(overlays.corpora().size(), 1u);

    // Loaded where the corpus was generated for: installed before the stubs
    // are bound; the corpus's own wrapper stays.
    const std::uint32_t uid = modules.load(prx, "disc0:/stage.prx", 0u, false, &error);
    const mh::Module *module = modules.find(static_cast<mk::Uid>(uid));
    CHECK(module != nullptr && module->base == kBase);
    CHECK_EQ(g_overlay_installs, 1);
    CHECK(overlays.installed(static_cast<mk::Uid>(uid)));
    CHECK(rt.has_function(kBase));
    CHECK(module != nullptr && module->bound_stubs.size() == 1u);
    CHECK(module != nullptr && module->bound_stubs[0] == 0x08804118u);
    // The cache is written back before the game links the stage: still the
    // image as loaded, so the overlay stays pending.
    overlays.verify();
    CHECK_EQ(overlays.stats().back().verified, 0u);
    CHECK_EQ(overlays.stats().back().mismatches, 0u);
    // Linked: verified, and the corpus runs.
    rt.memory().store32(kBase + 0x0Cu, (original & 0xFFFF0000u) | kLinkedWord);
    overlays.verify();
    const mh::OverlayStats &stats = overlays.stats().back();
    CHECK(stats.name == "synthetic");
    CHECK_EQ(stats.loads, 1u);
    CHECK_EQ(stats.installs, 1u);
    CHECK_EQ(stats.verified, 1u);
    CHECK_EQ(stats.mismatches, 0u);
    psprecomp::AllegrexContext ctx{};
    ctx.pc = kBase;
    ctx.gpr[31] = 0x08800000u;
    CHECK(rt.invoke_isolated_aot(kBase, ctx));
    CHECK_EQ(ctx.gpr[2], 0x5A5Au);
    // A second copy elsewhere: the corpus is for another base.
    const std::uint32_t other = modules.load(prx, "disc0:/other.prx", 0u, false, &error);
    const mh::Module *other_module = modules.find(static_cast<mk::Uid>(other));
    CHECK(other_module != nullptr && other_module->base == kBase + 0x200u);
    CHECK(!overlays.installed(static_cast<mk::Uid>(other)));
    CHECK_EQ(g_overlay_installs, 1);
    CHECK_EQ(modules.unload_module(static_cast<mk::Uid>(other)), other);
    // Unloading removes the corpus with the module.
    CHECK_EQ(modules.unload_module(static_cast<mk::Uid>(uid)), uid);
    CHECK(!rt.has_function(kBase));
    CHECK(!rt.has_function(0x08804110u));

    // Linked with something else: the corpus does not describe that memory.
    // It goes, the stubs are bound to the HLE and the interpreter runs it.
    const std::uint32_t bad = modules.load(prx, "disc0:/bad.prx", 0u, false, &error);
    CHECK_EQ(g_overlay_installs, 2);
    rt.memory().store32(kBase + 0x0Cu, (original & 0xFFFF0000u) | 0x0Eu);
    // Through the HLE the game calls right after its .rlc.
    const mh::HleTable::Entry *writeback = system.hle().find("UtilsForUser", 0x79D1C3FAu);
    CHECK(writeback != nullptr && writeback->kind == mh::HleKind::kImplemented);
    overlays.verify();
    CHECK(!overlays.installed(static_cast<mk::Uid>(bad)));
    const auto named =
        std::find_if(overlays.stats().begin(), overlays.stats().end(),
                     [](const mh::OverlayStats &s) { return s.name == "synthetic"; });
    CHECK(named != overlays.stats().end() && named->installs == 2u && named->mismatches == 1u);
    CHECK(!rt.has_function(kBase));
    const mh::Module *bad_module = modules.find(static_cast<mk::Uid>(bad));
    CHECK(rt.has_function(0x08804110u) && rt.has_function(0x08804118u));
    CHECK(bad_module != nullptr && bad_module->bound_stubs.size() == 2u);
    CHECK_EQ(modules.unload_module(static_cast<mk::Uid>(bad)), bad);
    CHECK(!rt.has_function(0x08804110u));
}

void test_runtime_memory() {
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    mh::RuntimeMemory memory(runtime.memory());
    runtime.memory().store32(0x08900000u, 0xCAFEF00Du);
    CHECK_EQ(memory.read_u32(0x48900000u), 0xCAFEF00Du); // uncached mirror
    CHECK_EQ(memory.read_u32(0x88900000u), 0xCAFEF00Du); // kernel mirror
    memory.write_u32(0x04000010u, 0x01020304u);
    CHECK_EQ(runtime.memory().load32(0x04200010u), 0x01020304u); // VRAM mirror
    CHECK(memory.translate(0x0A000000u, 4u) == nullptr);
    CHECK(memory.translate(0x09FFFFFEu, 4u) == nullptr);
    CHECK(memory.translate(0x00000000u, 4u) == nullptr);
}

// ---- F09: vblank, interrupts, IO, RTC ---------------------------------------

// The vblank runs at 59.94 Hz: a thread waits three of them, and a vblank
// sub-interrupt handler runs once per vblank in interrupt context.
void test_vblank_and_interrupts() {
    Fixture f({}, true);
    psprecomp::Runtime &rt = f.runtime;

    Asm handler(rt, kCode + 0x800u);
    const std::uint32_t handler_entry = handler.here();
    handler.li(t0, kData + 0x40u);
    handler.lw(t1, 0, t0);
    handler.addiu(t1, t1, 1);
    handler.sw(t1, 0, t0);
    handler.sw(a0, 4, t0);
    handler.sw(a1, 8, t0);
    handler.ret();

    Asm main(rt, kCode);
    const std::uint32_t entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.li(a0, 30u); // PSP_VBLANK_INTR
    main.li(a1, 2u);
    main.li(a2, handler_entry);
    main.li(a3, 0x1234u);
    main.call(kRegisterSubIntr);
    main.sw(v0, 0, s7);
    main.li(a0, 30u);
    main.li(a1, 2u);
    main.call(kEnableSubIntr);
    main.sw(v0, 4, s7);
    for (int i = 0; i < 3; ++i) main.call(kWaitVblankStartCB);
    main.call(kGetVcount);
    main.sw(v0, 8, s7);
    main.call(kGetSystemTimeWide);
    main.sw(v0, 12, s7);
    main.li(t0, 1u);
    main.sw(t0, 16, s7);
    main.leave();

    (void)start_thread(*f.system, "vblank", entry, 0x20u);
    CHECK(f.system->run(1'000'000u) == mk::RunResult::kDeadline);
    CHECK_EQ(f.word(kData), 0u);
    CHECK_EQ(f.word(kData + 4u), 0u);
    CHECK_EQ(f.word(kData + 16u), 1u);
    CHECK_EQ(f.word(kData + 8u), 3u);
    // The third vblank starts 3 * 1001/60 ms - 0.7315 ms in: 49 318.5 us.
    CHECK(f.word(kData + 12u) >= 49'319u && f.word(kData + 12u) < 49'400u);
    // 1 s at 59.94 Hz: the 60th vblank would start at 1 000 268.5 us.
    const mh::Display &display = f.system->display();
    CHECK_EQ(display.vcount(), 59u);
    CHECK_EQ(f.word(kData + 0x40u), display.vcount());
    CHECK_EQ(f.word(kData + 0x44u), 2u);      // $a0: the sub-interrupt
    CHECK_EQ(f.word(kData + 0x48u), 0x1234u); // $a1: its argument
    CHECK_EQ(f.system->interrupts().handled(), 59u);
    CHECK(display.flips() >= 59u);
    CHECK(!f.system->kernel().in_interrupt());
}

// ---- F10b: sceCtrl ------------------------------------------------------------

// The controller is sampled when each vblank ends. The first read gets the
// buffer __CtrlInit leaves, the second blocks until the first sample; after
// 150 ms (Cross held from 100 ms) peek, the latch, a negative read and the
// size check.
void test_ctrl() {
    Fixture f({}, true);
    psprecomp::Runtime &rt = f.runtime;
    f.system->ctrl().set_sampler([](std::uint64_t now) {
        mpo::frontend::SceCtrlData data;
        if (now >= 100'000u && now < 200'000u) data.buttons = mpo::frontend::ctrl::kCross;
        // Bits a game must not see (CTRL_MASK_USER), and the stick.
        data.buttons |= 0x01000000u;
        data.analog[0][0] = 200;
        data.analog[0][1] = 50;
        return data;
    });

    Asm main(rt, kCode);
    const std::uint32_t entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.li(a0, 1u); // Analog.
    main.call(kCtrlSetSamplingMode);
    main.sw(v0, 0, s7);
    main.li(a0, kData + 0x100u);
    main.li(a1, 8u);
    main.call(kCtrlReadBufferPositive);
    main.sw(v0, 4, s7);
    main.li(a0, kData + 0x180u);
    main.li(a1, 8u);
    main.call(kCtrlReadBufferPositive); // Blocks until the first sample.
    main.sw(v0, 8, s7);
    main.call(kGetSystemTimeWide);
    main.sw(v0, 12, s7);
    main.li(a0, 150'000u);
    main.call(kDelayThread);
    main.li(a0, kData + 0x200u);
    main.li(a1, 1u);
    main.call(kCtrlPeekBufferPositive);
    main.sw(v0, 16, s7);
    main.li(a0, kData + 0x280u);
    main.call(kCtrlReadLatch);
    main.sw(v0, 20, s7);
    main.li(a0, kData + 0x300u);
    main.li(a1, 1u);
    main.call(kCtrlReadBufferNegative);
    main.sw(v0, 24, s7);
    main.li(a0, kData + 0x380u);
    main.li(a1, 65u); // More than the 64 buffers.
    main.call(kCtrlPeekBufferPositive);
    main.sw(v0, 28, s7);
    main.li(a0, 0u); // Back to digital.
    main.call(kCtrlSetSamplingMode);
    main.sw(v0, 32, s7);
    main.li(t0, 1u);
    main.sw(t0, 36, s7);
    main.leave();

    (void)start_thread(*f.system, "ctrl", entry, 0x20u);
    CHECK(f.system->run(400'000u) == mk::RunResult::kDeadline);
    CHECK_EQ(f.word(kData + 36u), 1u);
    CHECK_EQ(f.word(kData), 0u);      // It was digital.
    CHECK_EQ(f.word(kData + 4u), 1u); // The buffer __CtrlInit leaves.
    CHECK_EQ(f.word(kData + 0x100u), 0u);
    CHECK_EQ(f.word(kData + 0x104u), 0u);
    CHECK_EQ(f.word(kData + 0x108u), 0x80808080u);
    // The first sample, when the first vblank ends at 1001/60 ms (16 684 us).
    CHECK_EQ(f.word(kData + 8u), 1u);
    CHECK_EQ(f.word(kData + 0x180u), 16'684u);
    CHECK_EQ(f.word(kData + 0x184u), 0u);
    CHECK_EQ(f.word(kData + 0x188u), 0x808032C8u); // Analog: x 200, y 50; stick 2 centred.
    CHECK_EQ(f.word(kData + 0x18Cu), 0u);
    CHECK(f.word(kData + 12u) >= 16'684u && f.word(kData + 12u) < 16'800u);
    // Peek at ~167 ms: Cross, and only the user bits.
    CHECK_EQ(f.word(kData + 16u), 1u);
    CHECK_EQ(f.word(kData + 0x204u), mpo::frontend::ctrl::kCross);
    CHECK_EQ(f.word(kData + 0x208u), 0x808032C8u);
    // The latch counts every sample since boot: Cross was made and pressed.
    CHECK(f.word(kData + 20u) >= 9u && f.word(kData + 20u) <= 10u);
    CHECK_EQ(f.word(kData + 0x280u), mpo::frontend::ctrl::kCross); // make
    CHECK_EQ(f.word(kData + 0x284u), 0u);                          // break
    CHECK_EQ(f.word(kData + 0x288u), mpo::frontend::ctrl::kCross); // press
    CHECK_EQ(f.word(kData + 0x28Cu), 0xFFFFFFFFu);                 // release
    // A negative read of the newest buffer.
    CHECK_EQ(f.word(kData + 24u), 1u);
    CHECK_EQ(f.word(kData + 0x304u), ~mpo::frontend::ctrl::kCross);
    CHECK_EQ(f.word(kData + 28u), 0x80000104u); // SCE_KERNEL_ERROR_INVALID_SIZE
    CHECK_EQ(f.word(kData + 32u), 1u);          // It was analog.
    CHECK(!f.system->ctrl().analog());
    // 400 ms at 59.94 Hz: 23 vblanks have ended.
    CHECK_EQ(f.system->ctrl().samples(), 23u);
}

// IO from a synthetic ISO: async open and read on the fd's helper thread,
// poll, a synchronous seek and read, close, a missing file, a listing.
void test_io() {
    std::vector<std::uint8_t> payload(3000);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(i * 13u);
    const mpo::test::SyntheticIso iso = mpo::test::build_iso({
        {"PSP_GAME/USRDIR/data.bin", payload},
        {"PSP_GAME/USRDIR/other.bin", std::vector<std::uint8_t>(10, 1)},
    });
    mh::SystemConfig config;
    config.disc_iso = mpo::test::write_temp_file("mpo_hle_test.iso", iso.bytes);
    {
        Fixture f(config);
        std::string error;
        CHECK(f.system->mount(&error));
        psprecomp::Runtime &rt = f.runtime;
        put_string(rt, kStrings, "disc0:/PSP_GAME/USRDIR/data.bin");
        put_string(rt, kStrings + 0x40u, "disc0:/PSP_GAME/USRDIR/missing.bin");
        put_string(rt, kStrings + 0x80u, "disc0:/PSP_GAME/USRDIR");
        constexpr std::uint32_t kBuffer = kData + 0x1000u;
        constexpr std::uint32_t kDirent = kData + 0x3000u;

        Asm main(rt, kCode);
        const std::uint32_t entry = main.here();
        main.enter();
        main.li(s7, kData);
        main.li(a0, kStrings);
        main.li(a1, 1u); // PSP_O_RDONLY
        main.li(a2, 0u);
        main.call(kIoOpenAsync);
        main.move(s0, v0);
        main.sw(v0, 0x00, s7);
        main.move(a0, s0);
        main.li(a1, kData + 0x80u);
        main.call(kIoWaitAsyncCB);
        main.sw(v0, 0x04, s7);
        main.move(a0, s0);
        main.li(a1, kBuffer);
        main.li(a2, 3000u);
        main.call(kIoReadAsync);
        main.sw(v0, 0x08, s7);
        main.move(a0, s0);
        main.li(a1, kData + 0x88u);
        main.call(kIoWaitAsyncCB);
        main.sw(v0, 0x0C, s7);
        main.move(a0, s0);
        main.li(a1, kData + 0x90u);
        main.call(kIoPollAsync);
        main.sw(v0, 0x10, s7);
        main.move(a0, s0);
        main.li(a1, 2990u);
        main.li(a2, 0u);
        main.call(kIoLseek32);
        main.sw(v0, 0x14, s7);
        main.move(a0, s0);
        main.li(a1, kBuffer + 0x1000u);
        main.li(a2, 100u);
        main.call(kIoRead);
        main.sw(v0, 0x18, s7);
        main.move(a0, s0);
        main.call(kIoClose);
        main.sw(v0, 0x1C, s7);
        main.li(a0, kStrings + 0x40u);
        main.li(a1, 1u);
        main.li(a2, 0u);
        main.call(kIoOpen);
        main.sw(v0, 0x20, s7);
        main.li(a0, kStrings + 0x80u);
        main.call(kIoDopen);
        main.move(s1, v0);
        main.move(a0, s1);
        main.li(a1, kDirent);
        main.call(kIoDread);
        main.sw(v0, 0x24, s7);
        main.move(a0, s1);
        main.li(a1, kDirent + 0x200u);
        main.call(kIoDread);
        main.sw(v0, 0x28, s7);
        main.move(a0, s1);
        main.li(a1, kDirent + 0x400u);
        main.call(kIoDread);
        main.sw(v0, 0x2C, s7);
        main.move(a0, s1);
        main.call(kIoDclose);
        main.sw(v0, 0x30, s7);
        main.call(kGetSystemTimeWide);
        main.sw(v0, 0x34, s7);
        main.li(t0, 1u);
        main.sw(t0, 0x38, s7);
        main.leave();

        (void)start_thread(*f.system, "io", entry, 0x20u);
        CHECK(f.system->run(10'000'000u) == mk::RunResult::kIdle);
        CHECK_EQ(f.word(kData + 0x38u), 1u);
        const std::uint32_t fd = f.word(kData);
        CHECK_EQ(fd, 3u);                             // The lowest free fd.
        CHECK_EQ(f.word(kData + 0x04u), 0u);          // WaitAsyncCB
        CHECK_EQ(f.dword(kData + 0x80u), fd);         // The open's result: the fd.
        CHECK_EQ(f.word(kData + 0x08u), 0u);          // ReadAsync
        CHECK_EQ(f.dword(kData + 0x88u), 3000u);      // The read's result.
        CHECK_EQ(f.word(kData + 0x10u), 0x8002032Au); // PollAsync: SCE_KERNEL_ERROR_NOASYNC
        CHECK_EQ(f.word(kData + 0x14u), 2990u);
        CHECK_EQ(f.word(kData + 0x18u), 10u); // Clamped at the end of the file.
        CHECK_EQ(f.word(kData + 0x1Cu), 0u);
        CHECK_EQ(f.word(kData + 0x20u), 0x80010002u); // ERRNO_FILE_NOT_FOUND
        bool same = true;
        for (std::size_t i = 0; i < payload.size(); ++i)
            same = same && rt.memory().load8(kBuffer + static_cast<std::uint32_t>(i)) == payload[i];
        CHECK(same);
        CHECK_EQ(rt.memory().load8(kBuffer + 0x1000u), payload[2990]);
        CHECK_EQ(f.word(kData + 0x24u), 1u);
        CHECK_EQ(f.word(kData + 0x28u), 1u);
        CHECK_EQ(f.word(kData + 0x2Cu), 0u); // The end of the listing.
        CHECK_EQ(f.word(kData + 0x30u), 0u);
        // On-disc order: data.bin, other.bin; SceIoStat says a regular file.
        const auto name_at = [&rt](std::uint32_t address) {
            std::string name;
            for (std::uint8_t c = rt.memory().load8(address); c != 0u;
                 c = rt.memory().load8(++address))
                name += static_cast<char>(c);
            return name;
        };
        CHECK(name_at(kDirent + 0x58u) == "data.bin");
        CHECK(name_at(kDirent + 0x258u) == "other.bin");
        CHECK_EQ(f.word(kDirent) & 0x2000u, 0x2000u);
        CHECK_EQ(f.dword(kDirent + 8u), 3000u);
        // UMD open 4 ms (async), read, sync read 100 us, close, a missing file
        // 6 ms, the first dread 1 ms.
        CHECK(f.word(kData + 0x34u) > 11'000u && f.word(kData + 0x34u) < 13'000u);
        CHECK_EQ(f.system->io().open_files(), 0u);
        bool helper = false;
        for (const mk::Uid uid : f.system->kernel().thread_list()) {
            mk::ThreadInfo info;
            if (f.system->kernel().thread_info(uid, info) && info.name == "SceIoAsync")
                helper = true;
        }
        CHECK(!helper); // Closing the fd deleted its helper thread.
    }
    std::error_code ec;
    std::filesystem::remove(config.disc_iso, ec);
}

// The RTC on a fixed base (2001-09-09 01:46:40 UTC) in a UTC+2 zone.
void test_rtc() {
    mh::SystemConfig config;
    config.rtc_from_host = false;
    config.rtc_base_seconds = 1'000'000'000;
    config.rtc_utc_offset_seconds = 7200;
    Fixture f(config);
    psprecomp::Runtime &rt = f.runtime;
    Asm main(rt, kCode);
    const std::uint32_t entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.li(a0, kData);
    main.call(kRtcGetCurrentTick);
    main.li(a0, kData + 8u);
    main.call(kLibcTime);
    main.sw(v0, 12, s7);
    main.li(a0, kData + 0x20u);
    main.call(kRtcGetClockLocal);
    main.leave();
    (void)start_thread(*f.system, "rtc", entry, 0x20u);
    CHECK(f.system->run(10'000'000u) == mk::RunResult::kIdle);
    const std::uint64_t base_ticks = 62'135'596'800'000'000ull + 1'000'000'000ull * 1'000'000ull;
    CHECK(f.dword(kData) >= base_ticks && f.dword(kData) < base_ticks + 10'000u);
    CHECK_EQ(f.word(kData + 8u), 1'000'000'000u);
    CHECK_EQ(f.word(kData + 12u), 1'000'000'000u);
    CHECK_EQ(f.word(kData + 0x20u), 2001u | (9u << 16u)); // year, month
    CHECK_EQ(f.word(kData + 0x24u), 9u | (3u << 16u));    // day, hour (local)
    CHECK_EQ(f.word(kData + 0x28u), 46u | (40u << 16u));  // minute, second
}

// ---- F11b: sceGe and sceMpeg ----------------------------------------------------

// A display list copies 16 bytes into VRAM with a block transfer, signals
// (handler, suspend) and finishes; ListSync returns once the finish handler
// ran, with the token, the callback's argument and the END's pc + 4. A second
// list starts stalled and runs when its stall address moves.
void test_ge() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    (void)f.system->kernel().set_compiled_sdk_version(0x02080010u);

    // Sub-interrupt handlers: counter, sum of $a0, last $a1, last $a2.
    const auto handler = [&rt](std::uint32_t at, std::uint32_t out) {
        Asm h(rt, at);
        h.li(t0, out);
        h.lw(t1, 0, t0);
        h.addiu(t1, t1, 1);
        h.sw(t1, 0, t0);
        h.lw(t1, 4, t0);
        h.addu(t1, t1, a0);
        h.sw(t1, 4, t0);
        h.sw(a1, 8, t0);
        h.sw(a2, 12, t0);
        h.ret();
        return at;
    };
    const std::uint32_t finish = handler(kCode + 0x800u, kData + 0x40u);
    const std::uint32_t signal = handler(kCode + 0x900u, kData + 0x50u);
    // PspGeCallbackData: signal_func, signal_arg, finish_func, finish_arg.
    rt.memory().store32(kData + 0x400u, signal);
    rt.memory().store32(kData + 0x404u, 0x5151u);
    rt.memory().store32(kData + 0x408u, finish);
    rt.memory().store32(kData + 0x40Cu, 0xF1F1u);

    const std::uint32_t source = kData + 0x2000u;
    for (std::uint32_t i = 0; i < 4u; ++i) rt.memory().store32(source + 4u * i, 0xA0B0C0D0u + i);
    const std::uint32_t list_a = kData + 0x1000u;
    const std::uint32_t list_a_words[] = {
        0xB2000000u | (source & 0xFFFFF0u),              // TRANSFERSRC
        0xB3000000u | ((source >> 8u) & 0xFF0000u) | 8u, // TRANSFERSRCW, stride 8
        0xB4000000u,                                     // TRANSFERDST 0x04000000
        0xB5000000u | 0x040000u | 8u,                    // TRANSFERDSTW
        0xEB000000u,                                     // TRANSFERSRCPOS
        0xEC000000u,                                     // TRANSFERDSTPOS
        0xEE000003u,                                     // TRANSFERSIZE 4 x 1
        0xEA000001u,                                     // TRANSFERSTART, 32 bits
        0x0E010042u,                                     // SIGNAL handler-suspend 0x42
        0x0C000000u,                                     // END
        0x0F001234u,                                     // FINISH 0x1234
        0x0C000000u,                                     // END
    };
    for (std::uint32_t i = 0; i < std::size(list_a_words); ++i)
        rt.memory().store32(list_a + 4u * i, list_a_words[i]);
    const std::uint32_t list_b = kData + 0x1100u;
    rt.memory().store32(list_b, 0x0F000077u); // FINISH 0x77
    rt.memory().store32(list_b + 4u, 0x0C000000u);

    Asm main(rt, kCode);
    const std::uint32_t entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.call(kGeEdramGetAddr);
    main.sw(v0, 0, s7);
    main.li(a0, kData + 0x400u);
    main.call(kGeSetCallback);
    main.sw(v0, 4, s7);
    main.move(s0, v0);
    main.li(a0, list_a);
    main.li(a1, 0u);
    main.move(a2, s0);
    main.li(a3, 0u);
    main.call(kGeListEnQueue);
    main.sw(v0, 8, s7);
    main.move(a0, v0);
    main.li(a1, 0u);
    main.call(kGeListSync); // Waits for the finish interrupt.
    main.sw(v0, 12, s7);
    main.lw(t0, 0x40, s7);
    main.sw(t0, 16, s7); // Finish handlers run by then.
    main.li(a0, 0u);
    main.call(kGeDrawSync);
    main.sw(v0, 20, s7);
    main.li(a0, list_b);
    main.li(a1, list_b); // Stalled at its first command.
    main.move(a2, s0);
    main.li(a3, 0u);
    main.call(kGeListEnQueue);
    main.sw(v0, 24, s7);
    main.move(s1, v0);
    main.move(a0, s1);
    main.li(a1, 1u);
    main.call(kGeListSync);
    main.sw(v0, 28, s7);
    main.move(a0, s1);
    main.li(a1, list_b + 8u);
    main.call(kGeListUpdateStallAddr);
    main.sw(v0, 32, s7);
    main.move(a0, s1);
    main.li(a1, 0u);
    main.call(kGeListSync);
    main.sw(v0, 36, s7);
    main.li(t0, 1u);
    main.sw(t0, 40, s7);
    main.leave();

    // F14b: the after-run hook sees every ge::Ge::Run, before guest code.
    std::uint64_t after_runs = 0;
    f.system->ge().set_after_run([&] { ++after_runs; });

    (void)start_thread(*f.system, "ge", entry, 0x20u);
    CHECK(f.system->run(1'000'000u) == mk::RunResult::kIdle);
    CHECK(!rt.stopped());
    CHECK_EQ(f.word(kData + 40u), 1u);
    CHECK_EQ(f.word(kData), 0x04000000u);
    CHECK_EQ(f.word(kData + 4u), 0u);          // Callback id 0.
    CHECK_EQ(f.word(kData + 8u), 0x35000000u); // LIST_ID_MAGIC ^ 0
    CHECK_EQ(f.word(kData + 12u), 0u);         // PSP_GE_LIST_COMPLETED
    CHECK_EQ(f.word(kData + 16u), 1u);
    CHECK_EQ(f.word(kData + 20u), 0u);
    CHECK_EQ(f.word(kData + 24u), 0x35000001u);
    CHECK_EQ(f.word(kData + 28u), 3u); // PSP_GE_LIST_STALLING
    CHECK_EQ(f.word(kData + 32u), 0u);
    CHECK_EQ(f.word(kData + 36u), 0u);
    // The block transfer wrote the runtime's VRAM.
    for (std::uint32_t i = 0; i < 4u; ++i) CHECK_EQ(f.word(0x04000000u + 4u * i), 0xA0B0C0D0u + i);
    // Signal: once, token 0x42, its argument, the END's pc + 4.
    CHECK_EQ(f.word(kData + 0x50u), 1u);
    CHECK_EQ(f.word(kData + 0x54u), 0x42u);
    CHECK_EQ(f.word(kData + 0x58u), 0x5151u);
    CHECK_EQ(f.word(kData + 0x5Cu), list_a + 4u * 9u + 4u);
    // Finish: both lists, tokens 0x1234 and 0x77.
    CHECK_EQ(f.word(kData + 0x40u), 2u);
    CHECK_EQ(f.word(kData + 0x44u), 0x1234u + 0x77u);
    CHECK_EQ(f.word(kData + 0x48u), 0xF1F1u);
    CHECK_EQ(f.word(kData + 0x4Cu), list_b + 8u);
    const mh::GeManager::Stats &stats = f.system->ge().stats();
    CHECK_EQ(stats.lists, 2u);
    CHECK_EQ(stats.interrupts, 3u);
    CHECK_EQ(stats.handlers, 3u);
    CHECK_EQ(stats.signals, 1u);
    CHECK_EQ(stats.finishes, 2u);
    // list_a to its signal and then to its finish; list_b to its stall and
    // then to its finish.
    CHECK_EQ(stats.runs, 4u);
    CHECK_EQ(after_runs, stats.runs);
    CHECK(!f.system->kernel().in_interrupt());
}

// A PSMF header gives the stream offset and size; the ring buffer and the
// mpeg are laid out as in PPSSPP, and sceMpegRingbufferPut calls the game's
// read callback with the ring's free space.
void test_mpeg() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    const std::uint32_t header = kData + 0x8000u;
    const std::uint8_t fields[] = {'P', 'S', 'M',  'F',  '0', '0',  '1',  '5',
                                   0,   0,   0x08, 0x00, 0,   0x01, 0x00, 0x00};
    for (std::uint32_t i = 0; i < sizeof(fields); ++i) rt.memory().store8(header + i, fields[i]);
    rt.memory().store8(header + 0x54u + 3u, 0x01u); // First timestamp 90000.
    rt.memory().store8(header + 0x54u + 4u, 0x5Fu);
    rt.memory().store8(header + 0x54u + 5u, 0x90u);
    rt.memory().store8(header + 142u, 30u); // 480 x 272
    rt.memory().store8(header + 143u, 17u);

    // The read callback: records its arguments, "reads" everything.
    Asm cb(rt, kCode + 0xA00u);
    const std::uint32_t callback = cb.here();
    cb.li(t0, kData + 0x80u);
    cb.sw(a0, 0, t0);
    cb.sw(a1, 4, t0);
    cb.sw(a2, 8, t0);
    cb.move(v0, a1);
    cb.ret();

    const std::uint32_t ring = kData + 0x500u;
    const std::uint32_t mpeg = kData + 0x600u;
    const std::uint32_t ring_data = kData + 0x10000u;
    const std::uint32_t mpeg_data = kData + 0x20000u;
    Asm main(rt, kCode);
    const std::uint32_t entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.call(kMpegInit);
    main.sw(v0, 0, s7);
    main.li(a0, 8u);
    main.call(kMpegRingbufferQueryMemSize);
    main.sw(v0, 4, s7);
    main.li(a0, ring);
    main.li(a1, 8u);
    main.li(a2, ring_data);
    main.li(a3, 8u * 2152u);
    main.li(t0, callback);
    main.li(t1, 0x77u);
    main.call(kMpegRingbufferConstruct);
    main.sw(v0, 8, s7);
    main.call(kMpegQueryMemSize);
    main.sw(v0, 12, s7);
    main.li(a0, mpeg);
    main.li(a1, mpeg_data);
    main.li(a2, 0x10000u);
    main.li(a3, ring);
    main.li(t0, 512u);
    main.li(t1, 0u);
    main.li(t2, 0u);
    main.call(kMpegCreate);
    main.sw(v0, 16, s7);
    main.li(a0, mpeg);
    main.li(a1, header);
    main.li(a2, kData + 0x100u);
    main.call(kMpegQueryStreamOffset);
    main.sw(v0, 20, s7);
    main.li(a0, header);
    main.li(a1, kData + 0x104u);
    main.call(kMpegQueryStreamSize);
    main.sw(v0, 24, s7);
    main.li(a0, mpeg);
    main.li(a1, 0u); // AVC
    main.li(a2, 0u);
    main.call(kMpegRegistStream);
    main.sw(v0, 28, s7);
    main.li(a0, ring);
    main.call(kMpegRingbufferAvailableSize);
    main.sw(v0, 32, s7);
    main.li(a0, ring);
    main.li(a1, 4u);
    main.li(a2, 8u);
    main.call(kMpegRingbufferPut);
    main.sw(v0, 36, s7);
    main.li(a0, ring);
    main.call(kMpegRingbufferAvailableSize);
    main.sw(v0, 40, s7);
    main.li(t0, 1u);
    main.sw(t0, 44, s7);
    main.leave();

    (void)start_thread(*f.system, "mpeg", entry, 0x20u);
    CHECK(f.system->run(1'000'000u) == mk::RunResult::kIdle);
    CHECK(!rt.stopped());
    CHECK_EQ(f.word(kData + 44u), 1u);
    CHECK_EQ(f.word(kData), 0u);
    CHECK_EQ(f.word(kData + 4u), 8u * (104u + 2048u));
    CHECK_EQ(f.word(kData + 8u), 0u);
    CHECK_EQ(f.word(kData + 12u), 0x10000u); // libmpeg 0x010A
    CHECK_EQ(f.word(kData + 16u), 0u);
    // SceMpegRingBuffer.
    CHECK_EQ(f.word(ring), 8u);
    CHECK_EQ(f.word(ring + 16u), 2048u);
    CHECK_EQ(f.word(ring + 20u), ring_data);
    CHECK_EQ(f.word(ring + 24u), callback);
    CHECK_EQ(f.word(ring + 28u), 0x77u);
    CHECK_EQ(f.word(ring + 32u), ring_data + 8u * 2048u);
    CHECK_EQ(f.word(ring + 40u), mpeg);
    // The handle lives in the mpeg data, with its fake struct.
    CHECK_EQ(f.word(mpeg), mpeg_data + 0x30u);
    CHECK_EQ(f.word(mpeg_data + 0x30u), 0x4D42494Cu); // "LIBM"
    CHECK_EQ(f.word(mpeg_data + 0x40u), ring);
    // The header.
    CHECK_EQ(f.word(kData + 20u), 0u);
    CHECK_EQ(f.word(kData + 0x100u), 0x800u);
    CHECK_EQ(f.word(kData + 24u), 0u);
    CHECK_EQ(f.word(kData + 0x104u), 0x10000u);
    CHECK_EQ(f.word(kData + 28u), 1u); // The first stream id.
    // Put: the callback reads 4 packets at the start of the ring.
    CHECK_EQ(f.word(kData + 32u), 8u);
    CHECK_EQ(f.word(kData + 36u), 4u);
    CHECK_EQ(f.word(kData + 0x80u), ring_data);
    CHECK_EQ(f.word(kData + 0x84u), 4u);
    CHECK_EQ(f.word(kData + 0x88u), 0x77u);
    CHECK_EQ(f.word(ring + 4u), 4u);  // packetsRead
    CHECK_EQ(f.word(ring + 8u), 4u);  // packetsWritePos
    CHECK_EQ(f.word(ring + 12u), 4u); // packetsAvail
    CHECK_EQ(f.word(kData + 40u), 4u);
}

// sceAudio, sceSasCore and sceAtrac3plus on mpo_audio.
void test_audio() {
    Fixture f;
    psprecomp::Runtime &rt = f.runtime;
    std::vector<std::int32_t> mixed;
    f.system->audio().set_output([&mixed](const std::int32_t *mix, int frames) {
        mixed.insert(mixed.end(), mix, mix + 2 * frames);
    });
    constexpr std::uint32_t kSamples = kData + 0x1000u;
    constexpr std::uint32_t kFrames = 256u;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const auto value = static_cast<std::uint16_t>(100u + i);
        rt.memory().store16(kSamples + 4u * i, value);
        rt.memory().store16(kSamples + 4u * i + 2u, static_cast<std::uint16_t>(-value));
    }
    constexpr std::uint32_t kSasCore = kData + 0x2000u; // 64-byte aligned.
    constexpr std::uint32_t kSasBuffer = kData + 0x3000u;

    Asm main(rt, kCode);
    const std::uint32_t entry = main.here();
    main.enter();
    main.li(s7, kData);
    main.li(a0, 0x1000u); // PSP_CODEC_AT3PLUS, before the module.
    main.call(kAtracGetAtracID);
    main.sw(v0, 0, s7);
    main.li(a0, 0u); // PSP_AV_MODULE_AVCODEC
    main.call(kUtilityLoadAvModule);
    main.sw(v0, 4, s7);
    main.li(a0, 2u); // PSP_AV_MODULE_ATRAC3PLUS
    main.call(kUtilityLoadAvModule);
    main.sw(v0, 8, s7);
    main.li(a0, 0x1001u); // PSP_CODEC_AT3
    main.call(kAtracGetAtracID);
    main.sw(v0, 12, s7);

    main.li(a0, 0xFFFFFFFFu); // Any channel.
    main.li(a1, kFrames);
    main.li(a2, 0u); // Stereo.
    main.call(kAudioChReserve);
    main.sw(v0, 16, s7);
    main.move(s0, v0);
    const auto output = [&](Stub s) {
        main.move(a0, s0);
        main.li(a1, 0x8000u);
        main.li(a2, 0x8000u);
        main.li(a3, kSamples);
        main.call(s);
    };
    output(kAudioOutputPannedBlocking); // Empty queue: no wait.
    main.sw(v0, 20, s7);
    main.call(kGetSystemTimeWide);
    main.sw(v0, 24, s7);
    output(kAudioOutputPanned); // Queued: busy.
    main.sw(v0, 28, s7);
    output(kAudioOutputPannedBlocking); // Waits for the 256 queued frames.
    main.sw(v0, 32, s7);
    main.call(kGetSystemTimeWide);
    main.sw(v0, 36, s7);
    main.move(a0, s0);
    main.call(kAudioGetChannelRestLength);
    main.sw(v0, 40, s7);

    main.li(a0, kSasCore);
    main.li(a1, 256u);
    main.li(a2, 32u);
    main.li(a3, 0u);
    main.li(t0, 44100u);
    main.call(kSasInit);
    main.sw(v0, 44, s7);
    main.call(kGetSystemTimeWide);
    main.sw(v0, 48, s7);
    main.li(a0, kSasCore);
    main.li(a1, kSasBuffer);
    main.li(a2, 0x1000u);
    main.li(a3, 0x1000u);
    main.call(kSasCoreWithMix);
    main.sw(v0, 52, s7);
    main.call(kGetSystemTimeWide);
    main.sw(v0, 56, s7);
    main.li(t0, 1u);
    main.sw(t0, 60, s7);
    main.leave();

    (void)start_thread(*f.system, "audio", entry, 0x20u);
    CHECK(f.system->run(100'000u) == mk::RunResult::kDeadline);
    CHECK_EQ(f.word(kData + 60u), 1u);
    CHECK_EQ(f.word(kData), 0x80630003u); // SCE_ERROR_ATRAC_NO_ATRACID
    CHECK_EQ(f.word(kData + 4u), 0u);
    CHECK_EQ(f.word(kData + 8u), 0u);
    CHECK_EQ(f.word(kData + 12u), 2u); // Contexts 2 and 3 start as ATRAC3.
    CHECK_EQ(f.word(kData + 16u), 7u); // GetFreeChannel: from the top.
    CHECK_EQ(f.word(kData + 20u), kFrames);
    CHECK_EQ(f.word(kData + 28u), 0x80260002u); // SCE_ERROR_AUDIO_CHANNEL_BUSY
    CHECK_EQ(f.word(kData + 32u), kFrames);
    // Woken by the fourth mixer update after the first output: 4 x 322176
    // cycles at 222 MHz (5805 us), give or take where the first one fell.
    const std::uint32_t waited = f.word(kData + 36u) - f.word(kData + 24u);
    CHECK(waited > 4u * 1451u - 1452u && waited <= 4u * 1452u);
    CHECK_EQ(f.word(kData + 40u), kFrames); // The second copy is still queued.
    CHECK_EQ(f.word(kData + 44u), 0u);
    CHECK_EQ(f.word(kData + 52u), 0u);
    // EstimateMixUs with no voice playing and a 256-sample grain: 20 + 153.
    const std::uint32_t mix_us = f.word(kData + 56u) - f.word(kData + 48u);
    CHECK(mix_us >= 173u && mix_us < 200u);

    // The mix: both copies, back to back, at full volume (0x8000).
    const mh::Audio::Stats &stats = f.system->audio().stats();
    CHECK_EQ(stats.channels_reserved, 1u);
    CHECK_EQ(stats.blocking_waits, 1u);
    CHECK_EQ(stats.sas_mixes, 1u);
    CHECK_EQ(stats.blocks, mixed.size() / 128u);
    std::size_t first = 0;
    while (first < mixed.size() && mixed[first] == 0) ++first;
    CHECK(first % 128u == 0u);
    CHECK(mixed.size() >= first + 4u * kFrames);
    for (std::uint32_t i = 0; i < 2u * kFrames && first + 2u * i + 1u < mixed.size(); ++i) {
        const auto value = static_cast<std::int32_t>(100u + i % kFrames);
        if (mixed[first + 2u * i] != value || mixed[first + 2u * i + 1u] != -value) {
            CHECK(false);
            break;
        }
    }
    // Nothing after the two copies.
    for (std::size_t i = first + 4u * kFrames; i < mixed.size(); ++i) {
        if (mixed[i] != 0) {
            CHECK(false);
            break;
        }
    }
}

void run_test(const char *name, void (*test)()) {
    const int before = g_failures;
    std::fprintf(stderr, "%s...\n", name);
    test();
    std::fprintf(stderr, "%s: %s\n", name, g_failures == before ? "ok" : "FAILED");
}

} // namespace

int main() {
    // Every test assembles real Allegrex code, which only the interpreter runs.
    psprecomp::set_interpreter_fallback_enabled(true);
    run_test("test_threads_and_semaphores", &test_threads_and_semaphores);
    run_test("test_callbacks", &test_callbacks);
    run_test("test_preemption_from_guest_loop", &test_preemption_from_guest_loop);
    run_test("test_exit_and_restart", &test_exit_and_restart);
    run_test("test_unimplemented_imports", &test_unimplemented_imports);
    run_test("test_module_loader", &test_module_loader);
    run_test("test_overlays", &test_overlays);
    run_test("test_runtime_memory", &test_runtime_memory);
    run_test("test_vblank_and_interrupts", &test_vblank_and_interrupts);
    run_test("test_ctrl", &test_ctrl);
    run_test("test_io", &test_io);
    run_test("test_rtc", &test_rtc);
    run_test("test_ge", &test_ge);
    run_test("test_mpeg", &test_mpeg);
    run_test("test_audio", &test_audio);
    std::printf("mpo_hle_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
