// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_kernel with synthetic threads (F08a): host lambdas stand in for guest
// code, a flat buffer for PSP RAM and a virtual clock for time. Each scenario
// states the PPSSPP v1.20.4 behaviour it reproduces (docs/sessions/F08a.md
// has the citations). No game data is needed.

#include "kernel/kernel.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace mk = mpo::kernel;
namespace err = mpo::kernel::error;
namespace ts = mpo::kernel::thread_status;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *expr, const char *file, int line) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        ++g_failures;
    }
}

template <class A, class B>
void check_eq(const A &a, const B &b, const char *ea, const char *eb, const char *file, int line) {
    ++g_checks;
    if (!(a == b)) {
        if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (0x%" PRIx64 " vs 0x%" PRIx64 ")\n", file,
                         line, ea, eb, static_cast<std::uint64_t>(a),
                         static_cast<std::uint64_t>(b));
        } else {
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", file, line, ea, eb);
        }
        ++g_failures;
    }
}

void check_log(const std::vector<std::string> &got, const std::vector<std::string> &want,
               const char *file, int line) {
    ++g_checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d: log mismatch\n  got: ", file, line);
        for (const auto &s : got) std::fprintf(stderr, "[%s] ", s.c_str());
        std::fprintf(stderr, "\n  want:");
        for (const auto &s : want) std::fprintf(stderr, "[%s] ", s.c_str());
        std::fprintf(stderr, "\n");
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_LOG(got, ...)                                                                        \
    check_log((got), std::vector<std::string>{__VA_ARGS__}, __FILE__, __LINE__)

using ThreadFn = std::function<std::uint32_t(const mk::ThreadStart &)>;
using GuestFn = std::function<std::uint32_t(std::span<const std::uint32_t>)>;

class Host final : public mk::HostHooks {
public:
    std::uint32_t run_thread(mk::Kernel &, const mk::ThreadStart &start) override {
        return threads.at(start.entry)(start);
    }
    std::uint32_t call_guest(mk::Kernel &, mk::Uid, std::uint32_t entry,
                             std::span<const std::uint32_t> args) override {
        return funcs.at(entry)(args);
    }
    void on_switch(mk::Kernel &, mk::Uid, mk::Uid) override { ++switches; }
    void on_thread_released(mk::Kernel &, mk::Uid) override { ++released; }

    std::map<std::uint32_t, ThreadFn> threads;
    std::map<std::uint32_t, GuestFn> funcs;
    int switches = 0;
    int released = 0;
};

constexpr std::uint32_t kRamBase = 0x08000000u;
constexpr std::uint32_t kRamSize = 0x02000000u;
constexpr std::uint32_t kUserBase = 0x08800000u;
constexpr std::uint32_t kUserEnd = 0x0A000000u;

struct Env {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(kRamSize);
    mk::FlatGuestMemory mem{kRamBase, ram};
    mk::VirtualClock clock;
    Host host;
    mk::Kernel kernel;
    std::vector<std::string> log;
    std::uint32_t next_entry = 0x08700000u;   // Volatile range: never allocated in these tests.
    std::uint32_t next_scratch = 0x08000100u; // Kernel range: only scratch here.

    explicit Env(std::size_t stack = 256u * 1024u) : kernel(config(stack)) {}

    mk::KernelConfig config(std::size_t stack) {
        mk::KernelConfig c;
        c.memory = &mem;
        c.clock = &clock;
        c.hooks = &host;
        c.fiber_stack_size = stack;
        return c;
    }

    std::uint32_t entry(ThreadFn fn) {
        const std::uint32_t address = next_entry;
        next_entry += 4;
        host.threads[address] = std::move(fn);
        return address;
    }
    std::uint32_t guest(GuestFn fn) {
        const std::uint32_t address = next_entry;
        next_entry += 4;
        host.funcs[address] = std::move(fn);
        return address;
    }
    std::uint32_t scratch(std::uint32_t value = 0) {
        const std::uint32_t address = next_scratch;
        next_scratch += 4;
        mem.write_u32(address, value);
        return address;
    }
    mk::Uid create(const char *name, std::uint32_t priority, ThreadFn fn,
                   std::uint32_t stack = 0x4000) {
        return static_cast<mk::Uid>(
            kernel.create_thread(name, entry(std::move(fn)), priority, stack, 0, 0));
    }
    mk::Uid spawn(const char *name, std::uint32_t priority, ThreadFn fn) {
        const mk::Uid uid = create(name, priority, std::move(fn));
        kernel.start_thread(uid, 0, 0);
        return uid;
    }
    void note(const std::string &s) { log.push_back(s); }
    void note_time(const std::string &s) {
        log.push_back(s + "@" + std::to_string(clock.now_us()));
    }
    std::uint32_t status(mk::Uid uid) {
        mk::ThreadInfo info;
        return kernel.thread_info(uid, info) ? info.status : 0xFFFFFFFFu;
    }
};

// ---------------------------------------------------------------------------

void test_thread_lifecycle() {
    Env e;
    const std::uint32_t main_entry = e.entry([&](const mk::ThreadStart &) -> std::uint32_t {
        e.note("main");
        return 0x1234;
    });
    // KernelObjectPool: first UID is 0x100 + 0x10.
    const auto uid =
        static_cast<mk::Uid>(e.kernel.create_thread("main", main_entry, 0x20, 0x1000, 0, 0));
    CHECK_EQ(uid, 0x110);
    CHECK_EQ(e.status(uid), ts::kDormant);
    // Created threads report DORMANT as their exit status.
    CHECK_EQ(e.kernel.get_thread_exit_status(uid), err::kDormant);
    CHECK_EQ(e.kernel.start_thread(uid, 0, 0), 0u);
    CHECK_EQ(e.status(uid), ts::kReady);
    CHECK_EQ(e.kernel.start_thread(uid, 0, 0), err::kNotDormant);
    CHECK_EQ(e.kernel.get_thread_exit_status(uid), err::kNotDormant);
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "main");
    CHECK_EQ(e.status(uid), ts::kDormant);
    CHECK_EQ(e.kernel.get_thread_exit_status(uid), 0x1234u);
    // A returned thread can be started again, from its entry.
    CHECK_EQ(e.kernel.start_thread(uid, 0, 0), 0u);
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "main", "main");
    CHECK_EQ(e.kernel.delete_thread(uid), 0u);
    CHECK_EQ(e.kernel.get_thread_exit_status(uid), err::kUnknownThid);
    CHECK_EQ(e.kernel.delete_thread(uid), err::kUnknownThid);

    // __KernelCreateThread validation.
    CHECK_EQ(e.kernel.create_thread("p", main_entry, 0x07, 0x1000, 0, 0), err::kIllegalPriority);
    CHECK_EQ(e.kernel.create_thread("p", main_entry, 0x78, 0x1000, 0, 0), err::kIllegalPriority);
    CHECK_EQ(e.kernel.create_thread("s", main_entry, 0x20, 0x1FF, 0, 0), err::kIllegalStackSize);
    CHECK_EQ(e.kernel.create_thread(nullptr, main_entry, 0x20, 0x1000, 0, 0), err::kError);
    CHECK_EQ(e.kernel.create_thread("a", 0x00000100u, 0x20, 0x1000, 0, 0), err::kIllegalAddr);
    CHECK_EQ(e.kernel.create_thread("k", main_entry, 0x20, 0x1000, mk::thread_attr::kKernel, 0),
             err::kIllegalAttr);
    // UIDs only move forward (the deleted 0x110 is not reused).
    const auto second =
        static_cast<mk::Uid>(e.kernel.create_thread("second", 0, 0x20, 0x1000, 0, 0));
    CHECK_EQ(second, 0x111);
    mk::ThreadInfo info;
    CHECK(e.kernel.thread_info(second, info));
    // The firmware ORs 0xFF and the USER bit into the attributes.
    CHECK_EQ(info.attr, mk::thread_attr::kUser | 0xFFu);
    CHECK_EQ(e.kernel.start_thread(0, 0, 0), err::kIllegalThid);
    CHECK_EQ(e.kernel.start_thread(0x999, 0, 0), err::kUnknownThid);
    CHECK_EQ(e.kernel.start_thread(second, 0xFFFFFFFFu, 0), err::kIllegalAddr);
    CHECK_EQ(e.kernel.start_thread(second, 4, 0x80000000u), err::kIllegalAddr);
    CHECK_EQ(e.kernel.delete_thread(0), err::kNotDormant);
}

