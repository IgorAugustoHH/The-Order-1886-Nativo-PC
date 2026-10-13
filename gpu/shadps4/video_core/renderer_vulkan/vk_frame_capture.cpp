// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/dof_capture.h"
#include "video_core/texture_cache/image_info.h"

namespace Vulkan {

namespace {

struct Target {
    VAddr address{};
    vk::Format format{};
    u32 width{}, height{};
    /// Image depth (3D) or layers, and the layers the binding's view covers (sampled only).
    u32 slices{1}, view_base{}, view_count{};
    u32 samples{1};
    bool operator==(const Target&) const = default;
};

struct Entry {
    struct DrawDetail {
        u64 vs{}, ps{};
        u32 indices{}, instances{};
        std::vector<std::string> bindings;
        std::string state;
    };
    bool compute{};
    std::vector<Target> colors;
    Target depth;
    u32 draws{};
    u64 indices{};
    std::vector<u64> shaders; ///< vs/ps pairs (graphics) or cs (compute), first few
    std::vector<std::pair<Target, bool>> sampled; ///< textures (false) and storage images (true)
    std::string note;
    std::vector<std::string> buffers; ///< dumped constants, per draw
    std::vector<DrawDetail> details;
    std::vector<std::string> notes;
};

// GPU thread only.
std::vector<Entry> entries;
std::vector<std::pair<Target, bool>> pending_sampled;
std::vector<std::string> pending_buffers;
struct DofInput {
    u64 hash{}, bytes{};
    std::string text;
};
std::vector<DofInput> pending_dof_inputs;
std::vector<std::string> pending_bindings;
std::string pending_state;
constexpr u32 MaxDrawDetails = 2048;
constexpr size_t MaxDetailBytes = 8 * 1024 * 1024;
u32 detail_draws{};
size_t detail_bytes{};
bool details_truncated{};
u32 dof_buffers{};
u64 dof_bytes{};
bool dof_truncated{};
u32 dof_retained{}, dof_unassociated{};
u64 dof_retained_bytes{};
std::string focus_depth_note;
bool focus_depth_taken{};
std::string focus_scalar_note;
bool focus_scalar_taken{};

void ResetDetails() {
    pending_bindings.clear();
    pending_state.clear();
    detail_draws = 0;
    detail_bytes = 0;
    details_truncated = false;
    dof_buffers = 0;
    dof_bytes = 0;
    dof_truncated = false;
    pending_dof_inputs.clear();
    dof_retained = dof_unassociated = 0;
    dof_retained_bytes = 0;
    focus_depth_note.clear();
    focus_depth_taken = false;
    focus_scalar_note.clear();
    focus_scalar_taken = false;
}

void AddDofInputs(Entry& entry, u64 ps) {
    for (auto& input : pending_dof_inputs) {
        if (!ps || input.hash != ps) {
            ++dof_unassociated;
            continue;
        }
        // Preserve bounded DoF inputs even when this target's pass coalesces >4 draws.
        entry.buffers.push_back(std::format("  dof input draw {} {}", entry.draws - 1, input.text));
        ++dof_retained;
        dof_retained_bytes += input.bytes;
    }
    pending_dof_inputs.clear();
}

void AddBuffers(Entry& entry) {
    // Constants of full-screen passes (post-processing: camera matrices) and of the first draw
    // of each geometry pass.
    if (entry.draws <= 4) {
        for (auto& b : pending_buffers) {
            entry.buffers.push_back(std::move(b));
        }
    }
    pending_buffers.clear();
}
bool pass_open = false;
std::mutex display_mutex;
std::vector<VAddr> display_buffers;
// bbport: read on every draw; the game registers a handful of display buffers once.
std::array<std::atomic<VAddr>, 16> display_list{};
std::atomic<u32> display_count{0};

Target ToTarget(const VideoCore::ImageInfo& info) {
    return {info.guest_address, info.pixel_format, info.size.width, info.size.height,
            std::max(info.resources.layers, info.size.depth), 0, 0, info.num_samples};
}

std::string Describe(const Target& t) {
    if (!t.address) {
        return "-";
    }
    std::string text =
        std::format("{:#x} {} {}x{}", t.address, vk::to_string(t.format), t.width, t.height);
    if (t.slices > 1) {
        text += std::format(" ({} slices)", t.slices);
    }
    if (t.samples > 1) {
        text += std::format(" {}x MSAA", t.samples);
    }
    if (t.view_count) {
        text += std::format(" view layers {}+{}", t.view_base, t.view_count);
    }
    return text;
}

void AddSampled(Entry& entry) {
    for (const auto& s : pending_sampled) {
        if (entry.sampled.size() < 32 &&
            std::ranges::find(entry.sampled, s) == entry.sampled.end()) {
            entry.sampled.push_back(s);
        }
    }
    pending_sampled.clear();
}

void AddShader(Entry& entry, u64 hash) {
    if (entry.shaders.size() < 8 && std::ranges::find(entry.shaders, hash) == entry.shaders.end()) {
        entry.shaders.push_back(hash);
    }
}

void Write(VAddr presented) {
    const char* dir = std::getenv("BB_CAPTURE_DIR");
    const std::string path =
        std::format("{}/frame_{}.txt", dir ? dir : ".", static_cast<long long>(std::time(nullptr)));
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("Frame capture: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "presented buffer %#llx\n", static_cast<unsigned long long>(presented));
    if (!focus_depth_note.empty()) {
        std::fprintf(f, "%s\n", focus_depth_note.c_str());
    }
    if (!focus_scalar_note.empty()) {
        std::fprintf(f, "%s\n", focus_scalar_note.c_str());
    }
    if (FrameCapture::DofConstantsEnabled()) {
        std::fprintf(f, "dof input coverage: schema 1 buffers %u captured_bytes %llu truncated %u unassociated %u "
                        "limits %u buffers/%llu bytes/%llu bytes_per_buffer\n",
                     dof_retained, static_cast<unsigned long long>(dof_retained_bytes),
                     unsigned(dof_truncated || dof_unassociated != 0), dof_unassociated,
                     DofCapture::MaxBuffers, static_cast<unsigned long long>(DofCapture::MaxFrameBytes),
                     static_cast<unsigned long long>(DofCapture::MaxBufferBytes));
    }
    if (FrameCapture::DrawDetailsEnabled()) {
        std::fprintf(f, "draw detail coverage: recorded %u, truncated %u, payload bytes %zu, "
                        "limits %u draws/%zu bytes\n",
                     detail_draws, unsigned(details_truncated), detail_bytes,
                     MaxDrawDetails, MaxDetailBytes);
    }
    u32 index = 0;
    for (const auto& e : entries) {
        if (e.compute) {
            std::fprintf(f, "\n#%u COMPUTE dispatches %u, cs", index++, e.draws);
        } else {
            std::fprintf(f, "\n#%u PASS draws %u (indices %llu), vs/ps", index++, e.draws,
                         static_cast<unsigned long long>(e.indices));
        }
        for (const u64 h : e.shaders) {
            std::fprintf(f, " %016llx", static_cast<unsigned long long>(h));
        }
        std::fprintf(f, "\n");
        for (size_t i = 0; i < e.colors.size(); ++i) {
            std::fprintf(f, "  color%zu %s\n", i, Describe(e.colors[i]).c_str());
        }
        if (!e.compute) {
            std::fprintf(f, "  depth  %s\n", Describe(e.depth).c_str());
        }
        for (const auto& [t, storage] : e.sampled) {
            std::fprintf(f, "  %s %s\n", storage ? "writes " : "samples", Describe(t).c_str());
        }
        if (!e.note.empty()) {
            std::fprintf(f, "  note: %s\n", e.note.c_str());
        }
        for (const auto& n : e.notes) {
            std::fprintf(f, "  note: %s\n", n.c_str());
        }
        u32 draw_index = 0;
        for (const auto& d : e.details) {
            std::fprintf(f, "  draw %u vs %016llx ps %016llx indices %u instances %u\n",
                         draw_index++, static_cast<unsigned long long>(d.vs),
                         static_cast<unsigned long long>(d.ps), d.indices, d.instances);
            if (!d.state.empty()) std::fprintf(f, "    state %s\n", d.state.c_str());
            for (const auto& b : d.bindings) std::fprintf(f, "    binding %s\n", b.c_str());
        }
        for (const auto& b : e.buffers) {
            std::fprintf(f, "%s\n", b.c_str());
        }
    }
    std::fclose(f);
    std::printf("Frame capture: %zu passes written to %s\n", entries.size(), path.c_str());
}

} // namespace

void FrameCapture::OnFlip(VAddr presented_address) {
    last_presented.store(presented_address, std::memory_order_relaxed);
    flips.fetch_add(1, std::memory_order_release);
    static const char* trigger = std::getenv("BB_CAPTURE_TRIGGER");
    if (trigger && state.load(std::memory_order_relaxed) == Idle &&
        std::filesystem::exists(trigger)) {
        std::error_code ec;
        std::filesystem::remove(trigger, ec);
        state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed\n");
    }
}

void FrameCapture::Poll() {}

bool FrameCapture::IsDisplayBuffer(VAddr address) {
    const u32 count = display_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        if (display_list[i].load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    if (count < display_list.size()) {
        return false;
    }
    std::scoped_lock lk{display_mutex};
    return std::ranges::find(display_buffers, address) != display_buffers.end();
}

void FrameCapture::AddDisplayBuffer(VAddr address) {
    std::scoped_lock lk{display_mutex};
    if (std::ranges::find(display_buffers, address) == display_buffers.end()) {
        display_buffers.push_back(address);
        const u32 count = display_count.load(std::memory_order_relaxed);
        if (count < display_list.size()) {
            display_list[count].store(address, std::memory_order_relaxed);
            display_count.store(count + 1, std::memory_order_release);
        }
    }
}

void FrameCapture::BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                             const VideoCore::ImageInfo* depth) {
    std::vector<Target> targets;
    bool display = false;
    {
        std::scoped_lock lk{display_mutex};
        for (u32 i = 0; i < num_colors; ++i) {
            targets.push_back(colors[i] ? ToTarget(*colors[i]) : Target{});
            display |= colors[i] && std::ranges::find(display_buffers, colors[i]->guest_address) !=
                                        display_buffers.end();
        }
    }
    // Frame boundaries in the command stream: the pass that writes a display buffer.
    const bool new_pass = !(pass_open && !entries.empty() && !entries.back().compute &&
                            entries.back().colors == targets);
    if (display && new_pass) {
        if (state.load(std::memory_order_relaxed) == Recording) {
            Write(last_presented.load(std::memory_order_relaxed));
            entries.clear();
            pending_sampled.clear();
            pending_buffers.clear();
            ResetDetails();
            state.store(Idle, std::memory_order_release);
            return;
        }
        entries.clear();
        pending_sampled.clear();
        pending_buffers.clear();
        ResetDetails();
        pass_open = false;
        state.store(Recording, std::memory_order_release);
    }
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        pending_bindings.clear();
        pending_state.clear();
        return;
    }
    const Target d = depth ? ToTarget(*depth) : Target{};
    if (pass_open && !entries.empty() && !entries.back().compute &&
        entries.back().colors == targets && entries.back().depth == d) {
        return;
    }
    entries.push_back({.compute = false, .colors = std::move(targets), .depth = d});
    pass_open = true;
}

void FrameCapture::Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances) {
    if (state.load(std::memory_order_relaxed) != Recording || entries.empty() ||
        entries.back().compute) {
        return;
    }
    auto& e = entries.back();
    ++e.draws;
    e.indices += u64(num_indices) * std::max(num_instances, 1u);
    AddShader(e, vs_hash);
    AddShader(e, ps_hash);
    if (DrawDetailsEnabled()) {
        if (detail_draws < MaxDrawDetails && detail_bytes < MaxDetailBytes) {
            e.details.push_back({vs_hash, ps_hash, num_indices, num_instances,
                                 std::move(pending_bindings), std::move(pending_state)});
            ++detail_draws;
        } else {
            details_truncated = true;
        }
    }
    pending_bindings.clear();
    pending_state.clear();
    AddSampled(e);
    AddDofInputs(e, ps_hash);
    AddBuffers(e);
}

