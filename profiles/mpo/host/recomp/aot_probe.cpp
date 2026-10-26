// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_aot_probe (F07b): loads the user's mgp_main next to its recompiled
// corpus and checks, without any HLE, that
//
//   1. every instruction word of the code ranges in the hints file has a
//      dispatcher entry (the addr -> fn table), except the import stubs,
//      which have their import wrapper;
//   2. the three functions mgp_main patches to `jr ra` at start-up return at
//      once (config/hints/mgp_main.txt, `patch`);
//   3. module_start runs as recompiled code until it reaches its first import,
//      which has no HLE yet (that is F08b): the stop reason names it.
//      With --stub-imports 1, every import instead returns 0 at once (not a
//      kernel: only to see how far module_start runs) and the probe lists
//      the imports it called, in order.
//
//   mpo_aot_probe --elf game/elf/mgp_main.elf --hints <out>/mgp_main.hints.txt
//                 --tables <dir with PPSSPP's vfpu/*.dat> [--max-dispatches N]
//                 [--stub-imports 1]
//
// Exit code 0 when 1 and 2 hold and step 3 stops on a missing import (or,
// with --stub-imports, when module_start returns).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "recomp/vfpu_executor.hpp"
#include "vfpu/vfpu.hpp"

namespace {

constexpr std::uint32_t kLoadBase = 0x08804000u;
// Where the probe parks control: never guest code.
constexpr std::uint32_t kReturnSentinel = 0x08800000u;
// Functions mgp_main turns into `jr ra; nop` at start-up [V, mgp_main.md §6.6].
constexpr std::uint32_t kPatchedToReturn[] = {0x089CFE80u, 0x089CFF78u, 0x089D002Cu};
constexpr std::uint32_t kStackTop = 0x09F00000u;
constexpr std::uint32_t kArgs = 0x09F00100u;

struct Range {
    std::uint32_t start;
    std::uint32_t end;
};

std::vector<Range> code_ranges(const std::string &hints_path) {
    std::vector<Range> ranges;
    std::ifstream in(hints_path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string directive;
        std::string start;
        std::string end;
        if (fields >> directive >> start >> end && directive == "code") {
            ranges.push_back({static_cast<std::uint32_t>(std::stoul(start, nullptr, 16)),
                              static_cast<std::uint32_t>(std::stoul(end, nullptr, 16))});
        }
    }
    return ranges;
}

void stop_at_sentinel(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &) {
    runtime.stop("module_start returned");
}

int fail(const std::string &message) {
    std::fprintf(stderr, "mpo_aot_probe: %s\n", message.c_str());
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    std::string elf_path;
    std::string hints_path;
    std::string tables;
    std::uint64_t max_dispatches = 1000000u;
    bool stub_imports = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--elf") {
            elf_path = argv[i + 1];
        } else if (arg == "--hints") {
            hints_path = argv[i + 1];
        } else if (arg == "--tables") {
            tables = argv[i + 1];
        } else if (arg == "--stub-imports") {
            stub_imports = std::string(argv[i + 1]) == "1";
        } else if (arg == "--max-dispatches") {
            max_dispatches = std::strtoull(argv[i + 1], nullptr, 0);
        } else {
            return fail("unknown option " + arg);
        }
    }
    if (elf_path.empty() || hints_path.empty() || tables.empty())
        return fail("usage: see aot_probe.cpp");

    std::string error;
    if (!mpo::vfpu::load_tables(tables, &error)) return fail(error);
    mpo::recomp::install_vfpu_executor();
    // Everything must be recompiled: the interpreter would hide a gap.
    psprecomp::set_interpreter_fallback_enabled(false);

    try {
        const auto elf = psprecomp::Elf32Image::from_file(elf_path);
        psprecomp::Runtime runtime(elf.required_ram_size(kLoadBase));
        (void)elf.load_and_relocate(runtime.memory(), kLoadBase);
        const auto module = elf.find_module_info(runtime.memory(), kLoadBase);
        if (!module) return fail("no sceModuleInfo in " + elf_path);
        std::set<std::uint32_t> stub_words;
        const auto imports = elf.scan_imports(runtime.memory(), *module);
        for (const auto &import : imports) {
            stub_words.insert(import.stub_address);
            stub_words.insert(import.stub_address + 4u);
        }
        psprecomp::register_generated_functions(runtime);
        int failures = 0;

        // 1. The dispatch table covers every code word.
        std::uint64_t words = 0;
        std::uint64_t registered = 0;
        std::uint64_t stubs_registered = 0;
        std::uint64_t missing = 0;
        for (const Range &range : code_ranges(hints_path)) {
            for (std::uint32_t pc = range.start; pc < range.end; pc += 4u) {
                if (stub_words.contains(pc)) {
                    stubs_registered += runtime.has_function(pc) ? 1u : 0u;
                    continue;
                }
                ++words;
                if (runtime.has_function(pc)) {
                    ++registered;
                } else if (missing++ < 5) {
                    std::printf("  no dispatcher entry at %s\n", psprecomp::hex32(pc).c_str());
                }
            }
        }
        std::printf("dispatch: %zu functions registered; %llu of %llu code words have an entry, "
                    "%llu of %zu import stubs have their wrapper\n",
                    runtime.function_count(), static_cast<unsigned long long>(registered),
                    static_cast<unsigned long long>(words),
                    static_cast<unsigned long long>(stubs_registered), imports.size());
        if (words == 0 || registered != words || stubs_registered != imports.size()) ++failures;

        // 2. The start-up patches are in the corpus.
        for (const std::uint32_t address : kPatchedToReturn) {
            psprecomp::AllegrexContext ctx{};
            ctx.gpr[31] = kReturnSentinel;
            ctx.gpr[29] = kStackTop;
            const bool ran = runtime.invoke_isolated_aot(address, ctx);
            const bool returned = ran && ctx.pc == kReturnSentinel;
            std::printf("patched %s: %s\n", psprecomp::hex32(address).c_str(),
                        returned ? "returns at once" : "DOES NOT RETURN");
            if (!returned) ++failures;
        }

        // 3. module_start until the first import without HLE.
        runtime.register_function(kReturnSentinel, &stop_at_sentinel, "probe_return_sentinel");
        const char kArgv0[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        for (std::uint32_t k = 0; k < sizeof(kArgv0); ++k)
            runtime.memory().store8(kArgs + k, static_cast<std::uint8_t>(kArgv0[k]));
        psprecomp::AllegrexContext &cpu = runtime.cpu();
        cpu = psprecomp::AllegrexContext{};
        cpu.gpr[4] = static_cast<std::uint32_t>(sizeof(kArgv0));
        cpu.gpr[5] = kArgs;
        cpu.gpr[28] = module->gp;
        cpu.gpr[29] = kStackTop;
        cpu.gpr[31] = kReturnSentinel;
        std::vector<std::string> import_calls;
        std::vector<std::uint32_t> thread_entries;
        std::uint32_t thread_args[2] = {0u, 0u}; // sceKernelStartThread's arglen, argp
        if (stub_imports) {
            for (const auto &import : imports) {
                const std::string name = import.library + "::" +
                                         runtime.nids()
                                             .resolve(import.library, import.nid)
                                             .value_or(psprecomp::hex32(import.nid));
                runtime.register_hle(import.library, import.nid,
                                     [&import_calls, &thread_entries, &thread_args,
                                      name](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                                         import_calls.push_back(name);
                                         if (name == "ThreadManForUser::sceKernelCreateThread")
                                             thread_entries.push_back(ctx.gpr[5]);
                                         if (name == "ThreadManForUser::sceKernelStartThread") {
                                             thread_args[0] = ctx.gpr[5];
                                             thread_args[1] = ctx.gpr[6];
                                         }
                                         ctx.gpr[2] = 0u;
                                     });
            }
        }
        const std::uint32_t entry = elf.runtime_entry(kLoadBase);
        runtime.run(entry, max_dispatches);
        std::printf("module_start %s: %s (pc %s, %llu dispatches)\n",
                    psprecomp::hex32(entry).c_str(), runtime.stop_reason().c_str(),
                    psprecomp::hex32(cpu.pc).c_str(),
                    static_cast<unsigned long long>(runtime.dispatch_work_count()));
        const auto stats = psprecomp::interpreter_stats();
        std::printf("interpreter: %llu instructions\n",
                    static_cast<unsigned long long>(stats.instructions));
        if (stub_imports) {
            std::printf("imports called: %zu\n", import_calls.size());
            for (std::size_t k = 0; k < import_calls.size() && k < 40u; ++k)
                std::printf("  %s\n", import_calls[k].c_str());
            if (runtime.stop_reason() != "module_start returned") ++failures;
            // The threads module_start created, one after another, on their
            // own stack, still with every import answering 0: how far does
            // the recompiled code get before it needs a real kernel?
            for (const std::uint32_t thread : thread_entries) {
                const std::size_t before = import_calls.size();
                cpu = psprecomp::AllegrexContext{};
                cpu.gpr[28] = module->gp;
                cpu.gpr[29] = kStackTop - 0x10000u;
                cpu.gpr[4] = thread_args[0];
                cpu.gpr[5] = thread_args[1];
                cpu.gpr[31] = kReturnSentinel;
                std::string outcome;
                try {
                    runtime.run(thread, max_dispatches);
                    outcome = runtime.stop_reason();
                } catch (const std::exception &e) {
                    // Stubbed imports return 0, so a "created" object is a
                    // null pointer sooner or later: report where.
                    outcome = std::string("exception: ") + e.what();
                    outcome = outcome.substr(0, outcome.find('\n'));
                }
                std::map<std::string, std::size_t> distinct;
                for (std::size_t k = before; k < import_calls.size(); ++k)
                    ++distinct[import_calls[k]];
                std::printf("thread %s: %s (pc %s), %zu import calls, %zu distinct\n",
                            psprecomp::hex32(thread).c_str(), outcome.c_str(),
                            psprecomp::hex32(cpu.pc).c_str(), import_calls.size() - before,
                            distinct.size());
            }
        } else if (runtime.stop_reason().rfind("Missing HLE import ", 0) != 0) {
            ++failures;
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        return fail(e.what());
    }
}