void test_thread_stack_layout() {
    Env e;
    const std::uint32_t args = e.scratch(0x44434241u);
    e.mem.write_u32(args + 4, 0x48474645u);
    std::uint32_t seen_sp = 0;
    mk::ThreadStart seen;
    const mk::Uid uid = e.create(
        "stack", 0x20,
        [&](const mk::ThreadStart &start) -> std::uint32_t {
            seen = start;
            seen_sp = start.sp;
            return 0;
        },
        0x1000);
    mk::ThreadInfo info;
    CHECK(e.kernel.thread_info(uid, info));
    // PSPThread::AllocateStack: from the top of user memory.
    CHECK_EQ(info.initial_stack, kUserEnd - 0x1000u);
    CHECK_EQ(info.stack_size, 0x1000u);
    CHECK_EQ(e.kernel.start_thread(uid, 8, args), 0u);
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    const std::uint32_t k0 = kUserEnd - 256u;
    CHECK_EQ(seen.k0, k0);
    // Arguments go below k0, 16-aligned; then 64 more bytes.
    CHECK_EQ(seen.a0, 8u);
    CHECK_EQ(seen.a1, k0 - 16u);
    CHECK_EQ(seen_sp, k0 - 16u - 64u);
    CHECK_EQ(seen.ra, seen_sp);
    CHECK_EQ(seen.fp, seen_sp);
    CHECK_EQ(e.mem.read_u32(seen.a1), 0x44434241u);
    CHECK_EQ(e.mem.read_u32(seen.a1 + 4), 0x48474645u);
    CHECK_EQ(e.mem.read_u32(seen_sp + 8), 0x1000FFFFu); // b -1
    CHECK_EQ(e.mem.read_u32(seen_sp + 12), 0u);         // nop
    // FillStack: the k0 block and the UID at the bottom.
    CHECK_EQ(e.mem.read_u32(k0 + 0xC0), static_cast<std::uint32_t>(uid));
    CHECK_EQ(e.mem.read_u32(k0 + 0xC8), info.initial_stack);
    CHECK_EQ(e.mem.read_u32(k0 + 0xF8), 0xFFFFFFFFu);
    CHECK_EQ(e.mem.read_u32(info.initial_stack), static_cast<std::uint32_t>(uid));
    CHECK_EQ(e.mem.read_u8(info.initial_stack + 4), 0xFFu);
    // The stack goes back to the allocator when the thread is deleted.
    CHECK_EQ(e.kernel.delete_thread(uid), 0u);
    CHECK_EQ(e.kernel.max_free_mem_size(), kUserEnd - kUserBase - 0x4000u);
}

void test_priority_and_preemption() {
    Env e;
    e.spawn("main", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        e.note("main start");
        e.spawn("A", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
            e.note("A");
            return 0;
        });
        e.note("after A"); // A is worse: no switch.
        e.spawn("B", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
            e.note("B");
            return 0;
        });
        e.note("after B"); // B was better: it ran inside start_thread.
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "main start", "after A", "B", "after B", "A");

    // Same priority: FIFO, no preemption, in start order.
    Env f;
    f.spawn("main", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        for (const char *name : {"T1", "T2", "T3"}) {
            const std::string n = name;
            f.spawn(name, 0x20, [&f, n](const mk::ThreadStart &) -> std::uint32_t {
                f.note(n);
                return 0;
            });
        }
        f.note("main end");
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "main end", "T1", "T2", "T3");

    // sceKernelRotateThreadReadyQueue(0): yield to the same priority.
    Env g;
    for (const char *name : {"X", "Y"}) {
        const std::string n = name;
        g.spawn(name, 0x20, [&g, n](const mk::ThreadStart &) -> std::uint32_t {
            for (int i = 0; i < 3; ++i) {
                g.note(n + std::to_string(i));
                CHECK_EQ(g.kernel.rotate_thread_ready_queue(0), 0u);
            }
            return 0;
        });
    }
    CHECK(g.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(g.log, "X0", "Y0", "X1", "Y1", "X2", "Y2");
    CHECK_EQ(g.kernel.rotate_thread_ready_queue(0x78), err::kIllegalContext); // No current thread.

    // sceKernelChangeThreadPriority: lowering itself below a ready thread switches.
    Env h;
    mk::Uid low = 0;
    low = h.spawn("low", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        h.spawn("other", 0x28, [&](const mk::ThreadStart &) -> std::uint32_t {
            h.note("other");
            return 0;
        });
        h.note("before");
        CHECK_EQ(h.kernel.change_thread_priority(0, 0x30), 0u);
        h.note("after");
        return 0;
    });
    CHECK(h.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(h.log, "before", "other", "after");
    CHECK_EQ(h.kernel.change_thread_priority(low, 0x20), err::kDormant);
}

void test_delays() {
    Env e;
    for (std::uint32_t usec : {500u, 100u, 1000u, 100u}) {
        const std::string name = "d" + std::to_string(usec);
        e.spawn(name.c_str(), 0x20, [&e, usec, name](const mk::ThreadStart &) -> std::uint32_t {
            CHECK_EQ(e.kernel.delay_thread(usec), 0u);
            e.note_time(name);
            return 0;
        });
    }
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    // __KernelDelayThreadUs: under 200 us takes 210, else usec + 10. Equal
    // deadlines wake in the order they were scheduled.
    CHECK_LOG(e.log, "d100@210", "d100@210", "d500@510", "d1000@1010");

    // A better thread whose delay ends preempts at the next scheduling point.
    Env f;
    f.spawn("hi", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
        f.kernel.delay_thread(300);
        f.note_time("hi");
        return 0;
    });
    f.spawn("lo", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        for (int i = 0; i < 5; ++i) {
            f.clock.advance(100);
            f.kernel.preemption_point(); // A visibility point in "guest code".
            f.note_time("lo");
        }
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "lo@100", "lo@200", "lo@300", "hi@400", "lo@400", "lo@500");

    // request_stop() from guest code returns from run() at the next
    // visibility point; the next run() continues the same thread.
    Env s;
    s.spawn("spinner", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        s.note("before stop");
        s.kernel.request_stop();
        s.kernel.preemption_point();
        s.note("after stop");
        return 0;
    });
    CHECK(s.kernel.run() == mk::RunResult::kStopped);
    CHECK_LOG(s.log, "before stop");
    CHECK(s.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(s.log, "before stop", "after stop");

    // run() deadline and host timers.
    Env g;
    int fired = 0;
    g.kernel.schedule_event(50, [&] { ++fired; });
    const mk::Kernel::EventId cancelled = g.kernel.schedule_event(80, [&] { fired += 100; });
    CHECK_EQ(g.kernel.unschedule_event(cancelled), 80);
    CHECK_EQ(g.kernel.unschedule_event(cancelled), -1);
    CHECK(g.kernel.run(40) == mk::RunResult::kDeadline);
    CHECK_EQ(g.clock.now_us(), 40u);
    CHECK_EQ(fired, 0);
    CHECK(g.kernel.run() == mk::RunResult::kIdle);
    CHECK_EQ(fired, 1);
    CHECK_EQ(g.clock.now_us(), 50u);
}

