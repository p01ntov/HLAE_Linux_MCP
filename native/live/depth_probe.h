#pragma once
#include "depth_provenance.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

namespace cs2_camera_depth {

// An explicit diagnostic path activates this probe for a launcher-owned offline
// session. It never grants the renderer a depth capability and never changes GPU
// commands. Keep per-frame writes bounded to one sample every 60 presentations.
inline const char* ProbePath() noexcept {
    static const std::string path = []() noexcept {
        // Proc lookup is part of the application's normal Vulkan path, even
        // before a Probe exists. Diagnostic allocation failure must not escape
        // through that ABI or alter the original function lookup.
        try {
            const char* value = std::getenv("CS2_CAMERA_DEPTH_PROBE");
            return value ? std::string(value) : std::string();
        } catch (...) { return std::string(); }
    }();
    return path.empty() ? nullptr : path.c_str();
}

struct Probe {
    Provenance model;
    std::ofstream stream;
    std::uint64_t presents = 0;
    bool failed = false;
    const std::uint64_t device_serial = NextSerial();
    static std::uint64_t NextSerial() { static std::atomic<std::uint64_t> serial{0}; return ++serial; }
    static std::int64_t Session() {
        static const auto session = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(session).count();
    }
#define DEPTH_FN(name) PFN_vk##name name{};
    DEPTH_FN(CreateImage) DEPTH_FN(DestroyImage) DEPTH_FN(CreateImageView) DEPTH_FN(DestroyImageView)
    DEPTH_FN(CreateRenderPass) DEPTH_FN(CreateRenderPass2) DEPTH_FN(CreateRenderPass2KHR) DEPTH_FN(DestroyRenderPass)
    DEPTH_FN(CreateFramebuffer) DEPTH_FN(DestroyFramebuffer)
    DEPTH_FN(CreateCommandPool) DEPTH_FN(ResetCommandPool) DEPTH_FN(DestroyCommandPool)
    DEPTH_FN(AllocateCommandBuffers) DEPTH_FN(FreeCommandBuffers)
    DEPTH_FN(BeginCommandBuffer) DEPTH_FN(EndCommandBuffer) DEPTH_FN(ResetCommandBuffer)
    DEPTH_FN(CmdBeginRenderPass) DEPTH_FN(CmdNextSubpass) DEPTH_FN(CmdEndRenderPass)
    DEPTH_FN(CmdBeginRenderPass2) DEPTH_FN(CmdNextSubpass2) DEPTH_FN(CmdEndRenderPass2)
    DEPTH_FN(CmdBeginRenderPass2KHR) DEPTH_FN(CmdNextSubpass2KHR) DEPTH_FN(CmdEndRenderPass2KHR)
    DEPTH_FN(CmdBeginRendering) DEPTH_FN(CmdEndRendering) DEPTH_FN(CmdBeginRenderingKHR) DEPTH_FN(CmdEndRenderingKHR)
    DEPTH_FN(CmdExecuteCommands) DEPTH_FN(QueueSubmit) DEPTH_FN(QueueSubmit2) DEPTH_FN(QueueSubmit2KHR)
#undef DEPTH_FN

