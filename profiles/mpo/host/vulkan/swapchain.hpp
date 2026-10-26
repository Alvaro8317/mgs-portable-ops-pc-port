// SPDX-License-Identifier: GPL-3.0-or-later
//
// Presentation of a GE render target on a window surface: a swapchain with
// frames in flight, and a blit (scaled, letterboxed by the caller's
// rectangle) from the target image to the swapchain image. It knows nothing
// about the window system: the surface comes from the VulkanContext (created
// by SDL in sdl_window.cpp), the drawable size from the caller.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "context.hpp"
#include "resources.hpp"

namespace mpo::vk {

struct PresentRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class SwapchainPresenter {
public:
    static constexpr int kFramesInFlight = 2;

    // `ctx` must have a surface. `vsync` picks FIFO; otherwise MAILBOX or
    // IMMEDIATE when available.
    static std::unique_ptr<SwapchainPresenter> Create(VulkanContext *ctx, int width, int height,
                                                      bool vsync, std::string *error);
    ~SwapchainPresenter();
    SwapchainPresenter(const SwapchainPresenter &) = delete;
    SwapchainPresenter &operator=(const SwapchainPresenter &) = delete;

    // The window's drawable size changed (the next Present recreates).
    void Resize(int width, int height);

    // Shows the `src_w` x `src_h` top-left corner of `src` (an RGBA8 image in
    // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL with TRANSFER_SRC usage, left in
    // that layout) inside `dst` of the swapchain image, black around it.
    // With `capture`, the swapchain image is read back (RGBA8, before
    // presenting) into it. Returns false on a Vulkan error.
    bool Present(VkImage src, int src_w, int src_h, const PresentRect &dst, bool linear,
                 std::vector<std::uint8_t> *capture, std::string *error);

    [[nodiscard]] int width() const { return static_cast<int>(extent_.width); }
    [[nodiscard]] int height() const { return static_cast<int>(extent_.height); }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkPresentModeKHR present_mode() const { return present_mode_; }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        bool pending = false;
    };

    explicit SwapchainPresenter(VulkanContext *ctx) : ctx_(ctx) {}
    bool Init(int width, int height, bool vsync, std::string *error);
    bool CreateSwapchain(std::string *error);
    void DestroySwapchain();

    VulkanContext *ctx_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};
    int want_width_ = 0;
    int want_height_ = 0;
    bool needs_recreate_ = false;
    bool can_capture_ = false;
    std::vector<VkImage> images_;
    // Signalled by the blit, waited by the present: one per swapchain image,
    // so a semaphore is never reused while the presentation engine holds it.
    std::vector<VkSemaphore> rendered_;
    std::vector<Frame> frames_;
    int frame_index_ = 0;
    Buffer capture_buffer_;
};

} // namespace mpo::vk
