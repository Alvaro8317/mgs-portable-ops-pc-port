// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_boot (F08b): boots the user's mgp_main on the HLE kernel, recompiled
// ahead of time, and reports how far it gets.
//
//   mpo_boot --elf game/elf/mgp_main.elf --disc game/disc [--tables <vfpu>]
//            [--nid-table analysis/nids/ppsspp-v1.20.4.tsv]
//            [--trace <file>] [--trace-limit N] [--until-us N]
//            [--unimplemented stop|continue] [--interpreter 0|1]
//            [--fiber-stack-mib N] [--preempt-interval N] [--us-per-preempt N]
//
// It measures the start-up (registering the dispatch table: time and memory),
// runs PSP threads until --until-us of virtual time (default 5 s), a runtime
// stop or until nothing can run, and prints the threads, the modules, the HLE
// histogram, the pending (unimplemented) imports by calls and by static call
// sites, and the interpreter statistics. Exit code 0 when the boot module
// started and its threads ran without a guest fault.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// windows.h first.
#include <psapi.h>
#endif

#include "hle/system.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "recomp/vfpu_executor.hpp"
#include "vfpu/vfpu.hpp"

namespace psprecomp {
#if MPO_CORPUS_SOUND2
void register_sound2_functions(Runtime &runtime);
#endif
#if MPO_CORPUS_KJFS
void register_kjfs_functions(Runtime &runtime);
#endif
#if MPO_CORPUS_ZLIBDEC
void register_zlibdec_functions(Runtime &runtime);
#endif
} // namespace psprecomp

namespace {

struct MemoryUse {
    std::uint64_t rss_kib = 0;
    std::uint64_t peak_kib = 0;
    bool known = false;
};

MemoryUse memory_use() {
    MemoryUse use;
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0)
            use.rss_kib = std::strtoull(line.c_str() + 6, nullptr, 10);
        if (line.rfind("VmHWM:", 0) == 0)
            use.peak_kib = std::strtoull(line.c_str() + 6, nullptr, 10);
    }
    use.known = use.rss_kib != 0u;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        use.rss_kib = counters.WorkingSetSize / 1024u;
        use.peak_kib = counters.PeakWorkingSetSize / 1024u;
        use.known = true;
    }
#endif
    return use;
}

void print_memory(const char *when, const MemoryUse &use) {
    if (use.known)
        std::printf("memory %s: RSS %.1f MiB, peak %.1f MiB\n", when,
                    static_cast<double>(use.rss_kib) / 1024.0,
                    static_cast<double>(use.peak_kib) / 1024.0);
    else
        std::printf("memory %s: not measured on this platform\n", when);
}

int fail(const std::string &message) {
    std::fprintf(stderr, "mpo_boot: %s\n", message.c_str());
    return 2;
}

const char *status_name(std::uint32_t status) {
    namespace ts = mpo::kernel::thread_status;
    switch (status) {
    case ts::kRunning:
        return "RUNNING";
    case ts::kReady:
        return "READY";
    case ts::kWait:
        return "WAIT";
    case ts::kSuspend:
        return "SUSPEND";
    case ts::kWait | ts::kSuspend:
        return "WAIT+SUSPEND";
    case ts::kDormant:
        return "DORMANT";
    case ts::kDead:
        return "DEAD";
    default:
        return "?";
    }
}

const char *wait_name(mpo::kernel::WaitType type) {
    using W = mpo::kernel::WaitType;
    switch (type) {
    case W::kNone:
        return "-";
    case W::kSleep:
        return "sleep";
    case W::kDelay:
        return "delay";
    case W::kSema:
        return "sema";
    case W::kEventFlag:
        return "eventflag";
    case W::kVpl:
        return "vpl";
    case W::kFpl:
        return "fpl";
    case W::kThreadEnd:
        return "thread-end";
    case W::kMutex:
        return "mutex";
    case W::kModule:
        return "module";
    case W::kHleDelay:
        return "hle-delay";
    case W::kVmem:
        return "vmem";
    default:
        return "other";
    }
}

