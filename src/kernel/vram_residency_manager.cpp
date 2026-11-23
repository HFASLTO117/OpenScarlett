#include "openscarlett/kernel/vram_residency_manager.h"

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "openscarlett/gpu/d3d12_adapter.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#if defined(_MSC_VER)
#define OS_UUIDOF(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_UUIDOF(T) __mingw_uuidof<T>()
#else
#define OS_UUIDOF(T) IID_##T
#endif
#endif

namespace openscarlett {

namespace {

uint64_t AlignUpBytes(uint64_t value, uint64_t alignment) noexcept {
    if (alignment <= 1ull) return value;
    const uint64_t rem = value % alignment;
    if (rem == 0ull) return value;
    if (value > UINT64_MAX - (alignment - rem)) return value;
    return value + (alignment - rem);
}

#if defined(_WIN32)
HRESULT CreateD3D12BufferLocal(
    ID3D12Device* device,
    D3D12_HEAP_TYPE heap_type,
    uint64_t size_bytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** out_res) {
    if (!device || !out_res || size_bytes == 0ull) return E_INVALIDARG;
    *out_res = nullptr;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0ull;
    rd.Width = std::max<uint64_t>(size_bytes, 256ull);
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1u;
    rd.SampleDesc.Quality = 0u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;

    return device->CreateCommittedResource(
        &hp,
        D3D12_HEAP_FLAG_NONE,
        &rd,
        initial_state,
        nullptr,
        OS_UUIDOF(ID3D12Resource),
        reinterpret_cast<void**>(out_res));
}

void BarrierTransition(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before_state,
    D3D12_RESOURCE_STATES after_state) noexcept {
    if (!cmd || !res || before_state == after_state) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before_state;
    b.Transition.StateAfter = after_state;
    cmd->ResourceBarrier(1u, &b);
}
#endif

}  // namespace

struct VramResidencyManager::Impl {
    struct Entry {
        uint64_t handle = 0ull;
        uint64_t size_bytes = 0ull;
        uint64_t aligned_size_bytes = 0ull;
        std::string debug_name;
        bool allow_uav = false;
        bool is_resident = false;
        bool gpu_evicted_flag = false;
        uint32_t pin_count = 0u;
        uint32_t active_ref_count = 0u;
        uint64_t in_flight_fence_value = 0ull;
        uint64_t eviction_count = 0ull;
        uint64_t page_in_count = 0ull;
        uint64_t last_touch_tick = 0ull;
        std::vector<uint8_t> system_ram_backing;
        std::list<uint64_t>::iterator lru_it;
        bool in_lru = false;
#if defined(_WIN32)
        ID3D12Resource* gpu_resource = nullptr;
        D3D12_RESOURCE_STATES gpu_state = D3D12_RESOURCE_STATE_COMMON;
#endif
    };

    mutable std::mutex mutex;
    bool initialized = false;
    VramResidencyConfig config{};
    uint64_t next_handle = 1ull;
    uint64_t touch_clock = 0ull;

    // Front = MRU, Back = LRU
    std::list<uint64_t> lru_list;
    std::unordered_map<uint64_t, Entry> entries;

    uint64_t resident_bytes = 0ull;
    uint64_t evicted_bytes = 0ull;
    uint64_t total_virtual_bytes = 0ull;
    uint64_t pinned_bytes = 0ull;
    uint64_t active_referenced_bytes = 0ull;
    uint64_t peak_resident_bytes = 0ull;
    uint64_t peak_virtual_bytes = 0ull;
    uint64_t resident_allocations = 0ull;
    uint64_t evicted_allocations = 0ull;
    uint64_t total_allocations = 0ull;
    uint64_t total_evictions = 0ull;
    uint64_t total_page_ins = 0ull;
    uint64_t total_bytes_paged_out = 0ull;
    uint64_t total_bytes_paged_in = 0ull;
    uint64_t d3d12_evict_calls = 0ull;
    uint64_t d3d12_make_resident_calls = 0ull;
    uint64_t oom_prevented_count = 0ull;
    uint64_t skipped_active_live_heaps = 0ull;
    uint64_t completed_fence_value = 0ull;

    bool is_warp = false;
    std::string adapter_name;

#if defined(_WIN32)
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmd_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0ull;

    Status ExecuteAndWaitLocked() {
        if (!queue || !cmd_list || !fence || !fence_event) {
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager D3D12 runtime not initialized");
        }
        if (FAILED(cmd_list->Close())) {
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager Close(cmd_list) failed");
        }
        ID3D12CommandList* lists[] = {cmd_list};
        queue->ExecuteCommandLists(1u, lists);
        const uint64_t wait_val = ++fence_value;
        if (FAILED(queue->Signal(fence, wait_val))) {
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager Queue::Signal failed");
        }
        if (fence->GetCompletedValue() < wait_val) {
            if (FAILED(fence->SetEventOnCompletion(wait_val, fence_event))) {
                return Status::Error(StatusCode::DeviceError, "VramResidencyManager SetEventOnCompletion failed");
            }
            const DWORD wr = WaitForSingleObject(fence_event, 30000u);
            if (wr != WAIT_OBJECT_0) {
                return Status::Error(StatusCode::DeviceError, "VramResidencyManager GPU fence wait timed out");
            }
        }
        return Status::Ok();
    }

