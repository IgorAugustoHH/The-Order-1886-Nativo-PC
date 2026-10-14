// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame analyzer. Creating the file named by BB_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in Bloodborne's frame.

#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    static void Indirect(u64 vs_hash, u64 ps_hash, VAddr args, VAddr count,
                         u32 stride, u32 max_count);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    static void Sampled(const VideoCore::ImageInfo& info, bool storage, u32 view_base = 0,
                        u32 view_count = 0);
    /// Opt-in records associate each draw with its effective T# and MSAA state.
    static bool DrawDetailsEnabled();
    static void Binding(const std::string& text);
    static void RecordDrawState(const std::string& text);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    /// Opt-in, bounded input diagnostics for named DoF PS programs, only during a requested frame.
    enum class DofSource { RingSnapshot, FlatBufferCPU, GuestCPU, FlatUserData };
    static bool DofConstantsEnabled();
    /// One requested-frame snapshot of the actual depth image bound by the focus PS.
    static bool FocusDepthWanted(u64 hash);
    static std::string FocusDepthPath(const VideoCore::ImageInfo& info, u32 image_id);
    static std::string FocusScalarPath(const VideoCore::ImageInfo& info, u32 image_id);
    static void DofBuffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size,
                          DofSource source, u64 ring_offset = 0);
    static void Note(const char* text);

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
};

} // namespace Vulkan
