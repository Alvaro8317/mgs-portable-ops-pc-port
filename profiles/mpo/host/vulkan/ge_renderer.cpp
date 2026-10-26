// SPDX-License-Identifier: GPL-3.0-or-later
// The line DDA (DrawLine) and the rectangle UV rotation are ported from PPSSPP
// GPU/Software/Rasterizer.cpp and GPU/Software/RasterizerRectangle.cpp
// @ v1.20.4 (GPL-2.0-or-later), through profiles/mpo/host/ge/rasterizer.cpp.

#include "ge_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "color.hpp"
#include "env.hpp"
#include "texture_decode.hpp"

namespace mpo::vk {
namespace {

constexpr int kScreenScale = ge::kScreenScale;
// softgpu samples a triangle pixel at (16x + 7, 16y + 7) in 12.4 screen
// units (TriangleEdge::Start); Vulkan samples at the pixel centre (x + 0.5).
// Shifting triangle vertices by +1/16 px makes both the same point. Rectangles
// (DrawRectangle) step their UVs from the pixel centre, so they get no shift.
constexpr int kTriangleBias = 1;

std::uint64_t Hash64(const std::uint8_t *data, std::size_t size) {
    // FNV-1a on 64-bit words, then the tail; not cryptographic, only a cache
    // key next to the address, format and size.
    std::uint64_t h = 1469598103934665603ull ^ size;
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, data + i, 8);
        h ^= w;
        h *= 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

template <typename T>
std::uint64_t HandleBits(T handle) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uint64_t>(handle);
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

VkCompareOp MapCompare(ge::Comparison c) {
    switch (c) {
    case ge::Comparison::kNever:
        return VK_COMPARE_OP_NEVER;
    case ge::Comparison::kAlways:
        return VK_COMPARE_OP_ALWAYS;
    case ge::Comparison::kEqual:
        return VK_COMPARE_OP_EQUAL;
    case ge::Comparison::kNotEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case ge::Comparison::kLess:
        return VK_COMPARE_OP_LESS;
    case ge::Comparison::kLequal:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case ge::Comparison::kGreater:
        return VK_COMPARE_OP_GREATER;
    case ge::Comparison::kGequal:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
    return VK_COMPARE_OP_ALWAYS;
}

int BytesPerPixel(ge::BufferFormat f) {
    return f == ge::BufferFormat::k8888 ? 4 : 2;
}

std::uint32_t FromVram(ge::BufferFormat fmt, std::uint32_t raw) {
    switch (fmt) {
    case ge::BufferFormat::k565:
        return ge::RGB565ToRGBA8888(static_cast<std::uint16_t>(raw));
    case ge::BufferFormat::k5551:
        return ge::RGBA5551ToRGBA8888(static_cast<std::uint16_t>(raw));
    case ge::BufferFormat::k4444:
        return ge::RGBA4444ToRGBA8888(static_cast<std::uint16_t>(raw));
    case ge::BufferFormat::k8888:
        return raw;
    }
    return raw;
}

std::uint32_t ToVram(ge::BufferFormat fmt, std::uint32_t rgba) {
    switch (fmt) {
    case ge::BufferFormat::k565:
        return ge::RGBA8888ToRGB565(rgba);
    case ge::BufferFormat::k5551:
        return ge::RGBA8888ToRGBA5551(rgba);
    case ge::BufferFormat::k4444:
        return ge::RGBA8888ToRGBA4444(rgba);
    case ge::BufferFormat::k8888:
        return rgba;
    }
    return rgba;
}

// RasterizerRectangle.cpp AlphaTestIsNeedless and UseDrawSinglePixel: when
// softgpu takes its fast 1:1 sprite path.
bool AlphaTestIsNeedless(const ge::PixelState &ps) {
    switch (ps.alpha_test_func) {
    case ge::Comparison::kNever:
    case ge::Comparison::kEqual:
    case ge::Comparison::kLess:
    case ge::Comparison::kLequal:
        return false;
    case ge::Comparison::kAlways:
        return true;
    case ge::Comparison::kNotEqual:
    case ge::Comparison::kGreater:
    case ge::Comparison::kGequal:
        return !(ps.alpha_test_ref != 0 || ps.has_alpha_test_mask);
    }
    return false;
}

bool UseDrawSinglePixel(const ge::PixelState &ps) {
    if (ps.clear_mode || ps.color_test || ps.stencil_test) return false;
    if (!AlphaTestIsNeedless(ps) || ps.depth_test_func != ge::Comparison::kAlways) return false;
    if (ps.alpha_blend) {
        if (ps.blend_eq != ge::BlendMode::kAdd || ps.blend_src != ge::PixelBlendFactor::kSrcAlpha)
            return false;
        if (ps.blend_dst != ge::PixelBlendFactor::kInvSrcAlpha) return false;
    }
    if (ps.depth_write || ps.apply_fog) return false;
    if (ps.dithering || ps.apply_logic_op || ps.apply_color_write_mask) return false;
    return true;
}

// Row-major index of pixel (x, y) in an image `width` pixels wide.
std::size_t PixelIndex(int x, int y, int width) {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x);
}

// The viewport covers the GE's whole screen space, so that Vulkan never clips
// (shaders/ge.vert): drawing coordinates are 12.4 screen coordinates minus
// the screen offset, which on the PSP puts them in [-2048, 2304) or so.
constexpr float kViewportOrigin = -2048.0f;

int TargetWidth(int stride) {
    return stride > 512 ? 1024 : 512;
}

int ScreenToDrawing(int v) {
    return v / kScreenScale;
}

// double -> int as CVTTSD2SI (rasterizer.cpp).
int TruncDoubleToInt(double d) {
    if (!(d > -2147483649.0 && d < 2147483648.0)) return static_cast<int>(0x80000000u);
    return static_cast<int>(d);
}

void LineTextureCoordinates(const ge::VertexData &v0, const ge::VertexData &v1, float p, bool proj,
                            float *s, float *t) {
    const float q0 = 1.f / v0.clipw;
    const float q1 = 1.f / v1.clipw;
    const float wq0 = p * q0;
    const float wq1 = (1.0f - p) * q1;
    const float q_recip = proj ? 1.0f / (v0.q * wq0 + v1.q * wq1) : 1.0f / (wq0 + wq1);
    *s = (v0.s * wq0 + v1.s * wq1) * q_recip;
    *t = (v0.t * wq0 + v1.t * wq1) * q_recip;
}

GpuVertex MakeVertex(const ge::VertexData &v, int bias, float w, std::uint32_t color0,
                     std::uint32_t color1) {
    GpuVertex g;
    g.x = static_cast<float>(v.screenpos.x + bias) / static_cast<float>(kScreenScale);
    g.y = static_cast<float>(v.screenpos.y + bias) / static_cast<float>(kScreenScale);
    g.z = static_cast<float>(v.screenpos.z & 0xFFFF) / 65535.0f;
    g.w = w;
    g.s = v.s;
    g.t = v.t;
    g.q = v.q;
    g.fog = v.fogdepth;
    g.color0 = color0;
    g.color1 = color1;
    return g;
}

} // namespace

void RendererStats::Approximate(std::string_view what) {
    auto it = approximations.find(what);
    if (it == approximations.end()) {
        approximations.emplace(std::string(what), 1);
        if (log) log(what);
    } else {
        ++it->second;
    }
}

void GeRenderer::DirtyRect::Add(int ax1, int ay1, int ax2, int ay2) {
    x1 = std::min(x1, ax1);
    y1 = std::min(y1, ay1);
    x2 = std::max(x2, ax2);
    y2 = std::max(y2, ay2);
}

std::size_t GeRenderer::TextureKeyHash::operator()(const TextureKey &k) const {
    std::uint64_t h = k.hash;
    h ^= (static_cast<std::uint64_t>(k.addr) << 32) ^ k.size;
    h *= 1099511628211ull;
    h ^= (static_cast<std::uint64_t>(k.format) << 40) ^ k.bufw;
    return static_cast<std::size_t>(h ^ (h >> 31));
}

bool GeRenderer::DrawState::SameAs(const DrawState &o) const {
    return skip == o.skip && key == o.key && std::memcmp(&push, &o.push, sizeof(push)) == 0 &&
           color_target == o.color_target && depth_target == o.depth_target && set == o.set &&
           clut_offset == o.clut_offset && scissor.offset.x == o.scissor.offset.x &&
           scissor.offset.y == o.scissor.offset.y &&
           scissor.extent.width == o.scissor.extent.width &&
           scissor.extent.height == o.scissor.extent.height && blend_constant == o.blend_constant;
}

std::unique_ptr<GeRenderer> GeRenderer::Create(VulkanContext *ctx, const ge::MemoryView &mem,
                                               std::string *error) {
    std::unique_ptr<GeRenderer> r(new GeRenderer(ctx, mem));
    if (!r->Init(error)) return nullptr;
    return r;
}

bool GeRenderer::Init(std::string *error) {
    trace_draws_ = GetEnv("MPO_VK_TRACE_DRAWS") == "1";
    // 8192 covers [-2048, 6144); the spec minimum (4096) still covers the
    // framebuffer and its surroundings.
    viewport_size_ =
        static_cast<float>(std::min<std::uint32_t>(8192, ctx_->caps().max_viewport_dimension));
    if (ctx_->caps().viewport_bounds_min > kViewportOrigin) {
        if (error) *error = "viewportBoundsRange does not reach -2048";
        return false;
    }
    if (const std::string max = GetEnv("MPO_VK_MAX_DRAWS"); !max.empty()) {
        max_draws_ = std::strtoull(max.c_str(), nullptr, 10);
    }
    if (!pipelines_.Init(ctx_, error)) return false;
    VkDevice dev = ctx_->device();

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    VkResult r = vkCreateSampler(dev, &sci, nullptr, &sampler_);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreateSampler", r);
        return false;
    }

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage2D(*ctx_, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, sampled, VK_IMAGE_ASPECT_COLOR_BIT,
                       "dummy rgba texture", &dummy_rgba_, error) ||
        !CreateImage2D(*ctx_, 1, 1, VK_FORMAT_R8_UINT, sampled, VK_IMAGE_ASPECT_COLOR_BIT,
                       "dummy index texture", &dummy_index_, error)) {
        return false;
    }
    if (!CreateBuffer(*ctx_, 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HostAccess::kSequential,
                      "dummy clut", &dummy_ubo_, error)) {
        return false;
    }
    std::memset(dummy_ubo_.mapped, 0, 1024);
    FlushBuffer(*ctx_, dummy_ubo_, 0, VK_WHOLE_SIZE);

    for (Frame &f : frames_) {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx_->queue_family();
        r = vkCreateCommandPool(dev, &pci, nullptr, &f.pool);
        if (r != VK_SUCCESS) {
            if (error) *error = ResultError("vkCreateCommandPool", r);
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        VkCommandBuffer cmds[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        r = vkAllocateCommandBuffers(dev, &cai, cmds);
        if (r != VK_SUCCESS) {
            if (error) *error = ResultError("vkAllocateCommandBuffers", r);
            return false;
        }
        f.init_cmd = cmds[0];
        f.draw_cmd = cmds[1];
        ctx_->SetName(VK_OBJECT_TYPE_COMMAND_BUFFER, f.init_cmd, "ge uploads");
        ctx_->SetName(VK_OBJECT_TYPE_COMMAND_BUFFER, f.draw_cmd, "ge draws");
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = vkCreateFence(dev, &fci, nullptr, &f.fence);
        if (r != VK_SUCCESS) {
            if (error) *error = ResultError("vkCreateFence", r);
            return false;
        }
        f.vertices.Init(ctx_, 1u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "ge vertices");
        f.uniforms.Init(ctx_, 256u << 10, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "ge cluts");
        f.staging.Init(ctx_, 4u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "ge staging");
    }
    return true;
}

GeRenderer::~GeRenderer() {
    VkDevice dev = ctx_->device();
    vkDeviceWaitIdle(dev);
    for (Frame &f : frames_) {
        for (Image &img : f.dead_images) DestroyImage(*ctx_, &img);
        for (VkDescriptorPool p : f.desc_pools) vkDestroyDescriptorPool(dev, p, nullptr);
        f.vertices.Destroy();
        f.uniforms.Destroy();
        f.staging.Destroy();
        if (f.fence) vkDestroyFence(dev, f.fence, nullptr);
        if (f.pool) vkDestroyCommandPool(dev, f.pool, nullptr);
    }
    for (auto &[key, fb] : framebuffers_) vkDestroyFramebuffer(dev, fb, nullptr);
    for (auto &[key, tex] : textures_) DestroyImage(*ctx_, &tex.image);
    for (ColorTarget &t : color_targets_) {
        DestroyImage(*ctx_, &t.image);
        DestroyBuffer(*ctx_, &t.readback);
    }
    for (DepthTarget &t : depth_targets_) {
        DestroyImage(*ctx_, &t.image);
        DestroyBuffer(*ctx_, &t.readback);
    }
    DestroyBuffer(*ctx_, &dummy_ubo_);
    DestroyImage(*ctx_, &dummy_rgba_);
    DestroyImage(*ctx_, &dummy_index_);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    pipelines_.Destroy();
}

void GeRenderer::Fail(std::string message) {
    if (!failed_) {
        failed_ = true;
        error_ = std::move(message);
        ctx_->Log("vulkan renderer: " + error_);
    }
}

bool GeRenderer::EnsureFrame() {
    if (failed_) return false;
    if (frame_open_) return true;
    Frame &f = frame();
    VkDevice dev = ctx_->device();
    if (f.pending) {
        const VkResult r = vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) {
            Fail(ResultError("vkWaitForFences", r));
            return false;
        }
        vkResetFences(dev, 1, &f.fence);
        f.pending = false;
    }
    for (Image &img : f.dead_images) DestroyImage(*ctx_, &img);
    f.dead_images.clear();
    vkResetCommandPool(dev, f.pool, 0);
    f.vertices.Reset();
    f.uniforms.Reset();
    f.staging.Reset();
    for (VkDescriptorPool p : f.desc_pools) vkResetDescriptorPool(dev, p, 0);
    f.desc_pool_index = 0;
    f.sets.clear();
    ++frame_serial_;
    f.serial = frame_serial_;
    last_clut_valid_ = false;

    // Textures no frame in flight can still use.
    if (textures_.size() > 1024) {
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (it->second.last_frame + kFramesInFlight < frame_serial_) {
                DestroyImage(*ctx_, &it->second.image);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.init_cmd, &bi);
    vkBeginCommandBuffer(f.draw_cmd, &bi);
    frame_open_ = true;
    in_pass_ = false;
    pass_color_ = -1;
    pass_depth_ = -1;
    bound_pipeline_ = VK_NULL_HANDLE;
    state_ = DrawState{};
    pending_ = DrawState{};
    vertices_.clear();

    if (!dummies_ready_) {
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image *img : {&dummy_rgba_, &dummy_index_}) {
            ImageBarrier(f.init_cmd, img->image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(f.init_cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zero, 1, &range);
            ImageBarrier(f.init_cmd, img->image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_SHADER_READ_BIT);
        }
        dummies_ready_ = true;
    }
    return true;
}

bool GeRenderer::StageUpload(const void *data, std::size_t size, UploadArena::Allocation *out) {
    std::string err;
    if (!frame().staging.Allocate(size, 16, out, &err)) {
        Fail(err);
        return false;
    }
    std::memcpy(out->ptr, data, size);
    return true;
}

int GeRenderer::GetColorTarget(std::uint32_t offset, int stride, ge::BufferFormat format) {
    offset &= ge::MemoryView::kVramSize - 1;
    for (std::size_t i = 0; i < color_targets_.size(); ++i) {
        const ColorTarget &t = color_targets_[i];
        if (t.offset == offset && t.stride == stride && t.format == format)
            return static_cast<int>(i);
    }
    for (const ColorTarget &t : color_targets_) {
        if (t.offset == offset) {
            stats_.Approximate("framebuffer reused with another format or stride (F15)");
            break;
        }
    }
    ColorTarget t;
    t.offset = offset;
    t.stride = stride;
    t.format = format;
    const int w = TargetWidth(stride);
    const int h = kTargetHeight;
    std::string err;
    char name[64];
    std::snprintf(name, sizeof(name), "ge fb 0x%06X", static_cast<unsigned>(offset));
    if (!CreateImage2D(*ctx_, w, h, PipelineCache::kColorFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, name, &t.image, &err) ||
        !CreateBuffer(*ctx_, static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      HostAccess::kRandom, name, &t.readback, &err)) {
        Fail(err);
        return -1;
    }
    // Initial contents: the VRAM framebuffer as it is now.
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * h);
    const auto bpp = static_cast<std::uint32_t>(BytesPerPixel(format));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint32_t off = offset + static_cast<std::uint32_t>(y * stride + x) * bpp;
            const std::uint32_t raw = bpp == 4 ? mem_.VramRead32(off) : mem_.VramRead16(off);
            pixels[PixelIndex(x, y, w)] = FromVram(format, raw);
        }
    }
    UploadArena::Allocation a;
    if (!StageUpload(pixels.data(), pixels.size() * 4, &a)) return -1;
    VkCommandBuffer cmd = frame().init_cmd;
    ImageBarrier(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.bufferOffset = a.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd, a.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    ImageBarrier(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    ++stats_.target_uploads;
    color_targets_.push_back(std::move(t));
    return static_cast<int>(color_targets_.size() - 1);
}

int GeRenderer::GetDepthTarget(std::uint32_t offset, int stride, int width) {
    offset &= ge::MemoryView::kVramSize - 1;
    for (std::size_t i = 0; i < depth_targets_.size(); ++i) {
        const DepthTarget &t = depth_targets_[i];
        if (t.offset == offset && t.stride == stride && t.width == width)
            return static_cast<int>(i);
    }
    DepthTarget t;
    t.offset = offset;
    t.stride = stride;
    t.width = width;
    const int h = kTargetHeight;
    std::string err;
    char name[64];
    std::snprintf(name, sizeof(name), "ge depth 0x%06X", static_cast<unsigned>(offset));
    if (!CreateImage2D(*ctx_, width, h, PipelineCache::kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, name, &t.image, &err) ||
        !CreateBuffer(*ctx_, static_cast<VkDeviceSize>(width) * h * 2,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, HostAccess::kRandom, name, &t.readback,
                      &err)) {
        Fail(err);
        return -1;
    }
    std::vector<std::uint16_t> depth(static_cast<std::size_t>(width) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < width; ++x) {
            depth[PixelIndex(x, y, width)] =
                mem_.VramRead16(offset + static_cast<std::uint32_t>(y * stride + x) * 2);
        }
    }
    UploadArena::Allocation a;
    if (!StageUpload(depth.data(), depth.size() * 2, &a)) return -1;
    VkCommandBuffer cmd = frame().init_cmd;
    ImageBarrier(cmd, t.image.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.bufferOffset = a.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd, a.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    ImageBarrier(
        cmd, t.image.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    ++stats_.target_uploads;
    depth_targets_.push_back(std::move(t));
    return static_cast<int>(depth_targets_.size() - 1);
}

bool GeRenderer::ResolveTexture(const ge::SamplerState &st, VkImageView *rgba, VkImageView *index) {
    *rgba = dummy_rgba_.view;
    *index = dummy_index_.view;
    const std::uint32_t bytes = TextureSourceBytes(st);
    const std::uint8_t *src = bytes ? mem_.Translate(st.texaddr[0], bytes) : nullptr;

    TextureKey key;
    key.addr = st.texaddr[0] & 0x1FFFFFFF;
    key.format = static_cast<std::uint32_t>(st.texfmt) | (st.swizzle ? 0x100u : 0u);
    key.size = static_cast<std::uint32_t>(st.sizes[0].w) |
               (static_cast<std::uint32_t>(st.sizes[0].h) << 16);
    key.bufw = static_cast<std::uint32_t>(st.texbufw[0]);

    DecodedTexture decoded;
    bool have_decoded = false;
    if (src) {
        key.hash = Hash64(src, bytes);
    } else {
        // Partly outside memory: hash what the sampler would read.
        DecodeTexture(mem_, st, &decoded);
        have_decoded = true;
        key.hash = Hash64(decoded.data.data(), decoded.data.size()) ^ 0x5A5A5A5A5A5A5A5Aull;
    }

    auto it = textures_.find(key);
    if (it == textures_.end()) {
        if (!have_decoded) DecodeTexture(mem_, st, &decoded);
        if (decoded.width <= 0 || decoded.height <= 0) return false;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        switch (decoded.kind) {
        case TexelKind::kIndex8:
            format = VK_FORMAT_R8_UINT;
            break;
        case TexelKind::kIndex16:
            format = VK_FORMAT_R16_UINT;
            break;
        case TexelKind::kIndex32:
            format = VK_FORMAT_R32_UINT;
            break;
        case TexelKind::kRgba8:
            break;
        }
        Texture tex;
        char name[64];
        std::snprintf(name, sizeof(name), "ge tex 0x%08X fmt %u", key.addr, key.format & 0xFF);
        std::string err;
        if (!CreateImage2D(*ctx_, decoded.width, decoded.height, format,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, name, &tex.image, &err)) {
            Fail(err);
            return false;
        }
        UploadArena::Allocation a;
        if (!StageUpload(decoded.data.data(), decoded.data.size(), &a)) {
            DestroyImage(*ctx_, &tex.image);
            return false;
        }
        VkCommandBuffer cmd = frame().init_cmd;
        ImageBarrier(cmd, tex.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = a.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(decoded.width),
                            static_cast<std::uint32_t>(decoded.height), 1};
        vkCmdCopyBufferToImage(cmd, a.buffer, tex.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
        ImageBarrier(cmd, tex.image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        ++stats_.texture_uploads;
        it = textures_.emplace(key, std::move(tex)).first;
    } else {
        ++stats_.texture_hits;
    }
    it->second.last_frame = frame_serial_;
    if (decoded.kind == TexelKind::kRgba8 && !IsClutFormat(st.texfmt)) {
        *rgba = it->second.image.view;
    } else {
        *index = it->second.image.view;
    }
    return true;
}

bool GeRenderer::UploadClut(const std::uint8_t *clut, std::uint32_t *offset, VkBuffer *buffer) {
    if (!clut) {
        *offset = 0;
        *buffer = dummy_ubo_.buffer;
        return true;
    }
    if (last_clut_valid_ && std::memcmp(last_clut_.data(), clut, 1024) == 0) {
        *offset = last_clut_offset_;
        *buffer = last_clut_buffer_;
        return true;
    }
    UploadArena::Allocation a;
    std::string err;
    if (!frame().uniforms.Allocate(1024, ctx_->caps().min_uniform_buffer_offset_alignment, &a,
                                   &err)) {
        Fail(err);
        return false;
    }
    std::memcpy(a.ptr, clut, 1024);
    std::memcpy(last_clut_.data(), clut, 1024);
    last_clut_valid_ = true;
    last_clut_offset_ = static_cast<std::uint32_t>(a.offset);
    last_clut_buffer_ = a.buffer;
    *offset = last_clut_offset_;
    *buffer = a.buffer;
    ++stats_.clut_uploads;
    return true;
}

VkDescriptorSet GeRenderer::GetDescriptorSet(VkImageView rgba, VkImageView index,
                                             VkBuffer clut_buffer) {
    Frame &f = frame();
    const std::array<std::uint64_t, 3> key = {HandleBits(rgba), HandleBits(index),
                                              HandleBits(clut_buffer)};
    if (auto it = f.sets.find(key); it != f.sets.end()) return it->second;

    VkDevice dev = ctx_->device();
    VkDescriptorSetLayout layout = pipelines_.set_layout();
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (f.desc_pool_index >= f.desc_pools.size()) {
            const std::array<VkDescriptorPoolSize, 2> sizes = {{
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 512},
            }};
            VkDescriptorPoolCreateInfo pci{};
            pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pci.maxSets = 512;
            pci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
            pci.pPoolSizes = sizes.data();
            VkDescriptorPool pool = VK_NULL_HANDLE;
            const VkResult r = vkCreateDescriptorPool(dev, &pci, nullptr, &pool);
            if (r != VK_SUCCESS) {
                Fail(ResultError("vkCreateDescriptorPool", r));
                return VK_NULL_HANDLE;
            }
            f.desc_pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = f.desc_pools[f.desc_pool_index];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        const VkResult r = vkAllocateDescriptorSets(dev, &ai, &set);
        if (r == VK_SUCCESS) break;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            Fail(ResultError("vkAllocateDescriptorSets", r));
            return VK_NULL_HANDLE;
        }
        ++f.desc_pool_index;
    }

    VkDescriptorImageInfo images[2] = {
        {sampler_, rgba, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_, index, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkDescriptorBufferInfo ubo{clut_buffer, 0, 1024};
    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &images[0];
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &images[1];
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[2].pBufferInfo = &ubo;
    vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);
    f.sets.emplace(key, set);
    return set;
}

void GeRenderer::BeginDraw(const ge::GeState &gs, const ge::RasterizerState &rs) {
    if (!EnsureFrame()) return;
    ++stats_.draws;
    if (stats_.draws > max_draws_) {
        pending_.skip = true;
        return;
    }
    const ge::PixelState &ps = rs.pixel;
    DrawState ds;
    ds.skip = false;

    ds.color_target = GetColorTarget(ps.fb_offset, ps.fb_stride, ps.fb_format);
    if (ds.color_target < 0) return;
    const ColorTarget &ct = color_targets_[static_cast<std::size_t>(ds.color_target)];
    ds.depth_target = GetDepthTarget(ps.depth_offset, ps.depth_stride, ct.image.width);
    if (ds.depth_target < 0) return;

    // BinManager scissor: the scissor box, limited by the drawing region.
    const int sx1 = gs.ScissorX1();
    const int sy1 = gs.ScissorY1();
    const int sx2 = std::min({gs.ScissorX2(), gs.RegionX2(), ct.image.width - 1});
    const int sy2 = std::min({gs.ScissorY2(), gs.RegionY2(), ct.image.height - 1});
    if (sx2 < sx1 || sy2 < sy1) {
        ds.skip = true;
    } else {
        ds.scissor.offset = {sx1, sy1};
        ds.scissor.extent = {static_cast<std::uint32_t>(sx2 - sx1 + 1),
                             static_cast<std::uint32_t>(sy2 - sy1 + 1)};
    }

    PipelineKey &k = ds.key;
    PushConstants &pc = ds.push;
    pc.ndc_scale[0] = 2.0f / viewport_size_;
    pc.ndc_scale[1] = 2.0f / viewport_size_;
    pc.ndc_origin[0] = kViewportOrigin;
    pc.ndc_origin[1] = kViewportOrigin;
    k.fb_format = static_cast<std::uint8_t>(ps.fb_format);
    for (int i = 0; i < 16; ++i) {
        pc.dither[i / 4] |=
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(ps.dither_matrix[i]))
            << ((i % 4) * 8);
    }

    VkImageView rgba_view = dummy_rgba_.view;
    VkImageView index_view = dummy_index_.view;
    VkBuffer clut_buffer = dummy_ubo_.buffer;

    if (ps.clear_mode) {
        k.clear = 1;
        k.quantize = 1;
        k.dither = ps.dithering ? 1 : 0;
        k.color_write_mask = static_cast<std::uint8_t>(
            (ps.color_test
                 ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)
                 : 0) |
            (ps.stencil_test ? VK_COLOR_COMPONENT_A_BIT : 0));
        k.depth_test = ps.depth_write ? 1 : 0;
        k.depth_compare = static_cast<std::uint8_t>(VK_COMPARE_OP_ALWAYS);
        k.depth_write = ps.depth_write ? 1 : 0;
        if (ps.apply_color_write_mask) stats_.Approximate("colour write mask in clear mode (F15)");
    } else {
        k.secondary = 1;
        if (rs.enable_textures) {
            const ge::SamplerState &st = rs.sampler;
            if (!ResolveTexture(st, &rgba_view, &index_view)) {
                if (failed_) return;
            }
            const bool clut = IsClutFormat(st.texfmt);
            k.tex_mode = clut ? 2 : 1;
            k.clut_format = static_cast<std::uint8_t>(st.clutfmt);
            k.tex_func = static_cast<std::uint8_t>(st.tex_func);
            k.tex_alpha = st.use_texture_alpha ? 1 : 0;
            k.doubling = st.use_color_doubling ? 1 : 0;
            k.mag_linear = rs.mag_filt ? 1 : 0;
            k.min_linear = rs.min_filt ? 1 : 0;
            k.clamp_s = st.clamp_s ? 1 : 0;
            k.clamp_t = st.clamp_t ? 1 : 0;
            k.tex_proj = rs.texture_proj ? 1 : 0;
            k.lod_mode = static_cast<std::uint8_t>(rs.tex_level_mode);
            pc.tex_size = static_cast<std::uint32_t>(st.sizes[0].w) |
                          (static_cast<std::uint32_t>(st.sizes[0].h) << 16);
            if (rs.through_mode) {
                pc.uv_scale[0] = 1.0f / static_cast<float>(1 << st.width0_shift);
                pc.uv_scale[1] = 1.0f / static_cast<float>(1 << st.height0_shift);
            }
            pc.env_color = st.tex_blend_color & 0x00FFFFFF;
            pc.lod_offset = rs.tex_level_offset;
            pc.lod_slope = rs.texture_lod_slope;
            if (clut) {
                if (st.has_clut_shift || st.has_clut_mask || st.has_clut_offset) {
                    const std::uint32_t shift = (st.clut_format >> 2) & 0x1F;
                    const std::uint32_t mask = (st.clut_format >> 8) & 0xFF;
                    const std::uint32_t offset = ((st.clut_format >> 16) & 0x1F) << 4;
                    pc.clut_params = shift | (mask << 8) | (offset << 16) | 0x80000000u;
                }
                if (!UploadClut(st.clut, &ds.clut_offset, &clut_buffer)) return;
            }
            if (rs.max_tex_level > 0) stats_.Approximate("mipmaps: level 0 only (F15)");
            if (!st.use_shared_clut) stats_.Approximate("CLUT4 palettes per mip level (F15)");
            if (ge::MemoryView::IsVramAddress(st.texaddr[0])) {
                const std::uint32_t taddr = st.texaddr[0] & (ge::MemoryView::kVramSize - 1);
                for (const ColorTarget &t : color_targets_) {
                    const std::uint32_t end =
                        t.offset +
                        static_cast<std::uint32_t>(t.stride * 272 * BytesPerPixel(t.format));
                    if (taddr >= t.offset && taddr < end) {
                        stats_.Approximate("texture from a render target (F15)");
                        break;
                    }
                }
            }
        }

        k.alpha_func = static_cast<std::uint8_t>(ps.alpha_test_func);
        pc.alpha_test =
            ps.alpha_test_ref |
            (static_cast<std::uint32_t>(ps.has_alpha_test_mask ? ps.alpha_test_mask : 0xFF) << 8);
        if (ps.color_test) {
            k.color_func = static_cast<std::uint8_t>(ps.color_test_func);
            pc.color_ref = ps.color_test_ref;
            pc.color_mask = ps.color_test_mask;
        }
        if (ps.apply_fog) {
            k.fog = 1;
            pc.fog_color = ps.fog_color;
        }
        if (ps.apply_depth_range) {
            k.depth_range = 1;
            pc.depth_range = static_cast<std::uint32_t>(ps.minz & 0xFFFF) |
                             (static_cast<std::uint32_t>(ps.maxz & 0xFFFF) << 16);
        }
        k.depth_test = (ps.depth_test_func != ge::Comparison::kAlways || ps.depth_write) ? 1 : 0;
        k.depth_compare = static_cast<std::uint8_t>(MapCompare(ps.depth_test_func));
        k.depth_write = ps.depth_write ? 1 : 0;

        // The alpha channel is the GE stencil: kept unless a stencil op writes
        // it (stencil is F15).
        std::uint8_t mask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        if (ps.stencil_test) stats_.Approximate("stencil test and stencil ops (F15)");
        if (ps.apply_logic_op) stats_.Approximate("logic op (F15)");
        if (ps.apply_color_write_mask) {
            const std::uint32_t raw = gs.ColorMask();
            const std::uint32_t bytes[3] = {raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF};
            const std::uint8_t bits[3] = {VK_COLOR_COMPONENT_R_BIT, VK_COLOR_COMPONENT_G_BIT,
                                          VK_COLOR_COMPONENT_B_BIT};
            for (int i = 0; i < 3; ++i) {
                if (bytes[i] == 0xFF) {
                    mask = static_cast<std::uint8_t>(mask & ~bits[i]);
                } else if (bytes[i] != 0) {
                    stats_.Approximate("per-bit colour write mask (F15)");
                }
            }
        }
        k.color_write_mask = mask;

        if (ps.alpha_blend) {
            k.blend = 1;
            std::uint32_t fix = 0;
            bool have_fix = false;
            auto factor = [&](ge::PixelBlendFactor f, bool is_src) -> VkBlendFactor {
                switch (f) {
                case ge::PixelBlendFactor::kOtherColor:
                    return is_src ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
                case ge::PixelBlendFactor::kInvOtherColor:
                    return is_src ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
                                  : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case ge::PixelBlendFactor::kSrcAlpha:
                    return VK_BLEND_FACTOR_SRC_ALPHA;
                case ge::PixelBlendFactor::kInvSrcAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case ge::PixelBlendFactor::kDstAlpha:
                    return VK_BLEND_FACTOR_DST_ALPHA;
                case ge::PixelBlendFactor::kInvDstAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case ge::PixelBlendFactor::kDoubleSrcAlpha:
                    stats_.Approximate("doubled blend factors (F15)");
                    return VK_BLEND_FACTOR_SRC_ALPHA;
                case ge::PixelBlendFactor::kDoubleInvSrcAlpha:
                    stats_.Approximate("doubled blend factors (F15)");
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case ge::PixelBlendFactor::kDoubleDstAlpha:
                    stats_.Approximate("doubled blend factors (F15)");
                    return VK_BLEND_FACTOR_DST_ALPHA;
                case ge::PixelBlendFactor::kDoubleInvDstAlpha:
                    stats_.Approximate("doubled blend factors (F15)");
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case ge::PixelBlendFactor::kZero:
                    return VK_BLEND_FACTOR_ZERO;
                case ge::PixelBlendFactor::kOne:
                    return VK_BLEND_FACTOR_ONE;
                case ge::PixelBlendFactor::kFix: {
                    const std::uint32_t c = is_src ? ps.blend_fix_src : ps.blend_fix_dst;
                    if (have_fix && c != fix) {
                        stats_.Approximate("two different fixed blend colours (F15)");
                    }
                    fix = c;
                    have_fix = true;
                    return VK_BLEND_FACTOR_CONSTANT_COLOR;
                }
                }
                return VK_BLEND_FACTOR_ONE;
            };
            k.src_factor = static_cast<std::uint8_t>(factor(ps.blend_src, true));
            k.dst_factor = static_cast<std::uint8_t>(factor(ps.blend_dst, false));
            ds.blend_constant = fix & 0x00FFFFFF;
            switch (ps.blend_eq) {
            case ge::BlendMode::kAdd:
                k.blend_op = VK_BLEND_OP_ADD;
                break;
            case ge::BlendMode::kSubtract:
                k.blend_op = VK_BLEND_OP_SUBTRACT;
                break;
            case ge::BlendMode::kReverseSubtract:
                k.blend_op = VK_BLEND_OP_REVERSE_SUBTRACT;
                break;
            case ge::BlendMode::kMin:
                k.blend_op = VK_BLEND_OP_MIN;
                break;
            case ge::BlendMode::kMax:
                k.blend_op = VK_BLEND_OP_MAX;
                break;
            case ge::BlendMode::kAbsDiff:
                stats_.Approximate("blend ABS_DIFF (F15)");
                k.blend_op = VK_BLEND_OP_MAX;
                break;
            }
            // Fixed-function blending works on floats and the result is
            // truncated to the target format on readback: softgpu's integer
            // blend ((2c+1)(2f+1) >> 10) and the dither after the blend need
            // shader blending (F15).
            stats_.Approximate("blending in fixed function (GE integer rounding, F15)");
            if (ps.dithering) stats_.Approximate("dither on blended draws (F15)");
        } else {
            k.quantize = 1;
            k.dither = ps.dithering ? 1 : 0;
        }
    }

    ds.set = GetDescriptorSet(rgba_view, index_view, clut_buffer);
    if (ds.set == VK_NULL_HANDLE) return;
    if (trace_draws_) {
        char line[512];
        std::snprintf(
            line, sizeof(line),
            "draw %llu: fb 0x%06X fmt %d clear %d tex %d (fmt %d clut %d func %d a %d x2 %d "
            "lin %d/%d clamp %d/%d %dx%d through %d) alpha %d/%08X color %d fog %d "
            "blend %d op %d %d/%d fix %06X depth %d/%d/%d range %d mask %X dither %d "
            "quant %d scissor %d,%d %ux%u",
            static_cast<unsigned long long>(stats_.draws), ps.fb_offset & 0x1FFFFF, k.fb_format,
            k.clear, k.tex_mode, static_cast<int>(rs.sampler.texfmt), k.clut_format, k.tex_func,
            k.tex_alpha, k.doubling, k.mag_linear, k.min_linear, k.clamp_s, k.clamp_t,
            rs.sampler.sizes[0].w, rs.sampler.sizes[0].h, rs.through_mode ? 1 : 0, k.alpha_func,
            pc.alpha_test, k.color_func, k.fog, k.blend, k.blend_op, k.src_factor, k.dst_factor,
            ds.blend_constant, k.depth_test, k.depth_compare, k.depth_write, k.depth_range,
            k.color_write_mask, k.dither, k.quantize, ds.scissor.offset.x, ds.scissor.offset.y,
            ds.scissor.extent.width, ds.scissor.extent.height);
        ctx_->Log(line);
    }
    pending_ = ds;
}

void GeRenderer::SetTopology(Topology topology) {
    DrawState want = pending_;
    want.key.topology = topology;
    UseState(want);
}

void GeRenderer::UseState(const DrawState &want) {
    if (!want.SameAs(state_)) {
        FlushBatch();
        state_ = want;
    }
}

void GeRenderer::MarkDirty(const ge::BinCoords &range) {
    const int x1 = range.x1 / kScreenScale;
    const int y1 = range.y1 / kScreenScale;
    const int x2 = range.x2 / kScreenScale;
    const int y2 = range.y2 / kScreenScale;
    if (state_.key.color_write_mask != 0 && state_.color_target >= 0) {
        color_targets_[static_cast<std::size_t>(state_.color_target)].dirty.Add(x1, y1, x2, y2);
    }
    if (state_.key.depth_write && state_.depth_target >= 0) {
        depth_targets_[static_cast<std::size_t>(state_.depth_target)].dirty.Add(x1, y1, x2, y2);
    }
}

void GeRenderer::MarkDirtyPixel(int x, int y) {
    const ge::BinCoords r{x * kScreenScale, y * kScreenScale, x * kScreenScale, y * kScreenScale};
    MarkDirty(r);
}

void GeRenderer::DrawTriangle(const ge::VertexData &v0, const ge::VertexData &v1,
                              const ge::VertexData &v2, const ge::BinCoords &range,
                              const ge::RasterizerState &state) {
    if (failed_ || pending_.skip) return;
    SetTopology(Topology::kTriangles);
    // Perspective-correct texture coordinates use the clip w; through mode
    // and degenerate w (a vertex behind the camera that the -Z clipper kept)
    // interpolate linearly, as on screen.
    const bool persp = !state.through_mode && v0.clipw > 0.0f && v1.clipw > 0.0f && v2.clipw > 0.0f;
    const bool flat = !state.shade_gouraud;
    const ge::VertexData *vs[3] = {&v0, &v1, &v2};
    for (const ge::VertexData *v : vs) {
        const std::uint32_t c0 = flat ? v2.color0 : v->color0;
        const std::uint32_t c1 = flat ? v2.color1 : v->color1;
        Push(MakeVertex(*v, kTriangleBias, persp ? v->clipw : 1.0f, c0, c1 & 0x00FFFFFF));
        if (trace_draws_ && GetEnv("MPO_VK_TRACE_DRAW") == std::to_string(stats_.draws)) {
            const GpuVertex &g = vertices_.back();
            char line[256];
            std::snprintf(
                line, sizeof(line), "  vtx %.4f %.4f %.6f w %.4f st %.4f %.4f q %.4f c %08X",
                static_cast<double>(g.x), static_cast<double>(g.y), static_cast<double>(g.z),
                static_cast<double>(g.w), static_cast<double>(g.s), static_cast<double>(g.t),
                static_cast<double>(g.q), g.color0);
            ctx_->Log(line);
        }
    }
    MarkDirty(range);
    ++stats_.triangles;
}

// RotateUV: the corner of v0 gets v0's UV and the corner of v1 gets v1's; on
// a rectangle mirrored in one axis the other two corners swap theirs
// (RasterizerRectangle.cpp DrawRectangle's four cases).
void GeRenderer::EmitQuad(const ge::VertexData &v0, const ge::VertexData &v1,
                          const ge::RasterizerState &state) {
    (void)state;
    const std::uint32_t c0 = v1.color0;
    const std::uint32_t c1 = v1.color1 & 0x00FFFFFF;
    ge::VertexData a = v1;
    a.screenpos.x = v0.screenpos.x;
    a.screenpos.y = v0.screenpos.y;
    a.s = v0.s;
    a.t = v0.t;
    ge::VertexData b = v1;
    ge::VertexData c = v1; // (x1, y0)
    c.screenpos.y = v0.screenpos.y;
    ge::VertexData d = v1; // (x0, y1)
    d.screenpos.x = v0.screenpos.x;
    const bool same = (v0.screenpos.x < v1.screenpos.x) == (v0.screenpos.y < v1.screenpos.y);
    if (same) {
        c.s = v1.s;
        c.t = v0.t;
        d.s = v0.s;
        d.t = v1.t;
    } else {
        c.s = v0.s;
        c.t = v1.t;
        d.s = v1.s;
        d.t = v0.t;
    }
    const GpuVertex ga = MakeVertex(a, 0, 1.0f, c0, c1);
    const GpuVertex gb = MakeVertex(b, 0, 1.0f, c0, c1);
    const GpuVertex gc = MakeVertex(c, 0, 1.0f, c0, c1);
    const GpuVertex gd = MakeVertex(d, 0, 1.0f, c0, c1);
    Push(ga);
    Push(gc);
    Push(gb);
    Push(ga);
    Push(gb);
    Push(gd);
}

void GeRenderer::DrawRectangle(const ge::VertexData &v0, const ge::VertexData &v1,
                               const ge::BinCoords &range, const ge::RasterizerState &state) {
    if (failed_ || pending_.skip) return;
    SetTopology(Topology::kTriangles);
    EmitQuad(v0, v1, state);
    MarkDirty(range);
    ++stats_.rectangles;
}

// softgpu's 1:1 sprite path (RasterizerRectangle.cpp DrawSprite) samples
// with NEAREST whatever the filter (a 1:1 sprite hits texel centres anyway),
// and its fast variant DrawSpriteTex draws the texel as it is, without the
// colour doubling, when the vertex colour is exactly white (or the function
// is REPLACE): reproduced here as REPLACE without doubling (F11a, "Decisiones").
void GeRenderer::DrawSprite(const ge::VertexData &v0, const ge::VertexData &v1,
                            const ge::BinCoords &range, const ge::RasterizerState &state) {
    if (failed_ || pending_.skip) return;
    DrawState want = pending_;
    want.key.topology = Topology::kTriangles;
    if (state.enable_textures) {
        want.key.mag_linear = 0;
        want.key.min_linear = 0;
        const ge::SamplerState &st = state.sampler;
        const bool fast =
            UseDrawSinglePixel(state.pixel) &&
            (st.tex_func == ge::TexFunc::kModulate || st.tex_func == ge::TexFunc::kReplace) &&
            st.use_texture_alpha;
        if (fast && (v1.color0 == 0xFFFFFFFFu || st.tex_func == ge::TexFunc::kReplace)) {
            want.key.tex_func = static_cast<std::uint8_t>(ge::TexFunc::kReplace);
            want.key.doubling = 0;
            want.key.tex_alpha = 1;
        }
    }
    UseState(want);
    EmitQuad(v0, v1, state);
    MarkDirty(range);
    ++stats_.rectangles;
}

void GeRenderer::ClearRectangle(const ge::VertexData &v0, const ge::VertexData &v1,
                                const ge::BinCoords &range, const ge::RasterizerState &state) {
    DrawRectangle(v0, v1, range, state);
}

void GeRenderer::EmitPointPixel(int x, int y, int z, std::uint32_t color0, std::uint32_t color1,
                                float fog, float s, float t, float q) {
    SetTopology(Topology::kPoints);
    GpuVertex g;
    g.x = static_cast<float>(x) + 0.5f;
    g.y = static_cast<float>(y) + 0.5f;
    g.z = static_cast<float>(z & 0xFFFF) / 65535.0f;
    g.w = 1.0f;
    g.s = s;
    g.t = t;
    g.q = q;
    g.fog = fog;
    g.color0 = color0;
    g.color1 = color1 & 0x00FFFFFF;
    Push(g);
    MarkDirtyPixel(x, y);
}

void GeRenderer::DrawPoint(const ge::VertexData &v0, const ge::BinCoords &range,
                           const ge::RasterizerState &state) {
    (void)range;
    (void)state;
    if (failed_ || pending_.skip) return;
    EmitPointPixel(ScreenToDrawing(v0.screenpos.x), ScreenToDrawing(v0.screenpos.y), v0.screenpos.z,
                   v0.color0, v0.color1, v0.fogdepth, v0.s, v0.t, v0.q);
    ++stats_.points;
}

// softgpu's DrawLine DDA on the CPU: each step becomes a one-pixel point with
// the colour, depth, fog and UV softgpu computes for it, so the GPU draws
// exactly the pixels of the reference renderer.
void GeRenderer::DrawLine(const ge::VertexData &v0, const ge::VertexData &v1,
                          const ge::BinCoords &range, const ge::RasterizerState &state) {
    if (failed_ || pending_.skip) return;
    ++stats_.lines;
    const int ax = v0.screenpos.x;
    const int ay = v0.screenpos.y;
    const int az = v0.screenpos.z;
    const int bx = v1.screenpos.x;
    const int by = v1.screenpos.y;
    const int bz = v1.screenpos.z;
    int dx = bx - ax;
    int dy = by - ay;
    const int dz = bz - az;
    int steps;
    if (std::abs(dx) < std::abs(dy)) {
        steps = std::abs(dy) / kScreenScale;
    } else {
        steps = std::abs(dx) / kScreenScale;
    }
    if (steps <= 0) return;
    if (dx < 0 && dx >= -kScreenScale) dx++;
    if (dy < 0 && dy >= -kScreenScale) dy++;
    const double xinc = static_cast<double>(dx) / steps;
    const double yinc = static_cast<double>(dy) / steps;
    const double zinc = static_cast<double>(dz) / steps;

    const bool interpolate_color =
        state.shade_gouraud && !(v0.color0 == v1.color0 && v0.color1 == v1.color1);
    const ge::Vec4i v0_c0 = ge::Vec4i::FromRGBA(v0.color0);
    const ge::Vec4i v1_c0 = ge::Vec4i::FromRGBA(v1.color0);
    const ge::Vec4i v0_c1 = ge::Vec4i::FromRGBA(v0.color1 & 0xFFFFFF);
    const ge::Vec4i v1_c1 = ge::Vec4i::FromRGBA(v1.color1 & 0xFFFFFF);
    if (state.enable_textures && state.antialias_lines)
        stats_.Approximate("antialiased textured lines (F15)");

    double x = ax > bx ? ax - 1 : ax;
    double y = ay > by ? ay - 1 : ay;
    double z = az;
    for (int i = 0; i < steps; ++i) {
        const int px = ScreenToDrawing(TruncDoubleToInt(x));
        const int py = ScreenToDrawing(TruncDoubleToInt(y));
        const int zi = TruncDoubleToInt(z);
        const bool inside = x >= range.x1 && y >= range.y1 && x <= range.x2 && y <= range.y2;
        if (inside) {
            ge::Vec4i prim;
            ge::Vec4i sec;
            if (interpolate_color) {
                for (int k = 0; k < 4; ++k)
                    prim[k] = (v0_c0[k] * (steps - i) + v1_c0[k] * i) / steps;
                for (int k = 0; k < 3; ++k)
                    sec[k] = (v0_c1[k] * (steps - i) + v1_c1[k] * i) / steps;
            } else {
                prim = v1_c0;
                sec = v1_c1;
            }
            if (state.antialias_lines) prim[3] = 0x7F;
            const float fog = (v0.fogdepth * static_cast<float>(steps - i) +
                               v1.fogdepth * static_cast<float>(i)) /
                              static_cast<float>(steps);
            float s = 0;
            float t = 0;
            if (state.enable_textures) {
                if (state.through_mode) {
                    const float fa = static_cast<float>(steps - i);
                    const float fb = static_cast<float>(i);
                    const auto fs = static_cast<float>(steps);
                    s = (v0.s * fa + v1.s * fb) / fs;
                    t = (v0.t * fa + v1.t * fb) / fs;
                } else {
                    LineTextureCoordinates(
                        v0, v1, static_cast<float>(steps - i) / static_cast<float>(steps),
                        state.texture_proj, &s, &t);
                }
            }
            EmitPointPixel(px, py, zi, prim.ToRGBA(), sec.ToRGB(), fog, s, t, 1.0f);
            ++stats_.line_pixels;
        }
        x += xinc;
        y += yinc;
        z += zinc;
    }
}

void GeRenderer::BeginPass(int color, int depth) {
    if (in_pass_ && pass_color_ == color && pass_depth_ == depth) return;
    EndPass();
    Frame &f = frame();
    const ColorTarget &ct = color_targets_[static_cast<std::size_t>(color)];
    const DepthTarget &dt = depth_targets_[static_cast<std::size_t>(depth)];
    const auto w = static_cast<std::uint32_t>(std::min(ct.image.width, dt.image.width));
    const auto h = static_cast<std::uint32_t>(std::min(ct.image.height, dt.image.height));
    const VkRect2D area{{0, 0}, {w, h}};
    if (ctx_->caps().dynamic_rendering) {
        VkRenderingAttachmentInfo ca{};
        ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca.imageView = ct.image.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo da = ca;
        da.imageView = dt.image.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = area;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(f.draw_cmd, &ri);
    } else {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(color)) << 32) |
            static_cast<std::uint32_t>(depth);
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (auto it = framebuffers_.find(key); it != framebuffers_.end()) {
            fb = it->second;
        } else {
            const VkImageView views[2] = {ct.image.view, dt.image.view};
            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = pipelines_.render_pass();
            fci.attachmentCount = 2;
            fci.pAttachments = views;
            fci.width = w;
            fci.height = h;
            fci.layers = 1;
            const VkResult r = vkCreateFramebuffer(ctx_->device(), &fci, nullptr, &fb);
            if (r != VK_SUCCESS) {
                Fail(ResultError("vkCreateFramebuffer", r));
                return;
            }
            framebuffers_.emplace(key, fb);
        }
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = pipelines_.render_pass();
        rbi.framebuffer = fb;
        rbi.renderArea = area;
        vkCmdBeginRenderPass(f.draw_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    }
    const VkViewport vp{
        kViewportOrigin, kViewportOrigin, viewport_size_, viewport_size_, 0.0f, 1.0f};
    vkCmdSetViewport(f.draw_cmd, 0, 1, &vp);
    in_pass_ = true;
    pass_color_ = color;
    pass_depth_ = depth;
    bound_pipeline_ = VK_NULL_HANDLE;
}