    Status UploadToGpuLocked(Entry& e, uint64_t offset, const void* data, uint64_t size_bytes) {
        if (!device || !e.gpu_resource || size_bytes == 0ull) return Status::Ok();

        ID3D12Resource* staging = nullptr;
        const HRESULT hr = CreateD3D12BufferLocal(
            device,
            D3D12_HEAP_TYPE_UPLOAD,
            size_bytes,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            &staging);
        if (FAILED(hr) || !staging) {
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager upload staging creation failed");
        }

        void* mapped = nullptr;
        const D3D12_RANGE empty_read{0, 0};
        if (FAILED(staging->Map(0u, &empty_read, &mapped)) || !mapped) {
            staging->Release();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager upload staging Map failed");
        }
        std::memcpy(mapped, data, static_cast<size_t>(size_bytes));
        staging->Unmap(0u, nullptr);

        if (FAILED(allocator->Reset()) || FAILED(cmd_list->Reset(allocator, nullptr))) {
            staging->Release();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager command list Reset failed");
        }
        BarrierTransition(cmd_list, e.gpu_resource, e.gpu_state, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd_list->CopyBufferRegion(e.gpu_resource, offset, staging, 0ull, size_bytes);
        BarrierTransition(cmd_list, e.gpu_resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        e.gpu_state = D3D12_RESOURCE_STATE_COMMON;

        const Status st = ExecuteAndWaitLocked();
        staging->Release();
        return st;
    }

    Status ReadbackFromGpuLocked(Entry& e, uint64_t offset, void* out_data, uint64_t size_bytes) {
        if (!device || !e.gpu_resource || size_bytes == 0ull) return Status::Ok();

        ID3D12Resource* staging = nullptr;
        const HRESULT hr = CreateD3D12BufferLocal(
            device,
            D3D12_HEAP_TYPE_READBACK,
            size_bytes,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &staging);
        if (FAILED(hr) || !staging) {
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager readback staging creation failed");
        }

        if (FAILED(allocator->Reset()) || FAILED(cmd_list->Reset(allocator, nullptr))) {
            staging->Release();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager command list Reset failed");
        }
        BarrierTransition(cmd_list, e.gpu_resource, e.gpu_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd_list->CopyBufferRegion(staging, 0ull, e.gpu_resource, offset, size_bytes);
        BarrierTransition(cmd_list, e.gpu_resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        e.gpu_state = D3D12_RESOURCE_STATE_COMMON;

        const Status st = ExecuteAndWaitLocked();
        if (!st.ok()) {
            staging->Release();
            return st;
        }

        const void* mapped = nullptr;
        const D3D12_RANGE read_range{0, static_cast<SIZE_T>(size_bytes)};
        if (FAILED(staging->Map(0u, &read_range, const_cast<void**>(&mapped))) || !mapped) {
            staging->Release();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager readback staging Map failed");
        }
        std::memcpy(out_data, mapped, static_cast<size_t>(size_bytes));
        const D3D12_RANGE empty_write{0, 0};
        staging->Unmap(0u, &empty_write);
        staging->Release();
        return Status::Ok();
    }

    void ReleaseD3D12RuntimeLocked() {
        for (auto& kv : entries) {
            if (kv.second.gpu_resource) {
                kv.second.gpu_resource->Release();
                kv.second.gpu_resource = nullptr;
            }
        }
        if (cmd_list) {
            cmd_list->Release();
            cmd_list = nullptr;
        }
        if (allocator) {
            allocator->Release();
            allocator = nullptr;
        }
        if (queue) {
            queue->Release();
            queue = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
    }
#endif

    void MoveToMruLocked(Entry& e) {
        if (e.in_lru) {
            lru_list.erase(e.lru_it);
        }
        lru_list.push_front(e.handle);
        e.lru_it = lru_list.begin();
        e.in_lru = true;
        e.last_touch_tick = ++touch_clock;
    }

    void RemoveFromLruLocked(Entry& e) {
        if (e.in_lru) {
            lru_list.erase(e.lru_it);
            e.in_lru = false;
        }
    }

    bool IsEntryLiveProtectedLocked(const Entry& e) const noexcept {
        return e.pin_count > 0u ||
               e.active_ref_count > 0u ||
               e.in_flight_fence_value > completed_fence_value;
    }

    Status EvictEntryLocked(Entry& e, bool force_release_gpu) {
        if (!e.is_resident) return Status::Ok();
        if (IsEntryLiveProtectedLocked(e)) {
            ++skipped_active_live_heaps;
            return Status::Error(StatusCode::InvalidArgument,
                                 "Cannot evict pinned, actively-referenced, or in-flight VRAM allocation");
        }

        // 1. Copy GPU contents back to system RAM backing store before evicting so no bytes are lost.
        e.system_ram_backing.resize(static_cast<size_t>(e.size_bytes));
#if defined(_WIN32)
        if (device && e.gpu_resource) {
            Status rb_st = ReadbackFromGpuLocked(e, 0ull, e.system_ram_backing.data(), e.size_bytes);
            if (!rb_st.ok()) {
                return rb_st;
            }

            // 2. Evict from WDDM/D3D12 residency via ID3D12Device::Evict.
            ID3D12Pageable* pageable = e.gpu_resource;
            const HRESULT ev_hr = device->Evict(1u, &pageable);
            if (SUCCEEDED(ev_hr)) {
                ++d3d12_evict_calls;
                e.gpu_evicted_flag = true;
            }
            if (config.release_gpu_resource_on_evict || force_release_gpu) {
                e.gpu_resource->Release();
                e.gpu_resource = nullptr;
                e.gpu_evicted_flag = false;
            }
        }
#else
        (void)force_release_gpu;
#endif

        e.is_resident = false;
        ++e.eviction_count;
        RemoveFromLruLocked(e);

        if (resident_bytes >= e.aligned_size_bytes) {
            resident_bytes -= e.aligned_size_bytes;
        } else {
            resident_bytes = 0ull;
        }
        evicted_bytes += e.aligned_size_bytes;
        if (resident_allocations > 0ull) --resident_allocations;
        ++evicted_allocations;
        ++total_evictions;
        total_bytes_paged_out += e.size_bytes;
        return Status::Ok();
    }

    Status EvictToBudgetLocked(
        uint64_t target_budget_bytes,
        uint64_t protect_handle,
        bool force_release_gpu,
        uint64_t* out_evicted_count,
        uint64_t* out_evicted_bytes) {
        uint64_t evicted_cnt = 0ull;
        uint64_t evicted_sz = 0ull;

        while (resident_bytes > target_budget_bytes) {
            uint64_t victim_handle = 0ull;
            // Scan from LRU (back) to MRU (front) for an unpinned, unreferenced, idle resident victim.
            for (auto it = lru_list.rbegin(); it != lru_list.rend(); ++it) {
                if (*it == protect_handle) continue;
                auto found = entries.find(*it);
                if (found != entries.end() && found->second.is_resident) {
                    if (IsEntryLiveProtectedLocked(found->second)) {
                        ++skipped_active_live_heaps;
                        continue;
                    }
                    victim_handle = *it;
                    break;
                }
            }
            if (victim_handle == 0ull) {
                break;
            }
            Entry& victim = entries.at(victim_handle);
            const uint64_t sz = victim.aligned_size_bytes;
            Status st = EvictEntryLocked(victim, force_release_gpu);
            if (!st.ok()) {
                return st;
            }
            ++evicted_cnt;
            evicted_sz += sz;
        }

        if (out_evicted_count) *out_evicted_count = evicted_cnt;
        if (out_evicted_bytes) *out_evicted_bytes = evicted_sz;
        return Status::Ok();
    }

    Status MakeEntryResidentLocked(Entry& e) {
        if (e.is_resident) {
            MoveToMruLocked(e);
            return Status::Ok();
        }

        // Enforce budget BEFORE paging in (leaving room for e.aligned_size_bytes).
        if (config.auto_evict_on_pressure && config.vram_budget_bytes > 0ull) {
            const uint64_t target = (config.vram_budget_bytes > e.aligned_size_bytes)
                                        ? (config.vram_budget_bytes - e.aligned_size_bytes)
                                        : 0ull;
            if (resident_bytes > target) {
                uint64_t ev_cnt = 0ull;
                Status ev_st = EvictToBudgetLocked(target, e.handle, false, &ev_cnt, nullptr);
                if (!ev_st.ok()) return ev_st;
                if (ev_cnt > 0ull) {
                    ++oom_prevented_count;
                }
            }
        }

#if defined(_WIN32)
        if (device) {
            if (e.gpu_resource != nullptr) {
                if (e.gpu_evicted_flag) {
                    ID3D12Pageable* pageable = e.gpu_resource;
                    const HRESULT mr_hr = device->MakeResident(1u, &pageable);
                    if (FAILED(mr_hr)) {
                        // Retry after hard eviction & resource release of other LRU entries.
                        Status ev_st = EvictToBudgetLocked(0ull, e.handle, true, nullptr, nullptr);
                        if (!ev_st.ok()) return ev_st;
                        if (FAILED(device->MakeResident(1u, &pageable))) {
                            return Status::Error(StatusCode::OutOfMemory,
                                                 "ID3D12Device::MakeResident failed even after LRU eviction");
                        }
                        ++oom_prevented_count;
                    }
                    ++d3d12_make_resident_calls;
                    e.gpu_evicted_flag = false;
                }
            } else {
                const D3D12_RESOURCE_FLAGS flags =
                    e.allow_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                : D3D12_RESOURCE_FLAG_NONE;
                HRESULT hr = CreateD3D12BufferLocal(
                    device,
                    D3D12_HEAP_TYPE_DEFAULT,
                    e.aligned_size_bytes,
                    flags,
                    D3D12_RESOURCE_STATE_COMMON,
                    &e.gpu_resource);
                if (FAILED(hr) || !e.gpu_resource) {
                    // OOM fallback: force-evict and release GPU resources of LRU victims, then retry.
                    Status ev_st = EvictToBudgetLocked(0ull, e.handle, true, nullptr, nullptr);
                    if (!ev_st.ok()) return ev_st;
                    hr = CreateD3D12BufferLocal(
                        device,
                        D3D12_HEAP_TYPE_DEFAULT,
                        e.aligned_size_bytes,
                        flags,
                        D3D12_RESOURCE_STATE_COMMON,
                        &e.gpu_resource);
                    if (FAILED(hr) || !e.gpu_resource) {
                        return Status::Error(StatusCode::OutOfMemory,
                                             "CreateCommittedResource failed after LRU eviction");
                    }
                    ++oom_prevented_count;
                }
                e.gpu_state = D3D12_RESOURCE_STATE_COMMON;
            }

            if (!e.system_ram_backing.empty()) {
                Status up_st = UploadToGpuLocked(
                    e, 0ull, e.system_ram_backing.data(), e.system_ram_backing.size());
                if (!up_st.ok()) return up_st;
            }
        }
#endif

        e.is_resident = true;
        ++e.page_in_count;
        MoveToMruLocked(e);

        if (evicted_bytes >= e.aligned_size_bytes) {
            evicted_bytes -= e.aligned_size_bytes;
        } else {
            evicted_bytes = 0ull;
        }
        resident_bytes += e.aligned_size_bytes;
        if (resident_bytes > peak_resident_bytes) {
            peak_resident_bytes = resident_bytes;
        }
        if (evicted_allocations > 0ull) --evicted_allocations;
        ++resident_allocations;
        ++total_page_ins;
        total_bytes_paged_in += e.size_bytes;
        return Status::Ok();
    }
};

VramResidencyManager::VramResidencyManager() : impl_(std::make_unique<Impl>()) {}

VramResidencyManager::~VramResidencyManager() {
    Shutdown();
}

#if defined(_WIN32)
Status VramResidencyManager::Initialize(ID3D12Device* device, const VramResidencyConfig& config) {
    if (config.vram_budget_bytes == 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "VramResidencyManager requires vram_budget_bytes > 0");
    }

    Shutdown();
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->config = config;
    impl_->is_warp = false;
    impl_->adapter_name = "CPU-Synthetic";

    if (device) {
        device->AddRef();
        impl_->device = device;

        D3D12AdapterInfo info{};
        if (QueryD3D12DeviceAdapterInfo(device, nullptr, info).ok()) {
            impl_->is_warp = info.is_warp;
            impl_->adapter_name = info.description;
        } else {
            impl_->adapter_name = "External D3D12 Device";
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, OS_UUIDOF(ID3D12CommandQueue),
                                              reinterpret_cast<void**>(&impl_->queue))) ||
            !impl_->queue) {
            impl_->ReleaseD3D12RuntimeLocked();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager CreateCommandQueue failed");
        }
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                OS_UUIDOF(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&impl_->allocator))) ||
            !impl_->allocator) {
            impl_->ReleaseD3D12RuntimeLocked();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager CreateCommandAllocator failed");
        }
        if (FAILED(device->CreateCommandList(
                0u,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                impl_->allocator,
                nullptr,
                OS_UUIDOF(ID3D12GraphicsCommandList),
                reinterpret_cast<void**>(&impl_->cmd_list))) ||
            !impl_->cmd_list) {
            impl_->ReleaseD3D12RuntimeLocked();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager CreateCommandList failed");
        }
        impl_->cmd_list->Close();

