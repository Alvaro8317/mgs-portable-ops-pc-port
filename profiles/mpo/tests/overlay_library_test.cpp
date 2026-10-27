// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_overlay_library_test (F13): loads a real overlay library (built from
// overlay_test_corpus.cpp with mpo_add_overlay_library) with OverlayManager,
// the way mpo_boot loads the stage corpora, and runs the function it
// registers. Checks that the library resolves the runtime against this
// executable (ELF: -rdynamic; Windows: its import library), with no game data.
//
//   mpo_overlay_library_test <directory with the library>

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
    if (argc != 2) {
        std::fprintf(stderr, "usage: mpo_overlay_library_test <overlay directory>\n");
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

    corpus.install(runtime);
    check(runtime.has_function(0x08B8F900u), "the library registered its function");
    psprecomp::AllegrexContext ctx{};
    ctx.pc = 0x08B8F900u;
    ctx.gpr[31] = 0x08800000u;
    check(runtime.invoke_isolated_aot(0x08B8F900u, ctx), "the function runs");
    check(ctx.gpr[2] == 0xC0DEu, "its result");
    check(ctx.gpr[3] == static_cast<std::uint32_t>(psprecomp::g_runtime_starvation_interval_fast),
          "it reads the executable's runtime globals");

    // The library stays mapped: loading the directory again reuses it.
    check(overlays.load_libraries(argv[1]) == 1u && overlays.corpora().size() == 2u,
          "loaded again from the same mapping");
    // A directory that does not exist is reported, not fatal.
    check(overlays.load_libraries(std::filesystem::path(argv[1]) / "missing") == 0u,
          "missing directory");

    std::printf("mpo_overlay_library_test: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
