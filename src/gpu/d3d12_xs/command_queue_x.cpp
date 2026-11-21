#include "command_queue_x.h"

#include <algorithm>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>

#include "command_list_x.h"
#include "device_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"

namespace openscarlett::d3d12_xs {

namespace {

constexpr const char* kModule = "d3d12_xs.dll";

inline void RecordQueue(const char* method, int32_t slot, HRESULT hr = S_OK) {
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        method,
        SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        static_cast<int32_t>(hr),
        slot);
}

struct QueueRegistry {
    std::shared_mutex mutex;
    std::unordered_map<const void*, ID3D12CommandQueue*> map;
};

QueueRegistry& GetQueueRegistry() {
    static QueueRegistry reg;
    return reg;
}

DXGI_FORMAT NormalizeSwapChainFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return fmt;
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

}  // namespace

CommandQueueX::CommandQueueX(
    DeviceX* parent_device,
    ID3D12CommandQueue* host_queue,
    HWND initial_hwnd)
    : device_x_(parent_device), host_queue_(host_queue), attached_hwnd_(initial_hwnd) {
    if (device_x_) device_x_->AddRef();
    if (host_queue_) host_queue_->AddRef();
    auto& reg = GetQueueRegistry();
    std::unique_lock<std::shared_mutex> lock(reg.mutex);
    reg.map[static_cast<const ID3D12Pageable*>(this)] = host_queue_;
}

CommandQueueX::~CommandQueueX() {
    {
        auto& reg = GetQueueRegistry();
        std::unique_lock<std::shared_mutex> lock(reg.mutex);
        reg.map.erase(static_cast<const ID3D12Pageable*>(this));
    }
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        ReleasePresentObjectsLocked();
    }
    if (host_queue_) host_queue_->Release();
    if (device_x_) device_x_->Release();
}

ID3D12Pageable* CommandQueueX::UnwrapHostPageable(ID3D12Pageable* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetQueueRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.map.find(static_cast<const void*>(maybe_wrapped));
    if (it != reg.map.end()) {
        return it->second;
    }
    return maybe_wrapped;
}

// IUnknown (slots 0..2)
HRESULT STDMETHODCALLTYPE CommandQueueX::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, E_POINTER);
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (IsEqualIID(riid, __uuidof(IUnknown)) ||
        IsEqualIID(riid, __uuidof(ID3D12Object)) ||
        IsEqualIID(riid, __uuidof(ID3D12DeviceChild)) ||
        IsEqualIID(riid, __uuidof(ID3D12Pageable)) ||
        IsEqualIID(riid, __uuidof(ID3D12CommandQueue)) ||
        IsEqualIID(riid, IID_ID3D12CommandQueueX)) {
        *ppvObject = static_cast<ID3D12CommandQueueX*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettPresentCapture)) {
        *ppvObject = static_cast<IOpenScarlettPresentCapture*>(this);
        AddRef();
        RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    RecordQueue("ID3D12CommandQueueX::QueryInterface", 0, E_NOINTERFACE);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CommandQueueX::AddRef() {
    const ULONG refs = ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    RecordQueue("ID3D12CommandQueueX::AddRef", 1, S_OK);
    return refs;
}

ULONG STDMETHODCALLTYPE CommandQueueX::Release() {
    RecordQueue("ID3D12CommandQueueX::Release", 2, S_OK);
    const ULONG rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete this;
    }
    return rem;
}

