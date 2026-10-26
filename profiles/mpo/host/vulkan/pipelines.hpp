// SPDX-License-Identifier: GPL-3.0-or-later
//
// GE state -> Vulkan graphics pipeline. The key holds everything that changes
// the shader (as specialization constants of the GE ubershader, see
// shaders/ge.frag) or the fixed-function state (blending, depth, write mask,
// topology). Pipelines are created on first use and kept, with a
// VkPipelineCache underneath.
//
// Vertex layout (one binding, interleaved), produced on the CPU from mpo_ge's
// VertexData: position (x, y in pixels, z in [0, 1], w), (s, t, q), fog
// depth, colour 0 and colour 1 (RGBA8 UNORM).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "context.hpp"

namespace mpo::vk {

struct GpuVertex {
    float x = 0;
    float y = 0;
    float z = 0;
    float w = 1;
    float s = 0;
    float t = 0;
    float q = 1;
    float fog = 1;
    std::uint32_t color0 = 0;
    std::uint32_t color1 = 0;
};
static_assert(sizeof(GpuVertex) == 40);

// Push constants (shaders/ge.vert and ge.frag, same block in both).
struct PushConstants {
    float ndc_scale[2] = {0, 0};
    float uv_scale[2] = {1, 1};
    std::uint32_t tex_size = 0;
    std::uint32_t clut_params = 0;
    std::uint32_t alpha_test = 0xFF00;
    std::uint32_t color_ref = 0;
    std::uint32_t color_mask = 0;
    std::uint32_t fog_color = 0;
    std::uint32_t env_color = 0;
    std::uint32_t depth_range = 0xFFFF0000u;
    std::int32_t lod_offset = 0;
    float lod_slope = 0;
    float ndc_origin[2] = {0, 0};
    std::uint32_t dither[4] = {0, 0, 0, 0};
};
static_assert(sizeof(PushConstants) == 80);

enum class Topology : std::uint8_t {
    kTriangles, // triangles and rectangles (as two triangles)
    kPoints,    // points, and lines rasterized on the CPU as points
};

// Every field is a small integer; padding-free so the bytes are the key.
struct PipelineKey {
    // Fragment shader specialization (ge.frag constant ids 0-19).
    std::uint8_t tex_mode = 0;
    std::uint8_t clut_format = 3;
    std::uint8_t tex_func = 0;
    std::uint8_t tex_alpha = 0;
    std::uint8_t doubling = 0;
    std::uint8_t mag_linear = 0;
    std::uint8_t min_linear = 0;
    std::uint8_t clamp_s = 0;
    std::uint8_t clamp_t = 0;
    std::uint8_t tex_proj = 0;
    std::uint8_t lod_mode = 0;
    std::uint8_t alpha_func = 1;
    std::uint8_t color_func = 1;
    std::uint8_t fog = 0;
    std::uint8_t clear = 0;
    std::uint8_t fb_format = 3;
    std::uint8_t dither = 0;
    std::uint8_t quantize = 0;
    std::uint8_t depth_range = 0;
    std::uint8_t secondary = 0;
    // Fixed function (Vulkan enum values).
    std::uint8_t blend = 0;
    std::uint8_t blend_op = 0;
    std::uint8_t src_factor = 0;
    std::uint8_t dst_factor = 0;
    std::uint8_t depth_test = 0;
    std::uint8_t depth_compare = 0;
    std::uint8_t depth_write = 0;
    std::uint8_t color_write_mask = 0xF;
    Topology topology = Topology::kTriangles;
    std::uint8_t reserved[3] = {0, 0, 0};

    bool operator==(const PipelineKey &o) const {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};
static_assert(sizeof(PipelineKey) == 32);
// The first 20 bytes are the fragment shader's specialization constants 0-19.
static_assert(offsetof(PipelineKey, secondary) == 19);

struct PipelineKeyHash {
    std::size_t operator()(const PipelineKey &k) const;
};

class PipelineCache {
public:
    static constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D16_UNORM;

    PipelineCache() = default;
    ~PipelineCache();
    PipelineCache(const PipelineCache &) = delete;
    PipelineCache &operator=(const PipelineCache &) = delete;

    bool Init(VulkanContext *ctx, std::string *error);
    void Destroy();

    // Creates the pipeline on first use. VK_NULL_HANDLE on failure (error).
    VkPipeline Get(const PipelineKey &key, std::string *error);

    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSetLayout set_layout() const { return set_layout_; }
    // Render pass the pipelines are compatible with (Vulkan 1.2 path);
    // VK_NULL_HANDLE with dynamic rendering.
    [[nodiscard]] VkRenderPass render_pass() const { return render_pass_; }
    [[nodiscard]] std::size_t size() const { return pipelines_.size(); }

private:
    VulkanContext *ctx_ = nullptr;
    VkShaderModule vert_ = VK_NULL_HANDLE;
    VkShaderModule frag_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
};

} // namespace mpo::vk
