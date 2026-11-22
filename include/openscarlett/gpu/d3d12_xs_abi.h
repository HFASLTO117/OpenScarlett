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
    D3D12XBOX_COLOR_SPACE_RGB_FULL_G10_NONE_P709 = 3u,
};

enum D3D12XBOX_TONEMAP_MODE : uint32_t {
    D3D12XBOX_TONEMAP_AUTO = 0u,
    D3D12XBOX_TONEMAP_ACES = 1u,
    D3D12XBOX_TONEMAP_REINHARD = 2u,
    D3D12XBOX_TONEMAP_NONE = 3u,
};

enum D3D12XBOX_WAIT_VALUE_OP : uint32_t {
    D3D12XBOX_WAIT_VALUE_OP_ALWAYS = 0u,
    D3D12XBOX_WAIT_VALUE_OP_LESS = 1u,
    D3D12XBOX_WAIT_VALUE_OP_LESS_EQUAL = 2u,
    D3D12XBOX_WAIT_VALUE_OP_EQUAL = 3u,
    D3D12XBOX_WAIT_VALUE_OP_NOT_EQUAL = 4u,
    D3D12XBOX_WAIT_VALUE_OP_GREATER_EQUAL = 5u,
    D3D12XBOX_WAIT_VALUE_OP_GREATER = 6u,
};

struct D3D12XBOX_HDR10_METADATA {
    uint16_t RedPrimary[2] = {35400u, 14600u};    // BT.2020 (0.708, 0.292) in 0.00002 units
    uint16_t GreenPrimary[2] = {8500u, 39850u};   // BT.2020 (0.170, 0.797)
    uint16_t BluePrimary[2] = {6550u, 2300u};     // BT.2020 (0.131, 0.046)
    uint16_t WhitePoint[2] = {15635u, 16450u};    // D65 (0.3127, 0.3290)
    uint32_t MaxMasteringLuminance = 1000u;       // nits
    uint32_t MinMasteringLuminance = 50u;         // 0.0001 nit units (0.005 nits)
    uint16_t MaxContentLightLevel = 1000u;        // MaxCLL (nits)
    uint16_t MaxFrameAverageLightLevel = 400u;    // MaxFALL (nits)
};

struct D3D12XBOX_PRESENTX_CONFIG {
    HWND OutputWindow = nullptr;
    DXGI_FORMAT PreferredFormat = DXGI_FORMAT_UNKNOWN;
    D3D12XBOX_COLOR_SPACE ColorSpace = D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    D3D12XBOX_TONEMAP_MODE ToneMapMode = D3D12XBOX_TONEMAP_AUTO;
    float PaperWhiteNits = 200.0f;
    float MaxDisplayNits = 1000.0f;
    uint32_t ForceSdrToneMapFallback = 0u;
};

using PresentXConfig = D3D12XBOX_PRESENTX_CONFIG;
using PresentXWindowDesc = D3D12XBOX_PRESENTX_CONFIG;

struct D3D12XBOX_HDR_SWAPCHAIN_STATE {
    DXGI_FORMAT SwapChainFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE ActiveColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    uint32_t HasSwapChain4 = 0u;
    uint32_t ColorSpaceSupportFlags = 0u;
    uint32_t ColorSpaceApplied = 0u;
    uint32_t HdrMetaDataApplied = 0u;
    uint32_t ToneMapFallbackActive = 0u;
    D3D12XBOX_TONEMAP_MODE ToneMapMode = D3D12XBOX_TONEMAP_AUTO;
    DXGI_FORMAT LastPresentedSourceFormat = DXGI_FORMAT_UNKNOWN;
    D3D12XBOX_COLOR_SPACE LastPresentedColorSpace = D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    D3D12XBOX_HDR10_METADATA ActiveMetaData{};
};

