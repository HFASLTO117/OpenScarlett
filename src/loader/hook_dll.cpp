#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/loader/vfs_mounts.h"

namespace {

// ---------------------------------------------------------------------------
// Real Win32 API Function Pointer Types
// ---------------------------------------------------------------------------
using PfnLoadLibraryA = HMODULE(WINAPI*)(LPCSTR);
using PfnLoadLibraryW = HMODULE(WINAPI*)(LPCWSTR);
using PfnLoadLibraryExA = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using PfnLoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using PfnGetModuleHandleA = HMODULE(WINAPI*)(LPCSTR);
using PfnGetModuleHandleW = HMODULE(WINAPI*)(LPCWSTR);
using PfnGetModuleHandleExA = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);
using PfnGetModuleHandleExW = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
using PfnGetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using PfnCreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PfnCreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PfnGetFileAttributesA = DWORD(WINAPI*)(LPCSTR);
using PfnGetFileAttributesW = DWORD(WINAPI*)(LPCWSTR);
using PfnGetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using PfnCreateDirectoryW = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PfnFindFirstFileW = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using PfnExitProcess = VOID(WINAPI*)(UINT);

PfnLoadLibraryA g_real_LoadLibraryA = nullptr;
PfnLoadLibraryW g_real_LoadLibraryW = nullptr;
PfnLoadLibraryExA g_real_LoadLibraryExA = nullptr;
PfnLoadLibraryExW g_real_LoadLibraryExW = nullptr;
PfnGetModuleHandleA g_real_GetModuleHandleA = nullptr;
PfnGetModuleHandleW g_real_GetModuleHandleW = nullptr;
PfnGetModuleHandleExA g_real_GetModuleHandleExA = nullptr;
PfnGetModuleHandleExW g_real_GetModuleHandleExW = nullptr;
PfnGetProcAddress g_real_GetProcAddress = nullptr;
PfnCreateFileA g_real_CreateFileA = nullptr;
PfnCreateFileW g_real_CreateFileW = nullptr;
PfnGetFileAttributesA g_real_GetFileAttributesA = nullptr;
PfnGetFileAttributesW g_real_GetFileAttributesW = nullptr;
PfnGetFileAttributesExW g_real_GetFileAttributesExW = nullptr;
PfnCreateDirectoryW g_real_CreateDirectoryW = nullptr;
PfnFindFirstFileW g_real_FindFirstFileW = nullptr;
PfnExitProcess g_real_ExitProcess = nullptr;

// ---------------------------------------------------------------------------
// Hook Runtime State
// ---------------------------------------------------------------------------
HMODULE g_self_module = nullptr;
std::wstring g_shim_dir_w;
std::wstring g_windows_dir_lower_w;
std::string g_telemetry_path_utf8;
openscarlett::VfsMounts g_vfs;

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_flushed_on_exit{false};

SRWLOCK g_state_lock = SRWLOCK_INIT;
SRWLOCK g_patch_lock = SRWLOCK_INIT;
std::unordered_map<HMODULE, std::string> g_tracked_xbox_modules;
std::unordered_set<HMODULE> g_non_xbox_modules;

struct PatchedModuleKey {
    HMODULE base = nullptr;
    uint32_t size_of_image = 0;
    uint32_t timestamp = 0;
};
std::unordered_map<HMODULE, PatchedModuleKey> g_patched_modules;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::wstring ReadEnvW(const wchar_t* name) {
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring buf(needed, L'\0');
    DWORD written = ::GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return buf;
}

std::wstring AnsiToWide(LPCSTR text) {
    if (!text || *text == '\0') {
        return {};
    }
    int len = ::MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (len <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), len);
    return out;
}

std::wstring ToLowerAsciiW(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return out;
}

std::string ToLowerAsciiA(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::wstring GetModulePathW(HMODULE mod) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1) {
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

struct ShimmedDllSpec {
    const wchar_t* lower_name;
    const wchar_t* canonical_w;
    const char* canonical_a;
};

constexpr ShimmedDllSpec kShimmedXboxDlls[] = {
    {L"d3d12_xs.dll", L"d3d12_xs.dll", "d3d12_xs.dll"},
    {L"xg_xs.dll", L"xg_xs.dll", "xg_xs.dll"},
    {L"kernelx.dll", L"kernelx.dll", "kernelx.dll"},
    {L"xgameruntime.dll", L"xgameruntime.dll", "xgameruntime.dll"},
    {L"gameinput.dll", L"GameInput.dll", "GameInput.dll"},
    {L"xaudio2_9.dll", L"XAudio2_9.dll", "XAudio2_9.dll"},
    {L"acphal.dll", L"AcpHal.dll", "AcpHal.dll"},
    {L"dstorage_xs.dll", L"dstorage_xs.dll", "dstorage_xs.dll"},
    {L"winrt_xs.dll", L"winrt_xs.dll", "winrt_xs.dll"},
    {L"microsoft.xbox.services.gdk.c.dll", L"Microsoft.Xbox.Services.GDK.C.dll",
     "Microsoft.Xbox.Services.GDK.C.dll"},
    {L"xcrypt.dll", L"xcrypt.dll", "xcrypt.dll"},
    {L"xcurl.dll", L"xcurl.dll", "xcurl.dll"},
    {L"libhttpclient.gdk.dll", L"libHttpClient.GDK.dll", "libHttpClient.GDK.dll"},
};

bool MatchXboxShimmedDllW(std::wstring_view raw_name,
                          std::wstring& out_canonical_w,
                          std::string& out_canonical_a) {
    if (raw_name.empty()) {
        return false;
    }
    size_t slash = raw_name.find_last_of(L"\\/");
    std::wstring_view base = (slash == std::wstring_view::npos) ? raw_name : raw_name.substr(slash + 1);
    if (base.empty()) {
        return false;
    }
    std::wstring lower = ToLowerAsciiW(base);
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".dll") != 0) {
        lower.append(L".dll");
    }
    for (const auto& spec : kShimmedXboxDlls) {
        if (lower == spec.lower_name) {
            out_canonical_w = spec.canonical_w;
            out_canonical_a = spec.canonical_a;
            return true;
        }
    }
    return false;
}