void GeRenderer::EndPass() {
    if (!in_pass_) return;
    if (ctx_->caps().dynamic_rendering) {
        vkCmdEndRendering(frame().draw_cmd);
    } else {
        vkCmdEndRenderPass(frame().draw_cmd);
    }
    in_pass_ = false;
}

void GeRenderer::FlushBatch() {
    if (vertices_.empty()) return;
    if (failed_ || state_.skip || state_.color_target < 0 || state_.depth_target < 0) {
        vertices_.clear();
        return;
    }
    Frame &f = frame();
    std::string err;
    const VkPipeline pipeline = pipelines_.Get(state_.key, &err);
    if (pipeline == VK_NULL_HANDLE) {
        Fail(err);
        return;
    }
    UploadArena::Allocation a;
    const std::size_t bytes = vertices_.size() * sizeof(GpuVertex);
    if (!f.vertices.Allocate(bytes, 16, &a, &err)) {
        Fail(err);
        return;
    }
    std::memcpy(a.ptr, vertices_.data(), bytes);
    BeginPass(state_.color_target, state_.depth_target);
    if (failed_) return;
    VkCommandBuffer cmd = f.draw_cmd;
    if (pipeline != bound_pipeline_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bound_pipeline_ = pipeline;
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.layout(), 0, 1,
                            &state_.set, 1, &state_.clut_offset);
    vkCmdBindVertexBuffers(cmd, 0, 1, &a.buffer, &a.offset);
    vkCmdSetScissor(cmd, 0, 1, &state_.scissor);
    const float blend[4] = {static_cast<float>(state_.blend_constant & 0xFF) / 255.0f,
                            static_cast<float>((state_.blend_constant >> 8) & 0xFF) / 255.0f,
                            static_cast<float>((state_.blend_constant >> 16) & 0xFF) / 255.0f,
                            1.0f};
    vkCmdSetBlendConstants(cmd, blend);
    vkCmdPushConstants(cmd, pipelines_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PushConstants), &state_.push);
    vkCmdDraw(cmd, static_cast<std::uint32_t>(vertices_.size()), 1, 0, 0);
    ++stats_.batches;
    vertices_.clear();
}

