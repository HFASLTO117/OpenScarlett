#include "openscarlett/gpu/d3d12_adapter.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace openscarlett {

#if defined(_WIN32)

namespace {

template <typename T>
class LocalComPtr {
public:
    LocalComPtr() = default;
    ~LocalComPtr() { Reset(); }

    LocalComPtr(const LocalComPtr&) = delete;
    LocalComPtr& operator=(const LocalComPtr&) = delete;

    LocalComPtr(LocalComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    LocalComPtr& operator=(LocalComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }

    T* Detach() noexcept {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

using PfnD3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnD3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);

HMODULE LoadSystemD3D12Module() noexcept {
    static HMODULE cached = []() -> HMODULE {
        wchar_t sys_dir[MAX_PATH]{};
        UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
        if (len > 0 && len < MAX_PATH - 16) {
            std::wstring full_path(sys_dir, len);
            full_path += L"\\d3d12.dll";
            HMODULE mod = LoadLibraryW(full_path.c_str());
            if (mod) return mod;
        }
        return LoadLibraryW(L"d3d12.dll");
    }();
    return cached;
}

std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

uint32_t ProbeHighestShaderModel(ID3D12Device* device) noexcept {
    static constexpr D3D_SHADER_MODEL kCandidates[] = {
        static_cast<D3D_SHADER_MODEL>(0x67),  // D3D_SHADER_MODEL_6_7
        static_cast<D3D_SHADER_MODEL>(0x66),  // D3D_SHADER_MODEL_6_6
        D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4,
        D3D_SHADER_MODEL_6_3,
        D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1,
        D3D_SHADER_MODEL_6_0,
        D3D_SHADER_MODEL_5_1,
    };
    for (D3D_SHADER_MODEL candidate : kCandidates) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{};
        sm.HighestShaderModel = candidate;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            return static_cast<uint32_t>(sm.HighestShaderModel);
        }
    }
    return static_cast<uint32_t>(D3D_SHADER_MODEL_5_1);
}

uint32_t ProbeMaxFeatureLevel(ID3D12Device* device) noexcept {
    static constexpr D3D_FEATURE_LEVEL kLevels[] = {
        static_cast<D3D_FEATURE_LEVEL>(0xc200),  // D3D_FEATURE_LEVEL_12_2
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS data{};
    data.NumFeatureLevels = static_cast<UINT>(sizeof(kLevels) / sizeof(kLevels[0]));
    data.pFeatureLevelsRequested = kLevels;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data)))) {
        return static_cast<uint32_t>(data.MaxSupportedFeatureLevel);
    }
    return static_cast<uint32_t>(D3D_FEATURE_LEVEL_12_0);
}

bool IsSoftwareOrWarpAdapter(const DXGI_ADAPTER_DESC1& desc) noexcept {
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return true;
    // Microsoft Basic Render Driver / WARP (VID 0x1414, DID 0x008C) can enumerate
    // without DXGI_ADAPTER_FLAG_SOFTWARE on headless/Hyper-V Windows Server hosts.
    if (desc.VendorId == 0x1414u && (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0)) {
        return true;
    }
    return false;
}

bool TryCreateHostDevice(
    PfnD3D12CreateDevice create_fn,
    IDXGIAdapter1* adapter,
    D3D_FEATURE_LEVEL min_level,
    LocalComPtr<ID3D12Device>& out_dev) {
    if (!create_fn || !adapter) return false;
    if (SUCCEEDED(create_fn(adapter, min_level, IID_PPV_ARGS(out_dev.Put()))) && out_dev) {
        return true;
    }
    if (min_level > D3D_FEATURE_LEVEL_11_0) {
        if (SUCCEEDED(create_fn(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(out_dev.Put()))) && out_dev) {
            return true;
        }
    }
    return false;
}

}  // namespace

void* ResolveHostD3D12Proc(const char* proc_name) {
    if (!proc_name || !*proc_name) return nullptr;
    HMODULE mod = LoadSystemD3D12Module();
    if (!mod) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(mod, proc_name));
}

