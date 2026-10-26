// SPDX-License-Identifier: GPL-3.0-or-later
//
// An SDL3 window for the Vulkan renderer (SDL_WINDOW_VULKAN). It gives the
// VulkanContext what it needs from the window system (instance extensions,
// SDL's vkGetInstanceProcAddr and the surface) and the drawable size for the
// swapchain. F10a's SdlPresenter keeps its SDL_Renderer path; this window is
// the Vulkan counterpart, with the same PresenterConfig and the same
// letterboxing (FitRect).
//
// Not thread-safe: create and use it on the thread that runs SDL's events.

#pragma once

#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "context.hpp"
#include "sdl_presenter.h"
#include "swapchain.hpp"

namespace mpo::vk {

class SdlVulkanWindow {
public:
    static std::unique_ptr<SdlVulkanWindow> Create(const frontend::PresenterConfig &config,
                                                   std::string *error);
    ~SdlVulkanWindow();
    SdlVulkanWindow(const SdlVulkanWindow &) = delete;
    SdlVulkanWindow &operator=(const SdlVulkanWindow &) = delete;

    // Adds the surface extensions, SDL's loader and the surface factory.
    void FillContextOptions(ContextOptions *options) const;

    [[nodiscard]] SDL_Window *window() const { return window_; }
    void DrawableSize(int *width, int *height) const;
    // Where a 480x272 image goes in the window (FitRect).
    [[nodiscard]] PresentRect Destination(int src_w, int src_h) const;
    [[nodiscard]] static std::string video_driver();

private:
    SdlVulkanWindow() = default;

    frontend::PresenterConfig config_;
    SDL_Window *window_ = nullptr;
    bool video_initialised_ = false;
};

} // namespace mpo::vk
