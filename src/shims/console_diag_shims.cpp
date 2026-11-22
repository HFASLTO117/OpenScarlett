// Clean-room MIT Console Diagnostic & Legacy Compatibility Shims for OpenScarlett.
// Compiled into 4 standalone DLL targets via preprocessor target macros:
//   - OPENSCARLETT_DIAG_TARGET_ADVAPI32LEGACY    -> advapi32legacy.dll
//   - OPENSCARLETT_DIAG_TARGET_KERNELTRACECONTROL -> KernelTraceControl.dll
//   - OPENSCARLETT_DIAG_TARGET_HIX_XS            -> hix_xs.dll
//   - OPENSCARLETT_DIAG_TARGET_XBTP              -> xbtp.dll
//
// Every exported API logs into openscarlett::CallTelemetry and stays portable
// across MSVC (/W4), Clang-CL, and MinGW-w64.

#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
// Temporarily rename Win32 SDK prototypes that we re-export from advapi32legacy.dll
// and KernelTraceControl.dll so MSVC, Clang-CL, and MinGW-w64 never report
// dllimport/dllexport linkage conflicts.
#  define RegOpenKeyExW _os_win_RegOpenKeyExW
#  define RegOpenKeyExA _os_win_RegOpenKeyExA
#  define RegQueryValueExW _os_win_RegQueryValueExW
#  define RegQueryValueExA _os_win_RegQueryValueExA
#  define RegCloseKey _os_win_RegCloseKey
#  define GetUserNameW _os_win_GetUserNameW
#  define GetUserNameA _os_win_GetUserNameA
#  define CryptAcquireContextW _os_win_CryptAcquireContextW
#  define CryptGenRandom _os_win_CryptGenRandom
#  define CryptReleaseContext _os_win_CryptReleaseContext
#  define EventRegister _os_win_EventRegister
#  define EventWrite _os_win_EventWrite
#  define EventUnregister _os_win_EventUnregister
#  define StartKernelTrace _os_win_StartKernelTrace
#  define StopKernelTrace _os_win_StopKernelTrace
#  define EnableTraceEx2 _os_win_EnableTraceEx2
#  define TraceQueryInformation _os_win_TraceQueryInformation
#  define TraceSetInformation _os_win_TraceSetInformation
#  include <windows.h>
#  undef RegOpenKeyExW
#  undef RegOpenKeyExA
#  undef RegQueryValueExW
#  undef RegQueryValueExA
#  undef RegCloseKey
#  undef GetUserNameW
#  undef GetUserNameA
#  undef CryptAcquireContextW
#  undef CryptGenRandom
#  undef CryptReleaseContext
#  undef EventRegister
#  undef EventWrite
#  undef EventUnregister
#  undef StartKernelTrace
#  undef StopKernelTrace
#  undef EnableTraceEx2
#  undef TraceQueryInformation
#  undef TraceSetInformation
#endif

#if defined(_WIN32)
#  define OS_DIAG_API __declspec(dllexport)
#  define OS_DIAG_CALL __stdcall
#else
#  define OS_DIAG_API __attribute__((visibility("default")))
#  define OS_DIAG_CALL
#endif

namespace {

constexpr int32_t kHrOk = 0x00000000;
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);
constexpr int32_t kHrHandle = static_cast<int32_t>(0x80070006u);
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);

int32_t QueryModuleTelemetry(
    const char* module_name,
    const char* symbol_name,
    uint64_t* out_total,
    uint64_t* out_ok) {
    if (out_total) *out_total = 0u;
    if (out_ok) *out_ok = 0u;
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == module_name && entry.symbol_name == symbol_name) {
            if (out_total) *out_total = entry.total_calls;
            if (out_ok) *out_ok = entry.ok_calls;
            return 1;
        }
    }
    return 0;
}

#if defined(_WIN32)
HMODULE LoadSystemDll(const wchar_t* dll_name) {
    wchar_t sys_dir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0u && len < MAX_PATH - 32u) {
        std::wstring path(sys_dir, len);
        if (!path.empty() && path.back() != L'\\') {
            path.push_back(L'\\');
        }
        path += dll_name;
        HMODULE mod = LoadLibraryW(path.c_str());
        if (mod) return mod;
    }
    return LoadLibraryW(dll_name);
}
#endif

}  // namespace

// ===========================================================================
// TARGET 1: advapi32legacy.dll (UE4 startup / login / crypto / ETW parity)
// ===========================================================================
#if defined(OPENSCARLETT_DIAG_TARGET_ADVAPI32LEGACY)