void test_semaphores() {
    // FIFO: the waiting list keeps arrival order; PRIORITY sorts by priority.
    for (std::uint32_t attr : {0u, 0x100u}) {
        Env e;
        const auto sema = static_cast<mk::Uid>(e.kernel.create_sema("s", attr, 0, 10, 0));
        e.spawn("main", 0x40, [&](const mk::ThreadStart &) -> std::uint32_t {
            for (auto [name, prio] :
                 {std::pair{"W30", 0x30u}, std::pair{"W20", 0x20u}, std::pair{"W28", 0x28u}}) {
                const std::string n = name;
                e.spawn(name, prio, [&e, n, sema](const mk::ThreadStart &) -> std::uint32_t {
                    CHECK_EQ(e.kernel.wait_sema(sema, 1, 0), 0u);
                    e.note(n);
                    return 0;
                });
            }
            for (int i = 0; i < 3; ++i) CHECK_EQ(e.kernel.signal_sema(sema, 1), 0u);
            return 0;
        });
        CHECK(e.kernel.run() == mk::RunResult::kIdle);
        if (attr == 0) {
            CHECK_LOG(e.log, "W30", "W20", "W28");
        } else {
            CHECK_LOG(e.log, "W20", "W28", "W30");
        }
    }

    // One signal of 3 wakes everyone; they then run by priority.
    Env f;
    const auto sema = static_cast<mk::Uid>(f.kernel.create_sema("s", 0, 0, 10, 0));
    f.spawn("main", 0x40, [&](const mk::ThreadStart &) -> std::uint32_t {
        for (auto [name, prio] :
             {std::pair{"W30", 0x30u}, std::pair{"W20", 0x20u}, std::pair{"W28", 0x28u}}) {
            const std::string n = name;
            f.spawn(name, prio, [&f, n, sema](const mk::ThreadStart &) -> std::uint32_t {
                f.kernel.wait_sema(sema, 1, 0);
                f.note(n);
                return 0;
            });
        }
        CHECK_EQ(f.kernel.signal_sema(sema, 3), 0u);
        f.note("main");
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "W20", "W28", "W30", "main");

    // Timeouts: hardware rounding (<= 3 -> 24 us, <= 249 -> 245 us), the
    // remaining time is written back, 0 after a timeout.
    Env g;
    const auto s2 = static_cast<mk::Uid>(g.kernel.create_sema("s", 0, 0, 1, 0));
    const std::uint32_t t1 = g.scratch(1000);
    const std::uint32_t t2 = g.scratch(2);
    const std::uint32_t t3 = g.scratch(100);
    const std::uint32_t t4 = g.scratch(1000);
    g.spawn("timeouts", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(g.kernel.wait_sema(s2, 1, t1), err::kWaitTimeout);
        g.note_time("t1");
        CHECK_EQ(g.kernel.wait_sema(s2, 1, t2), err::kWaitTimeout);
        g.note_time("t2");
        CHECK_EQ(g.kernel.wait_sema(s2, 1, t3), err::kWaitTimeout);
        g.note_time("t3");
        CHECK_EQ(g.kernel.wait_sema(s2, 1, t4), 0u);
        g.note_time("t4");
        return 0;
    });
    g.spawn("signaller", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        g.kernel.delay_thread(1500); // Wakes at 1510: during t4's wait (1269..2269).
        CHECK_EQ(g.kernel.signal_sema(s2, 1), 0u);
        return 0;
    });
    CHECK(g.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(g.log, "t1@1000", "t2@1024", "t3@1269", "t4@1510");
    CHECK_EQ(g.mem.read_u32(t1), 0u);
    CHECK_EQ(g.mem.read_u32(t4), 1000u - (1510u - 1269u));

    // Errors.
    Env h;
    const auto s3 = static_cast<mk::Uid>(h.kernel.create_sema("s", 0, 1, 1, 0));
    CHECK_EQ(h.kernel.create_sema(nullptr, 0, 0, 1, 0), err::kError);
    CHECK_EQ(h.kernel.create_sema("bad", 0x200, 0, 1, 0), err::kIllegalAttr);
    CHECK_EQ(h.kernel.signal_sema(s3, 1), err::kSemaOvf);
    CHECK_EQ(h.kernel.signal_sema(0x999, 1), err::kUnknownSemid);
    CHECK_EQ(h.kernel.poll_sema(s3, 0), err::kIllegalCount);
    CHECK_EQ(h.kernel.poll_sema(s3, 2), err::kSemaZero);
    CHECK_EQ(h.kernel.poll_sema(s3, 1), 0u);
    CHECK_EQ(h.kernel.poll_sema(s3, 1), err::kSemaZero);
    h.spawn("errors", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(h.kernel.wait_sema(s3, 0, 0), err::kIllegalCount);
        CHECK_EQ(h.kernel.wait_sema(s3, 2, 0), err::kIllegalCount);
        CHECK_EQ(h.kernel.wait_sema(0x999, 1, 0), err::kUnknownSemid);
        return 0;
    });
    // Delete and cancel wake waiters with WAIT_DELETE / WAIT_CANCEL.
    const auto s4 = static_cast<mk::Uid>(h.kernel.create_sema("del", 0, 0, 5, 0));
    const auto s5 = static_cast<mk::Uid>(h.kernel.create_sema("can", 0, 2, 5, 0));
    const std::uint32_t num_wait = h.scratch(0xDEAD);
    h.spawn("deleted", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(h.kernel.wait_sema(s4, 1, 0), err::kWaitDelete);
        return 0;
    });
    h.spawn("cancelled", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(h.kernel.wait_sema(s5, 5, 0), err::kWaitCancel);
        return 0;
    });
    h.spawn("killer", 0x40, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(h.kernel.delete_sema(s4), 0u);
        CHECK_EQ(h.kernel.cancel_sema(s5, -1, num_wait), 0u);
        CHECK_EQ(h.kernel.delete_sema(s4), err::kUnknownSemid);
        return 0;
    });
    CHECK(h.kernel.run() == mk::RunResult::kIdle);
    CHECK_EQ(h.mem.read_u32(num_wait), 1u);
    // ReferSemaStatus: NativeSemaphore (56 bytes) with the count reset to init.
    const std::uint32_t info = h.next_scratch;
    h.next_scratch += 56;
    h.mem.write_u32(info, 56);
    CHECK_EQ(h.kernel.refer_sema_status(s5, info), 0u);
    CHECK_EQ(h.mem.read_u32(info), 56u);
    CHECK_EQ(h.mem.read_c_string(info + 4), std::string("can"));
    CHECK_EQ(h.mem.read_u32(info + 40), 2u); // initCount
    CHECK_EQ(h.mem.read_u32(info + 44), 2u); // currentCount
    CHECK_EQ(h.mem.read_u32(info + 48), 5u); // maxCount
    CHECK_EQ(h.mem.read_u32(info + 52), 0u); // numWaitThreads
}

