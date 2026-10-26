// SPDX-License-Identifier: GPL-3.0-or-later
//
// Buffers, images and the per-frame upload arena, on top of VMA.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "context.hpp"

namespace mpo::vk {

enum class HostAccess : std::uint8_t {
    kNone,       // device-local
    kSequential, // CPU writes (uploads), persistently mapped
    kRandom,     // CPU reads (readback), persistently mapped
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};

bool CreateBuffer(const VulkanContext &ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  HostAccess access, const char *name, Buffer *out, std::string *error);
void DestroyBuffer(const VulkanContext &ctx, Buffer *buffer);
// Makes CPU writes visible (no-op on HOST_COHERENT memory) or GPU writes
// readable (invalidate) on non-coherent memory.
void FlushBuffer(const VulkanContext &ctx, const Buffer &buffer, VkDeviceSize offset,
                 VkDeviceSize size);
void InvalidateBuffer(const VulkanContext &ctx, const Buffer &buffer);

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    int width = 0;
    int height = 0;
};

bool CreateImage2D(const VulkanContext &ctx, int width, int height, VkFormat format,
                   VkImageUsageFlags usage, VkImageAspectFlags aspect, const char *name, Image *out,
                   std::string *error);
void DestroyImage(const VulkanContext &ctx, Image *image);

// One image layout transition with the stages/accesses spelled out.
void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout old_layout, VkImageLayout new_layout,
                  VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                  VkPipelineStageFlags dst_stage, VkAccessFlags dst_access);

// A linear allocator over host-visible chunks, reset once per frame (after
// the frame's fence). Used for vertices, CLUTs (uniform buffer) and staging.
class UploadArena {
public:
    struct Allocation {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void *ptr = nullptr;
    };

    UploadArena() = default;
    UploadArena(const UploadArena &) = delete;
    UploadArena &operator=(const UploadArena &) = delete;

    void Init(const VulkanContext *ctx, VkDeviceSize chunk_size, VkBufferUsageFlags usage,
              const char *name);
    // Returns false (and an error) only when the device runs out of memory.
    bool Allocate(VkDeviceSize size, VkDeviceSize alignment, Allocation *out, std::string *error);
    void FlushAll();
    void Reset();
    void Destroy();
    [[nodiscard]] VkDeviceSize used_bytes() const;

private:
    struct Chunk {
        Buffer buffer;
        VkDeviceSize used = 0;
    };
    const VulkanContext *ctx_ = nullptr;
    VkDeviceSize chunk_size_ = 0;
    VkBufferUsageFlags usage_ = 0;
    std::string name_;
    std::vector<Chunk> chunks_;
    std::size_t current_ = 0;
};

} // namespace mpo::vk