bool IsWindowsSystemModule(HMODULE mod) {
    if (!mod || mod == g_self_module) {
        return true;
    }
    std::wstring path = GetModulePathW(mod);
    if (path.empty()) {
        return true;
    }
    std::wstring lower = ToLowerAsciiW(path);
    for (wchar_t& c : lower) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (!g_windows_dir_lower_w.empty()) {
        const std::wstring sys32 = g_windows_dir_lower_w + L"\\system32\\";
        const std::wstring syswow = g_windows_dir_lower_w + L"\\syswow64\\";
        const std::wstring winsxs = g_windows_dir_lower_w + L"\\winsxs\\";
        if (lower.rfind(sys32, 0) == 0 ||
            lower.rfind(syswow, 0) == 0 ||
            lower.rfind(winsxs, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool IsHookableImportDll(const char* dll_name) {
    if (!dll_name || *dll_name == '\0') {
        return false;
    }
    std::string lo = ToLowerAsciiA(dll_name);
    return lo == "kernel32.dll" ||
           lo == "kernelbase.dll" ||
           lo.rfind("api-ms-win-core-", 0) == 0;
}

void RegisterXboxModule(HMODULE mod, const std::string& canonical_name) {
    if (!mod) {
        return;
    }
    ::AcquireSRWLockExclusive(&g_state_lock);
    g_tracked_xbox_modules[mod] = canonical_name;
    g_non_xbox_modules.erase(mod);
    ::ReleaseSRWLockExclusive(&g_state_lock);
}

bool LookupXboxModuleName(HMODULE mod, std::string& out_name) {
    if (!mod) {
        return false;
    }
    ::AcquireSRWLockShared(&g_state_lock);
    auto it = g_tracked_xbox_modules.find(mod);
    if (it != g_tracked_xbox_modules.end()) {
        out_name = it->second;
        ::ReleaseSRWLockShared(&g_state_lock);
        return true;
    }
    bool known_non_xbox = (g_non_xbox_modules.find(mod) != g_non_xbox_modules.end());
    ::ReleaseSRWLockShared(&g_state_lock);

    if (known_non_xbox) {
        return false;
    }

    std::wstring path = GetModulePathW(mod);
    std::wstring canon_w;
    std::string canon_a;
    if (MatchXboxShimmedDllW(path, canon_w, canon_a)) {
        RegisterXboxModule(mod, canon_a);
        out_name = std::move(canon_a);
        return true;
    }

    ::AcquireSRWLockExclusive(&g_state_lock);
    g_non_xbox_modules.insert(mod);
    ::ReleaseSRWLockExclusive(&g_state_lock);
    return false;
}

int FlushTelemetryInternal(bool only_once) {
    if (only_once) {
        if (g_flushed_on_exit.exchange(true, std::memory_order_acq_rel)) {
            return 1;
        }
    }
    if (g_telemetry_path_utf8.empty()) {
        return 0;
    }
    std::wstring tel_w = openscarlett::Utf8ToWide(g_telemetry_path_utf8);
    std::wstring parent_w = std::filesystem::path(tel_w).parent_path().wstring();
    if (!parent_w.empty()) {
        openscarlett::EnsureDirectoryTreeW(parent_w);
    }
    auto st = openscarlett::CallTelemetry::Global().WriteJsonReport(g_telemetry_path_utf8);
    return st.ok() ? 1 : 0;
}

// Forward declarations for recursive IAT/delay-load patching
void PatchModuleImports(HMODULE mod);
void PatchAllLoadedNonSystemModules();

// ---------------------------------------------------------------------------
// Hook Implementations: Library Loading & Symbol Resolution
// ---------------------------------------------------------------------------
HMODULE RedirectAndLoadXboxDll(const std::wstring& canonical_w,
                               const std::string& canonical_a,
                               const char* api_name,
                               HANDLE hFile,
                               DWORD dwFlags,
                               bool use_ex) {
    std::wstring target_path = g_shim_dir_w.empty()
                                   ? canonical_w
                                   : (g_shim_dir_w + L"\\" + canonical_w);

    HMODULE loaded = nullptr;
    if (use_ex) {
        DWORD sanitized_flags = dwFlags;
        constexpr DWORD kConflictingSearchFlags =
            LOAD_LIBRARY_SEARCH_SYSTEM32 |
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
            LOAD_LIBRARY_SEARCH_USER_DIRS |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
        if ((sanitized_flags & kConflictingSearchFlags) != 0) {
            sanitized_flags &= ~kConflictingSearchFlags;
        }
        constexpr DWORD kDatafileFlags =
            LOAD_LIBRARY_AS_DATAFILE |
            LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
            LOAD_LIBRARY_AS_IMAGE_RESOURCE;
        if ((sanitized_flags & kDatafileFlags) == 0 && !g_shim_dir_w.empty()) {
            sanitized_flags |= LOAD_WITH_ALTERED_SEARCH_PATH;
        }
        loaded = g_real_LoadLibraryExW(target_path.c_str(), hFile, sanitized_flags);
    } else {
        loaded = g_real_LoadLibraryW(target_path.c_str());
    }

    DWORD last_err = ::GetLastError();

    if (!g_shutting_down.load(std::memory_order_acquire)) {
        if (loaded != nullptr) {
            RegisterXboxModule(loaded, canonical_a);
            openscarlett::CallTelemetry::Global().Record(
                canonical_a, api_name, openscarlett::CallOutcome::Ok, 0, -1);
            constexpr DWORD kDatafileFlags =
                LOAD_LIBRARY_AS_DATAFILE |
                LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                LOAD_LIBRARY_AS_IMAGE_RESOURCE;
            if ((dwFlags & kDatafileFlags) == 0) {
                PatchModuleImports(loaded);
                PatchAllLoadedNonSystemModules();
            }
        } else {
            openscarlett::CallTelemetry::Global().Record(
                canonical_a, api_name, openscarlett::CallOutcome::NotImplemented,
                static_cast<int32_t>(last_err), -1);
        }
    }

    ::SetLastError(last_err);
    return loaded;
}

HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        std::wstring wide_name = AnsiToWide(lpLibFileName);
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(wide_name, canon_w, canon_a)) {
            return RedirectAndLoadXboxDll(canon_w, canon_a, "LoadLibraryA", nullptr, 0, false);
        }
    }
    HMODULE mod = g_real_LoadLibraryA(lpLibFileName);
    DWORD err = ::GetLastError();
    if (mod != nullptr && !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(lpLibFileName, canon_w, canon_a)) {
            return RedirectAndLoadXboxDll(canon_w, canon_a, "LoadLibraryW", nullptr, 0, false);
        }
    }
    HMODULE mod = g_real_LoadLibraryW(lpLibFileName);
    DWORD err = ::GetLastError();
    if (mod != nullptr && !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        std::wstring wide_name = AnsiToWide(lpLibFileName);
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(wide_name, canon_w, canon_a)) {
            return RedirectAndLoadXboxDll(canon_w, canon_a, "LoadLibraryExA", hFile, dwFlags, true);
        }
    }
    HMODULE mod = g_real_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    DWORD err = ::GetLastError();
    constexpr DWORD kDatafileFlags =
        LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
        LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (mod != nullptr && (dwFlags & kDatafileFlags) == 0 &&
        !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpLibFileName != nullptr) {
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(lpLibFileName, canon_w, canon_a)) {
            return RedirectAndLoadXboxDll(canon_w, canon_a, "LoadLibraryExW", hFile, dwFlags, true);
        }
    }
    HMODULE mod = g_real_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    DWORD err = ::GetLastError();
    constexpr DWORD kDatafileFlags =
        LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
        LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (mod != nullptr && (dwFlags & kDatafileFlags) == 0 &&
        !g_shutting_down.load(std::memory_order_acquire)) {
        PatchModuleImports(mod);
        PatchAllLoadedNonSystemModules();
    }
    ::SetLastError(err);
    return mod;
}

