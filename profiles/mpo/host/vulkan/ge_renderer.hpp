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
// - Blending (F15a): softgpu's integer blend, the dither after it, the logic
//   op and the per-bit colour mask run in the fragment shader, which reads the
//   destination pixel (DstRead in pipelines.hpp: an input attachment with
//   rasterization order access, an input attachment with barriers between
//   groups of primitives that do not overlap, or a copy of the target per
//   such group). Fixed-function blending stays as a fast path where it gives
//   the same bits (RendererOptions::blend).
//
// - Render targets: one RGBA8 image (plus a D16 depth image) per GE colour
//   (depth) buffer address. A target is created on first use from the VRAM
//   contents at that moment; EndFrame(readback) converts what was drawn back
//   into VRAM in the GE format, so VRAM holds what softgpu would have left.
//   F14b: each target keeps a copy of the VRAM bytes it was last uploaded
//   from or read back to; at its first use in a frame, the rows of VRAM that
//   no longer match (the CPU wrote them: video frames, memcpy) are uploaded
//   again. With EndFrame(true) after every list run (mpo_boot), the game sees
//   VRAM as softgpu would leave it between runs.
// - Within a frame (F15b): before something reads or overwrites VRAM rows
//   that a target drew and has not read back yet, the frame ends there with a
//   readback (stats().hazard_flushes), so VRAM is again what softgpu would
//   hold at that point. That covers a texture or a CLUT taken from a target
//   (render-to-texture, texturing from the current target), block transfers
//   from or onto a target (SyncMemory) and a target that overlaps rows
//   another one drew (the same VRAM seen with another format or stride).
// - Textures: level 0, decoded on the CPU (unswizzled; CLUT formats keep the
//   raw index) and cached by address, format, size and a hash of the source
//   bytes. The palette goes to the shader in a uniform buffer.
// - Replaced textures (F24c, SamplerState::replaced): the replacement's own
//   image (RGBA8, at the PNG's size), cached by its id, sampled with the
//   same UV mapping; at another size than the GE texture (an HD pack) the
//   texel coordinates, clamp and wrap work at the image's size
//   (PushConstants::samp_size), the level of detail at the texture's.
//   F26a: with mip levels (the pack's or generated), the image has them all
//   and the shader picks one with softgpu's rule, in the replacement's own
//   texels and per image pixel (PushConstants::samp_mips); a [filtering]
//   entry arrives as the RasterizerState's filters.
// - Frames in flight: kFramesInFlight sets of command buffers, fences, upload
//   arenas and descriptor pools. Recording starts with the first draw.
// - Render scale (F23b, RendererOptions::render_scale): the targets are N
//   times the GE's size and the GPU rasterizes at that resolution (viewport,
//   scissor, points and line pixels as N x N blocks), with its own
//   interpolation (the exact path is softgpu's at 1x only). Everything that
//   goes back to VRAM comes down to 1x: the readback takes, from each N x N
//   block, the sample (s, s), s = (N - 1) / 2, and triangle vertices are
//   shifted so that this sample sits where softgpu samples the pixel
//   (16x + 7). Rectangles are not shifted: a half-pixel shift (their sample
//   point is the centre, between two samples at even N) would move every
//   sprite by one image pixel and leave the last column and row uncovered;
//   their edges go to the GE pixel edges that cover softgpu's pixels, and at
//   an even N the sample pixel alone takes its UV at the pixel centre
//   (PushConstants::rect_sample). Uploads from VRAM fill each block with its
//   pixel. So the game, the hazard readbacks, render-to-
//   texture and the captures see PSP resolution; only the presenter shows
//   the N x target.
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
#include <unordered_set>
#include <utility>
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
    // F15b: triangles without area that softgpu still draws (the exact
    // path covers their range).
    std::uint64_t zero_area_triangles = 0;
    // ... and those with no pixel to draw (not sent to the GPU).
    std::uint64_t zero_area_skipped = 0;
    std::uint64_t texture_uploads = 0;
    std::uint64_t texture_hits = 0;
    std::uint64_t clut_uploads = 0;
    std::uint64_t target_uploads = 0;
    // F14b: targets uploaded again at their first use in a frame because VRAM
    // changed under them, and the rows uploaded.
    std::uint64_t target_resyncs = 0;
    std::uint64_t target_resync_rows = 0;
    std::uint64_t readbacks = 0;
    // F15b: host time EndFrame(true) spends waiting for the GPU and writing
    // the targets back into VRAM (microseconds).
    std::uint64_t readback_wait_us = 0;
    std::uint64_t readback_write_us = 0;
    // Destination reads (F15a): batches that read it, barriers inside a
    // render pass between groups of primitives, target copies.
    std::uint64_t dst_batches = 0;
    std::uint64_t dst_barriers = 0;
    std::uint64_t dst_copies = 0;
    std::uint64_t fixed_blend_batches = 0;
    // F15b: frames ended early, with a readback, because something was about
    // to read or overwrite VRAM a target had drawn (reason -> count).
    std::map<std::string, std::uint64_t, std::less<>> hazard_flushes;
    // What is drawn but not exactly as softgpu would (name -> draws).
    std::map<std::string, std::uint64_t, std::less<>> approximations;
    // Optional sink, one line the first time each approximation appears.
    std::function<void(std::string_view)> log;

    void Approximate(std::string_view what);
};