void test_wait_quirks() {
    // KernelWaitHelpers.h: a thread that timed out keeps its wait until it
    // runs. If the object is deleted first it gets WAIT_DELETE, and a signal
    // still hands it the count (VerifyWait checks only waitType/waitID).
    for (int variant = 0; variant < 2; ++variant) {
        Env e;
        const auto sema = static_cast<mk::Uid>(e.kernel.create_sema("s", 0, 0, 1, 0));
        const std::uint32_t timeout = e.scratch(100);
        std::uint32_t result = 0;
        e.spawn("waiter", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
            result = e.kernel.wait_sema(sema, 1, timeout);
            e.note_time("waiter");
            return 0;
        });
        e.spawn("busy", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
            e.kernel.delay_thread(10); // Let the waiter block first (wakes at 210).
            for (int i = 0; i < 5; ++i) {
                e.clock.advance(100);
                e.kernel.preemption_point(); // The timeout fires at 245 us here.
            }
            if (variant == 0) {
                CHECK_EQ(e.kernel.delete_sema(sema), 0u);
            } else {
                CHECK_EQ(e.kernel.signal_sema(sema, 1), 0u);
            }
            e.note_time("busy");
            return 0;
        });
        CHECK(e.kernel.run() == mk::RunResult::kIdle);
        CHECK_LOG(e.log, "busy@710", "waiter@710");
        CHECK_EQ(result, variant == 0 ? err::kWaitDelete : 0u);
    }
}

void test_event_flags() {
    Env e;
    const auto flag =
        static_cast<mk::Uid>(e.kernel.create_event_flag("ev", 0x200, 0, 0)); // WAITMULTIPLE
    const std::uint32_t out_and = e.scratch(0);
    const std::uint32_t out_or = e.scratch(0);
    e.spawn("and", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.wait_event_flag(flag, 0x3, 0x00 /*AND*/, out_and, 0), 0u);
        e.note("and");
        return 0;
    });
    e.spawn("or", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.wait_event_flag(flag, 0x6, 0x01 | 0x20 /*OR|CLEAR*/, out_or, 0), 0u);
        e.note("or");
        return 0;
    });
    e.spawn("setter", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.set_event_flag(flag, 0x1), 0u); // Neither matches.
        e.note("set1");
        CHECK_EQ(e.kernel.set_event_flag(flag, 0x2),
                 0u); // Both: AND first, OR then clears 0x2|0x4.
        e.note("set2");
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "set1", "and", "or", "set2");
    CHECK_EQ(e.mem.read_u32(out_and), 0x3u);
    CHECK_EQ(e.mem.read_u32(out_or), 0x3u);
    const std::uint32_t poll_out = e.scratch(0);
    CHECK_EQ(e.kernel.poll_event_flag(flag, 0x1, 0, poll_out), 0u); // 0x1 is left.
    CHECK_EQ(e.kernel.poll_event_flag(flag, 0x2, 0, poll_out), err::kEvfCond);
    CHECK_EQ(e.mem.read_u32(poll_out), 0x1u);
    CHECK_EQ(e.kernel.poll_event_flag(flag, 0x1, 0x30, poll_out),
             err::kIllegalMode); // CLEAR|CLEARALL
    CHECK_EQ(e.kernel.poll_event_flag(flag, 0, 0, poll_out), err::kEvfIlpat);
    CHECK_EQ(e.kernel.clear_event_flag(flag, 0xFFFFFFFEu), 0u); // Pattern &= bits.
    CHECK_EQ(e.kernel.poll_event_flag(flag, 0x1, 0, poll_out), err::kEvfCond);

    // Without WAITMULTIPLE a second waiter gets EVF_MULTI; timeout writes the
    // current pattern to outBits; delete wakes with WAIT_DELETE.
    Env f;
    const auto single = static_cast<mk::Uid>(f.kernel.create_event_flag("one", 0, 0x10, 0));
    const std::uint32_t timeout = f.scratch(500);
    const std::uint32_t out = f.scratch(0xDEAD);
    f.spawn("first", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(f.kernel.wait_event_flag(single, 0x1, 0, out, timeout), err::kWaitTimeout);
        f.note_time("first");
        CHECK_EQ(f.kernel.wait_event_flag(single, 0x1, 0, 0, 0), err::kWaitDelete);
        f.note_time("first again");
        return 0;
    });
    f.spawn("second", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(f.kernel.wait_event_flag(single, 0x1, 0, 0, 0), err::kEvfMulti);
        CHECK_EQ(f.kernel.wait_event_flag(single, 0x1, 0x40, 0, 0), err::kIllegalMode);
        CHECK_EQ(f.kernel.wait_event_flag(single, 0, 0, 0, 0), err::kEvfIlpat);
        f.kernel.delay_thread(1000);
        CHECK_EQ(f.kernel.delete_event_flag(single), 0u);
        f.note_time("deleted");
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    // The deleter is worse, so the woken waiter runs inside delete_event_flag.
    CHECK_LOG(f.log, "first@500", "first again@1010", "deleted@1010");
    CHECK_EQ(f.mem.read_u32(out), 0x10u);
    CHECK_EQ(f.mem.read_u32(timeout), 0u);
    CHECK_EQ(f.kernel.create_event_flag("bad", 0x100, 0, 0), err::kIllegalAttr);
    CHECK_EQ(f.kernel.set_event_flag(single, 1), err::kUnknownEvfid);
}

void test_mutexes() {
    // No priority inheritance: a medium thread runs while the high one waits on
    // a mutex the low one holds (priority inversion, as on PSP and in PPSSPP).
    Env e;
    const auto mutex = static_cast<mk::Uid>(e.kernel.create_mutex("m", 0, 0, 0));
    e.spawn("low", 0x40, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.lock_mutex(mutex, 1, 0), 0u);
        e.note("low locked");
        e.spawn("high", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
            e.spawn("medium", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
                e.note("medium");
                return 0;
            });
            CHECK_EQ(e.kernel.lock_mutex(mutex, 1, 0), 0u);
            e.note("high locked");
            CHECK_EQ(e.kernel.unlock_mutex(mutex, 1), 0u);
            return 0;
        });
        e.note("low unlock");
        CHECK_EQ(e.kernel.unlock_mutex(mutex, 1), 0u);
        e.note("low end");
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "low locked", "medium", "low unlock", "high locked", "low end");

    // Errors, recursion and release on thread exit.
    Env f;
    const auto plain = static_cast<mk::Uid>(f.kernel.create_mutex("plain", 0, 0, 0));
    const auto recursive = static_cast<mk::Uid>(f.kernel.create_mutex("rec", 0x200, 0, 0));
    CHECK_EQ(f.kernel.create_mutex("bad", 0, 2, 0), err::kIllegalCount);
    CHECK_EQ(f.kernel.create_mutex("bad", 0x1000, 0, 0), err::kIllegalAttr);
    f.spawn("owner", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(f.kernel.lock_mutex(plain, 1, 0), 0u);
        CHECK_EQ(f.kernel.lock_mutex(plain, 1, 0), err::kMutexAlreadyLocked);
        CHECK_EQ(f.kernel.lock_mutex(plain, 2, 0), err::kIllegalCount);
        CHECK_EQ(f.kernel.lock_mutex(recursive, 2, 0), 0u);
        CHECK_EQ(f.kernel.lock_mutex(recursive, 1, 0), 0u);
        CHECK_EQ(f.kernel.unlock_mutex(recursive, 4), err::kMutexUnlockUnderflow);
        CHECK_EQ(f.kernel.unlock_mutex(recursive, 3), 0u);
        CHECK_EQ(f.kernel.unlock_mutex(recursive, 1), err::kMutexNotLocked);
        return 0; // Exits still holding `plain`.
    });
    f.spawn("next", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        // Released by __KernelMutexThreadEnd when the owner stopped.
        CHECK_EQ(f.kernel.try_lock_mutex(plain, 1), 0u);
        CHECK_EQ(f.kernel.try_lock_mutex(plain, 1), err::kMutexAlreadyLocked);
        f.note("next locked");
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "next locked");
    f.spawn("third", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(f.kernel.lock_mutex(recursive, 1, 0), 0u);
        f.spawn("fourth", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
            CHECK_EQ(f.kernel.try_lock_mutex(recursive, 1), err::kMutexTrylockFailed);
            return 0;
        });
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_EQ(f.kernel.unlock_mutex(0x999, 1), err::kMutexNoSuchMutex);
}

