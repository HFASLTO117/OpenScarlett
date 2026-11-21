#pragma once

#include <cstdint>

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

#if defined(_WIN32)
#if defined(OPENSCARLETT_BUILDING_D3D12_XS)
#define OPENSCARLETT_D3D12_API __declspec(dllexport)
#else
#define OPENSCARLETT_D3D12_API __declspec(dllimport)
#endif
#else
#define OPENSCARLETT_D3D12_API
#endif

#if defined(_WIN32)

// Clean-room Xbox Series X|S (Scarlett) D3D12X ABI definitions based on public GDK documentation.

using D3D12XBOX_FRAME_PIPELINE_TOKEN = uint64_t;
constexpr D3D12XBOX_FRAME_PIPELINE_TOKEN D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL = 0ull;

enum D3D12XBOX_PROCESS_DEBUG_FLAGS : uint32_t {
    D3D12XBOX_PROCESS_DEBUG_FLAG_NONE = 0x0u,
    D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG = 0x1u,
    D3D12XBOX_PROCESS_DEBUG_FLAG_VALIDATED = 0x2u,
    D3D12XBOX_PROCESS_DEBUG_FLAG_INSTRUMENTED = 0x4u,
};

enum D3D12XBOX_FRAME_EVENT : uint32_t {
    D3D12XBOX_FRAME_EVENT_ORIGIN = 0u,
    D3D12XBOX_FRAME_EVENT_DISPLAY = 1u,
};

enum D3D12XBOX_PRESENT_FLAGS : uint32_t {
    D3D12XBOX_PRESENT_FLAG_NONE = 0x0u,
    D3D12XBOX_PRESENT_FLAG_IMMEDIATE = 0x1u,
};

enum D3D12XBOX_COLOR_SPACE : uint32_t {
    D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709 = 0u,
    D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 = 1u,
    D3D12XBOX_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 = 2u,
};

struct D3D12XBOX_CREATE_DEVICE_PARAMETERS {
    uint32_t Version;
    uint32_t ProcessDebugFlags;
    uint32_t GraphicsCommandQueueRingSizeBytes;
    uint32_t GraphicsScratchMemorySizeBytes;
    uint32_t ComputeScratchMemorySizeBytes;
    uint32_t DisableGeometryShaderAllocations;
    uint32_t DisableTessellationShaderAllocations;
    uint32_t DisableDXR;
    uint32_t CreateDeviceFlags;
    uint32_t AutoAttachPix;
    HWND OutputWindow;
    uint32_t PreferWarp;
};

struct D3D12XBOX_GPU_HARDWARE_INFO {
    uint32_t VendorId;
    uint32_t DeviceId;
    uint64_t DedicatedVideoMemoryBytes;
    uint32_t MeshShaderTier;
    uint32_t RaytracingTier;
    uint32_t HighestShaderModel;
    uint32_t WaveLaneCountMin;
    uint32_t WaveLaneCountMax;
    uint32_t IsWarp;
    char AdapterName[128];
};

struct D3D12XBOX_PRESENT_PLANE_PARAMETERS {
    D3D12XBOX_FRAME_PIPELINE_TOKEN Token;
    uint32_t ResourceCount;
    ID3D12Resource* const* ppResources;
    D3D12XBOX_COLOR_SPACE ColorSpace;
    D3D12_RECT SourceRect;
    D3D12_RECT DestRect;
    uint32_t Flags;
    D3D12_RESOURCE_STATES ResourceState;  // 0 = D3D12_RESOURCE_STATE_PRESENT / COMMON
};

struct D3D12XBOX_PRESENT_PARAMETERS {
    uint32_t Flags;
    uint32_t SyncInterval;
    uint32_t ImmediateThresholdPercent;
    HWND OutputWindow;
};

// Clean-room OpenScarlett COM interface IIDs for Xbox D3D12X proxy interfaces.
// {4F534431-3258-4445-5600-000000000001}
inline constexpr GUID IID_ID3D12DeviceX = {
    0x4F534431u, 0x3258u, 0x4445u, {0x56u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u}};

// {4F534431-3258-434D-5100-000000000002}
inline constexpr GUID IID_ID3D12CommandQueueX = {
    0x4F534431u, 0x3258u, 0x434Du, {0x51u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u}};

// {4F534431-3258-434D-4C00-000000000003}
inline constexpr GUID IID_ID3D12GraphicsCommandListX = {
    0x4F534431u, 0x3258u, 0x434Du, {0x4Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u}};

// {4F534431-3258-5052-4300-000000000004}
inline constexpr GUID IID_IOpenScarlettPresentCapture = {
    0x4F534431u, 0x3258u, 0x5052u, {0x43u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u}};

struct __declspec(uuid("4F534431-3258-4445-5600-000000000001")) ID3D12DeviceX : public ID3D12Device6 {
    virtual HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* pOutput,
        uint32_t PeriodMicroseconds,
        uint32_t MaxFrameLatency,
        uint32_t Flags) = 0;

    virtual HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT Event,
        uint64_t DelayMicroseconds,
        HANDLE hEvent,
        uint32_t Flags) = 0;

    virtual HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT Event,
        uint32_t TimeoutMilliseconds,
        void* pReserved,
        D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) = 0;

    virtual HRESULT STDMETHODCALLTYPE CreatePlacedResourceX(
        D3D12_GPU_VIRTUAL_ADDRESS VirtualAddress,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid,
        void** ppvResource) = 0;

    virtual HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeX() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGpuHardwareInfoX(D3D12XBOX_GPU_HARDWARE_INFO* pOutInfo) = 0;
};

struct __declspec(uuid("4F534431-3258-434D-5100-000000000002")) ID3D12CommandQueueX : public ID3D12CommandQueue {
    virtual HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters,
        void* pPresentToken) = 0;

    virtual HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeX() = 0;
};

struct __declspec(uuid("4F534431-3258-434D-4C00-000000000003")) ID3D12GraphicsCommandListX : public ID3D12GraphicsCommandList6 {
    virtual void STDMETHODCALLTYPE SetGpuVirtualAddressHintX(uint64_t Hint) = 0;
    virtual ID3D12GraphicsCommandList* STDMETHODCALLTYPE GetUnderlyingHostCommandList() = 0;
};

struct __declspec(uuid("4F534431-3258-5052-4300-000000000004")) IOpenScarlettPresentCapture : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOutputWindow(HWND hwnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetReadbackCaptureEnabled(BOOL enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLastPresentStats(
        uint64_t* out_fnv1a_hash,
        uint32_t* out_width,
        uint32_t* out_height,
        uint64_t* out_present_count) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyLastPresentPixels(
        uint8_t* out_rgba8,
        uint32_t buffer_bytes,
        uint32_t* out_copied_bytes) = 0;
};

extern "C" {

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12CreateDeviceX(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice);

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    REFIID riid,
    void** ppDevice);

// Test & harness control helpers exported by d3d12_xs.dll.
OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetPreferWarp(int prefer_warp);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetLastPresentHash(
    ID3D12CommandQueue* queue,
    uint64_t* out_hash,
    uint32_t* out_width,
    uint32_t* out_height);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CopyLastPresentPixels(
    ID3D12CommandQueue* queue,
    uint8_t* out_rgba8,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes);

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ResetTelemetry();

OPENSCARLETT_D3D12_API int WINAPI OpenScarlettD3D12GetTelemetryEntry(
    const char* symbol_name,
    int32_t* out_vtable_slot,
    uint64_t* out_total_calls);

}  // extern "C"

#endif  // _WIN32
