// SPDX-License-Identifier: GPL-3.0-or-later

#include "swapchain.hpp"

#include <algorithm>
#include <cstring>

namespace mpo::vk {
namespace {

bool HasFlag(VkFormatFeatureFlags flags, VkFormatFeatureFlags bit) {
    return (flags & bit) == bit;
}

} // namespace

std::unique_ptr<SwapchainPresenter> SwapchainPresenter::Create(VulkanContext *ctx, int width,
                                                               int height, bool vsync,
                                                               std::string *error) {
    std::unique_ptr<SwapchainPresenter> p(new SwapchainPresenter(ctx));
    if (!p->Init(width, height, vsync, error)) return nullptr;
    return p;
}

bool SwapchainPresenter::Init(int width, int height, bool vsync, std::string *error) {
    if (ctx_->surface() == VK_NULL_HANDLE) {
        if (error) *error = "the Vulkan context has no surface";
        return false;
    }
    want_width_ = width;
    want_height_ = height;
    VkPhysicalDevice phys = ctx_->physical_device();
    VkSurfaceKHR surface = ctx_->surface();

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, formats.data());
    formats.resize(count);
    if (formats.empty()) {
        if (error) *error = "the surface has no formats";
        return false;
    }
    // UNORM, not SRGB: the PSP framebuffer values go to the screen as they
    // are, like on the PSP and in PPSSPP.
    format_ = VK_FORMAT_UNDEFINED;
    for (const VkSurfaceFormatKHR &f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format_ = f.format;
            color_space_ = f.colorSpace;
            break;
        }
    }
    if (format_ == VK_FORMAT_UNDEFINED) {
        format_ = formats[0].format;
        color_space_ = formats[0].colorSpace;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys, format_, &fp);
    if (!HasFlag(fp.optimalTilingFeatures, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        if (error) *error = "the swapchain format does not support blits";
        return false;
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &count, modes.data());
    modes.resize(count);
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                present_mode_ = m;
                break;
            }
        }
    }

    VkDevice dev = ctx_->device();
    frames_.resize(kFramesInFlight);
    for (Frame &f : frames_) {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx_->queue_family();
        VkResult r = vkCreateCommandPool(dev, &pci, nullptr, &f.pool);
        if (r != VK_SUCCESS) {
            if (error) *error = ResultError("vkCreateCommandPool", r);
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cai, &f.cmd);
        ctx_->SetName(VK_OBJECT_TYPE_COMMAND_BUFFER, f.cmd, "present");
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(dev, &fci, nullptr, &f.fence);
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        r = vkCreateSemaphore(dev, &sci, nullptr, &f.acquired);
        if (r != VK_SUCCESS) {
            if (error) *error = ResultError("vkCreateSemaphore", r);
            return false;
        }
    }
    return CreateSwapchain(error);
}

bool SwapchainPresenter::CreateSwapchain(std::string *error) {
    VkPhysicalDevice phys = ctx_->physical_device();
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, ctx_->surface(), &caps);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
        return false;
    }
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp(static_cast<std::uint32_t>(std::max(want_width_, 1)),
                                   caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<std::uint32_t>(std::max(want_height_, 1)),
                                    caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) {
        // Minimised: try again on the next present.
        needs_recreate_ = true;
        return true;
    }
    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) image_count = std::min(image_count, caps.maxImageCount);
    can_capture_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & alpha) == 0) {
        for (VkCompositeAlphaFlagBitsKHR a :
             {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if ((caps.supportedCompositeAlpha & a) != 0) {
                alpha = a;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = ctx_->surface();
    sci.minImageCount = image_count;
    sci.imageFormat = format_;
    sci.imageColorSpace = color_space_;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (can_capture_) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageUsage = usage;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = present_mode_;
    sci.clipped = VK_TRUE;
    r = vkCreateSwapchainKHR(ctx_->device(), &sci, nullptr, &swapchain_);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreateSwapchainKHR", r);
        return false;
    }
    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, images_.data());
    rendered_.resize(n, VK_NULL_HANDLE);
    for (VkSemaphore &s : rendered_) {
        VkSemaphoreCreateInfo semi{};
        semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(ctx_->device(), &semi, nullptr, &s);
    }
    needs_recreate_ = false;
    return true;
}

void SwapchainPresenter::DestroySwapchain() {
    VkDevice dev = ctx_->device();
    for (VkSemaphore s : rendered_) vkDestroySemaphore(dev, s, nullptr);
    rendered_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

SwapchainPresenter::~SwapchainPresenter() {
    VkDevice dev = ctx_->device();
    vkDeviceWaitIdle(dev);
    DestroySwapchain();
    for (Frame &f : frames_) {
        if (f.acquired) vkDestroySemaphore(dev, f.acquired, nullptr);
        if (f.fence) vkDestroyFence(dev, f.fence, nullptr);
        if (f.pool) vkDestroyCommandPool(dev, f.pool, nullptr);
        DestroyBuffer(*ctx_, &f.upload);
    }
    DestroyBuffer(*ctx_, &capture_buffer_);
    DestroyImage(*ctx_, &upload_image_);
}

void SwapchainPresenter::Resize(int width, int height) {
    if (width == want_width_ && height == want_height_) return;
    want_width_ = width;
    want_height_ = height;
    needs_recreate_ = true;
}

bool SwapchainPresenter::Present(VkImage src, int src_w, int src_h, const PresentRect &dst,
                                 bool linear, std::vector<std::uint8_t> *capture,
                                 std::string *error) {
    Source source;
    source.image = src;
    source.width = src_w;
    source.height = src_h;
    return PresentImpl(source, dst, linear, capture, error);
}

bool SwapchainPresenter::PresentRgba(const std::uint8_t *rgba, int width, int height,
                                     const PresentRect &dst, bool linear,
                                     std::vector<std::uint8_t> *capture, std::string *error) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        if (error) *error = "PresentRgba: no pixels";
        return false;
    }
    Source source;
    source.pixels = rgba;
    source.width = width;
    source.height = height;
    return PresentImpl(source, dst, linear, capture, error);
}