namespace {

constexpr const char* kAdvapiMod = "advapi32legacy.dll";
constexpr uint32_t kErrSuccess = 0u;
constexpr uint32_t kErrInvalidParam = 87u;
constexpr uint32_t kErrInsufficientBuf = 122u;
constexpr uint32_t kErrCallNotImpl = 120u;

inline void RecordAdvapi(const char* sym, bool ok, int32_t code = 0) {
    openscarlett::CallTelemetry::Global().Record(
        kAdvapiMod,
        sym,
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        code,
        -1);
}

#if defined(_WIN32)
using PfnRegOpenKeyExW = int32_t(WINAPI*)(void*, const wchar_t*, uint32_t, uint32_t, void**);
using PfnRegOpenKeyExA = int32_t(WINAPI*)(void*, const char*, uint32_t, uint32_t, void**);
using PfnRegQueryValueExW = int32_t(WINAPI*)(void*, const wchar_t*, uint32_t*, uint32_t*, uint8_t*, uint32_t*);
using PfnRegQueryValueExA = int32_t(WINAPI*)(void*, const char*, uint32_t*, uint32_t*, uint8_t*, uint32_t*);
using PfnRegCloseKey = int32_t(WINAPI*)(void*);
using PfnGetUserNameW = int32_t(WINAPI*)(wchar_t*, uint32_t*);
using PfnGetUserNameA = int32_t(WINAPI*)(char*, uint32_t*);
using PfnCryptAcquireContextW = int32_t(WINAPI*)(uintptr_t*, const wchar_t*, const wchar_t*, uint32_t, uint32_t);
using PfnCryptGenRandom = int32_t(WINAPI*)(uintptr_t, uint32_t, uint8_t*);
using PfnCryptReleaseContext = int32_t(WINAPI*)(uintptr_t, uint32_t);
using PfnEventRegister = uint32_t(WINAPI*)(const void*, void*, void*, uint64_t*);
using PfnEventWrite = uint32_t(WINAPI*)(uint64_t, const void*, uint32_t, void*);
using PfnEventUnregister = uint32_t(WINAPI*)(uint64_t);
using PfnBCryptGenRandom = int32_t(WINAPI*)(void*, uint8_t*, uint32_t, uint32_t);

struct HostAdvapi {
    std::mutex mutex;
    bool loaded = false;
    HMODULE advapi = nullptr;
    HMODULE bcrypt = nullptr;

    PfnRegOpenKeyExW reg_open_w = nullptr;
    PfnRegOpenKeyExA reg_open_a = nullptr;
    PfnRegQueryValueExW reg_query_w = nullptr;
    PfnRegQueryValueExA reg_query_a = nullptr;
    PfnRegCloseKey reg_close = nullptr;
    PfnGetUserNameW get_user_w = nullptr;
    PfnGetUserNameA get_user_a = nullptr;
    PfnCryptAcquireContextW crypt_acq_w = nullptr;
    PfnCryptGenRandom crypt_gen_random = nullptr;
    PfnCryptReleaseContext crypt_rel = nullptr;
    PfnEventRegister event_reg = nullptr;
    PfnEventWrite event_write = nullptr;
    PfnEventUnregister event_unreg = nullptr;
    PfnBCryptGenRandom bcrypt_gen_random = nullptr;

    std::atomic<uintptr_t> next_local_prov{0x00A10001u};
    std::atomic<uint64_t> next_local_etw{0x00E70001ull};
    std::unordered_set<uintptr_t> local_provs;
    std::unordered_set<uint64_t> local_etw_handles;

    void EnsureLoaded() {
        std::lock_guard<std::mutex> lk(mutex);
        if (loaded) return;
        loaded = true;
        advapi = LoadSystemDll(L"advapi32.dll");
        bcrypt = LoadSystemDll(L"bcrypt.dll");
        if (advapi) {
            auto sym = [&](const char* n) -> void* {
                return reinterpret_cast<void*>(GetProcAddress(advapi, n));
            };
            reg_open_w = reinterpret_cast<PfnRegOpenKeyExW>(sym("RegOpenKeyExW"));
            reg_open_a = reinterpret_cast<PfnRegOpenKeyExA>(sym("RegOpenKeyExA"));
            reg_query_w = reinterpret_cast<PfnRegQueryValueExW>(sym("RegQueryValueExW"));
            reg_query_a = reinterpret_cast<PfnRegQueryValueExA>(sym("RegQueryValueExA"));
            reg_close = reinterpret_cast<PfnRegCloseKey>(sym("RegCloseKey"));
            get_user_w = reinterpret_cast<PfnGetUserNameW>(sym("GetUserNameW"));
            get_user_a = reinterpret_cast<PfnGetUserNameA>(sym("GetUserNameA"));
            crypt_acq_w = reinterpret_cast<PfnCryptAcquireContextW>(sym("CryptAcquireContextW"));
            crypt_gen_random = reinterpret_cast<PfnCryptGenRandom>(sym("CryptGenRandom"));
            crypt_rel = reinterpret_cast<PfnCryptReleaseContext>(sym("CryptReleaseContext"));
            event_reg = reinterpret_cast<PfnEventRegister>(sym("EventRegister"));
            event_write = reinterpret_cast<PfnEventWrite>(sym("EventWrite"));
            event_unreg = reinterpret_cast<PfnEventUnregister>(sym("EventUnregister"));
        }
        if (bcrypt) {
            bcrypt_gen_random = reinterpret_cast<PfnBCryptGenRandom>(
                reinterpret_cast<void*>(GetProcAddress(bcrypt, "BCryptGenRandom")));
        }
    }