        if (FAILED(device->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_UUIDOF(ID3D12Fence),
                                       reinterpret_cast<void**>(&impl_->fence))) ||
            !impl_->fence) {
            impl_->ReleaseD3D12RuntimeLocked();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager CreateFence failed");
        }
        impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!impl_->fence_event) {
            impl_->ReleaseD3D12RuntimeLocked();
            return Status::Error(StatusCode::DeviceError, "VramResidencyManager CreateEventW failed");
        }
    }

    impl_->initialized = true;
    return Status::Ok();
}
#else
Status VramResidencyManager::Initialize(void* /*device*/, const VramResidencyConfig& config) {
    if (config.vram_budget_bytes == 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "VramResidencyManager requires vram_budget_bytes > 0");
    }
    Shutdown();
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->config = config;
    impl_->adapter_name = "CPU-Synthetic";
    impl_->initialized = true;
    return Status::Ok();
}
#endif

void VramResidencyManager::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->mutex);
#if defined(_WIN32)
    impl_->ReleaseD3D12RuntimeLocked();
#endif
    impl_->entries.clear();
    impl_->lru_list.clear();
    impl_->resident_bytes = 0ull;
    impl_->evicted_bytes = 0ull;
    impl_->total_virtual_bytes = 0ull;
    impl_->pinned_bytes = 0ull;
    impl_->active_referenced_bytes = 0ull;
    impl_->peak_resident_bytes = 0ull;
    impl_->peak_virtual_bytes = 0ull;
    impl_->resident_allocations = 0ull;
    impl_->evicted_allocations = 0ull;
    impl_->total_allocations = 0ull;
    impl_->total_evictions = 0ull;
    impl_->total_page_ins = 0ull;
    impl_->total_bytes_paged_out = 0ull;
    impl_->total_bytes_paged_in = 0ull;
    impl_->d3d12_evict_calls = 0ull;
    impl_->d3d12_make_resident_calls = 0ull;
    impl_->oom_prevented_count = 0ull;
    impl_->skipped_active_live_heaps = 0ull;
    impl_->completed_fence_value = 0ull;
    impl_->initialized = false;
}

