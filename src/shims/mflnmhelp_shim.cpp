// MIT Media Foundation License & Network Helper (mflnmhelp.dll) for OpenScarlett.
// Real stateful License / Revocation / Renewal / Expiry / Manifest-URL / Network-Caps
// Media Foundation Helper (upgraded from stub-tier in Sprint 16).
// Portable and 0-warning clean across MSVC (/W4), Clang-CL, and MinGW-w64.

#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHrAccessDenied = static_cast<int32_t>(0x80070005u);
constexpr int32_t kHrLicenseRevoked = static_cast<int32_t>(0xC00D7159u);

// License status codes returned by MfLnmCheckLicenseStatus / MfLnmGetLicenseInfo
constexpr uint32_t kLicenseStatusUnknown = 0u;
constexpr uint32_t kLicenseStatusValid = 1u;
constexpr uint32_t kLicenseStatusRevoked = 2u;
constexpr uint32_t kLicenseStatusExpired = 3u;

// Network & streaming protocol capability bitmask
constexpr uint32_t kNetCapHttp = 0x00000001u;
constexpr uint32_t kNetCapHttps = 0x00000002u;
constexpr uint32_t kNetCapHls = 0x00000004u;
constexpr uint32_t kNetCapDash = 0x00000008u;
constexpr uint32_t kNetCapPlayReady = 0x00000010u;
constexpr uint32_t kNetCapSmoothStreaming = 0x00000020u;
constexpr uint32_t kNetCapAll =
    kNetCapHttp | kNetCapHttps | kNetCapHls | kNetCapDash | kNetCapPlayReady | kNetCapSmoothStreaming;

constexpr uint64_t kDefaultLicenseTtlSec = 86400ull;  // 24 hours
constexpr uint64_t kMaxExtendSec = 365ull * 86400ull;

uint64_t CurrentUnixSeconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return sec > 0 ? static_cast<uint64_t>(sec) : 1ull;
}

std::wstring ToLowerW(const wchar_t* s) {
    if (!s) return {};
    std::wstring out;
    while (*s != L'\0') {
        out.push_back(static_cast<wchar_t>(std::towlower(*s)));
        ++s;
    }
    return out;
}

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

struct LicenseRecord {
    std::wstring content_id;
    uint64_t issued_unix_sec = 0ull;
    uint64_t expiry_unix_sec = 0ull;
    uint32_t renewal_count = 0u;
    bool revoked = false;
};

struct MfLnmState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uint32_t init_flags = 0u;
    uintptr_t next_license = 0x4C4E0001u;
    std::unordered_map<uintptr_t, LicenseRecord> licenses;
    std::unordered_set<std::wstring> revoked_content_ids;
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

uint32_t EvaluateRecordStatusLocked(const MfLnmState& st, const LicenseRecord& rec, uint64_t now_sec) {
    if (rec.revoked || st.revoked_content_ids.count(rec.content_id) > 0u) {
        return kLicenseStatusRevoked;
    }
    if (rec.expiry_unix_sec <= now_sec) {
        return kLicenseStatusExpired;
    }
    return kLicenseStatusValid;
}

}  // namespace