// Static call sites per import: `jal <stub>` words in each module's image.
std::map<std::pair<std::string, std::uint32_t>, std::uint32_t>
static_call_sites(mpo::hle::System &system) {
    std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> sites;
    psprecomp::GuestMemory &memory = system.runtime().memory();
    for (const mpo::hle::Module *module : system.modules().modules()) {
        if (module->fake) continue;
        std::map<std::uint32_t, const psprecomp::PspImport *> stubs;
        for (const psprecomp::PspImport &import : module->imports)
            stubs.emplace(import.stub_address, &import);
        for (std::uint32_t pc = module->base; pc + 4u <= module->base + module->size; pc += 4u) {
            const std::uint32_t word = memory.load32(pc);
            if ((word >> 26u) != 3u) continue; // jal
            const std::uint32_t target = (pc & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2u);
            const auto found = stubs.find(target);
            if (found != stubs.end()) ++sites[{found->second->library, found->second->nid}];
        }
    }
    return sites;
}

} // namespace

int main(int argc, char **argv) {
    std::string elf_path;
    std::string disc;
    std::string tables = MPO_DEFAULT_VFPU_TABLES;
    std::string nid_table;
    std::string trace_path;
    std::uint64_t trace_limit = 200000u;
    std::uint64_t until_us = 5000000u;
    mpo::hle::SystemConfig config;
    bool interpreter = true;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (option == "--elf") {
            elf_path = value;
        } else if (option == "--disc") {
            disc = value;
        } else if (option == "--tables") {
            tables = value;
        } else if (option == "--nid-table") {
            nid_table = value;
        } else if (option == "--trace") {
            trace_path = value;
        } else if (option == "--trace-limit") {
            trace_limit = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--until-us") {
            until_us = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--unimplemented") {
            if (value != "stop" && value != "continue")
                return fail("--unimplemented takes stop or continue");
            config.unimplemented = value == "stop" ? mpo::hle::UnimplementedPolicy::kStop
                                                   : mpo::hle::UnimplementedPolicy::kReturnZero;
        } else if (option == "--interpreter") {
            interpreter = value != "0";
        } else if (option == "--fiber-stack-mib") {
            config.fiber_stack_size =
                static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 0)) << 20u;
        } else if (option == "--preempt-interval") {
            config.preemption_interval = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--us-per-preempt") {
            config.us_per_preemption = std::strtoull(value.c_str(), nullptr, 0);
        } else {
            return fail("unknown option " + option);
        }
    }
    if (elf_path.empty()) return fail("usage: see boot_main.cpp (--elf is required)");
    config.disc_root = disc;

    std::string error;
    if (!mpo::vfpu::load_tables(tables, &error)) return fail(error);
    mpo::recomp::install_vfpu_executor();
    psprecomp::set_interpreter_fallback_enabled(interpreter);

    std::ifstream elf_file(elf_path, std::ios::binary);
    if (!elf_file) return fail("cannot read " + elf_path);
    const std::vector<std::uint8_t> elf((std::istreambuf_iterator<char>(elf_file)),
                                        std::istreambuf_iterator<char>());

    // Start-up: the dispatch table of every corpus.
    print_memory("before the runtime", memory_use());
    const auto t0 = std::chrono::steady_clock::now();
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    psprecomp::register_generated_functions(runtime);
    const std::size_t main_functions = runtime.function_count();
    const char *corpora = "mgp_main";
#if MPO_CORPUS_ZLIBDEC
    psprecomp::register_zlibdec_functions(runtime);
#endif
#if MPO_CORPUS_SOUND2
    psprecomp::register_sound2_functions(runtime);
#endif
#if MPO_CORPUS_KJFS
    psprecomp::register_kjfs_functions(runtime);
#endif
#if MPO_CORPUS_ZLIBDEC && MPO_CORPUS_SOUND2 && MPO_CORPUS_KJFS
    corpora = "mgp_main, zlibdec, sound2, kjfs";
#elif MPO_CORPUS_ZLIBDEC || MPO_CORPUS_SOUND2 || MPO_CORPUS_KJFS
    corpora = "mgp_main and some of zlibdec, sound2, kjfs";
