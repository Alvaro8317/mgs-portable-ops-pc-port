// SPDX-License-Identifier: GPL-3.0-or-later

#include "pipelines.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace mpo::vk {
namespace {

#include "shaders/ge_shaders_spv.inc"

constexpr std::uint32_t kSpecCount = 20;

bool CreateModule(VkDevice device, const std::uint32_t *code, std::size_t words,
                  VkShaderModule *out, std::string *error) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * 4;
    ci.pCode = code;
    const VkResult r = vkCreateShaderModule(device, &ci, nullptr, out);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreateShaderModule", r);
        return false;
    }
    return true;
}

} // namespace

std::size_t PipelineKeyHash::operator()(const PipelineKey &k) const {
    // FNV-1a over the key bytes.
    const auto *p = reinterpret_cast<const std::uint8_t *>(&k);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < sizeof(k); ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
}

PipelineCache::~PipelineCache() {
    Destroy();
}

bool PipelineCache::Init(VulkanContext *ctx, std::string *error) {
    ctx_ = ctx;
    VkDevice dev = ctx->device();
    if (!CreateModule(dev, kGeVertSpv, std::size(kGeVertSpv), &vert_, error)) return false;
    if (!CreateModule(dev, kGeFragSpv, std::size(kGeFragSpv), &frag_, error)) return false;
    ctx->SetName(VK_OBJECT_TYPE_SHADER_MODULE, vert_, "ge.vert");
    ctx->SetName(VK_OBJECT_TYPE_SHADER_MODULE, frag_, "ge.frag");

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1] = bindings[0];
    bindings[1].binding = 1;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = static_cast<std::uint32_t>(bindings.size());
    dl.pBindings = bindings.data();
    VkResult r = vkCreateDescriptorSetLayout(dev, &dl, nullptr, &set_layout_);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreateDescriptorSetLayout", r);
        return false;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    r = vkCreatePipelineLayout(dev, &pl, nullptr, &layout_);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreatePipelineLayout", r);
        return false;
    }

    if (!ctx->caps().dynamic_rendering) {
        std::array<VkAttachmentDescription, 2> att{};
        att[0].format = kColorFormat;
        att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1] = att[0];
        att[1].format = kDepthFormat;
        att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &color_ref;
        sub.pDepthStencilAttachment = &depth_ref;
        VkRenderPassCreateInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = static_cast<std::uint32_t>(att.size());
        rp.pAttachments = att.data();
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        r = vkCreateRenderPass(dev, &rp, nullptr, &render_pass_);
        if (r != VK_SUCCESS) {
            if (error) *error = ResultError("vkCreateRenderPass", r);
            return false;
        }
        ctx->SetName(VK_OBJECT_TYPE_RENDER_PASS, render_pass_, "ge render pass");
    }

    VkPipelineCacheCreateInfo pc{};
    pc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    r = vkCreatePipelineCache(dev, &pc, nullptr, &cache_);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreatePipelineCache", r);
        return false;
    }
    return true;
}

void PipelineCache::Destroy() {
    if (!ctx_) return;
    VkDevice dev = ctx_->device();
    for (auto &[key, pipeline] : pipelines_) vkDestroyPipeline(dev, pipeline, nullptr);
    pipelines_.clear();
    if (cache_) vkDestroyPipelineCache(dev, cache_, nullptr);
    if (render_pass_) vkDestroyRenderPass(dev, render_pass_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    if (frag_) vkDestroyShaderModule(dev, frag_, nullptr);
    if (vert_) vkDestroyShaderModule(dev, vert_, nullptr);
    cache_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    frag_ = VK_NULL_HANDLE;
    vert_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

VkPipeline PipelineCache::Get(const PipelineKey &key, std::string *error) {
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;

    // Specialization data: constant id i reads the 32-bit value at 4 * i.
    std::uint8_t fields[sizeof(PipelineKey)];
    std::memcpy(fields, &key, sizeof(key));
    std::array<std::uint32_t, kSpecCount> spec_data{};
    std::array<VkSpecializationMapEntry, kSpecCount> entries{};
    for (std::uint32_t i = 0; i < kSpecCount; ++i) {
        spec_data[i] = fields[i];
        entries[i].constantID = i;
        entries[i].offset = i * 4;
        entries[i].size = 4;
    }
    VkSpecializationInfo spec{};
    spec.mapEntryCount = kSpecCount;
    spec.pMapEntries = entries.data();
    spec.dataSize = sizeof(spec_data);
    spec.pData = spec_data.data();

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &spec;

    VkVertexInputBindingDescription binding{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 5> attrs = {{
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, s)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(GpuVertex, fog)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color0)},
        {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color1)},
    }};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = key.topology == Topology::kPoints ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                                    : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    // z is always in [0, w]; the clamp only guards against rounding.
    rs.depthClampEnable = ctx_->caps().depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // mpo_ge already culled (and only sends counter-clockwise triangles).
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = static_cast<VkCompareOp>(key.depth_compare);

    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = key.blend ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = static_cast<VkBlendFactor>(key.src_factor);
    cb.dstColorBlendFactor = static_cast<VkBlendFactor>(key.dst_factor);
    cb.colorBlendOp = static_cast<VkBlendOp>(key.blend_op);
    // The alpha channel holds the GE stencil: never blended.
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = key.color_write_mask;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &cb;

    const std::array<VkDynamicState, 3> dyn_states = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<std::uint32_t>(dyn_states.size());
    dyn.pDynamicStates = dyn_states.data();

    VkFormat color_format = kColorFormat;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    if (render_pass_ == VK_NULL_HANDLE) gp.pNext = &rendering;
    gp.stageCount = static_cast<std::uint32_t>(stages.size());
    gp.pStages = stages.data();
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &blend;
    gp.pDynamicState = &dyn;
    gp.layout = layout_;
    gp.renderPass = render_pass_;
    gp.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r =
        vkCreateGraphicsPipelines(ctx_->device(), cache_, 1, &gp, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        if (error) *error = ResultError("vkCreateGraphicsPipelines", r);
        return VK_NULL_HANDLE;
    }
    pipelines_.emplace(key, pipeline);
    return pipeline;
}

} // namespace mpo::vk
