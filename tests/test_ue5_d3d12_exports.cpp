#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "os_check.h"

namespace {

#if defined(_WIN32)

HMODULE LoadD3D12XsModule() {
    wchar_t exe_path[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(
        nullptr, exe_path, static_cast<DWORD>(sizeof(exe_path) / sizeof(exe_path[0]) - 1u));
    if (len > 0u) {
        const std::filesystem::path candidate =
            std::filesystem::path(exe_path).parent_path() / L"d3d12_xs.dll";
        if (HMODULE mod = LoadLibraryW(candidate.c_str())) {
            return mod;
        }
    }
#if defined(OPENSCARLETT_BINARY_DIR)
    {
        const std::filesystem::path bin_candidate =
            std::filesystem::path(OPENSCARLETT_BINARY_DIR) / "d3d12_xs.dll";
        if (HMODULE mod = LoadLibraryW(bin_candidate.c_str())) {
            return mod;
        }
    }
#endif
    return LoadLibraryW(L"d3d12_xs.dll");
}

template <typename Iface>
void VerifyExportedGuid(HMODULE mod, const char* symbol_name, size_t& verified_count) {
    FARPROC proc = GetProcAddress(mod, symbol_name);
    if (!proc) {
        std::fprintf(stderr, "Missing exported GUID symbol: %s\n", symbol_name);
        OS_REQUIRE(proc != nullptr);
    }
    const auto* exported = reinterpret_cast<const GUID*>(proc);
    const GUID expected = __uuidof(Iface);
    if (std::memcmp(exported, &expected, sizeof(GUID)) != 0) {
        std::fprintf(stderr, "GUID byte mismatch for %s\n", symbol_name);
        OS_REQUIRE(std::memcmp(exported, &expected, sizeof(GUID)) == 0);
    }
    ++verified_count;
}

template <typename FnPtr>
FnPtr ResolveRequiredProc(HMODULE mod, const char* name) {
    FARPROC p = GetProcAddress(mod, name);
    if (!p) {
        std::fprintf(stderr, "Missing exported function: %s\n", name);
        OS_REQUIRE(p != nullptr);
    }
    return reinterpret_cast<FnPtr>(p);
}

using PfnD3D12XboxCreateDevice = HRESULT(WINAPI*)(
    IUnknown*, const D3D12XBOX_CREATE_DEVICE_PARAMETERS*, REFIID, void**);
using PfnD3D12CreateDeviceX = HRESULT(WINAPI*)(
    IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnSetPreferWarp = void(WINAPI*)(int);
using PfnResetTelemetry = void(WINAPI*)();
using PfnGetTelemetryEntry = int(WINAPI*)(const char*, int32_t*, uint64_t*);
using PfnSetProcessGpuPriority = HRESULT(WINAPI*)(D3D12XBOX_PROCESS_GPU_PRIORITY);
using PfnGetProcessGpuPriority = HRESULT(WINAPI*)(D3D12XBOX_PROCESS_GPU_PRIORITY*);
using PfnSetProcessMemoryPriority = HRESULT(WINAPI*)(D3D12XBOX_PROCESS_MEMORY_PRIORITY);
using PfnGetProcessMemoryPriority = HRESULT(WINAPI*)(D3D12XBOX_PROCESS_MEMORY_PRIORITY*);
using PfnGetDevice = HRESULT(WINAPI*)(REFIID, void**);
using PfnSetFrameInterval = HRESULT(WINAPI*)(IDXGIOutput*, uint32_t, uint32_t, uint32_t);
using PfnReportError = HRESULT(WINAPI*)(HRESULT, const char*);
using PfnSetGpuTimeLimit = HRESULT(WINAPI*)(uint32_t);
using PfnGetDisplayMode = HRESULT(WINAPI*)(D3D12XBOX_DISPLAY_MODE*);
using PfnGetFrameIntervalState = HRESULT(WINAPI*)(D3D12XBOX_FRAME_INTERVAL_STATE*);
using PfnGetGpuTimeLimit = uint32_t(WINAPI*)();
using PfnGetReportedErrorCount = uint64_t(WINAPI*)(HRESULT*);
using PfnResetTrackedDevice = void(WINAPI*)();

void VerifyAllGuidExports(HMODULE mod) {
    size_t verified = 0u;

    // Base interfaces (3)
    VerifyExportedGuid<ID3D12Object>(mod, "IID_ID3D12Object", verified);
    VerifyExportedGuid<ID3D12DeviceChild>(mod, "IID_ID3D12DeviceChild", verified);
    VerifyExportedGuid<ID3D12Pageable>(mod, "IID_ID3D12Pageable", verified);

    // Devices (11)
    VerifyExportedGuid<ID3D12Device>(mod, "IID_ID3D12Device", verified);
    VerifyExportedGuid<ID3D12Device1>(mod, "IID_ID3D12Device1", verified);
    VerifyExportedGuid<ID3D12Device2>(mod, "IID_ID3D12Device2", verified);
    VerifyExportedGuid<ID3D12Device3>(mod, "IID_ID3D12Device3", verified);
    VerifyExportedGuid<ID3D12Device4>(mod, "IID_ID3D12Device4", verified);
    VerifyExportedGuid<ID3D12Device5>(mod, "IID_ID3D12Device5", verified);
    VerifyExportedGuid<ID3D12Device6>(mod, "IID_ID3D12Device6", verified);
    VerifyExportedGuid<ID3D12Device7>(mod, "IID_ID3D12Device7", verified);
    VerifyExportedGuid<ID3D12Device8>(mod, "IID_ID3D12Device8", verified);
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    VerifyExportedGuid<ID3D12Device9>(mod, "IID_ID3D12Device9", verified);
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    VerifyExportedGuid<ID3D12Device10>(mod, "IID_ID3D12Device10", verified);
#endif

    // Command lists (9)
    VerifyExportedGuid<ID3D12CommandList>(mod, "IID_ID3D12CommandList", verified);
    VerifyExportedGuid<ID3D12GraphicsCommandList>(mod, "IID_ID3D12GraphicsCommandList", verified);
    VerifyExportedGuid<ID3D12GraphicsCommandList1>(mod, "IID_ID3D12GraphicsCommandList1", verified);
    VerifyExportedGuid<ID3D12GraphicsCommandList2>(mod, "IID_ID3D12GraphicsCommandList2", verified);
    VerifyExportedGuid<ID3D12GraphicsCommandList3>(mod, "IID_ID3D12GraphicsCommandList3", verified);
    VerifyExportedGuid<ID3D12GraphicsCommandList4>(mod, "IID_ID3D12GraphicsCommandList4", verified);
    VerifyExportedGuid<ID3D12GraphicsCommandList5>(mod, "IID_ID3D12GraphicsCommandList5", verified);
    VerifyExportedGuid<ID3D12GraphicsCommandList6>(mod, "IID_ID3D12GraphicsCommandList6", verified);
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    VerifyExportedGuid<ID3D12GraphicsCommandList7>(mod, "IID_ID3D12GraphicsCommandList7", verified);
#endif

    // Queue / Resource / Heap / Fence / PSO / RootSig / Descriptor / Allocator / Query / State / Debug (19)
    VerifyExportedGuid<ID3D12CommandQueue>(mod, "IID_ID3D12CommandQueue", verified);
    VerifyExportedGuid<ID3D12Resource>(mod, "IID_ID3D12Resource", verified);
    VerifyExportedGuid<ID3D12Resource1>(mod, "IID_ID3D12Resource1", verified);
    VerifyExportedGuid<ID3D12Resource2>(mod, "IID_ID3D12Resource2", verified);
    VerifyExportedGuid<ID3D12Heap>(mod, "IID_ID3D12Heap", verified);
    VerifyExportedGuid<ID3D12Heap1>(mod, "IID_ID3D12Heap1", verified);
    VerifyExportedGuid<ID3D12Fence>(mod, "IID_ID3D12Fence", verified);
    VerifyExportedGuid<ID3D12Fence1>(mod, "IID_ID3D12Fence1", verified);
    VerifyExportedGuid<ID3D12PipelineState>(mod, "IID_ID3D12PipelineState", verified);
    VerifyExportedGuid<ID3D12RootSignature>(mod, "IID_ID3D12RootSignature", verified);
    VerifyExportedGuid<ID3D12DescriptorHeap>(mod, "IID_ID3D12DescriptorHeap", verified);
    VerifyExportedGuid<ID3D12CommandAllocator>(mod, "IID_ID3D12CommandAllocator", verified);
    VerifyExportedGuid<ID3D12QueryHeap>(mod, "IID_ID3D12QueryHeap", verified);
    VerifyExportedGuid<ID3D12CommandSignature>(mod, "IID_ID3D12CommandSignature", verified);
    VerifyExportedGuid<ID3D12StateObject>(mod, "IID_ID3D12StateObject", verified);
    VerifyExportedGuid<ID3D12StateObjectProperties>(mod, "IID_ID3D12StateObjectProperties", verified);
    VerifyExportedGuid<ID3D12Debug>(mod, "IID_ID3D12Debug", verified);
    VerifyExportedGuid<ID3D12Debug1>(mod, "IID_ID3D12Debug1", verified);
    VerifyExportedGuid<ID3D12InfoQueue>(mod, "IID_ID3D12InfoQueue", verified);

    OS_REQUIRE(verified >= 39u);
    std::printf("[test_ue5_d3d12_exports] Verified %zu exported IID_ID3D12* GUID symbols (byte-exact)\n",
                verified);
}

void VerifyXboxProcessAndDeviceHelpers(HMODULE mod) {
    auto fn_create_xbox = ResolveRequiredProc<PfnD3D12XboxCreateDevice>(mod, "D3D12XboxCreateDevice");
    auto fn_create_x = ResolveRequiredProc<PfnD3D12CreateDeviceX>(mod, "D3D12CreateDeviceX");
    auto fn_set_warp = ResolveRequiredProc<PfnSetPreferWarp>(mod, "OpenScarlettD3D12SetPreferWarp");
    auto fn_reset_tel = ResolveRequiredProc<PfnResetTelemetry>(mod, "OpenScarlettD3D12ResetTelemetry");
    auto fn_get_tel = ResolveRequiredProc<PfnGetTelemetryEntry>(mod, "OpenScarlettD3D12GetTelemetryEntry");
    auto fn_set_gpu_prio = ResolveRequiredProc<PfnSetProcessGpuPriority>(
        mod, "D3D12XboxSetProcessGpuPriority");
    auto fn_get_gpu_prio = ResolveRequiredProc<PfnGetProcessGpuPriority>(
        mod, "D3D12XboxGetProcessGpuPriority");
    auto fn_set_mem_prio = ResolveRequiredProc<PfnSetProcessMemoryPriority>(
        mod, "D3D12XboxSetProcessMemoryPriority");
    auto fn_get_mem_prio = ResolveRequiredProc<PfnGetProcessMemoryPriority>(
        mod, "D3D12XboxGetProcessMemoryPriority");
    auto fn_get_device = ResolveRequiredProc<PfnGetDevice>(mod, "D3D12XboxGetDevice");
    auto fn_set_interval = ResolveRequiredProc<PfnSetFrameInterval>(mod, "D3D12XboxSetFrameInterval");
    auto fn_report_error = ResolveRequiredProc<PfnReportError>(mod, "D3D12XboxReportError");
    auto fn_set_time_limit = ResolveRequiredProc<PfnSetGpuTimeLimit>(mod, "D3D12XboxSetGpuTimeLimit");
    auto fn_get_disp_mode = ResolveRequiredProc<PfnGetDisplayMode>(mod, "D3D12XboxGetDisplayMode");
    auto fn_get_interval_state = ResolveRequiredProc<PfnGetFrameIntervalState>(
        mod, "OpenScarlettD3D12GetFrameIntervalState");
    auto fn_get_time_limit = ResolveRequiredProc<PfnGetGpuTimeLimit>(
        mod, "OpenScarlettD3D12GetGpuTimeLimit");
    auto fn_get_err_count = ResolveRequiredProc<PfnGetReportedErrorCount>(
        mod, "OpenScarlettD3D12GetReportedErrorCount");
    auto fn_reset_dev = ResolveRequiredProc<PfnResetTrackedDevice>(
        mod, "OpenScarlettD3D12ResetTrackedDevice");

    fn_reset_tel();
    fn_reset_dev();

    // 1. D3D12XboxGetDevice before any device creation -> DXGI_ERROR_NOT_FOUND, null ppDevice -> E_POINTER.
    OS_REQUIRE(fn_get_device(__uuidof(ID3D12Device), nullptr) == E_POINTER);
    ID3D12Device* missing_dev = reinterpret_cast<ID3D12Device*>(static_cast<uintptr_t>(0x1));
    OS_REQUIRE(fn_get_device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&missing_dev)) ==
               DXGI_ERROR_NOT_FOUND);
    OS_REQUIRE(missing_dev == nullptr);

    // 2. Process GPU priority validation + state persistence + thread safety.
    D3D12XBOX_PROCESS_GPU_PRIORITY gpu_prio = D3D12XBOX_PROCESS_GPU_PRIORITY_LOW;
    OS_REQUIRE(fn_get_gpu_prio(nullptr) == E_POINTER);
    OS_REQUIRE(fn_get_gpu_prio(&gpu_prio) == S_OK);
    OS_REQUIRE(gpu_prio == D3D12XBOX_PROCESS_GPU_PRIORITY_NORMAL);
    OS_REQUIRE(fn_set_gpu_prio(static_cast<D3D12XBOX_PROCESS_GPU_PRIORITY>(99u)) == E_INVALIDARG);
    OS_REQUIRE(fn_set_gpu_prio(D3D12XBOX_PROCESS_GPU_PRIORITY_HIGH) == S_OK);
    OS_REQUIRE(fn_get_gpu_prio(&gpu_prio) == S_OK);
    OS_REQUIRE(gpu_prio == D3D12XBOX_PROCESS_GPU_PRIORITY_HIGH);
    OS_REQUIRE(fn_set_gpu_prio(D3D12XBOX_PROCESS_GPU_PRIORITY_REALTIME) == S_OK);
    OS_REQUIRE(fn_get_gpu_prio(&gpu_prio) == S_OK);
    OS_REQUIRE(gpu_prio == D3D12XBOX_PROCESS_GPU_PRIORITY_REALTIME);

    // 3. Process memory priority validation + state persistence.
    D3D12XBOX_PROCESS_MEMORY_PRIORITY mem_prio = D3D12XBOX_PROCESS_MEMORY_PRIORITY_LOW;
    OS_REQUIRE(fn_get_mem_prio(nullptr) == E_POINTER);
    OS_REQUIRE(fn_get_mem_prio(&mem_prio) == S_OK);
    OS_REQUIRE(mem_prio == D3D12XBOX_PROCESS_MEMORY_PRIORITY_NORMAL);
    OS_REQUIRE(fn_set_mem_prio(static_cast<D3D12XBOX_PROCESS_MEMORY_PRIORITY>(42u)) == E_INVALIDARG);
    OS_REQUIRE(fn_set_mem_prio(D3D12XBOX_PROCESS_MEMORY_PRIORITY_HIGH) == S_OK);
    OS_REQUIRE(fn_get_mem_prio(&mem_prio) == S_OK);
    OS_REQUIRE(mem_prio == D3D12XBOX_PROCESS_MEMORY_PRIORITY_HIGH);

    // Concurrent priority updates to confirm lock-free atomic safety.
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < 64; ++i) {
                    const auto gp = static_cast<D3D12XBOX_PROCESS_GPU_PRIORITY>((t + i) % 4);
                    const auto mp = static_cast<D3D12XBOX_PROCESS_MEMORY_PRIORITY>((t + i) % 3);
                    OS_REQUIRE(fn_set_gpu_prio(gp) == S_OK);
                    OS_REQUIRE(fn_set_mem_prio(mp) == S_OK);
                    D3D12XBOX_PROCESS_GPU_PRIORITY rgp{};
                    D3D12XBOX_PROCESS_MEMORY_PRIORITY rmp{};
                    OS_REQUIRE(fn_get_gpu_prio(&rgp) == S_OK);
                    OS_REQUIRE(fn_get_mem_prio(&rmp) == S_OK);
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
    }

