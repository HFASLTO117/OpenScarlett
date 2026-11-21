#define OPENSCARLETT_BUILDING_D3D12_XS 1
#include "openscarlett/gpu/d3d12_xs_abi.h"

#include <atomic>
#include <cstring>
#include <new>

#include "gpu/d3d12_xs/device_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"

#if defined(_MSC_VER)
#pragma comment(linker, "/EXPORT:D3D12CreateDevice=OS_D3D12CreateDevice")
#pragma comment(linker, "/EXPORT:D3D12SerializeRootSignature=OS_D3D12SerializeRootSignature")
#pragma comment(linker, "/EXPORT:D3D12SerializeVersionedRootSignature=OS_D3D12SerializeVersionedRootSignature")
#pragma comment(linker, "/EXPORT:D3D12CreateRootSignatureDeserializer=OS_D3D12CreateRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12CreateVersionedRootSignatureDeserializer=OS_D3D12CreateVersionedRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12GetDebugInterface=OS_D3D12GetDebugInterface")
#pragma comment(linker, "/EXPORT:D3D12EnableExperimentalFeatures=OS_D3D12EnableExperimentalFeatures")
#endif

namespace {

constexpr const char* kModule = "d3d12_xs.dll";
std::atomic<int> g_prefer_warp{0};

inline void RecordExport(const char* sym, HRESULT hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        sym,
        SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        static_cast<int32_t>(hr),
        -1);
}

using PfnD3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnD3D12SerializeRootSignature = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using PfnD3D12SerializeVersionedRootSignature = HRESULT(WINAPI*)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using PfnD3D12CreateRootSignatureDeserializer = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, REFIID, void**);
using PfnD3D12CreateVersionedRootSignatureDeserializer = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, REFIID, void**);
using PfnD3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
using PfnD3D12EnableExperimentalFeatures = HRESULT(WINAPI*)(
    UINT, const IID*, void*, UINT*);

HRESULT CreateWrappedDeviceInternal(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL min_level,
    bool prefer_warp,
    bool enable_debug_layer,
    HWND default_hwnd,
    REFIID riid,
    void** ppDevice) {
    auto host_create = reinterpret_cast<PfnD3D12CreateDevice>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateDevice"));
    if (!host_create) {
        return E_FAIL;
    }

    // D3D12 probe mode (ppDevice == nullptr)
    if (!ppDevice) {
        return host_create(pAdapter, min_level, riid, nullptr);
    }
    *ppDevice = nullptr;

    ID3D12Device* host_dev = nullptr;
    IDXGIAdapter1* adapter1 = nullptr;
    openscarlett::D3D12AdapterInfo info{};

    if (pAdapter) {
        HRESULT hr = host_create(pAdapter, min_level, IID_PPV_ARGS(&host_dev));
        if (FAILED(hr) || !host_dev) {
            return hr;
        }
        pAdapter->QueryInterface(IID_PPV_ARGS(&adapter1));
        openscarlett::QueryD3D12DeviceAdapterInfo(host_dev, adapter1, info);
        if (adapter1) adapter1->Release();
    } else {
        openscarlett::D3D12AdapterSelectionOptions opts{};
        opts.prefer_warp = prefer_warp;
        opts.allow_warp_fallback = true;
        opts.enable_debug_layer = enable_debug_layer;
        opts.min_feature_level = static_cast<uint32_t>(min_level);
        openscarlett::Status st = openscarlett::SelectD3D12AdapterAndCreateDevice(
            opts, &host_dev, &adapter1, &info);
        if (adapter1) adapter1->Release();
        if (!st.ok() || !host_dev) {
            return DXGI_ERROR_UNSUPPORTED;
        }
    }

    auto* dev_x = new (std::nothrow) openscarlett::d3d12_xs::DeviceX(host_dev, info, default_hwnd);
    host_dev->Release();
    if (!dev_x) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = dev_x->QueryInterface(riid, ppDevice);
    dev_x->Release();
    return hr;
}

}  // namespace

