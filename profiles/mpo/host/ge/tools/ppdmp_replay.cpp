// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_ppdmp_replay: replays PPSSPP frame dumps (.ppdmp) on the software GE
// (mpo_ge) and writes the resulting framebuffers as PNG.
//
//   mpo_ppdmp_replay --out <dir> [--alpha] [--renderer=soft|vulkan] <dump.ppdmp | dir>...
//
// --renderer=vulkan (F14a) rasterizes on the GPU with mpo::vk::GeRenderer
// instead of softgpu, then reads the targets back into VRAM, so the PNG
// files come out of the same code and compare 1:1 with the soft ones
// (ge_frame_diff.py). The Vulkan validation layers are on in Debug builds
// (MPO_VK_VALIDATION=0/1 overrides); any validation error fails the dump.
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

#if MPO_HAVE_VULKAN
#include <memory>

#include "context.hpp"
#include "ge_renderer.hpp"
#endif

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
                 "usage: mpo_ppdmp_replay --out <dir> [--alpha] [--renderer=soft|vulkan]"
                 " <dump.ppdmp | dir>...\n"
                 "  --out <dir>        folder for the PNG files (git-ignored, e.g. analysis/f11a)\n"
                 "  --alpha            keep the framebuffer alpha/stencil bits in the PNG\n"
                 "  --renderer=<name>  soft (softgpu, default) or vulkan (GPU, F14a)\n");
}

#if MPO_HAVE_VULKAN
struct VulkanBench {
    std::unique_ptr<mpo::vk::VulkanContext> ctx;
};

bool OpenVulkan(VulkanBench *vk) {
    mpo::vk::ContextOptions opts;
    opts.app_name = "mpo_ppdmp_replay";
    opts.validation = mpo::vk::DefaultValidation();
    opts.log = [](std::string_view line) {
        std::printf("  %.*s\n", static_cast<int>(line.size()), line.data());
    };
    mpo::vk::VulkanContext::Status status;
    std::string err;
    vk->ctx = mpo::vk::VulkanContext::Create(opts, &status, &err);
    if (!vk->ctx) {
        std::fprintf(stderr, "vulkan: %s\n", err.c_str());
        return false;
    }
    const auto &caps = vk->ctx->caps();
    std::printf("vulkan: %s (%s), API %s, %s, validation %s%s\n", caps.device_name.c_str(),
                caps.driver_name.c_str(), mpo::vk::VersionString(caps.api_version).c_str(),
                caps.dynamic_rendering ? "dynamic rendering" : "render passes",
                caps.validation_active ? "on" : "off",
                caps.sync_validation_active ? " (+synchronization)" : "");
    return true;
}
#endif

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

bool ReplayOne(const fs::path &path, const fs::path &out_dir, bool alpha, void *vulkan) {
    (void)vulkan; // null unless --renderer=vulkan
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
#if MPO_HAVE_VULKAN
    std::unique_ptr<mpo::vk::GeRenderer> gpu;
    int validation_before = 0;
    if (vulkan) {
        auto *vk = static_cast<VulkanBench *>(vulkan);
        validation_before = vk->ctx->validation().errors.load();
        std::string verr;
        gpu = mpo::vk::GeRenderer::Create(vk->ctx.get(), replayer.memory(), &verr);
        if (!gpu) {
            std::fprintf(stderr, "  error: vulkan renderer: %s\n", verr.c_str());
            return false;
        }
        gpu->stats().log = [](std::string_view what) {
            std::printf("  vulkan approximate: %.*s\n", static_cast<int>(what.size()), what.data());
        };
        replayer.ge().SetDrawBackend(gpu.get());
    }
#endif
    std::string err = replayer.Run(dump, &summary);
#if MPO_HAVE_VULKAN
    if (gpu) {
        std::string verr;
        if (!gpu->EndFrame(true, &verr) && err.empty()) err = "vulkan: " + verr;
        replayer.ge().SetDrawBackend(nullptr);
        const auto &vs = gpu->stats();
        std::printf("  vulkan: draws %llu, batches %llu, pipelines %zu, textures %zu"
                    " (uploads %llu, hits %llu), CLUT uploads %llu, line pixels %llu\n",
                    static_cast<unsigned long long>(vs.draws),
                    static_cast<unsigned long long>(vs.batches), gpu->pipeline_count(),
                    gpu->texture_count(), static_cast<unsigned long long>(vs.texture_uploads),
                    static_cast<unsigned long long>(vs.texture_hits),
                    static_cast<unsigned long long>(vs.clut_uploads),
                    static_cast<unsigned long long>(vs.line_pixels));
        std::printf("  vulkan: blend %s, destination reads by %s: %llu batches (%llu barriers,"
                    " %llu copies), %llu batches with fixed-function blending\n",
                    mpo::vk::BlendPathName(gpu->options().blend),
                    mpo::vk::DstReadName(gpu->dst_read()),
                    static_cast<unsigned long long>(vs.dst_batches),
                    static_cast<unsigned long long>(vs.dst_barriers),
                    static_cast<unsigned long long>(vs.dst_copies),
                    static_cast<unsigned long long>(vs.fixed_blend_batches));
        for (const auto &[what, count] : vs.approximations) {
            std::printf("  vulkan approximate: %s x%llu\n", what.c_str(),
                        static_cast<unsigned long long>(count));
        }
        gpu.reset();
        auto *vk = static_cast<VulkanBench *>(vulkan);
        const int errors = vk->ctx->validation().errors.load() - validation_before;
        std::printf("  vulkan validation errors %d\n", errors);
        if (errors > 0 && err.empty()) err = "vulkan validation errors";
    }
#endif
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
    std::string renderer = "soft";
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--alpha") {
            alpha = true;
        } else if (arg.rfind("--renderer=", 0) == 0) {
            renderer = std::string(arg.substr(11));
        } else if (arg == "--renderer" && i + 1 < argc) {
            renderer = argv[++i];
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
    void *vulkan = nullptr;
#if MPO_HAVE_VULKAN
    VulkanBench bench;
    if (renderer == "vulkan") {
        if (!OpenVulkan(&bench)) return 3;
        vulkan = &bench;
    }
#endif
    if (renderer != "soft" && vulkan == nullptr) {
        std::fprintf(stderr, "renderer '%s' is not available in this build\n", renderer.c_str());
        return 2;
    }
    int failures = 0;
    for (const fs::path &dump : dumps) {
        if (!ReplayOne(dump, out_dir, alpha, vulkan)) ++failures;
    }
    std::printf("%zu dump(s), %d failed\n", dumps.size(), failures);
    return failures == 0 ? 0 : 1;
}