void test_sleep_wakeup_suspend() {
    Env e;
    mk::Uid sleeper = 0;
    sleeper = e.spawn("sleeper", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.sleep_thread(), 0u); // Woken by "waker".
        e.note("woken");
        CHECK_EQ(e.kernel.sleep_thread(), 0u); // A wakeup is pending: returns at once.
        e.note("no sleep");
        CHECK_EQ(e.kernel.sleep_thread(), err::kReleaseWait);
        e.note("released");
        return 0;
    });
    e.spawn("waker", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        e.kernel.delay_thread(10); // Let the sleeper go to sleep.
        CHECK_EQ(e.kernel.wakeup_thread(sleeper), 0u);
        CHECK_EQ(e.kernel.wakeup_thread(e.kernel.current_thread()), err::kIllegalThid);
        // Not sleeping any more (ready): the wakeups are counted.
        CHECK_EQ(e.kernel.wakeup_thread(sleeper), 0u);
        CHECK_EQ(e.kernel.wakeup_thread(sleeper), 0u);
        CHECK_EQ(e.kernel.cancel_wakeup_thread(sleeper), 2u);
        CHECK_EQ(e.kernel.wakeup_thread(sleeper), 0u);
        e.note("waker yields");
        e.kernel.delay_thread(100);
        CHECK_EQ(e.kernel.release_wait_thread(sleeper), 0u);
        CHECK_EQ(e.kernel.release_wait_thread(e.kernel.current_thread()), err::kIllegalThid);
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "waker yields", "woken", "no sleep", "released");
    CHECK_EQ(e.kernel.release_wait_thread(sleeper), err::kNotWait);

    // Suspend/resume a ready thread.
    Env f;
    mk::Uid victim = 0;
    f.spawn("ctl", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        victim = f.spawn("victim", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
            f.note("victim");
            return 0;
        });
        CHECK_EQ(f.kernel.suspend_thread(victim), 0u);
        CHECK_EQ(f.kernel.suspend_thread(victim), err::kSuspend);
        f.kernel.delay_thread(100); // The victim can't run.
        f.note("ctl resumes");
        CHECK_EQ(f.kernel.resume_thread(victim), 0u);
        CHECK_EQ(f.kernel.resume_thread(victim), err::kNotSuspend);
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "ctl resumes", "victim");
}

void test_thread_end_and_termination() {
    Env e;
    const std::uint32_t timeout = e.scratch(300);
    mk::Uid worker = 0;
    mk::Uid sleeper = 0;
    e.spawn("main", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        worker = e.spawn("worker", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
            e.kernel.delay_thread(1000);
            return 0x77;
        });
        CHECK_EQ(e.kernel.wait_thread_end(worker, timeout), err::kWaitTimeout);
        e.note_time("timeout");
        CHECK_EQ(e.kernel.wait_thread_end(worker, 0), 0x77u);
        e.note_time("ended");
        // Already dormant: returns the exit status at once.
        CHECK_EQ(e.kernel.wait_thread_end(worker, 0), 0x77u);
        CHECK_EQ(e.kernel.wait_thread_end(0, 0), err::kIllegalThid);
        sleeper = e.spawn("sleeper", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
            e.kernel.sleep_thread();
            e.note("never");
            return 0;
        });
        e.kernel.delay_thread(10);
        CHECK_EQ(e.kernel.terminate_thread(sleeper), 0u);
        CHECK_EQ(e.kernel.terminate_thread(sleeper), err::kDormant);
        CHECK_EQ(e.kernel.get_thread_exit_status(sleeper), err::kThreadTerminated);
        // Exit status < 0 becomes ILLEGAL_ARGUMENT.
        const mk::Uid exiter =
            e.spawn("exiter", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
                e.kernel.exit_thread(0xFFFFFFFFu);
                e.note("never");
                return 0;
            });
        CHECK_EQ(e.kernel.get_thread_exit_status(exiter), err::kIllegalArgument);
        // ExitDeleteThread removes the thread entirely.
        const mk::Uid gone = e.spawn("gone", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
            e.kernel.exit_delete_thread(5);
            e.note("never");
            return 0;
        });
        CHECK_EQ(e.kernel.get_thread_exit_status(gone), err::kUnknownThid);
        // Restarting a terminated thread runs it from its entry on a new fiber.
        CHECK_EQ(e.kernel.start_thread(sleeper, 0, 0), 0u);
        e.kernel.delay_thread(10);
        CHECK_EQ(e.kernel.terminate_delete_thread(sleeper), 0u);
        CHECK_EQ(e.kernel.delete_thread(e.kernel.current_thread()), err::kNotDormant);
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "timeout@300", "ended@1010");
    // Termination wakes a thread waiting for the end with THREAD_TERMINATED.
    Env f;
    mk::Uid target = 0;
    f.spawn("waiter", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        target = f.spawn("target", 0x40, [&](const mk::ThreadStart &) -> std::uint32_t {
            f.kernel.sleep_thread();
            return 0;
        });
        CHECK_EQ(f.kernel.wait_thread_end(target, 0), err::kThreadTerminated);
        f.note("waiter");
        return 0;
    });
    f.spawn("killer", 0x50, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(f.kernel.terminate_thread(target), 0u);
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "waiter");
}

void test_callbacks() {
    Env e;
    mk::Uid owner = 0;
    mk::Uid cb = 0;
    std::vector<std::uint32_t> seen;
    const std::uint32_t cb_entry =
        e.guest([&](std::span<const std::uint32_t> args) -> std::uint32_t {
            seen.assign(args.begin(), args.end());
            e.note_time("cb");
            return 0; // Keep the callback.
        });
    owner = e.spawn("owner", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        cb = static_cast<mk::Uid>(e.kernel.create_callback("cb", cb_entry, 0xC0FFEE));
        // DelayThreadCB: the callback runs on this thread in the middle, and the
        // delay keeps its original deadline.
        CHECK_EQ(e.kernel.delay_thread_cb(1000), 0u);
        e.note_time("delay done");
        CHECK_EQ(e.kernel.sleep_thread_cb(), 0u);
        e.note_time("sleep done");
        return 0;
    });
    e.spawn("notifier", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        e.kernel.delay_thread(300);
        CHECK_EQ(e.kernel.notify_callback(cb, 1), 0u);
        CHECK_EQ(e.kernel.notify_callback(cb, 2), 0u); // Count 2, last argument wins.
        CHECK_EQ(e.kernel.get_callback_count(cb), 2u);
        e.note_time("notified");
        e.kernel.delay_thread(990);      // Until after the delay ends (1010).
        e.kernel.notify_callback(cb, 3); // Runs during SleepThreadCB...
        e.note_time("notified again");
        e.kernel.delay_thread(100);
        CHECK_EQ(e.kernel.wakeup_thread(owner), 0u); // ...which keeps sleeping after it.
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "notified@310", "cb@310", "delay done@1010", "notified again@1310", "cb@1310",
              "sleep done@1520");
    CHECK_EQ(seen.size(), 3u);
    CHECK_EQ(seen[0], 1u);
    CHECK_EQ(seen[1], 3u);
    CHECK_EQ(seen[2], 0xC0FFEEu);

    // A callback that returns non-zero is deleted; a plain (non-CB) wait never
    // runs callbacks; sceKernelCheckCallback runs them on demand.
    Env f;
    mk::Uid once = 0;
    int calls = 0;
    const std::uint32_t once_entry = f.guest([&](std::span<const std::uint32_t>) -> std::uint32_t {
        ++calls;
        f.note("once");
        return 1;
    });
    f.spawn("t", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        once = static_cast<mk::Uid>(f.kernel.create_callback("once", once_entry, 0));
        CHECK_EQ(f.kernel.check_callback(), 0u);
        f.kernel.notify_callback(once, 0);
        f.kernel.delay_thread(100); // Not a CB wait.
        f.note("after plain delay");
        CHECK_EQ(f.kernel.check_callback(), 1u);
        f.note("after check");
        CHECK_EQ(f.kernel.get_callback_count(once), err::kUnknownCbid);
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "after plain delay", "once", "after check");
    CHECK_EQ(calls, 1);

    // WaitSemaCB: the callback signals the semaphore, so the wait completes as
    // soon as the callback returns (the end-of-callback unlock in PPSSPP).
    Env g;
    const auto sema = static_cast<mk::Uid>(g.kernel.create_sema("s", 0, 0, 1, 0));
    const std::uint32_t signal_entry =
        g.guest([&](std::span<const std::uint32_t>) -> std::uint32_t {
            g.note("cb signals");
            CHECK_EQ(g.kernel.signal_sema(sema, 1), 0u);
            return 0;
        });
    mk::Uid cb2 = 0;
    g.spawn("w", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        cb2 = static_cast<mk::Uid>(g.kernel.create_callback("sig", signal_entry, 0));
        CHECK_EQ(g.kernel.wait_sema_cb(sema, 1, 0), 0u);
        g.note_time("got sema");
        return 0;
    });
    g.kernel.schedule_event(500, [&] { g.kernel.notify_callback_from_host(cb2, 0); });
    CHECK(g.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(g.log, "cb signals", "got sema@500");

    // RegisterExitCallback keeps the id; unknown ids are accepted below SDK 3.95.
    CHECK_EQ(g.kernel.register_exit_callback(cb2), 0u);
    CHECK_EQ(g.kernel.exit_callback(), cb2);
    CHECK_EQ(g.kernel.register_exit_callback(0x999), 0u);
    CHECK_EQ(g.kernel.create_callback("bad", 0x10000000u, 0), err::kIllegalAddr);
    CHECK_EQ(g.kernel.create_callback(nullptr, 0, 0), err::kError);
}