HMODULE WINAPI Hook_GetModuleHandleA(LPCSTR lpModuleName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpModuleName != nullptr && !g_shim_dir_w.empty()) {
        std::wstring wide_name = AnsiToWide(lpModuleName);
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(wide_name, canon_w, canon_a)) {
            std::wstring redirected = g_shim_dir_w + L"\\" + canon_w;
            HMODULE mod = g_real_GetModuleHandleW(redirected.c_str());
            if (mod != nullptr) {
                return mod;
            }
        }
    }
    return g_real_GetModuleHandleA(lpModuleName);
}

HMODULE WINAPI Hook_GetModuleHandleW(LPCWSTR lpModuleName) {
    if (!g_shutting_down.load(std::memory_order_acquire) && lpModuleName != nullptr && !g_shim_dir_w.empty()) {
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(lpModuleName, canon_w, canon_a)) {
            std::wstring redirected = g_shim_dir_w + L"\\" + canon_w;
            HMODULE mod = g_real_GetModuleHandleW(redirected.c_str());
            if (mod != nullptr) {
                return mod;
            }
        }
    }
    return g_real_GetModuleHandleW(lpModuleName);
}

BOOL WINAPI Hook_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 &&
        lpModuleName != nullptr && !g_shim_dir_w.empty()) {
        std::wstring wide_name = AnsiToWide(lpModuleName);
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(wide_name, canon_w, canon_a)) {
            std::wstring redirected = g_shim_dir_w + L"\\" + canon_w;
            if (g_real_GetModuleHandleExW(dwFlags, redirected.c_str(), phModule)) {
                return TRUE;
            }
        }
    }
    return g_real_GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI Hook_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 &&
        lpModuleName != nullptr && !g_shim_dir_w.empty()) {
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(lpModuleName, canon_w, canon_a)) {
            std::wstring redirected = g_shim_dir_w + L"\\" + canon_w;
            if (g_real_GetModuleHandleExW(dwFlags, redirected.c_str(), phModule)) {
                return TRUE;
            }
        }
    }
    return g_real_GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    FARPROC proc = g_real_GetProcAddress(hModule, lpProcName);
    DWORD last_err = ::GetLastError();

    if (!g_shutting_down.load(std::memory_order_acquire) && hModule != nullptr && lpProcName != nullptr) {
        std::string mod_name;
        if (LookupXboxModuleName(hModule, mod_name)) {
            std::string sym_name;
            if (reinterpret_cast<uintptr_t>(lpProcName) <= 0xFFFFu) {
                sym_name = "ordinal#" + std::to_string(reinterpret_cast<uintptr_t>(lpProcName));
            } else {
                sym_name = lpProcName;
            }
            if (proc != nullptr) {
                openscarlett::CallTelemetry::Global().Record(
                    mod_name, sym_name, openscarlett::CallOutcome::Ok, 0, -1);
            } else {
                openscarlett::CallTelemetry::Global().Record(
                    mod_name,
                    sym_name,
                    openscarlett::CallOutcome::NotImplemented,
                    static_cast<int32_t>(0x80004001u),  // E_NOTIMPL
                    -1);
            }
        }
    }

    ::SetLastError(last_err);
    return proc;
}

