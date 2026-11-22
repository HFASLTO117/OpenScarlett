#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/descriptor_sgpr_bridge.h"
#include "openscarlett/gpu/frame_capture.h"
#include "openscarlett/gpu/shader_cache.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/kernel/title_va_mirror.h"

namespace openscarlett::d3d12_xs {

struct VulkanPipelineInfo {
    bool is_spirv_direct = false;
    bool is_compute = false;
    bool is_mesh = false;
    std::vector<uint32_t> vs_spirv;
    std::vector<uint32_t> ps_spirv;
    std::vector<uint32_t> cs_spirv;
    std::vector<uint32_t> as_spirv;
    std::vector<uint32_t> ms_spirv;
    std::string vs_entry = "main";
    std::string ps_entry = "main";
    std::string cs_entry = "main";
    std::string as_entry = "main";
    std::string ms_entry = "main";
    std::vector<openscarlett::VulkanVertexAttribute> vertex_attrs;
    uint32_t vertex_stride_hint = 0u;
    bool enable_depth = false;
    bool depth_write = true;
    openscarlett::VulkanImageFormat color_format = openscarlett::VulkanImageFormat::Rgba8Unorm;
};

struct VulkanTrackedResource {
    ID3D12Resource* resource = nullptr;
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    D3D12_GPU_VIRTUAL_ADDRESS gpu_va = 0ull;
    std::vector<uint8_t> shadow_bytes;
};

struct UnifiedTextureMapping {
    ID3D12Resource* resource = nullptr;
    uint64_t guest_va = 0ull;
    uint64_t byte_size = 0ull;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth_or_array_size = 1u;
    uint32_t mip_levels = 1u;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t row_pitch_bytes = 0u;
    uint64_t page_generation = 1ull;
    uint64_t uploaded_generation = 0ull;
    bool dirty = false;
    bool cpu_readable = false;
};

struct XMemAllocationRecord {
    uint64_t guest_va = 0ull;
    uint64_t byte_size = 0ull;
    uint64_t aligned_size = 0ull;
    ID3D12Heap* heap = nullptr;
    ID3D12Resource* anchor_buffer = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS desktop_gpu_va = 0ull;
};

class DeviceX final
    : public ID3D12DeviceX,
      public IOpenScarlettUnifiedMemory,
      public IOpenScarlettVulkanRouting {
public:
    DeviceX(ID3D12Device* host_device,
            const D3D12AdapterInfo& adapter_info,
            HWND default_hwnd = nullptr);
    ~DeviceX();

    DeviceX(const DeviceX&) = delete;
    DeviceX& operator=(const DeviceX&) = delete;

    ID3D12Device* HostDevice() const noexcept { return host_dev_; }
    const D3D12AdapterInfo& AdapterInfo() const noexcept { return adapter_info_; }
    HWND DefaultWindow() const noexcept { return default_hwnd_; }
    bool SupportsDepthBoundsTest() const noexcept { return supports_depth_bounds_test_; }
    bool SupportsViewInstancing() const noexcept { return supports_view_instancing_; }
    bool SupportsProgrammableSamplePositions() const noexcept {
        return supports_programmable_sample_positions_;
    }

    // Lifecycle cleanup notifications invoked by proxy destructors to prevent stale pointer reuse.
    void NotifyResourceDestroyed(ID3D12Resource* host_resource) noexcept;
    void NotifyHeapDestroyed(ID3D12Heap* host_heap) noexcept;
    void NotifyPipelineDestroyed(ID3D12PipelineState* pso_key) noexcept;

    // Zero-overhead C++ integration hooks for TitleVAMirror & DescriptorSgprBridge.
    using GuestGpuVaTranslatorFn = D3D12_GPU_VIRTUAL_ADDRESS (*)(
        D3D12_GPU_VIRTUAL_ADDRESS guest_va, void* user_ctx) noexcept;
    using DescriptorCreateHookFn = void (*)(
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
        D3D12_CPU_DESCRIPTOR_HANDLE dest,
        ID3D12Resource* host_resource,
        D3D12_GPU_VIRTUAL_ADDRESS gpu_va,
        void* user_ctx) noexcept;
    void SetGuestGpuVaTranslatorHook(GuestGpuVaTranslatorFn fn, void* user_ctx) noexcept;
    void SetDescriptorCreateHook(DescriptorCreateHookFn fn, void* user_ctx) noexcept;
    openscarlett::TitleVAMirror& GetTitleVaMirror() noexcept { return title_va_mirror_; }
    const openscarlett::TitleVAMirror& GetTitleVaMirror() const noexcept { return title_va_mirror_; }
    openscarlett::DescriptorSgprBridge& GetDescriptorBridge() noexcept { return descriptor_bridge_; }
    const openscarlett::DescriptorSgprBridge& GetDescriptorBridge() const noexcept { return descriptor_bridge_; }
    bool IsR9G9B9E5UavFallbackActive() const noexcept;
    static void SetForceR9G9B9E5UavFallback(bool force) noexcept;
    static void SetGlobalBackendMode(uint32_t backend_mode) noexcept;
    static uint32_t ResolveEffectiveBackendMode() noexcept;

    bool IsVulkanRoutingActive() const noexcept { return vulkan_routing_active_; }
    openscarlett::VulkanBackend* GetVulkanBackend() const noexcept { return vulkan_backend_.get(); }
    bool LookupVulkanPipeline(ID3D12PipelineState* pso, VulkanPipelineInfo& out_info) const;
    bool IsSpirvDirectPipeline(ID3D12PipelineState* pso) const;
    ID3D12Resource* ResolveVulkanRtvResource(D3D12_CPU_DESCRIPTOR_HANDLE rtv) const;
    ID3D12Resource* ResolveVulkanDsvResource(D3D12_CPU_DESCRIPTOR_HANDLE dsv) const;
    ID3D12Resource* ResolveVulkanBufferByGpuVa(
        D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t* out_offset_bytes = nullptr) const;
    bool ReadVulkanBufferData(
        ID3D12Resource* res, uint64_t offset, uint64_t size, std::vector<uint8_t>& out_bytes);
    bool WriteVulkanBufferData(
        ID3D12Resource* res, uint64_t offset, const void* data, size_t size);
    void StoreVulkanRenderTargetFramebuffer(
        ID3D12Resource* rt_res, const openscarlett::FrameBufferRgba8& fb);
    bool TryGetVulkanRenderTargetFramebuffer(
        ID3D12Resource* rt_res, openscarlett::FrameBufferRgba8& out_fb) const;
    void RecordVulkanRoutingCounters(
        uint64_t cmd_lists,
        uint64_t draws,
        uint64_t indexed_draws,
        uint64_t dispatches,
        uint64_t mesh_dispatches,
        uint64_t copies,
        uint64_t barriers,
        uint64_t rtv_clears,
        uint64_t dsv_clears,
        uint64_t presents,
        uint64_t icd_execs,
        uint64_t spirv_draws,
        uint64_t spirv_dispatches,
        uint64_t spirv_mesh_dispatches) noexcept;
    D3D12_GPU_VIRTUAL_ADDRESS TranslateGuestGpuVa(D3D12_GPU_VIRTUAL_ADDRESS address) const noexcept;
    uint32_t FrameIntervalMicroseconds() const noexcept {
        return frame_interval_us_.load(std::memory_order_relaxed);
    }
    uint32_t MaxFrameLatency() const noexcept {
        return max_frame_latency_.load(std::memory_order_relaxed);
    }
    bool HasRegisteredUnifiedTextures() const noexcept {
        return registered_unified_count_.load(std::memory_order_acquire) > 0u;
    }
    bool HasDirtyUnifiedTextures() const noexcept {
        return dirty_unified_count_.load(std::memory_order_acquire) > 0u;
    }
    void NotifyResourceStateTransition(
        ID3D12Resource* resource, D3D12_RESOURCE_STATES new_state) noexcept;

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ID3D12Device (slots 7..43)
    UINT STDMETHODCALLTYPE GetNodeCount() override;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(
        const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override;
    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes,
        REFIID riid, void** ppvRootSignature) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(
        const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateSampler(
        const D3D12_SAMPLER_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CopyDescriptors(
        UINT NumDestDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
        const UINT* pDestDescriptorRangeSizes,
        UINT NumSrcDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
        const UINT* pSrcDescriptorRangeSizes,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(
        UINT NumDescriptors,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
        D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device::GetResourceAllocationInfo;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(
        D3D12_RESOURCE_ALLOCATION_INFO* pRet,
        UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs) override;
    using ID3D12Device::GetCustomHeapProperties;
    D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(
        D3D12_HEAP_PROPERTIES* pRet,
        UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(
        UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource,
        void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(
        const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid,
        void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
        DWORD Access, LPCWSTR Name, HANDLE* pHandle) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(
        HANDLE NTHandle, REFIID riid, void** ppvObj) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(
        LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override;
    HRESULT STDMETHODCALLTYPE MakeResident(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE Evict(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetCopyableFootprints(
        const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
        UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
        UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(
        const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override;
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(
        const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature,
        REFIID riid, void** ppvCommandSignature) override;
    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
        D3D12_PACKED_MIP_INFO* pPackedMipDesc,
        D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
        UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
        D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device::GetAdapterLuid;
    LUID* STDMETHODCALLTYPE GetAdapterLuid(LUID* pRet) override;
#else
    LUID STDMETHODCALLTYPE GetAdapterLuid() override;
#endif

    // ID3D12Device1 (slots 44..46)
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(
        const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid,
        void** ppPipelineLibrary) override;
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
        ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) override;
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(
        UINT NumObjects, ID3D12Pageable* const* ppObjects,
        const D3D12_RESIDENCY_PRIORITY* pPriorities) override;

    // ID3D12Device2 (slot 47)
    HRESULT STDMETHODCALLTYPE CreatePipelineState(
        const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid,
        void** ppPipelineState) override;

    // ID3D12Device3 (slots 48..50)
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(
        const void* pAddress, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(
        HANDLE hFileMapping, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(
        D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects,
        ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) override;

    // ID3D12Device4 (slots 51..56)
    HRESULT STDMETHODCALLTYPE CreateCommandList1(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid, void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid,
        void** ppSession) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap1(
        const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvResource) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device4::GetResourceAllocationInfo1;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo1(
        D3D12_RESOURCE_ALLOCATION_INFO* pRet,
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override;
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override;
#endif

    // ID3D12Device5 (slots 57..64)
    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(
        ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) override;
    void STDMETHODCALLTYPE RemoveDevice() override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(
        UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(
        REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage,
        UINT* pTotalStructureSizeInBytes, UINT* pParameterCount,
        D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) override;
    HRESULT STDMETHODCALLTYPE CreateMetaCommand(
        REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
        SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** ppMetaCommand) override;
    HRESULT STDMETHODCALLTYPE CreateStateObject(
        const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject) override;
    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) override;
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) override;

    // ID3D12Device6 (slot 65)
    HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(
        D3D12_BACKGROUND_PROCESSING_MODE Mode,
        D3D12_MEASUREMENTS_ACTION MeasurementsAction,
        HANDLE hEventToSignalUponCompletion,
        BOOL* pbFurtherMeasurementsDesired) override;

    // ID3D12DeviceX (slots 66..72)
    HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* pOutput, uint32_t PeriodMicroseconds,
        uint32_t MaxFrameLatency, uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, uint64_t DelayMicroseconds,
        HANDLE hEvent, uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, uint32_t TimeoutMilliseconds,
        void* pReserved, D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResourceX(
        D3D12_GPU_VIRTUAL_ADDRESS VirtualAddress, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE ResumeX() override;
    HRESULT STDMETHODCALLTYPE GetGpuHardwareInfoX(D3D12XBOX_GPU_HARDWARE_INFO* pOutInfo) override;

    // IOpenScarlettUnifiedMemory
    HRESULT STDMETHODCALLTYPE RegisterUnifiedTexture(
        const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE UnregisterUnifiedTexture(
        ID3D12Resource* resource) override;
    HRESULT STDMETHODCALLTYPE NotifyVirtualRemap(
        uint64_t guest_va_base,
        uint64_t byte_size,
        uint64_t new_backing_va,
        uint32_t* out_flagged_count) override;
    HRESULT STDMETHODCALLTYPE FlushDirtyUnifiedTextures(
        ID3D12CommandQueue* queue,
        uint32_t* out_flushed_count) override;
    HRESULT STDMETHODCALLTYPE GetUnifiedTextureStats(
        D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats) override;
    HRESULT STDMETHODCALLTYPE AttachXMemAllocation(
        uint64_t guest_va,
        uint64_t byte_size,
        uint64_t* out_desktop_gpu_va) override;
    HRESULT STDMETHODCALLTYPE DetachXMemAllocation(
        uint64_t guest_va) override;
    HRESULT STDMETHODCALLTYPE ResolveXMemGpuVa(
        uint64_t guest_va,
        uint64_t* out_desktop_gpu_va) override;
    HRESULT STDMETHODCALLTYPE GetR9G9B9E5UavFallbackActive(
        int* out_active) override;

    // IOpenScarlettVulkanRouting
    uint32_t STDMETHODCALLTYPE GetActiveBackendMode() override;
    HRESULT STDMETHODCALLTYPE GetVulkanRoutingStats(
        D3D12XBOX_VULKAN_ROUTING_STATS* out_stats) override;

private:
    void TrackVulkanResource(
        IUnknown* obj, D3D12_HEAP_TYPE heap_type, const D3D12_RESOURCE_DESC& desc);
    bool ShouldRewriteR9G9B9E5Uav(const D3D12_RESOURCE_DESC& desc) const noexcept;
    void TrackFallbackResource(IUnknown* obj, bool is_fallback);
    bool IsTrackedFallbackResource(ID3D12Resource* resource) const noexcept;
    HRESULT EnsureUnifiedUploadObjectsLocked();
    HRESULT EnsureUnifiedUploadBufferLocked(uint64_t required_bytes);
    void ReleaseUnifiedMemoryObjects();
    void AutoRegisterUnifiedResource(
        ID3D12Resource* resource,
        uint64_t guest_va,
        const D3D12_RESOURCE_DESC& desc,
        D3D12_RESOURCE_STATES initial_state);

    std::atomic<ULONG> ref_count_{1};
    ID3D12Device* host_dev_ = nullptr;
    ID3D12Device1* host_dev1_ = nullptr;
    ID3D12Device2* host_dev2_ = nullptr;
    ID3D12Device3* host_dev3_ = nullptr;
    ID3D12Device4* host_dev4_ = nullptr;
    ID3D12Device5* host_dev5_ = nullptr;
    ID3D12Device6* host_dev6_ = nullptr;
    D3D12AdapterInfo adapter_info_{};
    HWND default_hwnd_ = nullptr;
    bool supports_r9g9b9e5_uav_ = false;
    bool supports_depth_bounds_test_ = false;
    bool supports_view_instancing_ = false;
    bool supports_programmable_sample_positions_ = false;
    std::atomic<uint32_t> frame_interval_us_{16667u};
    std::atomic<uint32_t> max_frame_latency_{2u};
    std::atomic<uint64_t> next_frame_token_{1ull};
    std::atomic<bool> suspended_{false};
    std::mutex shader_lift_mutex_;
    openscarlett::ShaderCache shader_cache_;

    std::atomic<GuestGpuVaTranslatorFn> va_translator_hook_{nullptr};
    std::atomic<void*> va_translator_ctx_{nullptr};
    std::atomic<DescriptorCreateHookFn> descriptor_create_hook_{nullptr};
    std::atomic<void*> descriptor_create_ctx_{nullptr};
    openscarlett::TitleVAMirror title_va_mirror_;
    openscarlett::DescriptorSgprBridge descriptor_bridge_;

    mutable std::mutex fallback_mutex_;
    std::unordered_set<ID3D12Resource*> r9g9b9e5_fallback_resources_;

    // Title unified-memory texture remap tracking (ivall/xcl fd74662/bdae5a4)
    mutable std::mutex unified_mutex_;
    std::unordered_map<ID3D12Resource*, UnifiedTextureMapping> unified_textures_;
    std::unordered_map<ID3D12Heap*, uint64_t> address_heaps_;
    std::vector<XMemAllocationRecord> xmem_allocations_;
    std::atomic<uint32_t> xmem_allocation_count_{0u};
    std::atomic<uint32_t> registered_unified_count_{0u};
    std::atomic<uint32_t> dirty_unified_count_{0u};
    uint64_t total_remap_notifications_{0ull};
    uint64_t total_unified_flushes_{0ull};
    uint64_t total_unified_bytes_uploaded_{0ull};

    std::mutex unified_upload_mutex_;
    ID3D12CommandAllocator* unified_alloc_ = nullptr;
    ID3D12GraphicsCommandList* unified_cmd_ = nullptr;
    ID3D12Fence* unified_fence_ = nullptr;
    HANDLE unified_fence_event_ = nullptr;
    uint64_t unified_fence_val_ = 0ull;
    ID3D12Resource* unified_upload_buf_ = nullptr;
    uint64_t unified_upload_capacity_ = 0ull;

    // Direct d3d12_xs -> Vulkan 1.4 routing state (active when OPENSCARLETT_BACKEND=vulkan)
    bool vulkan_routing_active_ = false;
    std::unique_ptr<openscarlett::VulkanBackend> vulkan_backend_;
    mutable std::mutex vk_state_mutex_;
    std::unordered_map<ID3D12PipelineState*, VulkanPipelineInfo> vk_pipelines_;
    std::unordered_map<SIZE_T, ID3D12Resource*> vk_rtv_map_;
    std::unordered_map<SIZE_T, ID3D12Resource*> vk_dsv_map_;
    std::unordered_map<ID3D12Resource*, VulkanTrackedResource> vk_resources_;
    std::unordered_map<ID3D12Resource*, openscarlett::FrameBufferRgba8> vk_rt_framebuffers_;
    std::atomic<uint64_t> vk_routed_cmd_lists_{0ull};
    std::atomic<uint64_t> vk_routed_draws_{0ull};
    std::atomic<uint64_t> vk_routed_indexed_draws_{0ull};
    std::atomic<uint64_t> vk_routed_dispatches_{0ull};
    std::atomic<uint64_t> vk_routed_mesh_dispatches_{0ull};
    std::atomic<uint64_t> vk_routed_copies_{0ull};
    std::atomic<uint64_t> vk_routed_barriers_{0ull};
    std::atomic<uint64_t> vk_routed_rtv_clears_{0ull};
    std::atomic<uint64_t> vk_routed_dsv_clears_{0ull};
    std::atomic<uint64_t> vk_routed_presents_{0ull};
    std::atomic<uint64_t> vk_icd_executions_{0ull};
    std::atomic<uint64_t> vk_spirv_direct_draws_{0ull};
    std::atomic<uint64_t> vk_spirv_direct_dispatches_{0ull};
    std::atomic<uint64_t> vk_spirv_direct_mesh_dispatches_{0ull};
};

}  // namespace openscarlett::d3d12_xs
