#include "command_queue_x.h"

#include <algorithm>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>

#include "command_list_x.h"
#include "device_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace openscarlett::d3d12_xs {

struct WindowCallbackState {
    std::atomic<uint32_t> client_width{0};
    std::atomic<uint32_t> client_height{0};
    std::atomic<bool> is_minimized{false};
    std::atomic<bool> is_active{true};
    std::atomic<bool> close_requested{false};
    std::atomic<bool> resize_pending{false};
    std::atomic<bool> post_quit_on_close{false};
    std::atomic<bool> quit_posted{false};
    std::atomic<bool> is_fullscreen{false};
    std::atomic<bool> in_fullscreen_toggle{false};

    std::mutex fullscreen_mutex;
    LONG_PTR saved_style = 0;
    LONG_PTR saved_ex_style = 0;
    RECT saved_rect{};
    WINDOWPLACEMENT saved_placement{};
    bool has_saved_window_state = false;
};

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

struct SubclassEntry {
    std::shared_ptr<WindowCallbackState> state;
    WNDPROC orig_proc = nullptr;
};

struct SubclassRegistry {
    std::mutex mutex;
    std::unordered_map<HWND, SubclassEntry> map;
};

SubclassRegistry& GetSubclassRegistry() {
    static SubclassRegistry reg;
    return reg;
}

void RefreshClientMetrics(HWND hwnd, WindowCallbackState& state) noexcept {
    RECT cr{};
    if (GetClientRect(hwnd, &cr) != FALSE) {
        const uint32_t cw = (cr.right > cr.left) ? static_cast<uint32_t>(cr.right - cr.left) : 0u;
        const uint32_t ch = (cr.bottom > cr.top) ? static_cast<uint32_t>(cr.bottom - cr.top) : 0u;
        state.client_width.store(cw, std::memory_order_release);
        state.client_height.store(ch, std::memory_order_release);
        state.is_minimized.store(
            (IsIconic(hwnd) != FALSE) || cw == 0u || ch == 0u, std::memory_order_release);
    }
}

