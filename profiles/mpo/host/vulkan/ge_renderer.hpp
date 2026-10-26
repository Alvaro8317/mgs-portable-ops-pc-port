// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GE on the GPU: a mpo::ge::DrawBackend that rasterizes on Vulkan what
// mpo_ge's primitive assembler produced.
//
// The split is PPSSPP's "software transform": decoding, skinning, matrices,
// lighting, fog depth, UV generation, spline/bezier tessellation, the -Z
// clipper, the guard band, depth clamp, culling and rectangle detection all
// stay in mpo_ge (the reference renderer, F12), so the GPU gets the very same
// screen-space vertices softgpu would rasterize. The GPU does the
// rasterization, texturing (CLUT in the shader), the per-pixel tests, fog,
// dither, blending and the depth buffer.
//
// - Render targets: one RGBA8 image (plus a D16 depth image) per GE colour
//   (depth) buffer address. A target is created on first use from the VRAM
//   contents at that moment; EndFrame(readback) converts what was drawn back
//   into VRAM in the GE format, so VRAM holds what softgpu would have left.
//   Tracking CPU writes to VRAM, render-to-texture and texturing from a
//   target are F15.
// - Textures: level 0, decoded on the CPU (unswizzled; CLUT formats keep the
//   raw index) and cached by address, format, size and a hash of the source
//   bytes. The palette goes to the shader in a uniform buffer.
// - Frames in flight: kFramesInFlight sets of command buffers, fences, upload
//   arenas and descriptor pools. Recording starts with the first draw.
//
// What the GPU path does not reproduce exactly is counted (stats().
// approximations), never dropped silently (CLAUDE.md §5).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "context.hpp"
#include "draw_backend.hpp"
#include "memory_view.hpp"
#include "pipelines.hpp"
#include "resources.hpp"

namespace mpo::vk {

struct RendererStats {
    std::uint64_t frames = 0;
    std::uint64_t draws = 0;
    std::uint64_t batches = 0;
    std::uint64_t triangles = 0;
    std::uint64_t rectangles = 0;
    std::uint64_t points = 0;
    std::uint64_t lines = 0;
    std::uint64_t line_pixels = 0;
    std::uint64_t texture_uploads = 0;
    std::uint64_t texture_hits = 0;
    std::uint64_t clut_uploads = 0;
    std::uint64_t target_uploads = 0;
    std::uint64_t readbacks = 0;
    // What is drawn but not exactly as softgpu would (name -> draws).
    std::map<std::string, std::uint64_t, std::less<>> approximations;
    // Optional sink, one line the first time each approximation appears.
    std::function<void(std::string_view)> log;

    void Approximate(std::string_view what);
};

class GeRenderer final : public ge::DrawBackend {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kTargetHeight = 512;

    // `mem` must outlive the renderer (it is the GE's memory view).
    static std::unique_ptr<GeRenderer> Create(VulkanContext *ctx, const ge::MemoryView &mem,
                                              std::string *error);
    ~GeRenderer() override;
    GeRenderer(const GeRenderer &) = delete;
    GeRenderer &operator=(const GeRenderer &) = delete;

    void BeginDraw(const ge::GeState &gs, const ge::RasterizerState &state) override;
    void DrawTriangle(const ge::VertexData &v0, const ge::VertexData &v1, const ge::VertexData &v2,
                      const ge::BinCoords &range, const ge::RasterizerState &state) override;
    void DrawRectangle(const ge::VertexData &v0, const ge::VertexData &v1,
                       const ge::BinCoords &range, const ge::RasterizerState &state) override;
    void DrawSprite(const ge::VertexData &v0, const ge::VertexData &v1, const ge::BinCoords &range,
                    const ge::RasterizerState &state) override;
    void ClearRectangle(const ge::VertexData &v0, const ge::VertexData &v1,
                        const ge::BinCoords &range, const ge::RasterizerState &state) override;
    void DrawPoint(const ge::VertexData &v0, const ge::BinCoords &range,
                   const ge::RasterizerState &state) override;
    void DrawLine(const ge::VertexData &v0, const ge::VertexData &v1, const ge::BinCoords &range,
                  const ge::RasterizerState &state) override;

    // Submits what was recorded since the last call (nothing to do if no
    // draw came). With `readback`, waits for the GPU and writes every target
    // drawn since the previous readback into VRAM. Returns false on a Vulkan
    // error (also for errors during the draws, see error()).
    bool EndFrame(bool readback, std::string *error);

    struct TargetInfo {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
        int stride = 0;
        ge::BufferFormat format = ge::BufferFormat::k5551;
    };
    // The colour target for the framebuffer at `fb_address` (with or without
    // the VRAM base). Between frames targets stay in
    // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; a user (the presenter) that
    // reads one must transition it and put it back, on the same queue.
    bool FindTarget(std::uint32_t fb_address, TargetInfo *out) const;

