// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <iterator>
#include <vector>
#include <cstdlib>
#include <cstring>
#include "common/types.h"
#include "video_core/buffer_cache/range_set.h"

namespace VideoCore {

// Diagnostic A/B only: reproduce the pre-1cee927 merge and upload rules.
inline bool DeferredReadbackMergeFixed() {
    static const bool fixed = [] {
        const char* mode = std::getenv("BB_READBACK_MERGE");
        return !mode || std::strcmp(mode, "legacy") != 0;
    }();
    return fixed;
}

/// bbport: a deferred readback keeps the guest's bytes as they were at the fault (`before`).
/// Byte runs of `current` (the guest's bytes now) that equal `before` still hold stale data the
/// GPU data replaces; runs that differ were written by the CPU after the fault and are newer
/// than the GPU data. Calls func(address, size) for each run where `changed` is the case.
template <bool changed, typename Func>
void ForEachDeferredRun(VAddr addr, const u8* before, const u8* current, u64 size, Func&& func) {
    for (u64 i = 0; i < size;) {
        const bool differs = current[i] != before[i];
        u64 j = i + 1;
        while (j < size && (current[j] != before[j]) == differs) {
            ++j;
        }
        if (differs == changed) {
            func(addr + i, j - i);
        }
        i = j;
    }
}

/// Bytes of the range that the guest rewrote since the fault of the newest batch holding them
/// (batches oldest first; items with addr, size, snapshot_offset; batch.snapshot). An older
/// snapshot predates CPU writes that the GPU data of a newer batch replaces, so each byte is
/// only compared with the newest one. read(addr, dst, size) reads guest memory.
template <typename Batches, typename Read, typename Func>
void ForEachRewrittenPending(const Batches& batches, VAddr addr, u64 size, Read&& read,
                             Func&& func) {
    if (!DeferredReadbackMergeFixed()) return;
    RangeSet newer;
    std::vector<u8> current;
    for (auto batch = std::rbegin(batches); batch != std::rend(batches); ++batch) {
        for (const auto& item : batch->items) {
            const VAddr start = std::max(addr, item.addr);
            const VAddr end = std::min(addr + size, item.addr + item.size);
            if (start >= end) {
                continue;
            }
            newer.ForEachNotInRange(start, end - start, [&](VAddr part, u64 part_size) {
                current.resize(part_size);
                read(part, current.data(), part_size);
                const u8* before =
                    batch->snapshot.data() + item.snapshot_offset + (part - item.addr);
                ForEachDeferredRun<true>(part, before, current.data(), part_size, func);
            });
        }
        for (const auto& item : batch->items) {
            newer.Add(item.addr, item.size);
        }
    }
}

/// Writes the GPU data of `batch` (gpu(item) points at the item's bytes) to guest memory where
/// the guest bytes are still those of the fault, leaving bytes that a batch in `later` (newer,
/// still pending) also holds to it: this older data written first would look like a CPU write
/// to it. Returns, per item, how many bytes the guest changed (on_item(item, changed)).
template <typename Batch, typename Later, typename Gpu, typename Read, typename Write,
          typename OnItem>
void ApplyDeferredBatch(const Batch& batch, const Later& later, Gpu&& gpu, Read&& read,
                        Write&& write, OnItem&& on_item) {
    RangeSet newer;
    if (DeferredReadbackMergeFixed()) {
        for (const auto& next : later) {
            for (const auto& item : next.items) {
                newer.Add(item.addr, item.size);
            }
        }
    }
    std::vector<u8> current;
    for (const auto& item : batch.items) {
        u64 changed = 0;
        newer.ForEachNotInRange(item.addr, item.size, [&](VAddr part, u64 part_size) {
            const u64 offset = part - item.addr;
            const u8* data = gpu(item) + offset;
            const u8* before = batch.snapshot.data() + item.snapshot_offset + offset;
            current.resize(part_size);
            read(part, current.data(), part_size);
            ForEachDeferredRun<false>(part, before, current.data(), part_size,
                                      [&](VAddr run, u64 run_size) {
                                          write(run, data + (run - part), run_size);
                                      });
            ForEachDeferredRun<true>(part, before, current.data(), part_size,
                                     [&](VAddr, u64 run_size) { changed += run_size; });
        });
        on_item(item, changed);
    }
}

} // namespace VideoCore