void FrameCapture::Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        pending_bindings.clear();
        pending_state.clear();
        return;
    }
    if (!entries.empty() && entries.back().compute && !entries.back().shaders.empty() &&
        entries.back().shaders[0] == cs_hash) {
        ++entries.back().draws;
    } else {
        entries.push_back({.compute = true, .draws = 1, .shaders = {cs_hash}});
        entries.back().note = std::format("groups {}x{}x{}", x, y, z);
    }
    AddSampled(entries.back());
    AddDofInputs(entries.back(), 0); // A PS input cannot belong to a compute dispatch.
    AddBuffers(entries.back());
    // Compute bindings must not become the following graphics draw's resources.
    pending_bindings.clear();
    pending_state.clear();
    pass_open = false;
}

void FrameCapture::Indirect(u64 vs_hash, u64 ps_hash, VAddr args, VAddr count,
                            u32 stride, u32 max_count) {
    if (DrawDetailsEnabled()) {
        pending_state += std::format(" indirect_args {:#x} count_address {:#x} stride {} "
                                     "max_count {} counts_unknown_gpu_arguments",
                                     args, count, stride, max_count);
    }
    // Zero is a placeholder, not an observed GPU vertex or instance count.
    Draw(vs_hash, ps_hash, 0, 0);
}

void FrameCapture::Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size) {
    const u64 bytes = std::min<u64>(size, 1024) & ~u64(3);
    std::string out = std::format("  buffer stage {:016x} slot {} at {:#x} size {}:", stage_hash,
                                  slot, address, size);
    const auto* words = static_cast<const float*>(data);
    for (u64 i = 0; i < bytes / 4; ++i) {
        if (i % 8 == 0) {
            out += std::format("\n    [{:3}]", i);
        }
        out += std::format(" {:12.6g}", words[i]);
    }
    pending_buffers.push_back(std::move(out));
}

