#include "command_list_x.h"

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
        host_list_->QueryInterface(IID_PPV_ARGS(&host_list1_));
        host_list_->QueryInterface(IID_PPV_ARGS(&host_list2_));
        host_list_->QueryInterface(IID_PPV_ARGS(&host_list3_));
        host_list_->QueryInterface(IID_PPV_ARGS(&host_list4_));
        host_list_->QueryInterface(IID_PPV_ARGS(&host_list5_));
        host_list_->QueryInterface(IID_PPV_ARGS(&host_list6_));
    }
    auto& reg = GetCommandListRegistry();
    std::unique_lock<std::shared_mutex> lock(reg.mutex);
    reg.map[static_cast<const ID3D12CommandList*>(this)] = host_list_;
}

CommandListX::~CommandListX() {
    {
        auto& reg = GetCommandListRegistry();
        std::unique_lock<std::shared_mutex> lock(reg.mutex);
        reg.map.erase(static_cast<const ID3D12CommandList*>(this));
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

    if (IsEqualIID(riid, __uuidof(IUnknown)) ||
        IsEqualIID(riid, __uuidof(ID3D12Object)) ||
        IsEqualIID(riid, __uuidof(ID3D12DeviceChild)) ||
        IsEqualIID(riid, __uuidof(ID3D12CommandList)) ||
        IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList)) ||
        IsEqualIID(riid, IID_ID3D12GraphicsCommandListX)) {
        *ppvObject = static_cast<ID3D12GraphicsCommandListX*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList1)) && host_list1_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList1*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList2)) && host_list2_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList2*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList3)) && host_list3_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList3*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList4)) && host_list4_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList4*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList5)) && host_list5_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList5*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList6)) && host_list6_) {
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
    const HRESULT hr = host_list_->Reset(pAllocator, pInitialState);
    RecordList("ID3D12GraphicsCommandListX::Reset", 10, hr);
    return hr;
}

