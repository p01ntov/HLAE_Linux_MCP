#pragma once

// Separable sigma=2 backdrop for the live Vulkan overlay. No loader imports,
// device-idle waits, command submission, swapchain ownership changes, or ImGui
// globals live here; the caller provides its next-layer Vulkan dispatch and
// texture registration callbacks.
//
// Integration contract:
//  * negotiate VK_IMAGE_USAGE_TRANSFER_SRC_BIT for the swapchain first;
//  * Initialize after the ImGui Vulkan backend is initialized;
//  * wait the chosen swap image's previous overlay fence before Record;
//  * wait the application's present semaphores at TRANSFER (or ALL_COMMANDS);
//  * Record outside render passes, before rendering ImGui in the same command
//    buffer. The source starts and finishes in sourceLayout (normally PRESENT);
//  * draw Texture(index) only under panel rectangles, with screen-relative UVs;
//  * wait all GPU uses, then Destroy BEFORE shutting down the ImGui backend.
//
// Raw image copies may use size-compatible UNORM/SRGB formats, per Vulkan
// vkCmdCopyImage VUID-srcImage-01548. B8/R8 swapchain bytes are sampled through
// the corresponding SRGB view so Gaussian filtering happens in linear RGB.
// Final RGBA16F sampling and blending also expect a linear-color ImGui pass.
#include <vulkan/vulkan.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "overlay_blur_spv.h"

namespace cs2_camera_blur {

struct InitInfo {
    VkPhysicalDevice physical{};
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties{};
    PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties{};
    VkExtent2D extent{};
    VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
    std::uint32_t imageCount = 0;
    // Match an encoded-space CSS black backdrop while the ImGui target blends
    // in linear RGB. Applied once, after the vertical Gaussian pass.
    float encodedDimAlpha = 0;
    std::function<VkDescriptorSet(VkSampler, VkImageView, VkImageLayout)> AddTexture;
    std::function<void(VkDescriptorSet)> RemoveTexture;
};

class Backdrop {
public:
    Backdrop() = default;
    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;
    ~Backdrop() { Destroy(); }