bool FrameCapture::DofConstantsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_CAPTURE_DOF_CONSTANTS");
        return value && value[0] == '1';
    }();
    return enabled;
}

bool FrameCapture::FocusDepthWanted(u64 hash) {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_CAPTURE_FOCUS_DEPTH");
        return value && value[0] == '1';
    }();
    return enabled && hash == 0xf77d3cf1 && !focus_depth_taken &&
           state.load(std::memory_order_relaxed) == Recording;
}

std::string FrameCapture::FocusDepthPath(const VideoCore::ImageInfo& info, u32 image_id) {
    if (!FocusDepthWanted(0xf77d3cf1) || info.pixel_format != vk::Format::eD32Sfloat ||
        !info.props.is_depth || info.num_samples <= 1 || info.resources.layers != 1 ||
        u64(info.size.width) * info.size.height * 4 > 8 * 1024 * 1024) return {};
    focus_depth_taken = true;
    const char* dir = std::getenv("BB_CAPTURE_DIR");
    std::string path = std::format("{}/focus_depth_{}_{}.bin", dir ? dir : ".",
                                  static_cast<long long>(std::time(nullptr)), flips.load());
    focus_depth_note = std::format(
        "focus depth snapshot: image_id {} address {:#x} {}x{} samples {} sample 0 file {}",
        image_id, info.guest_address, info.size.width, info.size.height, info.num_samples, path);
    return path;
}