bool GeRenderer::EndFrame(bool readback, std::string *error) {
    if (failed_) {
        if (error) *error = error_;
        return false;
    }
    if (!frame_open_) return true;
    Frame &f = frame();
    FlushBatch();
    EndPass();
    if (failed_) {
        if (error) *error = error_;
        return false;
    }
    VkCommandBuffer cmd = f.draw_cmd;
    std::vector<int> color_reads;
    std::vector<int> depth_reads;
    if (readback) {
        for (std::size_t i = 0; i < color_targets_.size(); ++i) {
            ColorTarget &t = color_targets_[i];
            if (t.dirty.Empty()) continue;
            color_reads.push_back(static_cast<int>(i));
            ImageBarrier(
                cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {static_cast<std::uint32_t>(t.image.width),
                                static_cast<std::uint32_t>(t.image.height), 1};
            vkCmdCopyImageToBuffer(cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   t.readback.buffer, 1, &copy);
            ImageBarrier(
                cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
        for (std::size_t i = 0; i < depth_targets_.size(); ++i) {
            DepthTarget &t = depth_targets_[i];
            if (t.dirty.Empty()) continue;
            depth_reads.push_back(static_cast<int>(i));
            const VkPipelineStageFlags tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            ImageBarrier(cmd, t.image.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tests,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            copy.imageExtent = {static_cast<std::uint32_t>(t.image.width),
                                static_cast<std::uint32_t>(t.image.height), 1};
            vkCmdCopyImageToBuffer(cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   t.readback.buffer, 1, &copy);
            ImageBarrier(cmd, t.image.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, tests,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
        if (!color_reads.empty() || !depth_reads.empty()) {
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 1, &mb, 0, nullptr, 0, nullptr);
        }
    }
    vkEndCommandBuffer(f.init_cmd);
    vkEndCommandBuffer(f.draw_cmd);
    f.vertices.FlushAll();
    f.uniforms.FlushAll();
    f.staging.FlushAll();

    const VkCommandBuffer cmds[2] = {f.init_cmd, f.draw_cmd};
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 2;
    si.pCommandBuffers = cmds;
    VkResult r = vkQueueSubmit(ctx_->queue(), 1, &si, f.fence);
    frame_open_ = false;
    if (r != VK_SUCCESS) {
        Fail(ResultError("vkQueueSubmit", r));
        if (error) *error = error_;
        return false;
    }
    f.pending = true;
    ++stats_.frames;
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;

    if (readback) {
        r = vkWaitForFences(ctx_->device(), 1, &f.fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) {
            Fail(ResultError("vkWaitForFences", r));
            if (error) *error = error_;
            return false;
        }
        vkResetFences(ctx_->device(), 1, &f.fence);
        f.pending = false;
        for (int i : color_reads) {
            ColorTarget &t = color_targets_[static_cast<std::size_t>(i)];
            InvalidateBuffer(*ctx_, t.readback);
            const auto *px = static_cast<const std::uint8_t *>(t.readback.mapped);
            const int x1 = std::max(t.dirty.x1, 0);
            const int y1 = std::max(t.dirty.y1, 0);
            const int x2 = std::min({t.dirty.x2, t.image.width - 1, t.stride - 1});
            const int y2 = std::min(t.dirty.y2, t.image.height - 1);
            const auto bpp = static_cast<std::uint32_t>(BytesPerPixel(t.format));
            for (int y = y1; y <= y2; ++y) {
                for (int x = x1; x <= x2; ++x) {
                    std::uint32_t rgba = 0;
                    std::memcpy(&rgba, px + PixelIndex(x, y, t.image.width) * 4, 4);
                    const std::uint32_t off =
                        t.offset + static_cast<std::uint32_t>(y * t.stride + x) * bpp;
                    const std::uint32_t v = ToVram(t.format, rgba);
                    if (bpp == 4) {
                        mem_.VramWrite32(off, v);
                    } else {
                        mem_.VramWrite16(off, static_cast<std::uint16_t>(v));
                    }
                }
            }
            t.dirty = DirtyRect{};
            ++stats_.readbacks;
        }
        for (int i : depth_reads) {
            DepthTarget &t = depth_targets_[static_cast<std::size_t>(i)];
            InvalidateBuffer(*ctx_, t.readback);
            const auto *px = static_cast<const std::uint8_t *>(t.readback.mapped);
            const int x1 = std::max(t.dirty.x1, 0);
            const int y1 = std::max(t.dirty.y1, 0);
            const int x2 = std::min({t.dirty.x2, t.image.width - 1, t.stride - 1});
            const int y2 = std::min(t.dirty.y2, t.image.height - 1);
            for (int y = y1; y <= y2; ++y) {
                for (int x = x1; x <= x2; ++x) {
                    std::uint16_t d = 0;
                    std::memcpy(&d, px + PixelIndex(x, y, t.image.width) * 2, 2);
                    mem_.VramWrite16(t.offset + static_cast<std::uint32_t>(y * t.stride + x) * 2,
                                     d);
                }
            }
            t.dirty = DirtyRect{};
            ++stats_.readbacks;
        }
    }
    return true;
}

bool GeRenderer::FindTarget(std::uint32_t fb_address, TargetInfo *out) const {
    const std::uint32_t offset = fb_address & (ge::MemoryView::kVramSize - 1);
    for (const ColorTarget &t : color_targets_) {
        if (t.offset != offset) continue;
        out->image = t.image.image;
        out->view = t.image.view;
        out->width = t.image.width;
        out->height = t.image.height;
        out->stride = t.stride;
        out->format = t.format;
        return true;
    }
    return false;
}

} // namespace mpo::vk
