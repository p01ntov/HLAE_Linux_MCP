#pragma once
// Included inside camera_overlay after Device/Find. These wrappers are only
// advertised when the explicit probe is enabled AND the next layer advertises
// the original function; diagnostics must not invent extension support.

template<class H> Device* DepthDevice(H handle) {
    const auto found = devices.find(Key(handle));
    return found == devices.end() ? nullptr : found->second.get();
}
#define DEPTH_LOOKUP(name, dispatch_handle, result) \
    std::lock_guard<std::recursive_mutex> lock(mutex); \
    auto* d = DepthDevice(dispatch_handle); \
    if (!d) return result; \
    auto* probe = d->depth_probe.get(); \
    const auto next = probe ? probe->name : reinterpret_cast<PFN_vk##name>(d->gdpa(d->handle, "vk" #name)); \
    if (!next) return result;
#define DEPTH_OBSERVE(expression) if (probe) probe->Observe([&](cs2_camera_depth::Provenance& model) { expression; });
#define DEPTH_CREATE(name, type, handle, expression) \
    VKAPI_ATTR VkResult VKAPI_CALL Depth##name(VkDevice device, const type* info, const VkAllocationCallbacks* allocator, handle* output) { \
        DEPTH_LOOKUP(name, device, VK_ERROR_INITIALIZATION_FAILED) \
        const auto result = next(device, info, allocator, output); \
        if (result == VK_SUCCESS) { DEPTH_OBSERVE(expression) } return result; \
    }
#define DEPTH_DESTROY(name, handle, expression) \
    VKAPI_ATTR void VKAPI_CALL Depth##name(VkDevice device, handle value, const VkAllocationCallbacks* allocator) { \
        DEPTH_LOOKUP(name, device, ) \
        DEPTH_OBSERVE(expression) next(device, value, allocator); \
    }
DEPTH_CREATE(CreateImage, VkImageCreateInfo, VkImage, model.CreateImage(*output, *info))
DEPTH_DESTROY(DestroyImage, VkImage, model.DestroyImage(value))
DEPTH_CREATE(CreateImageView, VkImageViewCreateInfo, VkImageView, model.CreateView(*output, *info))
DEPTH_DESTROY(DestroyImageView, VkImageView, model.DestroyView(value))
DEPTH_CREATE(CreateRenderPass, VkRenderPassCreateInfo, VkRenderPass, model.CreatePass(*output, *info))
DEPTH_CREATE(CreateRenderPass2, VkRenderPassCreateInfo2, VkRenderPass, model.CreatePass(*output, *info))
DEPTH_CREATE(CreateRenderPass2KHR, VkRenderPassCreateInfo2, VkRenderPass, model.CreatePass(*output, *info))
DEPTH_DESTROY(DestroyRenderPass, VkRenderPass, model.DestroyPass(value))
DEPTH_CREATE(CreateFramebuffer, VkFramebufferCreateInfo, VkFramebuffer, model.CreateFramebuffer(*output, *info))
DEPTH_DESTROY(DestroyFramebuffer, VkFramebuffer, model.DestroyFramebuffer(value))
DEPTH_CREATE(CreateCommandPool, VkCommandPoolCreateInfo, VkCommandPool, model.CreatePool(*output, info->queueFamilyIndex))
DEPTH_DESTROY(DestroyCommandPool, VkCommandPool, model.DestroyPool(value))

