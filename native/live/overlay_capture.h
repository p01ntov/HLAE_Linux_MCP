#pragma once

// Opt-in, one-shot diagnostics. Ordinary frames allocate, copy and wait nothing.
// A private directory contains a monotonically changing numeric `request` file;
// the matching GPU image and CPU draw description are saved before present.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cs2_overlay_capture {
class Probe {
public:
    struct Info {
        VkPhysicalDevice physical{};
        VkDevice device{};
        PFN_vkGetDeviceProcAddr gdpa{};
        PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties{};
        VkExtent2D extent{};
        VkFormat format{};
    };
    Probe() = default;
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe() { if (directory_ >= 0) close(directory_); }

    bool Requested(const Info& info) {
        if (!configured_) {
            configured_ = true;
            const char* path = std::getenv("CS2_CAMERA_OVERLAY_CAPTURE_DIR");
            if (!path || !*path) return false;
            directory_ = open(path, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat status{};
            if (directory_ < 0 || fstat(directory_, &status) || status.st_uid != geteuid() ||
                (status.st_mode & 077) != 0) {
                error_ = "capture directory must be owned and private";
                if (directory_ >= 0) close(directory_);
                directory_ = -1;
            }
        }
        if (directory_ < 0) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextPoll_) return false;
        nextPoll_ = now + std::chrono::milliseconds(100);
        const auto before=unsigned(blurDisabled_)|(unsigned(layoutOnly_)<<1)|(unsigned(waitOnly_)<<2)|(unsigned(cpuDelay_)<<3);
        blurDisabled_=OwnedFile("no_blur");
        layoutOnly_=OwnedFile("layout_only");
        waitOnly_=OwnedFile("wait_only");
        cpuDelay_=OwnedFile("cpu_delay");
        const auto after=unsigned(blurDisabled_)|(unsigned(layoutOnly_)<<1)|(unsigned(waitOnly_)<<2)|(unsigned(cpuDelay_)<<3);
        if(before!=after)++modeRevision_;
        const auto openShot=ReadToken("open_shot");
        if(openShot && openShot!=lastOpenShot_) {lastOpenShot_=openShot;openShot_=true;}
        const auto openMap=ReadToken("open_map");
        if(openMap && openMap!=lastOpenMap_) {lastOpenMap_=openMap;openMap_=true;}
        const auto openCurves=ReadToken("open_curves");
        if(openCurves && openCurves!=lastOpenCurves_) {lastOpenCurves_=openCurves;openCurves_=true;}
        const auto openTimeline=ReadToken("open_timeline");
        if(openTimeline && openTimeline!=lastOpenTimeline_) {lastOpenTimeline_=openTimeline;openTimeline_=true;}
        const auto openSettings=ReadToken("open_settings");
        if(openSettings && openSettings!=lastOpenSettings_) {lastOpenSettings_=openSettings;openSettings_=true;}
        const auto openCurveMenu=ReadToken("open_curve_menu");
        if(openCurveMenu && openCurveMenu!=lastOpenCurveMenu_) {lastOpenCurveMenu_=openCurveMenu;openCurveMenu_=true;}
        if (captures_ >= 8 || pending_ || blocked_) return false;
        const auto sequence=ReadToken("request");
        if (!sequence || sequence == lastRequest_) return false;
        lastRequest_ = sequence;
        if (!Initialize(info)) return false;
        pending_ = sequence;
        return true;
    }