    [[nodiscard]] RendererStats &stats() { return stats_; }
    [[nodiscard]] const std::string &error() const { return error_; }
    [[nodiscard]] std::size_t pipeline_count() const { return pipelines_.size(); }
    [[nodiscard]] std::size_t texture_count() const { return textures_.size(); }

private:
    struct DirtyRect {
        int x1 = 1 << 30;
        int y1 = 1 << 30;
        int x2 = -1;
        int y2 = -1;
        [[nodiscard]] bool Empty() const { return x2 < x1 || y2 < y1; }
        void Add(int ax1, int ay1, int ax2, int ay2);
    };
    struct ColorTarget {
        std::uint32_t offset = 0;
        int stride = 0;
        ge::BufferFormat format = ge::BufferFormat::k5551;
        Image image;
        Buffer readback;
        DirtyRect dirty;
    };
    struct DepthTarget {
        std::uint32_t offset = 0;
        int stride = 0;
        int width = 0;
        Image image;
        Buffer readback;
        DirtyRect dirty;
    };
    struct TextureKey {
        std::uint32_t addr = 0;
        std::uint32_t format = 0;
        std::uint32_t size = 0; // w | h << 16
        std::uint32_t bufw = 0;
        std::uint64_t hash = 0;
        bool operator==(const TextureKey &o) const {
            return addr == o.addr && format == o.format && size == o.size && bufw == o.bufw &&
                   hash == o.hash;
        }
    };
    struct TextureKeyHash {
        std::size_t operator()(const TextureKey &k) const;
    };
    struct Texture {
        Image image;
        std::uint64_t last_frame = 0;
    };
    // Everything the primitives of one batch share.
    struct DrawState {
        PipelineKey key;
        PushConstants push;
        int color_target = -1;
        int depth_target = -1;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::uint32_t clut_offset = 0;
        VkRect2D scissor{};
        std::uint32_t blend_constant = 0;
        bool skip = true;
        bool SameAs(const DrawState &o) const;
    };
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer init_cmd = VK_NULL_HANDLE;
        VkCommandBuffer draw_cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;
        UploadArena vertices;
        UploadArena uniforms;
        UploadArena staging;
        std::vector<VkDescriptorPool> desc_pools;
        std::size_t desc_pool_index = 0;
        std::map<std::array<std::uint64_t, 3>, VkDescriptorSet> sets;
        std::vector<Image> dead_images;
        std::uint64_t serial = 0;
    };

    GeRenderer(VulkanContext *ctx, const ge::MemoryView &mem) : ctx_(ctx), mem_(mem) {}
    bool Init(std::string *error);
    void Fail(std::string message);
    bool EnsureFrame();
    Frame &frame() { return frames_[static_cast<std::size_t>(frame_index_)]; }

    int GetColorTarget(std::uint32_t offset, int stride, ge::BufferFormat format);
    int GetDepthTarget(std::uint32_t offset, int stride, int width);
    bool ResolveTexture(const ge::SamplerState &st, VkImageView *rgba, VkImageView *index);
    bool UploadClut(const std::uint8_t *clut, std::uint32_t *offset, VkBuffer *buffer);
    VkDescriptorSet GetDescriptorSet(VkImageView rgba, VkImageView index, VkBuffer clut_buffer);
    bool StageUpload(const void *data, std::size_t size, UploadArena::Allocation *out);

    void SetTopology(Topology topology);
    // Starts a new batch unless `want` matches the current one.
    void UseState(const DrawState &want);
    void Push(const GpuVertex &v) { vertices_.push_back(v); }
    void MarkDirty(const ge::BinCoords &range);
    void MarkDirtyPixel(int x, int y);
    void EmitQuad(const ge::VertexData &v0, const ge::VertexData &v1,
                  const ge::RasterizerState &state);
    void EmitPointPixel(int x, int y, int z, std::uint32_t color0, std::uint32_t color1, float fog,
                        float s, float t, float q);
    void FlushBatch();
    void BeginPass(int color, int depth);
    void EndPass();

    VulkanContext *ctx_;
    const ge::MemoryView &mem_;
    PipelineCache pipelines_;
    std::array<Frame, kFramesInFlight> frames_;
    int frame_index_ = 0;
    bool frame_open_ = false;
    std::uint64_t frame_serial_ = 0;

    VkSampler sampler_ = VK_NULL_HANDLE;
    Image dummy_rgba_;
    Image dummy_index_;
    Buffer dummy_ubo_;
    bool dummies_ready_ = false;

    std::vector<ColorTarget> color_targets_;
    std::vector<DepthTarget> depth_targets_;
    std::unordered_map<std::uint64_t, VkFramebuffer> framebuffers_; // 1.2 path
    std::unordered_map<TextureKey, Texture, TextureKeyHash> textures_;

    // CLUT dedupe within a frame.
    std::array<std::uint8_t, 1024> last_clut_{};
    bool last_clut_valid_ = false;
    std::uint32_t last_clut_offset_ = 0;
    VkBuffer last_clut_buffer_ = VK_NULL_HANDLE;

    DrawState state_;   // of the current batch
    DrawState pending_; // from the last BeginDraw
    std::vector<GpuVertex> vertices_;

    // Command buffer state inside the frame.
    bool in_pass_ = false;
    int pass_color_ = -1;
    int pass_depth_ = -1;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    float viewport_size_ = 4096.0f;

    RendererStats stats_;
    std::string error_;
    bool failed_ = false;
    // MPO_VK_TRACE_DRAWS=1: one log line per draw with its Vulkan state.
    bool trace_draws_ = false;
    // MPO_VK_MAX_DRAWS=N: draw only the first N draws (bisecting a frame).
    std::uint64_t max_draws_ = UINT64_MAX;
};

} // namespace mpo::vk