// ---------------------------------------------------------------------------
// Hook Implementations: Virtual Drive VFS (G:\, T:\, S:\)
// ---------------------------------------------------------------------------
HANDLE WINAPI Hook_CreateFileW(LPCWSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring host_path;
        auto st = g_vfs.TranslateW(lpFileName, host_path, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "CreateFileW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            HANDLE h = g_real_CreateFileW(host_path.c_str(),
                                          dwDesiredAccess,
                                          dwShareMode,
                                          lpSecurityAttributes,
                                          dwCreationDisposition,
                                          dwFlagsAndAttributes,
                                          hTemplateFile);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "CreateFileW",
                h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return h;
        }
    }
    return g_real_CreateFileW(lpFileName,
                              dwDesiredAccess,
                              dwShareMode,
                              lpSecurityAttributes,
                              dwCreationDisposition,
                              dwFlagsAndAttributes,
                              hTemplateFile);
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring wide_in = AnsiToWide(lpFileName);
        std::wstring host_path;
        auto st = g_vfs.TranslateW(wide_in, host_path, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "CreateFileA:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            HANDLE h = g_real_CreateFileW(host_path.c_str(),
                                          dwDesiredAccess,
                                          dwShareMode,
                                          lpSecurityAttributes,
                                          dwCreationDisposition,
                                          dwFlagsAndAttributes,
                                          hTemplateFile);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "CreateFileA",
                h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return h;
        }
    }
    return g_real_CreateFileA(lpFileName,
                              dwDesiredAccess,
                              dwShareMode,
                              lpSecurityAttributes,
                              dwCreationDisposition,
                              dwFlagsAndAttributes,
                              hTemplateFile);
}

DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR lpFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring host_path;
        auto st = g_vfs.TranslateW(lpFileName, host_path, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            DWORD attrs = g_real_GetFileAttributesW(host_path.c_str());
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "GetFileAttributesW",
                attrs != INVALID_FILE_ATTRIBUTES ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                attrs != INVALID_FILE_ATTRIBUTES ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return attrs;
        }
    }
    return g_real_GetFileAttributesW(lpFileName);
}

DWORD WINAPI Hook_GetFileAttributesA(LPCSTR lpFileName) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveA(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring wide_in = AnsiToWide(lpFileName);
        std::wstring host_path;
        auto st = g_vfs.TranslateW(wide_in, host_path, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesA:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            DWORD attrs = g_real_GetFileAttributesW(host_path.c_str());
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "GetFileAttributesA",
                attrs != INVALID_FILE_ATTRIBUTES ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                attrs != INVALID_FILE_ATTRIBUTES ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return attrs;
        }
    }
    return g_real_GetFileAttributesA(lpFileName);
}

BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR lpFileName,
                                      GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring host_path;
        auto st = g_vfs.TranslateW(lpFileName, host_path, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "GetFileAttributesExW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            BOOL ok = g_real_GetFileAttributesExW(host_path.c_str(), fInfoLevelId, lpFileInformation);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "GetFileAttributesExW",
                ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                ok ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return ok;
        }
    }
    return g_real_GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

BOOL WINAPI Hook_CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpPathName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpPathName) != openscarlett::VfsDrive::None) {
        std::wstring host_path;
        auto st = g_vfs.TranslateW(lpPathName, host_path, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "CreateDirectoryW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            BOOL ok = g_real_CreateDirectoryW(host_path.c_str(), lpSecurityAttributes);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "CreateDirectoryW",
                ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                ok ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return ok;
        }
    }
    return g_real_CreateDirectoryW(lpPathName, lpSecurityAttributes);
}

HANDLE WINAPI Hook_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        lpFileName != nullptr &&
        openscarlett::VfsMounts::DetectVirtualDriveW(lpFileName) != openscarlett::VfsDrive::None) {
        std::wstring host_path;
        auto st = g_vfs.TranslateW(lpFileName, host_path, true);
        if (st == openscarlett::VfsTranslateStatus::RejectedTraversal) {
            openscarlett::CallTelemetry::Global().Record(
                "vfs", "FindFirstFileW:traversal_blocked", openscarlett::CallOutcome::Error,
                static_cast<int32_t>(ERROR_ACCESS_DENIED), -1);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (st == openscarlett::VfsTranslateStatus::Translated) {
            HANDLE h = g_real_FindFirstFileW(host_path.c_str(), lpFindFileData);
            DWORD err = ::GetLastError();
            openscarlett::CallTelemetry::Global().Record(
                "vfs",
                "FindFirstFileW",
                h != INVALID_HANDLE_VALUE ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
                h != INVALID_HANDLE_VALUE ? 0 : static_cast<int32_t>(err),
                -1);
            ::SetLastError(err);
            return h;
        }
    }
    return g_real_FindFirstFileW(lpFileName, lpFindFileData);
}

VOID WINAPI Hook_ExitProcess(UINT uExitCode) {
    FlushTelemetryInternal(true);
    g_shutting_down.store(true, std::memory_order_release);
    g_real_ExitProcess(uExitCode);
}

// ---------------------------------------------------------------------------
// IAT & Delay-Load Import Table Patcher (PE32+ x64)
// ---------------------------------------------------------------------------
struct HookBinding {
    const char* symbol_name;
    void* replacement;
    void** real_slot;
};

void* ResolveHookForSymbol(const char* name) {
    if (!name) {
        return nullptr;
    }
    static const HookBinding kBindings[] = {
        {"LoadLibraryA", reinterpret_cast<void*>(&Hook_LoadLibraryA),
         reinterpret_cast<void**>(&g_real_LoadLibraryA)},
        {"LoadLibraryW", reinterpret_cast<void*>(&Hook_LoadLibraryW),
         reinterpret_cast<void**>(&g_real_LoadLibraryW)},
        {"LoadLibraryExA", reinterpret_cast<void*>(&Hook_LoadLibraryExA),
         reinterpret_cast<void**>(&g_real_LoadLibraryExA)},
        {"LoadLibraryExW", reinterpret_cast<void*>(&Hook_LoadLibraryExW),
         reinterpret_cast<void**>(&g_real_LoadLibraryExW)},
        {"GetModuleHandleA", reinterpret_cast<void*>(&Hook_GetModuleHandleA),
         reinterpret_cast<void**>(&g_real_GetModuleHandleA)},
        {"GetModuleHandleW", reinterpret_cast<void*>(&Hook_GetModuleHandleW),
         reinterpret_cast<void**>(&g_real_GetModuleHandleW)},
        {"GetModuleHandleExA", reinterpret_cast<void*>(&Hook_GetModuleHandleExA),
         reinterpret_cast<void**>(&g_real_GetModuleHandleExA)},
        {"GetModuleHandleExW", reinterpret_cast<void*>(&Hook_GetModuleHandleExW),
         reinterpret_cast<void**>(&g_real_GetModuleHandleExW)},
        {"GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
         reinterpret_cast<void**>(&g_real_GetProcAddress)},
        {"CreateFileA", reinterpret_cast<void*>(&Hook_CreateFileA),
         reinterpret_cast<void**>(&g_real_CreateFileA)},
        {"CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW),
         reinterpret_cast<void**>(&g_real_CreateFileW)},
        {"GetFileAttributesA", reinterpret_cast<void*>(&Hook_GetFileAttributesA),
         reinterpret_cast<void**>(&g_real_GetFileAttributesA)},
        {"GetFileAttributesW", reinterpret_cast<void*>(&Hook_GetFileAttributesW),
         reinterpret_cast<void**>(&g_real_GetFileAttributesW)},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&Hook_GetFileAttributesExW),
         reinterpret_cast<void**>(&g_real_GetFileAttributesExW)},
        {"CreateDirectoryW", reinterpret_cast<void*>(&Hook_CreateDirectoryW),
         reinterpret_cast<void**>(&g_real_CreateDirectoryW)},
        {"FindFirstFileW", reinterpret_cast<void*>(&Hook_FindFirstFileW),
         reinterpret_cast<void**>(&g_real_FindFirstFileW)},
        {"ExitProcess", reinterpret_cast<void*>(&Hook_ExitProcess),
         reinterpret_cast<void**>(&g_real_ExitProcess)},
    };
    for (const auto& b : kBindings) {
        if (std::strcmp(name, b.symbol_name) == 0) {
            return (*b.real_slot != nullptr) ? b.replacement : nullptr;
        }
    }
    return nullptr;
}

