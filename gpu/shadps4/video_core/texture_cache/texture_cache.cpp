#include "bb_scene_diag.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>

#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/cache_gc.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

void TextureCache::ProcessDownloadImages() {
    std::unique_lock lk{download_images_mutex};
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DumpImage(VAddr address, const std::string& path) {
    ImageId found{};
    u64 found_size = 0;
    ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
        // Every image starting here: a texture read of a render target can land on a copy.
        if (image.info.guest_address == address) {
            std::printf("Image dump: %#llx has image %u: %ux%ux%u %s, %u samples, %u layers, "
                        "tick %llu%s\n",
                        (unsigned long long)address, image_id.index, image.info.size.width,
                        image.info.size.height, image.info.size.depth,
                        vk::to_string(image.info.pixel_format).c_str(), image.info.num_samples,
                        image.info.resources.layers, (unsigned long long)image.tick_accessed_last,
                        True(image.flags & ImageFlagBits::GpuModified) ? ", GPU-modified" : "");
        }
        if (image.info.guest_address == address && image.info.guest_size > found_size) {
            found = image_id;
            found_size = image.info.guest_size;
        }
    });
    if (!found) {
        std::printf("Image dump: no image at %#llx\n", (unsigned long long)address);
        return;
    }
    Image& image = slot_images[found];
    const auto& info = image.info;
    if (info.props.is_depth && info.num_samples > 1) {
        DumpMsaaDepth(image, path);
        return;
    }
    if (info.props.is_block || info.num_samples > 1) {
        std::printf("Image dump: %#llx is compressed or multisampled, skipped\n",
                    (unsigned long long)address);
        return;
    }
    const u32 width = info.size.width, height = info.size.height;
    const bool volume = info.props.is_volume;
    const u32 slices = volume ? info.size.depth : info.resources.layers;
    const u32 texel = info.num_bits / 8;
    const u64 size = u64(width) * height * slices * texel;
    const auto staging = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16);
    const vk::BufferImageCopy copy = {
        .bufferOffset = staging.offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                  : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = volume ? 1u : slices,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, volume ? slices : 1u},
    };
    runtime.DownloadImage(&image, staging.buffer, std::span{&copy, 1});
    scheduler.Finish();
    staging.Invalidate();
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "%u %u %u %u %s\n", width, height, slices, texel,
                     vk::to_string(info.pixel_format).c_str());
        std::fwrite(staging.mapped, 1, size, f);
        std::fclose(f);
        std::printf("Image dump: %#llx %ux%ux%u %s -> %s\n", (unsigned long long)address, width,
                    height, slices, vk::to_string(info.pixel_format).c_str(), path.c_str());
    }
}

void TextureCache::DumpFocusDepth(ImageId id, const std::string& path) {
    if (!id || path.empty()) return;
    auto& image = slot_images[id];
    const auto& info = image.info;
    if (info.pixel_format != vk::Format::eD32Sfloat || !info.props.is_depth ||
        info.num_samples <= 1 || info.resources.layers != 1 ||
        u64(info.size.width) * info.size.height * 4 > 8_MB) {
        std::printf("Focus depth snapshot: unsupported bound image %u\n", id.index);
        return;
    }
    DumpMsaaDepth(image, path);
}

void TextureCache::DumpFocusScalar(ImageId id, const std::string& path) {
    if (!id || path.empty()) return;
    auto& image = slot_images[id];
    const auto& info = image.info;
    if (info.pixel_format != vk::Format::eR32Sfloat || info.size.width != 1 ||
        info.size.height != 1 || info.size.depth != 1 || info.num_samples != 1 ||
        info.resources.layers != 1 || info.resources.levels != 1) return;
    const auto staging = runtime.GetStagingPool().Request(4, MemoryType::HostCached, 16);
    const vk::BufferImageCopy copy{
        .bufferOffset = staging.offset,
        .imageSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent{1, 1, 1},
    };
    runtime.DownloadImage(&image, staging.buffer, std::span{&copy, 1});
    scheduler.Finish();
    staging.Invalidate();
    if (auto* file = std::fopen(path.c_str(), "wb")) {
        std::fprintf(file, "1 1 1 4 R32Sfloat\n");
        std::fwrite(staging.mapped, 1, 4, file);
        std::fclose(file);
        float value;
        std::memcpy(&value, staging.mapped, sizeof(value));
        std::printf("Focus scalar dump: %#llx value %.9g -> %s\n",
                    static_cast<unsigned long long>(info.guest_address), value, path.c_str());
    }
}

void TextureCache::ClearImageAt(VAddr address, const std::array<float, 4>& color) {
    bool cleared = false;
    ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address || image.info.props.is_depth) {
            return;
        }
        SubresourceRange range{};
        range.extent.levels = image.info.resources.levels;
        range.extent.layers = image.info.resources.layers;
        runtime.ClearImage(&image, range,
                           vk::ClearValue{.color = vk::ClearColorValue{color}});
        cleared = true;
    });
    std::printf("Image clear: %#llx %s (%g %g %g %g)\n", (unsigned long long)address,
                cleared ? "cleared" : "no colour image", color[0], color[1], color[2], color[3]);
}

