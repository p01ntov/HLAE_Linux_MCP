#include "../lens_effects.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
using cs2_camera_lens::Settings;
unsigned validation_errors = 0;
void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}
VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        std::cerr << data->pMessage << '\n';
    }
    return VK_FALSE;
}
struct Device {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{};
    VkQueue queue{}; unsigned family = 0; VkCommandPool pool{}; VkCommandBuffer command{};
    VkDebugUtilsMessengerEXT messenger{};
    Device() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "CS2 Camera original lens pixel validation"; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug.pfnUserCallback = Validation;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        features.enabledValidationFeatureCount = 1; features.pEnabledValidationFeatures = &sync;
        if (std::getenv("CS2_LENS_TEST_VALIDATION")) {
            create.enabledLayerCount = 1; create.ppEnabledLayerNames = &layer;
            create.enabledExtensionCount = 2; create.ppEnabledExtensionNames = extensions;
            debug.pNext = &features; create.pNext = &debug;
        }
        Check(vkCreateInstance(&create, nullptr, &instance));
        if (create.pNext) {
            debug.pNext = nullptr;
            auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            Check(make(instance, &debug, nullptr, &messenger));
        }
        unsigned count = 0; Check(vkEnumeratePhysicalDevices(instance, &count, nullptr)); assert(count);
        std::vector<VkPhysicalDevice> devices(count); Check(vkEnumeratePhysicalDevices(instance, &count, devices.data())); physical = devices[0];
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
        std::cout << "Device: " << properties.deviceName << "\n";
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        assert(family < count); float priority = 1;
        VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qc.queueFamilyIndex = family; qc.queueCount = 1; qc.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc;
        Check(vkCreateDevice(physical, &dc, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pc.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &pc, nullptr, &pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &ca, &command));
    }
    ~Device() {
        vkDeviceWaitIdle(device); vkDestroyCommandPool(device, pool, nullptr); vkDestroyDevice(device, nullptr);
        if (messenger) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,
            "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
    unsigned Memory(unsigned bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties properties{}; vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (unsigned i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no matching memory type");
    }
    void Begin() const {
        Check(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(command, &begin));
    }
    void Submit() const {
        Check(vkEndCommandBuffer(command)); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); Check(vkQueueWaitIdle(queue));
    }
    void Barrier(VkImage image, VkImageLayout before, VkImageLayout after, VkAccessFlags src, VkAccessFlags dst) const {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = src; barrier.dstAccessMask = dst; barrier.oldLayout = before; barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
};

struct Target {
    Device& d; unsigned width, height; VkFormat format;
    VkImage image{}; VkDeviceMemory imageMemory{}; VkImageView view{};
    VkBuffer buffer{}; VkDeviceMemory bufferMemory{}; unsigned char* mapped{}; bool uploaded = false;
    Target(Device& owner, unsigned w, unsigned h, VkFormat f): d(owner), width(w), height(h), format(f) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; create.imageType = VK_IMAGE_TYPE_2D;
        create.format = format; create.extent = {width, height, 1}; create.mipLevels = create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT; create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        Check(vkCreateImage(d.device, &create, nullptr, &image)); VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(d.device, image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = d.Memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(d.device, &allocate, nullptr, &imageMemory)); Check(vkBindImageMemory(d.device, image, imageMemory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.format = format;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(d.device, &vi, nullptr, &view));
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = width * height * 8;
        bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(d.device, &bc, nullptr, &buffer)); vkGetBufferMemoryRequirements(d.device, buffer, &requirements);
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = d.Memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(d.device, &allocate, nullptr, &bufferMemory)); Check(vkBindBufferMemory(d.device, buffer, bufferMemory, 0));
        void* pointer = nullptr; Check(vkMapMemory(d.device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &pointer));
        mapped = static_cast<unsigned char*>(pointer);
    }
    ~Target() {
        vkUnmapMemory(d.device, bufferMemory); vkDestroyBuffer(d.device, buffer, nullptr); vkFreeMemory(d.device, bufferMemory, nullptr);
        vkDestroyImageView(d.device, view, nullptr); vkDestroyImage(d.device, image, nullptr); vkFreeMemory(d.device, imageMemory, nullptr);
    }
    bool Bgra() const { return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB; }
    void Upload(const std::vector<unsigned char>& rgba) {
        std::memcpy(mapped, rgba.data(), rgba.size());
        if (Bgra()) for (unsigned i = 0; i < width * height; ++i) std::swap(mapped[i * 4], mapped[i * 4 + 2]);
        d.Begin(); d.Barrier(image, uploaded ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uploaded ? VK_ACCESS_MEMORY_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(d.command, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        d.Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        d.Submit(); uploaded = true;
    }
    std::vector<unsigned char> Readback() {
        // Called inside the same command buffer as the lens pass: this is the
        // capture ordering contract used by the real present layer.
        d.Barrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{}; copy.bufferOffset = width * height * 4;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(d.command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        d.Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(d.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        d.Submit(); std::vector<unsigned char> rgba(mapped + copy.bufferOffset, mapped + width * height * 8);
        if (Bgra()) for (unsigned i = 0; i < width * height; ++i) std::swap(rgba[i * 4], rgba[i * 4 + 2]);
        return rgba;
    }
};

double Decode(double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); }
double Encode(double value) { return value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1. / 2.4) - .055; }
std::vector<unsigned char> Source(unsigned width, unsigned height) {
    std::vector<unsigned char> result(width * height * 4);
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto i = (y * width + x) * 4;
        result[i] = 32 + x * 210 / (width - 1); result[i + 1] = 16 + y * 220 / (height - 1);
        result[i + 2] = (x / 7 + y / 5) % 2 ? 230 : 30; result[i + 3] = 255;
    }
    return result;
}
std::vector<unsigned char> Reference(const std::vector<unsigned char>& source, unsigned width, unsigned height, const Settings& settings) {
    if (!settings.Active()) return source;
    auto result = source;
    auto sample = [&](double u, double v, unsigned channel) {
        const double px = u * width - .5, py = v * height - .5;
        const int x = int(std::floor(px)), y = int(std::floor(py));
        const double fx = px - x, fy = py - y;
        auto at = [&](int ix, int iy) { return Decode(source[(std::clamp(iy, 0, int(height) - 1) * width + std::clamp(ix, 0, int(width) - 1)) * 4 + channel] / 255.); };
        return (at(x, y) * (1 - fx) + at(x + 1, y) * fx) * (1 - fy) +
               (at(x, y + 1) * (1 - fx) + at(x + 1, y + 1) * fx) * fy;
    };
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const double cx = 2 * (x + .5) / width - 1, cy = 2 * (y + .5) / height - 1, aspect = double(width) / height;
        const double rx = cx * aspect, ry = cy, r2 = (rx * rx + ry * ry) / (aspect * aspect + 1);
        const double k = .4 * settings.distortion, scale = (1 + k * r2) / (1 + std::max(k, 0.));
        const double u = cx * scale * .5 + .5, v = cy * scale * .5 + .5;
        const double length = std::max(std::sqrt(rx * rx + ry * ry), .000001);
        const double shiftx = rx / length / width * settings.chromatic * r2, shifty = ry / length / height * settings.chromatic * r2;
        const double t = std::clamp((r2 - .2) / .8, 0., 1.), dim = 1 - settings.vignette * t * t * (3 - 2 * t);
        for (unsigned c = 0; c < 3; ++c) {
            const double sign = c == 0 ? 1 : c == 2 ? -1 : 0;
            result[(y * width + x) * 4 + c] = static_cast<unsigned char>(std::clamp(std::lround(Encode(sample(u + sign * shiftx, v + sign * shifty, c) * dim) * 255), 0l, 255l));
        }
    }
    return result;
}
void Save(const char* name, unsigned width, unsigned height, const std::vector<unsigned char>& rgba) {
    const auto* directory = std::getenv("CS2_LENS_TEST_ARTIFACTS"); if (!directory) return;
    std::ofstream stream(std::string(directory) + "/" + name + ".ppm", std::ios::binary);
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    for (unsigned i = 0; i < width * height; ++i) stream.write(reinterpret_cast<const char*>(rgba.data() + i * 4), 3);
}
} // namespace