    // 4. D3D12XboxSetFrameInterval + D3D12XboxGetDisplayMode + D3D12XboxSetGpuTimeLimit.
    OS_REQUIRE(fn_set_interval(nullptr, 8333u, 99u, 0u) == E_INVALIDARG);
    OS_REQUIRE(fn_set_interval(nullptr, 8333u, 3u, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE) == S_OK);
    D3D12XBOX_FRAME_INTERVAL_STATE interval_state{};
    OS_REQUIRE(fn_get_interval_state(&interval_state) == S_OK);
    OS_REQUIRE(interval_state.PeriodMicroseconds == 8333u);
    OS_REQUIRE(interval_state.MaxFrameLatency == 3u);
    OS_REQUIRE(interval_state.UpdateCount >= 1ull);

    OS_REQUIRE(fn_set_time_limit(5000u) == S_OK);
    OS_REQUIRE(fn_get_time_limit() == 5000u);

    OS_REQUIRE(fn_get_disp_mode(nullptr) == E_POINTER);
    D3D12XBOX_DISPLAY_MODE mode{};
    OS_REQUIRE(fn_get_disp_mode(&mode) == S_OK);
    OS_REQUIRE(mode.Width >= 320u && mode.Height >= 240u);
    OS_REQUIRE(mode.RefreshRateNumerator == 120u);
    OS_REQUIRE(mode.RefreshRateDenominator == 1u);