// Where blending happens (MPO_VK_BLEND).
enum class BlendPath : std::uint8_t {
    // Fixed function where it is exact (no rounding to differ: factors 0 and
    // 1, MIN and MAX, no dither), the shader elsewhere. "auto", the default.
    kAuto,
    // Every blended draw in the shader. "shader".
    kShader,
    // F14a: fixed function wherever it fits, approximating softgpu's rounding
    // (counted in stats().approximations). "fixed"; for A/B comparisons.
    kFixed,
};
const char *BlendPathName(BlendPath path);

// A/B toggles (CLAUDE.md §7). FromEnv() reads them from the environment:
//   MPO_VK_BLEND=auto|shader|fixed      blending path
//   MPO_VK_DST_READ=auto|raster|barrier|copy  how the shader reads the
//                                       destination (auto: the first the
//                                       device supports, in that order)
//   MPO_VK_NO_BATCH=1                   one vkCmdDraw per GE draw (no merging
//                                       of consecutive draws with one state)
//   MPO_VK_NO_TEX_CACHE=1               decode and upload every texture on
//                                       every use
//   MPO_VK_NO_EXACT=1                   F15b: triangles interpolated by the
//                                       GPU (as before F15b) instead of
//                                       softgpu's integer edge weights
//   MPO_VK_NO_HAZARD_FLUSH=1            F15b: no early readback when a list
//                                       reads or overwrites what a target
//                                       drew (VRAM is then stale until the
//                                       end of the run, as before F15b)
//   MPO_RENDER_SCALE=N                  F23b: render at N x 480x272, N = 1..8
//                                       (anything else is 1)
// (the device toggles MPO_VK_NO_13, MPO_VK_NO_DEPTH_CLAMP,
// MPO_VK_NO_RASTER_ORDER and MPO_VK_NO_LOCAL_READ are read by the context).
struct RendererOptions {
    BlendPath blend = BlendPath::kAuto;
    DstRead dst_read = DstRead::kAuto;
    bool batching = true;
    bool texture_cache = true;
    bool hazard_flush = true;
    bool exact = true;
    // F23b: 1 (the PSP's resolution) to GeRenderer::kMaxRenderScale. The
    // renderer lowers it when the device cannot (render_scale()).
    int render_scale = 1;
    // Seed for the VkPipelineCache (GeRenderer::PipelineCacheData of an
    // earlier renderer on the same device); not read from the environment.
    std::vector<std::uint8_t> pipeline_cache_data;

    static RendererOptions FromEnv();
};

class GeRenderer final : public ge::DrawBackend {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kTargetHeight = 512;
    static constexpr int kMaxRenderScale = 8;

    // `mem` must outlive the renderer (it is the GE's memory view). The
    // options come from the environment (RendererOptions::FromEnv).
    static std::unique_ptr<GeRenderer> Create(VulkanContext *ctx, const ge::MemoryView &mem,
                                              std::string *error);
    // Fails when options.dst_read asks for a method the device lacks.
    static std::unique_ptr<GeRenderer> Create(VulkanContext *ctx, const ge::MemoryView &mem,
                                              const RendererOptions &options, std::string *error);
    // Whether the device (and the path it uses, 1.3 or 1.2) supports `mode`.
    static bool Supports(const VulkanContext &ctx, DstRead mode);
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
    // F15b: a CLUT load or a block transfer is about to read (or write)
    // memory; ends the frame with a readback first when that memory holds
    // rows a target drew (or, for a write, rows of a target in use).
    void SyncMemory(std::uint32_t addr, std::uint32_t size, bool write) override;

