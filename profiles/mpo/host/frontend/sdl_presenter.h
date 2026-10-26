// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL3 window that shows the PSP display framebuffer: the pixels are
// converted on the CPU (framebuffer.h), uploaded to a streaming RGBA texture
// and drawn scaled to the window, keeping the 480x272 aspect ratio
// (letterbox) or with whole multiples only (integer scaling). Point sampling
// by default.
//
// Not thread-safe: create it and call it from the thread that runs SDL's
// event loop (the main thread on most platforms).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "framebuffer.h"

namespace mpo::frontend {

struct PresenterConfig {
    std::string title = "MGS: Portable Ops";
    // Initial window size: 480x272 times this.
    int scale = 2;
    bool fullscreen = false;
    bool hidden = false;
    bool integer_scaling = false;
    bool linear_filter = false;
    bool vsync = false;
    // SDL render driver ("software", "opengl", "vulkan"...); empty lets SDL pick.
    std::string render_driver;
};

// The window contents as drawn (back buffer), read before presenting.
struct WindowCapture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, R G B A
};

class SdlPresenter {
public:
    // Initialises SDL's video subsystem (reference counted by SDL) and opens
    // the window. Returns null and fills `error` on failure.
    static std::unique_ptr<SdlPresenter> Create(const PresenterConfig &config, std::string *error);
    ~SdlPresenter();
    SdlPresenter(const SdlPresenter &) = delete;
    SdlPresenter &operator=(const SdlPresenter &) = delete;

    // Converts the framebuffer and shows it. With `capture`, the window
    // contents are read back before the swap.
    ConvertResult Present(const GuestMemory &memory, const FramebufferDesc &desc,
                          const ConvertOptions &options = {}, WindowCapture *capture = nullptr);
    // Shows an already converted RGBA8 image.
    bool PresentRgba(const std::uint8_t *rgba, int width, int height,
                     WindowCapture *capture = nullptr);

    // The last converted frame (width * height * 4).
    [[nodiscard]] const std::vector<std::uint8_t> &last_frame() const { return frame_; }
    [[nodiscard]] SDL_Window *window() const { return window_; }
    [[nodiscard]] SDL_WindowID window_id() const;
    [[nodiscard]] std::string render_driver() const;
    [[nodiscard]] static std::string video_driver();
    // Where the image goes in the window, in pixels (last present).
    [[nodiscard]] SDL_FRect last_destination() const { return last_dst_; }
    [[nodiscard]] const std::string &last_error() const { return last_error_; }

    void SetTitle(const std::string &title);

private:
    SdlPresenter() = default;
    bool EnsureTexture(int width, int height);

    PresenterConfig config_;
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    int texture_width_ = 0;
    int texture_height_ = 0;
    bool video_initialised_ = false;
    std::vector<std::uint8_t> frame_;
    SDL_FRect last_dst_{};
    std::string last_error_;
};

// Destination rectangle for a `src_w` x `src_h` image in a `dst_w` x `dst_h`
// output: centred, aspect kept, and with `integer_scaling` a whole multiple
// (at least 1x, cropped if the window is smaller).
[[nodiscard]] SDL_FRect FitRect(int src_w, int src_h, int dst_w, int dst_h, bool integer_scaling);

} // namespace mpo::frontend
