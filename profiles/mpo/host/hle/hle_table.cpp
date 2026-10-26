// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle/hle_table.hpp"

#include <algorithm>
#include <cstdarg>
#include <fstream>
#include <sstream>

namespace mpo::hle {

const char *hle_kind_name(HleKind kind) {
    switch (kind) {
    case HleKind::kImplemented:
        return "hle";
    case HleKind::kStub:
        return "stub";
    case HleKind::kLink:
        return "link";
    case HleKind::kUnimplemented:
        return "UNIMPLEMENTED";
    }
    return "?";
}

HleTable::Entry &HleTable::add(const std::string &library, std::uint32_t nid,
                               const std::string &name, HleKind kind, Handler handler,
                               bool may_not_return) {
    const auto key = std::make_pair(library, nid);
    Entry *entry = nullptr;
    if (const auto found = index_.find(key); found != index_.end()) {
        entry = found->second;
    } else {
        entry = &entries_.emplace_back();
        index_.emplace(key, entry);
        entry->library = library;
        entry->nid = nid;
    }
    entry->name = name;
    entry->kind = kind;
    entry->handler = std::move(handler);
    entry->may_not_return = may_not_return;
    runtime_.register_hle(library, nid,
                          [this, entry](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                              dispatch(*entry, ctx);
                          });
    return *entry;
}

const HleTable::Entry *HleTable::find(const std::string &library, std::uint32_t nid) const {
    const auto found = index_.find(std::make_pair(library, nid));
    return found != index_.end() ? found->second : nullptr;
}

bool HleTable::load_nid_names(const std::string &tsv_path, std::string *error) {
    std::ifstream in(tsv_path);
    if (!in) {
        if (error != nullptr) *error = "cannot read " + tsv_path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string nid_text;
        std::string library;
        std::string name;
        if (!std::getline(fields, nid_text, '\t') || !std::getline(fields, library, '\t') ||
            !std::getline(fields, name, '\t'))
            continue;
        char *end = nullptr;
        const unsigned long nid = std::strtoul(nid_text.c_str(), &end, 16);
        if (end == nid_text.c_str() || *end != '\0' || name.empty()) continue;
        names_.emplace(std::make_pair(library, static_cast<std::uint32_t>(nid)), name);
    }
    return true;
}

void HleTable::add_nid_name(const std::string &library, std::uint32_t nid,
                            const std::string &name) {
    names_[std::make_pair(library, nid)] = name;
}

std::string HleTable::nid_name(const std::string &library, std::uint32_t nid) const {
    if (const Entry *entry = find(library, nid); entry != nullptr) return entry->name;
    if (const auto found = names_.find(std::make_pair(library, nid)); found != names_.end())
        return found->second;
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", nid);
    return library + "_" + text;
}

void HleTable::cover_imports(const std::vector<psprecomp::PspImport> &imports) {
    for (const psprecomp::PspImport &import : imports) {
        if (find(import.library, import.nid) != nullptr) continue;
        add(import.library, import.nid, nid_name(import.library, import.nid),
            HleKind::kUnimplemented, nullptr);
    }
}

bool HleTable::trace_room() {
    if (trace_ == nullptr || trace_lines_ >= trace_limit_) return false;
    ++trace_lines_;
    return true;
}

void HleTable::trace_event(const char *format, ...) {
    if (!trace_room()) return;
    const kernel::Uid thread = kernel_.current_thread();
    std::fprintf(trace_, "[%10llu us] [%5d] ", static_cast<unsigned long long>(kernel_.now_us()),
                 thread);
    std::va_list args;
    va_start(args, format);
    std::vfprintf(trace_, format, args);
    va_end(args);
    std::fputc('\n', trace_);
}

void HleTable::trace_line(const Entry &entry, const char *when, std::uint64_t start_us,
                          std::uint32_t stub, std::uint32_t ra, const std::uint32_t (&args)[6],
                          const psprecomp::AllegrexContext *ctx) {
    if (!trace_room()) return;
    const kernel::Uid thread = kernel_.current_thread();
    std::fprintf(
        trace_,
        "[%10llu us] [%5d] %-4s %s::%s(%08X, %08X, %08X, %08X, %08X, %08X) stub=%08X ra=%08X %s",
        static_cast<unsigned long long>(start_us), thread, hle_kind_name(entry.kind),
        entry.library.c_str(), entry.name.c_str(), args[0], args[1], args[2], args[3], args[4],
        args[5], stub, ra, when);
    if (ctx != nullptr) {
        std::fprintf(trace_, " -> %08X", ctx->gpr[2]);
        const std::uint64_t now = kernel_.now_us();
        if (now != start_us)
            std::fprintf(trace_, " (at %llu us)", static_cast<unsigned long long>(now));
    }
    std::fputc('\n', trace_);
}

void HleTable::dispatch(Entry &entry, psprecomp::AllegrexContext &ctx) {
    // Only scalars in this frame: the handler may block (another fiber runs)
    // or never return (the thread exits and its fiber is abandoned).
    ++entry.calls;
    ++total_calls_;
    const std::uint32_t stub = ctx.pc;
    const std::uint32_t ra = ctx.gpr[31];
    if (entry.first_caller == 0u) entry.first_caller = ra - 8u;
    const std::uint32_t args[6] = {ctx.gpr[4], ctx.gpr[5], ctx.gpr[6],
                                   ctx.gpr[7], ctx.gpr[8], ctx.gpr[9]};
    const std::uint64_t start_us = kernel_.now_us();
    if (trace_ != nullptr && entry.may_not_return)
        trace_line(entry, "enter", start_us, stub, ra, args, nullptr);
    if (entry.kind == HleKind::kUnimplemented || !entry.handler) {
        unimplemented(entry, ctx);
    } else {
        entry.handler(ctx);
    }
    ctx.gpr[0] = 0u;
    // An import returns to its caller, unless it moved the pc (a link to a
    // module export, or a stop). After a blocking call this is again the
    // calling thread's context.
    if (ctx.pc == stub && !runtime_.stopped()) ctx.pc = ra;
    if (trace_ != nullptr) trace_line(entry, "", start_us, stub, ra, args, &ctx);
}

void HleTable::unimplemented(Entry &entry, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t caller = ctx.gpr[31] - 8u;
    kernel::ThreadInfo info;
    const kernel::Uid thread = kernel_.current_thread();
    const bool named = kernel_.thread_info(thread, info);
    char message[256];
    std::snprintf(
        message, sizeof(message),
        "unimplemented HLE import %s::%s (NID %08X), stub %08X, called from %08X on thread %d '%s'",
        entry.library.c_str(), entry.name.c_str(), entry.nid, ctx.pc, caller, thread,
        named ? info.name.c_str() : "?");
    if (policy_ == UnimplementedPolicy::kStop) {
        std::fprintf(stderr, "mpo: %s\n", message);
        runtime_.stop(message);
        return;
    }
    if (entry.calls == 1u) std::fprintf(stderr, "mpo: %s; returning 0\n", message);
    ctx.gpr[2] = 0u;
    ctx.gpr[3] = 0u;
}

std::vector<const HleTable::Entry *> HleTable::entries() const {
    std::vector<const Entry *> out;
    out.reserve(entries_.size());
    for (const Entry &entry : entries_) out.push_back(&entry);
    return out;
}

std::vector<const HleTable::Entry *> HleTable::histogram() const {
    std::vector<const Entry *> out = entries();
    std::stable_sort(out.begin(), out.end(), [](const Entry *a, const Entry *b) {
        if (a->calls != b->calls) return a->calls > b->calls;
        if (a->library != b->library) return a->library < b->library;
        return a->name < b->name;
    });
    return out;
}

void HleTable::report_histogram(std::FILE *out, std::size_t limit) const {
    std::size_t called = 0;
    for (const Entry &entry : entries_) called += entry.calls != 0u ? 1u : 0u;
    std::fprintf(out, "[hle-histogram] %zu imports registered, %zu called, %llu calls\n",
                 entries_.size(), called, static_cast<unsigned long long>(total_calls_));
    std::size_t shown = 0;
    for (const Entry *entry : histogram()) {
        if (entry->calls == 0u || shown++ >= limit) break;
        std::fprintf(out, "[hle-histogram] %10llu  %-13s %s::%s\n",
                     static_cast<unsigned long long>(entry->calls), hle_kind_name(entry->kind),
                     entry->library.c_str(), entry->name.c_str());
    }
}

} // namespace mpo::hle
