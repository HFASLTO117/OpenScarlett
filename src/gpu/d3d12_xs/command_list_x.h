#pragma once

#include <atomic>
#include <cstdint>

#include "openscarlett/gpu/d3d12_xs_abi.h"

namespace openscarlett::d3d12_xs {

class DeviceX;

class CommandListX final : public ID3D12GraphicsCommandListX {
public:
    CommandListX(DeviceX* parent_device, ID3D12GraphicsCommandList* host_list);
    ~CommandListX();

    CommandListX(const CommandListX&) = delete;
    CommandListX& operator=(const CommandListX&) = delete;

    ID3D12GraphicsCommandList* HostList() const noexcept { return host_list_; }

    // Unwraps a CommandListX* back to its underlying host ID3D12CommandList*,
    // or returns the pointer unchanged if it is already a native host command list.
    static ID3D12CommandList* UnwrapHostList(ID3D12CommandList* maybe_wrapped) noexcept;
    static ID3D12GraphicsCommandList* UnwrapHostGraphicsList(
        ID3D12GraphicsCommandList* maybe_wrapped) noexcept;

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

    // ID3D12CommandList (slot 8)
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override;

    // ID3D12GraphicsCommandList (slots 9..59)
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(
        ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE DrawInstanced(
        UINT VertexCountPerInstance, UINT InstanceCount,
        UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(
        UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
        INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE Dispatch(
        UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE CopyBufferRegion(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(
        ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) override;
    void STDMETHODCALLTYPE CopyTiles(
        ID3D12Resource* pTiledResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pTileRegionSize,
        ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes,
        D3D12_TILE_COPY_FLAGS Flags) override;
    void STDMETHODCALLTYPE ResolveSubresource(
        ID3D12Resource* pDstResource, UINT DstSubresource,
        ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(
        D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override;
    void STDMETHODCALLTYPE RSSetViewports(
        UINT NumViewports, const D3D12_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(
        UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE ResourceBarrier(
        UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(
        UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(
        ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(
        ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(
        UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE SOSetTargets(
        UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(
        UINT NumRenderTargetDescriptors,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
        BOOL RTsSingleHandleToDescriptorRange,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(
        D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
        FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(
        D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4],
        UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
        const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
        const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE DiscardResource(
        ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) override;
    void STDMETHODCALLTYPE BeginQuery(
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE EndQuery(
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE ResolveQueryData(
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries,
        ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override;
    void STDMETHODCALLTYPE SetPredication(
        ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset,
        D3D12_PREDICATION_OP Operation) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE ExecuteIndirect(
        ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
        ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
        ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) override;

    // ID3D12GraphicsCommandList1 (slots 60..65)
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
        ID3D12Resource* const* ppDependentResources,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override;
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
        ID3D12Resource* const* ppDependentResources,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override;
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT Min, FLOAT Max) override;
    void STDMETHODCALLTYPE SetSamplePositions(
        UINT NumSamplesPerPixel, UINT NumPixels,
        D3D12_SAMPLE_POSITION* pSamplePositions) override;
    void STDMETHODCALLTYPE ResolveSubresourceRegion(
        ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
        ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect,
        DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode) override;
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT Mask) override;

    // ID3D12GraphicsCommandList2 (slot 66)
    void STDMETHODCALLTYPE WriteBufferImmediate(
        UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes) override;

    // ID3D12GraphicsCommandList3 (slot 67)
    void STDMETHODCALLTYPE SetProtectedResourceSession(
        ID3D12ProtectedResourceSession* pProtectedResourceSession) override;

    // ID3D12GraphicsCommandList4 (slots 68..76)
    void STDMETHODCALLTYPE BeginRenderPass(
        UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
        D3D12_RENDER_PASS_FLAGS Flags) override;
    void STDMETHODCALLTYPE EndRenderPass() override;
    void STDMETHODCALLTYPE InitializeMetaCommand(
        ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData,
        SIZE_T InitializationParametersDataSizeInBytes) override;
    void STDMETHODCALLTYPE ExecuteMetaCommand(
        ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData,
        SIZE_T ExecutionParametersDataSizeInBytes) override;
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
        UINT NumPostbuildInfoDescs,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) override;
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc,
        UINT NumSourceAccelerationStructures,
        const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) override;
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
        D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
        D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) override;
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* pStateObject) override;
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) override;

    // ID3D12GraphicsCommandList5 (slots 77..78)
    void STDMETHODCALLTYPE RSSetShadingRate(
        D3D12_SHADING_RATE baseShadingRate,
        const D3D12_SHADING_RATE_COMBINER* combiners) override;
    void STDMETHODCALLTYPE RSSetShadingRateImage(
        ID3D12Resource* shadingRateImage) override;

    // ID3D12GraphicsCommandList6 (slot 79)
    void STDMETHODCALLTYPE DispatchMesh(
        UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;

    // ID3D12GraphicsCommandListX (slots 80..85)
    void STDMETHODCALLTYPE SetGpuVirtualAddressHintX(uint64_t Hint) override;
    ID3D12GraphicsCommandList* STDMETHODCALLTYPE GetUnderlyingHostCommandList() override;
    void STDMETHODCALLTYPE KickoffX() override;
    void STDMETHODCALLTYPE Wait32BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint32_t Value,
        uint32_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) override;
    void STDMETHODCALLTYPE Wait64BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint64_t Value,
        uint64_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) override;
    void STDMETHODCALLTYPE RSSetDepthBiasX(
        FLOAT DepthBias,
        FLOAT DepthBiasClamp,
        FLOAT SlopeScaledDepthBias) override;

    // Inspection helper for tests/runtime diagnostics.
    void GetDepthBiasXState(
        float* out_depth_bias,
        float* out_depth_bias_clamp,
        float* out_slope_scaled_depth_bias) const noexcept;
    uint64_t KickoffCount() const noexcept { return kickoff_count_.load(std::memory_order_relaxed); }

private:
    std::atomic<ULONG> ref_count_{1};
    DeviceX* device_x_ = nullptr;
    ID3D12GraphicsCommandList* host_list_ = nullptr;
    ID3D12GraphicsCommandList1* host_list1_ = nullptr;
    ID3D12GraphicsCommandList2* host_list2_ = nullptr;
    ID3D12GraphicsCommandList3* host_list3_ = nullptr;
    ID3D12GraphicsCommandList4* host_list4_ = nullptr;
    ID3D12GraphicsCommandList5* host_list5_ = nullptr;
    ID3D12GraphicsCommandList6* host_list6_ = nullptr;
    std::atomic<uint64_t> gpu_va_hint_{0};
    std::atomic<uint64_t> kickoff_count_{0};
    std::atomic<float> depth_bias_{0.0f};
    std::atomic<float> depth_bias_clamp_{0.0f};
    std::atomic<float> slope_scaled_depth_bias_{0.0f};
};

}  // namespace openscarlett::d3d12_xs