void* ResolveHookForRealAddress(ULONGLONG addr) {
    if (addr == 0) {
        return nullptr;
    }
    auto match = [addr](auto real_fn, auto hook_fn) -> void* {
        if (real_fn != nullptr && reinterpret_cast<ULONGLONG>(real_fn) == addr) {
            return reinterpret_cast<void*>(hook_fn);
        }
        return nullptr;
    };
    if (void* p = match(g_real_LoadLibraryA, &Hook_LoadLibraryA)) return p;
    if (void* p = match(g_real_LoadLibraryW, &Hook_LoadLibraryW)) return p;
    if (void* p = match(g_real_LoadLibraryExA, &Hook_LoadLibraryExA)) return p;
    if (void* p = match(g_real_LoadLibraryExW, &Hook_LoadLibraryExW)) return p;
    if (void* p = match(g_real_GetModuleHandleA, &Hook_GetModuleHandleA)) return p;
    if (void* p = match(g_real_GetModuleHandleW, &Hook_GetModuleHandleW)) return p;
    if (void* p = match(g_real_GetModuleHandleExA, &Hook_GetModuleHandleExA)) return p;
    if (void* p = match(g_real_GetModuleHandleExW, &Hook_GetModuleHandleExW)) return p;
    if (void* p = match(g_real_GetProcAddress, &Hook_GetProcAddress)) return p;
    if (void* p = match(g_real_CreateFileA, &Hook_CreateFileA)) return p;
    if (void* p = match(g_real_CreateFileW, &Hook_CreateFileW)) return p;
    if (void* p = match(g_real_GetFileAttributesA, &Hook_GetFileAttributesA)) return p;
    if (void* p = match(g_real_GetFileAttributesW, &Hook_GetFileAttributesW)) return p;
    if (void* p = match(g_real_GetFileAttributesExW, &Hook_GetFileAttributesExW)) return p;
    if (void* p = match(g_real_CreateDirectoryW, &Hook_CreateDirectoryW)) return p;
    if (void* p = match(g_real_FindFirstFileW, &Hook_FindFirstFileW)) return p;
    if (void* p = match(g_real_ExitProcess, &Hook_ExitProcess)) return p;
    return nullptr;
}

bool WriteIatSlot(ULONGLONG* slot, void* replacement) {
    if (!slot || !replacement) {
        return false;
    }
    const ULONGLONG target = reinterpret_cast<ULONGLONG>(replacement);
    if (*slot == target) {
        return true;
    }
    DWORD old_prot = 0;
    if (!::VirtualProtect(slot, sizeof(ULONGLONG), PAGE_READWRITE, &old_prot)) {
        return false;
    }
    *slot = target;
    DWORD dummy = 0;
    ::VirtualProtect(slot, sizeof(ULONGLONG), old_prot, &dummy);
    return true;
}

void PatchThunkTable(uint8_t* base,
                     uint32_t image_size,
                     uint32_t int_rva,
                     uint32_t iat_rva) {
    if (iat_rva == 0 || iat_rva >= image_size) {
        return;
    }
    auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + iat_rva);
    auto* orig = (int_rva != 0 && int_rva < image_size)
                     ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + int_rva)
                     : nullptr;

    for (size_t i = 0; i < 16384; ++i) {
        if (iat_rva + (i + 1) * sizeof(IMAGE_THUNK_DATA64) > image_size) {
            break;
        }
        if (iat[i].u1.Function == 0) {
            break;
        }

        void* replacement = nullptr;
        if (orig != nullptr && int_rva + (i + 1) * sizeof(IMAGE_THUNK_DATA64) <= image_size) {
            const ULONGLONG ord_val = orig[i].u1.Ordinal;
            if (ord_val == 0) {
                break;
            }
            if ((ord_val & IMAGE_ORDINAL_FLAG64) == 0) {
                const uint32_t name_rva = static_cast<uint32_t>(orig[i].u1.AddressOfData);
                if (name_rva + sizeof(WORD) + 1 < image_size) {
                    const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_rva);
                    replacement = ResolveHookForSymbol(ibn->Name);
                }
            }
        }
        if (!replacement) {
            replacement = ResolveHookForRealAddress(iat[i].u1.Function);
        }
        if (replacement) {
            WriteIatSlot(&iat[i].u1.Function, replacement);
        }
    }
}