VKAPI_ATTR VkResult VKAPI_CALL DepthResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    DEPTH_LOOKUP(ResetCommandPool, device, VK_ERROR_INITIALIZATION_FAILED)
    const auto result = next(device, pool, flags);
    if (result == VK_SUCCESS) { DEPTH_OBSERVE(model.ResetPool(pool)) } return result;
}
VKAPI_ATTR VkResult VKAPI_CALL DepthAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* output) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto* d = DepthDevice(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    auto* probe = d->depth_probe.get();
    // Returning our allocation entry point bypasses the loader trampoline just
    // like the overlay's own allocation. Preserve its tested loader-data setup.
    const auto result = AllocateOverlayCommandBuffers(device, info, output);
    if (result == VK_SUCCESS) { DEPTH_OBSERVE(model.Allocate(info->commandPool, info->commandBufferCount, output)) } return result;
}
VKAPI_ATTR void VKAPI_CALL DepthFreeCommandBuffers(VkDevice device, VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* commands) {
    DEPTH_LOOKUP(FreeCommandBuffers, device, )
    DEPTH_OBSERVE(model.Free(count, commands)) next(device, pool, count, commands);
}
VKAPI_ATTR VkResult VKAPI_CALL DepthBeginCommandBuffer(VkCommandBuffer command, const VkCommandBufferBeginInfo* info) {
    DEPTH_LOOKUP(BeginCommandBuffer, command, VK_ERROR_INITIALIZATION_FAILED)
    const auto result = next(command, info);
    if (result == VK_SUCCESS) { DEPTH_OBSERVE(model.Begin(command)) } return result;
}
VKAPI_ATTR VkResult VKAPI_CALL DepthEndCommandBuffer(VkCommandBuffer command) {
    DEPTH_LOOKUP(EndCommandBuffer, command, VK_ERROR_INITIALIZATION_FAILED)
    const auto result = next(command);
    if (result == VK_SUCCESS) { DEPTH_OBSERVE(model.End(command)) } return result;
}
VKAPI_ATTR VkResult VKAPI_CALL DepthResetCommandBuffer(VkCommandBuffer command, VkCommandBufferResetFlags flags) {
    DEPTH_LOOKUP(ResetCommandBuffer, command, VK_ERROR_INITIALIZATION_FAILED)
    const auto result = next(command, flags);
    if (result == VK_SUCCESS) { DEPTH_OBSERVE(model.Begin(command)) } return result;
}
VKAPI_ATTR void VKAPI_CALL DepthCmdBeginRenderPass(VkCommandBuffer command, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    DEPTH_LOOKUP(CmdBeginRenderPass, command, )
    next(command, info, contents); DEPTH_OBSERVE(model.BeginPass(command, *info))
}
VKAPI_ATTR void VKAPI_CALL DepthCmdNextSubpass(VkCommandBuffer command, VkSubpassContents contents) {
    DEPTH_LOOKUP(CmdNextSubpass, command, )
    next(command, contents); DEPTH_OBSERVE(model.NextSubpass(command))
}
VKAPI_ATTR void VKAPI_CALL DepthCmdEndRenderPass(VkCommandBuffer command) {
    DEPTH_LOOKUP(CmdEndRenderPass, command, )
    next(command); DEPTH_OBSERVE(model.EndPass(command))
}
#define DEPTH_RENDERPASS2(suffix) \
    VKAPI_ATTR void VKAPI_CALL DepthCmdBeginRenderPass2##suffix(VkCommandBuffer command, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* begin) { \
        DEPTH_LOOKUP(CmdBeginRenderPass2##suffix, command, ) \
        next(command, info, begin); DEPTH_OBSERVE(model.BeginPass(command, *info)) \
    } \
    VKAPI_ATTR void VKAPI_CALL DepthCmdNextSubpass2##suffix(VkCommandBuffer command, const VkSubpassBeginInfo* begin, const VkSubpassEndInfo* end) { \
        DEPTH_LOOKUP(CmdNextSubpass2##suffix, command, ) \
        next(command, begin, end); DEPTH_OBSERVE(model.NextSubpass(command)) \
    } \
    VKAPI_ATTR void VKAPI_CALL DepthCmdEndRenderPass2##suffix(VkCommandBuffer command, const VkSubpassEndInfo* end) { \
        DEPTH_LOOKUP(CmdEndRenderPass2##suffix, command, ) \
        next(command, end); DEPTH_OBSERVE(model.EndPass(command)) \
    }
DEPTH_RENDERPASS2()
DEPTH_RENDERPASS2(KHR)
#define DEPTH_RENDERING(suffix) \
    VKAPI_ATTR void VKAPI_CALL DepthCmdBeginRendering##suffix(VkCommandBuffer command, const VkRenderingInfo* info) { \
        DEPTH_LOOKUP(CmdBeginRendering##suffix, command, ) \
        next(command, info); DEPTH_OBSERVE(model.BeginRendering(command, *info)) \
    } \
    VKAPI_ATTR void VKAPI_CALL DepthCmdEndRendering##suffix(VkCommandBuffer command) { \
        DEPTH_LOOKUP(CmdEndRendering##suffix, command, ) \
        next(command); DEPTH_OBSERVE(model.EndPass(command)) \
    }
DEPTH_RENDERING()
DEPTH_RENDERING(KHR)

