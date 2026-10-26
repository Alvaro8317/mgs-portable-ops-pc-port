// SPDX-License-Identifier: GPL-3.0-or-later

#include "resources.hpp"

#include <algorithm>

namespace mpo::vk {

bool CreateBuffer(const VulkanContext &ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  HostAccess access, const char *name, Buffer *out, std::string *error) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (access == HostAccess::kSequential) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else if (access == HostAccess::kRandom) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    const VkResult r =
        vmaCreateBuffer(ctx.allocator(), &bci, &aci, &out->buffer, &out->allocation, &info);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vmaCreateBuffer", r);
        *out = Buffer{};
        return false;
    }
    out->mapped = info.pMappedData;
    out->size = size;
    ctx.SetName(VK_OBJECT_TYPE_BUFFER, out->buffer, name);
    return true;
}

void DestroyBuffer(const VulkanContext &ctx, Buffer *buffer) {
    if (buffer->buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(ctx.allocator(), buffer->buffer, buffer->allocation);
    *buffer = Buffer{};
}

void FlushBuffer(const VulkanContext &ctx, const Buffer &buffer, VkDeviceSize offset,
                 VkDeviceSize size) {
    if (buffer.allocation != VK_NULL_HANDLE)
        vmaFlushAllocation(ctx.allocator(), buffer.allocation, offset, size);
}

void InvalidateBuffer(const VulkanContext &ctx, const Buffer &buffer) {
    if (buffer.allocation != VK_NULL_HANDLE)
        vmaInvalidateAllocation(ctx.allocator(), buffer.allocation, 0, VK_WHOLE_SIZE);
}

bool CreateImage2D(const VulkanContext &ctx, int width, int height, VkFormat format,
                   VkImageUsageFlags usage, VkImageAspectFlags aspect, const char *name, Image *out,
                   std::string *error) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkResult r =
        vmaCreateImage(ctx.allocator(), &ici, &aci, &out->image, &out->allocation, nullptr);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vmaCreateImage", r);
        *out = Image{};
        return false;
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    r = vkCreateImageView(ctx.device(), &vci, nullptr, &out->view);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreateImageView", r);
        vmaDestroyImage(ctx.allocator(), out->image, out->allocation);
        *out = Image{};
        return false;
    }
    out->format = format;
    out->aspect = aspect;
    out->width = width;
    out->height = height;
    ctx.SetName(VK_OBJECT_TYPE_IMAGE, out->image, name);
    ctx.SetName(VK_OBJECT_TYPE_IMAGE_VIEW, out->view, name);
    return true;
}

void DestroyImage(const VulkanContext &ctx, Image *image) {
    if (image->view != VK_NULL_HANDLE) vkDestroyImageView(ctx.device(), image->view, nullptr);
    if (image->image != VK_NULL_HANDLE)
        vmaDestroyImage(ctx.allocator(), image->image, image->allocation);
    *image = Image{};
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout old_layout, VkImageLayout new_layout,
                  VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                  VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void UploadArena::Init(const VulkanContext *ctx, VkDeviceSize chunk_size, VkBufferUsageFlags usage,
                       const char *name) {
    ctx_ = ctx;
    chunk_size_ = chunk_size;
    usage_ = usage;
    name_ = name;
}

bool UploadArena::Allocate(VkDeviceSize size, VkDeviceSize alignment, Allocation *out,
                           std::string *error) {
    if (alignment == 0) alignment = 1;
    for (; current_ < chunks_.size(); ++current_) {
        Chunk &c = chunks_[current_];
        const VkDeviceSize offset = (c.used + alignment - 1) / alignment * alignment;
        if (offset + size <= c.buffer.size) {
            c.used = offset + size;
            out->buffer = c.buffer.buffer;
            out->offset = offset;
            out->ptr = static_cast<std::uint8_t *>(c.buffer.mapped) + offset;
            return true;
        }
    }
    Chunk c;
    const VkDeviceSize chunk = std::max(chunk_size_, size);
    if (!CreateBuffer(*ctx_, chunk, usage_, HostAccess::kSequential, name_.c_str(), &c.buffer,
                      error)) {
        return false;
    }
    c.used = size;
    out->buffer = c.buffer.buffer;
    out->offset = 0;
    out->ptr = c.buffer.mapped;
    chunks_.push_back(c);
    current_ = chunks_.size() - 1;
    return true;
}

void UploadArena::FlushAll() {
    for (const Chunk &c : chunks_) {
        if (c.used > 0) FlushBuffer(*ctx_, c.buffer, 0, c.used);
    }
}

void UploadArena::Reset() {
    for (Chunk &c : chunks_) c.used = 0;
    current_ = 0;
}

void UploadArena::Destroy() {
    for (Chunk &c : chunks_) DestroyBuffer(*ctx_, &c.buffer);
    chunks_.clear();
    current_ = 0;
}

VkDeviceSize UploadArena::used_bytes() const {
    VkDeviceSize total = 0;
    for (const Chunk &c : chunks_) total += c.used;
    return total;
}

} // namespace mpo::vk