void STDMETHODCALLTYPE CommandListX::ClearState(ID3D12PipelineState* pPipelineState) {
    host_list_->ClearState(pPipelineState);
    RecordList("ID3D12GraphicsCommandListX::ClearState", 11, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation) {
    host_list_->DrawInstanced(
        VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    RecordList("ID3D12GraphicsCommandListX::DrawInstanced", 12, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DrawIndexedInstanced(
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation) {
    host_list_->DrawIndexedInstanced(
        IndexCountPerInstance, InstanceCount, StartIndexLocation,
        BaseVertexLocation, StartInstanceLocation);
    RecordList("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 13, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Dispatch(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    host_list_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    RecordList("ID3D12GraphicsCommandListX::Dispatch", 14, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyBufferRegion(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) {
    host_list_->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
    RecordList("ID3D12GraphicsCommandListX::CopyBufferRegion", 15, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyTextureRegion(
    const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
    const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) {
    host_list_->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
    RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyResource(
    ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) {
    host_list_->CopyResource(pDstResource, pSrcResource);
    RecordList("ID3D12GraphicsCommandListX::CopyResource", 17, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyTiles(
    ID3D12Resource* pTiledResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
    const D3D12_TILE_REGION_SIZE* pTileRegionSize,
    ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes,
    D3D12_TILE_COPY_FLAGS Flags) {
    host_list_->CopyTiles(
        pTiledResource, pTileRegionStartCoordinate, pTileRegionSize,
        pBuffer, BufferStartOffsetInBytes, Flags);
    RecordList("ID3D12GraphicsCommandListX::CopyTiles", 18, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveSubresource(
    ID3D12Resource* pDstResource, UINT DstSubresource,
    ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) {
    host_list_->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
    RecordList("ID3D12GraphicsCommandListX::ResolveSubresource", 19, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetPrimitiveTopology(
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) {
    host_list_->IASetPrimitiveTopology(PrimitiveTopology);
    RecordList("ID3D12GraphicsCommandListX::IASetPrimitiveTopology", 20, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetViewports(
    UINT NumViewports, const D3D12_VIEWPORT* pViewports) {
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
    host_list_->SetPipelineState(pPipelineState);
    RecordList("ID3D12GraphicsCommandListX::SetPipelineState", 25, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResourceBarrier(
    UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    if (device_x_ && pBarriers && NumBarriers > 0u && device_x_->HasRegisteredUnifiedTextures()) {
        for (UINT i = 0; i < NumBarriers; ++i) {
            if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                pBarriers[i].Transition.pResource != nullptr) {
                device_x_->NotifyResourceStateTransition(
                    pBarriers[i].Transition.pResource,
                    pBarriers[i].Transition.StateAfter);
            }
        }
    }
    host_list_->ResourceBarrier(NumBarriers, pBarriers);
    RecordList("ID3D12GraphicsCommandListX::ResourceBarrier", 26, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) {
    host_list_->ExecuteBundle(UnwrapHostGraphicsList(pCommandList));
    RecordList("ID3D12GraphicsCommandListX::ExecuteBundle", 27, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetDescriptorHeaps(
    UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) {
    host_list_->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
    RecordList("ID3D12GraphicsCommandListX::SetDescriptorHeaps", 28, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootSignature(
    ID3D12RootSignature* pRootSignature) {
    host_list_->SetComputeRootSignature(pRootSignature);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootSignature", 29, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootSignature(
    ID3D12RootSignature* pRootSignature) {
    host_list_->SetGraphicsRootSignature(pRootSignature);
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
    host_list_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRoot32BitConstant", 33, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRoot32BitConstant(
    UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) {
    host_list_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRoot32BitConstant", 34, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRoot32BitConstants(
    UINT RootParameterIndex, UINT Num32BitValuesToSet,
    const void* pSrcData, UINT DestOffsetIn32BitValues) {
    host_list_->SetComputeRoot32BitConstants(
        RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRoot32BitConstants", 35, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRoot32BitConstants(
    UINT RootParameterIndex, UINT Num32BitValuesToSet,
    const void* pSrcData, UINT DestOffsetIn32BitValues) {
    host_list_->SetGraphicsRoot32BitConstants(
        RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRoot32BitConstants", 36, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    host_list_->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootConstantBufferView", 37, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    host_list_->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootConstantBufferView", 38, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    host_list_->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootShaderResourceView", 39, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    host_list_->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootShaderResourceView", 40, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    host_list_->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootUnorderedAccessView", 41, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    host_list_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootUnorderedAccessView", 42, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) {
    host_list_->IASetIndexBuffer(pView);
    RecordList("ID3D12GraphicsCommandListX::IASetIndexBuffer", 43, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetVertexBuffers(
    UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) {
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
    host_list_->OMSetRenderTargets(
        NumRenderTargetDescriptors, pRenderTargetDescriptors,
        RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    RecordList("ID3D12GraphicsCommandListX::OMSetRenderTargets", 46, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearDepthStencilView(
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
    FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->ClearDepthStencilView(
        DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearDepthStencilView", 47, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearRenderTargetView(
    D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4],
    UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearRenderTargetView", 48, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearUnorderedAccessViewUint(
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
    const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->ClearUnorderedAccessViewUint(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewUint", 49, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearUnorderedAccessViewFloat(
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
    const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->ClearUnorderedAccessViewFloat(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewFloat", 50, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DiscardResource(
    ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) {
    host_list_->DiscardResource(pResource, pRegion);
    RecordList("ID3D12GraphicsCommandListX::DiscardResource", 51, S_OK);
}

void STDMETHODCALLTYPE CommandListX::BeginQuery(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    host_list_->BeginQuery(pQueryHeap, Type, Index);
    RecordList("ID3D12GraphicsCommandListX::BeginQuery", 52, S_OK);
}

void STDMETHODCALLTYPE CommandListX::EndQuery(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    host_list_->EndQuery(pQueryHeap, Type, Index);
    RecordList("ID3D12GraphicsCommandListX::EndQuery", 53, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveQueryData(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries,
    ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) {
    host_list_->ResolveQueryData(
        pQueryHeap, Type, StartIndex, NumQueries,
        pDestinationBuffer, AlignedDestinationBufferOffset);
    RecordList("ID3D12GraphicsCommandListX::ResolveQueryData", 54, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetPredication(
    ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) {
    host_list_->SetPredication(pBuffer, AlignedBufferOffset, Operation);
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
        pCommandSignature, MaxCommandCount, pArgumentBuffer,
        ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
    RecordList("ID3D12GraphicsCommandListX::ExecuteIndirect", 59, S_OK);
}

// ID3D12GraphicsCommandList1 (slots 60..65)
void STDMETHODCALLTYPE CommandListX::AtomicCopyBufferUINT(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
    ID3D12Resource* const* ppDependentResources,
    const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) {
    if (host_list1_) {
        host_list1_->AtomicCopyBufferUINT(
            pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies,
            ppDependentResources, pDependentSubresourceRanges);
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
        host_list1_->AtomicCopyBufferUINT64(
            pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies,
            ppDependentResources, pDependentSubresourceRanges);
    }
    RecordList("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT64",
               61, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::OMSetDepthBounds(FLOAT Min, FLOAT Max) {
    if (host_list1_) host_list1_->OMSetDepthBounds(Min, Max);
    RecordList("ID3D12GraphicsCommandListX::OMSetDepthBounds",
               62, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::SetSamplePositions(
    UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION* pSamplePositions) {
    if (host_list1_) {
        host_list1_->SetSamplePositions(NumSamplesPerPixel, NumPixels, pSamplePositions);
    }
    RecordList("ID3D12GraphicsCommandListX::SetSamplePositions",
               63, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::ResolveSubresourceRegion(
    ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
    ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect,
    DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode) {
    if (host_list1_) {
        host_list1_->ResolveSubresourceRegion(
            pDstResource, DstSubresource, DstX, DstY,
            pSrcResource, SrcSubresource, pSrcRect, Format, ResolveMode);
    }
    RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion",
               64, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::SetViewInstanceMask(UINT Mask) {
    if (host_list1_) host_list1_->SetViewInstanceMask(Mask);
    RecordList("ID3D12GraphicsCommandListX::SetViewInstanceMask",
               65, host_list1_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList2 (slot 66)
void STDMETHODCALLTYPE CommandListX::WriteBufferImmediate(
    UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams,
    const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes) {
    if (host_list2_) host_list2_->WriteBufferImmediate(Count, pParams, pModes);
    RecordList("ID3D12GraphicsCommandListX::WriteBufferImmediate",
               66, host_list2_ ? S_OK : E_NOINTERFACE);
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
        host_list4_->BeginRenderPass(NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
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
    if (host_list4_) host_list4_->SetPipelineState1(pStateObject);
    RecordList("ID3D12GraphicsCommandListX::SetPipelineState1",
               75, host_list4_ ? S_OK : E_NOINTERFACE);
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
    if (host_list5_) host_list5_->RSSetShadingRateImage(shadingRateImage);
    RecordList("ID3D12GraphicsCommandListX::RSSetShadingRateImage",
               78, host_list5_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList6 (slot 79)
void STDMETHODCALLTYPE CommandListX::DispatchMesh(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    if (host_list6_) {
        host_list6_->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    RecordList("ID3D12GraphicsCommandListX::DispatchMesh",
               79, host_list6_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandListX (slots 80..81)
void STDMETHODCALLTYPE CommandListX::SetGpuVirtualAddressHintX(uint64_t Hint) {
    gpu_va_hint_.store(Hint, std::memory_order_relaxed);
    RecordList("ID3D12GraphicsCommandListX::SetGpuVirtualAddressHintX", 80, S_OK);
}

ID3D12GraphicsCommandList* STDMETHODCALLTYPE CommandListX::GetUnderlyingHostCommandList() {
    RecordList("ID3D12GraphicsCommandListX::GetUnderlyingHostCommandList", 81, S_OK);
    return host_list_;
}

}  // namespace openscarlett::d3d12_xs
