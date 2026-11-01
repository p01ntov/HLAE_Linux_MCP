#pragma once

// Original color-only Linux Vulkan lens adapter. Does not inspect game memory,
// import a Vulkan loader, own swapchain views, submit commands or wait for GPU.
//
// Contract: initialize after negotiating TRANSFER_SRC + COLOR_ATTACHMENT on the
// swapchain; wait that swap image's previous submission fence before Record;
// call outside render passes while waiting the application's present semaphores
// at ALL_COMMANDS; destroy after all GPU uses and before destroying sourceViews.
// Record modifies the supplied swap image and restores sourceLayout. Capture
// that image after Record for exact preview/export parity. ImGui is rendered
// afterwards so camera distortion never warps editor text or controls.
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include "lens_settings.h"
#include "lens_effects_spv.h"

namespace cs2_camera_lens {
struct InitInfo {
    VkPhysicalDevice physical{};
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties{};
    PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties{};
    VkExtent2D extent{};
    VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
    // Each view must belong to the matching source passed to Record.
    std::vector<VkImageView> sourceViews;
};

class Effects {
public:
    Effects() = default;
    Effects(const Effects&) = delete;
    Effects& operator=(const Effects&) = delete;
    ~Effects() { Destroy(); }

    bool Initialize(const InitInfo& info) {
        Destroy();
        error_.clear();
        info_ = info;
        try {
            if (!info.device || !info.physical || !info.gdpa || !info.getMemoryProperties || !info.getFormatProperties)
                throw std::runtime_error("lens requires complete Vulkan dispatch");
            if (!info.extent.width || !info.extent.height || info.sourceViews.empty() || info.sourceViews.size() > 32)
                throw std::runtime_error("invalid lens image extent/count");
            captureFormat_ = CaptureFormat(info.sourceFormat);
            if (captureFormat_ == VK_FORMAT_UNDEFINED)
                throw std::runtime_error("lens requires an SDR B8/R8 UNORM or SRGB swapchain");
            LoadDispatch();
            RequireFormat(captureFormat_, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
            RequireFormat(info.sourceFormat, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
            info.getMemoryProperties(info.physical, &memory_);
            MakeSharedObjects();
            frames_.resize(info.sourceViews.size());
            std::vector<VkDescriptorSetLayout> layouts(frames_.size(), setLayout_);
            std::vector<VkDescriptorSet> sets(frames_.size());
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = descriptorPool_;
            allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
            allocate.pSetLayouts = layouts.data();
            Check(vk_.AllocateDescriptorSets(info.device, &allocate, sets.data()), "allocate lens descriptors");
            for (std::size_t index = 0; index < frames_.size(); ++index) {
                if (!info.sourceViews[index]) throw std::runtime_error("missing lens target view");
                auto& frame = frames_[index];
                MakeImage(frame.capture, captureFormat_, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
                frame.captureSet = sets[index];
                WriteSet(frame.captureSet, frame.capture.view);
                VkFramebufferCreateInfo target{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                target.renderPass = renderPass_; target.attachmentCount = 1;
                target.pAttachments = &info.sourceViews[index];
                target.width = info.extent.width; target.height = info.extent.height; target.layers = 1;
                Check(vk_.CreateFramebuffer(info.device, &target, nullptr, &frame.target), "create lens target framebuffer");
            }
            ready_ = true;
            return true;
        } catch (const std::exception& exception) {
            error_ = exception.what();
            Destroy();
            return false;
        }
    }

    bool Record(VkCommandBuffer command, std::uint32_t index, VkImage source, const Settings& settings,
                VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        // Disabled/neutral/invalid requests issue no GPU commands and leave the
        // original pixels bit-identical. Cancellation needs no state rollback.
        if (!ready_ || index >= frames_.size() || !command || !source || !settings.Active()) return false;
        auto& frame = frames_[index];
        Barrier(command, source, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // The snapshot is completely overwritten. UNDEFINED is deliberate:
        // canceled command buffers cannot poison a CPU-side layout tracker.
        Barrier(command, frame.capture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {info_.extent.width, info_.extent.height, 1};
        vk_.CmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame.capture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        Barrier(command, frame.capture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        Pass(command, frame.target, frame.captureSet, settings);
        Barrier(command, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, sourceLayout,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        return true;
    }
    bool Ready() const noexcept { return ready_; }
    const std::string& Error() const noexcept { return error_; }
    void Destroy() noexcept {
        ready_ = false;
        if (info_.device) {
            for (auto& frame : frames_) {
                if (frame.target && vk_.DestroyFramebuffer) vk_.DestroyFramebuffer(info_.device, frame.target, nullptr);
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
        captureFormat_ = VK_FORMAT_UNDEFINED; info_ = {}; vk_ = {};
    }

private:
    struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; };
    struct Frame { Image capture; VkDescriptorSet captureSet{}; VkFramebuffer target{}; };
    struct Dispatch {
#define LENS_FN(name) PFN_vk##name name{};
        LENS_FN(CreateImage) LENS_FN(DestroyImage) LENS_FN(GetImageMemoryRequirements)
        LENS_FN(AllocateMemory) LENS_FN(FreeMemory) LENS_FN(BindImageMemory)
        LENS_FN(CreateImageView) LENS_FN(DestroyImageView) LENS_FN(CreateFramebuffer) LENS_FN(DestroyFramebuffer)
        LENS_FN(CreateSampler) LENS_FN(DestroySampler) LENS_FN(CreateDescriptorSetLayout) LENS_FN(DestroyDescriptorSetLayout)
        LENS_FN(CreateDescriptorPool) LENS_FN(DestroyDescriptorPool) LENS_FN(AllocateDescriptorSets) LENS_FN(UpdateDescriptorSets)
        LENS_FN(CreateRenderPass) LENS_FN(DestroyRenderPass) LENS_FN(CreatePipelineLayout) LENS_FN(DestroyPipelineLayout)
        LENS_FN(CreateShaderModule) LENS_FN(DestroyShaderModule) LENS_FN(CreateGraphicsPipelines) LENS_FN(DestroyPipeline)
        LENS_FN(CmdPipelineBarrier) LENS_FN(CmdCopyImage) LENS_FN(CmdBeginRenderPass) LENS_FN(CmdEndRenderPass)
        LENS_FN(CmdBindPipeline) LENS_FN(CmdBindDescriptorSets) LENS_FN(CmdPushConstants)
        LENS_FN(CmdSetViewport) LENS_FN(CmdSetScissor) LENS_FN(CmdDraw)
#undef LENS_FN
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
            throw std::runtime_error("lens format features unavailable for format " + std::to_string(format));
    }
    void LoadDispatch() {
#define LENS_LOAD(name) vk_.name = reinterpret_cast<PFN_vk##name>(info_.gdpa(info_.device, "vk" #name)); \
        if (!vk_.name) throw std::runtime_error("missing Vulkan lens function vk" #name);
        LENS_LOAD(CreateImage) LENS_LOAD(DestroyImage) LENS_LOAD(GetImageMemoryRequirements)
        LENS_LOAD(AllocateMemory) LENS_LOAD(FreeMemory) LENS_LOAD(BindImageMemory)
        LENS_LOAD(CreateImageView) LENS_LOAD(DestroyImageView) LENS_LOAD(CreateFramebuffer) LENS_LOAD(DestroyFramebuffer)
        LENS_LOAD(CreateSampler) LENS_LOAD(DestroySampler) LENS_LOAD(CreateDescriptorSetLayout) LENS_LOAD(DestroyDescriptorSetLayout)
        LENS_LOAD(CreateDescriptorPool) LENS_LOAD(DestroyDescriptorPool) LENS_LOAD(AllocateDescriptorSets) LENS_LOAD(UpdateDescriptorSets)
        LENS_LOAD(CreateRenderPass) LENS_LOAD(DestroyRenderPass) LENS_LOAD(CreatePipelineLayout) LENS_LOAD(DestroyPipelineLayout)
        LENS_LOAD(CreateShaderModule) LENS_LOAD(DestroyShaderModule) LENS_LOAD(CreateGraphicsPipelines) LENS_LOAD(DestroyPipeline)
        LENS_LOAD(CmdPipelineBarrier) LENS_LOAD(CmdCopyImage) LENS_LOAD(CmdBeginRenderPass) LENS_LOAD(CmdEndRenderPass)
        LENS_LOAD(CmdBindPipeline) LENS_LOAD(CmdBindDescriptorSets) LENS_LOAD(CmdPushConstants)
        LENS_LOAD(CmdSetViewport) LENS_LOAD(CmdSetScissor) LENS_LOAD(CmdDraw)
#undef LENS_LOAD
    }
    void MakeImage(Image& image, VkFormat format, VkImageUsageFlags usage) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D; create.format = format;
        create.extent = {info_.extent.width, info_.extent.height, 1};
        create.mipLevels = 1; create.arrayLayers = 1; create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vk_.CreateImage(info_.device, &create, nullptr, &image.image), "create lens image");
        VkMemoryRequirements requirements{};
        vk_.GetImageMemoryRequirements(info_.device, image.image, &requirements);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        if (type == UINT32_MAX) throw std::runtime_error("no device-local memory for lens");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
        Check(vk_.AllocateMemory(info_.device, &allocate, nullptr, &image.memory), "allocate lens image memory");
        Check(vk_.BindImageMemory(info_.device, image.image, image.memory, 0), "bind lens image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vk_.CreateImageView(info_.device, &view, nullptr, &image.view), "create lens image view");
    }
    void DropImage(Image& image) noexcept {
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
        Check(vk_.CreateSampler(info_.device, &sampler, nullptr, &sampler_), "create lens sampler");
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 1; set.pBindings = &binding;
        Check(vk_.CreateDescriptorSetLayout(info_.device, &set, nullptr, &setLayout_), "create lens descriptor layout");
        const auto count = static_cast<std::uint32_t>(info_.sourceViews.size());
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = count; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        Check(vk_.CreateDescriptorPool(info_.device, &pool, nullptr, &descriptorPool_), "create lens descriptor pool");
        VkAttachmentDescription attachment{};
        attachment.format = info_.sourceFormat; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
        VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        render.attachmentCount = 1; render.pAttachments = &attachment; render.subpassCount = 1; render.pSubpasses = &subpass;
        Check(vk_.CreateRenderPass(info_.device, &render, nullptr, &renderPass_), "create lens render pass");
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 6};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &setLayout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        Check(vk_.CreatePipelineLayout(info_.device, &layout, nullptr, &pipelineLayout_), "create lens pipeline layout");
        MakePipeline();
    }
    void MakePipeline() {
        VkShaderModule vertex{}, fragment{};
        try {
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize = sizeof(spirv::Vertex); module.pCode = spirv::Vertex;
            Check(vk_.CreateShaderModule(info_.device, &module, nullptr, &vertex), "create lens vertex shader");
            module.codeSize = sizeof(spirv::Fragment); module.pCode = spirv::Fragment;
            Check(vk_.CreateShaderModule(info_.device, &module, nullptr, &fragment), "create lens fragment shader");
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
            Check(vk_.CreateGraphicsPipelines(info_.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_), "create lens pipeline");
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
    void Pass(VkCommandBuffer command, VkFramebuffer framebuffer, VkDescriptorSet input, const Settings& settings) const {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = renderPass_; begin.framebuffer = framebuffer; begin.renderArea.extent = info_.extent;
        vk_.CmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vk_.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vk_.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &input, 0, nullptr);
        const bool encoded = info_.sourceFormat == VK_FORMAT_R8G8B8A8_UNORM || info_.sourceFormat == VK_FORMAT_B8G8R8A8_UNORM;
        const std::array<float, 6> parameters{1.f / info_.extent.width, 1.f / info_.extent.height,
            settings.distortion, settings.vignette, settings.chromatic, encoded ? 1.f : 0.f};
        vk_.CmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters), parameters.data());
        VkViewport viewport{0, 0, static_cast<float>(info_.extent.width), static_cast<float>(info_.extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, info_.extent};
        vk_.CmdSetViewport(command, 0, 1, &viewport); vk_.CmdSetScissor(command, 0, 1, &scissor);
        vk_.CmdDraw(command, 3, 1, 0, 0);
        vk_.CmdEndRenderPass(command);
    }
};
} // namespace cs2_camera_lens