Status QueryD3D12DeviceAdapterInfo(
    ID3D12Device* device,
    IDXGIAdapter1* adapter,
    D3D12AdapterInfo& out_info) {
    out_info = D3D12AdapterInfo{};
    if (!device) {
        return Status::Error(StatusCode::InvalidArgument, "QueryD3D12DeviceAdapterInfo: null device");
    }

    LocalComPtr<IDXGIAdapter1> owned_adapter;
    IDXGIAdapter1* query_adapter = adapter;

    if (!query_adapter) {
        const LUID luid = device->GetAdapterLuid();
        LocalComPtr<IDXGIFactory4> factory4;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory4.Put()))) && factory4) {
            if (SUCCEEDED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(owned_adapter.Put()))) &&
                owned_adapter) {
                query_adapter = owned_adapter.Get();
            }
        }
    }

    if (query_adapter) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(query_adapter->GetDesc1(&desc))) {
            out_info.description = WideToUtf8(desc.Description);
            out_info.vendor_id = desc.VendorId;
            out_info.device_id = desc.DeviceId;
            out_info.subsys_id = desc.SubSysId;
            out_info.revision = desc.Revision;
            out_info.dedicated_video_memory_bytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            out_info.dedicated_video_memory_mb =
                static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ull * 1024ull);
            out_info.shared_system_memory_bytes = static_cast<uint64_t>(desc.SharedSystemMemory);
            out_info.luid_low = desc.AdapterLuid.LowPart;
            out_info.luid_high = desc.AdapterLuid.HighPart;
            out_info.is_warp = IsSoftwareOrWarpAdapter(desc);
        }
    } else {
        const LUID luid = device->GetAdapterLuid();
        out_info.luid_low = luid.LowPart;
        out_info.luid_high = luid.HighPart;
        out_info.description = "D3D12 Device";
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
        out_info.wave_lane_min = opt1.WaveLaneCountMin;
        out_info.wave_lane_max = opt1.WaveLaneCountMax;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) {
        out_info.raytracing_tier = static_cast<uint32_t>(opt5.RaytracingTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)))) {
        out_info.mesh_shader_tier = static_cast<uint32_t>(opt7.MeshShaderTier);
    }

    out_info.highest_shader_model = ProbeHighestShaderModel(device);
    out_info.feature_level = ProbeMaxFeatureLevel(device);
    return Status::Ok();
}

Status SelectD3D12AdapterAndCreateDevice(
    const D3D12AdapterSelectionOptions& options,
    ID3D12Device** out_device,
    IDXGIAdapter1** out_adapter,
    D3D12AdapterInfo* out_info) {
    if (!out_device) {
        return Status::Error(StatusCode::InvalidArgument, "SelectD3D12AdapterAndCreateDevice: null out_device");
    }
    *out_device = nullptr;
    if (out_adapter) *out_adapter = nullptr;
    if (out_info) *out_info = D3D12AdapterInfo{};

    auto create_fn = reinterpret_cast<PfnD3D12CreateDevice>(ResolveHostD3D12Proc("D3D12CreateDevice"));
    if (!create_fn) {
        return Status::Error(StatusCode::DeviceError, "Failed to resolve host d3d12.dll!D3D12CreateDevice");
    }

    if (options.enable_debug_layer) {
        auto debug_fn = reinterpret_cast<PfnD3D12GetDebugInterface>(
            ResolveHostD3D12Proc("D3D12GetDebugInterface"));
        if (debug_fn) {
            LocalComPtr<ID3D12Debug> debug_ctrl;
            if (SUCCEEDED(debug_fn(IID_PPV_ARGS(debug_ctrl.Put()))) && debug_ctrl) {
                debug_ctrl->EnableDebugLayer();
            }
        }
    }

    LocalComPtr<IDXGIFactory6> factory6;
    LocalComPtr<IDXGIFactory4> factory4;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory6.Put()));
    if (SUCCEEDED(hr) && factory6) {
        factory6->QueryInterface(IID_PPV_ARGS(factory4.Put()));
    } else {
        hr = CreateDXGIFactory1(IID_PPV_ARGS(factory4.Put()));
        if (FAILED(hr) || !factory4) {
            return Status::Error(StatusCode::DeviceError, "CreateDXGIFactory failed");
        }
    }

    const auto min_fl = static_cast<D3D_FEATURE_LEVEL>(
        options.min_feature_level != 0 ? options.min_feature_level
                                       : static_cast<uint32_t>(D3D_FEATURE_LEVEL_12_0));

    LocalComPtr<ID3D12Device> selected_dev;
    LocalComPtr<IDXGIAdapter1> selected_adapter;

    if (options.prefer_warp) {
        LocalComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(warp.Put()))) && warp) {
            if (TryCreateHostDevice(create_fn, warp.Get(), min_fl, selected_dev)) {
                selected_adapter = std::move(warp);
            }
        }
    } else {
        // Enumerate via IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) first.
        struct Candidate {
            LocalComPtr<IDXGIAdapter1> adapter;
            uint64_t dedicated_vram = 0;
        };
        std::vector<Candidate> hw_candidates;

        for (UINT idx = 0;; ++idx) {
            LocalComPtr<IDXGIAdapter1> adapter;
            HRESULT enum_hr = factory6
                                  ? factory6->EnumAdapterByGpuPreference(
                                        idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.Put()))
                                  : factory4->EnumAdapters1(idx, adapter.Put());
            if (enum_hr == DXGI_ERROR_NOT_FOUND || FAILED(enum_hr) || !adapter) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) continue;
            if (IsSoftwareOrWarpAdapter(desc)) continue;

            Candidate c;
            c.dedicated_vram = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            c.adapter = std::move(adapter);
            hw_candidates.push_back(std::move(c));
        }

        // Prefer the first high-performance discrete GPU (dedicated VRAM >= 512 MB);
        // if DXGI's index 0 is an integrated GPU (< 512 MB) while a discrete GPU exists,
        // promote the discrete GPU with largest dedicated VRAM ahead of integrated graphics.
        if (hw_candidates.size() > 1 && hw_candidates[0].dedicated_vram < (512ull * 1024ull * 1024ull)) {
            std::stable_sort(
                hw_candidates.begin(),
                hw_candidates.end(),
                [](const Candidate& a, const Candidate& b) {
                    return a.dedicated_vram > b.dedicated_vram;
                });
        }

        for (auto& c : hw_candidates) {
            if (TryCreateHostDevice(create_fn, c.adapter.Get(), min_fl, selected_dev)) {
                selected_adapter = std::move(c.adapter);
                break;
            }
        }

        if (!selected_dev && options.allow_warp_fallback) {
            LocalComPtr<IDXGIAdapter1> warp;
            if (SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(warp.Put()))) && warp) {
                if (TryCreateHostDevice(create_fn, warp.Get(), min_fl, selected_dev)) {
                    selected_adapter = std::move(warp);
                }
            }
        }
    }

    if (!selected_dev) {
        return Status::Error(StatusCode::DeviceError, "Failed to create host D3D12 device on any adapter");
    }

    if (out_info) {
        QueryD3D12DeviceAdapterInfo(selected_dev.Get(), selected_adapter.Get(), *out_info);
    }

    *out_device = selected_dev.Detach();
    if (out_adapter) {
        *out_adapter = selected_adapter.Detach();
    }
    return Status::Ok();
}