VKAPI_ATTR void VKAPI_CALL DepthCmdExecuteCommands(VkCommandBuffer command, std::uint32_t count, const VkCommandBuffer* secondary) {
    DEPTH_LOOKUP(CmdExecuteCommands, command, )
    next(command, count, secondary); DEPTH_OBSERVE(model.Execute(command, count, secondary))
}
VKAPI_ATTR VkResult VKAPI_CALL DepthQueueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    DEPTH_LOOKUP(QueueSubmit, queue, VK_ERROR_INITIALIZATION_FAILED)
    const auto result = next(queue, count, submits, fence);
    if (result == VK_SUCCESS) for (std::uint32_t i = 0; i < count; ++i) {
        DEPTH_OBSERVE(model.Submit(submits[i].commandBufferCount, submits[i].pCommandBuffers))
    }
    return result;
}
#define DEPTH_SUBMIT2(suffix) \
    VKAPI_ATTR VkResult VKAPI_CALL DepthQueueSubmit2##suffix(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits, VkFence fence) { \
        DEPTH_LOOKUP(QueueSubmit2##suffix, queue, VK_ERROR_INITIALIZATION_FAILED) \
        const auto result = next(queue, count, submits, fence); \
        if (result == VK_SUCCESS) for (std::uint32_t i = 0; i < count; ++i) { \
            DEPTH_OBSERVE( \
                std::vector<VkCommandBuffer> commands; commands.reserve(submits[i].commandBufferInfoCount); \
                for (std::uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j) commands.push_back(submits[i].pCommandBufferInfos[j].commandBuffer); \
                model.Submit(static_cast<std::uint32_t>(commands.size()), commands.data())) \
        } \
        return result; \
    }
DEPTH_SUBMIT2()
DEPTH_SUBMIT2(KHR)

PFN_vkVoidFunction DepthIntercept(const char* name) {
    if (!cs2_camera_depth::ProbePath()) return nullptr;
#define DEPTH_INTERCEPT(fn) if (std::strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(Depth##fn);
    DEPTH_INTERCEPT(CreateImage) DEPTH_INTERCEPT(DestroyImage) DEPTH_INTERCEPT(CreateImageView) DEPTH_INTERCEPT(DestroyImageView)
    DEPTH_INTERCEPT(CreateRenderPass) DEPTH_INTERCEPT(CreateRenderPass2) DEPTH_INTERCEPT(CreateRenderPass2KHR) DEPTH_INTERCEPT(DestroyRenderPass)
    DEPTH_INTERCEPT(CreateFramebuffer) DEPTH_INTERCEPT(DestroyFramebuffer)
    DEPTH_INTERCEPT(CreateCommandPool) DEPTH_INTERCEPT(ResetCommandPool) DEPTH_INTERCEPT(DestroyCommandPool)
    DEPTH_INTERCEPT(AllocateCommandBuffers) DEPTH_INTERCEPT(FreeCommandBuffers)
    DEPTH_INTERCEPT(BeginCommandBuffer) DEPTH_INTERCEPT(EndCommandBuffer) DEPTH_INTERCEPT(ResetCommandBuffer)
    DEPTH_INTERCEPT(CmdBeginRenderPass) DEPTH_INTERCEPT(CmdNextSubpass) DEPTH_INTERCEPT(CmdEndRenderPass)
    DEPTH_INTERCEPT(CmdBeginRenderPass2) DEPTH_INTERCEPT(CmdNextSubpass2) DEPTH_INTERCEPT(CmdEndRenderPass2)
    DEPTH_INTERCEPT(CmdBeginRenderPass2KHR) DEPTH_INTERCEPT(CmdNextSubpass2KHR) DEPTH_INTERCEPT(CmdEndRenderPass2KHR)
    DEPTH_INTERCEPT(CmdBeginRendering) DEPTH_INTERCEPT(CmdEndRendering) DEPTH_INTERCEPT(CmdBeginRenderingKHR) DEPTH_INTERCEPT(CmdEndRenderingKHR)
    DEPTH_INTERCEPT(CmdExecuteCommands) DEPTH_INTERCEPT(QueueSubmit) DEPTH_INTERCEPT(QueueSubmit2) DEPTH_INTERCEPT(QueueSubmit2KHR)
#undef DEPTH_INTERCEPT
    return nullptr;
}
PFN_vkVoidFunction DepthDispatch(PFN_vkVoidFunction next, const char* name) {
    if (next) if (const auto wrapped = DepthIntercept(name)) return wrapped;
    return next;
}
#undef DEPTH_LOOKUP
#undef DEPTH_OBSERVE
#undef DEPTH_CREATE
#undef DEPTH_DESTROY
#undef DEPTH_RENDERPASS2
#undef DEPTH_RENDERING
#undef DEPTH_SUBMIT2
