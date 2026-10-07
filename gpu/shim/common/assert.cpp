// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"

// bbport: which call failed (e.g. a generic vk::Result Check), as module+offset frames;
// symbolize with llvm-symbolizer --obj=bb-probe.exe 0x140000000+offset.
static void print_backtrace() {
#ifdef _WIN32
    void* frames[32];
    const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module) &&
            module) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base = std::strrchr(name, '\\');
        std::fprintf(stderr, "  @%u %s+0x%llx\n", i, base ? base + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
#endif
}

void assert_fail_impl() {
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    print_backtrace();
    std::fflush(stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