    // Submits what was recorded since the last call (nothing to do if no
    // draw came). With `readback`, waits for the GPU and writes every target
    // drawn since the previous readback into VRAM. Returns false on a Vulkan
    // error (also for errors during the draws, see error()).
    bool EndFrame(bool readback, std::string *error);

    struct TargetInfo {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        // The image's size: `scale` times the target's size in GE pixels.
        int width = 0;
        int height = 0;
        int stride = 0;
        ge::BufferFormat format = ge::BufferFormat::k5551;
        int scale = 1;
    };
    // The colour target for the framebuffer at `fb_address` (with or without
    // the VRAM base). Between frames targets stay in
    // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; a user (the presenter) that
    // reads one must transition it and put it back, on the same queue.
    bool FindTarget(std::uint32_t fb_address, TargetInfo *out) const;
    // The one with this stride and format (several targets can share an
    // address, F15b).
    bool FindTarget(std::uint32_t fb_address, int stride, ge::BufferFormat format,
                    TargetInfo *out) const;
    // Whether the colour target of the framebuffer at `fb_address` (this
    // stride and format) holds what VRAM holds in the first `width` x `rows`
    // pixels: no frame open, nothing drawn since the last readback and no CPU
    // write since. Then a presenter can show the target instead of VRAM
    // (F14b).
    [[nodiscard]] bool TargetMatchesVram(std::uint32_t fb_address, int stride,
                                         ge::BufferFormat format, int width, int rows) const;

    [[nodiscard]] RendererStats &stats() { return stats_; }
    [[nodiscard]] const std::string &error() const { return error_; }
    [[nodiscard]] std::size_t pipeline_count() const { return pipelines_.size(); }
    [[nodiscard]] std::vector<std::uint8_t> PipelineCacheData() const { return pipelines_.Data(); }
    [[nodiscard]] std::size_t texture_count() const { return textures_.size(); }
    [[nodiscard]] const RendererOptions &options() const { return options_; }
    // The method in use (never kAuto).
    [[nodiscard]] DstRead dst_read() const { return dst_read_; }
    // The render scale in use (F23b): options().render_scale, clamped to
    // 1..kMaxRenderScale and lowered to what the device supports.
    [[nodiscard]] int render_scale() const { return scale_; }

private:
    struct DirtyRect {
        int x1 = 1 << 30;
        int y1 = 1 << 30;
        int x2 = -1;
        int y2 = -1;
        [[nodiscard]] bool Empty() const { return x2 < x1 || y2 < y1; }
        void Add(int ax1, int ay1, int ax2, int ay2);
    };
    // The VRAM bytes under a target (row by row, `row_bytes` each) as of its
    // last upload or readback, and the frame it was last compared in.
    struct VramShadow {
        std::vector<std::uint8_t> bytes;
        int row_bytes = 0;
        std::uint64_t checked_frame = 0;
    };
    struct ColorTarget {
        std::uint32_t offset = 0;
        int stride = 0;
        ge::BufferFormat format = ge::BufferFormat::k5551;
        // In GE pixels; the image is render_scale() times as large (F23b).
        int width = 0;
        int height = 0;
        Image image;
        Buffer readback;
        DirtyRect dirty;
        VramShadow shadow;
        // Destination reads: the copy (DstRead::kCopy) and the set 1
        // descriptor (the input attachment or the copy), made on first use.
        Image copy;
        bool copy_written = false;
        VkDescriptorSet dst_set = VK_NULL_HANDLE;
    };
    struct PixelRect {
        int x1 = 0;
        int y1 = 0;
        int x2 = -1;
        int y2 = -1;
        [[nodiscard]] bool Overlaps(const PixelRect &o) const {
            return x1 <= o.x2 && o.x1 <= x2 && y1 <= o.y2 && o.y1 <= y2;
        }
    };
    // One primitive of a destination-reading group: the pixels it can cover
    // and, for a triangle, its vertices (12.4 screen units) for an exact
    // separation test.
    struct GroupPrimitive {
        PixelRect r;
        bool triangle = false;
        std::array<int, 3> x{};
        std::array<int, 3> y{};
    };
    struct DepthTarget {
        std::uint32_t offset = 0;
        int stride = 0;
        int width = 0; // in GE pixels (the height is kTargetHeight)
        Image image;
        Buffer readback;
        DirtyRect dirty;
        VramShadow shadow;
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
        // What `set` was made of (an exact batch makes its own set with its
        // triangles, F15b).
        VkImageView rgba_view = VK_NULL_HANDLE;
        VkImageView index_view = VK_NULL_HANDLE;
        VkBuffer clut_buffer = VK_NULL_HANDLE;
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
        UploadArena triangles; // GpuTriangle records of exact batches (F15b)
        std::vector<VkDescriptorPool> desc_pools;
        std::size_t desc_pool_index = 0;
        std::map<std::array<std::uint64_t, 5>, VkDescriptorSet> sets;
        std::vector<Image> dead_images;
        std::uint64_t serial = 0;
    };

