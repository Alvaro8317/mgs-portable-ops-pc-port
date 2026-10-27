// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vulkan_compare (F15a): replays PPSSPP frame dumps (.ppdmp) with the
// software GE (softgpu, the reference) and with the Vulkan renderer, compares
// the framebuffers pixel by pixel and fails when the difference passes a
// threshold.
//
//   mpo_vulkan_compare [--max-pixels N] [--max-delta D] [--paths both|13|12]
//                      [--alpha] [--out <dir>] <dump.ppdmp | dir>...
//
// - For each dump it compares the colour buffer the GE drew to (480x272 from
//   its top-left corner) and, when the last DISPLAY record points elsewhere,
//   the display buffer, as mpo_ppdmp_replay writes them. RGB by default;
//   --alpha also compares the alpha/stencil bits.
// - A frame fails when more than N pixels differ (--max-pixels, default 130,
//   0.1 % of 480x272) or any channel differs by more than D (--max-delta,
//   default 24, three 5-bit steps); a dump also fails on any Vulkan
//   validation error or replay error. --max-pixels 0 --max-delta 0 asks for
//   identical frames. The defaults are for real GPUs: see
//   docs/sessions/F15a.md.
// - --paths: the Vulkan 1.3 path (dynamic rendering, when the device has
//   1.3), the 1.2 path (render passes, like MPO_VK_NO_13=1) or both (the
//   default). The renderer's A/B toggles (MPO_VK_BLEND, MPO_VK_DST_READ, ...)
//   apply as usual; validation follows MPO_VK_VALIDATION (on in Debug).
// - --out writes, for each frame that differs, <stem>.<frame>.<path>.diff.png:
//   equal pixels darkened, differing ones in red (brighter is larger). The
//   images hold game pixels: write them to an ignored folder such as
//   analysis/f15a/ (CLAUDE.md §1.1).
//
// Exit status: 0 when every frame is within the threshold, 1 when not, 2 on
// usage errors, 3 when Vulkan is not available.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "context.hpp"
#include "dump_replayer.hpp"
#include "ge_renderer.hpp"
#include "image.hpp"
#include "ppdmp.hpp"