    // The caller records this after the overlay render pass, then waits only its
    // submitted fence before Complete. Source layout is restored for the WSI.
    void Record(VkCommandBuffer command, VkImage image,
                VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) const {
        if (!pending_) return;
        VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        imageBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.oldLayout = sourceLayout;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.srcQueueFamilyIndex = imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image = image;
        imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {info_.extent.width, info_.extent.height, 1};
        vk_.CmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &region);
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.dstAccessMask = 0;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.newLayout = sourceLayout;
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = buffer_; host.size = VK_WHOLE_SIZE;
        vk_.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 1, &host, 1, &imageBarrier);
    }
    bool Complete(const std::string& drawDescription) {
        if (!pending_) return false;
        const auto sequence = pending_; pending_ = 0; ++captures_;
        void* mapped = nullptr;
        if (vk_.MapMemory(info_.device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return Fail("map capture memory failed");
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory_; range.size = VK_WHOLE_SIZE;
        const auto result = vk_.InvalidateMappedMemoryRanges(info_.device, 1, &range);
        std::string pixels,alpha;
        if (result == VK_SUCCESS) {
            pixels = Ppm(mapped, info_.extent, info_.format);
            alpha = Alpha(mapped,info_.extent);
        }
        vk_.UnmapMemory(info_.device, memory_);
        if (result != VK_SUCCESS) return Fail("invalidate capture memory failed");
        const auto prefix = "capture-" + std::to_string(sequence);
        return Save(prefix + "-gpu.ppm", pixels) && Save(prefix + "-alpha.pgm",alpha) &&
            Save(prefix + "-draw.json", drawDescription);
    }
    void Cancel() { pending_ = 0; }
    void Abandon() { blocked_ = true; pending_ = 0; }
    bool BlurDisabled() const { return blurDisabled_; }
    bool LayoutOnly() const {return layoutOnly_;}
    bool WaitOnly() const {return waitOnly_;}
    bool CpuDelay() const {return cpuDelay_;}
    std::uint64_t ModeRevision() const {return modeRevision_;}
    bool TakeOpenShot() {const bool value=openShot_;openShot_=false;return value;}
    bool TakeOpenMap() {const bool value=openMap_;openMap_=false;return value;}
    bool TakeOpenCurves() {const bool value=openCurves_;openCurves_=false;return value;}
    bool TakeOpenTimeline() {const bool value=openTimeline_;openTimeline_=false;return value;}
    bool TakeOpenSettings() {const bool value=openSettings_;openSettings_=false;return value;}
    bool TakeOpenCurveMenu() {const bool value=openCurveMenu_;openCurveMenu_=false;return value;}
    std::uint64_t Pending() const { return pending_; }
    std::uint64_t LastRequest() const { return lastRequest_; }
    const std::string& Error() const { return error_; }
    std::string TakeError() {auto error=std::move(error_);error_.clear();return error;}
    void Destroy() {
        pending_ = 0;
        if (buffer_) vk_.DestroyBuffer(info_.device, buffer_, nullptr);
        if (memory_) vk_.FreeMemory(info_.device, memory_, nullptr);
        buffer_ = {}; memory_ = {};
    }
    static std::string Ppm(const void* data, VkExtent2D extent, VkFormat format) {
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        const auto* pixels = static_cast<const unsigned char*>(data);
        std::string output = "P6\n" + std::to_string(extent.width) + " " +
            std::to_string(extent.height) + "\n255\n";
        output.reserve(output.size() + std::size_t(extent.width) * extent.height * 3);
        for (std::size_t index = 0; index < std::size_t(extent.width) * extent.height; ++index) {
            output.push_back(static_cast<char>(pixels[index * 4 + (bgra ? 2 : 0)]));
            output.push_back(static_cast<char>(pixels[index * 4 + 1]));
            output.push_back(static_cast<char>(pixels[index * 4 + (bgra ? 0 : 2)]));
        }
        return output;
    }
    static std::string Alpha(const void* data,VkExtent2D extent) {
        const auto* pixels=static_cast<const unsigned char*>(data);
        std::string output="P5\n"+std::to_string(extent.width)+" "+std::to_string(extent.height)+"\n255\n";
        output.reserve(output.size()+std::size_t(extent.width)*extent.height);
        for(std::size_t index=0;index<std::size_t(extent.width)*extent.height;++index)
            output.push_back(static_cast<char>(pixels[index*4+3]));
        return output;
    }
    static void LayoutRoundTrip(PFN_vkCmdPipelineBarrier barrier,VkCommandBuffer command,VkImage image,
                                VkImageLayout sourceLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        transition.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        transition.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        transition.oldLayout=sourceLayout;
        transition.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        transition.srcQueueFamilyIndex=transition.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        transition.image=image;transition.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        barrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,0,nullptr,1,&transition);
        transition.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;transition.dstAccessMask=0;
        transition.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;transition.newLayout=sourceLayout;
        barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,0,nullptr,0,nullptr,1,&transition);
    }
