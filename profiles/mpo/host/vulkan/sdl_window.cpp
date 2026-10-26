// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdl_window.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>

#include "framebuffer.h"

namespace mpo::vk {

std::unique_ptr<SdlVulkanWindow> SdlVulkanWindow::Create(const frontend::PresenterConfig &config,
                                                         std::string *error) {
    std::unique_ptr<SdlVulkanWindow> w(new SdlVulkanWindow());
    w->config_ = config;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        if (error) *error = std::string("SDL_InitSubSystem(VIDEO): ") + SDL_GetError();
        return nullptr;
    }
    w->video_initialised_ = true;
    const int scale = std::clamp(config.scale, 1, 16);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN;
    if (config.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (config.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    w->window_ = SDL_CreateWindow(config.title.c_str(), frontend::kPspScreenWidth * scale,
                                  frontend::kPspScreenHeight * scale, flags);
    if (w->window_ == nullptr) {
        if (error) *error = std::string("SDL_CreateWindow(VULKAN): ") + SDL_GetError();
        return nullptr;
    }
    SDL_SetWindowMinimumSize(w->window_, frontend::kPspScreenWidth / 2,
                             frontend::kPspScreenHeight / 2);
    return w;
}

SdlVulkanWindow::~SdlVulkanWindow() {
    if (window_) SDL_DestroyWindow(window_);
    if (video_initialised_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void SdlVulkanWindow::FillContextOptions(ContextOptions *options) const {
    Uint32 count = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
    for (Uint32 i = 0; names != nullptr && i < count; ++i)
        options->instance_extensions.emplace_back(names[i]);
    options->get_instance_proc_addr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    SDL_Window *window = window_;
    options->create_surface = [window](VkInstance instance) -> VkSurfaceKHR {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            return VK_NULL_HANDLE;
        }
        return surface;
    };
}

void SdlVulkanWindow::DrawableSize(int *width, int *height) const {
    if (!SDL_GetWindowSizeInPixels(window_, width, height)) {
        *width = 0;
        *height = 0;
    }
}

PresentRect SdlVulkanWindow::Destination(int src_w, int src_h) const {
    int w = 0;
    int h = 0;
    DrawableSize(&w, &h);
    const SDL_FRect r = frontend::FitRect(src_w, src_h, w, h, config_.integer_scaling);
    return PresentRect{static_cast<int>(std::lround(r.x)), static_cast<int>(std::lround(r.y)),
                       static_cast<int>(std::lround(r.w)), static_cast<int>(std::lround(r.h))};
}

std::string SdlVulkanWindow::video_driver() {
    const char *name = SDL_GetCurrentVideoDriver();
    return name ? name : "";
}

} // namespace mpo::vk
