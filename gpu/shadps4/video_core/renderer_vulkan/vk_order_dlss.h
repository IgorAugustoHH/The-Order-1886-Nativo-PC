// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (The Order: 1886): NVIDIA DLSS in place of the game's temporal MSAA resolve
// (customresolve_cs_msaa2x/4x_temporal). The resolve already receives everything DLSS needs:
// the MSAA HDR scene, the prepass velocity target and the exposure; the scene depth is the depth
// target drawn with that color. DLSS writes the resolve's output (also the game's history), so
// bloom, DoF, the focus and the tonemap downstream are untouched. BB_ORDER_DLSS=1 with BB_DLSS=1
// and the inherited Bloodborne upscaler off (BB_UPSCALER=none). Native resolution (DLAA) only:
// the game renders without jitter. See docs/order_dlss.md.
#pragma once

#include <array>
#include <chrono>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;

class OrderDlss {
public:
    OrderDlss(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
              Runtime& runtime);
    ~OrderDlss();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }
    [[nodiscard]] static bool IsResolve(u64 cs_hash) noexcept;

    /// Every draw: remembers the depth target an MSAA RGBA16F scene color is drawn with.
    void OnTargets(VideoCore::ImageId color, VideoCore::ImageId depth);

    /// After the resolve's resources are bound: records DLSS into its output. True when the
    /// game's dispatch must be skipped.
    bool Replace(std::span<const VideoCore::ImageId> images, std::span<const u32> flatbuf);

private:
    bool EnsureResources(u32 width, u32 height);
    vk::ImageView TempView(const VideoCore::Image& image, vk::Format format,
                           vk::ImageAspectFlags aspect);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;

    bool enabled = false;
    bool failed = false;
    bool depth_invert = false;
    VideoCore::ImageId scene_color{};
    VideoCore::ImageId scene_depth{};
    VideoCore::ImageId last_output{};
    u64 last_frame_dispatch = 0;
    u64 frames = 0;
    bool reset = true;
    std::chrono::steady_clock::time_point last_time{};

    u32 width = 0, height = 0;
    VideoCore::UniqueImage color_image, motion_image, depth_image, output_image;
    vk::UniqueImageView color_view, motion_view, depth_view, output_view;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
};

} // namespace Vulkan