/// Sample 0 of every pixel of a multisampled depth image (copies cannot read multisampled
/// images; a depth resolve in an empty render pass can), saved like DumpImage.
void TextureCache::DumpMsaaDepth(Image& image, const std::string& path) {
    const auto& info = image.info;
    const vk::Device device = instance.GetDevice();
    const u32 width = info.size.width, height = info.size.height;
    vk::ImageCreateInfo image_ci = image.backing->image.image_ci;
    image_ci.flags = {};
    image_ci.imageType = vk::ImageType::e2D;
    image_ci.extent = vk::Extent3D{width, height, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = vk::SampleCountFlagBits::e1;
    image_ci.tiling = vk::ImageTiling::eOptimal;
    image_ci.usage =
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferSrc;
    image_ci.initialLayout = vk::ImageLayout::eUndefined;
    UniqueImage resolved{device, instance.GetAllocator()};
    resolved.Create(image_ci);

    const auto make_view = [&](vk::Image handle) {
        return device
            .createImageViewUnique(vk::ImageViewCreateInfo{
                .image = handle,
                .viewType = vk::ImageViewType::e2D,
                .format = image_ci.format,
                .subresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
            })
            .value;
    };
    const auto src_view = make_view(image.GetImage());
    const auto dst_view = make_view(resolved.image);

    constexpr auto layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    scheduler.EndRendering();
    runtime.Transit(&image, layout, vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
    runtime.FlushBarriers();

    const u32 texel = info.num_bits / 8;
    const u64 size = u64(width) * height * 4; // the depth aspect copies as 32-bit floats
    const auto staging = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16);
    scheduler.Record([src = *src_view, dst = *dst_view, dst_image = resolved.image, width, height,
                      buffer = staging.buffer->Handle(), offset = staging.offset,
                      layout](vk::CommandBuffer cmdbuf) {
        const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
            const vk::ImageMemoryBarrier2 b = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = from,
                .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = &b});
        };
        barrier(vk::ImageLayout::eUndefined, layout);
        const vk::RenderingAttachmentInfo depth = {
            .imageView = src,
            .imageLayout = layout,
            .resolveMode = vk::ResolveModeFlagBits::eSampleZero,
            .resolveImageView = dst,
            .resolveImageLayout = layout,
            .loadOp = vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmdbuf.beginRendering(vk::RenderingInfo{
            .renderArea = {{0, 0}, {width, height}},
            .layerCount = 1,
            .pDepthAttachment = &depth,
        });
        cmdbuf.endRendering();
        barrier(layout, vk::ImageLayout::eTransferSrcOptimal);
        const vk::BufferImageCopy copy = {
            .bufferOffset = offset,
            .imageSubresource{vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyImageToBuffer(dst_image, vk::ImageLayout::eTransferSrcOptimal, buffer, copy);
    });
    scheduler.Finish();
    staging.Invalidate();
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "%u %u 1 4 %s-sample0\n", width, height,
                     vk::to_string(info.pixel_format).c_str());
        std::fwrite(staging.mapped, 1, size, f);
        std::fclose(f);
        std::printf("Image dump: %#llx %ux%u %s sample 0 (of %u) -> %s\n",
                    (unsigned long long)info.guest_address, width, height,
                    vk::to_string(info.pixel_format).c_str(), info.num_samples, path.c_str());
    }
    (void)texel;
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download.mapped, download_size);
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        new_info.UpdateSize();
        const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        if (!runtime.CopyColorAndDepth(&cache_image, &new_image)) {
            // A missing transfer is not a successful cache conversion. Keep the
            // only GPU copy of the old contents; let normal overlap handling retry.
            FreeImage(new_image_id);
            return {};
        }

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        static int unresolvable_logs = 0;
        if (unresolvable_logs++ < 4)
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        // BBPort: the address was reused for an unrelated image (pool allocation).
        // Treat it like the tile-mode case instead of aborting the game.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }
        return {merged_image_id, -1, -1};
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    runtime.CopyImage(&src_image, &new_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format;
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level = cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer = cached.view_slice;
        }
        return cached.image_id;
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.insert(instance, runtime, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    // bbport: an image claiming more than 512 MiB of guest memory is a garbage descriptor
    // (The Order: 1886: a 3 GiB upload exhausted VRAM). Leave its contents alone.
    if (image.info.guest_size > (512ull << 20)) {
        static int warned = 0;
        if (warned++ < 16) {
            LOG_WARNING(Render_Vulkan, "Not uploading a {} MiB image at {:#x} ({}x{}x{}, {} bits)",
                        image.info.guest_size >> 20, image.info.guest_address,
                        image.info.size.width, image.info.size.height, image.info.size.depth,
                        image.info.num_bits);
        }
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    BbStats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    // bbport: run80 stopped on system RAM after four uploads of ~550 MiB each (host staging plus
    // a detiling buffer of the same size in VRAM). Which images are that big?
    if (image.info.guest_size >= (128ull << 20)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 32) {
            std::printf("Big image upload: %llu MiB at %#llx, %ux%ux%u, %u layers, %u mips, "
                        "%u bits, vk format %u, type %u, pitch %u, tiling %u, %s%s\n",
                        (unsigned long long)(image.info.guest_size >> 20),
                        (unsigned long long)image.info.guest_address, image.info.size.width,
                        image.info.size.height, image.info.size.depth,
                        image.info.resources.layers, image.info.resources.levels,
                        image.info.num_bits, u32(image.info.pixel_format), u32(image.info.type),
                        image.info.pitch, u32(image.info.tile_mode),
                        is_gpu_modified ? "GPU-modified " : "",
                        image.info.props.is_tiled ? "tiled" : "linear");
        }
    }
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
    if (BbSceneDiag::Enabled()) {
        BbSceneDiag::image_bytes += image.info.guest_size;
        ++BbSceneDiag::image_uploads;
        BbSceneDiag::Event("image_upload_submitted", image.info.guest_address, image.info.guest_size, image_copies.size());
    }
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base,
                                                        is_depth, extra_lod_bias);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }

    return it->second.Handle();
}

