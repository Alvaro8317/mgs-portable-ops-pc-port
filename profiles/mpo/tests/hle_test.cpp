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
// module, the boot IO stub and the memory adapter.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "hle/system.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

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
constexpr int zero = 0, v0 = 2, a0 = 4, a1 = 5, a2 = 6, a3 = 7, t0 = 8, t1 = 9, s0 = 16, s1 = 17,
              s2 = 18, s6 = 22, s7 = 23, t9 = 25, ra = 31;

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
    {"ModuleMgrForUser", 0x2E0911AAu},
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
    explicit Fixture(mh::SystemConfig cfg = {}) : config(std::move(cfg)) {
        config.fiber_stack_size = 1u << 20u;
        register_stubs(runtime);
        system = std::make_unique<mh::System>(runtime, config);
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
    code.li(a0, uid);
    code.call(kUnloadModule);
    code.sw(v0, 12, s7);
    code.leave();
    rt.memory().store32(kData + 0x40u, 0xFFFFFFFFu);
    rt.memory().store32(kData + 0x44u, 0xFFFFFFFFu);
    (void)start_thread(system, "loader", entry, 0x20u);
    CHECK(system.run(10'000'000u) == mk::RunResult::kIdle);
    CHECK(!rt.stopped());
    CHECK_EQ(f.word(kData), uid);
    CHECK_EQ(f.word(kData + 0x40u), 7u);
    CHECK_EQ(f.word(kData + 4u), uid2);
    CHECK_EQ(f.word(kData + 0x44u), 7u);
    CHECK_EQ(f.word(kData + 8u), 105u);
    CHECK_EQ(f.word(kData + 12u), uid);
    CHECK(modules.find(static_cast<mk::Uid>(uid)) == nullptr);
    CHECK(modules.find(static_cast<mk::Uid>(uid2)) != nullptr);
    // Its memory is free again: the next load lands there.
    const std::uint32_t uid3 = modules.load(prx, "disc0:/test3.prx", 0u, false, &error);
    const mh::Module *module3 = modules.find(static_cast<mk::Uid>(uid3));
    CHECK(module3 != nullptr && module3->base == 0x08804000u);
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

void test_boot_io_and_memory() {
    mh::BootIo io("/nonexistent-mpo-disc");
    CHECK(io.host_path("disc0:/PSP_GAME/USRDIR/modules/a.prx") ==
          std::filesystem::path("/nonexistent-mpo-disc") / "PSP_GAME" / "USRDIR" / "modules" /
              "a.prx");
    CHECK(io.host_path("umd1:PSP_GAME/x").filename() == "x");
    CHECK(io.host_path("disc0:/../etc/passwd").empty());
    CHECK(io.host_path("ms0:/PSP/SAVEDATA/x").empty());
    CHECK_EQ(io.open("disc0:/missing", 1u), 0x80010002u);
    CHECK_EQ(io.open("disc0:/missing", 2u), 0x80010016u); // write access
    CHECK_EQ(io.close(3u), 0x80020323u);

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
    run_test("test_boot_io_and_memory", &test_boot_io_and_memory);
    std::printf("mpo_hle_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