HRESULT ToggleBorderlessFullscreenForWindow(HWND hwnd, WindowCallbackState& state) {
    if (!hwnd || IsWindow(hwnd) == FALSE) {
        return E_HANDLE;
    }
    if (state.in_fullscreen_toggle.exchange(true, std::memory_order_acq_rel)) {
        return S_OK;
    }
    struct ScopeReset {
        std::atomic<bool>& flag;
        ~ScopeReset() { flag.store(false, std::memory_order_release); }
    } reset_guard{state.in_fullscreen_toggle};

    const bool was_fullscreen = state.is_fullscreen.load(std::memory_order_acquire);
    if (!was_fullscreen) {
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd, &wp);

        {
            std::lock_guard<std::mutex> lk(state.fullscreen_mutex);
            state.saved_style = style;
            state.saved_ex_style = ex_style;
            state.saved_rect = rect;
            state.saved_placement = wp;
            state.has_saved_window_state = true;
        }

        RECT target_rect = rect;
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi) != FALSE) {
            target_rect = mi.rcMonitor;
        } else {
            target_rect.left = 0;
            target_rect.top = 0;
            target_rect.right = GetSystemMetrics(SM_CXSCREEN);
            target_rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        }

        const bool was_visible = (style & WS_VISIBLE) != 0;
        const LONG_PTR new_style =
            static_cast<LONG_PTR>(WS_POPUP) | (was_visible ? static_cast<LONG_PTR>(WS_VISIBLE) : 0);
        const LONG_PTR ex_strip = static_cast<LONG_PTR>(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        const LONG_PTR new_ex_style = ex_style & ~ex_strip;

        SetWindowLongPtrW(hwnd, GWL_STYLE, new_style);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_ex_style);

        const UINT swp_flags = was_visible
                                   ? (SWP_FRAMECHANGED | SWP_NOOWNERZORDER)
                                   : (SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetWindowPos(
            hwnd,
            was_visible ? HWND_TOP : nullptr,
            target_rect.left,
            target_rect.top,
            target_rect.right - target_rect.left,
            target_rect.bottom - target_rect.top,
            swp_flags);

        state.is_fullscreen.store(true, std::memory_order_release);
        state.resize_pending.store(true, std::memory_order_release);
        RefreshClientMetrics(hwnd, state);
    } else {
        LONG_PTR saved_style = 0;
        LONG_PTR saved_ex_style = 0;
        RECT saved_rect{};
        WINDOWPLACEMENT saved_wp{};
        bool has_saved = false;
        {
            std::lock_guard<std::mutex> lk(state.fullscreen_mutex);
            saved_style = state.saved_style;
            saved_ex_style = state.saved_ex_style;
            saved_rect = state.saved_rect;
            saved_wp = state.saved_placement;
            has_saved = state.has_saved_window_state;
        }

        if (has_saved) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, saved_style);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, saved_ex_style);
            const bool restore_visible = (saved_style & WS_VISIBLE) != 0;
            const UINT swp_flags = restore_visible
                                       ? (SWP_FRAMECHANGED | SWP_NOOWNERZORDER)
                                       : (SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            SetWindowPos(
                hwnd,
                restore_visible ? HWND_NOTOPMOST : nullptr,
                saved_rect.left,
                saved_rect.top,
                saved_rect.right - saved_rect.left,
                saved_rect.bottom - saved_rect.top,
                swp_flags);
            if (restore_visible && saved_wp.showCmd == SW_SHOWMAXIMIZED) {
                SetWindowPlacement(hwnd, &saved_wp);
            }
        } else {
            const LONG_PTR cur_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            const bool cur_visible = (cur_style & WS_VISIBLE) != 0;
            const LONG_PTR fallback_style =
                static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW) |
                (cur_visible ? static_cast<LONG_PTR>(WS_VISIBLE) : 0);
            SetWindowLongPtrW(hwnd, GWL_STYLE, fallback_style);
            SetWindowPos(
                hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }

        state.is_fullscreen.store(false, std::memory_order_release);
        state.resize_pending.store(true, std::memory_order_release);
        RefreshClientMetrics(hwnd, state);
    }

    return S_OK;
}

LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    std::shared_ptr<WindowCallbackState> state;
    WNDPROC orig_proc = nullptr;
    {
        auto& reg = GetSubclassRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        auto it = reg.map.find(hwnd);
        if (it != reg.map.end()) {
            state = it->second.state;
            orig_proc = it->second.orig_proc;
        }
    }

    if (state) {
        switch (msg) {
            case WM_SIZE: {
                const uint32_t new_w = static_cast<uint32_t>(LOWORD(lParam));
                const uint32_t new_h = static_cast<uint32_t>(HIWORD(lParam));
                const bool min_now = (wParam == SIZE_MINIMIZED) || (new_w == 0u) || (new_h == 0u);
                state->client_width.store(new_w, std::memory_order_release);
                state->client_height.store(new_h, std::memory_order_release);
                state->is_minimized.store(min_now, std::memory_order_release);
                state->resize_pending.store(true, std::memory_order_release);
                break;
            }
            case WM_ACTIVATEAPP: {
                state->is_active.store(wParam != FALSE, std::memory_order_release);
                break;
            }
            case WM_SYSKEYDOWN: {
                const bool alt_down = (lParam & (static_cast<LPARAM>(1) << 29)) != 0;
                const bool is_repeat = (lParam & (static_cast<LPARAM>(1) << 30)) != 0;
                if (wParam == VK_RETURN && alt_down && !is_repeat) {
                    ToggleBorderlessFullscreenForWindow(hwnd, *state);
                    return 0;
                }
                break;
            }
            case WM_SYSCHAR: {
                const bool alt_down = (lParam & (static_cast<LPARAM>(1) << 29)) != 0;
                if (wParam == VK_RETURN && alt_down) {
                    return 0;
                }
                break;
            }
            case WM_CLOSE: {
                state->close_requested.store(true, std::memory_order_release);
                if (state->post_quit_on_close.load(std::memory_order_acquire)) {
                    if (!state->quit_posted.exchange(true, std::memory_order_acq_rel)) {
                        PostQuitMessage(0);
                    }
                }
                break;
            }
            case WM_DESTROY: {
                state->close_requested.store(true, std::memory_order_release);
                break;
            }
            case WM_NCDESTROY: {
                LRESULT res = (orig_proc && orig_proc != &SubclassWndProc)
                                  ? CallWindowProcW(orig_proc, hwnd, msg, wParam, lParam)
                                  : DefWindowProcW(hwnd, msg, wParam, lParam);
                {
                    auto& reg = GetSubclassRegistry();
                    std::lock_guard<std::mutex> lk(reg.mutex);
                    auto it = reg.map.find(hwnd);
                    if (it != reg.map.end() && it->second.state == state) {
                        reg.map.erase(it);
                    }
                }
                if (orig_proc && orig_proc != &SubclassWndProc &&
                    GetWindowLongPtrW(hwnd, GWLP_WNDPROC) ==
                        reinterpret_cast<LONG_PTR>(&SubclassWndProc)) {
                    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig_proc));
                }
                return res;
            }
            default:
                break;
        }
    }

    if (orig_proc && orig_proc != &SubclassWndProc) {
        return CallWindowProcW(orig_proc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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

int64_t QueryQpcNow() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

}  // namespace

CommandQueueX::CommandQueueX(
    DeviceX* parent_device,
    ID3D12CommandQueue* host_queue,
    HWND initial_hwnd)
    : device_x_(parent_device),
      host_queue_(host_queue),
      attached_hwnd_(initial_hwnd),
      window_state_(std::make_shared<WindowCallbackState>()) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpc_freq_ = freq.QuadPart;

    if (device_x_) device_x_->AddRef();
    if (host_queue_) host_queue_->AddRef();
    {
        auto& reg = GetQueueRegistry();
        std::unique_lock<std::shared_mutex> lock(reg.mutex);
        reg.map[static_cast<const ID3D12Pageable*>(this)] = host_queue_;
    }
    if (initial_hwnd && IsWindow(initial_hwnd) != FALSE) {
        std::lock_guard<std::mutex> lock(present_mutex_);
        AttachWindowSubclassLocked(initial_hwnd);
    }
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
    if (IsEqualIID(riid, IID_IOpenScarlettPresentWindow)) {
        *ppvObject = static_cast<IOpenScarlettPresentWindow*>(this);
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

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE CommandQueueX::GetDesc(D3D12_COMMAND_QUEUE_DESC* pRet) {
    D3D12_COMMAND_QUEUE_DESC* desc = host_queue_->GetDesc(pRet);
    RecordQueue("ID3D12CommandQueueX::GetDesc", 18, S_OK);
    return desc;
}
#else
D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE CommandQueueX::GetDesc() {
    const D3D12_COMMAND_QUEUE_DESC desc = host_queue_->GetDesc();
    RecordQueue("ID3D12CommandQueueX::GetDesc", 18, S_OK);
    return desc;
}
#endif

// Window subclass management
void CommandQueueX::AttachWindowSubclassLocked(HWND hwnd) {
    if (subclassed_hwnd_ == hwnd) {
        return;
    }
    DetachWindowSubclassLocked();

    if (!hwnd || IsWindow(hwnd) == FALSE) {
        return;
    }

    const bool keep_post_quit =
        window_state_ ? window_state_->post_quit_on_close.load(std::memory_order_acquire) : false;
    window_state_ = std::make_shared<WindowCallbackState>();
    window_state_->post_quit_on_close.store(keep_post_quit, std::memory_order_release);
    RefreshClientMetrics(hwnd, *window_state_);

    auto& reg = GetSubclassRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);

    auto existing = reg.map.find(hwnd);
    WNDPROC cur_proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (cur_proc == &SubclassWndProc && existing != reg.map.end()) {
        existing->second.state = window_state_;
        subclassed_hwnd_ = hwnd;
        return;
    }

    SubclassEntry entry{};
    entry.state = window_state_;
    entry.orig_proc = cur_proc;
    reg.map[hwnd] = entry;

    SetLastError(0);
    const LONG_PTR prev = SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassWndProc));
    if (prev == 0 && GetLastError() != 0) {
        reg.map.erase(hwnd);
        return;
    }
    reg.map[hwnd].orig_proc = reinterpret_cast<WNDPROC>(prev);
    subclassed_hwnd_ = hwnd;
}