namespace {

namespace fs = std::filesystem;
using mpo::ge::replay::DisplayFramebuffer;
using mpo::ge::replay::Image;

constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;

struct Options {
    int max_pixels = 130;
    int max_delta = 24;
    bool alpha = false;
    fs::path out_dir;
    std::vector<bool> paths = {true, false}; // allow Vulkan 1.3
};

struct Frame {
    std::string name; // "target" or "display"
    Image image;
};

struct Replay {
    bool ok = false;
    std::string error;
    std::vector<Frame> frames;
    int validation_errors = 0;
    mpo::vk::RendererStats stats;
    mpo::vk::DstRead dst_read = mpo::vk::DstRead::kAuto;
};

Image ReadFrame(const mpo::ge::MemoryView &mem, const DisplayFramebuffer &fb, bool alpha) {
    return mpo::ge::replay::ReadFramebuffer(mem, fb.addr, fb.stride, fb.format, kScreenWidth,
                                            kScreenHeight, !alpha);
}

// Replays one dump; `ctx` null means softgpu.
Replay RunDump(const mpo::ge::replay::FrameDump &dump, mpo::vk::VulkanContext *ctx, bool alpha) {
    Replay out;
    mpo::ge::replay::DumpReplayer replayer;
    mpo::ge::replay::ReplaySummary summary;
    std::unique_ptr<mpo::vk::GeRenderer> gpu;
    int validation_before = 0;
    if (ctx) {
        validation_before = ctx->validation().errors.load();
        std::string err;
        gpu = mpo::vk::GeRenderer::Create(ctx, replayer.memory(), &err);
        if (!gpu) {
            out.error = "vulkan renderer: " + err;
            return out;
        }
        replayer.ge().SetDrawBackend(gpu.get());
    }
    out.error = replayer.Run(dump, &summary);
    if (gpu) {
        std::string err;
        if (!gpu->EndFrame(true, &err) && out.error.empty()) out.error = "vulkan: " + err;
        replayer.ge().SetDrawBackend(nullptr);
        out.stats = gpu->stats();
        out.stats.log = nullptr;
        out.dst_read = gpu->dst_read();
        gpu.reset();
        out.validation_errors = ctx->validation().errors.load() - validation_before;
    }
    if (!out.error.empty()) return out;
    const std::uint32_t target_vram = summary.draw_target.addr & 0x1FFFFF;
    if (summary.draw_target.stride > 0) {
        out.frames.push_back({"target", ReadFrame(replayer.memory(), summary.draw_target, alpha)});
    }
    if (!summary.displays.empty()) {
        const DisplayFramebuffer &disp = summary.displays.back();
        if ((disp.addr & 0x1FFFFF) != target_vram || summary.draw_target.stride <= 0) {
            out.frames.push_back({"display", ReadFrame(replayer.memory(), disp, alpha)});
        }
    }
    out.ok = true;
    return out;
}

struct Diff {
    int differing = 0;
    std::array<int, 4> max_delta{};
    int large = 0; // pixels with a channel delta above the threshold
};

Diff CompareImages(const Image &a, const Image &b, bool alpha, int max_delta, Image *vis) {
    Diff d;
    const int channels = alpha ? 4 : 3;
    if (vis) {
        vis->width = a.width;
        vis->height = a.height;
        vis->rgba.assign(a.rgba.size(), std::uint8_t{0});
    }
    for (std::size_t p = 0; p + 3 < a.rgba.size() && p + 3 < b.rgba.size(); p += 4) {
        int worst = 0;
        for (int c = 0; c < channels; ++c) {
            const int delta = std::abs(static_cast<int>(a.rgba[p + static_cast<std::size_t>(c)]) -
                                       static_cast<int>(b.rgba[p + static_cast<std::size_t>(c)]));
            worst = std::max(worst, delta);
            d.max_delta[static_cast<std::size_t>(c)] =
                std::max(d.max_delta[static_cast<std::size_t>(c)], delta);
        }
        if (worst > 0) ++d.differing;
        if (worst > max_delta) ++d.large;
        if (vis) {
            if (worst > 0) {
                vis->rgba[p] = static_cast<std::uint8_t>(std::min(255, 96 + worst * 2));
            } else {
                for (int c = 0; c < 3; ++c) {
                    vis->rgba[p + static_cast<std::size_t>(c)] =
                        static_cast<std::uint8_t>(a.rgba[p + static_cast<std::size_t>(c)] / 4);
                }
            }
            vis->rgba[p + 3] = 255;
        }
    }
    return d;
}

std::unique_ptr<mpo::vk::VulkanContext> OpenVulkan(bool allow_13, int *status_code) {
    mpo::vk::ContextOptions opts;
    opts.app_name = "mpo_vulkan_compare";
    opts.validation = mpo::vk::DefaultValidation();
    opts.allow_vulkan_1_3 = allow_13;
    opts.log = [](std::string_view line) {
        std::printf("  %.*s\n", static_cast<int>(line.size()), line.data());
    };
    mpo::vk::VulkanContext::Status status;
    std::string err;
    auto ctx = mpo::vk::VulkanContext::Create(opts, &status, &err);
    if (!ctx) {
        std::fprintf(stderr, "vulkan: %s\n", err.c_str());
        *status_code = status == mpo::vk::VulkanContext::Status::kUnavailable ? 3 : 1;
    }
    return ctx;
}

void Usage() {
    std::fprintf(stderr,
                 "usage: mpo_vulkan_compare [--max-pixels N] [--max-delta D] [--paths both|13|12]"
                 " [--alpha] [--out <dir>] <dump.ppdmp | dir>...\n"
                 "  --max-pixels N  differing pixels allowed per frame (default 130 = 0.1 %%)\n"
                 "  --max-delta D   largest per-channel difference allowed (default 24)\n"
                 "  --paths P       Vulkan 1.3 path, 1.2 path or both (default both)\n"
                 "  --alpha         also compare the alpha/stencil bits\n"
                 "  --out <dir>     diff images of the frames that differ (git-ignored dir)\n");
}

bool ParseInt(const char *text, int *out) {
    char *end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 0 || v > 1 << 24) return false;
    *out = static_cast<int>(v);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Options opt;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--max-pixels" && has_value) {
            if (!ParseInt(argv[++i], &opt.max_pixels)) {
                Usage();
                return 2;
            }
        } else if (arg == "--max-delta" && has_value) {
            if (!ParseInt(argv[++i], &opt.max_delta)) {
                Usage();
                return 2;
            }
        } else if (arg == "--paths" && has_value) {
            const std::string_view p = argv[++i];
            if (p == "both") {
                opt.paths = {true, false};
            } else if (p == "13") {
                opt.paths = {true};
            } else if (p == "12") {
                opt.paths = {false};
            } else {
                Usage();
                return 2;
            }
        } else if (arg == "--alpha") {
            opt.alpha = true;
        } else if (arg == "--out" && has_value) {
            opt.out_dir = argv[++i];
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
    if (inputs.empty()) {
        Usage();
        return 2;
    }
    std::error_code ec;
    if (!opt.out_dir.empty()) {
        fs::create_directories(opt.out_dir, ec);
        if (ec) {
            std::fprintf(stderr, "cannot create %s: %s\n", opt.out_dir.string().c_str(),
                         ec.message().c_str());
            return 2;
        }
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

    // softgpu once per dump.
    std::vector<mpo::ge::replay::FrameDump> loaded(dumps.size());
    std::vector<Replay> soft(dumps.size());
    for (std::size_t i = 0; i < dumps.size(); ++i) {
        if (std::string err = mpo::ge::replay::LoadFrameDump(dumps[i], &loaded[i]); !err.empty()) {
            soft[i].error = err;
            continue;
        }
        soft[i] = RunDump(loaded[i], nullptr, opt.alpha);
    }

    std::printf("threshold: at most %d differing pixels per frame, max channel delta %d%s\n",
                opt.max_pixels, opt.max_delta, opt.alpha ? " (RGBA)" : " (RGB)");
    int failures = 0;
    int frames_compared = 0;
    for (const bool allow_13 : opt.paths) {
        int status_code = 0;
        auto ctx = OpenVulkan(allow_13, &status_code);
        if (!ctx) return status_code;
        const auto &caps = ctx->caps();
        const std::string path = caps.dynamic_rendering ? "13" : "12";
        if (allow_13 && !caps.dynamic_rendering) {
            std::printf("(the device has no Vulkan 1.3 dynamic rendering: the 1.3 run uses the "
                        "1.2 path)\n");
        }
        const mpo::vk::RendererOptions ro = mpo::vk::RendererOptions::FromEnv();
        std::printf("\nvulkan: %s (%s), API %s, %s, validation %s%s\n", caps.device_name.c_str(),
                    caps.driver_name.c_str(), mpo::vk::VersionString(caps.api_version).c_str(),
                    caps.dynamic_rendering ? "dynamic rendering" : "render passes",
                    caps.validation_active ? "on" : "off",
                    caps.sync_validation_active ? " (+synchronization)" : "");
        std::printf("blend %s, destination reads %s (requested), batching %s, texture cache %s\n",
                    mpo::vk::BlendPathName(ro.blend), mpo::vk::DstReadName(ro.dst_read),
                    ro.batching ? "on" : "off", ro.texture_cache ? "on" : "off");
        std::printf("%-40s %-8s %9s %8s %-15s %7s %s\n", "dump", "frame", "differing", "%",
                    "max delta", "valid.", "result");
        mpo::vk::RendererStats total;
        mpo::vk::DstRead used = mpo::vk::DstRead::kAuto;
        for (std::size_t i = 0; i < dumps.size(); ++i) {
            const std::string stem = dumps[i].stem().string();
            if (!soft[i].ok) {
                std::printf("%-40s soft replay failed: %s\n", stem.c_str(), soft[i].error.c_str());
                ++failures;
                continue;
            }
            const Replay gpu = RunDump(loaded[i], ctx.get(), opt.alpha);
            if (!gpu.ok) {
                std::printf("%-40s vulkan replay failed: %s\n", stem.c_str(), gpu.error.c_str());
                ++failures;
                continue;
            }
            for (const Frame &sf : soft[i].frames) {
                const auto it = std::find_if(gpu.frames.begin(), gpu.frames.end(),
                                             [&](const Frame &f) { return f.name == sf.name; });
                if (it == gpu.frames.end()) {
                    std::printf("%-40s %-8s missing in the vulkan replay\n", stem.c_str(),
                                sf.name.c_str());
                    ++failures;
                    continue;
                }
                Image vis;
                const Diff d = CompareImages(sf.image, it->image, opt.alpha, opt.max_delta,
                                             opt.out_dir.empty() ? nullptr : &vis);
                ++frames_compared;
                const int worst = *std::max_element(d.max_delta.begin(), d.max_delta.end());
                const bool pass = d.differing <= opt.max_pixels && worst <= opt.max_delta &&
                                  gpu.validation_errors == 0;
                if (!pass) ++failures;
                char delta[32];
                if (opt.alpha) {
                    std::snprintf(delta, sizeof(delta), "%d %d %d %d", d.max_delta[0],
                                  d.max_delta[1], d.max_delta[2], d.max_delta[3]);
                } else {
                    std::snprintf(delta, sizeof(delta), "%d %d %d", d.max_delta[0], d.max_delta[1],
                                  d.max_delta[2]);
                }
                const double pct = 100.0 * d.differing / (kScreenWidth * kScreenHeight);
                std::printf("%-40s %-8s %9d %8.3f %-15s %7d %s", stem.c_str(), sf.name.c_str(),
                            d.differing, pct, delta, gpu.validation_errors, pass ? "ok" : "FAIL");
                if (d.large > 0) std::printf(" (%d pixels above delta %d)", d.large, opt.max_delta);
                std::printf("\n");
                if (d.differing > 0 && !opt.out_dir.empty()) {
                    const fs::path out =
                        opt.out_dir / (stem + "." + sf.name + "." + path + ".diff.png");
                    if (const std::string err = mpo::ge::replay::WritePng(out, vis); !err.empty())
                        std::fprintf(stderr, "  error: %s\n", err.c_str());
                }
            }
            used = gpu.dst_read;
            total.draws += gpu.stats.draws;
            total.batches += gpu.stats.batches;
            total.dst_batches += gpu.stats.dst_batches;
            total.dst_barriers += gpu.stats.dst_barriers;
            total.dst_copies += gpu.stats.dst_copies;
            total.fixed_blend_batches += gpu.stats.fixed_blend_batches;
            if (!gpu.stats.approximations.empty()) {
                for (const auto &[what, count] : gpu.stats.approximations) {
                    std::printf("  approximate: %s x%llu\n", what.c_str(),
                                static_cast<unsigned long long>(count));
                }
            }
        }
        std::printf("path %s, destination reads by %s: %llu draws in %llu batches, %llu reading "
                    "the destination (%llu barriers, %llu copies), %llu with fixed-function "
                    "blending\n",
                    path.c_str(), mpo::vk::DstReadName(used),
                    static_cast<unsigned long long>(total.draws),
                    static_cast<unsigned long long>(total.batches),
                    static_cast<unsigned long long>(total.dst_batches),
                    static_cast<unsigned long long>(total.dst_barriers),
                    static_cast<unsigned long long>(total.dst_copies),
                    static_cast<unsigned long long>(total.fixed_blend_batches));
    }
    std::printf("\n%d frame comparison(s), %d failed\n", frames_compared, failures);
    return failures == 0 ? 0 : 1;
}
