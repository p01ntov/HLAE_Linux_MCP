#include "../depth_provenance.h"
#include <cassert>
#include <iostream>

using namespace cs2_camera_depth;
template<class T> T Handle(std::uintptr_t n) { return reinterpret_cast<T>(n); }

struct Scene {
    Provenance tracker;
    const VkImage image = Handle<VkImage>(1);
    const VkImageView view = Handle<VkImageView>(2);
    const VkRenderPass pass = Handle<VkRenderPass>(3);
    const VkFramebuffer framebuffer = Handle<VkFramebuffer>(4);
    const VkCommandPool pool = Handle<VkCommandPool>(5);
    const VkCommandBuffer command = Handle<VkCommandBuffer>(6);
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    Scene() {
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = {1920, 1080, 1}; imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        tracker.CreateImage(image, imageInfo);
        MakeView();
        VkAttachmentDescription attachment{};
        attachment.format = imageInfo.format; attachment.samples = imageInfo.samples;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference depth{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pDepthStencilAttachment = &depth;
        VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        render.attachmentCount = 1; render.pAttachments = &attachment;
        render.subpassCount = 1; render.pSubpasses = &subpass;
        tracker.CreatePass(pass, render);
        VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        frame.renderPass = pass; frame.width = 1920; frame.height = 1080; frame.layers = 1;
        frame.attachmentCount = 1; frame.pAttachments = &view;
        tracker.CreateFramebuffer(framebuffer, frame);
        tracker.CreatePool(pool, 7); tracker.Allocate(pool, 1, &command);
    }
    void MakeView() {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image; info.format = imageInfo.format; info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        tracker.CreateView(view, info);
    }
    void Record(VkCommandBuffer target = VK_NULL_HANDLE) {
        if (!target) target = command;
        tracker.Begin(target);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass; begin.framebuffer = framebuffer; begin.renderArea.extent = {1920, 1080};
        tracker.BeginPass(target, begin); tracker.EndPass(target); tracker.End(target);
    }
};

int main() {
    {
        Scene scene; scene.Record();
        assert(scene.tracker.Take().empty()); // recorded is not submitted
        scene.tracker.Submit(1, &scene.command);
        auto events = scene.tracker.Take(); assert(events.size() == 1);
        const auto& event = events.front();
        assert(event.image.extent.width == 1920 && event.image.format == VK_FORMAT_D32_SFLOAT);
        assert(event.ended && !event.dynamic && event.family == 7);
        assert(event.store == VK_ATTACHMENT_STORE_OP_STORE && event.after == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        assert(!(event.image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)); // never add a missing usage bit
        scene.tracker.Submit(1, &scene.command); scene.tracker.Submit(1, &scene.command);
        events = scene.tracker.Take(); assert(events.size() == 1 && events.front().observations == 2);
    }
    {
        Scene scene; scene.Record(); scene.tracker.Begin(scene.command);
        scene.tracker.End(scene.command); scene.tracker.Submit(1, &scene.command);
        assert(scene.tracker.Take().empty()); // abandoned commands cannot fabricate GPU evidence
        scene.Record(); scene.tracker.ResetPool(scene.pool); scene.tracker.Submit(1, &scene.command);
        assert(scene.tracker.Take().empty());
        scene.Record(); scene.tracker.Free(1, &scene.command); scene.tracker.Submit(1, &scene.command);
        assert(scene.tracker.Take().empty());
    }
    {
        Scene scene; scene.Record(); scene.tracker.DestroyImage(scene.image);
        scene.tracker.CreateImage(scene.image, scene.imageInfo); // driver reused the same integer handle
        scene.tracker.Submit(1, &scene.command); assert(scene.tracker.Take().empty());
        scene.Record(); scene.tracker.Submit(1, &scene.command); assert(scene.tracker.Take().empty()); // stale view
        scene.MakeView(); scene.Record(); scene.tracker.Submit(1, &scene.command);
        assert(scene.tracker.Take().size() == 1);
    }
    {
        Scene scene;
        const VkCommandBuffer child = Handle<VkCommandBuffer>(11);
        scene.tracker.Allocate(scene.pool, 1, &child); scene.Record(child);
        scene.tracker.Begin(scene.command); scene.tracker.Execute(scene.command, 1, &child);
        scene.tracker.End(scene.command); scene.tracker.Submit(1, &scene.command);
        assert(scene.tracker.Take().size() == 1);
        scene.tracker.DestroyPool(scene.pool); scene.tracker.Submit(1, &scene.command);
        assert(scene.tracker.Take().empty());
    }
    {
        Scene scene;
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT; info.attachmentCount = 1;
        info.width = 1920; info.height = 1080; info.layers = 1;
        scene.tracker.CreateFramebuffer(scene.framebuffer, info);
        VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
        attachments.attachmentCount = 1; attachments.pAttachments = &scene.view;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = scene.pass; begin.framebuffer = scene.framebuffer;
        begin.pNext = &attachments; begin.renderArea.extent = {1920, 1080};
        scene.tracker.Begin(scene.command); scene.tracker.BeginPass(scene.command, begin);
        scene.tracker.EndPass(scene.command); scene.tracker.End(scene.command);
        scene.tracker.Submit(1, &scene.command); assert(scene.tracker.Take().size() == 1);
    }
    {
        Scene scene;
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = scene.view; attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO}; render.pDepthAttachment = &attachment;
        render.renderArea.extent = {960, 540}; render.viewMask = 3;
        render.flags = VK_RENDERING_SUSPENDING_BIT;
        scene.tracker.Begin(scene.command); scene.tracker.BeginRendering(scene.command, render);
        scene.tracker.EndPass(scene.command); scene.tracker.End(scene.command);
        scene.tracker.Submit(1, &scene.command); const auto events = scene.tracker.Take();
        assert(events.size() == 1 && events.front().dynamic && events.front().continuation);
        assert(events.front().store == VK_ATTACHMENT_STORE_OP_DONT_CARE && events.front().viewMask == 3);
        assert(events.front().renderArea.extent.width == 960); // resolution heuristic must retain actual area
    }
    {
        Scene scene;
        VkAttachmentDescription2 attachment{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        attachment.format = VK_FORMAT_D32_SFLOAT; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkAttachmentReference2 depth{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2}; depth.attachment = 0;
        depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSubpassDescription2 subpasses[2]{{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2}, {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2}};
        subpasses[1].pDepthStencilAttachment = &depth; subpasses[1].viewMask = 5;
        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info.attachmentCount = 1; info.pAttachments = &attachment; info.subpassCount = 2; info.pSubpasses = subpasses;
        scene.tracker.CreatePass(scene.pass, info);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = scene.pass; begin.framebuffer = scene.framebuffer;
        scene.tracker.Begin(scene.command); scene.tracker.BeginPass(scene.command, begin);
        scene.tracker.NextSubpass(scene.command); scene.tracker.EndPass(scene.command); scene.tracker.End(scene.command);
        scene.tracker.Submit(1, &scene.command); const auto events = scene.tracker.Take();
        assert(events.size() == 1 && events.front().subpass == 1 && events.front().viewMask == 5);
    }
    std::cout << "Depth provenance: submission, reset, handle lifetime, secondary, imageless, dynamic, renderpass2 passed\n";
}
