#include "command_list_x.h"

#include <algorithm>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>

#include "device_x.h"
#include "openscarlett/core/telemetry.h"

namespace openscarlett::d3d12_xs {

namespace {

constexpr const char* kModule = "d3d12_xs.dll";

inline void RecordList(const char* method, int32_t slot, HRESULT hr = S_OK) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL || hr == E_NOINTERFACE) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (FAILED(hr)) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        method,
        outcome,
        static_cast<int32_t>(hr),
        slot);
}

struct CommandListRegistry {
    std::shared_mutex mutex;
    std::unordered_map<const void*, ID3D12GraphicsCommandList*> map;
    std::unordered_map<const void*, CommandListX*> wrapper_map;
};

CommandListRegistry& GetCommandListRegistry() {
    static CommandListRegistry reg;
    return reg;
}

}  // namespace

CommandListX::CommandListX(DeviceX* parent_device, ID3D12GraphicsCommandList* host_list)
    : device_x_(parent_device), host_list_(host_list) {
    if (device_x_) device_x_->AddRef();
    if (host_list_) {
        host_list_->AddRef();
        list_type_ = host_list_->GetType();
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList1), reinterpret_cast<void**>(&host_list1_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList2), reinterpret_cast<void**>(&host_list2_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList3), reinterpret_cast<void**>(&host_list3_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList4), reinterpret_cast<void**>(&host_list4_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList5), reinterpret_cast<void**>(&host_list5_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList6), reinterpret_cast<void**>(&host_list6_));
    }
    auto& reg = GetCommandListRegistry();
    std::unique_lock<std::shared_mutex> lock(reg.mutex);
    reg.map[static_cast<const ID3D12CommandList*>(this)] = host_list_;
    reg.wrapper_map[static_cast<const ID3D12CommandList*>(this)] = this;
}

CommandListX::~CommandListX() {
    {
        auto& reg = GetCommandListRegistry();
        std::unique_lock<std::shared_mutex> lock(reg.mutex);
        reg.map.erase(static_cast<const ID3D12CommandList*>(this));
        reg.wrapper_map.erase(static_cast<const ID3D12CommandList*>(this));
    }
    if (host_list6_) host_list6_->Release();
    if (host_list5_) host_list5_->Release();
    if (host_list4_) host_list4_->Release();
    if (host_list3_) host_list3_->Release();
    if (host_list2_) host_list2_->Release();
    if (host_list1_) host_list1_->Release();
    if (host_list_) host_list_->Release();
    if (device_x_) device_x_->Release();
}

CommandListX* CommandListX::TryFromCommandList(ID3D12CommandList* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetCommandListRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.wrapper_map.find(static_cast<const void*>(maybe_wrapped));
    return (it != reg.wrapper_map.end()) ? it->second : nullptr;
}

void CommandListX::CopyVulkanRecordedBatch(VulkanRecordedBatch& out_batch) const {
    std::lock_guard<std::mutex> lock(vk_rec_mutex_);
    out_batch = vk_batch_;
}

ID3D12CommandList* CommandListX::UnwrapHostList(ID3D12CommandList* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetCommandListRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.map.find(static_cast<const void*>(maybe_wrapped));
    if (it != reg.map.end()) {
        return it->second;
    }
    return maybe_wrapped;
}

ID3D12GraphicsCommandList* CommandListX::UnwrapHostGraphicsList(
    ID3D12GraphicsCommandList* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetCommandListRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.map.find(static_cast<const void*>(static_cast<ID3D12CommandList*>(maybe_wrapped)));
    if (it != reg.map.end()) {
        return it->second;
    }
    return maybe_wrapped;
}

// IUnknown (slots 0..2)
HRESULT STDMETHODCALLTYPE CommandListX::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, E_POINTER);
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12CommandList)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList)) ||
        IsEqualIID(riid, IID_ID3D12GraphicsCommandListX)) {
        *ppvObject = static_cast<ID3D12GraphicsCommandListX*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettCommandListExtState)) {
        *ppvObject = static_cast<IOpenScarlettCommandListExtState*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList1)) && host_list1_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList1*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList2)) && host_list2_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList2*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList3)) && host_list3_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList3*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList4)) && host_list4_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList4*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList5)) && host_list5_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList5*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList6)) && host_list6_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList6*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }

    RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, E_NOINTERFACE);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CommandListX::AddRef() {
    const ULONG refs = ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    RecordList("ID3D12GraphicsCommandListX::AddRef", 1, S_OK);
    return refs;
}

ULONG STDMETHODCALLTYPE CommandListX::Release() {
    RecordList("ID3D12GraphicsCommandListX::Release", 2, S_OK);
    const ULONG rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete this;
    }
    return rem;
}

