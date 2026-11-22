// OpenScarlett MIT dxcompiler_xs.dll shim.
// Bridges Xbox Series X|S DXC shader compiler imports (DxcCreateInstance /
// DxcCreateInstance2) to the host Windows dxcompiler.dll with CallTelemetry.

#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <objidl.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kModName = "dxcompiler_xs.dll";

#if defined(_WIN32)

using PfnDxcCreateInstance = HRESULT(__stdcall*)(REFCLSID rclsid, REFIID riid, LPVOID* ppv);
using PfnDxcCreateInstance2 =
    HRESULT(__stdcall*)(IMalloc* pMalloc, REFCLSID rclsid, REFIID riid, LPVOID* ppv);

inline HRESULT RecordHr(const char* sym, HRESULT hr) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (FAILED(hr)) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, outcome, static_cast<int32_t>(hr), -1);
    return hr;
}

struct HostDxcState {
    std::mutex mutex;
    HMODULE module = nullptr;
    PfnDxcCreateInstance create_instance = nullptr;
    PfnDxcCreateInstance2 create_instance2 = nullptr;
    bool attempted = false;
};

HostDxcState& GetHostDxc() {
    static HostDxcState* s = new HostDxcState();
    return *s;
}

bool TryBindDxcModule(HMODULE mod, HostDxcState& st) {
    if (!mod) return false;
    auto fn1 = reinterpret_cast<PfnDxcCreateInstance>(
        GetProcAddress(mod, "DxcCreateInstance"));
    if (!fn1) {
        return false;
    }
    auto fn2 = reinterpret_cast<PfnDxcCreateInstance2>(
        GetProcAddress(mod, "DxcCreateInstance2"));
    st.module = mod;
    st.create_instance = fn1;
    st.create_instance2 = fn2;
    return true;
}

bool TryLoadDxcFromPath(const fs::path& path, HostDxcState& st) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return false;
    }
    const std::wstring wpath = path.wstring();
    HMODULE mod = LoadLibraryExW(
        wpath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        mod = LoadLibraryW(wpath.c_str());
    }
    if (!mod) {
        return false;
    }
    if (TryBindDxcModule(mod, st)) {
        return true;
    }
    FreeLibrary(mod);
    return false;
}

std::wstring GetModuleDirectoryFromAddress(const void* addr) {
    HMODULE self_mod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(addr),
            &self_mod) ||
        !self_mod) {
        return {};
    }

    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(
            self_mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0u) {
            return {};
        }
        if (len < buf.size() - 1u) {
            buf.resize(len);
            return fs::path(buf).parent_path().wstring();
        }
        if (buf.size() >= 32768u) {
            return {};
        }
        buf.resize(buf.size() * 2u);
    }
}

void ResolveHostDxcLocked(HostDxcState& st) {
    if (st.create_instance != nullptr || st.attempted) {
        return;
    }
    st.attempted = true;

    // 1. Check sibling dxcompiler.dll next to dxcompiler_xs.dll.
    const std::wstring self_dir =
        GetModuleDirectoryFromAddress(reinterpret_cast<const void*>(&ResolveHostDxcLocked));
    if (!self_dir.empty()) {
        if (TryLoadDxcFromPath(fs::path(self_dir) / L"dxcompiler.dll", st)) {
            return;
        }
    }

    // 2. Standard Windows DLL search path (exe dir / PATH / System32).
    if (HMODULE mod = LoadLibraryW(L"dxcompiler.dll"); mod != nullptr) {
        if (TryBindDxcModule(mod, st)) {
            return;
        }
        FreeLibrary(mod);
    }

    // 3. Fallback: scan %ProgramFiles(x86)%\Windows Kits\10\bin\10.*\x64\dxcompiler.dll.
    std::wstring pf86 = L"C:\\Program Files (x86)";
    {
        wchar_t env_buf[1024]{};
        const DWORD env_len = GetEnvironmentVariableW(
            L"ProgramFiles(x86)", env_buf, static_cast<DWORD>(std::size(env_buf)));
        if (env_len > 0u && env_len < std::size(env_buf)) {
            pf86.assign(env_buf, env_len);
        }
    }

    const fs::path kits_bin = fs::path(pf86) / L"Windows Kits" / L"10" / L"bin";
    std::error_code ec;
    if (fs::exists(kits_bin, ec)) {
        std::vector<fs::path> sdk_dlls;
        for (const auto& entry : fs::directory_iterator(kits_bin, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"10.", 0) != 0) continue;
            const fs::path cand = entry.path() / L"x64" / L"dxcompiler.dll";
            if (fs::exists(cand, ec)) {
                sdk_dlls.push_back(cand);
            }
        }
        std::sort(sdk_dlls.begin(), sdk_dlls.end());
        for (auto it = sdk_dlls.rbegin(); it != sdk_dlls.rend(); ++it) {
            if (TryLoadDxcFromPath(*it, st)) {
                return;
            }
        }
    }
}

#endif  // _WIN32

}  // namespace

extern "C" {

#if defined(_WIN32)

__declspec(dllexport) HRESULT __stdcall DxcCreateInstance(
    REFCLSID rclsid,
    REFIID riid,
    LPVOID* ppv) {
    if (!ppv) {
        return RecordHr("DxcCreateInstance", E_POINTER);
    }
    *ppv = nullptr;

    PfnDxcCreateInstance fn = nullptr;
    {
        auto& st = GetHostDxc();
        std::lock_guard<std::mutex> lk(st.mutex);
        ResolveHostDxcLocked(st);
        fn = st.create_instance;
    }

    if (!fn) {
        return RecordHr("DxcCreateInstance", HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
    }

    const HRESULT hr = fn(rclsid, riid, ppv);
    return RecordHr("DxcCreateInstance", hr);
}

__declspec(dllexport) HRESULT __stdcall DxcCreateInstance2(
    IMalloc* pMalloc,
    REFCLSID rclsid,
    REFIID riid,
    LPVOID* ppv) {
    if (!ppv) {
        return RecordHr("DxcCreateInstance2", E_POINTER);
    }
    *ppv = nullptr;

    PfnDxcCreateInstance fn1 = nullptr;
    PfnDxcCreateInstance2 fn2 = nullptr;
    {
        auto& st = GetHostDxc();
        std::lock_guard<std::mutex> lk(st.mutex);
        ResolveHostDxcLocked(st);
        fn1 = st.create_instance;
        fn2 = st.create_instance2;
    }

    if (!fn1 && !fn2) {
        return RecordHr("DxcCreateInstance2", HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
    }

    if (fn2) {
        const HRESULT hr = fn2(pMalloc, rclsid, riid, ppv);
        return RecordHr("DxcCreateInstance2", hr);
    }

    if (pMalloc == nullptr && fn1 != nullptr) {
        const HRESULT hr = fn1(rclsid, riid, ppv);
        return RecordHr("DxcCreateInstance2", hr);
    }

    return RecordHr("DxcCreateInstance2", E_NOINTERFACE);
}

__declspec(dllexport) int __stdcall OpenScarlettDxcGetTelemetryDetail(
    const char* symbol_name,
    uint64_t* out_total_calls,
    int32_t* out_last_hr) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_total_calls) *out_total_calls = entry.total_calls;
            if (out_last_hr) *out_last_hr = entry.last_hr;
            return 1;
        }
    }
    if (out_total_calls) *out_total_calls = 0;
    if (out_last_hr) *out_last_hr = 0;
    return 0;
}

#endif  // _WIN32

}  // extern "C"
