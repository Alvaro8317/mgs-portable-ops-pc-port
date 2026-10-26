// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_ppdmp_replay: replays PPSSPP frame dumps (.ppdmp) on the software GE
// (mpo_ge) and writes the resulting framebuffers as PNG.
//
//   mpo_ppdmp_replay --out <dir> [--alpha] <dump.ppdmp | dir>...
//
// For each dump it writes <stem>.target.png, the colour buffer the GE was
// drawing to at the end of the frame (480x272 from its top-left corner; the
// buffer PPSSPP's GE debugger previews), and, when the last DISPLAY record
// points elsewhere, <stem>.display.png. A dump ends at the flip, so the
// display buffer is usually the previous frame, which the dump does not
// contain. The output holds game pixels: write it to an ignored folder such
// as analysis/f11a/ and never commit it (CLAUDE.md §1.1).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "dump_replayer.hpp"
#include "image.hpp"
#include "ppdmp.hpp"

namespace {

namespace fs = std::filesystem;
using mpo::ge::replay::DisplayFramebuffer;

constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;

const char *FormatName(std::uint32_t fmt) {
    switch (fmt & 3) {
    case 0:
        return "565";
    case 1:
        return "5551";
    case 2:
        return "4444";
    default:
        return "8888";
    }
}

void Usage() {
    std::fprintf(stderr,
                 "usage: mpo_ppdmp_replay --out <dir> [--alpha] <dump.ppdmp | dir>...\n"
                 "  --out <dir>  folder for the PNG files (git-ignored, e.g. analysis/f11a)\n"
                 "  --alpha      keep the framebuffer alpha/stencil bits in the PNG\n");
}

bool WriteFb(const mpo::ge::MemoryView &mem, const DisplayFramebuffer &fb, bool alpha,
             const fs::path &path) {
    const auto image = mpo::ge::replay::ReadFramebuffer(mem, fb.addr, fb.stride, fb.format,
                                                        kScreenWidth, kScreenHeight, !alpha);
    const std::string err = mpo::ge::replay::WritePng(path, image);
    if (!err.empty()) {
        std::fprintf(stderr, "  error: %s\n", err.c_str());
        return false;
    }
    std::printf("  wrote %s (0x%08X stride %d %s)\n", path.string().c_str(), fb.addr, fb.stride,
                FormatName(fb.format));
    return true;
}

bool ReplayOne(const fs::path &path, const fs::path &out_dir, bool alpha) {
    std::printf("== %s\n", path.filename().string().c_str());
    mpo::ge::replay::FrameDump dump;
    if (std::string err = mpo::ge::replay::LoadFrameDump(path, &dump); !err.empty()) {
        std::fprintf(stderr, "  error: %s\n", err.c_str());
        return false;
    }
    std::printf("  version %u, game %s, %zu records, pushbuf %zu bytes\n", dump.version,
                dump.game_id.c_str(), dump.records.size(), dump.pushbuf.size());

    mpo::ge::replay::DumpReplayer replayer;
    mpo::ge::replay::ReplaySummary summary;
    replayer.ge().stats().log = [](std::string_view what) {
        std::printf("  unsupported: %.*s\n", static_cast<int>(what.size()), what.data());
    };
    const std::string err = replayer.Run(dump, &summary);
    const auto &st = replayer.ge().stats();
    std::printf("  register blocks %zu, GE commands %llu, draw commands %llu, vertices %llu\n",
                summary.register_blocks, static_cast<unsigned long long>(summary.commands),
                static_cast<unsigned long long>(st.draw_commands),
                static_cast<unsigned long long>(st.vertices));
    std::printf(
        "  drawn: triangles %llu, rectangles %llu, sprites %llu, clears %llu, points %llu"
        " (culled %llu), CLUT loads %llu\n",
        static_cast<unsigned long long>(st.triangles),
        static_cast<unsigned long long>(st.rectangles), static_cast<unsigned long long>(st.sprites),
        static_cast<unsigned long long>(st.clear_rects), static_cast<unsigned long long>(st.points),
        static_cast<unsigned long long>(st.culled), static_cast<unsigned long long>(st.clut_loads));
    std::printf("  lines %llu, patches %llu, clipped triangles %llu, bounding boxes %llu"
                " (culled %llu)\n",
                static_cast<unsigned long long>(st.lines),
                static_cast<unsigned long long>(st.patches),
                static_cast<unsigned long long>(st.clipped_triangles),
                static_cast<unsigned long long>(st.bbox_tests),
                static_cast<unsigned long long>(st.bbox_culled));
    std::uint64_t unsupported_total = 0;
    for (const auto &[what, count] : st.unsupported) unsupported_total += count;
    std::printf("  unsupported total %llu\n", static_cast<unsigned long long>(unsupported_total));
    for (const auto &[what, count] : st.unsupported) {
        std::printf("  not rendered/approximate: %s x%llu\n", what.c_str(),
                    static_cast<unsigned long long>(count));
    }
    if (!err.empty()) {
        std::fprintf(stderr, "  error: %s\n", err.c_str());
        return false;
    }
    if (!summary.finished) std::printf("  warning: the list did not reach FINISH\n");

    const std::string stem = path.stem().string();
    bool ok = true;
    const std::uint32_t target_vram = summary.draw_target.addr & 0x1FFFFF;
    if (summary.draw_target.stride > 0) {
        ok = WriteFb(replayer.memory(), summary.draw_target, alpha,
                     out_dir / (stem + ".target.png")) &&
             ok;
    }
    if (!summary.displays.empty()) {
        const DisplayFramebuffer &disp = summary.displays.back();
        if ((disp.addr & 0x1FFFFF) != target_vram || summary.draw_target.stride <= 0) {
            ok = WriteFb(replayer.memory(), disp, alpha, out_dir / (stem + ".display.png")) && ok;
        }
    }
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    fs::path out_dir;
    bool alpha = false;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--alpha") {
            alpha = true;
        } else if (arg == "-h" || arg == "--help") {
            Usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            Usage();
            return 2;
        } else {
            inputs.emplace_back(arg);
        }
    }
    if (out_dir.empty() || inputs.empty()) {
        Usage();
        return 2;
    }
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n", out_dir.string().c_str(),
                     ec.message().c_str());
        return 2;
    }

    std::vector<fs::path> dumps;
    for (const fs::path &input : inputs) {
        if (fs::is_directory(input, ec)) {
            for (const auto &entry : fs::directory_iterator(input, ec)) {
                if (entry.path().extension() == ".ppdmp") dumps.push_back(entry.path());
            }
        } else {
            dumps.push_back(input);
        }
    }
    std::sort(dumps.begin(), dumps.end());
    if (dumps.empty()) {
        std::fprintf(stderr, "no .ppdmp files found\n");
        return 2;
    }
    int failures = 0;
    for (const fs::path &dump : dumps) {
        if (!ReplayOne(dump, out_dir, alpha)) ++failures;
    }
    std::printf("%zu dump(s), %d failed\n", dumps.size(), failures);
    return failures == 0 ? 0 : 1;
}