void CommandQueueX::DetachWindowSubclassLocked() {
    HWND old_hwnd = subclassed_hwnd_;
    subclassed_hwnd_ = nullptr;
    if (!old_hwnd) {
        return;
    }

    WNDPROC orig_proc = nullptr;
    {
        auto& reg = GetSubclassRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        auto it = reg.map.find(old_hwnd);
        if (it != reg.map.end() && it->second.state == window_state_) {
            orig_proc = it->second.orig_proc;
            reg.map.erase(it);
        }
    }

    if (orig_proc && orig_proc != &SubclassWndProc && IsWindow(old_hwnd) != FALSE) {
        if (GetWindowLongPtrW(old_hwnd, GWLP_WNDPROC) ==
            reinterpret_cast<LONG_PTR>(&SubclassWndProc)) {
            SetWindowLongPtrW(old_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig_proc));
        }
    }
}

void CommandQueueX::ReleaseSwapChainLocked() {
    if (present_fence_ && host_queue_) {
        WaitForInternalFenceLocked();
    }
    if (frame_latency_waitable_object_) {
        CloseHandle(frame_latency_waitable_object_);
        frame_latency_waitable_object_ = nullptr;
    }
    if (swapchain_) {
        swapchain_->Release();
        swapchain_ = nullptr;
    }
    swapchain_hwnd_ = nullptr;
    swapchain_width_ = 0;
    swapchain_height_ = 0;
    swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    swapchain_flags_ = 0;
    swapchain_buffer_count_ = 0;
    swapchain_max_latency_ = 0;
    swapchain_occluded_ = false;
    last_paced_qpc_ = 0;
}

