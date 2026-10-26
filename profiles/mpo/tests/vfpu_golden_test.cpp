// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vfpu_golden_test: recomputes every golden vector of
// profiles/mpo/tests/data/vfpu_golden.txt with mpo_vfpu alone (no PPSSPP code
// linked) and compares the digests bit for bit.
//
//   mpo_vfpu_golden_test --tables DIR --golden FILE
//
// Exit codes: 0 pass, 1 differences, 2 error, 77 skipped (no tables).

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "difftest/difftest.hpp"
#include "golden/golden.hpp"
#include "vfpu/vfpu.hpp"

int main(int argc, char **argv) {
    std::string tables;
    std::string golden_path;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--tables") tables = argv[i + 1];
        if (arg == "--golden") golden_path = argv[i + 1];
    }
    std::string error;
    if (tables.empty() || !mpo::vfpu::load_tables(tables, &error)) {
        std::printf("SKIP: VFPU tables not available (%s)\n", error.c_str());
        return 77;
    }
    std::ifstream in(golden_path);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", golden_path.c_str());
        return 2;
    }

    auto executor = mpo::difftest::make_reference_interpreter();
    int checked = 0;
    int failed = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        mpo::golden::Entry entry;
        if (!mpo::golden::parse(line, &entry)) {
            std::fprintf(stderr, "malformed line: %s\n", line.c_str());
            return 2;
        }
        std::uint64_t digest = 0;
        if (entry.kind == "op" || entry.kind == "pin") {
            if (!mpo::golden::digest_op(entry, *executor, &digest, &error)) {
                std::fprintf(stderr, "%s: %s\n", entry.name.c_str(), error.c_str());
                return 2;
            }
        } else {
            bool found = false;
            for (const mpo::golden::FunctionInfo &info : mpo::golden::all_functions()) {
                if (entry.name != info.name) continue;
                found = true;
                const std::vector<float> inputs =
                    mpo::golden::make_inputs(info, entry.seed, entry.count);
                digest = mpo::golden::digest_function(info, inputs, mpo::golden::call_mpo);
            }
            if (!found) {
                std::fprintf(stderr, "unknown function %s\n", entry.name.c_str());
                return 2;
            }
        }
        ++checked;
        if (digest != entry.digest) {
            ++failed;
            std::printf("MISMATCH %s %s: golden %016llx, mpo_vfpu %016llx\n", entry.kind.c_str(),
                        entry.name.c_str(), static_cast<unsigned long long>(entry.digest),
                        static_cast<unsigned long long>(digest));
        }
    }
    std::printf("%d golden entries checked, %d mismatched\n", checked, failed);
    if (checked == 0) return 2;
    return failed == 0 ? 0 : 1;
}
