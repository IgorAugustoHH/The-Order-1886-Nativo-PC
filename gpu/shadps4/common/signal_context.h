// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

void IncrementRip(void* ctx, u64 length);

// Complete a failed SRT MOV for this invocation, without modifying executable code.
// pointer_load selects RDI (64-bit); otherwise R10D (zero extends into R10).
bool CompleteSrtReadFault(void* ctx, bool pointer_load, u64 instruction_length);

bool IsWriteError(void* ctx);

} // namespace Common