void test_pools_and_partitions() {
    // FPL: blocks come out in a rotating order; an exhausted pool waits.
    Env e;
    const auto fpl = static_cast<mk::Uid>(e.kernel.create_fpl("fpl", 2, 0, 0x30, 2, 0));
    // First user allocation lands right after the 0x4000 "usersystemlib".
    const std::uint32_t base = kUserBase + 0x4000u;
    const std::uint32_t out1 = e.scratch(0);
    const std::uint32_t out2 = e.scratch(0);
    const std::uint32_t out3 = e.scratch(0);
    e.spawn("alloc", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.allocate_fpl(fpl, out1, 0), 0u);
        CHECK_EQ(e.kernel.allocate_fpl(fpl, out2, 0), 0u);
        CHECK_EQ(e.kernel.try_allocate_fpl(fpl, out3), err::kNoMemory);
        CHECK_EQ(e.kernel.allocate_fpl(fpl, out3, 0), 0u); // Waits for "freer".
        e.note("got third");
        return 0;
    });
    e.spawn("freer", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.free_fpl(fpl, e.mem.read_u32(out1)), 0u);
        e.note("freed");
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "got third", "freed");
    CHECK_EQ(e.mem.read_u32(out1), base);
    CHECK_EQ(e.mem.read_u32(out2), base + 0x30u);
    CHECK_EQ(e.mem.read_u32(out3), base);
    CHECK_EQ(e.kernel.free_fpl(fpl, base + 0x60u), err::kIllegalMemblock);
    CHECK_EQ(e.kernel.free_fpl(fpl, 0x0B000000u), err::kIllegalAddr);
    CHECK_EQ(e.kernel.create_fpl("bad", 7, 0, 0x10, 1, 0), err::kIllegalArgument);
    CHECK_EQ(e.kernel.create_fpl("bad", 1, 0, 0x10, 1, 0),
             err::kIllegalPerm); // Kernel partition from user mode.
    CHECK_EQ(e.kernel.create_fpl("bad", 2, 0, 0, 1, 0), err::kIllegalMemsize);

    // VPL: the header lives in guest memory; blocks come from the top of the pool.
    Env f;
    const auto vpl = static_cast<mk::Uid>(f.kernel.create_vpl("vpl", 2, 0, 0x1000, 0));
    const std::uint32_t block = kUserBase + 0x4000u;
    const std::uint32_t a1 = f.scratch(0);
    const std::uint32_t a2 = f.scratch(0);
    const std::uint32_t a3 = f.scratch(0);
    const std::uint32_t timeout = f.scratch(0);
    f.spawn("v", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(f.kernel.allocate_vpl(vpl, 0x100, a1, 0), 0u);
        CHECK_EQ(f.kernel.allocate_vpl(vpl, 0x100, a2, 0), 0u);
        CHECK_EQ(f.kernel.try_allocate_vpl(vpl, 0x1000, a3), err::kIllegalMemsize);
        CHECK_EQ(f.kernel.try_allocate_vpl(vpl, 0xE00, a3), err::kNoMemory);
        CHECK_EQ(f.kernel.allocate_vpl(vpl, 0xE00, a3, timeout),
                 err::kWaitTimeout); // *timeout == 0
        CHECK_EQ(f.kernel.free_vpl(vpl, f.mem.read_u32(a1)), 0u);
        CHECK_EQ(f.kernel.free_vpl(vpl, f.mem.read_u32(a1)),
                 err::kIllegalMemblock); // Already free.
        CHECK_EQ(f.kernel.allocate_vpl(vpl, 0x10, a3, 0), 0u);
        return 0;
    });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    // SceKernelVplHeader::Allocate: (0x1000 - 0x28) / 8 + 1 = 508 blocks free,
    // 33 taken from the end for each 0x100-byte request.
    CHECK_EQ(f.mem.read_u32(a1), block + 0x18u + (508u - 33u) * 8u + 8u);
    CHECK_EQ(f.mem.read_u32(a2), block + 0x18u + (508u - 66u) * 8u + 8u);
    // The freed 33-block hole is reused, split from its end (3 blocks for 0x10).
    CHECK_EQ(f.mem.read_u32(a3), block + 0x18u + (508u - 3u) * 8u + 8u);
    const std::uint32_t info = f.next_scratch;
    f.next_scratch += 52;
    f.mem.write_u32(info, 52);
    CHECK_EQ(f.kernel.refer_vpl_status(vpl, info), 0u);
    CHECK_EQ(f.mem.read_u32(info + 40), 0x1000u - 0x20u);                   // poolSize
    CHECK_EQ(f.mem.read_u32(info + 44), 0x1000u - 0x20u - (33u + 3u) * 8u); // freeSize
    CHECK_EQ(f.kernel.delete_vpl(vpl), 0u);
    CHECK_EQ(f.kernel.free_vpl(vpl, 0), err::kUnknownVplid);

    // SysMem: reproduce the layout F03 measured in PPSSPP after mgp_main
    // (docs/analysis/mgp_main.md §7): module at 0x08804000 (0x2F7800 bytes),
    // "UserSbrk" LowAligned 0x1000, then sceUtilityLoadAvModule's two blocks,
    // so the next module (zlibdec) starts at 0x08B20000.
    Env g;
    CHECK_EQ(g.kernel.user_memory().alloc_at(0x08804000u, 0x2F7800u, "mgp_main"), 0x08804000u);
    const auto sbrk =
        static_cast<mk::Uid>(g.kernel.alloc_partition_memory(2, "UserSbrk", 3, 0x10000, 0x1000));
    CHECK_EQ(g.kernel.get_block_head_addr(sbrk), 0x08AFC000u);
    std::uint32_t atrac = 0x8000;
    std::uint32_t mpeg = 0xC000;
    CHECK_EQ(g.kernel.user_memory().alloc(atrac, false, "av_atrac3plus"), 0x08B0C000u);
    CHECK_EQ(g.kernel.user_memory().alloc(mpeg, false, "av_mpegbase"), 0x08B14000u);
    std::uint32_t zlib = 0x4100;
    CHECK_EQ(g.kernel.user_memory().alloc(zlib, false, "zlibdec"), 0x08B20000u);
    // Partition API errors.
    CHECK_EQ(g.kernel.alloc_partition_memory(2, "x", 5, 0x100, 0), err::kIllegalMemblockType);
    CHECK_EQ(g.kernel.alloc_partition_memory(2, "x", 3, 0x100, 0x300), err::kIllegalAlignmentSize);
    CHECK_EQ(g.kernel.alloc_partition_memory(7, "x", 0, 0x100, 0), err::kIllegalArgument);
    CHECK_EQ(g.kernel.alloc_partition_memory(1, "x", 0, 0x100, 0), err::kIllegalPartition);
    CHECK_EQ(g.kernel.alloc_partition_memory(2, "x", 0, 0, 0), err::kMemblockAllocFailed);
    const auto high = static_cast<mk::Uid>(g.kernel.alloc_partition_memory(2, "hi", 1, 0x100, 0));
    CHECK_EQ(g.kernel.get_block_head_addr(high), kUserEnd - 0x100u);
    CHECK_EQ(g.kernel.free_partition_memory(high), 0u);
    CHECK_EQ(g.kernel.free_partition_memory(high), err::kUnknownUid);
    CHECK_EQ(g.kernel.get_block_head_addr(high), 0u);
    g.kernel.set_kernel_mode(true);
    const auto kblock = static_cast<mk::Uid>(g.kernel.alloc_partition_memory(1, "k", 0, 0x100, 0));
    CHECK_EQ(g.kernel.get_block_head_addr(kblock), 0x08000000u);
}