#endif
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("dispatch table: %zu functions (mgp_main %zu) from %s in %.1f ms\n",
                runtime.function_count(), main_functions, corpora,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    print_memory("after registering", memory_use());

    std::FILE *trace = nullptr;
    if (!trace_path.empty()) {
#if defined(_MSC_VER)
        if (fopen_s(&trace, trace_path.c_str(), "w") != 0) trace = nullptr;
#else
        trace = std::fopen(trace_path.c_str(), "w");
#endif
        if (trace == nullptr) return fail("cannot write " + trace_path);
    }

    int exit_code = 1;
    {
        mpo::hle::System system(runtime, config);
        if (!nid_table.empty() && !system.hle().load_nid_names(nid_table, &error))
            return fail(error);
        system.hle().set_trace(trace, trace_limit);
        const auto t2 = std::chrono::steady_clock::now();
        if (!system.boot(elf, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &error)) return fail(error);
        const mpo::kernel::RunResult result = system.run(until_us);
        const auto t3 = std::chrono::steady_clock::now();
        const char *result_name = result == mpo::kernel::RunResult::kIdle
                                      ? "idle (every thread waits, no timer)"
                                  : result == mpo::kernel::RunResult::kStopped ? "stopped"
                                                                               : "deadline";
        std::printf("\nrun: %s at %llu us of virtual time, %.1f ms of host time\n", result_name,
                    static_cast<unsigned long long>(system.kernel().now_us()),
                    std::chrono::duration<double, std::milli>(t3 - t2).count());
        if (runtime.stopped()) std::printf("runtime stop: %s\n", runtime.stop_reason().c_str());
        std::printf("thread switches: %llu, guest callbacks: %llu, HLE calls: %llu\n",
                    static_cast<unsigned long long>(system.cpu().switches()),
                    static_cast<unsigned long long>(system.cpu().guest_calls()),
                    static_cast<unsigned long long>(system.hle().total_calls()));
        print_memory("after the run", memory_use());

        std::printf("\nmodules:\n");
        for (const mpo::hle::Module *module : system.modules().modules()) {
            std::printf("  %5d %-24s %08X..%08X %s gp=%08X module_start=%08X %s\n", module->uid,
                        module->name.c_str(), module->base, module->base + module->size,
                        module->fake ? "fake" : "elf ", module->gp, module->start_func,
                        module->path.c_str());
        }
        std::printf("\nthreads:\n");
        for (const mpo::kernel::Uid uid : system.kernel().thread_list()) {
            mpo::kernel::ThreadInfo info;
            if (!system.kernel().thread_info(uid, info)) continue;
            std::printf(
                "  %5d %-28s %-8s wait=%-10s prio=%02X entry=%08X stack=%08X+%X module=%d\n", uid,
                info.name.c_str(), status_name(info.status), wait_name(info.wait_type),
                static_cast<unsigned>(info.current_priority), info.entry, info.initial_stack,
                info.stack_size, info.module_id);
        }
        std::printf("\n");
        system.hle().report_histogram(stdout);

        // Pending imports: unimplemented, by calls in this run, then by static
        // call sites in the loaded modules.
        const auto sites = static_call_sites(system);
        std::vector<const mpo::hle::HleTable::Entry *> pending;
        for (const auto *entry : system.hle().histogram())
            if (entry->kind == mpo::hle::HleKind::kUnimplemented) pending.push_back(entry);
        std::stable_sort(pending.begin(), pending.end(), [&sites](const auto *a, const auto *b) {
            if (a->calls != b->calls) return a->calls > b->calls;
            const auto sa = sites.find({a->library, a->nid});
            const auto sb = sites.find({b->library, b->nid});
            return (sa != sites.end() ? sa->second : 0u) > (sb != sites.end() ? sb->second : 0u);
        });
        std::printf("\npending imports (unimplemented): %zu\n", pending.size());
        std::printf("  %8s %6s  %s\n", "calls", "sites", "import");
        for (const auto *entry : pending) {
            const auto site = sites.find({entry->library, entry->nid});
            std::printf("  %8llu %6u  %s::%s%s\n", static_cast<unsigned long long>(entry->calls),
                        site != sites.end() ? site->second : 0u, entry->library.c_str(),
                        entry->name.c_str(), entry->calls != 0u ? "" : "");
        }
        const psprecomp::InterpreterStats stats = psprecomp::interpreter_stats();
        std::printf("\ninterpreter: %llu instructions in %llu entries\n",
                    static_cast<unsigned long long>(stats.instructions),
                    static_cast<unsigned long long>(stats.entries));
        const bool faulted =
            runtime.stopped() &&
            runtime.stop_reason().find("unimplemented HLE import") == std::string::npos &&
            runtime.stop_reason().find("sceKernelExitGame") == std::string::npos;
        exit_code = faulted ? 1 : 0;
    }
    if (trace != nullptr) std::fclose(trace);
    print_memory("at exit", memory_use());
    return exit_code;
}
