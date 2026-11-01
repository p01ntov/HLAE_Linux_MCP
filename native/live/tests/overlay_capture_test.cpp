#include "../overlay_capture.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <thread>

static std::vector<VkImageMemoryBarrier> recorded;
static void VKAPI_PTR RecordBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,
    std::uint32_t memoryCount,const VkMemoryBarrier*,std::uint32_t bufferCount,const VkBufferMemoryBarrier*,
    std::uint32_t imageCount,const VkImageMemoryBarrier* images) {
    assert(!memoryCount && !bufferCount && imageCount==1);
    recorded.push_back(images[0]);
}

static void Check(VkResult result) { assert(result == VK_SUCCESS); }
static std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main() {
    using cs2_overlay_capture::Probe;
    unsetenv("CS2_CAMERA_OVERLAY_CAPTURE_DIR");
    Probe disabled;
    assert(!disabled.Requested({})); // No Vulkan entry point is invoked.
    assert(!disabled.LayoutOnly() && !disabled.WaitOnly() && !disabled.CpuDelay());
    Probe::LayoutRoundTrip(RecordBarrier,VK_NULL_HANDLE,VK_NULL_HANDLE);
    assert(recorded.size()==2);
    assert(recorded[0].oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && recorded[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(recorded[1].oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && recorded[1].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(recorded[0].srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT && recorded[0].dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT);
    assert(recorded[1].srcAccessMask==VK_ACCESS_TRANSFER_READ_BIT && !recorded[1].dstAccessMask);
    std::string directory = (std::filesystem::temp_directory_path() / "cs2-overlay-capture-XXXXXX").string();
    assert(mkdtemp(directory.data()));
    setenv("CS2_CAMERA_OVERLAY_CAPTURE_DIR", directory.c_str(), 1);
    const std::string root = directory;
    assert(!chmod(directory.c_str(), 0777));
    { Probe rejected; assert(!rejected.Requested({})); assert(!rejected.Error().empty()); }
    assert(!chmod(directory.c_str(), 0700));
    assert(!mkfifo((root + "/request").c_str(), 0600));
    { Probe rejected; assert(!rejected.Requested({})); assert(rejected.Error().empty()); }
    assert(!unlink((root + "/request").c_str()));
    { std::ofstream file(root + "/target"); file << "7\n"; }
    assert(!symlink("target", (root + "/request").c_str()));
    { Probe rejected; assert(!rejected.Requested({})); assert(rejected.Error().empty()); }
    assert(!unlink((root + "/request").c_str()));
    assert(!unlink((root + "/target").c_str()));
    { std::ofstream file(root + "/request"); file << "123456789012345678901234567890"; }
    { Probe rejected; assert(!rejected.Requested({})); assert(rejected.Error().empty()); }
    {std::ofstream file(root+"/layout_only");}
    {std::ofstream file(root+"/wait_only");}
    {std::ofstream file(root+"/cpu_delay");}
    {
        Probe modes;
        assert(!modes.Requested({}) && modes.Error().empty()); // Modes alone do not allocate or dispatch Vulkan.
        assert(modes.LayoutOnly() && modes.WaitOnly() && modes.CpuDelay() && modes.ModeRevision()==1);
        assert(!unlink((root+"/layout_only").c_str()));
        assert(!unlink((root+"/wait_only").c_str()));
        assert(!unlink((root+"/cpu_delay").c_str()));
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        assert(!modes.Requested({}) && modes.Error().empty());
        assert(!modes.LayoutOnly() && !modes.WaitOnly() && !modes.CpuDelay() && modes.ModeRevision()==2);
    }
    { std::ofstream file(root + "/request"); file << "7\n"; }
    { std::ofstream file(root + "/no_blur"); }
    { std::ofstream file(root + "/open_shot"); file << "9\n"; }
    { std::ofstream file(root + "/open_map"); file << "9\n"; }
    { std::ofstream file(root + "/open_curves"); file << "9\n"; }
    { std::ofstream file(root + "/open_timeline"); file << "9\n"; }
    { std::ofstream file(root + "/open_settings"); file << "9\n"; }
    { std::ofstream file(root + "/open_curve_menu"); file << "9\n"; }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO}; application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instanceInfo.pApplicationInfo = &application;
    VkInstance instance{}; Check(vkCreateInstance(&instanceInfo, nullptr, &instance));
    unsigned count = 0; Check(vkEnumeratePhysicalDevices(instance, &count, nullptr)); assert(count);
    std::vector<VkPhysicalDevice> physicals(count); Check(vkEnumeratePhysicalDevices(instance, &count, physicals.data()));
    const auto physical = physicals[0];
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
    std::cout << properties.deviceName << '\n';
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    unsigned family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    assert(family < count);
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device{}; Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
    constexpr VkExtent2D extent{32, 24};
    Probe::Info info{physical, device, vkGetDeviceProcAddr, vkGetPhysicalDeviceMemoryProperties, extent, VK_FORMAT_R8G8B8A8_UNORM};
    Probe probe; assert(probe.Requested(info)); assert(probe.Pending() == 7); assert(probe.BlurDisabled());
    assert(probe.TakeOpenShot()); assert(!probe.TakeOpenShot());
    assert(probe.TakeOpenMap()); assert(!probe.TakeOpenMap());
    assert(probe.TakeOpenCurves()); assert(!probe.TakeOpenCurves());
    assert(probe.TakeOpenTimeline()); assert(!probe.TakeOpenTimeline());
    assert(probe.TakeOpenSettings()); assert(!probe.TakeOpenSettings());
    assert(probe.TakeOpenCurveMenu()); assert(!probe.TakeOpenCurveMenu());

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = info.format; imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = imageInfo.arrayLayers = 1; imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image{}; Check(vkCreateImage(device, &imageInfo, nullptr, &image));
    VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device, image, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{}; vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    unsigned type = 0; while (type < memoryProperties.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) ++type;
    assert(type < memoryProperties.memoryTypeCount);
    VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memoryInfo.allocationSize = requirements.size; memoryInfo.memoryTypeIndex = type;
    VkDeviceMemory memory{}; Check(vkAllocateMemory(device, &memoryInfo, nullptr, &memory)); Check(vkBindImageMemory(device, image, memory, 0));
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; viewInfo.image = image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = info.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{}; Check(vkCreateImageView(device, &viewInfo, nullptr, &view));
    VkAttachmentDescription attachment{}; attachment.format = info.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; renderInfo.attachmentCount = 1; renderInfo.pAttachments = &attachment; renderInfo.subpassCount = 1; renderInfo.pSubpasses = &subpass;
    VkRenderPass renderPass{}; Check(vkCreateRenderPass(device, &renderInfo, nullptr, &renderPass));
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; framebufferInfo.renderPass = renderPass; framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &view; framebufferInfo.width = extent.width; framebufferInfo.height = extent.height; framebufferInfo.layers = 1;
    VkFramebuffer framebuffer{}; Check(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer));
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = family;
    VkCommandPool pool{}; Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; commandInfo.commandPool = pool; commandInfo.commandBufferCount = 1; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer command{}; Check(vkAllocateCommandBuffers(device, &commandInfo, &command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Check(vkBeginCommandBuffer(command, &begin));
    VkClearValue red{}; red.color = {{1, 0, 0, 1}};
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass = renderPass; render.framebuffer = framebuffer;
    render.renderArea.extent = extent; render.clearValueCount = 1; render.pClearValues = &red;
    vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment green{}; green.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; green.clearValue.color = {{0, 1, 0, 1}};
    VkClearRect rectangle{{{3, 5}, {19, 11}}, 0, 1}; vkCmdClearAttachments(command, 1, &green, 1, &rectangle);
    vkCmdEndRenderPass(command);
    Probe::LayoutRoundTrip(vkCmdPipelineBarrier,command,image,VK_IMAGE_LAYOUT_GENERAL);
    probe.Record(command, image, VK_IMAGE_LAYOUT_GENERAL);
    Check(vkEndCommandBuffer(command));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; Check(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    Check(vkQueueSubmit(queue, 1, &submit, fence)); Check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
    assert(probe.Complete("{\"frame\":42,\"same_frame\":true}"));
    assert(Read(root + "/capture-7-draw.json") == "{\"frame\":42,\"same_frame\":true}");
    const auto alpha=Read(root+"/capture-7-alpha.pgm");
    const auto alphaHeader=std::string("P5\n32 24\n255\n");
    assert(alpha.substr(0,alphaHeader.size())==alphaHeader);
    assert(alpha.size()==alphaHeader.size()+extent.width*extent.height);
    for(std::size_t index=alphaHeader.size();index<alpha.size();++index)assert(static_cast<unsigned char>(alpha[index])==255);
    const auto ppm = Read(root + "/capture-7-gpu.ppm"); const auto header = std::string("P6\n32 24\n255\n");
    assert(ppm.size() == header.size() + extent.width * extent.height * 3);
    assert(ppm.substr(0, header.size()) == header);
    for (unsigned y = 0; y < extent.height; ++y) for (unsigned x = 0; x < extent.width; ++x) {
        const auto offset = header.size() + (y * extent.width + x) * 3;
        const bool inside = x >= 3 && x < 22 && y >= 5 && y < 16;
        assert(static_cast<unsigned char>(ppm[offset]) == (inside ? 0 : 255));
        assert(static_cast<unsigned char>(ppm[offset + 1]) == (inside ? 255 : 0));
        assert(ppm[offset + 2] == 0);
    }
    const unsigned char bgra[] = {7, 8, 9, 255};
    assert(Probe::Ppm(bgra, {1, 1}, VK_FORMAT_B8G8R8A8_UNORM) == std::string("P6\n1 1\n255\n\x09\x08\x07", 14));
    assert(!probe.Pending()); assert(!probe.Requested(info));
    assert(!probe.TakeOpenShot());
    assert(!probe.TakeOpenMap()); assert(!probe.TakeOpenCurves());
    assert(!probe.TakeOpenTimeline()); assert(!probe.TakeOpenSettings());
    assert(!probe.TakeOpenCurveMenu());
    probe.Abandon(); assert(!probe.Requested(info));
    probe.Destroy();
    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, view, nullptr); vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    unlink((root + "/capture-7-gpu.ppm").c_str()); unlink((root + "/capture-7-draw.json").c_str());
    unlink((root + "/capture-7-alpha.pgm").c_str());
    unlink((root + "/request").c_str()); unlink((root + "/no_blur").c_str());
    unlink((root + "/open_shot").c_str());unlink((root + "/open_map").c_str());
    unlink((root + "/open_curves").c_str());unlink((root + "/open_timeline").c_str());
    unlink((root + "/open_settings").c_str());unlink((root + "/open_curve_menu").c_str());assert(!rmdir(directory.c_str()));
    std::cout << "One-shot after-render GPU pixels, clipping, orientation, same-frame metadata, BGRA and disabled path passed\n";
}
