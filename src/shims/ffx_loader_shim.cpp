#define OPENSCARLETT_BUILDING_FFX_LOADER 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"

#include <mutex>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "amd_fidelityfx_loader_xbox.dll";

using PfnFfxQuery = int32_t(OS_SHIM_CALL*)(FfxContext*, FfxQueryDescHeader*);
using PfnFfxCreateContext = int32_t(OS_SHIM_CALL*)(
    FfxContext*, FfxCreateContextDescHeader*, const FfxAllocationCallbacks*);
using PfnFfxDestroyContext = int32_t(OS_SHIM_CALL*)(
    FfxContext*, const FfxAllocationCallbacks*);
using PfnFfxConfigure = int32_t(OS_SHIM_CALL*)(FfxContext*, const FfxConfigureDescHeader*);
using PfnFfxDispatch = int32_t(OS_SHIM_CALL*)(FfxContext*, const FfxDispatchDescHeader*);

struct UpscalerApi2Table {
    PfnFfxQuery query = nullptr;
    PfnFfxCreateContext create_ctx = nullptr;
    PfnFfxDestroyContext destroy_ctx = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

const UpscalerApi2Table& ResolveUpscalerApi2() {
    static UpscalerApi2Table table{};
    static std::once_flag once;
    std::call_once(once, []() {
#if defined(_WIN32)
        // Load our sibling amd_fidelityfx_upscaler_xbox.dll from the same directory as this loader
        // DLL rather than statically importing an internal helper, avoiding STATUS_ENTRYPOINT_NOT_FOUND
        // when a guest title ships its own amd_fidelityfx_upscaler_xbox.dll in the executable directory.
        HMODULE mod = nullptr;
        HMODULE self = nullptr;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ResolveUpscalerApi2),
                &self) && self) {
            wchar_t path_buf[MAX_PATH * 2]{};
            const DWORD len = ::GetModuleFileNameW(
                self, path_buf, static_cast<DWORD>(sizeof(path_buf) / sizeof(wchar_t)));
            if (len > 0u && len < (sizeof(path_buf) / sizeof(wchar_t))) {
                std::wstring dir(path_buf, len);
                const size_t slash = dir.find_last_of(L"\\/");
                if (slash != std::wstring::npos) {
                    const std::wstring sibling =
                        dir.substr(0, slash + 1u) + L"amd_fidelityfx_upscaler_xbox.dll";
                    mod = ::LoadLibraryExW(sibling.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                }
            }
        }
        if (!mod) {
            mod = ::GetModuleHandleA("amd_fidelityfx_upscaler_xbox.dll");
        }
        if (!mod) {
            mod = ::LoadLibraryA("amd_fidelityfx_upscaler_xbox.dll");
        }
        if (mod) {
            table.query = reinterpret_cast<PfnFfxQuery>(::GetProcAddress(mod, "ffxQuery"));
            table.create_ctx = reinterpret_cast<PfnFfxCreateContext>(
                ::GetProcAddress(mod, "ffxCreateContext"));
            table.destroy_ctx = reinterpret_cast<PfnFfxDestroyContext>(
                ::GetProcAddress(mod, "ffxDestroyContext"));
            table.configure = reinterpret_cast<PfnFfxConfigure>(
                ::GetProcAddress(mod, "ffxConfigure"));
            table.dispatch = reinterpret_cast<PfnFfxDispatch>(
                ::GetProcAddress(mod, "ffxDispatch"));
        }
#endif
    });
    return table;
}

inline int32_t RecordRet(const char* sym, int32_t rc) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        rc == FFX_API_RETURN_OK ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        rc,
        -1);
    return rc;
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CALL ffxQuery(FfxContext* context, FfxQueryDescHeader* header) {
    const auto& api = ResolveUpscalerApi2();
    if (!api.query) {
        return RecordRet("ffxQuery", FFX_API_RETURN_NO_PROVIDER);
    }
    return RecordRet("ffxQuery", api.query(context, header));
}

int32_t OS_SHIM_CALL ffxCreateContext(
    FfxContext* context,
    FfxCreateContextDescHeader* desc,
    const FfxAllocationCallbacks* memCb) {
    const auto& api = ResolveUpscalerApi2();
    if (!api.create_ctx) {
        return RecordRet("ffxCreateContext", FFX_API_RETURN_NO_PROVIDER);
    }
    return RecordRet("ffxCreateContext", api.create_ctx(context, desc, memCb));
}

int32_t OS_SHIM_CALL ffxDestroyContext(
    FfxContext* context,
    const FfxAllocationCallbacks* memCb) {
    const auto& api = ResolveUpscalerApi2();
    if (!api.destroy_ctx) {
        return RecordRet("ffxDestroyContext", FFX_API_RETURN_NO_PROVIDER);
    }
    return RecordRet("ffxDestroyContext", api.destroy_ctx(context, memCb));
}

int32_t OS_SHIM_CALL ffxConfigure(
    FfxContext* context,
    const FfxConfigureDescHeader* desc) {
    const auto& api = ResolveUpscalerApi2();
    if (!api.configure) {
        return RecordRet("ffxConfigure", FFX_API_RETURN_NO_PROVIDER);
    }
    return RecordRet("ffxConfigure", api.configure(context, desc));
}

int32_t OS_SHIM_CALL ffxDispatch(
    FfxContext* context,
    const FfxDispatchDescHeader* desc) {
    const auto& api = ResolveUpscalerApi2();
    if (!api.dispatch) {
        return RecordRet("ffxDispatch", FFX_API_RETURN_NO_PROVIDER);
    }
    return RecordRet("ffxDispatch", api.dispatch(context, desc));
}

}  // extern "C"
