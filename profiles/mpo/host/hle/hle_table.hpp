// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HLE import table of the runtime (F08b).
//
// Every import a loaded module declares ends up registered with
// psprecomp::Runtime::register_hle() through this table, as one of:
//
//   - implemented: an HLE function (most go straight to mpo_kernel);
//   - stub: a minimal stand-in for a later phase (IO, UMD, time: F09), listed
//     apart in the histogram so it never passes for the real thing;
//   - link: an import of another loaded module (sound2, kjfs, zlibdec), which
//     jumps to that module's export;
//   - unimplemented: logs the library, name, stub and caller address and then
//     stops the runtime (policy kStop, the default) or returns 0 and goes on
//     (kReturnZero, to list how far the game gets). Never silent.
//
// The wrapper counts every call (the HLE histogram, like Yakumo's
// PSPRECOMP_HLE_HISTOGRAM), can trace each one, and returns to $ra unless the
// handler moved the pc (a link). Handlers read $a0..$t1 from the context and
// write $v0 (and $v1); a handler may block, switching PSP threads, and finds
// its own registers again when it resumes.

#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "kernel/kernel.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

namespace mpo::hle {

enum class HleKind : std::uint8_t { kImplemented, kStub, kLink, kUnimplemented };
enum class UnimplementedPolicy : std::uint8_t { kStop, kReturnZero };

[[nodiscard]] const char *hle_kind_name(HleKind kind);

class HleTable {
public:
    using Handler = std::function<void(psprecomp::AllegrexContext &)>;

    struct Entry {
        std::string library;
        std::uint32_t nid{};
        std::string name;
        HleKind kind{};
        Handler handler;
        // Traced before the call too: the handler may never return (exit).
        bool may_not_return{};
        std::uint64_t calls{};
        // The first caller, for the report of pending imports.
        std::uint32_t first_caller{};
    };

    HleTable(psprecomp::Runtime &runtime, kernel::Kernel &kernel)
        : runtime_(runtime), kernel_(kernel) {}
    HleTable(const HleTable &) = delete;
    HleTable &operator=(const HleTable &) = delete;

    // Registers (or replaces) the handler of library::nid.
    Entry &add(const std::string &library, std::uint32_t nid, const std::string &name, HleKind kind,
               Handler handler, bool may_not_return = false);
    [[nodiscard]] const Entry *find(const std::string &library, std::uint32_t nid) const;

    // Names for imports without a handler: PPSSPP's table (build_nid_table.py
    // TSV: nid, library, name, implemented, source), then `<library>_<NID>`.
    bool load_nid_names(const std::string &tsv_path, std::string *error);
    void add_nid_name(const std::string &library, std::uint32_t nid, const std::string &name);
    [[nodiscard]] std::string nid_name(const std::string &library, std::uint32_t nid) const;

    // Every import in `imports` without a handler becomes kUnimplemented.
    void cover_imports(const std::vector<psprecomp::PspImport> &imports);

    void set_unimplemented_policy(UnimplementedPolicy policy) { policy_ = policy; }
    // Each call as one line (nullptr: off), up to `limit` lines.
    void set_trace(std::FILE *trace, std::uint64_t limit = UINT64_MAX) {
        trace_ = trace;
        trace_limit_ = limit;
    }
    [[nodiscard]] std::FILE *trace() const noexcept { return trace_; }
    // Free-form event line in the trace, with the kernel time and thread.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void trace_event(const char *format, ...);

    [[nodiscard]] std::vector<const Entry *> entries() const;
    // Sorted by calls (then name): the HLE histogram.
    [[nodiscard]] std::vector<const Entry *> histogram() const;
    void report_histogram(std::FILE *out, std::size_t limit = SIZE_MAX) const;
    [[nodiscard]] std::uint64_t total_calls() const noexcept { return total_calls_; }

private:
    void dispatch(Entry &entry, psprecomp::AllegrexContext &ctx);
    void unimplemented(Entry &entry, psprecomp::AllegrexContext &ctx);
    void trace_line(const Entry &entry, const char *when, std::uint64_t start_us,
                    std::uint32_t stub, std::uint32_t ra, const std::uint32_t (&args)[6],
                    const psprecomp::AllegrexContext *ctx);
    [[nodiscard]] bool trace_room();

    psprecomp::Runtime &runtime_;
    kernel::Kernel &kernel_;
    std::deque<Entry> entries_;
    std::map<std::pair<std::string, std::uint32_t>, Entry *> index_;
    std::map<std::pair<std::string, std::uint32_t>, std::string> names_;
    UnimplementedPolicy policy_ = UnimplementedPolicy::kStop;
    std::FILE *trace_ = nullptr;
    std::uint64_t trace_limit_ = UINT64_MAX;
    std::uint64_t trace_lines_ = 0;
    std::uint64_t total_calls_ = 0;
};

} // namespace mpo::hle