bool SwapchainPresenter::EnsureUpload(Frame &f, int width, int height, std::string *error) {
    if (upload_image_.width != width || upload_image_.height != height) {
        vkDeviceWaitIdle(ctx_->device());
        DestroyImage(*ctx_, &upload_image_);
        upload_initialised_ = false;
        if (!CreateImage2D(*ctx_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                           // SAMPLED only for the view CreateImage2D makes.
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, "present upload", &upload_image_, error)) {
            return false;
        }
    }
    const auto bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    if (f.upload.size < bytes) {
        DestroyBuffer(*ctx_, &f.upload);
        if (!CreateBuffer(*ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostAccess::kSequential,
                          "present upload", &f.upload, error)) {
            return false;
        }
    }
    return true;
}

bool SwapchainPresenter::PresentImpl(const Source &source, const PresentRect &dst, bool linear,
                                     std::vector<std::uint8_t> *capture, std::string *error) {
    VkDevice dev = ctx_->device();
    if (needs_recreate_) {
        vkDeviceWaitIdle(dev);
        for (Frame &f : frames_) {
            if (f.pending) {
                vkResetFences(dev, 1, &f.fence);
                f.pending = false;
            }
        }
        DestroySwapchain();
        if (!CreateSwapchain(error)) return false;
        if (swapchain_ == VK_NULL_HANDLE) return true; // minimised
    }
    Frame &f = frames_[static_cast<std::size_t>(frame_index_)];
    if (f.pending) {
        VkResult r = vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) {
            if (error) *error = ResultError("vkWaitForFences", r);
            return false;
        }
        vkResetFences(dev, 1, &f.fence);
        f.pending = false;
    }

    std::uint32_t index = 0;
    VkResult r =
        vkAcquireNextImageKHR(dev, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_recreate_ = true;
        return true;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        needs_recreate_ = true;
    } else if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkAcquireNextImageKHR", r);
        return false;
    }
    const VkImage image = images_[index];
    const bool upload = source.pixels != nullptr;
    if (upload) {
        if (!EnsureUpload(f, source.width, source.height, error)) return false;
        std::memcpy(f.upload.mapped, source.pixels,
                    static_cast<std::size_t>(source.width) *
                        static_cast<std::size_t>(source.height) * 4);
        FlushBuffer(*ctx_, f.upload, 0, VK_WHOLE_SIZE);
    }
    const VkImage src = upload ? upload_image_.image : source.image;
    const int src_w = source.width;
    const int src_h = source.height;
    const bool do_capture = capture != nullptr && can_capture_;
    const VkDeviceSize capture_bytes =
        static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    if (do_capture && capture_buffer_.size < capture_bytes) {
        DestroyBuffer(*ctx_, &capture_buffer_);
        if (!CreateBuffer(*ctx_, capture_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          HostAccess::kRandom, "swapchain capture", &capture_buffer_, error)) {
            return false;
        }
    }

    vkResetCommandPool(dev, f.pool, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmd = f.cmd;
    vkBeginCommandBuffer(cmd, &bi);
    ctx_->BeginLabel(cmd, "present");
    if (upload) {
        // The upload image: after the blit of the previous present.
        ImageBarrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT,
                     upload_initialised_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                         : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(src_w), static_cast<std::uint32_t>(src_h),
                            1};
        vkCmdCopyBufferToImage(cmd, f.upload.buffer, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);
        ImageBarrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        upload_initialised_ = true;
    } else {
        ImageBarrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
    }
    ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    // Clear, then blit: two transfer writes to the same image.
    ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);

    const int x0 = std::clamp(dst.x, 0, static_cast<int>(extent_.width));
    const int y0 = std::clamp(dst.y, 0, static_cast<int>(extent_.height));
    const int x1 = std::clamp(dst.x + dst.width, 0, static_cast<int>(extent_.width));
    const int y1 = std::clamp(dst.y + dst.height, 0, static_cast<int>(extent_.height));
    if (x1 > x0 && y1 > y0 && src_w > 0 && src_h > 0) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {src_w, src_h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x1, y1, 1};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    if (do_capture) {
        ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture_buffer_.buffer, 1, &copy);
        ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &mb, 0, nullptr, 0, nullptr);
    } else {
        ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    if (!upload) {
        ImageBarrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    ctx_->EndLabel(cmd);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    r = vkQueueSubmit(ctx_->queue(), 1, &si, f.fence);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkQueueSubmit", r);
        return false;
    }
    f.pending = true;

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(ctx_->queue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        needs_recreate_ = true;
    } else if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkQueuePresentKHR", r);
        return false;
    }

    if (do_capture) {
        vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &f.fence);
        f.pending = false;
        InvalidateBuffer(*ctx_, capture_buffer_);
        capture->resize(static_cast<std::size_t>(capture_bytes));
        std::memcpy(capture->data(), capture_buffer_.mapped, capture->size());
        if (format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB) {
            for (std::size_t i = 0; i + 3 < capture->size(); i += 4)
                std::swap((*capture)[i], (*capture)[i + 2]);
        }
    } else if (capture) {
        capture->clear();
    }
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    return true;
}

} // namespace mpo::vk
