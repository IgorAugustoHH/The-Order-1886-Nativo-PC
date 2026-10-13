// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtMemoryCheck = u64 PS4_SYSV_ABI (*)(u64 address);
u64 PS4_SYSV_ABI SrtMemoryMapped(u64 address);
using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/,
                                         PFN_SrtMemoryCheck /*current_process_callback*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);
bool IsLegacySrtWalkerSafe(const u8* ptr, size_t size);
inline constexpr u32 SrtReaderRevision = 0x53525432; // SRT2, occupies old struct padding

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs
    u32 reader_revision = SrtReaderRevision;

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

#if defined(ARCH_X86_64)
static_assert(sizeof(PersistentSrtInfo) == 24);
#endif

} // namespace Shader
