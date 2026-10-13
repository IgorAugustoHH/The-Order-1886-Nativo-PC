// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/host_shaders/depth_resample_frag.h"
#include "video_core/host_shaders/depth_stencil_resample_frag.h"
#include "video_core/host_shaders/depth_stencil_bits_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/ms_resample_color_frag.h"
#include "video_core/host_shaders/ms_resample_color_ms_frag.h"
#include "video_core/host_shaders/ms_resample_depth_frag.h"
#include "video_core/host_shaders/ms_resample_depth_ms_frag.h"
#include "video_core/host_shaders/ms_resample_uint_frag.h"
#include "video_core/host_shaders/ms_resample_uint_ms_frag.h"
#include "bbport_toggles.h"
#include <cstdio>
#include <cstdlib>

namespace Vulkan {
SceneTargets::SceneTargets(const Instance& i, Scheduler& s, Runtime& r,
                           VideoCore::TextureCache& t)
    : SceneTargets(i, s, r, [&t](VideoCore::ImageId id, u64 uid) {
        return t.TryGetImage(id, uid);
    }) {}
SceneTargets::SceneTargets(const Instance& i, Scheduler& s, Runtime& r, Lookup get)
    : instance{i}, scheduler{s}, runtime{r}, lookup{std::move(get)} {
    runtime.scene_targets = this;
    const char* force = std::getenv("BB_SCENE_STENCIL_BITS");
    force_stencil_bits = force && force[0] == '1';
    CreateResampleResources();
}
SceneTargets::~SceneTargets() {
    scheduler.Finish();
    runtime.scene_targets = nullptr;
}
vk::FormatFeatureFlags SceneTargets::Features(vk::Format format) const {
    // Called for every attachment of every draw: the driver query was ~5% of the GPU thread.
    // Core formats index a flat table; extension formats (large enum values) use the map.
    const auto index = u32(format);
    if (index < format_table.size() && format_known[index]) return format_table[index];
    if (index >= format_table.size()) {
        if (const auto it = format_features.find(format); it != format_features.end()) {
            return it->second;
        }
    }
    const auto features =
        instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    if (index < format_table.size()) {
        format_table[index] = features;
        format_known[index] = true;
    } else {
        format_features.emplace(format, features);
    }
    return features;
}
bool SceneTargets::Blittable(vk::Format format) const {
    const auto features = Features(format);
    const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
    return (features & required) == required;
}
bool SceneTargets::ShaderResampled(const VideoCore::Image& image) const {
    const auto format = image.backing->image.image_ci.format;
    if (!image.info.props.is_depth || (Blittable(format) && !force_stencil_bits)) return false;
    const auto features = Features(format);
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eDepthStencilAttachment;
    // Without shader stencil export (e.g. Pascal) depth/stencil stays native: the eight-draw
    // stencil resampler costs too much on such GPUs. BB_SCENE_STENCIL_BITS=1 forces it (tests).
    return (features & required) == required &&
           (!(image.aspect_mask & vk::ImageAspectFlagBits::eStencil) ||
            instance.IsShaderStencilExportSupported() || force_stencil_bits);
}
// 1 for the scene size, 2 for half resolution (the game's post-processing chain), else 0.
// Half resolution needs a scene size that halves exactly: the proxy keeps the scene's factor,
// so the shaders' FragCoord conversion (1920 / scene width) stays correct for it.
static u32 SizeDivisor(const VideoCore::ImageInfo& i, SceneResolution::Size scene) {
    if (i.size.width == 1920 && i.size.height == 1080) return 1;
    if (i.size.width == 960 && i.size.height == 540 && scene.width % 2 == 0 &&
        scene.height % 2 == 0 && !BbToggle::Disabled(BbToggle::SceneHalfRes)) {
        return 2;
    }
    return 0;
}
bool SceneTargets::Eligible(const VideoCore::Image& image) const {
    const auto& i = image.info;
    const u32 div = SizeDivisor(i, size);
    // The Order (allow_msaa): a single-sample image may currently have a multisampled backing
    // (drawn in a 2x pass); its proxy takes the pass's sample count instead (Attachment).
    if (!div || i.size.depth != 1 || i.resources.layers != 1 || i.props.is_block ||
        !image.backing || (!allow_msaa && image.backing->num_samples != i.num_samples)) {
        return false;
    }
    if (i.num_samples != 1) {
        // The Order: 2x MSAA scene targets, copied by MsResample.
        return allow_msaa && div == 1 && i.resources.levels == 1 &&
               MsCopyable(image.backing->image.image_ci.format);
    }
    if (i.resources.levels != 1) {
        // Mip chains (the bloom pyramid at half resolution): one blitted proxy per level.
        return div == 2 && i.resources.levels <= 16 && !i.props.is_depth &&
               Blittable(image.backing->image.image_ci.format);
    }
    return Blittable(image.backing->image.image_ci.format) || ShaderResampled(image);
}
bool SceneTargets::MsCopyable(vk::Format format) const {
    switch (format) {
    case vk::Format::eD32Sfloat:
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eA2B10G10R10UnormPack32:
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR8Unorm:
    case vk::Format::eR16G16Sfloat:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eR16Uint:
        return true;
    default:
        return false;
    }
}
bool SceneTargets::EligibleScene(const VideoCore::Image& image) const {
    return image.info.size.width == 1920 && image.info.size.height == 1080 && Eligible(image);
}
SceneResolution::Size SceneTargets::ProxySize(const VideoCore::Image& image, u32 level) const {
    const u32 div = std::max(1u, SizeDivisor(image.info, size));
    return {std::max(1u, size.width / div >> level), std::max(1u, size.height / div >> level)};
}
bool SceneTargets::SetSize(SceneResolution::Size next) {
    if (next == size) return false;
    ResolveAll();
    scheduler.Finish();
    entries.clear(); // no command buffer can still reference these images/views
    tracked.clear();
    ++generation;
    recent = {};
    size = next;
    std::printf("Scene resolution: raster %ux%u, half-resolution targets %ux%u, UI 1920x1080 "
                "(live)\n", size.width, size.height, size.width / 2, size.height / 2);
    return true;
}
void SceneTargets::ResolveAll() {
    for (auto& [key, entry] : entries) {
        if (entry->state.dirty) {
            if (auto* image = lookup(entry->source, entry->uid)) Copy(*entry, *image, true);
        }
    }
}
void SceneTargets::NativeAccess(VideoCore::Image& image, vk::AccessFlags2 access) {
    if (copying || !image.scene_proxy) return; // most images never had a proxy
    constexpr auto writes = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
        vk::AccessFlagBits2::eMemoryWrite;
    const u32 levels = std::min(16u, image.info.resources.levels);
    for (u32 level = 0; level < levels; ++level) {
        const auto it = entries.find(Key(image.image_uid, level));
        if (it == entries.end()) continue;
        auto& entry = *it->second;
        if (entry.state.dirty) Copy(entry, image, true);
        if (access & writes) entry.state.NativeWrite();
    }
}
void SceneTargets::Transition(Entry& e, vk::ImageAspectFlags aspect, vk::ImageLayout layout,
                               vk::PipelineStageFlags2 stages, vk::AccessFlags2 access) {
    scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = stages, .dstAccessMask = access,
        .oldLayout = e.layout, .newLayout = layout,
        .image = e.image, .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    scheduler.Record([barrier](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    e.layout = layout;
}
static u32 image_samples(const VideoCore::Image& image) {
    return image.backing ? image.backing->num_samples : 1;
}
void SceneTargets::Copy(Entry& e, VideoCore::Image& original, bool to_native) {
    copying = true;
    scheduler.EndRendering();
    const Runtime::TransferMark mark{runtime, to_native ? "scene proxy resolve" : "scene proxy fill",
                                     original};
    if (debug) {
        std::printf("Scene %s: %s %ux%u level %u\n", to_native ? "resolve" : "fill",
                    vk::to_string(original.info.pixel_format).c_str(), original.info.size.width,
                    original.info.size.height, e.level);
    }
    // The Order: the native image is copied at the game's own sample count (a single-sample
    // target may have a multisampled backing from a native 2x pass).
    if (allow_msaa && image_samples(original) != original.info.num_samples) {
        runtime.SetBackingSamples(&original, original.info.num_samples, !to_native);
    }
    const vk::Image src = to_native ? vk::Image(e.image) : original.GetImage();
    const vk::Image dst = to_native ? original.GetImage() : vk::Image(e.image);
    if (original.info.num_samples > 1 || e.samples > 1) {
        const bool depth = original.info.props.is_depth;
        constexpr auto read_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        const auto write_layout = depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                        : vk::ImageLayout::eColorAttachmentOptimal;
        constexpr auto read_stage = vk::PipelineStageFlagBits2::eFragmentShader;
        const auto write_stage = depth ? vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests
                                       : vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto read_access = vk::AccessFlagBits2::eShaderSampledRead;
        const auto write_access = depth ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                        : vk::AccessFlagBits2::eColorAttachmentWrite;
        runtime.Transit(&original, to_native ? write_layout : read_layout,
                        to_native ? write_stage : read_stage,
                        to_native ? write_access : read_access);
        runtime.FlushBarriers();
        Transition(e, original.aspect_mask, to_native ? read_layout : write_layout,
                   to_native ? read_stage : write_stage, to_native ? read_access : write_access);
        const auto& proxy = e.image.image_ci.extent;
        const u32 native_samples = original.info.num_samples;
        MsResample(src, to_native ? e.samples : native_samples, dst,
                   to_native ? native_samples : e.samples, original,
                   to_native ? vk::Extent2D{original.info.size.width, original.info.size.height}
                             : vk::Extent2D{proxy.width, proxy.height});
        if (to_native) e.state.Resolved(); else e.state.CopiedToProxy();
        copying = false;
        return;
    }
    if (ShaderResampled(original)) {
        constexpr auto read_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        constexpr auto write_layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        constexpr auto read_stage = vk::PipelineStageFlagBits2::eFragmentShader;
        constexpr auto write_stage = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                     vk::PipelineStageFlagBits2::eLateFragmentTests;
        constexpr auto read_access = vk::AccessFlagBits2::eShaderSampledRead;
        constexpr auto write_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        runtime.Transit(&original, to_native ? write_layout : read_layout,
                        to_native ? write_stage : read_stage,
                        to_native ? write_access : read_access);
        runtime.FlushBarriers();
        Transition(e, original.aspect_mask, to_native ? read_layout : write_layout,
                   to_native ? read_stage : write_stage, to_native ? read_access : write_access);
        const auto& proxy = e.image.image_ci.extent;
        Resample(src, dst, original,
                 to_native ? vk::Extent2D{original.info.size.width, original.info.size.height}
                           : vk::Extent2D{proxy.width, proxy.height});
        // (Depth proxies are single-level: Eligible.)
        if (to_native) e.state.Resolved(); else e.state.CopiedToProxy();
        copying = false;
        return;
    }
    const VideoCore::SubresourceRange level_range{.base = {e.level, 0}, .extent = {1, 1}};
    runtime.Transit(&original, to_native ? vk::ImageLayout::eTransferDstOptimal
                                       : vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer,
                    to_native ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead,
                    level_range);
    runtime.FlushBarriers();
    Transition(e, original.aspect_mask,
               to_native ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eTransferDstOptimal,
               vk::PipelineStageFlagBits2::eTransfer,
               to_native ? vk::AccessFlagBits2::eTransferRead : vk::AccessFlagBits2::eTransferWrite);
    const vk::Offset3D native{s32(std::max(1u, original.info.size.width >> e.level)),
                              s32(std::max(1u, original.info.size.height >> e.level)), 1};
    const vk::Offset3D reduced{s32(e.image.image_ci.extent.width),
                               s32(e.image.image_ci.extent.height), 1};
    for (auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
                        vk::ImageAspectFlagBits::eStencil}) {
        if (!(original.aspect_mask & aspect)) continue;
        const vk::ImageBlit region{
            .srcSubresource = {aspect, to_native ? 0u : e.level, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{}, to_native ? reduced : native},
            .dstSubresource = {aspect, to_native ? e.level : 0u, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{}, to_native ? native : reduced},
        };
        // Nearest preserves depth/stencil and avoids assuming linear blit support for
        // integer G-buffer formats. FSR reconstructs color at the HDR scene boundary.
        scheduler.Record([src,dst,region](vk::CommandBuffer cmd) {
            cmd.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                          vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        });
    }
    if (to_native) e.state.Resolved(); else e.state.CopiedToProxy();
    copying = false;
}
void SceneTargets::Resample(vk::Image src, vk::Image dst, const VideoCore::Image& original,
                            vk::Extent2D dst_size) {
    const auto device = instance.GetDevice();
    const auto format = original.backing->image.image_ci.format;
    const bool stencil = bool(original.aspect_mask & vk::ImageAspectFlagBits::eStencil);
    const bool stencil_bits = stencil && !depth_stencil_frag;
    const auto make_view = [&](vk::Image image, vk::ImageAspectFlags aspect,
                               vk::ImageUsageFlags usage) {
        const vk::ImageViewUsageCreateInfo usage_ci{.usage = usage};
        return Check(device.createImageView({
            .pNext = &usage_ci, .image = image, .viewType = vk::ImageViewType::e2D,
            .format = format, .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    const auto depth_view = make_view(src, vk::ImageAspectFlagBits::eDepth,
                                      vk::ImageUsageFlagBits::eSampled);
    const auto stencil_view = stencil ? make_view(src, vk::ImageAspectFlagBits::eStencil,
                                                  vk::ImageUsageFlagBits::eSampled)
                                      : vk::ImageView{};
    const auto target_view = make_view(dst, original.aspect_mask,
                                       vk::ImageUsageFlagBits::eDepthStencilAttachment);
    scheduler.DeferOperation([device, depth_view, stencil_view, target_view] {
        device.destroyImageView(depth_view);
        if (stencil_view) device.destroyImageView(stencil_view);
        device.destroyImageView(target_view);
    });
    const auto pipeline = ResamplePipeline(format, stencil);
    const auto layout = *resample_layout;
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = target_view,
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        auto stencil_attachment = attachment;
        if (stencil_bits) {
            stencil_attachment.loadOp = vk::AttachmentLoadOp::eClear;
            stencil_attachment.clearValue.depthStencil = vk::ClearDepthStencilValue{0.f, 0};
        }
        cmd.beginRendering({
            .renderArea = {{0, 0}, dst_size}, .layerCount = 1,
            .pDepthAttachment = &attachment,
            .pStencilAttachment = stencil ? &stencil_attachment : nullptr,
        });
        const std::array infos{
            vk::DescriptorImageInfo{{}, depth_view, vk::ImageLayout::eShaderReadOnlyOptimal},
            vk::DescriptorImageInfo{{}, stencil_view, vk::ImageLayout::eShaderReadOnlyOptimal},
        };
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[0]},
            vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[1]},
        };
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0,
                                 vk::ArrayProxy<const vk::WriteDescriptorSet>(
                                     stencil ? 2u : 1u, writes.data()));
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmd.setViewportWithCount(vk::Viewport{0.f, 0.f, float(dst_size.width),
                                              float(dst_size.height), 0.f, 1.f});
        cmd.setScissorWithCount(vk::Rect2D{{0, 0}, dst_size});
        if (stencil_bits) {
            // Copy depth first, then reconstruct all eight stencil bits without needing
            // VK_EXT_shader_stencil_export (e.g. Pascal). Discard keeps unset bits clear.
            for (u32 bit : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u}) {
                cmd.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(bit), &bit);
                cmd.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, bit);
                cmd.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, bit);
                cmd.draw(3, 1, 0, 0);
            }
        } else {
            cmd.draw(3, 1, 0, 0);
        }
        cmd.endRendering();
    });
    // The guest pipelines' dynamic state must be emitted again after this pipeline.
    scheduler.GetDynamicState().Invalidate();
}
void SceneTargets::CreateResampleResources() {
    const auto device = instance.GetDevice();
    fs_tri_vert = vk::UniqueShaderModule(CompileSPV(FS_TRI_VERT, device), device);
    const auto module = [&](const auto& code) {
        return vk::UniqueShaderModule(CompileSPV(code, device), device);
    };
    ms_frags[0][0] = module(MS_RESAMPLE_COLOR_FRAG);
    ms_frags[0][1] = module(MS_RESAMPLE_COLOR_MS_FRAG);
    ms_frags[1][0] = module(MS_RESAMPLE_UINT_FRAG);
    ms_frags[1][1] = module(MS_RESAMPLE_UINT_MS_FRAG);
    ms_frags[2][0] = module(MS_RESAMPLE_DEPTH_FRAG);
    ms_frags[2][1] = module(MS_RESAMPLE_DEPTH_MS_FRAG);
    depth_frag = vk::UniqueShaderModule(CompileSPV(DEPTH_RESAMPLE_FRAG, device), device);
    if (instance.IsShaderStencilExportSupported() && !force_stencil_bits) {
        depth_stencil_frag =
            vk::UniqueShaderModule(CompileSPV(DEPTH_STENCIL_RESAMPLE_FRAG, device), device);
    }
    if (!depth_stencil_frag) {
        stencil_bits_frag =
            vk::UniqueShaderModule(CompileSPV(DEPTH_STENCIL_BITS_FRAG, device), device);
    }
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eFragment},
    };
    resample_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data(),
    }));
    const auto set_layout = *resample_set_layout;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(u32)};
    resample_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    }));
}
vk::Pipeline SceneTargets::ResamplePipeline(vk::Format format, bool stencil) {
    const bool stencil_bits = stencil && !depth_stencil_frag;
    const std::pair key{format, stencil};
    for (const auto& [k, pipeline] : resample_pipelines) if (k == key) return *pipeline;
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    // Reference, compare and write masks are static; the exported value replaces the reference.
    const vk::StencilOpState stencil_op{
        .failOp = vk::StencilOp::eReplace, .passOp = vk::StencilOp::eReplace,
        .depthFailOp = vk::StencilOp::eReplace, .compareOp = vk::CompareOp::eAlways,
        .compareMask = 0xff, .writeMask = 0xff, .reference = 0,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state{
        .depthTestEnable = true, .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
        .stencilTestEnable = stencil, .front = stencil_op, .back = stencil_op,
    };
    const std::array dynamic_states{vk::DynamicState::eViewportWithCount,
                                    vk::DynamicState::eScissorWithCount,
                                    vk::DynamicState::eStencilWriteMask,
                                    vk::DynamicState::eStencilReference};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = stencil_bits ? 4u : 2u, .pDynamicStates = dynamic_states.data(),
    };
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                                          .module = *fs_tri_vert, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                          .module = stencil ? (stencil_bits ? *stencil_bits_frag :
                                                             *depth_stencil_frag) : *depth_frag,
                                          .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .depthAttachmentFormat = format,
        .stencilAttachmentFormat = stencil ? format : vk::Format::eUndefined,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1.f};
    auto pipeline = Check(instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, {
        .pNext = &rendering, .stageCount = u32(stages.size()), .pStages = stages.data(),
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info, .pRasterizationState = &raster,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending, .pDynamicState = &dynamic_info,
        .layout = *resample_layout,
    }));
    const auto result = *pipeline;
    resample_pipelines.emplace_back(key, std::move(pipeline));
    return result;
}
void SceneTargets::MsResample(vk::Image src, u32 src_samples, vk::Image dst, u32 dst_samples,
                              const VideoCore::Image& original, vk::Extent2D dst_size) {
    const auto device = instance.GetDevice();
    const auto format = original.backing->image.image_ci.format;
    const bool depth = original.info.props.is_depth;
    const auto aspect = depth ? vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth)
                              : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
    const auto make_view = [&](vk::Image image, vk::ImageUsageFlags usage) {
        const vk::ImageViewUsageCreateInfo usage_ci{.usage = usage};
        return Check(device.createImageView({
            .pNext = &usage_ci, .image = image, .viewType = vk::ImageViewType::e2D,
            .format = format, .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    const auto source_view = make_view(src, vk::ImageUsageFlagBits::eSampled);
    const auto target_view = make_view(dst, depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                                                  : vk::ImageUsageFlagBits::eColorAttachment);
    scheduler.DeferOperation([device, source_view, target_view] {
        device.destroyImageView(source_view);
        device.destroyImageView(target_view);
    });
    const u32 kind = depth ? 2u : format == vk::Format::eR16Uint ? 1u : 0u;
    const auto pipeline = MsResamplePipeline(format, kind, src_samples > 1, dst_samples);
    const auto layout = *resample_layout;
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = target_view,
            .imageLayout = depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                 : vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmd.beginRendering({
            .renderArea = {{0, 0}, dst_size},
            .layerCount = 1,
            .colorAttachmentCount = depth ? 0u : 1u,
            .pColorAttachments = depth ? nullptr : &attachment,
            .pDepthAttachment = depth ? &attachment : nullptr,
        });
        const vk::DescriptorImageInfo info{{}, source_view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::WriteDescriptorSet write{.dstBinding = 0,
                                           .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eSampledImage,
                                           .pImageInfo = &info};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0, write);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmd.setViewportWithCount(vk::Viewport{0.f, 0.f, float(dst_size.width),
                                              float(dst_size.height), 0.f, 1.f});
        cmd.setScissorWithCount(vk::Rect2D{{0, 0}, dst_size});
        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();
    });
    scheduler.GetDynamicState().Invalidate();
}
vk::Pipeline SceneTargets::MsResamplePipeline(vk::Format format, u32 kind, bool src_ms,
                                              u32 samples) {
    const bool depth = kind == 2;
    const std::tuple key{format, kind, src_ms, samples};
    for (const auto& [k, pipeline] : ms_pipelines) if (k == key) return *pipeline;
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits(samples),
        .sampleShadingEnable = true,
        .minSampleShading = 1.0f,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state{
        .depthTestEnable = depth, .depthWriteEnable = depth,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states{vk::DynamicState::eViewportWithCount,
                                    vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = u32(dynamic_states.size()), .pDynamicStates = dynamic_states.data(),
    };
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                                          .module = *fs_tri_vert, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                          .module = *ms_frags[kind][src_ms ? 1 : 0],
                                          .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = depth ? 0u : 1u,
        .pColorAttachmentFormats = depth ? nullptr : &format,
        .depthAttachmentFormat = depth ? format : vk::Format::eUndefined,
    };
    const vk::PipelineColorBlendAttachmentState blend{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .attachmentCount = depth ? 0u : 1u, .pAttachments = &blend,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1.f};
    auto pipeline = Check(instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, {
        .pNext = &rendering, .stageCount = u32(stages.size()), .pStages = stages.data(),
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info, .pRasterizationState = &raster,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending, .pDynamicState = &dynamic_info,
        .layout = *resample_layout,
    }));
    const auto result = *pipeline;
    ms_pipelines.emplace_back(key, std::move(pipeline));
    return result;
}
SceneTargets::Entry& SceneTargets::Get(VideoCore::ImageId id, u32 level, u32 samples) {
    auto& original = *lookup(id, 0);
    const u64 key = Key(original.image_uid, level);
    // A draw has up to six targets, mostly the same as the previous draw's.
    for (const auto& [recent_key, recent_entry] : recent) {
        if (recent_entry && recent_key == key) {
            if (!recent_entry->state.valid) Copy(*recent_entry, original, false);
            return *recent_entry;
        }
    }
    auto& entry = entries[key];
    original.scene_proxy = true;
    if (!entry) {
        entry = std::make_unique<Entry>();
        entry->source = id;
        entry->uid = original.image_uid;
        entry->level = level;
        entry->image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        auto ci = original.backing->image.image_ci;
        ci.pNext = nullptr;
        const auto proxy = ProxySize(original, level);
        ci.extent = vk::Extent3D{proxy.width, proxy.height, 1};
        ci.mipLevels = 1;
        if (allow_msaa) {
            // The Order: the pass's sample count, else the game's (not a 2x backing's).
            entry->samples = samples ? samples : original.info.num_samples;
            ci.samples = vk::SampleCountFlagBits(entry->samples);
            if (entry->samples > 1) {
                ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
            }
        }
        entry->image.Create(ci);
        tracked.insert(original.image_uid);
    }
    recent[recent_next++ % recent.size()] = {key, entry.get()};
    if (!entry->state.valid) Copy(*entry, original, false);
    return *entry;
}
vk::ImageView SceneTargets::View(Entry& e, const VideoCore::Image& original,
                                 const VideoCore::ImageViewInfo& info) {
    for (const auto& [key,view] : e.views) if (key == info) return *view;
    const auto aspect = original.info.props.is_depth
        ? (info.is_storage ? original.aspect_mask : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth))
        : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
    const auto format = original.info.props.is_depth ? original.backing->image.image_ci.format : info.format;
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .image = e.image, .viewType = vk::ImageViewType::e2D, .format = format,
        .components = info.mapping, .subresourceRange = {aspect,0,1,0,1},
    }));
    const auto result = *view;
    e.views.emplace_back(info, std::move(view));
    return result;
}
SceneTargets::Target SceneTargets::Attachment(VideoCore::ImageId id,
                                               const VideoCore::ImageViewInfo& info, u32 samples,
                                               bool writes) {
    auto& original = *lookup(id, 0);
    auto& e = Get(id, info.range.base.level, samples);
    const bool depth = original.info.props.is_depth;
    const auto layout = depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                              : vk::ImageLayout::eColorAttachmentOptimal;
    // Consecutive draws can stay in the same render pass.
    if (e.layout != layout) {
        Transition(e, original.aspect_mask, layout,
            depth ? vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests
                  : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eColorAttachmentOutput),
            depth ? vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                  : vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    }
    // A depth attachment without depth writes (soft particles that also sample it) leaves
    // the native image current: no resolve before every such draw.
    if (writes) e.state.ProxyWrite();
    auto attachment_info = info;
    attachment_info.is_storage = true; // include stencil in attachment views
    return {e.image, View(e, original, attachment_info), e.layout, e.image.image_ci.usage};
}
bool SceneTargets::ProxyCurrent(const VideoCore::Image& image, u32 level, u32 levels) const {
    if (copying || !image.scene_proxy || image.info.props.is_depth || levels != 1) {
        return false;
    }
    const auto it = entries.find(Key(image.image_uid, level));
    return it != entries.end() && it->second->state.valid;
}
vk::ImageLayout SceneTargets::PrepareSample(const VideoCore::Image& image, u32 level) {
    auto& e = *entries.find(Key(image.image_uid, level))->second;
    constexpr auto layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    if (e.layout != layout) {
        Transition(e, image.aspect_mask, layout,
                   vk::PipelineStageFlagBits2::eVertexShader |
                       vk::PipelineStageFlagBits2::eFragmentShader |
                       vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderSampledRead);
    }
    return layout;
}
std::optional<SceneTargets::Target> SceneTargets::SampleProxy(
    const VideoCore::Image& image, const VideoCore::ImageViewInfo& info) {
    const u32 level = info.range.base.level;
    if (!ProxyCurrent(image, level, info.range.extent.levels)) {
        return std::nullopt;
    }
    auto& e = *entries.find(Key(image.image_uid, level))->second;
    const auto layout = PrepareSample(image, level);
    return Target{e.image, View(e, image, info), layout, e.image.image_ci.usage};
}
SceneTargets::Target SceneTargets::Read(VideoCore::ImageId id,
                                       const VideoCore::ImageViewInfo& info,
                                       vk::PipelineStageFlags2 stages, vk::AccessFlags2 access) {
    auto& original = *lookup(id, 0);
    auto& e = Get(id);
    Transition(e, original.aspect_mask, vk::ImageLayout::eGeneral, stages, access);
    return {e.image, View(e, original, info), e.layout, e.image.image_ci.usage};
}
} // namespace Vulkan
