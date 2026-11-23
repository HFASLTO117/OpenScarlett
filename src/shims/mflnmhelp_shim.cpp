// MIT Media Foundation License & Network Helper (mflnmhelp.dll) for OpenScarlett.
// Portable and 0-warning clean across MSVC (/W4), Clang-CL, and MinGW-w64.

#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#  include <windows.h>
#endif

#if defined(_WIN32)
#  define OS_MFLNM_API __declspec(dllexport)
#  define OS_MFLNM_CALL __stdcall
#  if defined(_MSC_VER)
#    define OS_MFLNM_COM_API
#  else
#    define OS_MFLNM_COM_API __declspec(dllexport)
#  endif
#else
#  define OS_MFLNM_API __attribute__((visibility("default")))
#  define OS_MFLNM_COM_API __attribute__((visibility("default")))
#  define OS_MFLNM_CALL
#endif

namespace {

constexpr const char* kMod = "mflnmhelp.dll";
constexpr int32_t kHrOk = 0x00000000;
constexpr int32_t kHrFalse = 0x00000001;
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kHrHandle = static_cast<int32_t>(0x80070006u);

int32_t QueryModTelemetry(const char* symbol_name,
                          uint64_t* out_total,
                          uint64_t* out_ok,
                          uint64_t* out_not_impl) {
    if (out_total) *out_total = 0u;
    if (out_ok) *out_ok = 0u;
    if (out_not_impl) *out_not_impl = 0u;
    if (!symbol_name) {
        return 0;
    }
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == kMod && entry.symbol_name == symbol_name) {
            if (out_total) *out_total = entry.total_calls;
            if (out_ok) *out_ok = entry.ok_calls;
            if (out_not_impl) *out_not_impl = entry.not_impl_calls;
            return 1;
        }
    }
    return 0;
}

int32_t QueryModTotals(uint64_t* out_total_calls, uint64_t* out_not_impl_calls) {
    uint64_t total = 0u;
    uint64_t not_impl = 0u;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == kMod) {
            total += entry.total_calls;
            not_impl += entry.not_impl_calls;
        }
    }
    if (out_total_calls) *out_total_calls = total;
    if (out_not_impl_calls) *out_not_impl_calls = not_impl;
    return 1;
}

struct MfLnmState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uintptr_t next_license = 0x4C4E0001u;
    std::unordered_map<uintptr_t, std::wstring> licenses;
};

MfLnmState& GetMfLnm() {
    static MfLnmState* s = new MfLnmState();
    return *s;
}

inline int32_t RecordMod(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

}  // namespace

extern "C" {

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmHelperInitialize(uint32_t flags) {
    (void)flags;
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    return RecordMod("MfLnmHelperInitialize", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmHelperShutdown(void) {
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    return RecordMod("MfLnmHelperShutdown", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmCheckLicenseStatus(const wchar_t* contentId, uint32_t* outStatus) {
    (void)contentId;
    if (!outStatus) {
        return RecordMod("MfLnmCheckLicenseStatus", kHrPointer);
    }
    *outStatus = 1u;
    return RecordMod("MfLnmCheckLicenseStatus", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmQueryNetworkCapabilities(uint32_t* outCaps) {
    if (!outCaps) {
        return RecordMod("MfLnmQueryNetworkCapabilities", kHrPointer);
    }
    *outCaps = 0x1u;
    return RecordMod("MfLnmQueryNetworkCapabilities", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmAcquireLicense(const wchar_t* contentId, void** outLicenseHandle) {
    if (!outLicenseHandle) {
        return RecordMod("MfLnmAcquireLicense", kHrPointer);
    }
    *outLicenseHandle = nullptr;
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t handle = st.next_license++;
    st.licenses[handle] = contentId ? std::wstring(contentId) : std::wstring();
    *outLicenseHandle = reinterpret_cast<void*>(handle);
    return RecordMod("MfLnmAcquireLicense", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmReleaseLicense(void* licenseHandle) {
    if (!licenseHandle) {
        return RecordMod("MfLnmReleaseLicense", kHrHandle);
    }
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.licenses.erase(reinterpret_cast<uintptr_t>(licenseHandle));
    return RecordMod("MfLnmReleaseLicense", kHrOk);
}

#if defined(_MSC_VER)
#  pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#endif
OS_MFLNM_COM_API int32_t OS_MFLNM_CALL DllCanUnloadNow(void) {
    return RecordMod("DllCanUnloadNow", kHrFalse);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL OpenScarlettMfLnmHelpGetTelemetry(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls,
    uint64_t* outNotImplCalls) {
    if (!symbolName) {
        return QueryModTotals(outTotalCalls, outNotImplCalls);
    }
    return QueryModTelemetry(symbolName, outTotalCalls, outOkCalls, outNotImplCalls);
}

}  // extern "C"