struct D3D12XBOX_UNIFIED_TEXTURE_DESC {
    ID3D12Resource* Resource = nullptr;
    uint64_t GuestVirtualAddress = 0ull;
    uint64_t ByteSize = 0ull;
    uint32_t RowPitchBytes = 0u;
    D3D12_RESOURCE_STATES ResourceState = D3D12_RESOURCE_STATE_COMMON;
};

struct D3D12XBOX_UNIFIED_TEXTURE_STATS {
    uint32_t RegisteredCount = 0u;
    uint32_t DirtyCount = 0u;
    uint64_t TotalRemapNotifications = 0ull;
    uint64_t TotalFlushes = 0ull;
    uint64_t TotalBytesUploaded = 0ull;
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

struct D3D12XBOX_PRESENT_WINDOW_STATE {
    uint32_t width;
    uint32_t height;
    uint32_t is_fullscreen;
    uint32_t is_minimized;
    uint32_t is_active;
    uint32_t close_requested;
    uint32_t swapchain_flags;
    uint32_t frame_interval_us;
    uint32_t max_frame_latency;
    uint64_t present_count;
    uint64_t latency_waits;
    double last_frame_ms;
};

// Separated PresentX GPU fence-signal vs DXGI/Vulkan presentation timing telemetry.
struct D3D12XBOX_PRESENT_TIMING_STATS {
    int64_t fence_signal_qpc = 0;
    double fence_wait_us = 0.0;
    uint64_t fence_completed_value = 0ull;
    int64_t present_submit_qpc = 0;
    int64_t present_complete_qpc = 0;
    double dxgi_present_us = 0.0;
    double total_presentx_us = 0.0;
    uint64_t present_count = 0ull;
};

// d3d12_xs execution backend mode (OPENSCARLETT_BACKEND / OPENSCARLETT_GPU_BACKEND).
enum D3D12XBOX_BACKEND_MODE : uint32_t {
    D3D12XBOX_BACKEND_AUTO = 0u,
    D3D12XBOX_BACKEND_D3D12 = 1u,
    D3D12XBOX_BACKEND_VULKAN = 2u,
};

// Detailed live d3d12_xs -> Vulkan 1.4 command-list routing telemetry.
struct D3D12XBOX_VULKAN_ROUTING_STATS {
    uint32_t vulkan_routing_active = 0u;
    uint32_t executed_on_vulkan_icd = 0u;
    uint32_t vulkan_software_fallback = 0u;
    uint32_t vulkan_api_major = 0u;
    uint32_t vulkan_api_minor = 0u;
    uint32_t vulkan_api_patch = 0u;
    uint32_t subgroup_size = 0u;
    uint32_t max_push_constants_size = 0u;
    uint64_t routed_command_lists = 0ull;
    uint64_t routed_draws = 0ull;
    uint64_t routed_indexed_draws = 0ull;
    uint64_t routed_dispatches = 0ull;
    uint64_t routed_mesh_dispatches = 0ull;
    uint64_t routed_copies = 0ull;
    uint64_t routed_barriers = 0ull;
    uint64_t routed_rtv_clears = 0ull;
    uint64_t routed_dsv_clears = 0ull;
    uint64_t routed_presents = 0ull;
    uint64_t vulkan_icd_executions = 0ull;
    uint64_t spirv_direct_draws = 0ull;
    uint64_t spirv_direct_dispatches = 0ull;
    uint64_t spirv_direct_mesh_dispatches = 0ull;
    char vulkan_device_name[128] = {};
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

// {4F534431-3258-5052-5700-000000000005}
inline constexpr GUID IID_IOpenScarlettPresentWindow = {
    0x4F534431u, 0x3258u, 0x5052u, {0x57u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x05u}};

// {4F534431-3258-4844-5200-000000000006}
inline constexpr GUID IID_IOpenScarlettPresentHdr = {
    0x4F534431u, 0x3258u, 0x4844u, {0x52u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x06u}};

// {4F534431-3258-554D-4500-000000000007}
inline constexpr GUID IID_IOpenScarlettUnifiedMemory = {
    0x4F534431u, 0x3258u, 0x554Du, {0x45u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x07u}};

// {4F534431-3258-5052-5400-000000000008}
inline constexpr GUID IID_IOpenScarlettPresentTiming = {
    0x4F534431u, 0x3258u, 0x5052u, {0x54u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x08u}};

// {4F534431-3258-564B-5200-000000000009}
inline constexpr GUID IID_IOpenScarlettVulkanRouting = {
    0x4F534431u, 0x3258u, 0x564Bu, {0x52u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x09u}};

#if defined(_MSC_VER)
#define OPENSCARLETT_COM_UUID(guid_str) __declspec(uuid(guid_str))
#else
#define OPENSCARLETT_COM_UUID(guid_str)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-4445-5600-000000000001") ID3D12DeviceX : public ID3D12Device6 {
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
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12DeviceX, 0x4F534431, 0x3258, 0x4445, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-434D-5100-000000000002") ID3D12CommandQueueX : public ID3D12CommandQueue {
    virtual HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters,
        void* pPresentToken) = 0;