void test_dispatch_and_interrupts() {
    Env e;
    const auto sema = static_cast<mk::Uid>(e.kernel.create_sema("s", 0, 0, 1, 0));
    e.spawn("t", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(e.kernel.suspend_dispatch_thread(), 1u);
        CHECK_EQ(e.kernel.wait_sema(sema, 1, 0), err::kCanNotWait);
        CHECK_EQ(e.kernel.delay_thread(10), err::kCanNotWait);
        CHECK_EQ(e.kernel.resume_dispatch_thread(1), 0u);
        CHECK_EQ(e.kernel.cpu_suspend_intr(), 1u);
        CHECK_EQ(e.kernel.cpu_suspend_intr(), 0u);
        CHECK_EQ(e.kernel.sleep_thread(), err::kCanNotWait);
        CHECK_EQ(e.kernel.suspend_dispatch_thread(), err::kCpuDi);
        e.kernel.cpu_resume_intr(1);
        e.kernel.set_in_interrupt(true);
        CHECK_EQ(e.kernel.wait_event_flag(0x999, 1, 0, 0, 0), err::kIllegalContext);
        CHECK_EQ(e.kernel.get_thread_id(), err::kIllegalContext);
        e.kernel.set_in_interrupt(false);
        CHECK_EQ(e.kernel.get_thread_id(), static_cast<std::uint32_t>(e.kernel.current_thread()));
        e.note("done");
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "done");
    // Blocking calls from the host side (no PSP thread) are refused.
    CHECK_EQ(e.kernel.delay_thread(10), err::kIllegalContext);
}

void test_callback_waits() {
    // WaitEventFlagCB with a callback already pending: callbacks run first and
    // CLEAR applies only when the wait completes afterwards.
    Env e;
    const auto flag = static_cast<mk::Uid>(e.kernel.create_event_flag("f", 0, 0x1, 0));
    const std::uint32_t out = e.scratch(0);
    const std::uint32_t cb_entry =
        e.guest([&](std::span<const std::uint32_t> args) -> std::uint32_t {
            e.note("cb " + std::to_string(args[1]));
            return 0;
        });
    e.spawn("t", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        const auto cb = static_cast<mk::Uid>(e.kernel.create_callback("cb", cb_entry, 0));
        CHECK_EQ(e.kernel.notify_callback(cb, 7), 0u);
        CHECK_EQ(e.kernel.wait_event_flag_cb(flag, 0x1, 0x20, out, 0), 0u);
        e.note("after");
        return 0;
    });
    e.spawn("same priority", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        e.note("other");
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(e.log, "cb 7", "after", "other");
    CHECK_EQ(e.mem.read_u32(out), 0x1u);
    CHECK_EQ(e.kernel.poll_event_flag(flag, 0x1, 0, 0), err::kEvfCond);

    // WaitThreadEndCB: a callback in the middle, then the real end.
    Env f;
    mk::Uid cb2 = 0;
    const std::uint32_t note_entry = f.guest([&](std::span<const std::uint32_t>) -> std::uint32_t {
        f.note_time("cb");
        return 0;
    });
    f.spawn("t", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        cb2 = static_cast<mk::Uid>(f.kernel.create_callback("cb", note_entry, 0));
        const mk::Uid worker =
            f.spawn("worker", 0x30, [&](const mk::ThreadStart &) -> std::uint32_t {
                f.kernel.delay_thread(1000);
                return 9;
            });
        CHECK_EQ(f.kernel.wait_thread_end_cb(worker, 0), 9u);
        f.note_time("end");
        return 0;
    });
    f.kernel.schedule_event(500, [&] { f.kernel.notify_callback_from_host(cb2, 0); });
    CHECK(f.kernel.run() == mk::RunResult::kIdle);
    CHECK_LOG(f.log, "cb@500", "end@1010");

    // WaitSemaCB whose deadline passes while the callback runs: TIMEOUT once it
    // returns (HLEKernel::WaitEndCallback); deleting the object in the
    // callback gives WAIT_DELETE instead.
    for (int variant = 0; variant < 2; ++variant) {
        Env g;
        const auto sema = static_cast<mk::Uid>(g.kernel.create_sema("s", 0, 0, 1, 0));
        const std::uint32_t timeout = g.scratch(1000);
        mk::Uid cb3 = 0;
        const std::uint32_t slow_entry =
            g.guest([&](std::span<const std::uint32_t>) -> std::uint32_t {
                g.clock.advance(800); // Guest work inside the callback.
                if (variant == 1) CHECK_EQ(g.kernel.delete_sema(sema), 0u);
                return 0;
            });
        std::uint32_t result = 0;
        g.spawn("t", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
            cb3 = static_cast<mk::Uid>(g.kernel.create_callback("slow", slow_entry, 0));
            result = g.kernel.wait_sema_cb(sema, 1, timeout);
            g.note_time("t");
            return 0;
        });
        g.kernel.schedule_event(500, [&] { g.kernel.notify_callback_from_host(cb3, 0); });
        CHECK(g.kernel.run() == mk::RunResult::kIdle);
        CHECK_LOG(g.log, "t@1300");
        CHECK_EQ(result, variant == 0 ? err::kWaitTimeout : err::kWaitDelete);
        CHECK_EQ(g.mem.read_u32(timeout), 0u);
    }

    // Small calls the game imports.
    Env h;
    h.spawn("t", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
        CHECK_EQ(h.kernel.change_current_thread_attr(0, mk::thread_attr::kVfpu), 0u);
        mk::ThreadInfo info;
        CHECK(h.kernel.thread_info(h.kernel.current_thread(), info));
        CHECK((info.attr & mk::thread_attr::kVfpu) != 0);
        CHECK_EQ(h.kernel.change_current_thread_attr(mk::thread_attr::kKernel, 0),
                 err::kIllegalAttr);
        h.clock.advance(1234);
        CHECK_EQ(h.kernel.get_system_time_wide(), 1234u);
        return 0;
    });
    CHECK(h.kernel.run() == mk::RunResult::kIdle);
}