int main() {
    Device device; cs2_camera_lens::Effects lens;
    unsigned cases = 0, worstError = 0;
    for (const auto extent : {VkExtent2D{128, 72}, VkExtent2D{73, 121}, VkExtent2D{31, 31}}) {
        for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}) {
            Target first(device, extent.width, extent.height, format), second(device, extent.width, extent.height, format);
            cs2_camera_lens::InitInfo init{device.physical, device.device, vkGetDeviceProcAddr,
                vkGetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceFormatProperties, extent, format, {first.view, second.view}};
            assert(lens.Initialize(init)); const auto source = Source(extent.width, extent.height);
            const std::vector<Settings> settings{{false, .6f, .7f, 5}, {true, 0, 0, 0}, {true, -.75f, 0, 0},
                {true, .75f, 0, 0}, {true, 0, .85f, 0}, {true, 0, 0, 8}, {true, -.6f, .4f, 4},
                {true, std::numeric_limits<float>::quiet_NaN(), .5f, 0}, {true, 1.f, 0, 0}, {true, 0, 0, -1}};
            for (unsigned test = 0; test < settings.size(); ++test) {
                auto& target = test % 2 ? second : first; const unsigned index = test % 2;
                target.Upload(source);
                // Record an effect then cancel it. The next recording must not
                // assume the unsubmitted capture image reached shader layout.
                device.Begin(); assert(lens.Record(device.command, index, target.image, {true, .2f, .3f, 1}, VK_IMAGE_LAYOUT_GENERAL));
                Check(vkEndCommandBuffer(device.command));
                device.Begin(); const bool recorded = lens.Record(device.command, index, target.image, settings[test], VK_IMAGE_LAYOUT_GENERAL);
                assert(recorded == settings[test].Active()); auto output = target.Readback();
                const auto expected = Reference(source, extent.width, extent.height, settings[test]);
                unsigned maxError = 0, changed = 0, maxPixel = 0; double absoluteError = 0;
                for (unsigned pixel = 0; pixel < output.size(); ++pixel) {
                    const auto error = unsigned(std::abs(int(output[pixel]) - int(expected[pixel])));
                    if (error > maxError) { maxError = error; maxPixel = pixel; }
                    absoluteError += error;
                    changed += output[pixel] != source[pixel];
                }
                if (maxError > 3) std::cerr << "pixel mismatch extent=" << extent.width << 'x' << extent.height
                    << " format=" << format << " case=" << test << " error=" << maxError << " pixel=" << maxPixel << " values=" << unsigned(output[maxPixel]) << '/' << unsigned(expected[maxPixel]) << " mean=" << absoluteError / output.size() << '\n';
                worstError = std::max(worstError, maxError);
                if (settings[test].Active()) { assert(changed > source.size() / 100); assert(maxError <= 2); }
                else assert(output == source);
                if (extent.width == 128 && format == VK_FORMAT_R8G8B8A8_SRGB) {
                    const char* names[] = {"disabled", "neutral", "pincushion", "barrel", "vignette", "chromatic", "combined", "invalid-nan", "invalid-range", "invalid-chromatic"};
                    Save(names[test], extent.width, extent.height, output);
                }
                ++cases;
            }
            auto invalid = init; invalid.sourceFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            assert(!lens.Initialize(invalid)); assert(!lens.Ready()); assert(!lens.Error().empty());
            invalid = init; invalid.sourceViews[0] = VK_NULL_HANDLE; assert(!lens.Initialize(invalid));
            assert(lens.Initialize(init)); lens.Destroy();
        }
    }
    if (std::getenv("CS2_LENS_TEST_BENCHMARK")) {
        Target target(device, 1920, 1080, VK_FORMAT_R8G8B8A8_SRGB);
        cs2_camera_lens::InitInfo init{device.physical, device.device, vkGetDeviceProcAddr,
            vkGetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceFormatProperties, {1920, 1080},
            VK_FORMAT_R8G8B8A8_SRGB, {target.view}};
        assert(lens.Initialize(init)); const auto source = Source(1920, 1080);
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(device.physical, &properties);
        unsigned count = 0; vkGetPhysicalDeviceQueueFamilyProperties(device.physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(device.physical, &count, families.data());
        if (families[device.family].timestampValidBits) {
            VkQueryPoolCreateInfo create{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; create.queryType = VK_QUERY_TYPE_TIMESTAMP; create.queryCount = 2;
            VkQueryPool timestamps{}; Check(vkCreateQueryPool(device.device, &create, nullptr, &timestamps));
            std::vector<double> timings;
            for (unsigned frame = 0; frame < 36; ++frame) {
                target.Upload(source); device.Begin(); vkCmdResetQueryPool(device.command, timestamps, 0, 2);
                vkCmdWriteTimestamp(device.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
                assert(lens.Record(device.command, 0, target.image, {true, -.6f, .4f, 4}, VK_IMAGE_LAYOUT_GENERAL));
                vkCmdWriteTimestamp(device.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1); device.Submit();
                std::uint64_t values[2]{}; Check(vkGetQueryPoolResults(device.device, timestamps, 0, 2, sizeof(values), values,
                    sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                const auto bits = families[device.family].timestampValidBits;
                const std::uint64_t mask = bits == 64 ? UINT64_MAX : ((std::uint64_t(1) << bits) - 1);
                if (frame >= 6) timings.push_back(double((values[1] - values[0]) & mask) * properties.limits.timestampPeriod / 1e6);
            }
            std::sort(timings.begin(), timings.end());
            std::cout << "1080p combined lens GPU median=" << timings[timings.size() / 2]
                << "ms p95=" << timings[(timings.size() * 95 / 100)] << "ms (30 warmed submissions)\n";
            vkDestroyQueryPool(device.device, timestamps, nullptr);
        }
        lens.Destroy();
    }
    assert(validation_errors == 0);
    std::cout << "Lens: " << cases << " GPU/reference pixel cases; max byte error=" << worstError
        << "; 4 SDR formats; two targets; resize; cancellation; bit-identical off; invalid input; teardown OK\n";
}