    virtual HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeX() = 0;
    virtual HRESULT STDMETHODCALLTYPE KickoffX() = 0;
    virtual HRESULT STDMETHODCALLTYPE Wait32BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint32_t Value,
        uint32_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE Wait64BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint64_t Value,
        uint64_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12CommandQueueX, 0x4F534431, 0x3258, 0x434D, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-434D-4C00-000000000003") ID3D12GraphicsCommandListX : public ID3D12GraphicsCommandList6 {
    virtual void STDMETHODCALLTYPE SetGpuVirtualAddressHintX(uint64_t Hint) = 0;
    virtual ID3D12GraphicsCommandList* STDMETHODCALLTYPE GetUnderlyingHostCommandList() = 0;
    virtual void STDMETHODCALLTYPE KickoffX() = 0;
    virtual void STDMETHODCALLTYPE Wait32BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint32_t Value,
        uint32_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) = 0;
    virtual void STDMETHODCALLTYPE Wait64BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint64_t Value,
        uint64_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) = 0;
    virtual void STDMETHODCALLTYPE RSSetDepthBiasX(
        FLOAT DepthBias,
        FLOAT DepthBiasClamp,
        FLOAT SlopeScaledDepthBias) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12GraphicsCommandListX, 0x4F534431, 0x3258, 0x434D, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-5052-4300-000000000004") IOpenScarlettPresentCapture : public IUnknown {
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
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IOpenScarlettPresentCapture, 0x4F534431, 0x3258, 0x5052, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-5052-5700-000000000005") IOpenScarlettPresentWindow : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPresentWindowState(
        D3D12XBOX_PRESENT_WINDOW_STATE* out_state) = 0;
    virtual HRESULT STDMETHODCALLTYPE ToggleBorderlessFullscreen() = 0;
    // Returns borrowed waitable latency handle owned by CommandQueueX (do not CloseHandle).
    virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetWindowCloseBehavior(BOOL post_quit_on_close) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IOpenScarlettPresentWindow, 0x4F534431, 0x3258, 0x5052, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-4844-5200-000000000006") IOpenScarlettPresentHdr : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetPresentConfig(
        const D3D12XBOX_PRESENTX_CONFIG* config) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetHdrMetaData(
        const D3D12XBOX_HDR10_METADATA* metadata) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHdrSwapChainState(
        D3D12XBOX_HDR_SWAPCHAIN_STATE* out_state) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyLastPresentRawPixels(
        uint8_t* out_raw,
        uint32_t buffer_bytes,
        uint32_t* out_copied_bytes,
        DXGI_FORMAT* out_format) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IOpenScarlettPresentHdr, 0x4F534431, 0x3258, 0x4844, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-554D-4500-000000000007") IOpenScarlettUnifiedMemory : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterUnifiedTexture(
        const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterUnifiedTexture(
        ID3D12Resource* resource) = 0;
    virtual HRESULT STDMETHODCALLTYPE NotifyVirtualRemap(
        uint64_t guest_va_base,
        uint64_t byte_size,
        uint64_t new_backing_va,
        uint32_t* out_flagged_count) = 0;
    virtual HRESULT STDMETHODCALLTYPE FlushDirtyUnifiedTextures(
        ID3D12CommandQueue* queue,
        uint32_t* out_flushed_count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUnifiedTextureStats(
        D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats) = 0;
    virtual HRESULT STDMETHODCALLTYPE AttachXMemAllocation(
        uint64_t guest_va,
        uint64_t byte_size,
        uint64_t* out_desktop_gpu_va) = 0;
    virtual HRESULT STDMETHODCALLTYPE DetachXMemAllocation(
        uint64_t guest_va) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResolveXMemGpuVa(
        uint64_t guest_va,
        uint64_t* out_desktop_gpu_va) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetR9G9B9E5UavFallbackActive(
        int* out_active) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IOpenScarlettUnifiedMemory, 0x4F534431, 0x3258, 0x554D, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-5052-5400-000000000008") IOpenScarlettPresentTiming : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPresentTimingStats(
        D3D12XBOX_PRESENT_TIMING_STATS* out_stats) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IOpenScarlettPresentTiming, 0x4F534431, 0x3258, 0x5052, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08)