void test_refer_thread_status() {
    Env e;
    e.kernel.set_compiled_sdk_version(0x02080010u); // mgp_main's SDK: the 108-byte layout.
    const std::uint32_t info = e.next_scratch;
    e.next_scratch += 112;
    const mk::Uid uid =
        e.create("referred", 0x24, [](const mk::ThreadStart &) -> std::uint32_t { return 0; });
    e.mem.write_u32(info, 108);
    e.mem.write_u32(info + 104, 0xDEADBEEFu);
    CHECK_EQ(e.kernel.refer_thread_status(uid, info), 0u);
    CHECK_EQ(e.mem.read_u32(info), 108u);
    CHECK_EQ(e.mem.read_c_string(info + 4), std::string("referred"));
    CHECK_EQ(e.mem.read_u32(info + 40), ts::kDormant);
    CHECK_EQ(e.mem.read_u32(info + 64), 0x24u);
    CHECK_EQ(e.mem.read_u32(info + 104), 0u); // Zeroed beyond 104.
    e.mem.write_u32(info, 112);
    CHECK_EQ(e.kernel.refer_thread_status(uid, info), err::kIllegalSize);
    CHECK_EQ(e.kernel.refer_thread_status(0x999, info), err::kUnknownThid);
}

// Many threads, semaphores, event flags, pools and delays, driven by a seeded
// RNG. Checks conservation (every item produced is consumed) and that two runs
// produce the same trace: the scheduler is deterministic on a virtual clock.
std::string stress_run(std::uint32_t seed) {
    Env e(128u * 1024u);
    std::mt19937 rng(seed);
    const auto items = static_cast<mk::Uid>(e.kernel.create_sema("items", 0, 0, 1000, 0));
    const auto slots = static_cast<mk::Uid>(e.kernel.create_sema("slots", 0x100, 16, 16, 0));
    const auto done = static_cast<mk::Uid>(e.kernel.create_event_flag("done", 0x200, 0, 0));
    const auto fpl = static_cast<mk::Uid>(e.kernel.create_fpl("pool", 2, 0x100, 0x40, 8, 0));
    constexpr int kProducers = 12;
    constexpr int kConsumers = 12;
    constexpr int kPerProducer = 40;
    int produced = 0;
    int consumed = 0;
    std::string trace;
    for (int p = 0; p < kProducers; ++p) {
        const auto prio = static_cast<std::uint32_t>(0x18 + rng() % 0x20);
        const auto pause = static_cast<std::uint32_t>(1 + rng() % 300);
        const std::string name = "P" + std::to_string(p);
        e.spawn(name.c_str(), prio, [&, pause, p](const mk::ThreadStart &) -> std::uint32_t {
            const std::uint32_t out = 0x08000800u + static_cast<std::uint32_t>(p) * 4u;
            for (int i = 0; i < kPerProducer; ++i) {
                CHECK_EQ(e.kernel.wait_sema(slots, 1, 0), 0u);
                CHECK_EQ(e.kernel.allocate_fpl(fpl, out, 0), 0u);
                CHECK_EQ(e.kernel.free_fpl(fpl, e.mem.read_u32(out)), 0u);
                ++produced;
                trace += "p" + std::to_string(p) + "@" + std::to_string(e.clock.now_us()) + ";";
                CHECK_EQ(e.kernel.signal_sema(items, 1), 0u);
                if ((i % 7) == 0) e.kernel.delay_thread(pause);
            }
            e.kernel.set_event_flag(done, 1u << p);
            return 0;
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        const auto prio = static_cast<std::uint32_t>(0x18 + rng() % 0x20);
        const auto pause = static_cast<std::uint32_t>(1 + rng() % 500);
        const std::string name = "C" + std::to_string(c);
        e.spawn(name.c_str(), prio, [&, pause, c](const mk::ThreadStart &) -> std::uint32_t {
            const std::uint32_t timeout = 0x08000C00u + static_cast<std::uint32_t>(c) * 4u;
            for (;;) {
                e.mem.write_u32(timeout, 2000);
                const std::uint32_t r = e.kernel.wait_sema(items, 1, timeout);
                if (r == err::kWaitTimeout) break;
                CHECK_EQ(r, 0u);
                ++consumed;
                trace += "c" + std::to_string(c) + "@" + std::to_string(e.clock.now_us()) + ";";
                CHECK_EQ(e.kernel.signal_sema(slots, 1), 0u);
                if ((consumed % 5) == 0) e.kernel.delay_thread(pause);
            }
            return 0;
        });
    }
    // Churn: threads created, run and deleted over and over (fiber reuse).
    e.spawn("churn", 0x50, [&](const mk::ThreadStart &) -> std::uint32_t {
        for (int i = 0; i < 64; ++i) {
            const mk::Uid t = e.spawn("tmp", 0x10, [&](const mk::ThreadStart &) -> std::uint32_t {
                e.kernel.delay_thread(50);
                return 1;
            });
            CHECK_EQ(e.kernel.wait_thread_end(t, 0), 1u);
            CHECK_EQ(e.kernel.delete_thread(t), 0u);
        }
        const std::uint32_t bits = e.scratch(0);
        CHECK_EQ(e.kernel.wait_event_flag(done, (1u << kProducers) - 1u, 0, bits, 0), 0u);
        trace += "all done@" + std::to_string(e.clock.now_us()) + ";";
        return 0;
    });
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_EQ(produced, kProducers * kPerProducer);
    CHECK_EQ(consumed, kProducers * kPerProducer);
    for (mk::Uid uid : e.kernel.thread_list()) CHECK_EQ(e.status(uid), ts::kDormant);
    return trace;
}

void test_stress_determinism() {
    const std::string a = stress_run(1234);
    const std::string b = stress_run(1234);
    const std::string c = stress_run(99);
    CHECK(!a.empty());
    CHECK(a == b);
    CHECK(a != c);
}

void test_big_stacks() {
    // The default 64 MiB per fiber is only reserved, not committed: 32 threads
    // that touch 1 MiB of host stack each.
    Env e(64u * 1024u * 1024u);
    int finished = 0;
    for (int i = 0; i < 32; ++i) {
        e.spawn("deep", 0x20, [&](const mk::ThreadStart &) -> std::uint32_t {
            volatile std::uint8_t buffer[1024 * 1024];
            for (std::size_t j = 0; j < sizeof(buffer); j += 4096)
                buffer[j] = static_cast<std::uint8_t>(j);
            e.kernel.delay_thread(100);
            ++finished;
            return buffer[4096];
        });
    }
    CHECK(e.kernel.run() == mk::RunResult::kIdle);
    CHECK_EQ(finished, 32);
}

} // namespace

int main() {
    test_thread_lifecycle();
    test_thread_stack_layout();
    test_priority_and_preemption();
    test_delays();
    test_semaphores();
    test_wait_quirks();
    test_event_flags();
    test_mutexes();
    test_sleep_wakeup_suspend();
    test_thread_end_and_termination();
    test_callbacks();
    test_pools_and_partitions();
    test_dispatch_and_interrupts();
    test_callback_waits();
    test_refer_thread_status();
    test_stress_determinism();
    test_big_stacks();
    std::printf("mpo_kernel_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