extern "C" {

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmHelperInitialize(uint32_t flags) {
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    st.init_flags = flags;
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
    if (!outStatus) {
        return RecordMod("MfLnmCheckLicenseStatus", kHrPointer);
    }
    *outStatus = kLicenseStatusUnknown;
    if (!contentId || *contentId == L'\0') {
        return RecordMod("MfLnmCheckLicenseStatus", kHrInvalidArg);
    }

    const std::wstring cid(contentId);
    const uint64_t now_sec = CurrentUnixSeconds();
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);

    if (st.revoked_content_ids.count(cid) > 0u) {
        *outStatus = kLicenseStatusRevoked;
        return RecordMod("MfLnmCheckLicenseStatus", kHrOk);
    }

    bool found_active = false;
    bool any_valid = false;
    bool any_expired = false;
    for (const auto& kv : st.licenses) {
        if (kv.second.content_id == cid) {
            found_active = true;
            const uint32_t ls = EvaluateRecordStatusLocked(st, kv.second, now_sec);
            if (ls == kLicenseStatusValid) {
                any_valid = true;
            } else if (ls == kLicenseStatusExpired) {
                any_expired = true;
            }
        }
    }

    if (found_active) {
        *outStatus = any_valid ? kLicenseStatusValid
                               : (any_expired ? kLicenseStatusExpired : kLicenseStatusRevoked);
    } else {
        // Default non-revoked PlayReady-eligible content reports Valid (status == 1).
        *outStatus = kLicenseStatusValid;
    }
    return RecordMod("MfLnmCheckLicenseStatus", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmQueryNetworkCapabilities(uint32_t* outCaps) {
    if (!outCaps) {
        return RecordMod("MfLnmQueryNetworkCapabilities", kHrPointer);
    }
    *outCaps = kNetCapAll;
    return RecordMod("MfLnmQueryNetworkCapabilities", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmAcquireLicense(const wchar_t* contentId, void** outLicenseHandle) {
    if (!outLicenseHandle) {
        return RecordMod("MfLnmAcquireLicense", kHrPointer);
    }
    *outLicenseHandle = nullptr;
    if (!contentId || *contentId == L'\0') {
        return RecordMod("MfLnmAcquireLicense", kHrInvalidArg);
    }

    const std::wstring cid(contentId);
    const uint64_t now_sec = CurrentUnixSeconds();
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.revoked_content_ids.count(cid) > 0u) {
        return RecordMod("MfLnmAcquireLicense", kHrLicenseRevoked);
    }

    const uintptr_t handle = st.next_license++;
    LicenseRecord rec{};
    rec.content_id = cid;
    rec.issued_unix_sec = now_sec;
    rec.expiry_unix_sec = now_sec + kDefaultLicenseTtlSec;
    rec.renewal_count = 0u;
    rec.revoked = false;
    st.licenses.emplace(handle, std::move(rec));
    *outLicenseHandle = reinterpret_cast<void*>(handle);
    return RecordMod("MfLnmAcquireLicense", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmReleaseLicense(void* licenseHandle) {
    if (!licenseHandle) {
        return RecordMod("MfLnmReleaseLicense", kHrHandle);
    }
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    const size_t erased = st.licenses.erase(reinterpret_cast<uintptr_t>(licenseHandle));
    if (erased == 0u) {
        return RecordMod("MfLnmReleaseLicense", kHrHandle);
    }
    return RecordMod("MfLnmReleaseLicense", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmRenewLicense(void* licenseHandle, uint64_t extendSeconds) {
    if (!licenseHandle) {
        return RecordMod("MfLnmRenewLicense", kHrHandle);
    }
    if (extendSeconds == 0ull || extendSeconds > kMaxExtendSec) {
        return RecordMod("MfLnmRenewLicense", kHrInvalidArg);
    }
    const uint64_t now_sec = CurrentUnixSeconds();
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.licenses.find(reinterpret_cast<uintptr_t>(licenseHandle));
    if (it == st.licenses.end()) {
        return RecordMod("MfLnmRenewLicense", kHrHandle);
    }
    if (it->second.revoked || st.revoked_content_ids.count(it->second.content_id) > 0u) {
        return RecordMod("MfLnmRenewLicense", kHrLicenseRevoked);
    }
    const uint64_t base = std::max(now_sec, it->second.expiry_unix_sec);
    it->second.expiry_unix_sec = base + extendSeconds;
    ++it->second.renewal_count;
    return RecordMod("MfLnmRenewLicense", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmRevokeLicense(const wchar_t* contentId) {
    if (!contentId || *contentId == L'\0') {
        return RecordMod("MfLnmRevokeLicense", kHrInvalidArg);
    }
    const std::wstring cid(contentId);
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.revoked_content_ids.insert(cid);
    for (auto& kv : st.licenses) {
        if (kv.second.content_id == cid) {
            kv.second.revoked = true;
        }
    }
    return RecordMod("MfLnmRevokeLicense", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmClearRevocation(const wchar_t* contentId) {
    if (!contentId || *contentId == L'\0') {
        return RecordMod("MfLnmClearRevocation", kHrInvalidArg);
    }
    const std::wstring cid(contentId);
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.revoked_content_ids.erase(cid);
    for (auto& kv : st.licenses) {
        if (kv.second.content_id == cid) {
            kv.second.revoked = false;
        }
    }
    return RecordMod("MfLnmClearRevocation", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmGetLicenseInfo(
    void* licenseHandle,
    uint32_t* outStatus,
    uint64_t* outExpiryUnixSec,
    uint32_t* outRenewalCount) {
    if (outStatus) *outStatus = kLicenseStatusUnknown;
    if (outExpiryUnixSec) *outExpiryUnixSec = 0ull;
    if (outRenewalCount) *outRenewalCount = 0u;
    if (!licenseHandle) {
        return RecordMod("MfLnmGetLicenseInfo", kHrHandle);
    }
    const uint64_t now_sec = CurrentUnixSeconds();
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.licenses.find(reinterpret_cast<uintptr_t>(licenseHandle));
    if (it == st.licenses.end()) {
        return RecordMod("MfLnmGetLicenseInfo", kHrHandle);
    }
    if (outStatus) {
        *outStatus = EvaluateRecordStatusLocked(st, it->second, now_sec);
    }
    if (outExpiryUnixSec) {
        *outExpiryUnixSec = it->second.expiry_unix_sec;
    }
    if (outRenewalCount) {
        *outRenewalCount = it->second.renewal_count;
    }
    return RecordMod("MfLnmGetLicenseInfo", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmGetActiveLicenseCount(uint32_t* outCount) {
    if (!outCount) {
        return RecordMod("MfLnmGetActiveLicenseCount", kHrPointer);
    }
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    *outCount = static_cast<uint32_t>(st.licenses.size());
    return RecordMod("MfLnmGetActiveLicenseCount", kHrOk);
}

OS_MFLNM_API int32_t OS_MFLNM_CALL MfLnmValidateManifestUrl(
    const wchar_t* url,
    uint32_t* outProtocolFlags) {
    if (outProtocolFlags) *outProtocolFlags = 0u;
    if (!url || *url == L'\0') {
        return RecordMod("MfLnmValidateManifestUrl", kHrInvalidArg);
    }
    const std::wstring low = ToLowerW(url);
    uint32_t flags = 0u;
    size_t host_start = 0u;
    if (low.rfind(L"https://", 0u) == 0u) {
        flags |= kNetCapHttps;
        host_start = 8u;
    } else if (low.rfind(L"http://", 0u) == 0u) {
        flags |= kNetCapHttp;
        host_start = 7u;
    } else {
        // Reject file://, javascript:, data:, or schemeless URLs.
        return RecordMod("MfLnmValidateManifestUrl", kHrAccessDenied);
    }

    // Must contain a non-empty host portion without whitespace or control chars.
    if (host_start >= low.size() || low[host_start] == L'/' || low[host_start] == L'?') {
        return RecordMod("MfLnmValidateManifestUrl", kHrInvalidArg);
    }
    for (wchar_t ch : low) {
        if (ch <= 0x20 || ch == L'<' || ch == L'>' || ch == L'"') {
            return RecordMod("MfLnmValidateManifestUrl", kHrInvalidArg);
        }
    }

    // Strip query string/fragment for extension classification.
    size_t path_end = low.find_first_of(L"?#", host_start);
    if (path_end == std::wstring::npos) {
        path_end = low.size();
    }
    const std::wstring path_part = low.substr(host_start, path_end - host_start);
    auto ends_with = [&](const wchar_t* suffix) -> bool {
        const size_t slen = std::wcslen(suffix);
        return path_part.size() >= slen &&
               path_part.compare(path_part.size() - slen, slen, suffix) == 0;
    };
    if (ends_with(L".m3u8")) {
        flags |= kNetCapHls;
    } else if (ends_with(L".mpd")) {
        flags |= kNetCapDash;
    } else if (ends_with(L".ism") || path_part.find(L".ism/manifest") != std::wstring::npos) {
        flags |= kNetCapSmoothStreaming;
    }

    if (outProtocolFlags) {
        *outProtocolFlags = flags;
    }
    return RecordMod("MfLnmValidateManifestUrl", kHrOk);
}

#if defined(_MSC_VER)
#  pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#endif
OS_MFLNM_COM_API int32_t OS_MFLNM_CALL DllCanUnloadNow(void) {
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    const bool idle = (st.init_count == 0u) && st.licenses.empty();
    return RecordMod("DllCanUnloadNow", idle ? kHrOk : kHrFalse);
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