    GeRenderer(VulkanContext *ctx, const ge::MemoryView &mem, const RendererOptions &options)
        : ctx_(ctx), mem_(mem), options_(options) {}
    bool Init(std::string *error);
    // F23b: the render scale the device supports and the viewport for it.
    bool InitScale(std::string *error);
    void Fail(std::string message);
    bool EnsureFrame();
    Frame &frame() { return frames_[static_cast<std::size_t>(frame_index_)]; }

    int GetColorTarget(std::uint32_t offset, int stride, ge::BufferFormat format);
    int GetDepthTarget(std::uint32_t offset, int stride, int width);
    // Records the upload of rows [y1, y2] of a target from VRAM into the
    // frame's init commands. `initial`: the image has no contents yet.
    bool UploadColorRows(ColorTarget &t, int y1, int y2, bool initial);
    bool UploadDepthRows(DepthTarget &t, int y1, int y2, bool initial);
    // F23b: copies `rows` rows of `row_bytes` (one row of the scaled image
    // per GE row, at `offset` in the staging buffer) into rows y1 * N ..
    // (y1 + rows) * N - 1 of `image`, each GE row N times.
    void CopyRowsToImage(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkImage image,
                         VkImageAspectFlags aspect, int width, int y1, int rows,
                         int bytes_per_pixel) const;
    // F23b: the copy into a target's readback buffer: the whole image at 1x;
    // at N x, rows y1..y2 of the sample row (y * N + s) of each GE row, row y
    // at y * width * N pixels.
    void CopyImageToRows(VkCommandBuffer cmd, const Image &image, VkBuffer buffer, int width,
                         int y1, int y2, int bytes_per_pixel) const;
    // F14b: at a target's first use in a frame, uploads again the rows whose
    // VRAM bytes changed since its shadow was taken.
    bool SyncColorTarget(ColorTarget &t);
    bool SyncDepthTarget(DepthTarget &t);
    // The VRAM bytes of row `y` of a target at `offset` (`row_bytes` long),
    // or nullptr past the end of VRAM.
    [[nodiscard]] const std::uint8_t *VramRow(std::uint32_t offset, int stride_bytes, int row_bytes,
                                              int y) const;
    void SnapshotRows(VramShadow &shadow, std::uint32_t offset, int stride_bytes, int y1, int y2);
    // Spans [first, last] of rows whose VRAM bytes differ from the shadow.
    [[nodiscard]] std::vector<std::pair<int, int>>
    ChangedRows(const VramShadow &shadow, std::uint32_t offset, int stride_bytes, int rows) const;
    bool ResolveTexture(const ge::SamplerState &st, VkImageView *rgba, VkImageView *index);
    // F24c: the image of a replaced texture (at its own size, RGBA8).
    bool ResolveReplacedTexture(const ge::ReplacedTexture &r, VkImageView *rgba);
    // A sampled image of `width` x `height` with `data`, uploaded in the
    // frame's init commands.
    bool CreateTexture(int width, int height, VkFormat format, const void *data, std::size_t size,
                       const char *name, Image *out);
    // F26a: the same with mip levels: `levels[i]` is level i, of
    // max(1, width >> i) x max(1, height >> i).
    struct LevelData {
        const void *data = nullptr;
        std::size_t size = 0;
    };
    bool CreateTexture(int width, int height, VkFormat format, const std::vector<LevelData> &levels,
                       const char *name, Image *out);
    bool UploadClut(const std::uint8_t *clut, std::uint32_t *offset, VkBuffer *buffer);
    // `tri_buffer` (with the bytes at `tri_offset`, `tri_range` long): the
    // batch's GpuTriangle records; the dummy buffer for other batches.
    VkDescriptorSet GetDescriptorSet(VkImageView rgba, VkImageView index, VkBuffer clut_buffer,
                                     VkBuffer tri_buffer, VkDeviceSize tri_offset,
                                     VkDeviceSize tri_range);
    bool StageUpload(const void *data, std::size_t size, UploadArena::Allocation *out);
    // F15b hazards. VRAM offsets [begin, end) against the rows each target
    // drew since its last readback (skipping one colour and one depth
    // target), or against rows 0-271 of the targets used in the open frame.
    [[nodiscard]] bool OverlapsDrawn(std::uint64_t begin, std::uint64_t end, int skip_color,
                                     int skip_depth) const;
    [[nodiscard]] bool OverlapsUsed(std::uint64_t begin, std::uint64_t end) const;
    // Before a draw: its texture, or its targets' rows, overlap what another
    // target drew in the open frame.
    void CheckDrawHazards(const ge::GeState &gs, const ge::RasterizerState &rs);
    // Ends the open frame with a readback, counted under `why`.
    void HazardFlush(std::string_view why);

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
    // Destination reads: a primitive covering `r` (pixels) joins the batch;
    // with kBarrier/kCopy a primitive that overlaps one already in the batch
    // starts a new batch (group), so no pixel is read and written twice by
    // one vkCmdDraw.
    void AddPrimitive(const GroupPrimitive &p);
    void AddPrimitive(const ge::BinCoords &range);
    void AddTriangle(const ge::BinCoords &range, const ge::VertexData &v0, const ge::VertexData &v1,
                     const ge::VertexData &v2);
    void AddRectangle(const ge::BinCoords &range, const ge::VertexData &v0,
                      const ge::VertexData &v1);
    // Points and the pixels of a line (softgpu's DDA): exact pixels.
    void AddPixels(const std::vector<std::uint32_t> &keys);
    void InflateForScale(PixelRect *r) const;
    bool PrepareDstSet(int color);
    void CopyForDstRead(ColorTarget &t, const PixelRect &r);
    void PassBarrier();