std::string FrameCapture::FocusScalarPath(const VideoCore::ImageInfo& info, u32 image_id) {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_CAPTURE_FOCUS_DEPTH");
        return value && value[0] == '1';
    }();
    if (!enabled || focus_scalar_taken || state.load(std::memory_order_relaxed) != Recording ||
        info.pixel_format != vk::Format::eR32Sfloat || info.size.width != 1 ||
        info.size.height != 1 || info.size.depth != 1 || info.num_samples != 1 ||
        info.resources.levels != 1 || info.resources.layers != 1) return {};
    focus_scalar_taken = true;
    const char* dir = std::getenv("BB_CAPTURE_DIR");
    auto path = std::format("{}/focus_scalar_{}_{}.bin", dir ? dir : ".",
                           static_cast<long long>(std::time(nullptr)), flips.load());
    focus_scalar_note = std::format(
        "focus scalar snapshot: image_id {} address {:#x} file {}",
        image_id, info.guest_address, path);
    return path;
}

void FrameCapture::DofBuffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size,
                            DofSource source, u64 ring_offset) {
    if (!DofConstantsEnabled() || state.load(std::memory_order_relaxed) != Recording ||
        !DofCapture::Includes(stage_hash) || !data || size < sizeof(u32)) return;
    if (dof_buffers >= DofCapture::MaxBuffers || dof_bytes >= DofCapture::MaxFrameBytes) {
        dof_truncated = true;
        return;
    }
    const u64 bytes = std::min({size, DofCapture::MaxBufferBytes,
                               DofCapture::MaxFrameBytes - dof_bytes}) & ~u64(3);
    if (!bytes) return;
    const bool truncated = bytes != size;
    dof_truncated |= truncated;
    constexpr std::array names{"ring_snapshot", "flat_buffer_cpu", "guest_cpu", "flat_user_data"};
    std::string out = std::format(
        "stage {:016x} slot {} at {:#x} source {} ring_offset {:#x} "
        "size {} captured {} truncated {}:", stage_hash, slot, address, names[u32(source)],
        ring_offset, size, bytes, unsigned(truncated));
    // Copy each dword: data can be unaligned; decimal float dumps lose significant bits.
    const auto* raw = static_cast<const u8*>(data);
    for (u64 i = 0; i < bytes / sizeof(u32); ++i) {
        u32 word;
        std::memcpy(&word, raw + i * sizeof(u32), sizeof(word));
        if (i % 8 == 0) out += std::format("\n    dwords [{:4}]", i);
        out += std::format(" {:08x}", word);
    }
    pending_dof_inputs.push_back({stage_hash, bytes, std::move(out)});
    ++dof_buffers;
    dof_bytes += bytes;
}