#endif

struct OPENSCARLETT_COM_UUID("4F534431-3258-564B-5200-000000000009") IOpenScarlettVulkanRouting : public IUnknown {
    virtual uint32_t STDMETHODCALLTYPE GetActiveBackendMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanRoutingStats(
        D3D12XBOX_VULKAN_ROUTING_STATS* out_stats) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IOpenScarlettVulkanRouting, 0x4F534431, 0x3258, 0x564B, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09)
#endif

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

// Interactive Win32 swapchain & windowing helpers exported by d3d12_xs.dll.
OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetPresentWindowState(
    ID3D12CommandQueue* queue,
    D3D12XBOX_PRESENT_WINDOW_STATE* out_state);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12ToggleBorderlessFullscreen(
    ID3D12CommandQueue* queue);

// Returns borrowed DXGI frame latency waitable handle owned by CommandQueueX (do not CloseHandle).
OPENSCARLETT_D3D12_API HANDLE WINAPI OpenScarlettD3D12GetFrameLatencyWaitableObject(
    ID3D12CommandQueue* queue);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetWindowCloseBehavior(
    ID3D12CommandQueue* queue,
    int post_quit_on_close);

// HDR10 / ST.2084 & scRGB PresentX configuration, metadata, readback & tone-mapping helpers.
OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetPresentConfig(
    ID3D12CommandQueue* queue,
    const D3D12XBOX_PRESENTX_CONFIG* config);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetHdrMetaData(
    ID3D12CommandQueue* queue,
    const D3D12XBOX_HDR10_METADATA* metadata);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetHdrSwapChainState(
    ID3D12CommandQueue* queue,
    D3D12XBOX_HDR_SWAPCHAIN_STATE* out_state);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CopyLastPresentRawPixels(
    ID3D12CommandQueue* queue,
    uint8_t* out_raw,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes,
    DXGI_FORMAT* out_format);

OPENSCARLETT_D3D12_API float WINAPI OpenScarlettD3D12LinearNitsToPqSt2084(float nits);
OPENSCARLETT_D3D12_API float WINAPI OpenScarlettD3D12PqSt2084ToLinearNits(float pq);

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(
    float r,
    float g,
    float b,
    float a,
    D3D12XBOX_TONEMAP_MODE mode,
    uint8_t out_rgba8[4]);