Status VramResidencyManager::SetBudgetBytes(uint64_t new_budget_bytes) {
    if (new_budget_bytes == 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "VRAM budget must be > 0");
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "VramResidencyManager not initialized");
    }
    impl_->config.vram_budget_bytes = new_budget_bytes;
    if (impl_->config.auto_evict_on_pressure && impl_->resident_bytes > new_budget_bytes) {
        return impl_->EvictToBudgetLocked(new_budget_bytes, 0ull, false, nullptr, nullptr);
    }
    return Status::Ok();
}

Status VramResidencyManager::Allocate(const VramAllocationDesc& desc, uint64_t& out_handle) {
    out_handle = 0ull;
    if (desc.size_bytes == 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "VramAllocationDesc::size_bytes must be > 0");
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "VramResidencyManager not initialized");
    }

    const uint64_t align = std::max<uint64_t>(desc.alignment, 256ull);
    const uint64_t aligned_size = AlignUpBytes(desc.size_bytes, align);
    if (aligned_size < desc.size_bytes) {
        return Status::Error(StatusCode::InvalidArgument, "VRAM allocation size alignment overflow");
    }

    // Evict LRU victims proactively if this new resident allocation would exceed budget.
    if (desc.start_resident && impl_->config.auto_evict_on_pressure && impl_->config.vram_budget_bytes > 0ull) {
        const uint64_t target = (impl_->config.vram_budget_bytes > aligned_size)
                                    ? (impl_->config.vram_budget_bytes - aligned_size)
                                    : 0ull;
        if (impl_->resident_bytes > target) {
            uint64_t ev_cnt = 0ull;
            Status ev_st = impl_->EvictToBudgetLocked(target, 0ull, false, &ev_cnt, nullptr);
            if (!ev_st.ok()) return ev_st;
            if (ev_cnt > 0ull) {
                ++impl_->oom_prevented_count;
            }
        }
    }

    Impl::Entry entry{};
    entry.handle = impl_->next_handle++;
    entry.size_bytes = desc.size_bytes;
    entry.aligned_size_bytes = aligned_size;
    entry.debug_name = desc.debug_name;
    entry.allow_uav = desc.allow_uav;
    entry.is_resident = desc.start_resident;
    entry.pin_count = desc.start_pinned ? 1u : 0u;
    entry.system_ram_backing.assign(static_cast<size_t>(desc.size_bytes), 0u);

