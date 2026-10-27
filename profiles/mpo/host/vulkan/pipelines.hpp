// SPDX-License-Identifier: GPL-3.0-or-later
//
// GE state -> Vulkan graphics pipeline. The key holds everything that changes
// the shader (as specialization constants of the GE ubershader, see
// shaders/ge.frag) or the fixed-function state (blending, depth, write mask,
// topology). Pipelines are created on first use and kept, with a
// VkPipelineCache underneath.
//
// Pipelines that read the destination (shader blending, logic ops, per-bit
// write masks; F15a) use the ge.frag build for the renderer's DstRead method
// and a second descriptor set (set 1: the input attachment or the sampled
// copy of the target).
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
#include <vector>

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
    // Shader blending and write mask (F15a; ge.frag only).
    std::uint32_t blend_fix_src = 0;
    std::uint32_t blend_fix_dst = 0;
    std::uint32_t write_mask = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(PushConstants) == 96);

// How a shader reads the destination pixel (F15a).
enum class DstRead : std::uint8_t {
    kAuto,
    // Input attachment with rasterization order access
    // (VK_EXT_rasterization_order_attachment_access): no barriers, no splits.
    kRasterOrder,
    // Input attachment with a framebuffer-local barrier between groups of
    // primitives that do not overlap (self-dependency).
    kBarrier,
    // A copy of the target, made before each group of primitives that do not
    // overlap. Works on any device.
    kCopy,
};
const char *DstReadName(DstRead mode);

enum class Topology : std::uint8_t {
    kTriangles, // triangles and rectangles (as two triangles)
    kPoints,    // points, and lines rasterized on the CPU as points
};

// Every field is a small integer; padding-free so the bytes are the key.
struct PipelineKey {
    // Fragment shader specialization (ge.frag constant ids 0-26).
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
    // Destination reads (F15a).
    std::uint8_t shader_blend = 0;
    std::uint8_t blend_eq = 0;  // ge::BlendMode
    std::uint8_t blend_src = 2; // ge::PixelBlendFactor
    std::uint8_t blend_dst = 3;
    std::uint8_t logic_op = 3;   // ge::LogicOp; COPY = none
    std::uint8_t write_mask = 0; // per-bit colour mask in the shader
    std::uint8_t read_dst = 0;   // the ge.frag build with a destination read
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
    std::uint8_t reserved[4] = {0, 0, 0, 0};

    bool operator==(const PipelineKey &o) const {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};
static_assert(sizeof(PipelineKey) == 40);
// The first 27 bytes are the fragment shader's specialization constants 0-26.
static_assert(offsetof(PipelineKey, read_dst) == 26);
constexpr std::uint32_t kSpecConstantCount = 27;

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

    // `dst_read` (resolved, not kAuto) picks the ge.frag build and the set 1
    // layout for the pipelines that read the destination, and, on the 1.2
    // path, the render pass.
    // `initial_data`: what Data() returned before (the driver checks it is
    // its own and ignores it otherwise); may be empty.
    bool Init(VulkanContext *ctx, DstRead dst_read, const std::vector<std::uint8_t> &initial_data,
              std::string *error);
    void Destroy();

    // Creates the pipeline on first use. VK_NULL_HANDLE on failure (error).
    VkPipeline Get(const PipelineKey &key, std::string *error);

    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSetLayout set_layout() const { return set_layout_; }
    // Set 0 + set 1 (the destination), for pipelines with read_dst.
    [[nodiscard]] VkPipelineLayout dst_layout() const { return dst_layout_; }
    [[nodiscard]] VkDescriptorSetLayout dst_set_layout() const { return dst_set_layout_; }
    [[nodiscard]] DstRead dst_read() const { return dst_read_; }
    // Render pass the pipelines are compatible with (Vulkan 1.2 path);
    // VK_NULL_HANDLE with dynamic rendering.
    [[nodiscard]] VkRenderPass render_pass() const { return render_pass_; }
    [[nodiscard]] std::size_t size() const { return pipelines_.size(); }
    // The VkPipelineCache contents, to seed a later cache (and skip the
    // driver's shader compiles).
    [[nodiscard]] std::vector<std::uint8_t> Data() const;

private:
    VulkanContext *ctx_ = nullptr;
    VkShaderModule vert_ = VK_NULL_HANDLE;
    VkShaderModule frag_ = VK_NULL_HANDLE;
    VkShaderModule frag_dst_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dst_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineLayout dst_layout_ = VK_NULL_HANDLE;
    DstRead dst_read_ = DstRead::kCopy;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
};

} // namespace mpo::vk