// ID3D12Object (slots 3..6)
HRESULT STDMETHODCALLTYPE CommandQueueX::GetPrivateData(
    REFGUID guid, UINT* pDataSize, void* pData) {
    const HRESULT hr = host_queue_->GetPrivateData(guid, pDataSize, pData);
    RecordQueue("ID3D12CommandQueueX::GetPrivateData", 3, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetPrivateData(
    REFGUID guid, UINT DataSize, const void* pData) {
    const HRESULT hr = host_queue_->SetPrivateData(guid, DataSize, pData);
    RecordQueue("ID3D12CommandQueueX::SetPrivateData", 4, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetPrivateDataInterface(
    REFGUID guid, const IUnknown* pData) {
    const HRESULT hr = host_queue_->SetPrivateDataInterface(guid, pData);
    RecordQueue("ID3D12CommandQueueX::SetPrivateDataInterface", 5, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetName(LPCWSTR Name) {
    const HRESULT hr = host_queue_->SetName(Name);
    RecordQueue("ID3D12CommandQueueX::SetName", 6, hr);
    return hr;
}

// ID3D12DeviceChild (slot 7)
HRESULT STDMETHODCALLTYPE CommandQueueX::GetDevice(REFIID riid, void** ppvDevice) {
    const HRESULT hr = device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
    RecordQueue("ID3D12CommandQueueX::GetDevice", 7, hr);
    return hr;
}

// ID3D12CommandQueue (slots 8..18)
void STDMETHODCALLTYPE CommandQueueX::UpdateTileMappings(
    ID3D12Resource* pResource,
    UINT NumResourceRegions,
    const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
    const D3D12_TILE_REGION_SIZE* pResourceRegionSizes,
    ID3D12Heap* pHeap,
    UINT NumRanges,
    const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
    const UINT* pHeapRangeStartOffsets,
    const UINT* pRangeTileCounts,
    D3D12_TILE_MAPPING_FLAGS Flags) {
    host_queue_->UpdateTileMappings(
        pResource, NumResourceRegions, pResourceRegionStartCoordinates, pResourceRegionSizes,
        pHeap, NumRanges, pRangeFlags, pHeapRangeStartOffsets, pRangeTileCounts, Flags);
    RecordQueue("ID3D12CommandQueueX::UpdateTileMappings", 8, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::CopyTileMappings(
    ID3D12Resource* pDstResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
    ID3D12Resource* pSrcResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
    const D3D12_TILE_REGION_SIZE* pRegionSize,
    D3D12_TILE_MAPPING_FLAGS Flags) {
    host_queue_->CopyTileMappings(
        pDstResource, pDstRegionStartCoordinate, pSrcResource,
        pSrcRegionStartCoordinate, pRegionSize, Flags);
    RecordQueue("ID3D12CommandQueueX::CopyTileMappings", 9, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::ExecuteCommandLists(
    UINT NumCommandLists,
    ID3D12CommandList* const* ppCommandLists) {
    // Unwrap any CommandListX* wrappers back to native host ID3D12CommandList* pointers.
    if (NumCommandLists == 0 || !ppCommandLists) {
        host_queue_->ExecuteCommandLists(NumCommandLists, ppCommandLists);
        RecordQueue("ID3D12CommandQueueX::ExecuteCommandLists", 10, S_OK);
        return;
    }

    constexpr UINT kStackLimit = 64u;
    ID3D12CommandList* stack_lists[kStackLimit];
    std::vector<ID3D12CommandList*> heap_lists;
    ID3D12CommandList** raw_lists = stack_lists;

    if (NumCommandLists > kStackLimit) {
        heap_lists.resize(NumCommandLists);
        raw_lists = heap_lists.data();
    }

    for (UINT i = 0; i < NumCommandLists; ++i) {
        raw_lists[i] = CommandListX::UnwrapHostList(ppCommandLists[i]);
    }

    host_queue_->ExecuteCommandLists(NumCommandLists, raw_lists);
    RecordQueue("ID3D12CommandQueueX::ExecuteCommandLists", 10, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::SetMarker(UINT Metadata, const void* pData, UINT Size) {
    host_queue_->SetMarker(Metadata, pData, Size);
    RecordQueue("ID3D12CommandQueueX::SetMarker", 11, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::BeginEvent(UINT Metadata, const void* pData, UINT Size) {
    host_queue_->BeginEvent(Metadata, pData, Size);
    RecordQueue("ID3D12CommandQueueX::BeginEvent", 12, S_OK);
}

void STDMETHODCALLTYPE CommandQueueX::EndEvent() {
    host_queue_->EndEvent();
    RecordQueue("ID3D12CommandQueueX::EndEvent", 13, S_OK);
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Signal(ID3D12Fence* pFence, UINT64 Value) {
    const HRESULT hr = host_queue_->Signal(pFence, Value);
    RecordQueue("ID3D12CommandQueueX::Signal", 14, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::Wait(ID3D12Fence* pFence, UINT64 Value) {
    const HRESULT hr = host_queue_->Wait(pFence, Value);
    RecordQueue("ID3D12CommandQueueX::Wait", 15, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetTimestampFrequency(UINT64* pFrequency) {
    const HRESULT hr = host_queue_->GetTimestampFrequency(pFrequency);
    RecordQueue("ID3D12CommandQueueX::GetTimestampFrequency", 16, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetClockCalibration(
    UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) {
    const HRESULT hr = host_queue_->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
    RecordQueue("ID3D12CommandQueueX::GetClockCalibration", 17, hr);
    return hr;
}

D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE CommandQueueX::GetDesc() {
    const D3D12_COMMAND_QUEUE_DESC desc = host_queue_->GetDesc();
    RecordQueue("ID3D12CommandQueueX::GetDesc", 18, S_OK);
    return desc;
}

// PresentX internal helpers
HRESULT CommandQueueX::EnsurePresentHelperObjectsLocked() {
    if (present_alloc_ && present_cmd_ && present_fence_ && present_fence_event_) {
        return S_OK;
    }
    ID3D12Device* dev = device_x_->HostDevice();
    const D3D12_COMMAND_LIST_TYPE list_type = host_queue_->GetDesc().Type;

    if (!present_alloc_) {
        HRESULT hr = dev->CreateCommandAllocator(list_type, IID_PPV_ARGS(&present_alloc_));
        if (FAILED(hr)) return hr;
    }
    if (!present_cmd_) {
        HRESULT hr = dev->CreateCommandList(
            0, list_type, present_alloc_, nullptr, IID_PPV_ARGS(&present_cmd_));
        if (FAILED(hr)) return hr;
        present_cmd_->Close();
    }
    if (!present_fence_) {
        HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence_));
        if (FAILED(hr)) return hr;
    }
    if (!present_fence_event_) {
        present_fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!present_fence_event_) return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT CommandQueueX::EnsureReadbackBufferLocked(uint64_t required_bytes) {
    if (readback_buf_ && readback_buf_capacity_ >= required_bytes) {
        return S_OK;
    }
    if (readback_buf_) {
        readback_buf_->Release();
        readback_buf_ = nullptr;
        readback_buf_capacity_ = 0;
    }

    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = required_bytes;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.Format = DXGI_FORMAT_UNKNOWN;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device_x_->HostDevice()->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_buf_));
    if (SUCCEEDED(hr)) {
        readback_buf_capacity_ = required_bytes;
    }
    return hr;
}

HRESULT CommandQueueX::EnsureSwapChainLocked(
    HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    const DXGI_FORMAT sc_fmt = NormalizeSwapChainFormat(format);
    if (swapchain_ && swapchain_hwnd_ == hwnd &&
        swapchain_width_ == width && swapchain_height_ == height &&
        swapchain_format_ == sc_fmt) {
        return S_OK;
    }

    if (swapchain_) {
        WaitForInternalFenceLocked();
        if (swapchain_hwnd_ == hwnd) {
            HRESULT resize_hr = swapchain_->ResizeBuffers(2, width, height, sc_fmt, 0);
            if (SUCCEEDED(resize_hr)) {
                swapchain_width_ = width;
                swapchain_height_ = height;
                swapchain_format_ = sc_fmt;
                return S_OK;
            }
        }
        swapchain_->Release();
        swapchain_ = nullptr;
        swapchain_hwnd_ = nullptr;
    }

    IDXGIFactory4* factory4 = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory4));
    if (FAILED(hr) || !factory4) {
        return hr;
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = sc_fmt;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1* sc1 = nullptr;
    hr = factory4->CreateSwapChainForHwnd(host_queue_, hwnd, &sc_desc, nullptr, nullptr, &sc1);
    factory4->Release();
    if (FAILED(hr) || !sc1) {
        return hr;
    }

    hr = sc1->QueryInterface(IID_PPV_ARGS(&swapchain_));
    sc1->Release();
    if (SUCCEEDED(hr) && swapchain_) {
        swapchain_hwnd_ = hwnd;
        swapchain_width_ = width;
        swapchain_height_ = height;
        swapchain_format_ = sc_fmt;
    }
    return hr;
}

HRESULT CommandQueueX::WaitForInternalFenceLocked() {
    if (!present_fence_ || !present_fence_event_) return S_OK;
    const uint64_t target = ++present_fence_val_;
    HRESULT hr = host_queue_->Signal(present_fence_, target);
    if (FAILED(hr)) return hr;
    if (present_fence_->GetCompletedValue() < target) {
        hr = present_fence_->SetEventOnCompletion(target, present_fence_event_);
        if (FAILED(hr)) return hr;
        const DWORD wr = WaitForSingleObject(present_fence_event_, 15000);
        if (wr != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return S_OK;
}

void CommandQueueX::ReleasePresentObjectsLocked() {
    if (present_fence_ && host_queue_) {
        WaitForInternalFenceLocked();
    }
    if (swapchain_) { swapchain_->Release(); swapchain_ = nullptr; }
    if (readback_buf_) { readback_buf_->Release(); readback_buf_ = nullptr; }
    if (present_cmd_) { present_cmd_->Release(); present_cmd_ = nullptr; }
    if (present_alloc_) { present_alloc_->Release(); present_alloc_ = nullptr; }
    if (present_fence_) { present_fence_->Release(); present_fence_ = nullptr; }
    if (present_fence_event_) { CloseHandle(present_fence_event_); present_fence_event_ = nullptr; }
    readback_buf_capacity_ = 0;
}

// ID3D12CommandQueueX (slots 19..21)
HRESULT STDMETHODCALLTYPE CommandQueueX::PresentX(
    UINT PlaneCount,
    const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
    const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters,
    void* pPresentToken) {
    (void)pPresentToken;
    // Xbox GDK titles (e.g. Xbox-GDK-Samples DeviceResources::~DeviceResources & resize)
    // legitimately call PresentX(0, nullptr, nullptr) to present a blank screen before teardown.
    if (PlaneCount == 0) {
        std::lock_guard<std::mutex> lock(present_mutex_);
        WaitForInternalFenceLocked();
        ++present_count_;
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, S_OK);
        return S_OK;
    }
    if (!pPlaneParameters ||
        pPlaneParameters[0].ResourceCount == 0 ||
        !pPlaneParameters[0].ppResources ||
        !pPlaneParameters[0].ppResources[0]) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, E_INVALIDARG);
        return E_INVALIDARG;
    }

    ID3D12Resource* src_tex = pPlaneParameters[0].ppResources[0];
    const D3D12_RESOURCE_DESC src_desc = src_tex->GetDesc();
    if (src_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        src_desc.Width == 0 || src_desc.Height == 0) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, E_INVALIDARG);
        return E_INVALIDARG;
    }

    const uint32_t width = static_cast<uint32_t>(src_desc.Width);
    const uint32_t height = src_desc.Height;
    const D3D12_RESOURCE_STATES caller_state = pPlaneParameters[0].ResourceState;

    std::lock_guard<std::mutex> lock(present_mutex_);

    HRESULT hr = EnsurePresentHelperObjectsLocked();
    if (FAILED(hr)) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
        return hr;
    }

    HWND target_hwnd = (pPresentParameters && pPresentParameters->OutputWindow)
                           ? pPresentParameters->OutputWindow
                           : attached_hwnd_;
    const bool use_swapchain = (target_hwnd != nullptr && IsWindow(target_hwnd) != FALSE);
    const bool need_readback = (!use_swapchain) || readback_capture_enabled_;

    if (use_swapchain) {
        hr = EnsureSwapChainLocked(target_hwnd, width, height, src_desc.Format);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size_bytes = 0;
    UINT64 total_bytes = 0;
    if (need_readback) {
        device_x_->HostDevice()->GetCopyableFootprints(
            &src_desc, 0, 1, 0, &footprint, &num_rows, &row_size_bytes, &total_bytes);
        hr = EnsureReadbackBufferLocked(total_bytes);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
    }

    hr = present_alloc_->Reset();
    if (FAILED(hr)) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
        return hr;
    }
    hr = present_cmd_->Reset(present_alloc_, nullptr);
    if (FAILED(hr)) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
        return hr;
    }

    // Transition plane 0 to COPY_SOURCE if needed.
    if (caller_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = src_tex;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = caller_state;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        present_cmd_->ResourceBarrier(1, &to_copy);
    }

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src_tex;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    ID3D12Resource* backbuffer = nullptr;
    if (use_swapchain && swapchain_) {
        const UINT bb_idx = swapchain_->GetCurrentBackBufferIndex();
        hr = swapchain_->GetBuffer(bb_idx, IID_PPV_ARGS(&backbuffer));
        if (SUCCEEDED(hr) && backbuffer) {
            D3D12_RESOURCE_BARRIER bb_to_dst{};
            bb_to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bb_to_dst.Transition.pResource = backbuffer;
            bb_to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bb_to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            bb_to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            present_cmd_->ResourceBarrier(1, &bb_to_dst);

            D3D12_TEXTURE_COPY_LOCATION bb_dst{};
            bb_dst.pResource = backbuffer;
            bb_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bb_dst.SubresourceIndex = 0;
            present_cmd_->CopyTextureRegion(&bb_dst, 0, 0, 0, &src_loc, nullptr);

            D3D12_RESOURCE_BARRIER bb_to_present{};
            bb_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bb_to_present.Transition.pResource = backbuffer;
            bb_to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bb_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            bb_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            present_cmd_->ResourceBarrier(1, &bb_to_present);
        }
    }

    if (need_readback && readback_buf_) {
        D3D12_TEXTURE_COPY_LOCATION rb_dst{};
        rb_dst.pResource = readback_buf_;
        rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rb_dst.PlacedFootprint = footprint;
        present_cmd_->CopyTextureRegion(&rb_dst, 0, 0, 0, &src_loc, nullptr);
    }

    // Restore plane 0 back to caller_state.
    if (caller_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER back_barrier{};
        back_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        back_barrier.Transition.pResource = src_tex;
        back_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        back_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        back_barrier.Transition.StateAfter = caller_state;
        present_cmd_->ResourceBarrier(1, &back_barrier);
    }

    hr = present_cmd_->Close();
    if (backbuffer) {
        backbuffer->Release();
        backbuffer = nullptr;
    }
    if (FAILED(hr)) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
        return hr;
    }

    ID3D12CommandList* cmd_lists[] = {present_cmd_};
    host_queue_->ExecuteCommandLists(1, cmd_lists);

    if (use_swapchain && swapchain_) {
        const UINT sync_interval = pPresentParameters ? pPresentParameters->SyncInterval : 1u;
        hr = swapchain_->Present(sync_interval, 0);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }
    }

    hr = WaitForInternalFenceLocked();
    if (FAILED(hr)) {
        RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
        return hr;
    }

    if (need_readback && readback_buf_) {
        void* mapped = nullptr;
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
        hr = readback_buf_->Map(0, &read_range, &mapped);
        if (FAILED(hr) || !mapped) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, FAILED(hr) ? hr : E_FAIL);
            return FAILED(hr) ? hr : E_FAIL;
        }

        const auto* rb_bytes = static_cast<const uint8_t*>(mapped);
        const size_t packed_row_bytes = static_cast<size_t>(row_size_bytes);
        const size_t total_packed = packed_row_bytes * height;
        last_present_pixels_.resize(total_packed);

        const bool is_bgra = (src_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              src_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* src_row =
                rb_bytes + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
            uint8_t* dst_row = last_present_pixels_.data() + static_cast<size_t>(y) * packed_row_bytes;
            if (is_bgra) {
                for (uint32_t x = 0; x < width; ++x) {
                    dst_row[x * 4u + 0u] = src_row[x * 4u + 2u];
                    dst_row[x * 4u + 1u] = src_row[x * 4u + 1u];
                    dst_row[x * 4u + 2u] = src_row[x * 4u + 0u];
                    dst_row[x * 4u + 3u] = src_row[x * 4u + 3u];
                }
            } else {
                std::memcpy(dst_row, src_row, packed_row_bytes);
            }
        }

        D3D12_RANGE write_none{0, 0};
        readback_buf_->Unmap(0, &write_none);

        last_present_hash_ = openscarlett::ComputeFnv1a64(
            last_present_pixels_.data(), last_present_pixels_.size());
    }

    last_present_width_ = width;
    last_present_height_ = height;
    ++present_count_;

    RecordQueue("ID3D12CommandQueueX::PresentX", 19, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SuspendX(uint32_t Flags) {
    (void)Flags;
    suspended_.store(true, std::memory_order_release);
    RecordQueue("ID3D12CommandQueueX::SuspendX", 20, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::ResumeX() {
    suspended_.store(false, std::memory_order_release);
    RecordQueue("ID3D12CommandQueueX::ResumeX", 21, S_OK);
    return S_OK;
}

// IOpenScarlettPresentCapture
HRESULT STDMETHODCALLTYPE CommandQueueX::SetOutputWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    attached_hwnd_ = hwnd;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetReadbackCaptureEnabled(BOOL enabled) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    readback_capture_enabled_ = (enabled != FALSE);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::GetLastPresentStats(
    uint64_t* out_fnv1a_hash,
    uint32_t* out_width,
    uint32_t* out_height,
    uint64_t* out_present_count) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (out_fnv1a_hash) *out_fnv1a_hash = last_present_hash_;
    if (out_width) *out_width = last_present_width_;
    if (out_height) *out_height = last_present_height_;
    if (out_present_count) *out_present_count = present_count_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::CopyLastPresentPixels(
    uint8_t* out_rgba8,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (out_copied_bytes) *out_copied_bytes = 0;
    if (last_present_pixels_.empty()) return S_FALSE;
    if (!out_rgba8) return E_POINTER;
    const uint32_t to_copy = static_cast<uint32_t>(
        std::min<size_t>(buffer_bytes, last_present_pixels_.size()));
    std::memcpy(out_rgba8, last_present_pixels_.data(), to_copy);
    if (out_copied_bytes) *out_copied_bytes = to_copy;
    return (to_copy == last_present_pixels_.size()) ? S_OK : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

}  // namespace openscarlett::d3d12_xs
