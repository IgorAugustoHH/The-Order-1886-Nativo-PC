// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bitset>
#include <string>
#include <type_traits>
#include "bb_resource_audit.h"
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

// bbport: runtime_memory.c: bytes of [address, address+size) mapped in guest memory.
extern "C" uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);

namespace Shader {

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization&) const = default;
};

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    bool operator==(const BufferSpecialization& other) const {
        return stride == other.stride && is_formatted == other.is_formatted &&
               swizzle_enable == other.swizzle_enable &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion)) &&
               (!swizzle_enable ||
                (index_stride == other.index_stride && element_size == other.element_size));
    }
};

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_cube = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // FIXME any pipeline cache changes needed?
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization&) const = default;
};

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization&) const = default;
};

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

/**
 * Alongside runtime information, this structure also checks bound resources
 * for compatibility. Can be used as a key for storing shader permutations.
 * Is separate from runtime information, because resource layout can only be deduced
 * after the first compilation of a module.
 */
struct StageSpecialization {
    static constexpr size_t MaxStageResources = 128;

    const Info* info{};
    RuntimeInfo runtime_info{};
    std::bitset<MaxStageResources> bitset{};
    std::optional<Gcn::FetchShaderData> fetch_shader_data{};
    boost::container::small_vector<VsAttribSpecialization, 32> vs_attribs;
    boost::container::small_vector<BufferSpecialization, 16> buffers;
    boost::container::small_vector<ImageSpecialization, 16> images;
    boost::container::small_vector<FMaskSpecialization, 8> fmasks;
    boost::container::small_vector<SamplerSpecialization, 16> samplers;
    Backend::Bindings start{};