#if defined(_WIN32)
    if (impl_->device && desc.start_resident) {
        const D3D12_RESOURCE_FLAGS flags =
            desc.allow_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                           : D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = CreateD3D12BufferLocal(
            impl_->device,
            D3D12_HEAP_TYPE_DEFAULT,
            aligned_size,
            flags,
            D3D12_RESOURCE_STATE_COMMON,
            &entry.gpu_resource);
        if (FAILED(hr) || !entry.gpu_resource) {
            // Emergency OOM prevention: force-release LRU GPU resources and retry.
            Status ev_st = impl_->EvictToBudgetLocked(0ull, 0ull, true, nullptr, nullptr);
            if (!ev_st.ok()) return ev_st;
            hr = CreateD3D12BufferLocal(
                impl_->device,
                D3D12_HEAP_TYPE_DEFAULT,
                aligned_size,
                flags,
                D3D12_RESOURCE_STATE_COMMON,
                &entry.gpu_resource);
            if (FAILED(hr) || !entry.gpu_resource) {
                return Status::Error(StatusCode::OutOfMemory,
                                     "VramResidencyManager CreateCommittedResource OOM");
            }
            ++impl_->oom_prevented_count;
        }
        entry.gpu_state = D3D12_RESOURCE_STATE_COMMON;
    }
#endif

    const uint64_t handle = entry.handle;
    auto [it, inserted] = impl_->entries.emplace(handle, std::move(entry));
    (void)inserted;
    Impl::Entry& stored = it->second;

    if (stored.is_resident) {
        impl_->MoveToMruLocked(stored);
        impl_->resident_bytes += stored.aligned_size_bytes;
        if (impl_->resident_bytes > impl_->peak_resident_bytes) {
            impl_->peak_resident_bytes = impl_->resident_bytes;
        }
        ++impl_->resident_allocations;
    } else {
        impl_->evicted_bytes += stored.aligned_size_bytes;
        ++impl_->evicted_allocations;
    }

    if (stored.pin_count > 0u) {
        impl_->pinned_bytes += stored.aligned_size_bytes;
    }
    impl_->total_virtual_bytes += stored.aligned_size_bytes;
    if (impl_->total_virtual_bytes > impl_->peak_virtual_bytes) {
        impl_->peak_virtual_bytes = impl_->total_virtual_bytes;
    }
    ++impl_->total_allocations;

    out_handle = handle;
    return Status::Ok();
}

Status VramResidencyManager::Free(uint64_t handle) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }

    Impl::Entry& e = it->second;
    impl_->RemoveFromLruLocked(e);

#if defined(_WIN32)
    if (e.gpu_resource) {
        e.gpu_resource->Release();
        e.gpu_resource = nullptr;
    }
#endif

    if (e.is_resident) {
        if (impl_->resident_bytes >= e.aligned_size_bytes) impl_->resident_bytes -= e.aligned_size_bytes;
        else impl_->resident_bytes = 0ull;
        if (impl_->resident_allocations > 0ull) --impl_->resident_allocations;
    } else {
        if (impl_->evicted_bytes >= e.aligned_size_bytes) impl_->evicted_bytes -= e.aligned_size_bytes;
        else impl_->evicted_bytes = 0ull;
        if (impl_->evicted_allocations > 0ull) --impl_->evicted_allocations;
    }
    if (e.pin_count > 0u) {
        if (impl_->pinned_bytes >= e.aligned_size_bytes) impl_->pinned_bytes -= e.aligned_size_bytes;
        else impl_->pinned_bytes = 0ull;
    }
    if (e.active_ref_count > 0u) {
        if (impl_->active_referenced_bytes >= e.aligned_size_bytes) {
            impl_->active_referenced_bytes -= e.aligned_size_bytes;
        } else {
            impl_->active_referenced_bytes = 0ull;
        }
    }
    if (impl_->total_virtual_bytes >= e.aligned_size_bytes) {
        impl_->total_virtual_bytes -= e.aligned_size_bytes;
    } else {
        impl_->total_virtual_bytes = 0ull;
    }

    impl_->entries.erase(it);
    return Status::Ok();
}

Status VramResidencyManager::Touch(uint64_t handle) {
    return EnsureResident(handle);
}

Status VramResidencyManager::EnsureResident(uint64_t handle) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    return impl_->MakeEntryResidentLocked(it->second);
}

Status VramResidencyManager::Pin(uint64_t handle) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    Impl::Entry& e = it->second;
    Status st = impl_->MakeEntryResidentLocked(e);
    if (!st.ok()) return st;
    if (e.pin_count == 0u) {
        impl_->pinned_bytes += e.aligned_size_bytes;
    }
    ++e.pin_count;
    return Status::Ok();
}

Status VramResidencyManager::Unpin(uint64_t handle) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    Impl::Entry& e = it->second;
    if (e.pin_count == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "VRAM allocation is not pinned");
    }
    --e.pin_count;
    if (e.pin_count == 0u) {
        if (impl_->pinned_bytes >= e.aligned_size_bytes) impl_->pinned_bytes -= e.aligned_size_bytes;
        else impl_->pinned_bytes = 0ull;
    }
    return Status::Ok();
}

Status VramResidencyManager::AddActiveRef(uint64_t handle) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    Impl::Entry& e = it->second;
    Status st = impl_->MakeEntryResidentLocked(e);
    if (!st.ok()) return st;
    if (e.active_ref_count == 0u) {
        impl_->active_referenced_bytes += e.aligned_size_bytes;
    }
    ++e.active_ref_count;
    return Status::Ok();
}

Status VramResidencyManager::ReleaseActiveRef(uint64_t handle) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    Impl::Entry& e = it->second;
    if (e.active_ref_count == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "VRAM allocation has no active reference");
    }
    --e.active_ref_count;
    if (e.active_ref_count == 0u) {
        if (impl_->active_referenced_bytes >= e.aligned_size_bytes) {
            impl_->active_referenced_bytes -= e.aligned_size_bytes;
        } else {
            impl_->active_referenced_bytes = 0ull;
        }
    }
    return Status::Ok();
}

Status VramResidencyManager::MarkInFlight(uint64_t handle, uint64_t fence_value) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    Impl::Entry& e = it->second;
    if (fence_value > e.in_flight_fence_value) {
        e.in_flight_fence_value = fence_value;
    }
    return Status::Ok();
}

