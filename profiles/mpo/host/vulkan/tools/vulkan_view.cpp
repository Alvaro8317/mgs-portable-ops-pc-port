// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vulkan_view (F14a): replays one PPSSPP frame dump (.ppdmp) on the
// Vulkan renderer and shows the GE's draw target in an SDL3 window through
// the swapchain, scaled with the 480x272 aspect ratio. Manual test of the
// presentation path; the output holds game pixels (CLAUDE.md §1.1).
//
//   mpo_vulkan_view [--frames N] [--scale S] [--integer] [--linear] [--vsync]
//                   [--capture out.png] <dump.ppdmp>
//
// --frames N exits after N presented frames (0 = until the window closes).
// --capture writes the last swapchain image as PNG (when the surface allows
// reading it back). Validation follows MPO_VK_VALIDATION / the build type;
// any validation error makes the exit code 1.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "context.hpp"
#include "dump_replayer.hpp"
#include "ge_renderer.hpp"
#include "image.hpp"
#include "ppdmp.hpp"
#include "sdl_window.hpp"
#include "swapchain.hpp"

namespace {

void Usage() {
    std::fprintf(stderr, "usage: mpo_vulkan_view [--frames N] [--scale S] [--integer] [--linear]"
                         " [--vsync] [--capture out.png] <dump.ppdmp>\n");
}

} // namespace

int main(int argc, char **argv) {
    int frames = 0;
    std::string capture_path;
    std::string dump_path;
    mpo::frontend::PresenterConfig config;
    config.title = "MGS: Portable Ops (Vulkan dump view)";
    bool linear = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (arg == "--scale" && i + 1 < argc) {
            config.scale = std::atoi(argv[++i]);
        } else if (arg == "--integer") {
            config.integer_scaling = true;
        } else if (arg == "--linear") {
            linear = true;
        } else if (arg == "--vsync") {
            config.vsync = true;
        } else if (arg == "--capture" && i + 1 < argc) {
            capture_path = argv[++i];
        } else if (!arg.empty() && arg[0] != '-' && dump_path.empty()) {
            dump_path = std::string(arg);
        } else {
            Usage();
            return 2;
        }
    }
    if (dump_path.empty()) {
        Usage();
        return 2;
    }

    mpo::ge::replay::FrameDump dump;
    if (std::string err = mpo::ge::replay::LoadFrameDump(dump_path, &dump); !err.empty()) {
        std::fprintf(stderr, "%s: %s\n", dump_path.c_str(), err.c_str());
        return 1;
    }

    std::string err;
    auto window = mpo::vk::SdlVulkanWindow::Create(config, &err);
    if (!window) {
        std::fprintf(stderr, "window: %s\n", err.c_str());
        return 3;
    }
    mpo::vk::ContextOptions opts;
    opts.app_name = "mpo_vulkan_view";
    opts.validation = mpo::vk::DefaultValidation();
    window->FillContextOptions(&opts);
    mpo::vk::VulkanContext::Status status;
    auto ctx = mpo::vk::VulkanContext::Create(opts, &status, &err);
    if (!ctx) {
        std::fprintf(stderr, "vulkan: %s\n", err.c_str());
        return 3;
    }
    const auto &caps = ctx->caps();
    std::printf("video driver %s; vulkan: %s, API %s, validation %s\n",
                mpo::vk::SdlVulkanWindow::video_driver().c_str(), caps.device_name.c_str(),
                mpo::vk::VersionString(caps.api_version).c_str(),
                caps.validation_active ? "on" : "off");

    mpo::ge::replay::DumpReplayer replayer;
    auto gpu = mpo::vk::GeRenderer::Create(ctx.get(), replayer.memory(), &err);
    if (!gpu) {
        std::fprintf(stderr, "renderer: %s\n", err.c_str());
        return 1;
    }
    replayer.ge().SetDrawBackend(gpu.get());
    mpo::ge::replay::ReplaySummary summary;
    err = replayer.Run(dump, &summary);
    if (!err.empty()) std::fprintf(stderr, "replay: %s\n", err.c_str());
    if (!gpu->EndFrame(false, &err)) {
        std::fprintf(stderr, "renderer: %s\n", err.c_str());
        return 1;
    }
    replayer.ge().SetDrawBackend(nullptr);
    mpo::vk::GeRenderer::TargetInfo target;
    if (!gpu->FindTarget(summary.draw_target.addr, &target)) {
        std::fprintf(stderr, "the dump drew nothing to its target 0x%08X\n",
                     summary.draw_target.addr);
        return 1;
    }
    std::printf("target 0x%08X (%dx%d image), %llu draws\n", summary.draw_target.addr, target.width,
                target.height, static_cast<unsigned long long>(gpu->stats().draws));

    int w = 0;
    int h = 0;
    window->DrawableSize(&w, &h);
    auto presenter = mpo::vk::SwapchainPresenter::Create(ctx.get(), w, h, config.vsync, &err);
    if (!presenter) {
        std::fprintf(stderr, "swapchain: %s\n", err.c_str());
        return 1;
    }
    std::printf("swapchain %dx%d, format %d, present mode %d\n", presenter->width(),
                presenter->height(), static_cast<int>(presenter->format()),
                static_cast<int>(presenter->present_mode()));

    std::vector<std::uint8_t> capture;
    bool running = true;
    int presented = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                presenter->Resize(e.window.data1, e.window.data2);
            }
        }
        const bool last = frames > 0 && presented + 1 >= frames;
        const bool want_capture = last && !capture_path.empty();
        if (!presenter->Present(target.image, mpo::frontend::kPspScreenWidth,
                                mpo::frontend::kPspScreenHeight,
                                window->Destination(mpo::frontend::kPspScreenWidth,
                                                    mpo::frontend::kPspScreenHeight),
                                linear, want_capture ? &capture : nullptr, &err)) {
            std::fprintf(stderr, "present: %s\n", err.c_str());
            return 1;
        }
        ++presented;
        if (last) running = false;
    }
    vkDeviceWaitIdle(ctx->device());
    std::printf("presented %d frame(s)\n", presented);

    if (!capture_path.empty()) {
        if (capture.empty()) {
            std::fprintf(stderr, "capture: the swapchain cannot be read back here\n");
        } else {
            mpo::ge::replay::Image img;
            img.width = presenter->width();
            img.height = presenter->height();
            img.rgba = capture;
            for (std::size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
            err = mpo::ge::replay::WritePng(capture_path, img);
            if (!err.empty()) {
                std::fprintf(stderr, "capture: %s\n", err.c_str());
                return 1;
            }
            std::printf("wrote %s (%dx%d)\n", capture_path.c_str(), img.width, img.height);
        }
    }
    presenter.reset();
    gpu.reset();
    const int errors = ctx->validation().errors.load();
    std::printf("vulkan validation errors %d, warnings %d\n", errors,
                ctx->validation().warnings.load());
    return errors == 0 ? 0 : 1;
}