extern "C" {

HRESULT WINAPI OS_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice) {
    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        MinimumFeatureLevel,
        g_prefer_warp.load(std::memory_order_acquire) != 0,
        false,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12CreateDevice", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12CreateDeviceX(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice) {
    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        MinimumFeatureLevel,
        g_prefer_warp.load(std::memory_order_acquire) != 0,
        false,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12CreateDeviceX", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    REFIID riid,
    void** ppDevice) {
    const bool param_warp = (pParameters && pParameters->PreferWarp != 0);
    const bool global_warp = (g_prefer_warp.load(std::memory_order_acquire) != 0);
    const bool debug_layer =
        (pParameters && (pParameters->ProcessDebugFlags & D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG) != 0);
    const HWND default_hwnd = pParameters ? pParameters->OutputWindow : nullptr;

    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        D3D_FEATURE_LEVEL_12_0,
        param_warp || global_warp,
        debug_layer,
        default_hwnd,
        riid,
        ppDevice);
    RecordExport("D3D12XboxCreateDevice", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    auto fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    const HRESULT hr = fn ? fn(pRootSignature, Version, ppBlob, ppErrorBlob) : E_FAIL;
    RecordExport("D3D12SerializeRootSignature", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    auto fn = reinterpret_cast<PfnD3D12SerializeVersionedRootSignature>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeVersionedRootSignature"));
    const HRESULT hr = fn ? fn(pRootSignature, ppBlob, ppErrorBlob) : E_FAIL;
    RecordExport("D3D12SerializeVersionedRootSignature", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    auto fn = reinterpret_cast<PfnD3D12CreateRootSignatureDeserializer>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateRootSignatureDeserializer"));
    const HRESULT hr = fn
                           ? fn(pSrcData, SrcDataSizeInBytes,
                                pRootSignatureDeserializerInterface,
                                ppRootSignatureDeserializer)
                           : E_FAIL;
    RecordExport("D3D12CreateRootSignatureDeserializer", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    auto fn = reinterpret_cast<PfnD3D12CreateVersionedRootSignatureDeserializer>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateVersionedRootSignatureDeserializer"));
    const HRESULT hr = fn
                           ? fn(pSrcData, SrcDataSizeInBytes,
                                pRootSignatureDeserializerInterface,
                                ppRootSignatureDeserializer)
                           : E_FAIL;
    RecordExport("D3D12CreateVersionedRootSignatureDeserializer", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12GetDebugInterface(REFIID riid, void** ppvDebug) {
    auto fn = reinterpret_cast<PfnD3D12GetDebugInterface>(
        openscarlett::ResolveHostD3D12Proc("D3D12GetDebugInterface"));
    const HRESULT hr = fn ? fn(riid, ppvDebug) : E_FAIL;
    RecordExport("D3D12GetDebugInterface", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12EnableExperimentalFeatures(
    UINT NumFeatures,
    const IID* pIIDs,
    void* pConfigurationStructs,
    UINT* pConfigurationStructSizes) {
    auto fn = reinterpret_cast<PfnD3D12EnableExperimentalFeatures>(
        openscarlett::ResolveHostD3D12Proc("D3D12EnableExperimentalFeatures"));
    const HRESULT hr = fn
                           ? fn(NumFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes)
                           : E_FAIL;
    RecordExport("D3D12EnableExperimentalFeatures", hr);
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetPreferWarp(int prefer_warp) {
    g_prefer_warp.store(prefer_warp ? 1 : 0, std::memory_order_release);
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetLastPresentHash(
    ID3D12CommandQueue* queue,
    uint64_t* out_hash,
    uint32_t* out_width,
    uint32_t* out_height) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentCapture* capture = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentCapture,
                                       reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = capture->GetLastPresentStats(out_hash, out_width, out_height, nullptr);
    capture->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CopyLastPresentPixels(
    ID3D12CommandQueue* queue,
    uint8_t* out_rgba8,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentCapture* capture = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentCapture,
                                       reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = capture->CopyLastPresentPixels(out_rgba8, buffer_bytes, out_copied_bytes);
    capture->Release();
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ResetTelemetry() {
    openscarlett::CallTelemetry::Global().Reset();
}

OPENSCARLETT_D3D12_API int WINAPI OpenScarlettD3D12GetTelemetryEntry(
    const char* symbol_name,
    int32_t* out_vtable_slot,
    uint64_t* out_total_calls) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_vtable_slot) *out_vtable_slot = entry.vtable_slot;
            if (out_total_calls) *out_total_calls = entry.total_calls;
            return 1;
        }
    }
    if (out_vtable_slot) *out_vtable_slot = -1;
    if (out_total_calls) *out_total_calls = 0;
    return 0;
}

}  // extern "C"