    void FillRandomFallback(uint8_t* dst, uint32_t len) {
        if (!dst || len == 0u) return;
        if (bcrypt_gen_random && bcrypt_gen_random(nullptr, dst, len, 0x00000002u) >= 0) {
            return;
        }
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32u) ^ static_cast<uint64_t>(rd()) ^
                     static_cast<uint64_t>(GetTickCount64());
        for (uint32_t i = 0u; i < len; ++i) {
            s += 0x9E3779B97F4A7C15ull;
            uint64_t z = s;
            z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
            z = z ^ (z >> 31u);
            dst[i] = static_cast<uint8_t>(z & 0xFFu);
        }
    }
};

HostAdvapi& GetHostAdvapi() {
    static HostAdvapi* s = new HostAdvapi();
    return *s;
}
#endif

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL RegOpenKeyExW(
    void* hKey,
    const wchar_t* lpSubKey,
    uint32_t ulOptions,
    uint32_t samDesired,
    void** phkResult) {
    if (!phkResult) {
        RecordAdvapi("RegOpenKeyExW", false, static_cast<int32_t>(kErrInvalidParam));
        return static_cast<int32_t>(kErrInvalidParam);
    }
    *phkResult = nullptr;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_open_w) {
        const int32_t st = host.reg_open_w(hKey, lpSubKey, ulOptions, samDesired, phkResult);
        RecordAdvapi("RegOpenKeyExW", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpSubKey; (void)ulOptions; (void)samDesired;
#endif
    RecordAdvapi("RegOpenKeyExW", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegOpenKeyExA(
    void* hKey,
    const char* lpSubKey,
    uint32_t ulOptions,
    uint32_t samDesired,
    void** phkResult) {
    if (!phkResult) {
        RecordAdvapi("RegOpenKeyExA", false, static_cast<int32_t>(kErrInvalidParam));
        return static_cast<int32_t>(kErrInvalidParam);
    }
    *phkResult = nullptr;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_open_a) {
        const int32_t st = host.reg_open_a(hKey, lpSubKey, ulOptions, samDesired, phkResult);
        RecordAdvapi("RegOpenKeyExA", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpSubKey; (void)ulOptions; (void)samDesired;
#endif
    RecordAdvapi("RegOpenKeyExA", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegQueryValueExW(
    void* hKey,
    const wchar_t* lpValueName,
    uint32_t* lpReserved,
    uint32_t* lpType,
    uint8_t* lpData,
    uint32_t* lpcbData) {
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_query_w) {
        const int32_t st = host.reg_query_w(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
        RecordAdvapi("RegQueryValueExW", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpValueName; (void)lpReserved; (void)lpType; (void)lpData; (void)lpcbData;
#endif
    RecordAdvapi("RegQueryValueExW", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegQueryValueExA(
    void* hKey,
    const char* lpValueName,
    uint32_t* lpReserved,
    uint32_t* lpType,
    uint8_t* lpData,
    uint32_t* lpcbData) {
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_query_a) {
        const int32_t st = host.reg_query_a(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
        RecordAdvapi("RegQueryValueExA", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpValueName; (void)lpReserved; (void)lpType; (void)lpData; (void)lpcbData;
#endif
    RecordAdvapi("RegQueryValueExA", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegCloseKey(void* hKey) {
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_close) {
        const int32_t st = host.reg_close(hKey);
        RecordAdvapi("RegCloseKey", st == 0, st);
        return st;
    }
#else
    (void)hKey;
#endif
    RecordAdvapi("RegCloseKey", true, 0);
    return static_cast<int32_t>(kErrSuccess);
}

OS_DIAG_API int32_t OS_DIAG_CALL GetUserNameW(wchar_t* lpBuffer, uint32_t* pcbBuffer) {
    if (!pcbBuffer) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("GetUserNameW", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.get_user_w) {
        const int32_t ok = host.get_user_w(lpBuffer, pcbBuffer);
        if (ok != 0 || GetLastError() == kErrInsufficientBuf) {
            RecordAdvapi("GetUserNameW", ok != 0, ok != 0 ? 0 : static_cast<int32_t>(GetLastError()));
            return ok;
        }
    }
#endif
    static constexpr wchar_t kFallbackUserW[] = L"OpenScarlettUser";
    constexpr uint32_t kNeeded = static_cast<uint32_t>(sizeof(kFallbackUserW) / sizeof(wchar_t));
    if (!lpBuffer || *pcbBuffer < kNeeded) {
        *pcbBuffer = kNeeded;
#if defined(_WIN32)
        SetLastError(kErrInsufficientBuf);
#endif
        RecordAdvapi("GetUserNameW", false, static_cast<int32_t>(kErrInsufficientBuf));
        return 0;
    }
    std::memcpy(lpBuffer, kFallbackUserW, sizeof(kFallbackUserW));
    *pcbBuffer = kNeeded;
    RecordAdvapi("GetUserNameW", true, 0);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL GetUserNameA(char* lpBuffer, uint32_t* pcbBuffer) {
    if (!pcbBuffer) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("GetUserNameA", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.get_user_a) {
        const int32_t ok = host.get_user_a(lpBuffer, pcbBuffer);
        if (ok != 0 || GetLastError() == kErrInsufficientBuf) {
            RecordAdvapi("GetUserNameA", ok != 0, ok != 0 ? 0 : static_cast<int32_t>(GetLastError()));
            return ok;
        }
    }
#endif
    static constexpr char kFallbackUserA[] = "OpenScarlettUser";
    constexpr uint32_t kNeeded = static_cast<uint32_t>(sizeof(kFallbackUserA));
    if (!lpBuffer || *pcbBuffer < kNeeded) {
        *pcbBuffer = kNeeded;
#if defined(_WIN32)
        SetLastError(kErrInsufficientBuf);
#endif
        RecordAdvapi("GetUserNameA", false, static_cast<int32_t>(kErrInsufficientBuf));
        return 0;
    }
    std::memcpy(lpBuffer, kFallbackUserA, sizeof(kFallbackUserA));
    *pcbBuffer = kNeeded;
    RecordAdvapi("GetUserNameA", true, 0);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL CryptAcquireContextW(
    uintptr_t* phProv,
    const wchar_t* szContainer,
    const wchar_t* szProvider,
    uint32_t dwProvType,
    uint32_t dwFlags) {
    if (!phProv) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("CryptAcquireContextW", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
    *phProv = 0u;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.crypt_acq_w) {
        if (host.crypt_acq_w(phProv, szContainer, szProvider, dwProvType, dwFlags) != 0) {
            RecordAdvapi("CryptAcquireContextW", true, 0);
            return 1;
        }
    }
    const uintptr_t local_h = host.next_local_prov.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        host.local_provs.insert(local_h);
    }
    *phProv = local_h;
    RecordAdvapi("CryptAcquireContextW", true, 0);
    return 1;
#else
    (void)szContainer; (void)szProvider; (void)dwProvType; (void)dwFlags;
    *phProv = 0x00A10001u;
    RecordAdvapi("CryptAcquireContextW", true, 0);
    return 1;
#endif
}

OS_DIAG_API int32_t OS_DIAG_CALL CryptGenRandom(
    uintptr_t hProv,
    uint32_t dwLen,
    uint8_t* pbBuffer) {
    if (dwLen > 0u && !pbBuffer) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("CryptGenRandom", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
    if (dwLen == 0u) {
        RecordAdvapi("CryptGenRandom", true, 0);
        return 1;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    bool is_local = false;
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        is_local = (host.local_provs.find(hProv) != host.local_provs.end());
    }
    if (!is_local && host.crypt_gen_random) {
        if (host.crypt_gen_random(hProv, dwLen, pbBuffer) != 0) {
            RecordAdvapi("CryptGenRandom", true, 0);
            return 1;
        }
    }
    host.FillRandomFallback(pbBuffer, dwLen);
    RecordAdvapi("CryptGenRandom", true, 0);
    return 1;
#else
    (void)hProv;
    std::memset(pbBuffer, 0x5A, dwLen);
    RecordAdvapi("CryptGenRandom", true, 0);
    return 1;
#endif
}

OS_DIAG_API int32_t OS_DIAG_CALL CryptReleaseContext(uintptr_t hProv, uint32_t dwFlags) {
    if (hProv == 0u) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("CryptReleaseContext", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        if (host.local_provs.erase(hProv) > 0u) {
            RecordAdvapi("CryptReleaseContext", true, 0);
            return 1;
        }
    }
    if (host.crypt_rel) {
        const int32_t ok = host.crypt_rel(hProv, dwFlags);
        RecordAdvapi("CryptReleaseContext", ok != 0, ok != 0 ? 0 : static_cast<int32_t>(GetLastError()));
        return ok;
    }
#else
    (void)dwFlags;
#endif
    RecordAdvapi("CryptReleaseContext", true, 0);
    return 1;
}

OS_DIAG_API uint32_t OS_DIAG_CALL EventRegister(
    const void* ProviderId,
    void* EnableCallback,
    void* CallbackContext,
    uint64_t* RegHandle) {
    if (!ProviderId || !RegHandle) {
        RecordAdvapi("EventRegister", false, static_cast<int32_t>(kErrInvalidParam));
        return kErrInvalidParam;
    }
    *RegHandle = 0ull;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.event_reg) {
        const uint32_t st = host.event_reg(ProviderId, EnableCallback, CallbackContext, RegHandle);
        if (st == kErrSuccess) {
            RecordAdvapi("EventRegister", true, 0);
            return kErrSuccess;
        }
    }
    const uint64_t h = host.next_local_etw.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        host.local_etw_handles.insert(h);
    }
    *RegHandle = h;
    RecordAdvapi("EventRegister", true, 0);
    return kErrSuccess;
#else
    (void)EnableCallback; (void)CallbackContext;
    *RegHandle = 1ull;
    RecordAdvapi("EventRegister", true, 0);
    return kErrSuccess;
#endif
}

OS_DIAG_API uint32_t OS_DIAG_CALL EventWrite(
    uint64_t RegHandle,
    const void* EventDescriptor,
    uint32_t UserDataCount,
    void* UserData) {
    if (RegHandle == 0ull || !EventDescriptor) {
        RecordAdvapi("EventWrite", false, static_cast<int32_t>(kErrInvalidParam));
        return kErrInvalidParam;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    bool is_local = false;
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        is_local = (host.local_etw_handles.find(RegHandle) != host.local_etw_handles.end());
    }
    if (!is_local && host.event_write) {
        const uint32_t st = host.event_write(RegHandle, EventDescriptor, UserDataCount, UserData);
        RecordAdvapi("EventWrite", st == kErrSuccess, static_cast<int32_t>(st));
        return st;
    }
#else
    (void)UserDataCount; (void)UserData;
#endif
    RecordAdvapi("EventWrite", true, 0);
    return kErrSuccess;
}

OS_DIAG_API uint32_t OS_DIAG_CALL EventUnregister(uint64_t RegHandle) {
    if (RegHandle == 0ull) {
        RecordAdvapi("EventUnregister", false, static_cast<int32_t>(kErrInvalidParam));
        return kErrInvalidParam;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        if (host.local_etw_handles.erase(RegHandle) > 0u) {
            RecordAdvapi("EventUnregister", true, 0);
            return kErrSuccess;
        }
    }
    if (host.event_unreg) {
        const uint32_t st = host.event_unreg(RegHandle);
        RecordAdvapi("EventUnregister", st == kErrSuccess, static_cast<int32_t>(st));
        return st;
    }
#endif
    RecordAdvapi("EventUnregister", true, 0);
    return kErrSuccess;
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettAdvapiLegacyGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kAdvapiMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 2: KernelTraceControl.dll (ETW / kernel trace session clean stubs)
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_KERNELTRACECONTROL)

namespace {

constexpr const char* kKtcMod = "KernelTraceControl.dll";
constexpr uint32_t kErrSuccess = 0u;
constexpr uint32_t kErrInvalidHandle = 6u;
constexpr uint32_t kErrInvalidParam = 87u;

struct TraceSession {
    uint64_t handle = 0u;
    uint32_t process_id = 0u;
    uint32_t enabled_providers = 0u;
    uint32_t update_count = 0u;
};

struct KtcState {
    std::mutex mutex;
    uint64_t next_handle = 0x77AC0001ull;
    std::unordered_map<uint64_t, TraceSession> sessions;
};

KtcState& GetKtc() {
    static KtcState* s = new KtcState();
    return *s;
}

inline uint32_t RecordKtc(const char* sym, uint32_t err) {
    openscarlett::CallTelemetry::Global().Record(
        kKtcMod,
        sym,
        err == kErrSuccess ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        static_cast<int32_t>(err),
        -1);
    return err;
}

}  // namespace

extern "C" {

OS_DIAG_API uint32_t OS_DIAG_CALL StartKernelTrace(
    uint64_t* TraceHandle,
    void* Properties,
    const void* StackTracingEventIds,
    uint32_t cStackTracingEventIds) {
    (void)Properties;
    if (!TraceHandle || (cStackTracingEventIds > 0u && !StackTracingEventIds)) {
        return RecordKtc("StartKernelTrace", kErrInvalidParam);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint64_t h = st.next_handle++;
    st.sessions[h] = TraceSession{h, 0u, 0u, 0u};
    *TraceHandle = h;
    return RecordKtc("StartKernelTrace", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL StartKernelTraceForProcess(
    uint64_t* TraceHandle,
    void* Properties,
    uint32_t ProcessId) {
    (void)Properties;
    if (!TraceHandle) {
        return RecordKtc("StartKernelTraceForProcess", kErrInvalidParam);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint64_t h = st.next_handle++;
    st.sessions[h] = TraceSession{h, ProcessId, 0u, 0u};
    *TraceHandle = h;
    return RecordKtc("StartKernelTraceForProcess", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL StopKernelTrace(
    uint64_t TraceHandle,
    void* Properties) {
    (void)Properties;
    if (TraceHandle == 0ull) {
        return RecordKtc("StopKernelTrace", kErrInvalidHandle);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.sessions.erase(TraceHandle) == 0u) {
        return RecordKtc("StopKernelTrace", kErrInvalidHandle);
    }
    return RecordKtc("StopKernelTrace", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL UpdateKernelTrace(
    uint64_t TraceHandle,
    void* Properties) {
    (void)Properties;
    if (TraceHandle == 0ull) {
        return RecordKtc("UpdateKernelTrace", kErrInvalidHandle);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(TraceHandle);
    if (it == st.sessions.end()) {
        return RecordKtc("UpdateKernelTrace", kErrInvalidHandle);
    }
    ++it->second.update_count;
    return RecordKtc("UpdateKernelTrace", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL EnableTraceEx2(
    uint64_t TraceHandle,
    const void* ProviderId,
    uint32_t ControlCode,
    uint8_t Level,
    uint64_t MatchAnyKeyword,
    uint64_t MatchAllKeyword,
    uint32_t Timeout,
    void* EnableParameters) {
    (void)ControlCode; (void)Level; (void)MatchAnyKeyword;
    (void)MatchAllKeyword; (void)Timeout; (void)EnableParameters;
    if (TraceHandle == 0ull || !ProviderId) {
        return RecordKtc("EnableTraceEx2", kErrInvalidParam);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(TraceHandle);
    if (it != st.sessions.end()) {
        ++it->second.enabled_providers;
    }
    return RecordKtc("EnableTraceEx2", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL TraceQueryInformation(
    uint64_t SessionHandle,
    uint32_t InformationClass,
    void* TraceInformation,
    uint32_t InformationLength,
    uint32_t* ReturnLength) {
    (void)SessionHandle; (void)InformationClass;
    if (InformationLength > 0u && !TraceInformation) {
        return RecordKtc("TraceQueryInformation", kErrInvalidParam);
    }
    if (TraceInformation && InformationLength > 0u) {
        std::memset(TraceInformation, 0, InformationLength);
    }
    if (ReturnLength) {
        *ReturnLength = InformationLength;
    }
    return RecordKtc("TraceQueryInformation", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL TraceSetInformation(
    uint64_t SessionHandle,
    uint32_t InformationClass,
    void* TraceInformation,
    uint32_t InformationLength) {
    (void)SessionHandle; (void)InformationClass;
    if (InformationLength > 0u && !TraceInformation) {
        return RecordKtc("TraceSetInformation", kErrInvalidParam);
    }
    return RecordKtc("TraceSetInformation", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL CreateMergedTraceFile(
    const wchar_t* wszMergedFileName,
    const wchar_t** wszTraceFileNames,
    uint32_t cTraceFileNames,
    uint32_t dwExtendedDataFlags) {
    (void)dwExtendedDataFlags;
    if (!wszMergedFileName || !*wszMergedFileName ||
        (cTraceFileNames > 0u && !wszTraceFileNames)) {
        return RecordKtc("CreateMergedTraceFile", kErrInvalidParam);
    }
    return RecordKtc("CreateMergedTraceFile", kErrSuccess);
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettKernelTraceGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kKtcMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 3: hix_xs.dll (Xbox Hang-Dump / Debug Instrumentation Forwards)
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_HIX_XS)

namespace {

namespace fs = std::filesystem;
constexpr const char* kHixMod = "hix_xs.dll";

struct HixCallbackEntry {
    void* callback = nullptr;
    void* context = nullptr;
};

struct HixState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uint32_t flags = 0u;
    uint32_t hang_timeout_ms = 5000u;
    uint64_t heartbeats = 0u;
    uint64_t dump_requests = 0u;
    uint64_t next_cb_token = 1u;
    std::string last_heartbeat_tag;
    std::map<uint32_t, std::pair<std::string, uint64_t>> breadcrumbs;
    std::unordered_map<uint64_t, HixCallbackEntry> callbacks;
};

HixState& GetHix() {
    static HixState* s = new HixState();
    return *s;
}

inline int32_t RecordHix(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kHixMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

void ForwardBreadcrumbToXCrd(uint32_t slot, const char* tag, uint64_t value) {
#if defined(_WIN32)
    HMODULE crd = GetModuleHandleW(L"xcrdapi.dll");
    if (!crd) return;
    using PfnSetBc = int32_t(WINAPI*)(uint32_t, const char*, uint64_t);
    auto fn = reinterpret_cast<PfnSetBc>(
        reinterpret_cast<void*>(GetProcAddress(crd, "XCrdSetBreadcrumb")));
    if (fn) {
        (void)fn(slot, tag, value);
    }
#else
    (void)slot; (void)tag; (void)value;
#endif
}

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL HixInitialize(uint32_t flags) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    st.flags = flags;
    return RecordHix("HixInitialize", kHrOk);
}

OS_DIAG_API void OS_DIAG_CALL HixUninitialize(void) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    RecordHix("HixUninitialize", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixSetHangTimeoutMs(uint32_t timeoutMs) {
    if (timeoutMs == 0u) {
        return RecordHix("HixSetHangTimeoutMs", kHrInvalidArg);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.hang_timeout_ms = timeoutMs;
    return RecordHix("HixSetHangTimeoutMs", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixGetHangTimeoutMs(uint32_t* outTimeoutMs) {
    if (!outTimeoutMs) {
        return RecordHix("HixGetHangTimeoutMs", kHrPointer);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    *outTimeoutMs = st.hang_timeout_ms;
    return RecordHix("HixGetHangTimeoutMs", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixHeartbeat(const char* tag) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.heartbeats;
    if (tag) {
        st.last_heartbeat_tag = tag;
    }
    return RecordHix("HixHeartbeat", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixRegisterHangCallback(
    void* callback,
    void* context,
    uint64_t* outToken) {
    if (!callback || !outToken) {
        return RecordHix("HixRegisterHangCallback", kHrInvalidArg);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint64_t tok = st.next_cb_token++;
    st.callbacks[tok] = HixCallbackEntry{callback, context};
    *outToken = tok;
    return RecordHix("HixRegisterHangCallback", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixUnregisterHangCallback(uint64_t token) {
    if (token == 0ull) {
        return RecordHix("HixUnregisterHangCallback", kHrInvalidArg);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.callbacks.erase(token) == 0u) {
        return RecordHix("HixUnregisterHangCallback", kHrInvalidArg);
    }
    return RecordHix("HixUnregisterHangCallback", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixRecordBreadcrumb(
    uint32_t slot,
    const char* tag,
    uint64_t value) {
    if (slot >= 256u || !tag || !*tag) {
        return RecordHix("HixRecordBreadcrumb", kHrInvalidArg);
    }
    {
        auto& st = GetHix();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.breadcrumbs[slot] = std::make_pair(std::string(tag), value);
    }
    ForwardBreadcrumbToXCrd(slot, tag, value);
    return RecordHix("HixRecordBreadcrumb", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixRequestHangDump(
    const char* reason,
    const char* optionalOutputPath) {
    if (!reason || !*reason) {
        return RecordHix("HixRequestHangDump", kHrInvalidArg);
    }
    std::ostringstream oss;
    {
        auto& st = GetHix();
        std::lock_guard<std::mutex> lk(st.mutex);
        ++st.dump_requests;
        oss << "{\n"
            << "  \"module\": \"hix_xs.dll\",\n"
            << "  \"reason\": \"" << reason << "\",\n"
            << "  \"timeout_ms\": " << st.hang_timeout_ms << ",\n"
            << "  \"heartbeats\": " << st.heartbeats << ",\n"
            << "  \"last_heartbeat\": \"" << st.last_heartbeat_tag << "\",\n"
            << "  \"dump_requests\": " << st.dump_requests << "\n"
            << "}\n";
    }
    if (optionalOutputPath && *optionalOutputPath) {
        fs::path out_path(optionalOutputPath);
        std::error_code ec;
        if (out_path.has_parent_path()) {
            fs::create_directories(out_path.parent_path(), ec);
        }
        std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return RecordHix("HixRequestHangDump", kHrFail);
        }
        const std::string body = oss.str();
        ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
        ofs.flush();
        if (!ofs.good()) {
            return RecordHix("HixRequestHangDump", kHrFail);
        }
    }
    return RecordHix("HixRequestHangDump", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixGetStats(
    uint64_t* outHeartbeats,
    uint64_t* outDumpRequests) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (outHeartbeats) *outHeartbeats = st.heartbeats;
    if (outDumpRequests) *outDumpRequests = st.dump_requests;
    return RecordHix("HixGetStats", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettHixGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kHixMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 4: xbtp.dll (Xbox Binary Telemetry / Profiling Session Stubs)
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_XBTP)

namespace {

constexpr const char* kXbtpMod = "xbtp.dll";

struct XbtpSession {
    std::string name;
    uint32_t flags = 0u;
    uint64_t events_written = 0u;
    uint64_t bytes_written = 0u;
    uint64_t flush_count = 0u;
};

struct XbtpState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uint32_t flags = 0u;
    uintptr_t next_handle = 0x7B790001u;
    std::unordered_map<uintptr_t, XbtpSession> sessions;
};

XbtpState& GetXbtp() {
    static XbtpState* s = new XbtpState();
    return *s;
}

inline int32_t RecordXbtp(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kXbtpMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL XbtpInitialize(uint32_t flags) {
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    st.flags = flags;
    return RecordXbtp("XbtpInitialize", kHrOk);
}

OS_DIAG_API void OS_DIAG_CALL XbtpShutdown(void) {
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    RecordXbtp("XbtpShutdown", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpOpenSession(
    const char* sessionName,
    uint32_t flags,
    void** outSession) {
    if (!outSession) {
        return RecordXbtp("XbtpOpenSession", kHrPointer);
    }
    *outSession = nullptr;
    if (!sessionName || !*sessionName) {
        return RecordXbtp("XbtpOpenSession", kHrInvalidArg);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t h = st.next_handle++;
    XbtpSession sess{};
    sess.name = sessionName;
    sess.flags = flags;
    st.sessions.emplace(h, std::move(sess));
    *outSession = reinterpret_cast<void*>(h);
    return RecordXbtp("XbtpOpenSession", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpCloseSession(void* session) {
    if (!session) {
        return RecordXbtp("XbtpCloseSession", kHrHandle);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t key = reinterpret_cast<uintptr_t>(session);
    if (st.sessions.erase(key) == 0u) {
        return RecordXbtp("XbtpCloseSession", kHrHandle);
    }
    return RecordXbtp("XbtpCloseSession", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpWriteEvent(
    void* session,
    uint32_t eventId,
    const void* payload,
    uint32_t payloadBytes) {
    (void)eventId;
    if (!session) {
        return RecordXbtp("XbtpWriteEvent", kHrHandle);
    }
    if (payloadBytes > 0u && !payload) {
        return RecordXbtp("XbtpWriteEvent", kHrInvalidArg);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(reinterpret_cast<uintptr_t>(session));
    if (it == st.sessions.end()) {
        return RecordXbtp("XbtpWriteEvent", kHrHandle);
    }
    ++it->second.events_written;
    it->second.bytes_written += payloadBytes;
    return RecordXbtp("XbtpWriteEvent", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpFlushSession(void* session) {
    if (!session) {
        return RecordXbtp("XbtpFlushSession", kHrHandle);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(reinterpret_cast<uintptr_t>(session));
    if (it == st.sessions.end()) {
        return RecordXbtp("XbtpFlushSession", kHrHandle);
    }
    ++it->second.flush_count;
    return RecordXbtp("XbtpFlushSession", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpGetSessionStats(
    void* session,
    uint64_t* outEventsWritten,
    uint64_t* outBytesWritten,
    uint64_t* outFlushCount) {
    if (!session) {
        return RecordXbtp("XbtpGetSessionStats", kHrHandle);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(reinterpret_cast<uintptr_t>(session));
    if (it == st.sessions.end()) {
        return RecordXbtp("XbtpGetSessionStats", kHrHandle);
    }
    if (outEventsWritten) *outEventsWritten = it->second.events_written;
    if (outBytesWritten) *outBytesWritten = it->second.bytes_written;
    if (outFlushCount) *outFlushCount = it->second.flush_count;
    return RecordXbtp("XbtpGetSessionStats", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettXbtpGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kXbtpMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

#else
#  error "Define one of OPENSCARLETT_DIAG_TARGET_{ADVAPI32LEGACY,KERNELTRACECONTROL,HIX_XS,XBTP}"
#endif