    Probe(VkDevice device, PFN_vkGetDeviceProcAddr next) {
        if (const char* path = ProbePath()) stream.open(path, std::ios::app);
#define DEPTH_LOAD(name) name = reinterpret_cast<PFN_vk##name>(next(device, "vk" #name));
        DEPTH_LOAD(CreateImage) DEPTH_LOAD(DestroyImage) DEPTH_LOAD(CreateImageView) DEPTH_LOAD(DestroyImageView)
        DEPTH_LOAD(CreateRenderPass) DEPTH_LOAD(CreateRenderPass2) DEPTH_LOAD(CreateRenderPass2KHR) DEPTH_LOAD(DestroyRenderPass)
        DEPTH_LOAD(CreateFramebuffer) DEPTH_LOAD(DestroyFramebuffer)
        DEPTH_LOAD(CreateCommandPool) DEPTH_LOAD(ResetCommandPool) DEPTH_LOAD(DestroyCommandPool)
        DEPTH_LOAD(AllocateCommandBuffers) DEPTH_LOAD(FreeCommandBuffers)
        DEPTH_LOAD(BeginCommandBuffer) DEPTH_LOAD(EndCommandBuffer) DEPTH_LOAD(ResetCommandBuffer)
        DEPTH_LOAD(CmdBeginRenderPass) DEPTH_LOAD(CmdNextSubpass) DEPTH_LOAD(CmdEndRenderPass)
        DEPTH_LOAD(CmdBeginRenderPass2) DEPTH_LOAD(CmdNextSubpass2) DEPTH_LOAD(CmdEndRenderPass2)
        DEPTH_LOAD(CmdBeginRenderPass2KHR) DEPTH_LOAD(CmdNextSubpass2KHR) DEPTH_LOAD(CmdEndRenderPass2KHR)
        DEPTH_LOAD(CmdBeginRendering) DEPTH_LOAD(CmdEndRendering) DEPTH_LOAD(CmdBeginRenderingKHR) DEPTH_LOAD(CmdEndRenderingKHR)
        DEPTH_LOAD(CmdExecuteCommands) DEPTH_LOAD(QueueSubmit) DEPTH_LOAD(QueueSubmit2) DEPTH_LOAD(QueueSubmit2KHR)
#undef DEPTH_LOAD
    }

    template<class F> void Observe(F&& operation) noexcept {
        if (failed) return;
        try { operation(model); } catch (...) { failed = true; }
    }

    void Present(VkExtent2D extent) noexcept {
        if (++presents % 60 != 1 || !stream || failed) return;
        try {
            nlohmann::json samples = nlohmann::json::array();
            for (const auto& event : model.Take()) {
                const auto& image = event.image;
                const auto& view = event.view;
                const bool extentMatch = image.extent.width == extent.width && image.extent.height == extent.height &&
                    event.renderArea.extent.width == extent.width && event.renderArea.extent.height == extent.height &&
                    event.renderArea.offset.x == 0 && event.renderArea.offset.y == 0;
                samples.push_back({{"image_generation", image.generation}, {"format", int(image.format)},
                    {"width", image.extent.width}, {"height", image.extent.height}, {"samples", unsigned(image.samples)},
                    {"usage", image.usage}, {"image_flags", image.flags}, {"tiling", int(image.tiling)},
                    {"view_type", int(view.type)}, {"view_mip", view.range.baseMipLevel},
                    {"view_levels", view.range.levelCount}, {"view_layer", view.range.baseArrayLayer},
                    {"view_layers", view.range.layerCount}, {"layers", image.layers}, {"view_mask", event.viewMask},
                    {"render_area", {event.renderArea.offset.x, event.renderArea.offset.y,
                                      event.renderArea.extent.width, event.renderArea.extent.height}},
                    {"load_op", int(event.load)}, {"store_op", int(event.store)},
                    {"layout_during", int(event.during)}, {"layout_at_pass_end", int(event.after)},
                    {"queue_family", event.family}, {"subpass", event.subpass}, {"dynamic", event.dynamic},
                    {"ended", event.ended}, {"continuation", event.continuation},
                    {"observations", event.observations}, {"last_submission", event.submissions},
                    {"presentation_extent_match", extentMatch},
                    {"has_transfer_src_usage", bool(image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)},
                    {"has_sampled_usage", bool(image.usage & VK_IMAGE_USAGE_SAMPLED_BIT)},
                    {"transient", bool(image.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)}});
            }
            stream << nlohmann::json{{"schema", 1}, {"event", "depth_attachment_provenance"},
                {"session", Session()}, {"device_serial", device_serial},
                {"presentation", presents}, {"presentation_extent", {extent.width, extent.height}},
                {"tracked_depth_images", model.ImageCount()}, {"submission_batches", model.Submissions()},
                {"dropped", model.Dropped()}, {"depth_available", false},
                {"reason", "Passive provenance only; main-view identity, contents and synchronization are not validated"},
                {"attachments", samples}}.dump() << '\n';
            stream.flush();
        } catch (...) { failed = true; }
    }
};

} // namespace cs2_camera_depth