    bool Initialize(const InitInfo& info) {
        Destroy();
        error_.clear();
        info_ = info;
        try {
            if (!info.device || !info.physical || !info.gdpa || !info.getMemoryProperties ||
                !info.getFormatProperties || !info.AddTexture || !info.RemoveTexture)
                throw std::runtime_error("backdrop requires Vulkan dispatch and texture callbacks");
            if (!info.extent.width || !info.extent.height || !info.imageCount || info.imageCount > 32)
                throw std::runtime_error("invalid backdrop image extent/count");
            if (!std::isfinite(info.encodedDimAlpha) || info.encodedDimAlpha < 0 || info.encodedDimAlpha > 1)
                throw std::runtime_error("encoded backdrop dim alpha must be in [0,1]");
            captureFormat_ = CaptureFormat(info.sourceFormat);
            if (captureFormat_ == VK_FORMAT_UNDEFINED)
                throw std::runtime_error("backdrop supports B8/R8 8-bit SDR swapchains only");
            LoadDispatch();
            RequireFormat(captureFormat_, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
            RequireFormat(info.sourceFormat, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
            RequireFormat(OutputFormat(), VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
            info.getMemoryProperties(info.physical, &memory_);
            MakeSharedObjects();
            frames_.resize(info.imageCount);
            std::vector<VkDescriptorSetLayout> layouts(info.imageCount * 2, setLayout_);
            std::vector<VkDescriptorSet> sets(layouts.size());
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = descriptorPool_;
            allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
            allocate.pSetLayouts = layouts.data();
            Check(vk_.AllocateDescriptorSets(info.device, &allocate, sets.data()), "allocate backdrop descriptors");
            for (std::uint32_t i = 0; i < info.imageCount; ++i) {
                auto& frame = frames_[i];
                MakeImage(frame.capture, captureFormat_, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false);
                MakeImage(frame.horizontal, OutputFormat(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, true);
                MakeImage(frame.result, OutputFormat(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true);
                frame.captureSet = sets[i * 2];
                frame.horizontalSet = sets[i * 2 + 1];
                WriteSet(frame.captureSet, frame.capture.view);
                WriteSet(frame.horizontalSet, frame.horizontal.view);
                frame.texture = info.AddTexture(sampler_, frame.result.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (!frame.texture) throw std::runtime_error("register backdrop ImGui texture failed");
            }
            ready_ = true;
            return true;
        } catch (const std::exception& exception) {
            error_ = exception.what();
            Destroy();
            return false;
        }
    }

    bool Record(VkCommandBuffer command, std::uint32_t index, VkImage source,
                VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        if (!ready_ || index >= frames_.size() || !command || !source) return false;
        auto& frame = frames_[index];
        const auto oldCapture = frame.recorded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        Barrier(command, source, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        Barrier(command, frame.capture.image, oldCapture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            frame.recorded ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            frame.recorded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {info_.extent.width, info_.extent.height, 1};
        vk_.CmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame.capture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        Barrier(command, frame.capture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        Pass(command, frame.horizontal.framebuffer, frame.captureSet, 1.f / info_.extent.width, 0, 0);
        Pass(command, frame.result.framebuffer, frame.horizontalSet, 0, 1.f / info_.extent.height, info_.encodedDimAlpha);
        frame.recorded = true;
        return true;
    }

    VkDescriptorSet Texture(std::uint32_t index) const {
        return ready_ && index < frames_.size() ? frames_[index].texture : VK_NULL_HANDLE;
    }
    // Diagnostics/readback only. Record leaves this image in SHADER_READ_ONLY.
    VkImage OutputImage(std::uint32_t index) const {
        return ready_ && index < frames_.size() ? frames_[index].result.image : VK_NULL_HANDLE;
    }
    static constexpr VkFormat OutputFormat() { return VK_FORMAT_R16G16B16A16_SFLOAT; }
    bool Ready() const { return ready_; }
    const std::string& Error() const { return error_; }

    void Destroy() noexcept {
        ready_ = false;
        if (info_.device) {
            for (auto& frame : frames_) {
                if (frame.texture && info_.RemoveTexture) {
                    try { info_.RemoveTexture(frame.texture); } catch (...) {}
                }
                DropImage(frame.result);
                DropImage(frame.horizontal);
                DropImage(frame.capture);
            }
            if (pipeline_ && vk_.DestroyPipeline) vk_.DestroyPipeline(info_.device, pipeline_, nullptr);
            if (pipelineLayout_ && vk_.DestroyPipelineLayout) vk_.DestroyPipelineLayout(info_.device, pipelineLayout_, nullptr);
            if (renderPass_ && vk_.DestroyRenderPass) vk_.DestroyRenderPass(info_.device, renderPass_, nullptr);
            if (descriptorPool_ && vk_.DestroyDescriptorPool) vk_.DestroyDescriptorPool(info_.device, descriptorPool_, nullptr);
            if (setLayout_ && vk_.DestroyDescriptorSetLayout) vk_.DestroyDescriptorSetLayout(info_.device, setLayout_, nullptr);
            if (sampler_ && vk_.DestroySampler) vk_.DestroySampler(info_.device, sampler_, nullptr);
        }
        frames_.clear();
        pipeline_ = {}; pipelineLayout_ = {}; renderPass_ = {}; descriptorPool_ = {}; setLayout_ = {}; sampler_ = {};
        info_ = {};
        vk_ = {};
    }

private:
    struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkFramebuffer framebuffer{}; };
    struct Frame {
        Image capture, horizontal, result;
        VkDescriptorSet captureSet{}, horizontalSet{}, texture{};
        bool recorded = false;
    };
    struct Dispatch {
#define BLUR_FN(name) PFN_vk##name name{};
        BLUR_FN(CreateImage) BLUR_FN(DestroyImage) BLUR_FN(GetImageMemoryRequirements)
        BLUR_FN(AllocateMemory) BLUR_FN(FreeMemory) BLUR_FN(BindImageMemory)
        BLUR_FN(CreateImageView) BLUR_FN(DestroyImageView) BLUR_FN(CreateFramebuffer) BLUR_FN(DestroyFramebuffer)
        BLUR_FN(CreateSampler) BLUR_FN(DestroySampler) BLUR_FN(CreateDescriptorSetLayout) BLUR_FN(DestroyDescriptorSetLayout)
        BLUR_FN(CreateDescriptorPool) BLUR_FN(DestroyDescriptorPool) BLUR_FN(AllocateDescriptorSets) BLUR_FN(UpdateDescriptorSets)
        BLUR_FN(CreateRenderPass) BLUR_FN(DestroyRenderPass) BLUR_FN(CreatePipelineLayout) BLUR_FN(DestroyPipelineLayout)
        BLUR_FN(CreateShaderModule) BLUR_FN(DestroyShaderModule) BLUR_FN(CreateGraphicsPipelines) BLUR_FN(DestroyPipeline)
        BLUR_FN(CmdPipelineBarrier) BLUR_FN(CmdCopyImage) BLUR_FN(CmdBeginRenderPass) BLUR_FN(CmdEndRenderPass)
        BLUR_FN(CmdBindPipeline) BLUR_FN(CmdBindDescriptorSets) BLUR_FN(CmdPushConstants)
        BLUR_FN(CmdSetViewport) BLUR_FN(CmdSetScissor) BLUR_FN(CmdDraw)
#undef BLUR_FN
    } vk_;
    InitInfo info_;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkFormat captureFormat_ = VK_FORMAT_UNDEFINED;
    VkSampler sampler_{};
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool descriptorPool_{};
    VkRenderPass renderPass_{};
    VkPipelineLayout pipelineLayout_{};
    VkPipeline pipeline_{};
    std::vector<Frame> frames_;
    bool ready_ = false;
    std::string error_;

    static VkFormat CaptureFormat(VkFormat format) {
        if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_SRGB;
        if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_SRGB;
        return VK_FORMAT_UNDEFINED;
    }
    static void Check(VkResult result, const char* operation) {
        if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ")");
    }
    void RequireFormat(VkFormat format, VkFormatFeatureFlags features) const {
        VkFormatProperties properties{};
        info_.getFormatProperties(info_.physical, format, &properties);
        if ((properties.optimalTilingFeatures & features) != features)
            throw std::runtime_error("backdrop format features unavailable for format " + std::to_string(format));
    }
    void LoadDispatch() {
#define BLUR_LOAD(name) vk_.name = reinterpret_cast<PFN_vk##name>(info_.gdpa(info_.device, "vk" #name)); \
        if (!vk_.name) throw std::runtime_error("missing Vulkan backdrop function vk" #name);
        BLUR_LOAD(CreateImage) BLUR_LOAD(DestroyImage) BLUR_LOAD(GetImageMemoryRequirements)
        BLUR_LOAD(AllocateMemory) BLUR_LOAD(FreeMemory) BLUR_LOAD(BindImageMemory)
        BLUR_LOAD(CreateImageView) BLUR_LOAD(DestroyImageView) BLUR_LOAD(CreateFramebuffer) BLUR_LOAD(DestroyFramebuffer)
        BLUR_LOAD(CreateSampler) BLUR_LOAD(DestroySampler) BLUR_LOAD(CreateDescriptorSetLayout) BLUR_LOAD(DestroyDescriptorSetLayout)
        BLUR_LOAD(CreateDescriptorPool) BLUR_LOAD(DestroyDescriptorPool) BLUR_LOAD(AllocateDescriptorSets) BLUR_LOAD(UpdateDescriptorSets)
        BLUR_LOAD(CreateRenderPass) BLUR_LOAD(DestroyRenderPass) BLUR_LOAD(CreatePipelineLayout) BLUR_LOAD(DestroyPipelineLayout)
        BLUR_LOAD(CreateShaderModule) BLUR_LOAD(DestroyShaderModule) BLUR_LOAD(CreateGraphicsPipelines) BLUR_LOAD(DestroyPipeline)
        BLUR_LOAD(CmdPipelineBarrier) BLUR_LOAD(CmdCopyImage) BLUR_LOAD(CmdBeginRenderPass) BLUR_LOAD(CmdEndRenderPass)
        BLUR_LOAD(CmdBindPipeline) BLUR_LOAD(CmdBindDescriptorSets) BLUR_LOAD(CmdPushConstants)
        BLUR_LOAD(CmdSetViewport) BLUR_LOAD(CmdSetScissor) BLUR_LOAD(CmdDraw)
#undef BLUR_LOAD
    }
    void MakeImage(Image& image, VkFormat format, VkImageUsageFlags usage, bool framebuffer) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D; create.format = format;
        create.extent = {info_.extent.width, info_.extent.height, 1};
        create.mipLevels = 1; create.arrayLayers = 1; create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vk_.CreateImage(info_.device, &create, nullptr, &image.image), "create backdrop image");
        VkMemoryRequirements requirements{};
        vk_.GetImageMemoryRequirements(info_.device, image.image, &requirements);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        if (type == UINT32_MAX) throw std::runtime_error("no device-local memory for backdrop");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
        Check(vk_.AllocateMemory(info_.device, &allocate, nullptr, &image.memory), "allocate backdrop image memory");
        Check(vk_.BindImageMemory(info_.device, image.image, image.memory, 0), "bind backdrop image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vk_.CreateImageView(info_.device, &view, nullptr, &image.view), "create backdrop image view");
        if (framebuffer) {
            VkFramebufferCreateInfo target{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            target.renderPass = renderPass_; target.attachmentCount = 1; target.pAttachments = &image.view;
            target.width = info_.extent.width; target.height = info_.extent.height; target.layers = 1;
            Check(vk_.CreateFramebuffer(info_.device, &target, nullptr, &image.framebuffer), "create backdrop framebuffer");
        }
    }
    void DropImage(Image& image) noexcept {
        if (image.framebuffer && vk_.DestroyFramebuffer) vk_.DestroyFramebuffer(info_.device, image.framebuffer, nullptr);
        if (image.view && vk_.DestroyImageView) vk_.DestroyImageView(info_.device, image.view, nullptr);
        if (image.image && vk_.DestroyImage) vk_.DestroyImage(info_.device, image.image, nullptr);
        if (image.memory && vk_.FreeMemory) vk_.FreeMemory(info_.device, image.memory, nullptr);
        image = {};
    }
    void WriteSet(VkDescriptorSet set, VkImageView view) {
        VkDescriptorImageInfo image{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &image;
        vk_.UpdateDescriptorSets(info_.device, 1, &write, 0, nullptr);
    }
    void MakeSharedObjects() {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0; sampler.maxAnisotropy = 1;
        Check(vk_.CreateSampler(info_.device, &sampler, nullptr, &sampler_), "create backdrop sampler");
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 1; set.pBindings = &binding;
        Check(vk_.CreateDescriptorSetLayout(info_.device, &set, nullptr, &setLayout_), "create backdrop descriptor layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info_.imageCount * 2};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = info_.imageCount * 2; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        Check(vk_.CreateDescriptorPool(info_.device, &pool, nullptr, &descriptorPool_), "create backdrop descriptor pool");

        VkAttachmentDescription attachment{};
        attachment.format = OutputFormat(); attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT; dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0; dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        render.attachmentCount = 1; render.pAttachments = &attachment; render.subpassCount = 1; render.pSubpasses = &subpass;
        render.dependencyCount = static_cast<std::uint32_t>(dependencies.size()); render.pDependencies = dependencies.data();
        Check(vk_.CreateRenderPass(info_.device, &render, nullptr, &renderPass_), "create backdrop render pass");
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 3};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &setLayout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        Check(vk_.CreatePipelineLayout(info_.device, &layout, nullptr, &pipelineLayout_), "create backdrop pipeline layout");
        MakePipeline();
    }
    void MakePipeline() {
        VkShaderModule vertex{}, fragment{};
        try {
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize = sizeof(spirv::Vertex); module.pCode = spirv::Vertex;
            Check(vk_.CreateShaderModule(info_.device, &module, nullptr, &vertex), "create backdrop vertex shader");
            module.codeSize = sizeof(spirv::Fragment); module.pCode = spirv::Fragment;
            Check(vk_.CreateShaderModule(info_.device, &module, nullptr, &fragment), "create backdrop fragment shader");
            std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
            for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment;
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1; viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState color{};
            color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1; blend.pAttachments = &color;
            const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = static_cast<std::uint32_t>(states.size()); dynamic.pDynamicStates = states.data();
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline.stageCount = static_cast<std::uint32_t>(stages.size()); pipeline.pStages = stages.data();
            pipeline.pVertexInputState = &input; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport;
            pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &samples; pipeline.pColorBlendState = &blend;
            pipeline.pDynamicState = &dynamic; pipeline.layout = pipelineLayout_; pipeline.renderPass = renderPass_;
            Check(vk_.CreateGraphicsPipelines(info_.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_), "create backdrop pipeline");
        } catch (...) {
            if (fragment) vk_.DestroyShaderModule(info_.device, fragment, nullptr);
            if (vertex) vk_.DestroyShaderModule(info_.device, vertex, nullptr);
            throw;
        }
        vk_.DestroyShaderModule(info_.device, fragment, nullptr);
        vk_.DestroyShaderModule(info_.device, vertex, nullptr);
    }
    void Barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after,
                 VkAccessFlags sourceAccess, VkAccessFlags targetAccess,
                 VkPipelineStageFlags sourceStage, VkPipelineStageFlags targetStage) const {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = targetAccess;
        barrier.oldLayout = before; barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_.CmdPipelineBarrier(command, sourceStage, targetStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    void Pass(VkCommandBuffer command, VkFramebuffer framebuffer, VkDescriptorSet input, float x, float y, float dimAlpha) const {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = renderPass_; begin.framebuffer = framebuffer; begin.renderArea.extent = info_.extent;
        vk_.CmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vk_.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vk_.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &input, 0, nullptr);
        // GLSL push offsets: vec2 at 0, float at 8. No trailing padding is read.
        const std::array<float, 3> direction{x, y, dimAlpha};
        vk_.CmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(direction), direction.data());
        VkViewport viewport{0, 0, static_cast<float>(info_.extent.width), static_cast<float>(info_.extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, info_.extent};
        vk_.CmdSetViewport(command, 0, 1, &viewport); vk_.CmdSetScissor(command, 0, 1, &scissor);
        vk_.CmdDraw(command, 3, 1, 0, 0);
        vk_.CmdEndRenderPass(command);
    }
};

} // namespace cs2_camera_blur
