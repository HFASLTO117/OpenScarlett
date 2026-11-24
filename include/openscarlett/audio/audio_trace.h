#pragma once

// Lightweight OPENSCARLETT_TRACE_RETAIL=1 & UNIMPL call-site RVA tracing for audio shims.

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <intrin.h>
#  include <windows.h>
#endif

namespace openscarlett::audio {

inline bool IsRetailTraceEnabled() noexcept {
    static const bool enabled = []() noexcept -> bool {
#if defined(_WIN32)
        char v[16]{};
        const DWORD na = ::GetEnvironmentVariableA(
            "OPENSCARLETT_TRACE_AUDIO", v, static_cast<DWORD>(sizeof(v)));
        if (na > 0u && v[0] != '0') {
            return true;
        }
        const DWORD nr = ::GetEnvironmentVariableA(
            "OPENSCARLETT_TRACE_RETAIL", v, static_cast<DWORD>(sizeof(v)));
        return nr > 0u && v[0] != '0';
#else
        return false;
#endif
    }();
    return enabled;
}

struct CallerRvaInfo {
    uint64_t rva = 0u;
    const char* module_name = "unknown";
    char module_buf[64]{};
};

inline CallerRvaInfo ResolveCallerRva(void* ret_addr) noexcept {
    CallerRvaInfo info{};
    if (!ret_addr) {
        return info;
    }
#if defined(_WIN32)
    HMODULE mod = nullptr;
    if (::GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(ret_addr),
            &mod) &&
        mod) {
        info.rva = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(ret_addr) - reinterpret_cast<uintptr_t>(mod));
        char path[MAX_PATH]{};
        const DWORD len = ::GetModuleFileNameA(mod, path, MAX_PATH);
        if (len > 0u && len < MAX_PATH) {
            const char* base = path;
            for (DWORD i = 0u; i < len; ++i) {
                if (path[i] == '\\' || path[i] == '/') {
                    base = path + i + 1u;
                }
            }
            std::snprintf(info.module_buf, sizeof(info.module_buf), "%s", base);
            info.module_name = info.module_buf;
        }
    } else {
        info.rva = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ret_addr));
    }
#else
    info.rva = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ret_addr));
#endif
    return info;
}

inline void TraceRetailAudio(
    const char* tag,
    void* ret_addr,
    const char* fmt,
    ...) noexcept {
    if (!IsRetailTraceEnabled()) {
        return;
    }
    const CallerRvaInfo caller = ResolveCallerRva(ret_addr);
    char msg[512]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[OpenScarlett][%s][rva=%s+0x%llX] %s\n",
                 tag, caller.module_name,
                 static_cast<unsigned long long>(caller.rva), msg);
    std::fflush(stderr);
}

inline void LogUnimplAudio(
    const char* tag,
    void* ret_addr,
    const char* fmt,
    ...) noexcept {
    const CallerRvaInfo caller = ResolveCallerRva(ret_addr);
    char msg[512]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[OpenScarlett][%s][UNIMPL][rva=%s+0x%llX] %s\n",
                 tag, caller.module_name,
                 static_cast<unsigned long long>(caller.rva), msg);
    std::fflush(stderr);
}

#if defined(_MSC_VER)
#  define OPENSCARLETT_CALLER_ADDR() _ReturnAddress()
#elif defined(__GNUC__) || defined(__clang__)
#  define OPENSCARLETT_CALLER_ADDR() __builtin_return_address(0)
#else
#  define OPENSCARLETT_CALLER_ADDR() nullptr
#endif

}  // namespace openscarlett::audio
