// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdl_presenter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mpo::frontend {

SDL_FRect FitRect(int src_w, int src_h, int dst_w, int dst_h, bool integer_scaling) {
    SDL_FRect r{0.0f, 0.0f, 0.0f, 0.0f};
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return r;
    float w = 0.0f;
    float h = 0.0f;
    if (integer_scaling) {
        const int factor = std::max(1, std::min(dst_w / src_w, dst_h / src_h));
        w = static_cast<float>(src_w * factor);
        h = static_cast<float>(src_h * factor);
    } else {
        const float scale = std::min(static_cast<float>(dst_w) / static_cast<float>(src_w),
                                     static_cast<float>(dst_h) / static_cast<float>(src_h));
        w = std::round(static_cast<float>(src_w) * scale);
        h = std::round(static_cast<float>(src_h) * scale);
    }
    r.w = w;
    r.h = h;
    r.x = std::floor((static_cast<float>(dst_w) - w) / 2.0f);
    r.y = std::floor((static_cast<float>(dst_h) - h) / 2.0f);
    return r;
}

std::unique_ptr<SdlPresenter> SdlPresenter::Create(const PresenterConfig &config,
                                                   std::string *error) {
    std::unique_ptr<SdlPresenter> p(new SdlPresenter());
    p->config_ = config;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        if (error) *error = std::string("SDL_InitSubSystem(VIDEO): ") + SDL_GetError();
        return nullptr;
    }
    p->video_initialised_ = true;

    const int scale = std::clamp(config.scale, 1, 16);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (config.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (config.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    p->window_ = SDL_CreateWindow(config.title.c_str(), kPspScreenWidth * scale,
                                  kPspScreenHeight * scale, flags);
    if (p->window_ == nullptr) {
        if (error) *error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return nullptr;
    }
    SDL_SetWindowMinimumSize(p->window_, kPspScreenWidth / 2, kPspScreenHeight / 2);
    p->renderer_ = SDL_CreateRenderer(
        p->window_, config.render_driver.empty() ? nullptr : config.render_driver.c_str());
    if (p->renderer_ == nullptr) {
        if (error) *error = std::string("SDL_CreateRenderer: ") + SDL_GetError();
        return nullptr;
    }
    // Not fatal: some drivers cannot change it.
    SDL_SetRenderVSync(p->renderer_, config.vsync ? 1 : 0);
    return p;
}

SdlPresenter::~SdlPresenter() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    if (video_initialised_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

SDL_WindowID SdlPresenter::window_id() const {
    return SDL_GetWindowID(window_);
}

std::string SdlPresenter::render_driver() const {
    const char *name = SDL_GetRendererName(renderer_);
    return name ? name : "";
}

std::string SdlPresenter::video_driver() {
    const char *name = SDL_GetCurrentVideoDriver();
    return name ? name : "";
}

void SdlPresenter::SetTitle(const std::string &title) {
    SDL_SetWindowTitle(window_, title.c_str());
}

bool SdlPresenter::EnsureTexture(int width, int height) {
    if (texture_ && texture_width_ == width && texture_height_ == height) return true;
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                 width, height);
    if (texture_ == nullptr) {
        last_error_ = std::string("SDL_CreateTexture: ") + SDL_GetError();
        texture_width_ = 0;
        texture_height_ = 0;
        return false;
    }
    SDL_SetTextureScaleMode(texture_,
                            config_.linear_filter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
    texture_width_ = width;
    texture_height_ = height;
    return true;
}

ConvertResult SdlPresenter::Present(const GuestMemory &memory, const FramebufferDesc &desc,
                                    const ConvertOptions &options, WindowCapture *capture) {
    const int w = std::clamp(desc.width, 1, 4096);
    const int h = std::clamp(desc.height, 1, 4096);
    frame_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    FramebufferDesc d = desc;
    d.width = w;
    d.height = h;
    const ConvertResult result = ConvertFramebuffer(memory, d, frame_, options);
    PresentRgba(frame_.data(), w, h, capture);
    return result;
}

bool SdlPresenter::PresentRgba(const std::uint8_t *rgba, int width, int height,
                               WindowCapture *capture) {
    last_error_.clear();
    if (!EnsureTexture(width, height)) return false;
    if (!SDL_UpdateTexture(texture_, nullptr, rgba, width * 4)) {
        last_error_ = std::string("SDL_UpdateTexture: ") + SDL_GetError();
        return false;
    }
    int out_w = 0;
    int out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &out_w, &out_h);
    last_dst_ = FitRect(width, height, out_w, out_h, config_.integer_scaling);

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, nullptr, &last_dst_);

    if (capture) {
        *capture = WindowCapture{};
        SDL_Surface *surface = SDL_RenderReadPixels(renderer_, nullptr);
        SDL_Surface *converted =
            surface ? SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32) : nullptr;
        if (converted) {
            capture->width = converted->w;
            capture->height = converted->h;
            const std::size_t row = static_cast<std::size_t>(converted->w) * 4;
            capture->rgba.resize(row * static_cast<std::size_t>(converted->h));
            for (int y = 0; y < converted->h; ++y) {
                std::memcpy(capture->rgba.data() + row * static_cast<std::size_t>(y),
                            static_cast<const std::uint8_t *>(converted->pixels) +
                                static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(converted->pitch),
                            row);
            }
        } else {
            last_error_ = std::string("SDL_RenderReadPixels: ") + SDL_GetError();
        }
        if (converted) SDL_DestroySurface(converted);
        if (surface) SDL_DestroySurface(surface);
    }
    SDL_RenderPresent(renderer_);
    return last_error_.empty();
}

} // namespace mpo::frontend