void PatchModuleImports(HMODULE mod) {
    if (!mod || IsWindowsSystemModule(mod)) {
        return;
    }

    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return;
    }

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return;
    }

    const uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    const uint32_t timestamp = nt->FileHeader.TimeDateStamp;

    ::AcquireSRWLockExclusive(&g_patch_lock);
    auto it = g_patched_modules.find(mod);
    if (it != g_patched_modules.end() &&
        it->second.size_of_image == image_size &&
        it->second.timestamp == timestamp) {
        ::ReleaseSRWLockExclusive(&g_patch_lock);
        return;
    }

    // 1. Standard Import Address Table (IMAGE_DIRECTORY_ENTRY_IMPORT)
    const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.VirtualAddress != 0 && imp_dir.VirtualAddress < image_size) {
        auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
        for (size_t idx = 0; idx < 2048; ++idx) {
            if (imp_dir.VirtualAddress + (idx + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR) > image_size) {
                break;
            }
            const auto& d = desc[idx];
            if (d.OriginalFirstThunk == 0 && d.Name == 0 && d.FirstThunk == 0) {
                break;
            }
            if (d.Name == 0 || d.Name >= image_size) {
                continue;
            }
            const char* dll_name = reinterpret_cast<const char*>(base + d.Name);
            if (!IsHookableImportDll(dll_name)) {
                continue;
            }
            PatchThunkTable(base, image_size, d.OriginalFirstThunk, d.FirstThunk);
        }
    }

    // 2. Delay-Load Import Table (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
    const auto& dly_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dly_dir.VirtualAddress != 0 && dly_dir.VirtualAddress < image_size) {
        auto* desc = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dly_dir.VirtualAddress);
        for (size_t idx = 0; idx < 1024; ++idx) {
            if (dly_dir.VirtualAddress + (idx + 1) * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) > image_size) {
                break;
            }
            const auto& d = desc[idx];
            if (d.DllNameRVA == 0 && d.ImportAddressTableRVA == 0) {
                break;
            }
            if (d.Attributes.RvaBased == 0) {
                continue;
            }
            if (d.DllNameRVA == 0 || d.DllNameRVA >= image_size) {
                continue;
            }
            const char* dll_name = reinterpret_cast<const char*>(base + d.DllNameRVA);
            if (!IsHookableImportDll(dll_name)) {
                continue;
            }
            PatchThunkTable(base, image_size, d.ImportNameTableRVA, d.ImportAddressTableRVA);
        }
    }

    g_patched_modules[mod] = PatchedModuleKey{mod, image_size, timestamp};
    ::ReleaseSRWLockExclusive(&g_patch_lock);
}

void PatchAllLoadedNonSystemModules() {
    std::vector<HMODULE> mods(256);
    DWORD bytes_needed = 0;
    if (::EnumProcessModules(::GetCurrentProcess(),
                             mods.data(),
                             static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                             &bytes_needed)) {
        size_t count = bytes_needed / sizeof(HMODULE);
        if (count > mods.size()) {
            mods.resize(count);
            if (::EnumProcessModules(::GetCurrentProcess(),
                                     mods.data(),
                                     static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                                     &bytes_needed)) {
                count = std::min(mods.size(), static_cast<size_t>(bytes_needed / sizeof(HMODULE)));
            } else {
                count = 0;
            }
        }
        mods.resize(count);
    } else {
        mods.clear();
    }

    HMODULE main_exe = g_real_GetModuleHandleW ? g_real_GetModuleHandleW(nullptr) : ::GetModuleHandleW(nullptr);
    if (main_exe != nullptr && std::find(mods.begin(), mods.end(), main_exe) == mods.end()) {
        mods.push_back(main_exe);
    }

    for (HMODULE mod : mods) {
        if (!mod || mod == g_self_module) {
            continue;
        }
        std::wstring path = GetModulePathW(mod);
        std::wstring canon_w;
        std::string canon_a;
        if (MatchXboxShimmedDllW(path, canon_w, canon_a)) {
            RegisterXboxModule(mod, canon_a);
        }
        PatchModuleImports(mod);
    }
}