private:
    struct Dispatch {
#define CAPTURE_FN(name) PFN_vk##name name{};
        CAPTURE_FN(CreateBuffer) CAPTURE_FN(DestroyBuffer) CAPTURE_FN(GetBufferMemoryRequirements)
        CAPTURE_FN(AllocateMemory) CAPTURE_FN(FreeMemory) CAPTURE_FN(BindBufferMemory)
        CAPTURE_FN(MapMemory) CAPTURE_FN(UnmapMemory) CAPTURE_FN(InvalidateMappedMemoryRanges)
        CAPTURE_FN(CmdPipelineBarrier) CAPTURE_FN(CmdCopyImageToBuffer)
#undef CAPTURE_FN
    } vk_;
    Info info_{};
    VkBuffer buffer_{};
    VkDeviceMemory memory_{};
    int directory_ = -1;
    bool configured_ = false, blocked_ = false, blurDisabled_ = false,openShot_=false,openMap_=false,openCurves_=false,openTimeline_=false,openSettings_=false,openCurveMenu_=false;
    bool layoutOnly_=false,waitOnly_=false,cpuDelay_=false;
    unsigned captures_ = 0;
    std::uint64_t pending_ = 0, lastRequest_ = 0,lastOpenShot_=0,lastOpenMap_=0,lastOpenCurves_=0,lastOpenTimeline_=0,lastOpenSettings_=0,lastOpenCurveMenu_=0,modeRevision_=0;
    std::chrono::steady_clock::time_point nextPoll_{};
    std::string error_;
    bool Fail(const char* reason) { error_ = reason; return false; }
    bool OwnedFile(const char* name) const {
        struct stat status{};
        return !fstatat(directory_,name,&status,AT_SYMLINK_NOFOLLOW) &&
            S_ISREG(status.st_mode) && status.st_uid==geteuid();
    }
    std::uint64_t ReadToken(const char* name) const {
        const int file=openat(directory_,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(file<0)return 0;
        struct stat status{};
        char text[25]{};
        const bool valid=!fstat(file,&status) && S_ISREG(status.st_mode) &&
            status.st_uid==geteuid() && status.st_size>0 && status.st_size<24;
        const auto size=valid?read(file,text,sizeof(text)-1):-1;
        close(file);
        std::uint64_t sequence=0;
        if(size<=0)return 0;
        for(ssize_t index=0;index<size;++index) {
            if(text[index]=='\n' && index==size-1)break;
            if(text[index]<'0' || text[index]>'9' || sequence>100000000)return 0;
            sequence=sequence*10+static_cast<unsigned>(text[index]-'0');
        }
        return sequence;
    }
    bool Initialize(const Info& info) {
        if (buffer_) return true;
        if (!info.device || !info.physical || !info.gdpa || !info.memoryProperties ||
            !info.extent.width || !info.extent.height || info.extent.width > 8192 || info.extent.height > 8192 ||
            (info.format != VK_FORMAT_B8G8R8A8_UNORM && info.format != VK_FORMAT_B8G8R8A8_SRGB &&
             info.format != VK_FORMAT_R8G8B8A8_UNORM && info.format != VK_FORMAT_R8G8B8A8_SRGB))
            return Fail("unsupported capture device, extent or format");
        info_ = info;
#define CAPTURE_LOAD(name) vk_.name = reinterpret_cast<PFN_vk##name>(info.gdpa(info.device, "vk" #name)); \
        if (!vk_.name) return Fail("missing capture function vk" #name);
        CAPTURE_LOAD(CreateBuffer) CAPTURE_LOAD(DestroyBuffer) CAPTURE_LOAD(GetBufferMemoryRequirements)
        CAPTURE_LOAD(AllocateMemory) CAPTURE_LOAD(FreeMemory) CAPTURE_LOAD(BindBufferMemory)
        CAPTURE_LOAD(MapMemory) CAPTURE_LOAD(UnmapMemory) CAPTURE_LOAD(InvalidateMappedMemoryRanges)
        CAPTURE_LOAD(CmdPipelineBarrier) CAPTURE_LOAD(CmdCopyImageToBuffer)
#undef CAPTURE_LOAD
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = std::uint64_t(info.extent.width) * info.extent.height * 4;
        create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vk_.CreateBuffer(info.device, &create, nullptr, &buffer_) != VK_SUCCESS)
            return Fail("create capture buffer failed");
        VkMemoryRequirements requirements{};
        vk_.GetBufferMemoryRequirements(info.device, buffer_, &requirements);
        VkPhysicalDeviceMemoryProperties properties{}; info.memoryProperties(info.physical, &properties);
        unsigned type = UINT32_MAX;
        for (unsigned index = 0; index < properties.memoryTypeCount; ++index)
            if ((requirements.memoryTypeBits & (1u << index)) &&
                (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = index; break; }
        if (type == UINT32_MAX) { Destroy(); return Fail("capture requires host-visible memory"); }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
        if (vk_.AllocateMemory(info.device, &allocate, nullptr, &memory_) != VK_SUCCESS ||
            vk_.BindBufferMemory(info.device, buffer_, memory_, 0) != VK_SUCCESS) {
            Destroy(); return Fail("allocate capture memory failed");
        }
        return true;
    }
    bool Save(const std::string& name, const std::string& data) {
        const int file = openat(directory_, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file < 0) return Fail("create capture output failed (file may already exist)");
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto written = write(file, data.data() + offset, data.size() - offset);
            if (written <= 0) { close(file); return Fail("write capture output failed"); }
            offset += static_cast<std::size_t>(written);
        }
        return close(file) == 0 || Fail("close capture output failed");
    }
};
} // namespace cs2_overlay_capture