    // 5. D3D12XboxReportError.
    OS_REQUIRE(fn_report_error(DXGI_ERROR_DEVICE_HUNG, "UE5 test GPU hang diagnostic") == S_OK);
    OS_REQUIRE(fn_report_error(E_FAIL, nullptr) == S_OK);
    HRESULT last_err = S_OK;
    const uint64_t err_count = fn_get_err_count(&last_err);
    OS_REQUIRE(err_count >= 2ull);
    OS_REQUIRE(last_err == E_FAIL);

    // 6. Live D3D12XboxCreateDevice + D3D12XboxGetDevice (WARP & Hardware) using exported IIDs.
    const auto* exported_iid_dev =
        reinterpret_cast<const GUID*>(GetProcAddress(mod, "IID_ID3D12Device"));
    const auto* exported_iid_dev8 =
        reinterpret_cast<const GUID*>(GetProcAddress(mod, "IID_ID3D12Device8"));
    OS_REQUIRE(exported_iid_dev != nullptr && exported_iid_dev8 != nullptr);

    for (int prefer_warp : {1, 0}) {
        fn_set_warp(prefer_warp);

        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1u;
        ID3D12Device* created_dev = nullptr;
        const HRESULT hr_create = fn_create_xbox(
            nullptr, &params, *exported_iid_dev, reinterpret_cast<void**>(&created_dev));
        if (prefer_warp == 1) {
            OS_REQUIRE(SUCCEEDED(hr_create) && created_dev != nullptr);
        } else if (FAILED(hr_create) || !created_dev) {
            continue;
        }

        // Retrieve the active device via D3D12XboxGetDevice using exported IID_ID3D12Device8.
        ID3D12Device8* fetched_dev8 = nullptr;
        OS_REQUIRE(SUCCEEDED(fn_get_device(*exported_iid_dev8, reinterpret_cast<void**>(&fetched_dev8))));
        OS_REQUIRE(fetched_dev8 != nullptr);
        OS_REQUIRE(static_cast<ID3D12Device*>(fetched_dev8) == created_dev);

        // Verify D3D12XboxSetFrameInterval propagates to the active DeviceX.
        OS_REQUIRE(fn_set_interval(nullptr, 16667u, 2u, 0u) == S_OK);

        // Releasing all caller references destroys DeviceX immediately (0 leaked refs)
        // and clears D3D12XboxGetDevice automatically via the private-data lifetime sentinel.
        OS_REQUIRE(fetched_dev8->Release() == 1u);
        OS_REQUIRE(created_dev->Release() == 0u);

        ID3D12Device* after_release = nullptr;
        OS_REQUIRE(fn_get_device(*exported_iid_dev, reinterpret_cast<void**>(&after_release)) ==
                   DXGI_ERROR_NOT_FOUND);
        OS_REQUIRE(after_release == nullptr);

        // Verify D3D12CreateDeviceX also registers with D3D12XboxGetDevice cleanly.
        ID3D12Device* created_dev_x = nullptr;
        OS_REQUIRE(SUCCEEDED(fn_create_x(
            nullptr, D3D_FEATURE_LEVEL_12_0, *exported_iid_dev,
            reinterpret_cast<void**>(&created_dev_x))));
        OS_REQUIRE(created_dev_x != nullptr);

        ID3D12Device* fetched_after_x = nullptr;
        OS_REQUIRE(SUCCEEDED(fn_get_device(*exported_iid_dev, reinterpret_cast<void**>(&fetched_after_x))));
        OS_REQUIRE(fetched_after_x == created_dev_x);

        OS_REQUIRE(fetched_after_x->Release() == 1u);
        OS_REQUIRE(created_dev_x->Release() == 0u);

        // Verify explicit reset helper is idempotent and leaves state clean.
        fn_reset_dev();
        ID3D12Device* after_reset = nullptr;
        OS_REQUIRE(fn_get_device(*exported_iid_dev, reinterpret_cast<void**>(&after_reset)) ==
                   DXGI_ERROR_NOT_FOUND);
        OS_REQUIRE(after_reset == nullptr);
    }

