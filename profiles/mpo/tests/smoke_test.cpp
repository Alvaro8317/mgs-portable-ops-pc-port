// SPDX-License-Identifier: GPL-3.0-or-later
//
// Profile smoke test: proves the profile links against psprecomp_core and that
// the runtime contract the profile relies on holds without any game data.

#include <psprecomp/runtime.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void test_registered_import_is_invoked() {
    psprecomp::Runtime runtime;
    int calls = 0;
    runtime.register_hle("TestLibrary", 0x12345678u,
                         [&calls](psprecomp::Runtime &, psprecomp::AllegrexContext &) { ++calls; });
    runtime.invoke_import("TestLibrary", 0x12345678u, runtime.cpu());
    expect(calls == 1, "registered HLE import was not invoked exactly once");
    expect(!runtime.stopped(), "runtime stopped after a registered import");
}

// CLAUDE.md §5: an unimplemented NID must fail visibly, never silently.
void test_missing_import_stops_visibly() {
    psprecomp::Runtime runtime;
    runtime.invoke_import("TestLibrary", 0xDEADBEEFu, runtime.cpu());
    expect(runtime.stopped(), "missing HLE import did not stop the runtime");
    expect(runtime.stop_reason().find("Missing HLE import") != std::string::npos,
           "missing HLE import stop reason does not name the import");
}

void test_user_ram_bounds() {
    psprecomp::Runtime runtime;
    auto &memory = runtime.memory();
    // PSP-1000 user RAM: 0x08800000-0x09FFFFFF (CLAUDE.md §5).
    expect(memory.contains(0x08800000u), "user RAM start is not mapped");
    expect(memory.contains(0x09FFFFFCu), "user RAM end is not mapped");
    expect(!memory.contains(0x0A000000u), "memory past user RAM is mapped");
}

} // namespace

int main() {
    test_registered_import_is_invoked();
    test_missing_import_stops_visibly();
    test_user_ram_bounds();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("mpo_profile_smoke_test: all checks passed");
    return EXIT_SUCCESS;
}
