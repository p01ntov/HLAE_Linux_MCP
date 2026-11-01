#pragma once

// Passive Vulkan attachment provenance. No Vulkan commands, image-usage changes,
// guessed engine offsets, or color-to-depth reconstruction are performed here.
// A depth attachment with the presentation extent is a candidate, not proof of
// the main camera depth buffer. The optional layer hooks report that distinction.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cs2_camera_depth {

inline bool IsDepth(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
        format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM_S8_UINT ||
        format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

struct Image {
    VkImage handle{};
    std::uint64_t generation = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE;
    std::uint32_t mipLevels = 0, layers = 0;
};

struct View {
    VkImageView handle{};
    VkImage image{};
    std::uint64_t imageGeneration = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageSubresourceRange range{};
};

struct Attachment {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkImageLayout initial = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout final = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Subpass {
    std::uint32_t depth = VK_ATTACHMENT_UNUSED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t viewMask = 0;
};

struct RenderPass {
    std::vector<Attachment> attachments;
    std::vector<Subpass> subpasses;
};

struct Framebuffer {
    std::vector<VkImageView> attachments;
    VkExtent2D extent{};
    std::uint32_t layers = 0;
    bool imageless = false;
};

struct Observation {
    Image image;
    View view;
    VkRect2D renderArea{};
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkImageLayout during = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout after = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t subpass = 0, viewMask = 0, family = VK_QUEUE_FAMILY_IGNORED;
    std::uint64_t submissions = 0, observations = 0;
    bool dynamic = false, ended = false, continuation = false;
};

// Only externally synchronized by the owning device's diagnostic mutex.
// Copies Vk*CreateInfo data: the application may free its arrays on return.
class Provenance {
public:
    static constexpr std::size_t MaxObjects = 65536, MaxEvents = 4096;

    void CreateImage(VkImage image, const VkImageCreateInfo& info) {
        DestroyImage(image);
        if (!IsDepth(info.format) || !(info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return;
        if (images_.size() >= MaxObjects) { ++dropped_; return; }
        images_[image] = {image, ++generation_, info.format, info.extent, info.usage,
            info.flags, info.samples, info.tiling, info.imageType, info.sharingMode, info.mipLevels, info.arrayLayers};
    }
    void DestroyImage(VkImage image) { images_.erase(image); }
    void CreateView(VkImageView view, const VkImageViewCreateInfo& info) {
        views_.erase(view);
        const auto image = images_.find(info.image);
        if (image == images_.end() || !(info.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
        if (views_.size() >= MaxObjects) { ++dropped_; return; }
        views_[view] = {view, info.image, image->second.generation, info.format, info.viewType, info.subresourceRange};
    }
    void DestroyView(VkImageView view) { views_.erase(view); }

    void CreatePass(VkRenderPass pass, const VkRenderPassCreateInfo& info) {
        RenderPass copy;
        for (std::uint32_t i = 0; i < info.attachmentCount; ++i) AddAttachment(copy, info.pAttachments[i]);
        for (std::uint32_t i = 0; i < info.subpassCount; ++i) {
            const auto* depth = info.pSubpasses[i].pDepthStencilAttachment;
            copy.subpasses.push_back({depth ? depth->attachment : VK_ATTACHMENT_UNUSED,
                depth ? depth->layout : VK_IMAGE_LAYOUT_UNDEFINED, 0});
        }
        for (auto* p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
                const auto& multi = *reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(p);
                for (std::size_t i = 0; i < copy.subpasses.size() && i < multi.subpassCount; ++i)
                    copy.subpasses[i].viewMask = multi.pViewMasks[i];
            }
        SavePass(pass, std::move(copy));
    }
    void CreatePass(VkRenderPass pass, const VkRenderPassCreateInfo2& info) {
        RenderPass copy;
        for (std::uint32_t i = 0; i < info.attachmentCount; ++i) AddAttachment(copy, info.pAttachments[i]);
        for (std::uint32_t i = 0; i < info.subpassCount; ++i) {
            const auto* depth = info.pSubpasses[i].pDepthStencilAttachment;
            copy.subpasses.push_back({depth ? depth->attachment : VK_ATTACHMENT_UNUSED,
                depth ? depth->layout : VK_IMAGE_LAYOUT_UNDEFINED, info.pSubpasses[i].viewMask});
        }
        SavePass(pass, std::move(copy));
    }
    void DestroyPass(VkRenderPass pass) { passes_.erase(pass); }
    void CreateFramebuffer(VkFramebuffer framebuffer, const VkFramebufferCreateInfo& info) {
        framebuffers_.erase(framebuffer);
        if (framebuffers_.size() >= MaxObjects) { ++dropped_; return; }
        Framebuffer copy;
        copy.extent = {info.width, info.height}; copy.layers = info.layers;
        copy.imageless = info.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
        if (!copy.imageless && info.attachmentCount)
            copy.attachments.assign(info.pAttachments, info.pAttachments + info.attachmentCount);
        framebuffers_[framebuffer] = std::move(copy);
    }
    void DestroyFramebuffer(VkFramebuffer framebuffer) { framebuffers_.erase(framebuffer); }

    void CreatePool(VkCommandPool pool, std::uint32_t family) {
        if (pools_.size() >= MaxObjects && !pools_.count(pool)) { ++dropped_; return; }
        pools_[pool] = family;
    }
    void ResetPool(VkCommandPool pool) {
        for (auto& pair : commands_) if (pair.second.pool == pool) Clear(pair.second);
    }
    void DestroyPool(VkCommandPool pool) {
        for (auto it = commands_.begin(); it != commands_.end();)
            if (it->second.pool == pool) it = commands_.erase(it); else ++it;
        pools_.erase(pool);
    }
    void Allocate(VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* commands) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (commands_.size() >= MaxObjects && !commands_.count(commands[i])) { ++dropped_; continue; }
            auto& item = commands_[commands[i]]; item = {}; item.pool = pool;
        }
    }
    void Free(std::uint32_t count, const VkCommandBuffer* commands) {
        for (std::uint32_t i = 0; i < count; ++i) commands_.erase(commands[i]);
    }
    void Begin(VkCommandBuffer command) { const auto it = commands_.find(command); if (it != commands_.end()) Clear(it->second); }
    void End(VkCommandBuffer command) { const auto it = commands_.find(command); if (it != commands_.end()) it->second.executable = true; }
    void BeginPass(VkCommandBuffer command, const VkRenderPassBeginInfo& info) {
        const auto it = commands_.find(command);
        if (it == commands_.end()) return;
        auto& cmd = it->second;
        cmd.pass = info.renderPass; cmd.framebuffer = info.framebuffer; cmd.subpass = 0; cmd.area = info.renderArea;
        cmd.views.clear();
        for (auto* p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                const auto& attachments = *reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(p);
                if (attachments.attachmentCount)
                    cmd.views.assign(attachments.pAttachments, attachments.pAttachments + attachments.attachmentCount);
            }
        ObserveSubpass(cmd);
    }
    void NextSubpass(VkCommandBuffer command) {
        const auto it = commands_.find(command);
        if (it == commands_.end()) return;
        ++it->second.subpass; ObserveSubpass(it->second);
    }
    void EndPass(VkCommandBuffer command) {
        const auto it = commands_.find(command);
        if (it == commands_.end()) return;
        auto& cmd = it->second;
        for (std::size_t i = cmd.passStart; i < cmd.events.size(); ++i) cmd.events[i].ended = true;
        cmd.passStart = cmd.events.size(); cmd.pass = {}; cmd.framebuffer = {}; cmd.views.clear();
    }
    void BeginRendering(VkCommandBuffer command, const VkRenderingInfo& info) {
        const auto it = commands_.find(command);
        if (it == commands_.end() || !info.pDepthAttachment || !info.pDepthAttachment->imageView) return;
        const auto& attachment = *info.pDepthAttachment;
        Observation event;
        if (!Resolve(attachment.imageView, event)) return;
        event.dynamic = true; event.renderArea = info.renderArea; event.viewMask = info.viewMask;
        event.load = attachment.loadOp; event.store = attachment.storeOp;
        event.during = event.after = attachment.imageLayout;
        event.continuation = (info.flags & (VK_RENDERING_RESUMING_BIT | VK_RENDERING_SUSPENDING_BIT)) != 0;
        Add(it->second, std::move(event));
    }
    void Execute(VkCommandBuffer primary, std::uint32_t count, const VkCommandBuffer* secondary) {
        const auto it = commands_.find(primary);
        if (it == commands_.end()) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto child = commands_.find(secondary[i]);
            if (child == commands_.end() || !child->second.executable) continue;
            for (const auto& event : child->second.events) Add(it->second, event);
        }
    }
    void Submit(std::uint32_t count, const VkCommandBuffer* commands) {
        ++submissions_;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto cmd = commands_.find(commands[i]);
            if (cmd == commands_.end() || !cmd->second.executable) continue;
            for (auto event : cmd->second.events) {
                const auto image = images_.find(event.image.handle);
                if (image == images_.end() || image->second.generation != event.image.generation) continue;
                const auto family = pools_.find(cmd->second.pool);
                event.family = family == pools_.end() ? VK_QUEUE_FAMILY_IGNORED : family->second;
                event.submissions = submissions_; event.observations = 1;
                auto found = std::find_if(observed_.begin(), observed_.end(), [&](const Observation& old) {
                    return old.image.generation == event.image.generation && old.view.handle == event.view.handle &&
                        old.subpass == event.subpass && old.dynamic == event.dynamic && old.family == event.family &&
                        old.during == event.during && old.after == event.after && old.store == event.store &&
                        old.load == event.load && old.viewMask == event.viewMask && old.ended == event.ended &&
                        old.continuation == event.continuation &&
                        old.renderArea.offset.x == event.renderArea.offset.x && old.renderArea.offset.y == event.renderArea.offset.y &&
                        old.renderArea.extent.width == event.renderArea.extent.width && old.renderArea.extent.height == event.renderArea.extent.height;
                });
                if (found != observed_.end()) { event.observations += found->observations; *found = event; }
                else if (observed_.size() < MaxEvents) observed_.push_back(event);
                else ++dropped_;
            }
        }
    }
    std::vector<Observation> Take() { std::vector<Observation> result; result.swap(observed_); return result; }
    std::size_t ImageCount() const { return images_.size(); }
    std::uint64_t Dropped() const { return dropped_; }
    std::uint64_t Submissions() const { return submissions_; }

private:
    struct Command {
        VkCommandPool pool{};
        VkRenderPass pass{};
        VkFramebuffer framebuffer{};
        std::uint32_t subpass = 0;
        VkRect2D area{};
        std::vector<VkImageView> views;
        std::vector<Observation> events;
        std::size_t passStart = 0;
        bool executable = false;
    };
    template<class T> static void AddAttachment(RenderPass& pass, const T& item) {
        pass.attachments.push_back({item.format, item.samples, item.loadOp, item.storeOp, item.initialLayout, item.finalLayout});
    }
    void SavePass(VkRenderPass pass, RenderPass data) {
        passes_.erase(pass);
        if (passes_.size() >= MaxObjects) { ++dropped_; return; }
        passes_[pass] = std::move(data);
    }
    static void Clear(Command& command) { const auto pool = command.pool; command = {}; command.pool = pool; }
    bool Resolve(VkImageView view, Observation& event) const {
        const auto found = views_.find(view);
        if (found == views_.end()) return false;
        const auto image = images_.find(found->second.image);
        if (image == images_.end() || image->second.generation != found->second.imageGeneration) return false;
        event.view = found->second; event.image = image->second;
        return true;
    }
    void Add(Command& command, Observation event) {
        if (command.events.size() < MaxEvents) command.events.push_back(std::move(event)); else ++dropped_;
    }
    void ObserveSubpass(Command& command) {
        const auto pass = passes_.find(command.pass);
        const auto framebuffer = framebuffers_.find(command.framebuffer);
        if (pass == passes_.end() || framebuffer == framebuffers_.end() || command.subpass >= pass->second.subpasses.size()) return;
        const auto& subpass = pass->second.subpasses[command.subpass];
        const auto& views = framebuffer->second.imageless ? command.views : framebuffer->second.attachments;
        if (subpass.depth == VK_ATTACHMENT_UNUSED || subpass.depth >= pass->second.attachments.size() || subpass.depth >= views.size()) return;
        Observation event;
        if (!Resolve(views[subpass.depth], event)) return;
        const auto& attachment = pass->second.attachments[subpass.depth];
        event.renderArea = command.area; event.subpass = command.subpass; event.viewMask = subpass.viewMask;
        event.load = attachment.load; event.store = attachment.store; event.during = subpass.layout; event.after = attachment.final;
        Add(command, std::move(event));
    }
    std::unordered_map<VkImage, Image> images_;
    std::unordered_map<VkImageView, View> views_;
    std::unordered_map<VkRenderPass, RenderPass> passes_;
    std::unordered_map<VkFramebuffer, Framebuffer> framebuffers_;
    std::unordered_map<VkCommandPool, std::uint32_t> pools_;
    std::unordered_map<VkCommandBuffer, Command> commands_;
    std::vector<Observation> observed_;
    std::uint64_t generation_ = 0, submissions_ = 0, dropped_ = 0;
};

} // namespace cs2_camera_depth