// PresentX internal helpers
HRESULT CommandQueueX::EnsurePresentHelperObjectsLocked() {
    if (!pacing_timer_) {
        pacing_timer_ = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!pacing_timer_) {
            pacing_timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
    }

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
    HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format, bool force_resize) {
    const DXGI_FORMAT sc_fmt = NormalizeSwapChainFormat(format);
    const uint32_t req_max_latency =
        device_x_ ? std::clamp(device_x_->MaxFrameLatency(), 1u, 3u) : 2u;
    const uint32_t req_buffer_count = std::clamp(req_max_latency + 1u, 2u, 3u);

    if (swapchain_ && swapchain_hwnd_ == hwnd) {
        if (!force_resize &&
            swapchain_width_ == width &&
            swapchain_height_ == height &&
            swapchain_format_ == sc_fmt) {
            // Apply dynamic SetFrameIntervalX max-frame-latency updates cleanly.
            if ((swapchain_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
                const uint32_t clamped_lat =
                    std::clamp(req_max_latency, 1u, std::max(swapchain_buffer_count_, 1u));
                if (clamped_lat != swapchain_max_latency_) {
                    if (SUCCEEDED(swapchain_->SetMaximumFrameLatency(clamped_lat))) {
                        swapchain_max_latency_ = clamped_lat;
                    }
                }
            }
            return S_OK;
        }

        WaitForInternalFenceLocked();
        const UINT resize_bufs =
            (swapchain_buffer_count_ >= 2u) ? swapchain_buffer_count_ : req_buffer_count;
        HRESULT resize_hr = swapchain_->ResizeBuffers(
            resize_bufs, width, height, sc_fmt, swapchain_flags_);
        if (SUCCEEDED(resize_hr)) {
            swapchain_width_ = width;
            swapchain_height_ = height;
            swapchain_format_ = sc_fmt;
            swapchain_occluded_ = false;
            return S_OK;
        }
        // Resize failed — recreate cleanly.
        ReleaseSwapChainLocked();
    } else if (swapchain_) {
        ReleaseSwapChainLocked();
    }

    IDXGIFactory4* factory4 = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory4));
    if (FAILED(hr) || !factory4) {
        return hr;
    }

    bool allow_tearing = false;
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory4->QueryInterface(IID_PPV_ARGS(&factory5))) && factory5) {
        BOOL tearing_supported = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &tearing_supported,
                sizeof(tearing_supported)))) {
            allow_tearing = (tearing_supported != FALSE);
        }
        factory5->Release();
    }

    // Build ordered fallback flag list: waitable+tearing -> waitable -> tearing -> 0
    uint32_t flag_candidates[4]{};
    uint32_t candidate_count = 0;
    if (allow_tearing) {
        flag_candidates[candidate_count++] =
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    flag_candidates[candidate_count++] = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (allow_tearing) {
        flag_candidates[candidate_count++] = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    flag_candidates[candidate_count++] = 0u;

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = sc_fmt;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = req_buffer_count;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1* sc1 = nullptr;
    uint32_t chosen_flags = 0u;
    hr = E_FAIL;
    for (uint32_t i = 0; i < candidate_count; ++i) {
        sc_desc.Flags = flag_candidates[i];
        hr = factory4->CreateSwapChainForHwnd(host_queue_, hwnd, &sc_desc, nullptr, nullptr, &sc1);
        if (SUCCEEDED(hr) && sc1) {
            chosen_flags = flag_candidates[i];
            break;
        }
    }

    // Disable DXGI's built-in exclusive fullscreen Alt+Enter interception.
    factory4->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
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
        swapchain_flags_ = chosen_flags;
        swapchain_buffer_count_ = req_buffer_count;
        swapchain_occluded_ = false;

        if ((chosen_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
            const uint32_t clamped_lat = std::clamp(req_max_latency, 1u, req_buffer_count);
            if (SUCCEEDED(swapchain_->SetMaximumFrameLatency(clamped_lat))) {
                swapchain_max_latency_ = clamped_lat;
            } else {
                swapchain_max_latency_ = 1u;
            }
            frame_latency_waitable_object_ = swapchain_->GetFrameLatencyWaitableObject();
        } else {
            swapchain_max_latency_ = req_max_latency;
        }
    }
    return hr;
}

void CommandQueueX::PaceFrameLocked(uint32_t frame_interval_us) {
    if (qpc_freq_ <= 0 || frame_interval_us == 0u) {
        return;
    }

    const int64_t now_qpc = QueryQpcNow();
    if (last_paced_qpc_ == 0) {
        last_paced_qpc_ = now_qpc;
        last_present_qpc_ = now_qpc;
        last_frame_ms_ = 0.0;
        return;
    }

    const int64_t interval_ticks =
        (static_cast<int64_t>(frame_interval_us) * qpc_freq_) / 1000000LL;
    const int64_t target_qpc = last_paced_qpc_ + interval_ticks;

    int64_t cur_qpc = now_qpc;
    if (cur_qpc < target_qpc) {
        const int64_t rem_us = ((target_qpc - cur_qpc) * 1000000LL) / qpc_freq_;
        if (rem_us > 2000LL) {
            const int64_t sleep_us = rem_us - 1500LL;
            if (pacing_timer_) {
                LARGE_INTEGER due{};
                due.QuadPart = -(sleep_us * 10LL);
                if (SetWaitableTimer(pacing_timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
                    WaitForSingleObject(pacing_timer_, 200u);
                }
            } else {
                const DWORD sleep_ms = static_cast<DWORD>(sleep_us / 1000LL);
                if (sleep_ms > 0u) {
                    Sleep(sleep_ms);
                }
            }
        }
        for (;;) {
            cur_qpc = QueryQpcNow();
            if (cur_qpc >= target_qpc) {
                break;
            }
            YieldProcessor();
        }
    }

    last_frame_ms_ =
        static_cast<double>(cur_qpc - last_present_qpc_) * 1000.0 / static_cast<double>(qpc_freq_);
    last_present_qpc_ = cur_qpc;

    // Preserve phase when on time; resync cleanly if a frame overran by more than one interval.
    if (cur_qpc <= target_qpc + interval_ticks) {
        last_paced_qpc_ = target_qpc;
    } else {
        last_paced_qpc_ = cur_qpc;
    }
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
    DetachWindowSubclassLocked();
    ReleaseSwapChainLocked();
    if (readback_buf_) { readback_buf_->Release(); readback_buf_ = nullptr; }
    if (present_cmd_) { present_cmd_->Release(); present_cmd_ = nullptr; }
    if (present_alloc_) { present_alloc_->Release(); present_alloc_ = nullptr; }
    if (present_fence_) { present_fence_->Release(); present_fence_ = nullptr; }
    if (present_fence_event_) { CloseHandle(present_fence_event_); present_fence_event_ = nullptr; }
    if (pacing_timer_) { CloseHandle(pacing_timer_); pacing_timer_ = nullptr; }
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

    // Ensure window subclass is active and pump pending Win32 messages WITHOUT holding present_mutex_.
    HWND pump_hwnd = nullptr;
    std::shared_ptr<WindowCallbackState> pump_state;
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        pump_hwnd = (pPresentParameters && pPresentParameters->OutputWindow)
                        ? pPresentParameters->OutputWindow
                        : attached_hwnd_;
        if (pump_hwnd && IsWindow(pump_hwnd) != FALSE) {
            AttachWindowSubclassLocked(pump_hwnd);
            pump_state = window_state_;
        }
    }

    if (pump_hwnd && IsWindow(pump_hwnd) != FALSE &&
        GetWindowThreadProcessId(pump_hwnd, nullptr) == GetCurrentThreadId()) {
        MSG msg{};
        constexpr uint32_t kMaxMessagesPerPresent = 64u;
        for (uint32_t i = 0; i < kMaxMessagesPerPresent &&
                             PeekMessageW(&msg, pump_hwnd, 0, 0, PM_REMOVE) != FALSE;
             ++i) {
            if (msg.message == WM_QUIT) {
                if (pump_state) {
                    pump_state->close_requested.store(true, std::memory_order_release);
                }
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

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

    bool window_minimized = false;
    uint32_t target_bb_width = width;
    uint32_t target_bb_height = height;
    if (use_swapchain) {
        AttachWindowSubclassLocked(target_hwnd);
        RECT cr{};
        const bool got_cr = (GetClientRect(target_hwnd, &cr) != FALSE);
        const uint32_t cw =
            (got_cr && cr.right > cr.left) ? static_cast<uint32_t>(cr.right - cr.left) : 0u;
        const uint32_t ch =
            (got_cr && cr.bottom > cr.top) ? static_cast<uint32_t>(cr.bottom - cr.top) : 0u;
        const bool iconic = (IsIconic(target_hwnd) != FALSE);
        const bool cb_min =
            window_state_ ? window_state_->is_minimized.load(std::memory_order_acquire) : false;
        window_minimized = iconic || cb_min || (cw == 0u || ch == 0u);
        if (!window_minimized) {
            target_bb_width = cw;
            target_bb_height = ch;
        }
    }

    const bool do_swapchain_present = use_swapchain && !window_minimized;
    const bool need_readback = (!use_swapchain) || readback_capture_enabled_;

    if (do_swapchain_present) {
        const bool resize_pending =
            window_state_
                ? window_state_->resize_pending.exchange(false, std::memory_order_acq_rel)
                : false;
        hr = EnsureSwapChainLocked(
            target_hwnd, target_bb_width, target_bb_height, src_desc.Format, resize_pending);
        if (FAILED(hr)) {
            RecordQueue("ID3D12CommandQueueX::PresentX", 19, hr);
            return hr;
        }

        if (frame_latency_waitable_object_) {
            const DWORD wr = WaitForSingleObjectEx(frame_latency_waitable_object_, 1000u, TRUE);
            if (wr == WAIT_OBJECT_0) {
                ++latency_waits_;
            }
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

    if (do_swapchain_present || need_readback) {
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
        if (do_swapchain_present && swapchain_) {
            const UINT bb_idx = swapchain_->GetCurrentBackBufferIndex();
            hr = swapchain_->GetBuffer(bb_idx, IID_PPV_ARGS(&backbuffer));
            if (SUCCEEDED(hr) && backbuffer) {
                const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
                const uint32_t copy_w =
                    std::min<uint32_t>(width, static_cast<uint32_t>(bb_desc.Width));
                const uint32_t copy_h = std::min<uint32_t>(height, bb_desc.Height);

                D3D12_RESOURCE_BARRIER bb_to_dst{};
                bb_to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb_to_dst.Transition.pResource = backbuffer;
                bb_to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb_to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                bb_to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                present_cmd_->ResourceBarrier(1, &bb_to_dst);

                if (copy_w > 0u && copy_h > 0u) {
                    D3D12_TEXTURE_COPY_LOCATION bb_dst{};
                    bb_dst.pResource = backbuffer;
                    bb_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    bb_dst.SubresourceIndex = 0;

                    D3D12_BOX src_box{};
                    src_box.left = 0u;
                    src_box.top = 0u;
                    src_box.front = 0u;
                    src_box.right = copy_w;
                    src_box.bottom = copy_h;
                    src_box.back = 1u;

                    present_cmd_->CopyTextureRegion(&bb_dst, 0, 0, 0, &src_loc, &src_box);
                }

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
    }

    const uint32_t frame_interval_us = device_x_ ? device_x_->FrameIntervalMicroseconds() : 0u;
    const bool immediate_flag =
        (pPresentParameters && (pPresentParameters->Flags & D3D12XBOX_PRESENT_FLAG_IMMEDIATE) != 0);
    const bool should_pace = do_swapchain_present && !immediate_flag && frame_interval_us > 0u;

    if (should_pace) {
        PaceFrameLocked(frame_interval_us);
    } else {
        const int64_t now_qpc = QueryQpcNow();
        if (last_present_qpc_ != 0 && qpc_freq_ > 0) {
            last_frame_ms_ = static_cast<double>(now_qpc - last_present_qpc_) * 1000.0 /
                             static_cast<double>(qpc_freq_);
        }
        last_present_qpc_ = now_qpc;
        last_paced_qpc_ = 0;
    }

    if (do_swapchain_present && swapchain_) {
        const UINT present_flags =
            ((swapchain_flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0)
                ? DXGI_PRESENT_ALLOW_TEARING
                : 0u;
        hr = swapchain_->Present(0u, present_flags);
        if (FAILED(hr) && present_flags != 0u) {
            hr = swapchain_->Present(0u, 0u);
        }
        if (hr == DXGI_STATUS_OCCLUDED) {
            swapchain_occluded_ = true;
            hr = S_OK;
        } else if (SUCCEEDED(hr)) {
            swapchain_occluded_ = false;
        }
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
            uint8_t* dst_row =
                last_present_pixels_.data() + static_cast<size_t>(y) * packed_row_bytes;
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
    if (attached_hwnd_ != hwnd ||
        subclassed_hwnd_ != hwnd ||
        (swapchain_ && swapchain_hwnd_ != hwnd)) {
        if (swapchain_ && swapchain_hwnd_ != hwnd) {
            ReleaseSwapChainLocked();
        }
        attached_hwnd_ = hwnd;
        if (hwnd && IsWindow(hwnd) != FALSE) {
            AttachWindowSubclassLocked(hwnd);
        } else {
            DetachWindowSubclassLocked();
        }
    }
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

// IOpenScarlettPresentWindow
HRESULT STDMETHODCALLTYPE CommandQueueX::GetPresentWindowState(
    D3D12XBOX_PRESENT_WINDOW_STATE* out_state) {
    if (!out_state) return E_POINTER;
    std::memset(out_state, 0, sizeof(*out_state));

    std::lock_guard<std::mutex> lock(present_mutex_);
    HWND effective_hwnd = subclassed_hwnd_ ? subclassed_hwnd_ : attached_hwnd_;
    const bool has_win = (effective_hwnd != nullptr && IsWindow(effective_hwnd) != FALSE);

    if (window_state_) {
        out_state->is_fullscreen = window_state_->is_fullscreen.load(std::memory_order_acquire) ? 1u : 0u;
        const bool cb_min = window_state_->is_minimized.load(std::memory_order_acquire);
        const bool iconic = has_win && (IsIconic(effective_hwnd) != FALSE);
        out_state->is_minimized = (cb_min || iconic) ? 1u : 0u;
        out_state->is_active = window_state_->is_active.load(std::memory_order_acquire) ? 1u : 0u;
        out_state->close_requested = window_state_->close_requested.load(std::memory_order_acquire) ? 1u : 0u;
        if (has_win || subclassed_hwnd_ != nullptr) {
            out_state->width = window_state_->client_width.load(std::memory_order_acquire);
            out_state->height = window_state_->client_height.load(std::memory_order_acquire);
        } else {
            out_state->width = swapchain_width_ ? swapchain_width_ : last_present_width_;
            out_state->height = swapchain_height_ ? swapchain_height_ : last_present_height_;
        }
    } else {
        out_state->width = swapchain_width_ ? swapchain_width_ : last_present_width_;
        out_state->height = swapchain_height_ ? swapchain_height_ : last_present_height_;
        out_state->is_active = 1u;
    }

    out_state->swapchain_flags = swapchain_flags_;
    out_state->frame_interval_us = device_x_ ? device_x_->FrameIntervalMicroseconds() : 0u;
    out_state->max_frame_latency = device_x_ ? device_x_->MaxFrameLatency() : 1u;
    out_state->present_count = present_count_;
    out_state->latency_waits = latency_waits_;
    out_state->last_frame_ms = last_frame_ms_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::ToggleBorderlessFullscreen() {
    HWND target_hwnd = nullptr;
    std::shared_ptr<WindowCallbackState> state;
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        target_hwnd = subclassed_hwnd_ ? subclassed_hwnd_ : attached_hwnd_;
        if (!target_hwnd || IsWindow(target_hwnd) == FALSE) {
            return E_HANDLE;
        }
        AttachWindowSubclassLocked(target_hwnd);
        state = window_state_;
    }
    if (!state) {
        return E_FAIL;
    }
    return ToggleBorderlessFullscreenForWindow(target_hwnd, *state);
}

HANDLE STDMETHODCALLTYPE CommandQueueX::GetFrameLatencyWaitableObject() {
    std::lock_guard<std::mutex> lock(present_mutex_);
    return frame_latency_waitable_object_;
}

HRESULT STDMETHODCALLTYPE CommandQueueX::SetWindowCloseBehavior(BOOL post_quit_on_close) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (!window_state_) {
        window_state_ = std::make_shared<WindowCallbackState>();
    }
    window_state_->post_quit_on_close.store(post_quit_on_close != FALSE, std::memory_order_release);
    window_state_->quit_posted.store(false, std::memory_order_release);
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
