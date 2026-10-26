// SPDX-License-Identifier: GPL-3.0-or-later

#include "golden/golden.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mpo::golden {

namespace {

constexpr int kOpCount = 256;
constexpr int kTableOpCount = 1024; // Table-driven ops get more inputs.
constexpr int kSeqCount = 4096;
constexpr int kFunctionCount = 786432;

std::uint64_t name_seed(const std::string &name) {
    std::uint64_t hash = difftest::kDigestSeed;
    difftest::digest_append(&hash, name.data(), name.size());
    return hash & 0xFFFFFFFFull;
}

} // namespace

std::vector<Entry> plan() {
    std::vector<Entry> entries;
    for (const difftest::Spec *spec : difftest::specs_of(difftest::InstrClass::kVfpu)) {
        Entry e;
        e.kind = "op";
        e.name = spec->name;
        e.seed = name_seed(spec->name);
        e.count = (spec->flags & difftest::kSpecNeedsTables) ? kTableOpCount : kOpCount;
        entries.push_back(e);
    }
    Entry seq;
    seq.kind = "op";
    seq.name = "vfpuseq";
    seq.seed = name_seed(seq.name);
    seq.count = kSeqCount;
    entries.push_back(seq);
    const std::size_t op_entries = entries.size();
    for (std::size_t i = 0; i < op_entries; ++i) {
        Entry pin = entries[i];
        pin.kind = "pin";
        entries.push_back(pin);
    }
    for (const FunctionInfo &info : all_functions()) {
        Entry e;
        e.kind = "fn";
        e.name = info.name;
        e.seed = name_seed(info.name);
        e.count = kFunctionCount;
        entries.push_back(e);
    }
    return entries;
}

bool digest_op(const Entry &entry, difftest::Executor &executor, std::uint64_t *digest,
               std::string *error) {
    std::vector<difftest::TestCase> cases;
    if (entry.name == "vfpuseq") {
        cases = difftest::generate_cases(difftest::InstrClass::kVfpuSeq, entry.seed, entry.count);
    } else {
        const difftest::Spec *spec = difftest::find_spec(entry.name);
        if (!spec) {
            *error = "unknown spec " + entry.name;
            return false;
        }
        cases = difftest::generate_cases_for_spec(*spec, entry.seed, entry.count);
    }
    std::vector<difftest::CpuState> out(cases.size());
    std::vector<std::uint8_t> supported(cases.size(), 1);
    if (!executor.run(cases, out, supported, error)) return false;
    std::uint64_t hash = difftest::kDigestSeed;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const std::uint64_t one =
            supported[i] ? difftest::digest(out[i], entry.kind == "op") : 0xDEADDEADDEADDEADull;
        difftest::digest_append(&hash, &one, sizeof(one));
    }
    *digest = hash;
    return true;
}

std::string format(const Entry &entry) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s %s %08" PRIx64 " %d %016" PRIx64, entry.kind.c_str(),
                  entry.name.c_str(), entry.seed, entry.count, entry.digest);
    return text;
}

bool parse(const std::string &line, Entry *entry) {
    std::istringstream in(line);
    std::string seed;
    std::string digest;
    if (!(in >> entry->kind >> entry->name >> seed >> entry->count >> digest)) return false;
    if (entry->kind != "op" && entry->kind != "fn" && entry->kind != "pin") return false;
    entry->seed = std::strtoull(seed.c_str(), nullptr, 16);
    entry->digest = std::strtoull(digest.c_str(), nullptr, 16);
    return true;
}

} // namespace mpo::golden