// Title unified-memory texture remap tracking & GPU upload synchronization helpers.
OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12RegisterUnifiedTexture(
    ID3D12Device* device,
    const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12UnregisterUnifiedTexture(
    ID3D12Device* device,
    ID3D12Resource* resource);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12NotifyVirtualRemap(
    ID3D12Device* device,
    uint64_t guest_va_base,
    uint64_t byte_size,
    uint64_t new_backing_va,
    uint32_t* out_flagged_count);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12FlushDirtyUnifiedTextures(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    uint32_t* out_flushed_count);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetUnifiedTextureStats(
    ID3D12Device* device,
    D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats);

// Extended telemetry inspection (exposes not_impl / error counts and last HRESULT).
OPENSCARLETT_D3D12_API int WINAPI OpenScarlettD3D12GetTelemetryDetail(
    const char* symbol_name,
    int32_t* out_vtable_slot,
    uint64_t* out_total_calls,
    uint64_t* out_ok_calls,
    uint64_t* out_not_impl_calls,
    uint64_t* out_error_calls,
    int32_t* out_last_hr);

// Fail-closed Xbox-exclusive hardware APIs without a desktop D3D12 equivalent (return E_NOTIMPL + telemetry).
OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxCreateDmaCommandQueue(
    ID3D12Device* pDevice,
    const void* pDesc,
    REFIID riid,
    void** ppCommandQueue);

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxEnableDccMetadata(
    ID3D12Device* pDevice,
    ID3D12Resource* pResource,
    uint32_t Flags);

// Xbox CommandQueueX / CommandListX C wrappers, XMem GPU VA attachment, and R9G9B9E5 UAV fallback controls.
OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueKickoffX(
    ID3D12CommandQueue* queue);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueWait32BitValueX(
    ID3D12CommandQueue* queue,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    uint32_t value,
    uint32_t mask,
    D3D12XBOX_WAIT_VALUE_OP operation);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueWait64BitValueX(
    ID3D12CommandQueue* queue,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    uint64_t value,
    uint64_t mask,
    D3D12XBOX_WAIT_VALUE_OP operation);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListKickoffX(
    ID3D12GraphicsCommandList* cmd_list);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListRSSetDepthBiasX(
    ID3D12GraphicsCommandList* cmd_list,
    float depth_bias,
    float depth_bias_clamp,
    float slope_scaled_depth_bias);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListGetDepthBiasX(
    ID3D12GraphicsCommandList* cmd_list,
    float* out_depth_bias,
    float* out_depth_bias_clamp,
    float* out_slope_scaled_depth_bias);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12AttachXMemAllocation(
    ID3D12Device* device,
    uint64_t guest_va,
    uint64_t byte_size,
    uint64_t* out_desktop_gpu_va);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12DetachXMemAllocation(
    ID3D12Device* device,
    uint64_t guest_va);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12ResolveXMemGpuVa(
    ID3D12Device* device,
    uint64_t guest_va,
    uint64_t* out_desktop_gpu_va);

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetForceR9G9B9E5UavFallback(
    int force_fallback);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetR9G9B9E5UavFallbackActive(
    ID3D12Device* device,
    int* out_active);

// Separated PresentX fence signaling vs DXGI/Vulkan presentation timing stats.
OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetPresentTimingStats(
    ID3D12CommandQueue* queue,
    D3D12XBOX_PRESENT_TIMING_STATS* out_stats);

// Direct d3d12_xs -> Vulkan 1.4 backend mode & routing telemetry helpers.
OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetBackendMode(
    uint32_t backend_mode);

OPENSCARLETT_D3D12_API uint32_t WINAPI OpenScarlettD3D12GetActiveBackend(
    ID3D12Device* device);

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetVulkanRoutingStats(
    ID3D12Device* device,
    D3D12XBOX_VULKAN_ROUTING_STATS* out_stats);

}  // extern "C"

namespace openscarlett::d3d12_xs {

inline float LinearNitsToPqSt2084(float nits) noexcept {
    return OpenScarlettD3D12LinearNitsToPqSt2084(nits);
}

inline float PqSt2084ToLinearNits(float pq) noexcept {
    return OpenScarlettD3D12PqSt2084ToLinearNits(pq);
}

}  // namespace openscarlett::d3d12_xs

#endif  // _WIN32