// bbport: what fills VRAM (The Order: 1886 ran out of a 6 GB card): registered image bytes
// (guest sizes) and buffer cache blocks made resident, which are never released.
static std::atomic<u64> g_image_bytes{0};
static std::atomic<u64> g_image_count{0};
extern std::atomic<u64> g_bb_resident_buffer_bytes;

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    // Heap usage includes buffers and driver allocations; do not mix it with guest sizes.
    if (!instance.CanReportMemoryUsage()) {
        total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    }
    g_image_bytes += Common::AlignUp(image.info.guest_size, 1024);
    ++g_image_count;
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    if (!instance.CanReportMemoryUsage()) {
        total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    }
    g_image_bytes -= Common::AlignUp(image.info.guest_size, 1024);
    --g_image_count;
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectImages() {
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        // bbport: on integrated GPUs (Steam Deck) the usage covers system-memory heaps holding
        // much more than images (buffers backing guest memory), and the startup budget left
        // ~1 GB after its 8 GB system reserve: usage stayed above the critical mark, so the
        // collector evicted images used two or three frames ago on every submission and wrote
        // GPU-written ones back. Compare with the driver's current budget instead.
        // BB_GC_BUDGET_MB=N: this rule with a fixed budget on any GPU (tests on a desktop).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                // bbport: margin below the budget. Past it WDDM moves allocations to system
                // memory; on a 6 GB card with 16 GB of RAM (The Order: 1886) that froze
                // Windows at 4.98 of 4.95 GB.
                trigger_gc_memory = budget / 100 * 60;
                pressure_gc_memory = budget / 100 * 75;
                critical_gc_memory = budget / 100 * 85;
            }
        }
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    const bool pressured = total_used_memory >= pressure_gc_memory;
    const bool critical = total_used_memory >= critical_gc_memory;
    // Greater pressure must allow younger (but never current-submission) candidates.
    const u64 age = critical ? 4 : pressured ? 16 : 160;
    if (gc_tick < age) {
        return;
    }
    u64 skipped_tiled = 0;
    const auto clean_up = [&](ImageId image_id) {
        auto& image = slot_images[image_id];
        if (image.binding.is_bound || image.scene_proxy || image.usage.vo_surface ||
            image.tick_accessed_last == scheduler.CurrentTick()) {
            return false;
        }
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            ++skipped_tiled;
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        if (download) {
            // bbport: synchronously, while the image still protects its pages. A deferred
            // write-back landed after FreeImage had unprotected them, over whatever the game
            // had meanwhile stored there (e.g. its heap after unloading an area).
            DownloadImageMemory(image_id, true);
            ++gc_downloads;
        }
        ++gc_evictions;
        FreeImage(image_id);
        return true;
    };

    const size_t removed = Common::CollectLru(lru_cache, gc_tick - age,
                                              critical ? 64 : pressured ? 32 : 10, clean_up);
    const auto now = std::chrono::steady_clock::now();
    if (critical && removed && now - gc_drain_time >= std::chrono::milliseconds(100)) {
        // Unregistering an image does not free its allocation until its GPU work retires.
        // Only stall at the critical watermark, not on ordinary cache maintenance.
        scheduler.Finish();
        scheduler.PopPendingOperations();
        gc_drain_time = now;
    }
    // bbport: evictions under memory pressure, at most every 5 s (BB_FRAME_STATS or not).
    if (pressured || gc_downloads != 0) {
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            if (instance.CanReportMemoryUsage()) {
                total_used_memory = instance.GetDeviceMemoryUsage();
            }
            std::printf("Texture cache: heap usage %llu MiB, pressure %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back; registered guest images "
                        "%llu MiB (%llu images), sparse buffers %llu MiB, "
                        "tiled writebacks skipped this sweep %llu\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads,
                        (unsigned long long)(g_image_bytes.load() >> 20),
                        (unsigned long long)g_image_count.load(),
                        (unsigned long long)(g_bb_resident_buffer_bytes.load() >> 20),
                        (unsigned long long)skipped_tiled);
            gc_report_time = now;
            gc_evictions = gc_downloads = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

} // namespace VideoCore