    // 7. Verify telemetry recorded calls for all D3D12Xbox* helpers.
    const char* expected_telemetry[] = {
        "D3D12XboxSetProcessGpuPriority",
        "D3D12XboxGetProcessGpuPriority",
        "D3D12XboxSetProcessMemoryPriority",
        "D3D12XboxGetProcessMemoryPriority",
        "D3D12XboxGetDevice",
        "D3D12XboxSetFrameInterval",
        "D3D12XboxReportError",
        "D3D12XboxSetGpuTimeLimit",
        "D3D12XboxGetDisplayMode",
    };
    for (const char* sym : expected_telemetry) {
        int32_t slot = -99;
        uint64_t calls = 0u;
        const int found = fn_get_tel(sym, &slot, &calls);
        OS_REQUIRE(found == 1);
        OS_REQUIRE(calls >= 1ull);
    }

    std::printf("[test_ue5_d3d12_exports] Verified all 9 D3D12Xbox* process/device helpers + telemetry\n");
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    HMODULE mod = LoadD3D12XsModule();
    OS_REQUIRE(mod != nullptr);

    VerifyAllGuidExports(mod);
    VerifyXboxProcessAndDeviceHelpers(mod);

    FreeLibrary(mod);
    std::printf("[test_ue5_d3d12_exports] PASS\n");
#endif
    return 0;
}