void VramResidencyManager::NotifyFenceCompleted(uint64_t completed_fence_value) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (completed_fence_value > impl_->completed_fence_value) {
        impl_->completed_fence_value = completed_fence_value;
    }
}

bool VramResidencyManager::IsAllocationLiveProtected(uint64_t handle) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) return false;
    return impl_->IsEntryLiveProtectedLocked(it->second);
}

Status VramResidencyManager::EvictToBudget(
    uint64_t target_budget_bytes,
    uint64_t* out_evicted_count,
    uint64_t* out_evicted_bytes) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "VramResidencyManager not initialized");
    }
    return impl_->EvictToBudgetLocked(
        target_budget_bytes, 0ull, false, out_evicted_count, out_evicted_bytes);
}

Status VramResidencyManager::WriteData(
    uint64_t handle, uint64_t offset, const void* data, uint64_t size_bytes) {
    if (!data && size_bytes > 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "WriteData null source pointer");
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    Impl::Entry& e = it->second;
    if (offset > e.size_bytes || size_bytes > e.size_bytes - offset) {
        return Status::Error(StatusCode::InvalidArgument, "WriteData range out of bounds");
    }
    if (size_bytes == 0ull) return Status::Ok();

    // Ensure resident so GPU VRAM reflects the new bytes immediately.
    Status st = impl_->MakeEntryResidentLocked(e);
    if (!st.ok()) return st;

    if (e.system_ram_backing.size() < e.size_bytes) {
        e.system_ram_backing.resize(static_cast<size_t>(e.size_bytes), 0u);
    }
    std::memcpy(e.system_ram_backing.data() + offset, data, static_cast<size_t>(size_bytes));

#if defined(_WIN32)
    if (impl_->device && e.gpu_resource) {
        st = impl_->UploadToGpuLocked(e, offset, data, size_bytes);
        if (!st.ok()) return st;
    }
#endif
    return Status::Ok();
}

Status VramResidencyManager::ReadData(
    uint64_t handle, uint64_t offset, void* out_data, uint64_t size_bytes, bool ensure_resident) {
    if (!out_data && size_bytes > 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "ReadData null destination pointer");
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) {
        return Status::Error(StatusCode::NotFound, "VRAM allocation handle not found");
    }
    Impl::Entry& e = it->second;
    if (offset > e.size_bytes || size_bytes > e.size_bytes - offset) {
        return Status::Error(StatusCode::InvalidArgument, "ReadData range out of bounds");
    }
    if (size_bytes == 0ull) return Status::Ok();

    if (ensure_resident && !e.is_resident) {
        Status st = impl_->MakeEntryResidentLocked(e);
        if (!st.ok()) return st;
    }

#if defined(_WIN32)
    if (e.is_resident && impl_->device && e.gpu_resource) {
        impl_->MoveToMruLocked(e);
        return impl_->ReadbackFromGpuLocked(e, offset, out_data, size_bytes);
    }
#endif

    if (e.system_ram_backing.size() < offset + size_bytes) {
        return Status::Error(StatusCode::DeviceError, "System RAM backing store incomplete");
    }
    std::memcpy(out_data, e.system_ram_backing.data() + offset, static_cast<size_t>(size_bytes));
    return Status::Ok();
}

#if defined(_WIN32)
ID3D12Resource* VramResidencyManager::GetResidentD3D12Resource(uint64_t handle) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end() || !it->second.is_resident) return nullptr;
    return it->second.gpu_resource;
}
#endif

bool VramResidencyManager::GetAllocationInfo(uint64_t handle, VramAllocationInfo& out_info) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->entries.find(handle);
    if (it == impl_->entries.end()) return false;
    const Impl::Entry& e = it->second;
    out_info.handle = e.handle;
    out_info.size_bytes = e.size_bytes;
    out_info.aligned_size_bytes = e.aligned_size_bytes;
    out_info.debug_name = e.debug_name;
    out_info.is_resident = e.is_resident;
    out_info.pin_count = e.pin_count;
    out_info.active_ref_count = e.active_ref_count;
    out_info.in_flight_fence_value = e.in_flight_fence_value;
    out_info.is_in_flight = (e.in_flight_fence_value > impl_->completed_fence_value);
#if defined(_WIN32)
    out_info.gpu_va = (e.is_resident && e.gpu_resource) ? e.gpu_resource->GetGPUVirtualAddress() : 0ull;
#else
    out_info.gpu_va = 0ull;
#endif
    out_info.eviction_count = e.eviction_count;
    out_info.page_in_count = e.page_in_count;
    out_info.last_touch_tick = e.last_touch_tick;
    return true;
}

VramResidencyStats VramResidencyManager::GetStats() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    VramResidencyStats s{};
    s.budget_bytes = impl_->config.vram_budget_bytes;
    s.resident_bytes = impl_->resident_bytes;
    s.evicted_bytes = impl_->evicted_bytes;
    s.total_virtual_bytes = impl_->total_virtual_bytes;
    s.pinned_bytes = impl_->pinned_bytes;
    s.active_referenced_bytes = impl_->active_referenced_bytes;
    s.peak_resident_bytes = impl_->peak_resident_bytes;
    s.peak_virtual_bytes = impl_->peak_virtual_bytes;
    s.active_allocations = static_cast<uint64_t>(impl_->entries.size());
    s.resident_allocations = impl_->resident_allocations;
    s.evicted_allocations = impl_->evicted_allocations;
    s.total_allocations = impl_->total_allocations;
    s.total_evictions = impl_->total_evictions;
    s.total_page_ins = impl_->total_page_ins;
    s.total_bytes_paged_out = impl_->total_bytes_paged_out;
    s.total_bytes_paged_in = impl_->total_bytes_paged_in;
    s.d3d12_evict_calls = impl_->d3d12_evict_calls;
    s.d3d12_make_resident_calls = impl_->d3d12_make_resident_calls;
    s.oom_prevented_count = impl_->oom_prevented_count;
    s.skipped_active_live_heaps = impl_->skipped_active_live_heaps;
