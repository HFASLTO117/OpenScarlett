#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "openscarlett/gpu/d3d12_xs_abi.h"

namespace openscarlett::d3d12_xs {

class DeviceX;
struct WindowCallbackState;

class CommandQueueX final
    : public ID3D12CommandQueueX,
      public IOpenScarlettPresentCapture,
      public IOpenScarlettPresentWindow {
public:
    CommandQueueX(DeviceX* parent_device,
                  ID3D12CommandQueue* host_queue,
                  HWND initial_hwnd = nullptr);
    ~CommandQueueX();

    CommandQueueX(const CommandQueueX&) = delete;
    CommandQueueX& operator=(const CommandQueueX&) = delete;

    ID3D12CommandQueue* HostQueue() const noexcept { return host_queue_; }

    static ID3D12Pageable* UnwrapHostPageable(ID3D12Pageable* maybe_wrapped) noexcept;

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ID3D12DeviceChild (slot 7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    // ID3D12CommandQueue (slots 8..18)
    void STDMETHODCALLTYPE UpdateTileMappings(
        ID3D12Resource* pResource,
        UINT NumResourceRegions,
        const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
        const D3D12_TILE_REGION_SIZE* pResourceRegionSizes,
        ID3D12Heap* pHeap,
        UINT NumRanges,
        const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
        const UINT* pHeapRangeStartOffsets,
        const UINT* pRangeTileCounts,
        D3D12_TILE_MAPPING_FLAGS Flags) override;
    void STDMETHODCALLTYPE CopyTileMappings(
        ID3D12Resource* pDstResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
        ID3D12Resource* pSrcResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pRegionSize,
        D3D12_TILE_MAPPING_FLAGS Flags) override;
    void STDMETHODCALLTYPE ExecuteCommandLists(
        UINT NumCommandLists,
        ID3D12CommandList* const* ppCommandLists) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(
        UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) override;
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override;

    // ID3D12CommandQueueX (slots 19..21)
    HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters,
        void* pPresentToken) override;
    HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE ResumeX() override;

    // IOpenScarlettPresentCapture
    HRESULT STDMETHODCALLTYPE SetOutputWindow(HWND hwnd) override;
    HRESULT STDMETHODCALLTYPE SetReadbackCaptureEnabled(BOOL enabled) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentStats(
        uint64_t* out_fnv1a_hash,
        uint32_t* out_width,
        uint32_t* out_height,
        uint64_t* out_present_count) override;
    HRESULT STDMETHODCALLTYPE CopyLastPresentPixels(
        uint8_t* out_rgba8,
        uint32_t buffer_bytes,
        uint32_t* out_copied_bytes) override;

    // IOpenScarlettPresentWindow
    HRESULT STDMETHODCALLTYPE GetPresentWindowState(
        D3D12XBOX_PRESENT_WINDOW_STATE* out_state) override;
    HRESULT STDMETHODCALLTYPE ToggleBorderlessFullscreen() override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetWindowCloseBehavior(BOOL post_quit_on_close) override;

private:
    HRESULT EnsurePresentHelperObjectsLocked();
    HRESULT EnsureSwapChainLocked(
        HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format, bool force_resize);
    HRESULT EnsureReadbackBufferLocked(uint64_t required_bytes);
    HRESULT WaitForInternalFenceLocked();
    void AttachWindowSubclassLocked(HWND hwnd);
    void DetachWindowSubclassLocked();
    void ReleaseSwapChainLocked();
    void ReleasePresentObjectsLocked();
    void PaceFrameLocked(uint32_t frame_interval_us);

    std::atomic<ULONG> ref_count_{1};
    DeviceX* device_x_ = nullptr;
    ID3D12CommandQueue* host_queue_ = nullptr;
    std::atomic<bool> suspended_{false};

    std::mutex present_mutex_;
    HWND attached_hwnd_ = nullptr;
    HWND subclassed_hwnd_ = nullptr;
    std::shared_ptr<WindowCallbackState> window_state_;
    bool readback_capture_enabled_ = true;

    ID3D12CommandAllocator* present_alloc_ = nullptr;
    ID3D12GraphicsCommandList* present_cmd_ = nullptr;
    ID3D12Fence* present_fence_ = nullptr;
    HANDLE present_fence_event_ = nullptr;
    uint64_t present_fence_val_ = 0;

    ID3D12Resource* readback_buf_ = nullptr;
    uint64_t readback_buf_capacity_ = 0;

    IDXGISwapChain3* swapchain_ = nullptr;
    HWND swapchain_hwnd_ = nullptr;
    uint32_t swapchain_width_ = 0;
    uint32_t swapchain_height_ = 0;
    DXGI_FORMAT swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    uint32_t swapchain_flags_ = 0;
    uint32_t swapchain_buffer_count_ = 0;
    uint32_t swapchain_max_latency_ = 0;
    bool swapchain_occluded_ = false;
    HANDLE frame_latency_waitable_object_ = nullptr;

    int64_t qpc_freq_ = 0;
    int64_t last_paced_qpc_ = 0;
    int64_t last_present_qpc_ = 0;
    HANDLE pacing_timer_ = nullptr;

    uint64_t present_count_ = 0;
    uint64_t latency_waits_ = 0;
    double last_frame_ms_ = 0.0;
    uint64_t last_present_hash_ = 0;
    uint32_t last_present_width_ = 0;
    uint32_t last_present_height_ = 0;
    std::vector<uint8_t> last_present_pixels_;
};

}  // namespace openscarlett::d3d12_xs
