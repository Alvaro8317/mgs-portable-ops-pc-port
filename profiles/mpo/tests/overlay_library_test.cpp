// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_overlay_library_test (F13): loads a real overlay library (built from
// overlay_test_corpus.cpp with mpo_add_overlay_library) with OverlayManager,
// the way mpo_boot loads the stage corpora, and runs the function it
// registers. Checks that the library resolves the runtime against this
// executable (ELF: -rdynamic; Windows: its import library), with no game data.
// F22b: with the quick (-O0) and the optimized library of one stage in the
// same folder, the optimized one is installed.
//
//   mpo_overlay_library_test <directory with the library> <directory with the two levels>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "hle/system.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    if (condition) return;
    std::fprintf(stderr, "mpo_overlay_library_test: FAILED: %s\n", what);
    ++g_failures;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: mpo_overlay_library_test <overlay directory> <levels directory>\n");
        return 2;
    }
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    mpo::hle::SystemConfig config;
    config.fiber_stack_size = 1u << 20u;
    config.vblank = false;
    mpo::hle::System system(runtime, config);
    mpo::hle::OverlayManager &overlays = system.overlays();

    check(overlays.load_libraries(argv[1]) == 1u, "one corpus loaded");
    if (overlays.corpora().size() != 1u) return 1;
    const mpo::hle::OverlayCorpus &corpus = overlays.corpora()[0];
    check(corpus.name == "library_test", "name");
    check(corpus.base == 0x08B8F900u && corpus.size == 0x1000u, "base and size");
    check(corpus.prx_sha256 == std::string(64u, 'a'), "PRX SHA-256");
    check(corpus.text_offset == 0x10u && corpus.text_size == 0x20u, "text range");
    check(corpus.text_sha256 == std::string(64u, 'b'), "text SHA-256");
    check(std::filesystem::path(corpus.origin).stem() == "test_overlay", "origin");
    check(corpus.opt_level == 2, "the library reports its optimization level");

    corpus.install(runtime);
    check(runtime.has_function(0x08B8F900u), "the library registered its function");
    psprecomp::AllegrexContext ctx{};
    ctx.pc = 0x08B8F900u;
    ctx.gpr[31] = 0x08800000u;
    check(runtime.invoke_isolated_aot(0x08B8F900u, ctx), "the function runs");
    check(ctx.gpr[2] == 0xC0DEu, "its result");
    check(ctx.gpr[3] == static_cast<std::uint32_t>(psprecomp::g_runtime_starvation_interval_fast),
          "it reads the executable's runtime globals");

    // Each library is tried once per manager: the same folder again adds nothing.
    check(overlays.load_libraries(argv[1]) == 0u && overlays.corpora().size() == 1u,
          "a library is added once");
    // A directory that does not exist is reported, not fatal.
    check(overlays.load_libraries(std::filesystem::path(argv[1]) / "missing") == 0u,
          "missing directory");

    // F22: a library that appears after the start (the launcher compiles the
    // stages in the background) is found when the game loads a stage with no
    // corpus.
    std::error_code ec;
    const std::filesystem::path late = std::filesystem::path(argv[1]).string() + "-late";
    std::filesystem::remove_all(late, ec);
    std::filesystem::create_directories(late, ec);
    check(overlays.load_libraries(late) == 0u, "empty folder");
    std::filesystem::path library;
    for (const auto &entry : std::filesystem::directory_iterator(argv[1], ec)) {
        const std::filesystem::path ext = entry.path().extension();
        if (ext == ".so" || ext == ".dll" || ext == ".dylib") library = entry.path();
    }
    check(!library.empty(), "the test library");
    std::filesystem::copy_file(library, late / ("late_overlay" + library.extension().string()), ec);
    check(!ec, "copy the library");
    const std::uint8_t unknown_prx[] = {1, 2, 3};
    check(!overlays.on_module_loaded(1, "unknown", unknown_prx, 0x08C00000u, 0x1000u, 0x08C00000u),
          "an unknown stage still runs on the interpreter");
    check(overlays.corpora().size() == 2u, "the late library was added");
    if (overlays.corpora().size() == 2u) {
        check(std::filesystem::path(overlays.corpora()[1].origin).stem() == "late_overlay",
              "late library origin");
    }
    check(
        !overlays.on_module_loaded(2, "unknown", unknown_prx, 0x08C00000u, 0x1000u, 0x08C00000u) &&
            overlays.corpora().size() == 2u,
        "a rescan does not add a library twice");

    // F22b: both levels of one stage. The quick library sorts first by name
    // (test_overlay_o0 < test_overlay_o2) and is still not the one installed.
    {
        psprecomp::Runtime levels_runtime(32u * 1024u * 1024u);
        mpo::hle::System levels_system(levels_runtime, config);
        mpo::hle::OverlayManager &levels = levels_system.overlays();
        check(levels.load_libraries(argv[2]) == 2u, "both levels loaded");
        int quick = 0;
        int optimized = 0;
        for (const mpo::hle::OverlayCorpus &c : levels.corpora()) {
            if (c.opt_level == 0) ++quick;
            if (c.opt_level == 2) ++optimized;
        }
        check(quick == 1 && optimized == 1, "one corpus per level");
        // The SHA-256 of an empty file is the PRX_SHA256 of both.
        check(levels.on_module_loaded(3, "mgp_stage", {}, 0x08B8F900u, 0x1000u, 0x08B8F904u),
              "the stage is installed");
        psprecomp::AllegrexContext levels_ctx{};
        levels_ctx.pc = 0x08B8F900u;
        levels_ctx.gpr[31] = 0x08800000u;
        check(levels_runtime.invoke_isolated_aot(0x08B8F900u, levels_ctx), "the stage runs");
        check(levels_ctx.gpr[2] == 0xC0D2u, "the optimized library was installed");
    }

    std::printf("mpo_overlay_library_test: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