bool InitializeHookRuntime(HMODULE self_mod) {
    g_self_module = self_mod;

    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        return false;
    }

    g_real_LoadLibraryA = reinterpret_cast<PfnLoadLibraryA>(::GetProcAddress(k32, "LoadLibraryA"));
    g_real_LoadLibraryW = reinterpret_cast<PfnLoadLibraryW>(::GetProcAddress(k32, "LoadLibraryW"));
    g_real_LoadLibraryExA = reinterpret_cast<PfnLoadLibraryExA>(::GetProcAddress(k32, "LoadLibraryExA"));
    g_real_LoadLibraryExW = reinterpret_cast<PfnLoadLibraryExW>(::GetProcAddress(k32, "LoadLibraryExW"));
    g_real_GetModuleHandleA = reinterpret_cast<PfnGetModuleHandleA>(::GetProcAddress(k32, "GetModuleHandleA"));
    g_real_GetModuleHandleW = reinterpret_cast<PfnGetModuleHandleW>(::GetProcAddress(k32, "GetModuleHandleW"));
    g_real_GetModuleHandleExA = reinterpret_cast<PfnGetModuleHandleExA>(::GetProcAddress(k32, "GetModuleHandleExA"));
    g_real_GetModuleHandleExW = reinterpret_cast<PfnGetModuleHandleExW>(::GetProcAddress(k32, "GetModuleHandleExW"));
    g_real_GetProcAddress = reinterpret_cast<PfnGetProcAddress>(::GetProcAddress(k32, "GetProcAddress"));
    g_real_CreateFileA = reinterpret_cast<PfnCreateFileA>(::GetProcAddress(k32, "CreateFileA"));
    g_real_CreateFileW = reinterpret_cast<PfnCreateFileW>(::GetProcAddress(k32, "CreateFileW"));
    g_real_GetFileAttributesA = reinterpret_cast<PfnGetFileAttributesA>(::GetProcAddress(k32, "GetFileAttributesA"));
    g_real_GetFileAttributesW = reinterpret_cast<PfnGetFileAttributesW>(::GetProcAddress(k32, "GetFileAttributesW"));
    g_real_GetFileAttributesExW = reinterpret_cast<PfnGetFileAttributesExW>(::GetProcAddress(k32, "GetFileAttributesExW"));
    g_real_CreateDirectoryW = reinterpret_cast<PfnCreateDirectoryW>(::GetProcAddress(k32, "CreateDirectoryW"));
    g_real_FindFirstFileW = reinterpret_cast<PfnFindFirstFileW>(::GetProcAddress(k32, "FindFirstFileW"));
    g_real_ExitProcess = reinterpret_cast<PfnExitProcess>(::GetProcAddress(k32, "ExitProcess"));

    // Resolve %SystemRoot% for Windows system DLL exclusion
    wchar_t win_dir[MAX_PATH]{};
    UINT win_len = ::GetSystemWindowsDirectoryW(win_dir, MAX_PATH);
    if (win_len > 0 && win_len < MAX_PATH) {
        g_windows_dir_lower_w = ToLowerAsciiW(std::wstring_view(win_dir, win_len));
        while (!g_windows_dir_lower_w.empty() && g_windows_dir_lower_w.back() == L'\\') {
            g_windows_dir_lower_w.pop_back();
        }
    }

    // Parse OpenScarlett environment variables
    g_shim_dir_w = ReadEnvW(L"OPENSCARLETT_SHIM_DIR");
    if (g_shim_dir_w.empty()) {
        std::wstring self_path = GetModulePathW(self_mod);
        g_shim_dir_w = std::filesystem::path(self_path).parent_path().wstring();
    }
    while (g_shim_dir_w.size() > 3 && (g_shim_dir_w.back() == L'\\' || g_shim_dir_w.back() == L'/')) {
        g_shim_dir_w.pop_back();
    }

    std::wstring tel_w = ReadEnvW(L"OPENSCARLETT_TELEMETRY_PATH");
    if (!tel_w.empty()) {
        g_telemetry_path_utf8 = openscarlett::WideToUtf8(tel_w);
    }

    std::wstring main_exe_path = GetModulePathW(nullptr);
    std::wstring main_exe_dir = std::filesystem::path(main_exe_path).parent_path().wstring();
    g_vfs.Configure(openscarlett::VfsMounts::BuildDefaultConfig(main_exe_dir));

    PatchAllLoadedNonSystemModules();
    g_initialized.store(true, std::memory_order_release);
    return true;
}

}  // namespace

extern "C" {

__declspec(dllexport) void __cdecl OpenScarlettHook_RecordCall(const char* module_name,
                                                               const char* symbol_name,
                                                               uint8_t outcome,
                                                               int32_t hr,
                                                               int32_t vtable_slot) {
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return;
    }
    const uint8_t clamped = (outcome <= 3u) ? outcome : 3u;
    openscarlett::CallTelemetry::Global().Record(
        module_name ? module_name : "",
        symbol_name ? symbol_name : "",
        static_cast<openscarlett::CallOutcome>(clamped),
        hr,
        vtable_slot);
}

__declspec(dllexport) int __cdecl OpenScarlettHook_FlushTelemetry() {
    return FlushTelemetryInternal(false);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            ::DisableThreadLibraryCalls(hinstDLL);
            InitializeHookRuntime(hinstDLL);
            return TRUE;
        case DLL_PROCESS_DETACH:
            FlushTelemetryInternal(true);
            g_shutting_down.store(true, std::memory_order_release);
            return TRUE;
        default:
            return TRUE;
    }
}
