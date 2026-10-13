// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

bool CompleteSrtReadFault(void* ctx, bool pointer_load, u64 instruction_length) {
#if defined(_WIN32) && defined(ARCH_X86_64)
    auto* regs = static_cast<EXCEPTION_POINTERS*>(ctx)->ContextRecord;
    if (pointer_load) regs->Rdi = 0;
    else regs->R10 = 0;
    regs->Rip += instruction_length;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    auto& regs = static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss;
    if (pointer_load) regs.__rdi = 0;
    else regs.__r10 = 0;
    regs.__rip += instruction_length;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    auto& regs = static_cast<ucontext_t*>(ctx)->uc_mcontext;
    if (pointer_load) regs.mc_rdi = 0;
    else regs.mc_r10 = 0;
    regs.mc_rip += instruction_length;
#elif defined(ARCH_X86_64)
    auto& regs = static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs;
    regs[pointer_load ? REG_RDI : REG_R10] = 0;
    regs[REG_RIP] += instruction_length;
#else
    return false;
#endif
    return true;
}

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#else
#error "Unsupported architecture"
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
