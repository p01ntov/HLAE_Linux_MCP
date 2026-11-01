#define VK_NO_PROTOTYPES
#include "../depth_probe.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace camera_overlay {
struct Device {
    VkDevice handle{};
    PFN_vkGetDeviceProcAddr gdpa{};
    std::unique_ptr<cs2_camera_depth::Probe> depth_probe;
};
template<class T> void* Key(T h) { return h ? *reinterpret_cast<void**>(h) : nullptr; }
std::recursive_mutex mutex;
std::unordered_map<void*, std::unique_ptr<Device>> devices;
VKAPI_ATTR VkResult VKAPI_CALL AllocateOverlayCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
#include "../depth_probe_layer.h"
} // namespace camera_overlay

namespace {
struct Handle { void* dispatch; } deviceObject{reinterpret_cast<void*>(0x1234)}, commandObject{deviceObject.dispatch}, queueObject{deviceObject.dispatch};
VkDevice device = reinterpret_cast<VkDevice>(&deviceObject);
VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(&commandObject);
VkQueue queue = reinterpret_cast<VkQueue>(&queueObject);
VkImage image = reinterpret_cast<VkImage>(0x4567);
VkResult createResult = VK_SUCCESS, submitResult = VK_SUCCESS;
unsigned calls = 0, submitCalls = 0;
VkImageCreateInfo lastInfo{};
VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice input, const VkImageCreateInfo* info, const VkAllocationCallbacks*, VkImage* output) {
    assert(input == device); ++calls; lastInfo = *info; *output = image; return createResult;
}
VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue input, std::uint32_t count, const VkSubmitInfo* info, VkFence fence) {
    assert(input == queue && count == 1 && info->commandBufferCount == 1 && *info->pCommandBuffers == command && !fence);
    ++submitCalls; return submitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL Submit2(VkQueue input, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence) {
    assert(input == queue && count == 1 && info->commandBufferInfoCount == 1 && info->pCommandBufferInfos->commandBuffer == command && !fence);
    ++submitCalls; return submitResult;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Next(VkDevice input, const char* name) {
    assert(input == device);
    if (!std::strcmp(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(Create);
    if (!std::strcmp(name, "vkQueueSubmit")) return reinterpret_cast<PFN_vkVoidFunction>(Submit);
    if (!std::strcmp(name, "vkQueueSubmit2")) return reinterpret_cast<PFN_vkVoidFunction>(Submit2);
    return nullptr;
}
}

namespace camera_overlay {
VKAPI_ATTR VkResult VKAPI_CALL AllocateOverlayCommandBuffers(VkDevice input, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* output) {
    assert(input == device && info->commandBufferCount == 1);
    *output = command;
    return VK_SUCCESS;
}
}

int main() {
    using namespace camera_overlay;
    static_assert(noexcept(cs2_camera_depth::ProbePath()), "Diagnostic lookup must not throw through the Vulkan ABI");
    auto data = std::make_unique<Device>(); data->handle = device; data->gdpa = Next;
    auto* d = data.get(); devices[Key(device)] = std::move(data);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.format = VK_FORMAT_D32_SFLOAT; info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.extent = {320, 240, 1}; info.samples = VK_SAMPLE_COUNT_1_BIT; info.mipLevels = info.arrayLayers = 1;
    VkImage output{};
    assert(DepthCreateImage(device, &info, nullptr, &output) == VK_SUCCESS && calls == 1);
    assert(lastInfo.usage == info.usage && lastInfo.pNext == info.pNext); // no hidden transfer capability
    d->depth_probe = std::make_unique<cs2_camera_depth::Probe>(device, Next);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocate.commandBufferCount = 1;
    VkCommandBuffer allocated{};
    assert(DepthAllocateCommandBuffers(device, &allocate, &allocated) == VK_SUCCESS && allocated == command);
    createResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(DepthCreateImage(device, &info, nullptr, &output) == createResult);
    assert(d->depth_probe->model.ImageCount() == 0);
    createResult = VK_SUCCESS;
    assert(DepthCreateImage(device, &info, nullptr, &output) == VK_SUCCESS);
    assert(d->depth_probe->model.ImageCount() == 1);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    submitResult = VK_ERROR_DEVICE_LOST;
    assert(DepthQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == submitResult);
    assert(submitCalls == 1 && d->depth_probe->model.Submissions() == 0);
    submitResult = VK_SUCCESS;
    assert(DepthQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(d->depth_probe->model.Submissions() == 1);
    VkCommandBufferSubmitInfo commandSubmit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; commandSubmit.commandBuffer = command;
    VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit2.commandBufferInfoCount = 1; submit2.pCommandBufferInfos = &commandSubmit;
    assert(DepthQueueSubmit2(queue, 1, &submit2, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(d->depth_probe->model.Submissions() == 2);
    const bool enabled = cs2_camera_depth::ProbePath() != nullptr;
    assert(bool(DepthIntercept("vkCreateImage")) == enabled);
    assert(!DepthIntercept("vkCreateMadeUpExtension"));
    assert(DepthDispatch(nullptr, "vkCmdBeginRendering") == nullptr); // Never advertise unavailable extensions.
    assert(DepthDispatch(reinterpret_cast<PFN_vkVoidFunction>(Create), "vkCreateImage") ==
        (enabled ? reinterpret_cast<PFN_vkVoidFunction>(DepthCreateImage) : reinterpret_cast<PFN_vkVoidFunction>(Create)));
    // The probe cannot turn a driver error into a success or fabricate missing function pointers.
    assert(DepthCreateRenderPass2(device, nullptr, nullptr, nullptr) == VK_ERROR_INITIALIZATION_FAILED);
    devices.clear();
    std::cout << "Depth probe wrappers: unmodified arguments, failed creation/submission, both queue APIs and opt-in lookup passed\n";
}