void FrameCapture::Sampled(const VideoCore::ImageInfo& info, bool storage, u32 view_base,
                           u32 view_count) {
    if (pending_sampled.size() < 64) {
        Target target = ToTarget(info);
        target.view_base = view_base;
        target.view_count = view_count;
        pending_sampled.emplace_back(target, storage);
    }
}

void FrameCapture::Note(const char* text) {
    if (!entries.empty()) {
        // Repeated per-draw depth notes used to fill multi-MiB frame reports.
        auto& notes = entries.back().notes;
        if (notes.size() < 64 && std::ranges::find(notes, text) == notes.end()) {
            notes.emplace_back(text);
        }
    }
}

bool FrameCapture::DrawDetailsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_CAPTURE_DRAW_RESOURCES");
        return value && value[0] == '1';
    }();
    return enabled;
}

void FrameCapture::Binding(const std::string& text) {
    if (!DrawDetailsEnabled() || !Active()) return;
    if (pending_bindings.size() < 128 && detail_draws < MaxDrawDetails &&
        detail_bytes + text.size() < MaxDetailBytes) {
        pending_bindings.push_back(text);
        detail_bytes += text.size();
    } else {
        details_truncated = true;
    }
}

void FrameCapture::RecordDrawState(const std::string& text) {
    if (!DrawDetailsEnabled() || !Active()) return;
    if (detail_draws < MaxDrawDetails && detail_bytes + text.size() < MaxDetailBytes) {
        pending_state = text;
        detail_bytes += text.size();
    } else {
        details_truncated = true;
    }
}

} // namespace Vulkan