#if defined(_WIN32)
    s.using_d3d12_device = (impl_->device != nullptr);
#else
    s.using_d3d12_device = false;
#endif
    s.is_warp = impl_->is_warp;
    s.adapter_name = impl_->adapter_name;
    return s;
}

// ---------------------------------------------------------------------------
// Sprint 14: Live DeviceX VRAM Residency, DXGI Adapter3 Budget Query,
// Series S / Series X Auto-Profile Selection, & Sparse VRAM Window-0 Isolation
// ---------------------------------------------------------------------------
namespace {

constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kSeriesSProfileThresholdBytes = 12ull * kGiB;
constexpr uint64_t kSeriesSBudgetBytes = 8ull * kGiB;                          // Lockhart 8 GiB
constexpr uint64_t kSeriesXBudgetBytes = 13ull * kGiB + (512ull * 1024ull * 1024ull);  // Anaconda 13.5 GiB

bool EqualsIgnoreCaseAscii(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

struct XboxConsoleProfileSelection {
    uint32_t profile_id = 0u;  // 0 = Series S (Lockhart 8 GiB), 1 = Series X (Anaconda 13.5 GiB)
    uint64_t target_console_vram_bytes = kSeriesSBudgetBytes;
    uint64_t host_dedicated_vram_bytes = 0ull;
    bool overridden_by_env = false;
};

XboxConsoleProfileSelection SelectXboxConsoleMemoryProfileFromHostVram(
    uint64_t host_dedicated_vram_bytes,
    const char* explicit_override_or_null) noexcept {
    XboxConsoleProfileSelection sel{};
    sel.host_dedicated_vram_bytes = host_dedicated_vram_bytes;

    const char* ov = explicit_override_or_null;
#if defined(_WIN32)
    char env_buf[64]{};
    if (!ov || ov[0] == '\0') {
        const DWORD len = GetEnvironmentVariableA(
            "OPENSCARLETT_CONSOLE_PROFILE", env_buf, static_cast<DWORD>(sizeof(env_buf)));
        if (len > 0u && len < sizeof(env_buf)) {
            ov = env_buf;
        }
    }
#endif

    if (ov && ov[0] != '\0') {
        if (EqualsIgnoreCaseAscii(ov, "series_s") ||
            EqualsIgnoreCaseAscii(ov, "lockhart") ||
            EqualsIgnoreCaseAscii(ov, "8gb") ||
            EqualsIgnoreCaseAscii(ov, "s")) {
            sel.profile_id = 0u;
            sel.target_console_vram_bytes = kSeriesSBudgetBytes;
            sel.overridden_by_env = true;
            return sel;
        }
        if (EqualsIgnoreCaseAscii(ov, "series_x") ||
            EqualsIgnoreCaseAscii(ov, "anaconda") ||
            EqualsIgnoreCaseAscii(ov, "13.5gb") ||
            EqualsIgnoreCaseAscii(ov, "x")) {
            sel.profile_id = 1u;
            sel.target_console_vram_bytes = kSeriesXBudgetBytes;
            sel.overridden_by_env = true;
            return sel;
        }
    }

    // Auto-select Series S (Lockhart 8 GiB) on PCs with < 12 GiB dedicated VRAM;
    // select Series X (Anaconda 13.5 GiB) on PCs with >= 12 GiB dedicated VRAM.
    if (host_dedicated_vram_bytes < kSeriesSProfileThresholdBytes) {
        sel.profile_id = 0u;
        sel.target_console_vram_bytes = kSeriesSBudgetBytes;
    } else {
        sel.profile_id = 1u;
        sel.target_console_vram_bytes = kSeriesXBudgetBytes;
    }
    return sel;
}

#if defined(_WIN32)
struct HostAdapterVideoMemoryInfo {
    bool queried_dxgi_adapter3 = false;
    uint64_t local_budget_bytes = 0ull;
    uint64_t local_current_usage_bytes = 0ull;
    uint64_t local_available_reservation_bytes = 0ull;
    uint64_t non_local_budget_bytes = 0ull;
    uint64_t non_local_current_usage_bytes = 0ull;
};

bool QueryHostAdapterVideoMemoryInfo(
    ID3D12Device* host_device,
    HostAdapterVideoMemoryInfo& out_info) noexcept {
    out_info = HostAdapterVideoMemoryInfo{};
    if (!host_device) return false;

    const LUID luid = host_device->GetAdapterLuid();
    IDXGIFactory4* factory4 = nullptr;
    if (FAILED(CreateDXGIFactory1(OS_UUIDOF(IDXGIFactory4), reinterpret_cast<void**>(&factory4))) ||
        !factory4) {
        return false;
    }

    IDXGIAdapter3* adapter3 = nullptr;
    HRESULT hr = factory4->EnumAdapterByLuid(
        luid, OS_UUIDOF(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3));
    factory4->Release();
    if (FAILED(hr) || !adapter3) {
        return false;
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO local_mem{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal_mem{};
    const HRESULT local_hr = adapter3->QueryVideoMemoryInfo(
        0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_mem);
    const HRESULT nonlocal_hr = adapter3->QueryVideoMemoryInfo(
        0u, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal_mem);
    adapter3->Release();

    if (SUCCEEDED(local_hr)) {
        out_info.queried_dxgi_adapter3 = true;
        out_info.local_budget_bytes = local_mem.Budget;
        out_info.local_current_usage_bytes = local_mem.CurrentUsage;
        out_info.local_available_reservation_bytes = local_mem.AvailableForReservation;
    }
    if (SUCCEEDED(nonlocal_hr)) {
        out_info.queried_dxgi_adapter3 = true;
        out_info.non_local_budget_bytes = nonlocal_mem.Budget;
        out_info.non_local_current_usage_bytes = nonlocal_mem.CurrentUsage;
    }
    return out_info.queried_dxgi_adapter3;
}
#endif

// Sparse VRAM Window Isolator (xcl #40 / acbbc55):
// Strictly separates Descriptor Heap Window 0 [0x1_0000_0000, 0x2_0000_0000)
// from Title Buffer / Resource Window 1+ [0x2_0000_0000, 0x80_0000_0000).
class SparseVramWindowIsolator {
public:
    static constexpr uint64_t kDescriptorWindow0Base = 0x0000000100000000ull;
    static constexpr uint64_t kDescriptorWindow0Limit = 0x0000000200000000ull;
    static constexpr uint64_t kTitleBufferWindow1Base = 0x0000000200000000ull;
    static constexpr uint64_t kTitleBufferWindow1Limit = 0x0000008000000000ull;

    uint64_t AllocateDescriptorWindow0(uint64_t size_bytes, uint64_t alignment = 65536ull) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        const uint64_t eff_align = std::max<uint64_t>(alignment, 65536ull);
        const uint64_t eff_size = AlignUpBytes(std::max<uint64_t>(size_bytes, 65536ull), eff_align);
        const uint64_t addr = AlignUpBytes(window0_cursor_, eff_align);
        if (addr < kDescriptorWindow0Base || addr > kDescriptorWindow0Limit ||
            eff_size > (kDescriptorWindow0Limit - addr)) {
            return 0ull;
        }
        window0_cursor_ = addr + eff_size;
        allocations_[addr] = Allocation{eff_size, true};
        ++window0_alloc_count_;
        window0_bytes_used_ += eff_size;
        return addr;
    }

    uint64_t AllocateTitleBufferWindow(uint64_t size_bytes, uint64_t alignment = 65536ull) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        const uint64_t eff_align = std::max<uint64_t>(alignment, 65536ull);
        const uint64_t eff_size = AlignUpBytes(std::max<uint64_t>(size_bytes, 65536ull), eff_align);
        const uint64_t addr = AlignUpBytes(window1_cursor_, eff_align);
        if (addr < kTitleBufferWindow1Base || addr > kTitleBufferWindow1Limit ||
            eff_size > (kTitleBufferWindow1Limit - addr)) {
            return 0ull;
        }
        window1_cursor_ = addr + eff_size;
        allocations_[addr] = Allocation{eff_size, false};
        ++window1_alloc_count_;
        window1_bytes_used_ += eff_size;
        return addr;
    }

    void Free(uint64_t va) noexcept {
        if (va == 0ull) return;
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = allocations_.find(va);
        if (it == allocations_.end()) return;
        if (it->second.is_window0) {
            if (window0_bytes_used_ >= it->second.size) window0_bytes_used_ -= it->second.size;
            else window0_bytes_used_ = 0ull;
        } else {
            if (window1_bytes_used_ >= it->second.size) window1_bytes_used_ -= it->second.size;
            else window1_bytes_used_ = 0ull;
        }
        allocations_.erase(it);
    }

    static bool IsInDescriptorWindow0(uint64_t va) noexcept {
        return va >= kDescriptorWindow0Base && va < kDescriptorWindow0Limit;
    }
    static bool IsInTitleBufferWindow(uint64_t va) noexcept {
        return va >= kTitleBufferWindow1Base && va < kTitleBufferWindow1Limit;
    }

    void GetStats(
        uint64_t& out_w0_allocs,
        uint64_t& out_w0_bytes,
        uint64_t& out_w1_allocs,
        uint64_t& out_w1_bytes) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        out_w0_allocs = window0_alloc_count_;
        out_w0_bytes = window0_bytes_used_;
        out_w1_allocs = window1_alloc_count_;
        out_w1_bytes = window1_bytes_used_;
    }

private:
    struct Allocation {
        uint64_t size = 0ull;
        bool is_window0 = false;
    };
    mutable std::mutex mutex_;
    uint64_t window0_cursor_ = kDescriptorWindow0Base;
    uint64_t window1_cursor_ = kTitleBufferWindow1Base;
    uint64_t window0_alloc_count_ = 0ull;
    uint64_t window0_bytes_used_ = 0ull;
    uint64_t window1_alloc_count_ = 0ull;
    uint64_t window1_bytes_used_ = 0ull;
    std::unordered_map<uint64_t, Allocation> allocations_;
};

// Exported C++ factory/helpers used by DeviceX (compiled in openscarlett_core).
uint64_t SparseVramAllocateWindow0(SparseVramWindowIsolator* iso, uint64_t size_bytes) noexcept {
    return iso ? iso->AllocateDescriptorWindow0(size_bytes) : 0ull;
}

uint64_t SparseVramAllocateTitleWindow(SparseVramWindowIsolator* iso, uint64_t size_bytes) noexcept {
    return iso ? iso->AllocateTitleBufferWindow(size_bytes) : 0ull;
}

void SparseVramFree(SparseVramWindowIsolator* iso, uint64_t va) noexcept {
    if (iso) iso->Free(va);
}

void SparseVramGetStats(
    const SparseVramWindowIsolator* iso,
    uint64_t& out_w0_allocs,
    uint64_t& out_w0_bytes,
    uint64_t& out_w1_allocs,
    uint64_t& out_w1_bytes) noexcept {
    if (!iso) {
        out_w0_allocs = out_w0_bytes = out_w1_allocs = out_w1_bytes = 0ull;
        return;
    }
    iso->GetStats(out_w0_allocs, out_w0_bytes, out_w1_allocs, out_w1_bytes);
}

SparseVramWindowIsolator* CreateSparseVramWindowIsolator() {
    return new SparseVramWindowIsolator();
}

void DestroySparseVramWindowIsolator(SparseVramWindowIsolator* iso) noexcept {
    delete iso;
}

}  // namespace openscarlett