    StageSpecialization() = default;
    StageSpecialization(const Info& info_, RuntimeInfo runtime_info_, const Profile& profile_,
                        Backend::Bindings start_)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        if (BbResourceAudit::Enabled()) {
            const auto signature = fmt::format("schema:{:x}:{}", info->pgm_hash, int(info->sw_stage));
            if (!BbResourceAudit::AlreadySeen(signature)) {
                BbResourceAudit::Write("resource_schema", fmt::format(
                    "\"shader\":\"{:x}\",\"stage\":{},\"buffers\":{},\"images\":{},"
                    "\"samplers\":{},\"fmasks\":{},\"flat_words\":{},"
                    "\"walker_start\":{},\"walker_bytes\":{},\"contains_ir_graph\":false",
                    info->pgm_hash, BbResourceAudit::Quote(magic_enum::enum_name(info->sw_stage)),
                    info->buffers.size(), info->images.size(), info->samplers.size(),
                    info->fmasks.size(), info->FlatUserData().size(),
                    reinterpret_cast<uintptr_t>(info->srt_info.walker_func),
                    info->srt_info.walker_func_size), signature);
            }
        }
        fetch_shader_data = Gcn::ParseFetchShader(info_);
        if (info_.sw_stage == SwStage::Vertex && fetch_shader_data) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data->attributes,
                         [this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
                             if (const auto step_rate = desc.GetStepRate();
                                 step_rate != InstanceIdType::None) {
                                 spec.divisor = step_rate == InstanceIdType::OverStepRate0
                                                    ? runtime_info.sw.vs.step_rate_0
                                                    : (step_rate == InstanceIdType::OverStepRate1
                                                           ? runtime_info.sw.vs.step_rate_1
                                                           : 1);
                             }
                             spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                         });
        }
        u32 binding{};
        ForEachSharp(binding, buffers, info->buffers,
                     [](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                         spec.stride = sharp.GetStride();
                         spec.is_formatted = desc.is_formatted;
                         spec.swizzle_enable = sharp.swizzle_enable;
                         if (spec.is_formatted) {
                             spec.data_format = static_cast<u32>(sharp.GetDataFmt());
                             spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                             spec.num_conversion = sharp.GetNumberConversion();
                         }
                         if (spec.swizzle_enable) {
                             spec.index_stride = sharp.index_stride;
                             spec.element_size = sharp.element_size;
                         }
                     });
        ForEachSharp(binding, images, info->images,
                     [&](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.type = sharp.GetViewType(desc.is_array);
                         spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
                         spec.is_storage = desc.is_written;
                         spec.is_cube = sharp.IsCube();
                         if (spec.is_storage) {
                             spec.dst_select = sharp.DstSelect();
                         } else {
                             spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
                         }
                         spec.num_conversion = sharp.GetNumberConversion();
                         spec.num_bindings = desc.NumBindings(*info);
                     });
        ForEachSharp(binding, fmasks, info->fmasks,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.width = sharp.width;
                         spec.height = sharp.height;
                     });
        ForEachSharp(samplers, info->samplers,
                     [](auto& spec, const auto& desc, AmdGpu::Sampler sharp) {
                         spec.force_unnormalized = sharp.force_unnormalized;
                         spec.force_degamma = sharp.force_degamma;
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->sw_stage == SwStage::TessellationControl ||
            info->sw_stage == SwStage::TessellationEval) {
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            runtime_info.InitFromTessConstants(tess_constants);
        }
    }

    /// bbport: garbage sharps (shader resource tables the recompiler could not resolve, The Order:
    /// 1886) carry random strides, view types, swizzles and FMASK sizes: specializing on them made
    /// a new permutation, and a shader compile, almost every draw (2523 of one program). A sharp at
    /// unmapped memory, or an image with an unused tile mode, is bound as null by the rasterizer
    /// anyway: specialize it as null too.
    static bool Plausible(const AmdGpu::Buffer& sharp) {
        return runtime_memory_clamp(sharp.base_address, 1) == 1;
    }
    static bool Plausible(const AmdGpu::Image& sharp) {
        const u32 tile = static_cast<u32>(sharp.GetTileMode());
        return (tile <= static_cast<u32>(AmdGpu::TileMode::Thick3DXThick) ||
                sharp.GetTileMode() == AmdGpu::TileMode::DisplayLinearGeneral) &&
               runtime_memory_clamp(sharp.Address(), 1) == 1;
    }
    static bool Plausible(const AmdGpu::Sampler&) {
        return true;
    }

    // First sample of each accepted/rejected resource origin. Includes cached shaders;
    // only fresh translations have an IR graph. Reads the already flattened host buffer.
    template <typename Desc, typename Sharp>
    void AuditSharp(u32 slot, const Desc& desc, const Sharp& sharp, bool accepted) const {
        if (!BbResourceAudit::Enabled()) return;
        if constexpr (requires { desc.sharp_fetch; }) {
            const char* kind = "sampler";
            if constexpr (std::is_same_v<Sharp, AmdGpu::Buffer>) kind = "buffer";
            if constexpr (std::is_same_v<Sharp, AmdGpu::Image>) kind = "image";
            const char* reason = accepted ? "accepted" : "null_after_GetSharp";
            if (!accepted && bool(sharp)) {
                reason = "unmapped_address";
                if constexpr (std::is_same_v<Sharp, AmdGpu::Image>) {
                    const auto tile = sharp.GetTileMode();
                    if (u32(tile) > u32(AmdGpu::TileMode::Thick3DXThick) &&
                        tile != AmdGpu::TileMode::DisplayLinearGeneral)
                        reason = "unsupported_tile_mode";
                }
            }
            const auto& fetch = desc.sharp_fetch;
            std::string origins, immediates;
            for (size_t i = 0; i < fetch.N; ++i) {
                if (i) { origins += ','; immediates += ','; }
                origins += std::to_string(fetch.offsets[i]);
                immediates += std::to_string(fetch.immediates[i]);
            }
            const auto signature = fmt::format("sharp:{:x}:{}:{}:{}:{}:{}:{}",
                info->pgm_hash, int(info->sw_stage), kind, slot, reason, int(fetch.load_mask), origins);
            if (BbResourceAudit::AlreadySeen(signature)) return;
            size_t words = fetch.N;
            if constexpr (requires { desc.is_r128; }) { if (desc.is_r128) words = 4; }
            std::array<u32, sizeof(Sharp) / sizeof(u32)> raw{}, final{};
            std::memcpy(final.data(), &sharp, sizeof(sharp));
            bool can_fetch = true;
            for (size_t i = 0; i < words; ++i) {
                const bool loaded = fetch.load_mask & (1u << i);
                if (fetch.offsets[i] == UNKNOWN_LOCATION ||
                    (loaded && fetch.offsets[i] >= info->FlatUserData().size())) {
                    can_fetch = false; break;
                }
                raw[i] = loaded ? info->FlatUserData()[fetch.offsets[i]] : fetch.immediates[i];
            }
            std::string raw_words, final_words;
            for (size_t i = 0; i < fetch.N; ++i) {
                if (i) { raw_words += ','; final_words += ','; }
                raw_words += std::to_string(raw[i]); final_words += std::to_string(final[i]);
            }
            std::string extra;
            if constexpr (requires { desc.buffer_type; })
                extra += fmt::format(",\"buffer_type\":{}", BbResourceAudit::Quote(magic_enum::enum_name(desc.buffer_type)));
            if constexpr (requires { desc.is_depth; })
                extra += fmt::format(",\"is_depth\":{}", desc.is_depth);
            if constexpr (std::is_same_v<Sharp, AmdGpu::Image>)
                extra += fmt::format(",\"tile_mode\":{},\"image_type\":{},\"format\":{}",
                    u32(sharp.GetTileMode()), u32(sharp.GetType()), u32(sharp.GetDataFmt()));
            BbResourceAudit::Write("resource_sample", fmt::format(
                "\"shader\":\"{:x}\",\"stage\":{},\"kind\":{},\"stage_slot\":{},"
                "\"accepted\":{},\"reason\":{},\"first_sample\":true,"
                "\"load_mask\":{},\"post_op\":{},\"offsets\":[{}],\"immediates\":[{}],"
                "\"raw_fetch_valid\":{},\"raw_word_count\":{},\"raw_words\":[{}],\"final_words\":[{}]{}",
                info->pgm_hash, BbResourceAudit::Quote(magic_enum::enum_name(info->sw_stage)),
                BbResourceAudit::Quote(kind), slot, accepted, BbResourceAudit::Quote(reason),
                int(fetch.load_mask), BbResourceAudit::Quote(magic_enum::enum_name(desc.post_op)),
                origins, immediates, can_fetch, words, raw_words, final_words, extra), signature);
        } else if constexpr (requires { desc.sharp_idx; }) {
            const auto signature = fmt::format("fmask:{:x}:{}:{}:{}", info->pgm_hash,
                int(info->sw_stage), slot, accepted);
            if (BbResourceAudit::AlreadySeen(signature)) return;
            std::array<u32, sizeof(Sharp) / sizeof(u32)> words{};
            std::memcpy(words.data(), &sharp, sizeof(sharp));
            std::string final_words;
            for (const auto word : words) {
                if (!final_words.empty()) final_words += ',';
                final_words += std::to_string(word);
            }
            BbResourceAudit::Write("resource_sample", fmt::format(
                "\"shader\":\"{:x}\",\"stage\":{},\"kind\":\"fmask\",\"stage_slot\":{},"
                "\"accepted\":{},\"reason\":{},\"first_sample\":true,\"flat_offset\":{},"
                "\"final_words\":[{}]", info->pgm_hash,
                BbResourceAudit::Quote(magic_enum::enum_name(info->sw_stage)), slot, accepted,
                BbResourceAudit::Quote(accepted ? "accepted" : "null_or_implausible"),
                desc.sharp_idx, final_words), signature);
        } else if constexpr (requires { desc.sgpr_base; desc.semantic; }) {
            const auto signature = fmt::format("vertex:{:x}:{}:{}", info->pgm_hash, slot, accepted);
            if (BbResourceAudit::AlreadySeen(signature)) return;
            std::array<u32, sizeof(Sharp) / sizeof(u32)> words{};
            std::memcpy(words.data(), &sharp, sizeof(sharp));
            std::string final_words;
            for (const auto word : words) {
                if (!final_words.empty()) final_words += ',';
                final_words += std::to_string(word);
            }
            BbResourceAudit::Write("resource_sample", fmt::format(
                "\"shader\":\"{:x}\",\"stage\":{},\"kind\":\"vertex_buffer\",\"stage_slot\":{},"
                "\"accepted\":{},\"reason\":{},\"first_sample\":true,\"sgpr_base\":{},"
                "\"dword_offset\":{},\"semantic\":{},\"dest_vgpr\":{},\"final_words\":[{}]",
                info->pgm_hash, BbResourceAudit::Quote(magic_enum::enum_name(info->sw_stage)),
                slot, accepted, BbResourceAudit::Quote(accepted ? "accepted" : "null_or_implausible"),
                desc.sgpr_base, desc.dword_offset, desc.semantic, desc.dest_vgpr, final_words), signature);
        }
    }

    void ForEachSharp(auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            const bool accepted = bool(sharp) && Plausible(sharp);
            AuditSharp(u32(spec_list.size() - 1), desc, sharp, accepted);
            if (!accepted) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            const bool accepted = bool(sharp) && Plausible(sharp);
            AuditSharp(binding, desc, sharp, accepted);
            if (!accepted) {
                binding++;
                continue;
            }
            bitset.set(binding++);
            func(spec, desc, sharp);
        }
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    // Runtime matching is directional: an unbound resource can accept several compiled
    // permutations. Disk-cache identity must be exact and tied to its stored index.
    bool SameCacheKey(const StageSpecialization& other) const {
        return Valid() && other.Valid() && runtime_info == other.runtime_info &&
               bitset == other.bitset && start == other.start &&
               fetch_shader_data == other.fetch_shader_data && vs_attribs == other.vs_attribs &&
               buffers == other.buffers && images == other.images && fmasks == other.fmasks &&
               samplers == other.samplers;
    }

    bool operator==(const StageSpecialization& other) const {
        if (!Valid() || !other.Valid() || buffers.size() != other.buffers.size() ||
            images.size() != other.images.size() || samplers.size() != other.samplers.size()) {
            return false;
        }

        if (vs_attribs != other.vs_attribs) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (fetch_shader_data != other.fetch_shader_data) {
            return false;
        }

        if (fmasks != other.fmasks) {
            return false;
        }

        // For VS which only generates geometry and doesn't have any inputs, its start
        // bindings still may change as they depend on previously processed FS. The check below
        // handles this case and prevents generation of redundant permutations. This is also safe
        // for other types of shaders with no bindings.
        if (bitset.none() && other.bitset.none()) {
            return true;
        }

        if (start != other.start) {
            return false;
        }

        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return false;
            }
        }

        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    /// bbport (BB_PERM_TRACE=1): which parts differ from another specialization of the same
    /// program, to find what keeps creating permutations.
    std::string DescribeDifference(const StageSpecialization& other) const {
        std::string out;
        const auto add = [&](const std::string& s) { out += out.empty() ? s : ", " + s; };
        if (runtime_info != other.runtime_info) {
            const auto &a = runtime_info, &b = other.runtime_info;
            std::string d = "runtime_info[stage " + std::to_string(int(a.hw_stage));
            if (!(a.props == b.props)) {
                d += " props(user_data " + std::to_string(a.props.num_user_data) + "/" +
                     std::to_string(b.props.num_user_data) + " vgprs " +
                     std::to_string(a.props.num_input_vgprs) + "/" +
                     std::to_string(b.props.num_input_vgprs) + " alloc " +
                     std::to_string(a.props.num_allocated_vgprs) + "/" +
                     std::to_string(b.props.num_allocated_vgprs) + ")";
            }
            if (!a.HasSameSwInfo(b.sw)) d += " sw";
            if (!a.HasSameHwInfo(b.hw)) {
                d += " hw";
                if (a.hw_stage == HwStage::Fragment) {
                    const auto &x = a.hw.fs, &y = b.hw.fs;
                    if (x.color_buffers != y.color_buffers) d += ":color_buffers";
                    if (x.num_inputs != y.num_inputs) d += ":num_inputs";
                    if (x.num_samples != y.num_samples) d += ":samples";
                    if (x.mrtz_mask != y.mrtz_mask) d += ":mrtz";
                    if (x.motion_vectors != y.motion_vectors) d += ":motion";
                    if (x.dual_source_blending != y.dual_source_blending) d += ":dual_src";
                } else if (a.hw_stage == HwStage::Compute) {
                    const auto &x = a.hw.cs, &y = b.hw.cs;
                    d += ":wg " + std::to_string(x.workgroup_size[0]) + "x" +
                         std::to_string(x.workgroup_size[1]) + " vs " +
                         std::to_string(y.workgroup_size[0]) + "x" +
                         std::to_string(y.workgroup_size[1]);
                }
            }
            add(d + "]");
        }
        if (fetch_shader_data != other.fetch_shader_data) add("fetch_shader");
        if (vs_attribs != other.vs_attribs) add("vs_attribs");
        if (start != other.start) add("start_bindings");
        if (fmasks.size() != other.fmasks.size()) add("fmask_count");
        for (size_t i = 0; i < std::min(fmasks.size(), other.fmasks.size()); ++i) {
            if (fmasks[i] != other.fmasks[i]) {
                add("fmask" + std::to_string(i) + "(" + std::to_string(fmasks[i].width) + "x" +
                    std::to_string(fmasks[i].height) + ")");
            }
        }
        for (size_t i = 0; i < std::min(buffers.size(), other.buffers.size()); ++i) {
            if (buffers[i] != other.buffers[i]) {
                add("buf" + std::to_string(i) + "(stride " + std::to_string(buffers[i].stride) +
                    " vs " + std::to_string(other.buffers[i].stride) + ")");
            }
        }
        for (size_t i = 0; i < std::min(images.size(), other.images.size()); ++i) {
            const auto &a = images[i], &b = other.images[i];
            if (a != b) {
                add("img" + std::to_string(i) + "(" + (a.type != b.type ? "type " : "") +
                    (a.is_integer != b.is_integer ? "int " : "") +
                    (a.is_cube != b.is_cube ? "cube " : "") + (a.is_srgb != b.is_srgb ? "srgb " : "") +
                    (a.dst_select != b.dst_select ? "swizzle " : "") +
                    (a.num_conversion != b.num_conversion ? "conv " : "") +
                    (a.num_bindings != b.num_bindings ? "bindings" : "") + ")");
            }
        }
        for (size_t i = 0; i < std::min(samplers.size(), other.samplers.size()); ++i) {
            if (samplers[i] != other.samplers[i]) add("smp" + std::to_string(i));
        }
        if (bitset != other.bitset) add("null-mask");
        return out.empty() ? "no visible difference" : out;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