Status EnumerateD3D12Adapters(std::vector<D3D12AdapterInfo>& out_adapters) {
    out_adapters.clear();
    auto create_fn = reinterpret_cast<PfnD3D12CreateDevice>(ResolveHostD3D12Proc("D3D12CreateDevice"));
    if (!create_fn) {
        return Status::Error(StatusCode::DeviceError, "Host d3d12.dll unavailable");
    }

    LocalComPtr<IDXGIFactory6> factory6;
    LocalComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory6.Put()))) && factory6) {
        factory6->QueryInterface(IID_PPV_ARGS(factory4.Put()));
    } else if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory4.Put()))) || !factory4) {
        return Status::Error(StatusCode::DeviceError, "CreateDXGIFactory failed");
    }

    for (UINT idx = 0;; ++idx) {
        LocalComPtr<IDXGIAdapter1> adapter;
        HRESULT enum_hr = factory6
                              ? factory6->EnumAdapterByGpuPreference(
                                    idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.Put()))
                              : factory4->EnumAdapters1(idx, adapter.Put());
        if (enum_hr == DXGI_ERROR_NOT_FOUND || FAILED(enum_hr) || !adapter) {
            break;
        }
        LocalComPtr<ID3D12Device> dev;
        if (!TryCreateHostDevice(create_fn, adapter.Get(), D3D_FEATURE_LEVEL_12_0, dev)) {
            continue;
        }
        D3D12AdapterInfo info{};
        if (QueryD3D12DeviceAdapterInfo(dev.Get(), adapter.Get(), info).ok()) {
            out_adapters.push_back(std::move(info));
        }
    }
    return Status::Ok();
}

#else

Status EnumerateD3D12Adapters(std::vector<D3D12AdapterInfo>& out_adapters) {
    out_adapters.clear();
    return Status::Error(StatusCode::NotImplemented, "D3D12 adapter enumeration requires Windows");
}

#endif  // _WIN32

}  // namespace openscarlett