// ID3D12Object (slots 3..6)
HRESULT STDMETHODCALLTYPE CommandListX::GetPrivateData(
    REFGUID guid, UINT* pDataSize, void* pData) {
    const HRESULT hr = host_list_->GetPrivateData(guid, pDataSize, pData);
    RecordList("ID3D12GraphicsCommandListX::GetPrivateData", 3, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::SetPrivateData(
    REFGUID guid, UINT DataSize, const void* pData) {
    const HRESULT hr = host_list_->SetPrivateData(guid, DataSize, pData);
    RecordList("ID3D12GraphicsCommandListX::SetPrivateData", 4, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::SetPrivateDataInterface(
    REFGUID guid, const IUnknown* pData) {
    const HRESULT hr = host_list_->SetPrivateDataInterface(guid, pData);
    RecordList("ID3D12GraphicsCommandListX::SetPrivateDataInterface", 5, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::SetName(LPCWSTR Name) {
    const HRESULT hr = host_list_->SetName(Name);
    RecordList("ID3D12GraphicsCommandListX::SetName", 6, hr);
    return hr;
}

// ID3D12DeviceChild (slot 7)
HRESULT STDMETHODCALLTYPE CommandListX::GetDevice(REFIID riid, void** ppvDevice) {
    const HRESULT hr = device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
    RecordList("ID3D12GraphicsCommandListX::GetDevice", 7, hr);
    return hr;
}

// ID3D12CommandList (slot 8)
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE CommandListX::GetType() {
    const D3D12_COMMAND_LIST_TYPE t = host_list_->GetType();
    RecordList("ID3D12GraphicsCommandListX::GetType", 8, S_OK);
    return t;
}

// ID3D12GraphicsCommandList (slots 9..59)
HRESULT STDMETHODCALLTYPE CommandListX::Close() {
    const HRESULT hr = host_list_->Close();
    RecordList("ID3D12GraphicsCommandListX::Close", 9, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::Reset(
    ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) {
    depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bias_clamp_.store(0.0f, std::memory_order_relaxed);
    slope_scaled_depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_min_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_max_.store(1.0f, std::memory_order_relaxed);
    view_instance_mask_.store(0u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        sample_positions_per_pixel_ = 0u;
        sample_positions_pixels_ = 0u;
        sample_positions_.clear();
    }
    ID3D12PipelineState* host_pso = UnwrapPipelineState(pInitialState);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        const bool is_spirv = device_x_->IsSpirvDirectPipeline(pInitialState);
        if (is_spirv) host_pso = nullptr;
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_batch_ = VulkanRecordedBatch{};
        vk_cur_pso_ = pInitialState;
        vk_cur_pso_is_spirv_ = is_spirv;
        vk_cur_rtv_ = nullptr;
        vk_cur_dsv_ = nullptr;
        vk_has_clear_color_ = false;
        vk_clear_color_[0] = vk_clear_color_[1] = vk_clear_color_[2] = 0.0f;
        vk_clear_color_[3] = 1.0f;
        vk_clear_depth_ = 1.0f;
        vk_vp_width_ = 0u;
        vk_vp_height_ = 0u;
        vk_has_vbv_ = false;
        vk_has_ibv_ = false;
        vk_gfx_root_constants_.clear();
        vk_cmp_root_constants_.clear();
        vk_gfx_cbvs_.clear();
        vk_gfx_srvs_.clear();
        vk_gfx_uavs_.clear();
        vk_cmp_cbvs_.clear();
        vk_cmp_srvs_.clear();
        vk_cmp_uavs_.clear();
    }
    const HRESULT hr = host_list_->Reset(UnwrapCommandAllocator(pAllocator), host_pso);
    RecordList("ID3D12GraphicsCommandListX::Reset", 10, hr);
    return hr;
}

void STDMETHODCALLTYPE CommandListX::ClearState(ID3D12PipelineState* pPipelineState) {
    depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bias_clamp_.store(0.0f, std::memory_order_relaxed);
    slope_scaled_depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_min_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_max_.store(1.0f, std::memory_order_relaxed);
    view_instance_mask_.store(0u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        sample_positions_per_pixel_ = 0u;
        sample_positions_pixels_ = 0u;
        sample_positions_.clear();
    }
    ID3D12PipelineState* host_pso = UnwrapPipelineState(pPipelineState);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        const bool is_spirv = device_x_->IsSpirvDirectPipeline(pPipelineState);
        if (is_spirv) host_pso = nullptr;
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_pso_ = pPipelineState;
        vk_cur_pso_is_spirv_ = is_spirv;
        vk_cur_rtv_ = nullptr;
        vk_cur_dsv_ = nullptr;
        vk_has_vbv_ = false;
        vk_has_ibv_ = false;
        vk_gfx_root_constants_.clear();
        vk_cmp_root_constants_.clear();
        vk_gfx_cbvs_.clear();
        vk_gfx_srvs_.clear();
        vk_gfx_uavs_.clear();
        vk_cmp_cbvs_.clear();
        vk_cmp_srvs_.clear();
        vk_cmp_uavs_.clear();
    }
    host_list_->ClearState(host_pso);
    RecordList("ID3D12GraphicsCommandListX::ClearState", 11, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::DrawInstanced;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = VertexCountPerInstance;
        op.y = InstanceCount;
        op.start_location = StartVertexLocation;
        op.rtv_resource = vk_cur_rtv_;
        op.dsv_resource = vk_cur_dsv_;
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        op.clear_depth = vk_clear_depth_;
        op.viewport_width = vk_vp_width_;
        op.viewport_height = vk_vp_height_;
        op.vbv = vk_cur_vbv_;
        op.has_vbv = vk_has_vbv_;
        op.root_constants = vk_gfx_root_constants_;
        op.root_cbvs = vk_gfx_cbvs_;
        op.root_srvs = vk_gfx_srvs_;
        op.root_uavs = vk_gfx_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    if (!skip_host) {
        host_list_->DrawInstanced(
            VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
    RecordList("ID3D12GraphicsCommandListX::DrawInstanced", 12, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DrawIndexedInstanced(
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::DrawIndexedInstanced;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = IndexCountPerInstance;
        op.y = InstanceCount;
        op.start_location = StartIndexLocation;
        op.base_vertex = BaseVertexLocation;
        op.rtv_resource = vk_cur_rtv_;
        op.dsv_resource = vk_cur_dsv_;
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        op.clear_depth = vk_clear_depth_;
        op.viewport_width = vk_vp_width_;
        op.viewport_height = vk_vp_height_;
        op.vbv = vk_cur_vbv_;
        op.has_vbv = vk_has_vbv_;
        op.ibv = vk_cur_ibv_;
        op.has_ibv = vk_has_ibv_;
        op.root_constants = vk_gfx_root_constants_;
        op.root_cbvs = vk_gfx_cbvs_;
        op.root_srvs = vk_gfx_srvs_;
        op.root_uavs = vk_gfx_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    if (!skip_host) {
        host_list_->DrawIndexedInstanced(
            IndexCountPerInstance, InstanceCount, StartIndexLocation,
            BaseVertexLocation, StartInstanceLocation);
    }
    RecordList("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 13, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Dispatch(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::Dispatch;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = ThreadGroupCountX;
        op.y = ThreadGroupCountY;
        op.z = ThreadGroupCountZ;
        op.root_constants = vk_cmp_root_constants_;
        op.root_cbvs = vk_cmp_cbvs_;
        op.root_srvs = vk_cmp_srvs_;
        op.root_uavs = vk_cmp_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    if (!skip_host) {
        host_list_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    RecordList("ID3D12GraphicsCommandListX::Dispatch", 14, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyBufferRegion(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) {
    ID3D12Resource* host_dst = UnwrapResource(pDstBuffer);
    ID3D12Resource* host_src = UnwrapResource(pSrcBuffer);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::CopyBuffer;
        op.dst_resource = host_dst;
        op.src_resource = host_src;
        op.dst_offset = DstOffset;
        op.src_offset = SrcOffset;
        op.num_bytes = NumBytes;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->CopyBufferRegion(host_dst, DstOffset, host_src, SrcOffset, NumBytes);
    RecordList("ID3D12GraphicsCommandListX::CopyBufferRegion", 15, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyTextureRegion(
    const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
    const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) {
    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    const D3D12_TEXTURE_COPY_LOCATION* dst_ptr = pDst;
    const D3D12_TEXTURE_COPY_LOCATION* src_ptr = pSrc;
    if (pDst) {
        dst_loc = *pDst;
        dst_loc.pResource = UnwrapResource(pDst->pResource);
        dst_ptr = &dst_loc;
    }
    if (pSrc) {
        src_loc = *pSrc;
        src_loc.pResource = UnwrapResource(pSrc->pResource);
        src_ptr = &src_loc;
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::CopyTexture;
        op.dst_resource = dst_ptr ? dst_ptr->pResource : nullptr;
        op.src_resource = src_ptr ? src_ptr->pResource : nullptr;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->CopyTextureRegion(dst_ptr, DstX, DstY, DstZ, src_ptr, pSrcBox);
    RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyResource(
    ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) {
    ID3D12Resource* host_dst = UnwrapResource(pDstResource);
    ID3D12Resource* host_src = UnwrapResource(pSrcResource);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::CopyResource;
        op.dst_resource = host_dst;
        op.src_resource = host_src;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->CopyResource(host_dst, host_src);
    RecordList("ID3D12GraphicsCommandListX::CopyResource", 17, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyTiles(
    ID3D12Resource* pTiledResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
    const D3D12_TILE_REGION_SIZE* pTileRegionSize,
    ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes,
    D3D12_TILE_COPY_FLAGS Flags) {
    host_list_->CopyTiles(
        UnwrapResource(pTiledResource), pTileRegionStartCoordinate, pTileRegionSize,
        UnwrapResource(pBuffer), BufferStartOffsetInBytes, Flags);
    RecordList("ID3D12GraphicsCommandListX::CopyTiles", 18, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveSubresource(
    ID3D12Resource* pDstResource, UINT DstSubresource,
    ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) {
    host_list_->ResolveSubresource(
        UnwrapResource(pDstResource), DstSubresource,
        UnwrapResource(pSrcResource), SrcSubresource, Format);
    RecordList("ID3D12GraphicsCommandListX::ResolveSubresource", 19, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetPrimitiveTopology(
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) {
    host_list_->IASetPrimitiveTopology(PrimitiveTopology);
    RecordList("ID3D12GraphicsCommandListX::IASetPrimitiveTopology", 20, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetViewports(
    UINT NumViewports, const D3D12_VIEWPORT* pViewports) {
    if (device_x_ && device_x_->IsVulkanRoutingActive() && NumViewports > 0u && pViewports) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_vp_width_ = static_cast<uint32_t>(std::max(0.0f, pViewports[0].Width));
        vk_vp_height_ = static_cast<uint32_t>(std::max(0.0f, pViewports[0].Height));
    }
    host_list_->RSSetViewports(NumViewports, pViewports);
    RecordList("ID3D12GraphicsCommandListX::RSSetViewports", 21, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetScissorRects(
    UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->RSSetScissorRects(NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::RSSetScissorRects", 22, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetBlendFactor(const FLOAT BlendFactor[4]) {
    host_list_->OMSetBlendFactor(BlendFactor);
    RecordList("ID3D12GraphicsCommandListX::OMSetBlendFactor", 23, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetStencilRef(UINT StencilRef) {
    host_list_->OMSetStencilRef(StencilRef);
    RecordList("ID3D12GraphicsCommandListX::OMSetStencilRef", 24, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetPipelineState(ID3D12PipelineState* pPipelineState) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        const bool is_spirv = device_x_->IsSpirvDirectPipeline(pPipelineState);
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_pso_ = pPipelineState;
        vk_cur_pso_is_spirv_ = is_spirv;
        skip_host = is_spirv;
    }
    if (!skip_host) {
        host_list_->SetPipelineState(UnwrapPipelineState(pPipelineState));
    }
    RecordList("ID3D12GraphicsCommandListX::SetPipelineState", 25, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResourceBarrier(
    UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    if (!pBarriers || NumBarriers == 0u) {
        host_list_->ResourceBarrier(NumBarriers, pBarriers);
        RecordList("ID3D12GraphicsCommandListX::ResourceBarrier", 26, S_OK);
        return;
    }

    constexpr UINT kStackBarriers = 16u;
    D3D12_RESOURCE_BARRIER stack_buf[kStackBarriers];
    std::vector<D3D12_RESOURCE_BARRIER> heap_buf;
    D3D12_RESOURCE_BARRIER* unwrapped = stack_buf;
    if (NumBarriers > kStackBarriers) {
        heap_buf.resize(NumBarriers);
        unwrapped = heap_buf.data();
    }

    const bool track_unified = device_x_ && device_x_->HasRegisteredUnifiedTextures();
    for (UINT i = 0u; i < NumBarriers; ++i) {
        unwrapped[i] = pBarriers[i];
        switch (unwrapped[i].Type) {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
                unwrapped[i].Transition.pResource = UnwrapResource(pBarriers[i].Transition.pResource);
                if (track_unified && unwrapped[i].Transition.pResource != nullptr) {
                    device_x_->NotifyResourceStateTransition(
                        unwrapped[i].Transition.pResource,
                        unwrapped[i].Transition.StateAfter);
                }
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                unwrapped[i].Aliasing.pResourceBefore = UnwrapResource(pBarriers[i].Aliasing.pResourceBefore);
                unwrapped[i].Aliasing.pResourceAfter = UnwrapResource(pBarriers[i].Aliasing.pResourceAfter);
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                unwrapped[i].UAV.pResource = UnwrapResource(pBarriers[i].UAV.pResource);
                break;
            default:
                break;
        }
    }

    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::Barrier;
        op.barrier_count = NumBarriers;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->ResourceBarrier(NumBarriers, unwrapped);
    RecordList("ID3D12GraphicsCommandListX::ResourceBarrier", 26, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) {
    host_list_->ExecuteBundle(UnwrapHostGraphicsList(pCommandList));
    RecordList("ID3D12GraphicsCommandListX::ExecuteBundle", 27, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetDescriptorHeaps(
    UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) {
    if (!ppDescriptorHeaps || NumDescriptorHeaps == 0u) {
        host_list_->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
        RecordList("ID3D12GraphicsCommandListX::SetDescriptorHeaps", 28, S_OK);
        return;
    }
    constexpr UINT kStackHeaps = 8u;
    ID3D12DescriptorHeap* stack_heaps[kStackHeaps];
    std::vector<ID3D12DescriptorHeap*> heap_vec;
    ID3D12DescriptorHeap** unwrapped = stack_heaps;
    if (NumDescriptorHeaps > kStackHeaps) {
        heap_vec.resize(NumDescriptorHeaps);
        unwrapped = heap_vec.data();
    }
    for (UINT i = 0u; i < NumDescriptorHeaps; ++i) {
        unwrapped[i] = UnwrapDescriptorHeap(ppDescriptorHeaps[i]);
    }
    host_list_->SetDescriptorHeaps(NumDescriptorHeaps, unwrapped);
    RecordList("ID3D12GraphicsCommandListX::SetDescriptorHeaps", 28, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootSignature(
    ID3D12RootSignature* pRootSignature) {
    host_list_->SetComputeRootSignature(UnwrapRootSignature(pRootSignature));
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootSignature", 29, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootSignature(
    ID3D12RootSignature* pRootSignature) {
    host_list_->SetGraphicsRootSignature(UnwrapRootSignature(pRootSignature));
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootSignature", 30, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootDescriptorTable(
    UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    host_list_->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootDescriptorTable", 31, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootDescriptorTable(
    UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    host_list_->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootDescriptorTable", 32, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRoot32BitConstant(
    UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_root_constants_.size() <= DestOffsetIn32BitValues) {
            vk_cmp_root_constants_.resize(static_cast<size_t>(DestOffsetIn32BitValues) + 1u, 0u);
        }
        vk_cmp_root_constants_[DestOffsetIn32BitValues] = SrcData;
    }
    host_list_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRoot32BitConstant", 33, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRoot32BitConstant(
    UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_root_constants_.size() <= DestOffsetIn32BitValues) {
            vk_gfx_root_constants_.resize(static_cast<size_t>(DestOffsetIn32BitValues) + 1u, 0u);
        }
        vk_gfx_root_constants_[DestOffsetIn32BitValues] = SrcData;
    }
    host_list_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRoot32BitConstant", 34, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRoot32BitConstants(
    UINT RootParameterIndex, UINT Num32BitValuesToSet,
    const void* pSrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_ && device_x_->IsVulkanRoutingActive() && pSrcData && Num32BitValuesToSet > 0u) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        const size_t req = static_cast<size_t>(DestOffsetIn32BitValues) + Num32BitValuesToSet;
        if (vk_cmp_root_constants_.size() < req) vk_cmp_root_constants_.resize(req, 0u);
        std::memcpy(vk_cmp_root_constants_.data() + DestOffsetIn32BitValues,
                    pSrcData, static_cast<size_t>(Num32BitValuesToSet) * sizeof(uint32_t));
    }
    host_list_->SetComputeRoot32BitConstants(
        RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRoot32BitConstants", 35, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRoot32BitConstants(
    UINT RootParameterIndex, UINT Num32BitValuesToSet,
    const void* pSrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_ && device_x_->IsVulkanRoutingActive() && pSrcData && Num32BitValuesToSet > 0u) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        const size_t req = static_cast<size_t>(DestOffsetIn32BitValues) + Num32BitValuesToSet;
        if (vk_gfx_root_constants_.size() < req) vk_gfx_root_constants_.resize(req, 0u);
        std::memcpy(vk_gfx_root_constants_.data() + DestOffsetIn32BitValues,
                    pSrcData, static_cast<size_t>(Num32BitValuesToSet) * sizeof(uint32_t));
    }
    host_list_->SetGraphicsRoot32BitConstants(
        RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRoot32BitConstants", 36, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_cbvs_.size() <= RootParameterIndex) vk_cmp_cbvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_cmp_cbvs_[RootParameterIndex] = translated;
    }
    host_list_->SetComputeRootConstantBufferView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootConstantBufferView", 37, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_cbvs_.size() <= RootParameterIndex) vk_gfx_cbvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_gfx_cbvs_[RootParameterIndex] = translated;
    }
    host_list_->SetGraphicsRootConstantBufferView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootConstantBufferView", 38, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_srvs_.size() <= RootParameterIndex) vk_cmp_srvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_cmp_srvs_[RootParameterIndex] = translated;
    }
    host_list_->SetComputeRootShaderResourceView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootShaderResourceView", 39, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_srvs_.size() <= RootParameterIndex) vk_gfx_srvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_gfx_srvs_[RootParameterIndex] = translated;
    }
    host_list_->SetGraphicsRootShaderResourceView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootShaderResourceView", 40, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_uavs_.size() <= RootParameterIndex) vk_cmp_uavs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_cmp_uavs_[RootParameterIndex] = translated;
    }
    host_list_->SetComputeRootUnorderedAccessView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootUnorderedAccessView", 41, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_uavs_.size() <= RootParameterIndex) vk_gfx_uavs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_gfx_uavs_[RootParameterIndex] = translated;
    }
    host_list_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootUnorderedAccessView", 42, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (pView) {
            vk_cur_ibv_ = *pView;
            vk_has_ibv_ = true;
        } else {
            vk_has_ibv_ = false;
        }
    }
    host_list_->IASetIndexBuffer(pView);
    RecordList("ID3D12GraphicsCommandListX::IASetIndexBuffer", 43, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetVertexBuffers(
    UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) {
    if (device_x_ && device_x_->IsVulkanRoutingActive() && StartSlot == 0u && NumViews > 0u && pViews) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_vbv_ = pViews[0];
        vk_has_vbv_ = true;
    }
    host_list_->IASetVertexBuffers(StartSlot, NumViews, pViews);
    RecordList("ID3D12GraphicsCommandListX::IASetVertexBuffers", 44, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SOSetTargets(
    UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) {
    host_list_->SOSetTargets(StartSlot, NumViews, pViews);
    RecordList("ID3D12GraphicsCommandListX::SOSetTargets", 45, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetRenderTargets(
    UINT NumRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_rtv_ = (NumRenderTargetDescriptors > 0u && pRenderTargetDescriptors)
                          ? device_x_->ResolveVulkanRtvResource(pRenderTargetDescriptors[0])
                          : nullptr;
        vk_cur_dsv_ = pDepthStencilDescriptor
                          ? device_x_->ResolveVulkanDsvResource(*pDepthStencilDescriptor)
                          : nullptr;
    }
    host_list_->OMSetRenderTargets(
        NumRenderTargetDescriptors, pRenderTargetDescriptors,
        RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    RecordList("ID3D12GraphicsCommandListX::OMSetRenderTargets", 46, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearDepthStencilView(
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
    FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_clear_depth_ = Depth;
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::ClearDsv;
        op.dsv_resource = device_x_->ResolveVulkanDsvResource(DepthStencilView);
        op.clear_depth = Depth;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->ClearDepthStencilView(
        DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearDepthStencilView", 47, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearRenderTargetView(
    D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4],
    UINT NumRects, const D3D12_RECT* pRects) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (ColorRGBA) {
            std::memcpy(vk_clear_color_, ColorRGBA, sizeof(vk_clear_color_));
            vk_has_clear_color_ = true;
        }
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::ClearRtv;
        op.rtv_resource = device_x_->ResolveVulkanRtvResource(RenderTargetView);
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearRenderTargetView", 48, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearUnorderedAccessViewUint(
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
    const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->ClearUnorderedAccessViewUint(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, UnwrapResource(pResource), Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewUint", 49, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearUnorderedAccessViewFloat(
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
    const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->ClearUnorderedAccessViewFloat(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, UnwrapResource(pResource), Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewFloat", 50, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DiscardResource(
    ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) {
    host_list_->DiscardResource(UnwrapResource(pResource), pRegion);
    RecordList("ID3D12GraphicsCommandListX::DiscardResource", 51, S_OK);
}

void STDMETHODCALLTYPE CommandListX::BeginQuery(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    host_list_->BeginQuery(UnwrapQueryHeap(pQueryHeap), Type, Index);
    RecordList("ID3D12GraphicsCommandListX::BeginQuery", 52, S_OK);
}

void STDMETHODCALLTYPE CommandListX::EndQuery(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    host_list_->EndQuery(UnwrapQueryHeap(pQueryHeap), Type, Index);
    RecordList("ID3D12GraphicsCommandListX::EndQuery", 53, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveQueryData(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries,
    ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) {
    host_list_->ResolveQueryData(
        UnwrapQueryHeap(pQueryHeap), Type, StartIndex, NumQueries,
        UnwrapResource(pDestinationBuffer), AlignedDestinationBufferOffset);
    RecordList("ID3D12GraphicsCommandListX::ResolveQueryData", 54, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetPredication(
    ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) {
    host_list_->SetPredication(UnwrapResource(pBuffer), AlignedBufferOffset, Operation);
    RecordList("ID3D12GraphicsCommandListX::SetPredication", 55, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetMarker(UINT Metadata, const void* pData, UINT Size) {
    host_list_->SetMarker(Metadata, pData, Size);
    RecordList("ID3D12GraphicsCommandListX::SetMarker", 56, S_OK);
}

void STDMETHODCALLTYPE CommandListX::BeginEvent(UINT Metadata, const void* pData, UINT Size) {
    host_list_->BeginEvent(Metadata, pData, Size);
    RecordList("ID3D12GraphicsCommandListX::BeginEvent", 57, S_OK);
}

void STDMETHODCALLTYPE CommandListX::EndEvent() {
    host_list_->EndEvent();
    RecordList("ID3D12GraphicsCommandListX::EndEvent", 58, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ExecuteIndirect(
    ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
    ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
    ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) {
    host_list_->ExecuteIndirect(
        UnwrapCommandSignature(pCommandSignature), MaxCommandCount,
        UnwrapResource(pArgumentBuffer), ArgumentBufferOffset,
        UnwrapResource(pCountBuffer), CountBufferOffset);
    RecordList("ID3D12GraphicsCommandListX::ExecuteIndirect", 59, S_OK);
}

// ID3D12GraphicsCommandList1 (slots 60..65)
void STDMETHODCALLTYPE CommandListX::AtomicCopyBufferUINT(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
    ID3D12Resource* const* ppDependentResources,
    const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) {
    if (host_list1_) {
        std::vector<ID3D12Resource*> deps;
        ID3D12Resource* const* dep_ptr = ppDependentResources;
        if (Dependencies > 0u && ppDependentResources) {
            deps.resize(Dependencies);
            for (UINT i = 0u; i < Dependencies; ++i) {
                deps[i] = UnwrapResource(ppDependentResources[i]);
            }
            dep_ptr = deps.data();
        }
        host_list1_->AtomicCopyBufferUINT(
            UnwrapResource(pDstBuffer), DstOffset, UnwrapResource(pSrcBuffer), SrcOffset,
            Dependencies, dep_ptr, pDependentSubresourceRanges);
    }
    RecordList("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT",
               60, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::AtomicCopyBufferUINT64(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
    ID3D12Resource* const* ppDependentResources,
    const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) {
    if (host_list1_) {
        std::vector<ID3D12Resource*> deps;
        ID3D12Resource* const* dep_ptr = ppDependentResources;
        if (Dependencies > 0u && ppDependentResources) {
            deps.resize(Dependencies);
            for (UINT i = 0u; i < Dependencies; ++i) {
                deps[i] = UnwrapResource(ppDependentResources[i]);
            }
            dep_ptr = deps.data();
        }
        host_list1_->AtomicCopyBufferUINT64(
            UnwrapResource(pDstBuffer), DstOffset, UnwrapResource(pSrcBuffer), SrcOffset,
            Dependencies, dep_ptr, pDependentSubresourceRanges);
    }
    RecordList("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT64",
               61, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::OMSetDepthBounds(FLOAT Min, FLOAT Max) {
    depth_bounds_min_.store(Min, std::memory_order_relaxed);
    depth_bounds_max_.store(Max, std::memory_order_relaxed);
    if (host_list1_ && device_x_ && device_x_->SupportsDepthBoundsTest() &&
        list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        host_list1_->OMSetDepthBounds(Min, Max);
    }
    RecordList("ID3D12GraphicsCommandListX::OMSetDepthBounds", 62, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetSamplePositions(
    UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION* pSamplePositions) {
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        sample_positions_per_pixel_ = NumSamplesPerPixel;
        sample_positions_pixels_ = NumPixels;
        const size_t total = static_cast<size_t>(NumSamplesPerPixel) * static_cast<size_t>(NumPixels);
        if (pSamplePositions && total > 0u && total <= 64u) {
            sample_positions_.assign(pSamplePositions, pSamplePositions + total);
        } else {
            sample_positions_.clear();
        }
    }
    if (host_list1_ && device_x_ && device_x_->SupportsProgrammableSamplePositions() &&
        list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        host_list1_->SetSamplePositions(NumSamplesPerPixel, NumPixels, pSamplePositions);
    }
    RecordList("ID3D12GraphicsCommandListX::SetSamplePositions", 63, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveSubresourceRegion(
    ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
    ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect,
    DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode) {
    if (host_list1_) {
        host_list1_->ResolveSubresourceRegion(
            UnwrapResource(pDstResource), DstSubresource, DstX, DstY,
            UnwrapResource(pSrcResource), SrcSubresource, pSrcRect, Format, ResolveMode);
    }
    RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion",
               64, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::SetViewInstanceMask(UINT Mask) {
    view_instance_mask_.store(Mask, std::memory_order_relaxed);
    if (host_list1_ && device_x_ && device_x_->SupportsViewInstancing() &&
        list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        host_list1_->SetViewInstanceMask(Mask);
    }
    RecordList("ID3D12GraphicsCommandListX::SetViewInstanceMask", 65, S_OK);
}

// ID3D12GraphicsCommandList2 (slot 66)
void STDMETHODCALLTYPE CommandListX::WriteBufferImmediate(
    UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams,
    const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes) {
    if (Count == 0u || !pParams) {
        RecordList("ID3D12GraphicsCommandListX::WriteBufferImmediate", 66, S_OK);
        return;
    }

    constexpr UINT kStackParams = 32u;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER stack_params[kStackParams];
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> heap_params;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* translated = stack_params;
    if (Count > kStackParams) {
        heap_params.resize(Count);
        translated = heap_params.data();
    }

    for (UINT i = 0u; i < Count; ++i) {
        translated[i] = pParams[i];
        if (device_x_) {
            translated[i].Dest = device_x_->TranslateGuestGpuVa(pParams[i].Dest);
            if (device_x_->IsVulkanRoutingActive()) {
                uint64_t buf_off = 0ull;
                ID3D12Resource* vk_res =
                    device_x_->ResolveVulkanBufferByGpuVa(translated[i].Dest, &buf_off);
                if (vk_res) {
                    const uint32_t v = translated[i].Value;
                    device_x_->WriteVulkanBufferData(vk_res, buf_off, &v, sizeof(v));
                }
            }
        }
    }

    if (host_list2_) {
        host_list2_->WriteBufferImmediate(Count, translated, pModes);
    } else {
        // Safe CPU fallback when host command list lacks ID3D12GraphicsCommandList2.
        for (UINT i = 0u; i < Count; ++i) {
            const uint64_t addr = translated[i].Dest;
            if (addr != 0ull) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
                                 &mbi, sizeof(mbi)) != 0 &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0u &&
                    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0u) {
                    const uint64_t region_end =
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
                        static_cast<uint64_t>(mbi.RegionSize);
                    if (addr + sizeof(uint32_t) <= region_end) {
                        *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(addr)) =
                            translated[i].Value;
                    }
                }
            }
        }
    }
    write_buffer_immediate_calls_.fetch_add(1ull, std::memory_order_relaxed);
    RecordList("ID3D12GraphicsCommandListX::WriteBufferImmediate", 66, S_OK);
}

// ID3D12GraphicsCommandList3 (slot 67)
void STDMETHODCALLTYPE CommandListX::SetProtectedResourceSession(
    ID3D12ProtectedResourceSession* pProtectedResourceSession) {
    if (host_list3_) host_list3_->SetProtectedResourceSession(pProtectedResourceSession);
    RecordList("ID3D12GraphicsCommandListX::SetProtectedResourceSession",
               67, host_list3_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList4 (slots 68..76)
void STDMETHODCALLTYPE CommandListX::BeginRenderPass(
    UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
    D3D12_RENDER_PASS_FLAGS Flags) {
    if (host_list4_) {
        std::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> rts;
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt_ptr = pRenderTargets;
        if (NumRenderTargets > 0u && pRenderTargets) {
            bool any_resolve = false;
            for (UINT i = 0u; i < NumRenderTargets; ++i) {
                if (pRenderTargets[i].EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
                    any_resolve = true;
                    break;
                }
            }
            if (any_resolve) {
                rts.assign(pRenderTargets, pRenderTargets + NumRenderTargets);
                for (auto& rt : rts) {
                    if (rt.EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
                        rt.EndingAccess.Resolve.pSrcResource = UnwrapResource(rt.EndingAccess.Resolve.pSrcResource);
                        rt.EndingAccess.Resolve.pDstResource = UnwrapResource(rt.EndingAccess.Resolve.pDstResource);
                    }
                }
                rt_ptr = rts.data();
            }
        }
        host_list4_->BeginRenderPass(NumRenderTargets, rt_ptr, pDepthStencil, Flags);
    }
    RecordList("ID3D12GraphicsCommandListX::BeginRenderPass",
               68, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::EndRenderPass() {
    if (host_list4_) host_list4_->EndRenderPass();
    RecordList("ID3D12GraphicsCommandListX::EndRenderPass",
               69, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::InitializeMetaCommand(
    ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData,
    SIZE_T InitializationParametersDataSizeInBytes) {
    if (host_list4_) {
        host_list4_->InitializeMetaCommand(
            pMetaCommand, pInitializationParametersData, InitializationParametersDataSizeInBytes);
    }
    RecordList("ID3D12GraphicsCommandListX::InitializeMetaCommand",
               70, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::ExecuteMetaCommand(
    ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData,
    SIZE_T ExecutionParametersDataSizeInBytes) {
    if (host_list4_) {
        host_list4_->ExecuteMetaCommand(
            pMetaCommand, pExecutionParametersData, ExecutionParametersDataSizeInBytes);
    }
    RecordList("ID3D12GraphicsCommandListX::ExecuteMetaCommand",
               71, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::BuildRaytracingAccelerationStructure(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
    UINT NumPostbuildInfoDescs,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) {
    if (host_list4_) {
        host_list4_->BuildRaytracingAccelerationStructure(
            pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
    }
    RecordList("ID3D12GraphicsCommandListX::BuildRaytracingAccelerationStructure",
               72, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::EmitRaytracingAccelerationStructurePostbuildInfo(
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc,
    UINT NumSourceAccelerationStructures,
    const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) {
    if (host_list4_) {
        host_list4_->EmitRaytracingAccelerationStructurePostbuildInfo(
            pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
    }
    RecordList("ID3D12GraphicsCommandListX::EmitRaytracingAccelerationStructurePostbuildInfo",
               73, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::CopyRaytracingAccelerationStructure(
    D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
    D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) {
    if (host_list4_) {
        host_list4_->CopyRaytracingAccelerationStructure(
            DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
    }
    RecordList("ID3D12GraphicsCommandListX::CopyRaytracingAccelerationStructure",
               74, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::SetPipelineState1(ID3D12StateObject* pStateObject) {
    ID3D12StateObject* host_so = UnwrapStateObject(pStateObject);
    const bool is_sw_proxy = (pStateObject != nullptr && host_so == nullptr);
    if (host_list4_ && !is_sw_proxy) {
        host_list4_->SetPipelineState1(host_so);
    }
    RecordList("ID3D12GraphicsCommandListX::SetPipelineState1",
               75, (host_list4_ || is_sw_proxy) ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) {
    if (host_list4_) host_list4_->DispatchRays(pDesc);
    RecordList("ID3D12GraphicsCommandListX::DispatchRays",
               76, host_list4_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList5 (slots 77..78)
void STDMETHODCALLTYPE CommandListX::RSSetShadingRate(
    D3D12_SHADING_RATE baseShadingRate,
    const D3D12_SHADING_RATE_COMBINER* combiners) {
    if (host_list5_) host_list5_->RSSetShadingRate(baseShadingRate, combiners);
    RecordList("ID3D12GraphicsCommandListX::RSSetShadingRate",
               77, host_list5_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::RSSetShadingRateImage(ID3D12Resource* shadingRateImage) {
    if (host_list5_) host_list5_->RSSetShadingRateImage(UnwrapResource(shadingRateImage));
    RecordList("ID3D12GraphicsCommandListX::RSSetShadingRateImage",
               78, host_list5_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList6 (slot 79)
void STDMETHODCALLTYPE CommandListX::DispatchMesh(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::DispatchMesh;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = ThreadGroupCountX;
        op.y = ThreadGroupCountY;
        op.z = ThreadGroupCountZ;
        op.rtv_resource = vk_cur_rtv_;
        op.dsv_resource = vk_cur_dsv_;
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        op.clear_depth = vk_clear_depth_;
        op.viewport_width = vk_vp_width_;
        op.viewport_height = vk_vp_height_;
        op.root_constants = vk_gfx_root_constants_;
        op.root_cbvs = vk_gfx_cbvs_;
        op.root_srvs = vk_gfx_srvs_;
        op.root_uavs = vk_gfx_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    if (!skip_host && host_list6_) {
        host_list6_->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    RecordList("ID3D12GraphicsCommandListX::DispatchMesh",
               79, (skip_host || host_list6_) ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandListX (slots 80..85)
void STDMETHODCALLTYPE CommandListX::SetGpuVirtualAddressHintX(uint64_t Hint) {
    gpu_va_hint_.store(Hint, std::memory_order_relaxed);
    RecordList("ID3D12GraphicsCommandListX::SetGpuVirtualAddressHintX", 80, S_OK);
}

ID3D12GraphicsCommandList* STDMETHODCALLTYPE CommandListX::GetUnderlyingHostCommandList() {
    RecordList("ID3D12GraphicsCommandListX::GetUnderlyingHostCommandList", 81, S_OK);
    return host_list_;
}

void STDMETHODCALLTYPE CommandListX::KickoffX() {
    kickoff_count_.fetch_add(1ull, std::memory_order_relaxed);
    RecordList("ID3D12GraphicsCommandListX::KickoffX", 82, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Wait32BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint32_t Value,
    uint32_t Mask,
    D3D12XBOX_WAIT_VALUE_OP Operation) {
    (void)Address;
    (void)Value;
    (void)Mask;
    (void)Operation;
    if (host_list_) {
        const D3D12_COMMAND_LIST_TYPE list_type = host_list_->GetType();
        if (list_type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
            list_type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            D3D12_RESOURCE_BARRIER uav_barrier{};
            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav_barrier.UAV.pResource = nullptr;
            host_list_->ResourceBarrier(1u, &uav_barrier);
        }
    }
    RecordList("ID3D12GraphicsCommandListX::Wait32BitValueX", 83, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Wait64BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint64_t Value,
    uint64_t Mask,
    D3D12XBOX_WAIT_VALUE_OP Operation) {
    (void)Address;
    (void)Value;
    (void)Mask;
    (void)Operation;
    if (host_list_) {
        const D3D12_COMMAND_LIST_TYPE list_type = host_list_->GetType();
        if (list_type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
            list_type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            D3D12_RESOURCE_BARRIER uav_barrier{};
            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav_barrier.UAV.pResource = nullptr;
            host_list_->ResourceBarrier(1u, &uav_barrier);
        }
    }
    RecordList("ID3D12GraphicsCommandListX::Wait64BitValueX", 84, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetDepthBiasX(
    FLOAT DepthBias,
    FLOAT DepthBiasClamp,
    FLOAT SlopeScaledDepthBias) {
    depth_bias_.store(DepthBias, std::memory_order_relaxed);
    depth_bias_clamp_.store(DepthBiasClamp, std::memory_order_relaxed);
    slope_scaled_depth_bias_.store(SlopeScaledDepthBias, std::memory_order_relaxed);
    RecordList("ID3D12GraphicsCommandListX::RSSetDepthBiasX", 85, S_OK);
}

void CommandListX::GetDepthBiasXState(
    float* out_depth_bias,
    float* out_depth_bias_clamp,
    float* out_slope_scaled_depth_bias) const noexcept {
    if (out_depth_bias) *out_depth_bias = depth_bias_.load(std::memory_order_relaxed);
    if (out_depth_bias_clamp) *out_depth_bias_clamp = depth_bias_clamp_.load(std::memory_order_relaxed);
    if (out_slope_scaled_depth_bias) {
        *out_slope_scaled_depth_bias = slope_scaled_depth_bias_.load(std::memory_order_relaxed);
    }
}

HRESULT STDMETHODCALLTYPE CommandListX::GetCommandListExtState(
    D3D12XBOX_CMDLIST_EXT_STATE* out_state) const noexcept {
    if (!out_state) return E_POINTER;
    out_state->depth_bounds_min = depth_bounds_min_.load(std::memory_order_relaxed);
    out_state->depth_bounds_max = depth_bounds_max_.load(std::memory_order_relaxed);
    out_state->view_instance_mask = view_instance_mask_.load(std::memory_order_relaxed);
    out_state->write_buffer_immediate_calls =
        write_buffer_immediate_calls_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        out_state->sample_positions_per_pixel = sample_positions_per_pixel_;
        out_state->sample_positions_pixels = sample_positions_pixels_;
    }
    out_state->host_supports_depth_bounds =
        (device_x_ && device_x_->SupportsDepthBoundsTest()) ? 1u : 0u;
    out_state->host_supports_view_instancing =
        (device_x_ && device_x_->SupportsViewInstancing()) ? 1u : 0u;
    out_state->host_supports_sample_positions =
        (device_x_ && device_x_->SupportsProgrammableSamplePositions()) ? 1u : 0u;
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