    VulkanContext *ctx_;
    const ge::MemoryView &mem_;
    RendererOptions options_;
    DstRead dst_read_ = DstRead::kCopy;
    PipelineCache pipelines_;
    std::vector<VkDescriptorPool> dst_pools_; // set 1 descriptors (per target)
    std::array<Frame, kFramesInFlight> frames_;
    int frame_index_ = 0;
    bool frame_open_ = false;
    std::uint64_t frame_serial_ = 0;
    // F26a: a replaced texture unused for this many frames (several per
    // game frame: one per readback) leaves the cache, checked every 256.
    static constexpr std::uint64_t kReplacedTextureFrames = 2048;
    bool has_replaced_ = false;

    VkSampler sampler_ = VK_NULL_HANDLE;
    Image dummy_rgba_;
    Image dummy_index_;
    Buffer dummy_ubo_;
    Buffer dummy_ssbo_; // one GpuTriangle, for the non-exact batches
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
    std::vector<GpuTriangle> triangles_; // one per 3 vertices in exact batches

    // Command buffer state inside the frame.
    bool in_pass_ = false;
    int pass_color_ = -1;
    int pass_depth_ = -1;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    // The viewport in GE pixels (F23b: at N x, the VkViewport is N times
    // this, inside the device's limits).
    float viewport_origin_ = -2048.0f;
    float viewport_size_ = 4096.0f;
    // F23b: the render scale, the sample of each N x N block the readback
    // takes, the shift (in GE pixels) that puts softgpu's sample point of a
    // triangle on it, and whether triangles take the exact path
    // (options_.exact, at 1x only).
    int scale_ = 1;
    int sample_ = 0;
    float triangle_bias_ = 1.0f / 16.0f;
    bool exact_ = true;
    // The primitives of the current batch (destination reads only) and their
    // bounds.
    std::vector<GroupPrimitive> batch_prims_;
    std::unordered_set<std::uint32_t> batch_pixels_; // y << 16 | x
    PixelRect batch_pixel_bounds_;
    std::vector<std::uint32_t> line_pixels_; // scratch
    PixelRect batch_bounds_;
    // kBarrier: colour writes and destination reads since the last barrier in
    // the current render pass.
    bool pass_written_ = false;
    bool pass_read_ = false;

    RendererStats stats_;
    std::string error_;
    bool failed_ = false;
    // MPO_VK_TRACE_DRAWS=1: one log line per draw with its Vulkan state.
    bool trace_draws_ = false;
    // MPO_VK_MAX_DRAWS=N: draw only the first N draws (bisecting a frame).
    std::uint64_t max_draws_ = UINT64_MAX;
};

} // namespace mpo::vk
