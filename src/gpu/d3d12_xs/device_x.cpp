#include "device_x.h"

#include <cstddef>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "command_list_x.h"
#include "command_queue_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_disasm.h"

namespace openscarlett::d3d12_xs {

namespace {

constexpr const char* kModule = "d3d12_xs.dll";

inline void RecordDev(const char* method, int32_t slot, HRESULT hr = S_OK) {
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        method,
        SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        static_cast<int32_t>(hr),
        slot);
}

void UnwrapPageables(
    UINT count,
    ID3D12Pageable* const* src,
    std::vector<ID3D12Pageable*>& dst) {
    dst.resize(count);
    for (UINT i = 0; i < count; ++i) {
        dst[i] = CommandQueueX::UnwrapHostPageable(src[i]);
    }
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // D3D12 pipeline state stream subobjects require pointer alignment
#endif
template <typename T>
struct alignas(void*) StreamSubobjLayout {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool IsShaderSubobjectType(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) noexcept {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            return true;
        default:
            return false;
    }
}

size_t GetSubobjectStrideBytes(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) noexcept {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            return sizeof(StreamSubobjLayout<ID3D12RootSignature*>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            return sizeof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            return sizeof(StreamSubobjLayout<D3D12_STREAM_OUTPUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            return sizeof(StreamSubobjLayout<D3D12_BLEND_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            return sizeof(StreamSubobjLayout<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
            return sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            return sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            return sizeof(StreamSubobjLayout<D3D12_INPUT_LAYOUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            return sizeof(StreamSubobjLayout<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            return sizeof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            return sizeof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            return sizeof(StreamSubobjLayout<DXGI_FORMAT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            return sizeof(StreamSubobjLayout<DXGI_SAMPLE_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            return sizeof(StreamSubobjLayout<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            return sizeof(StreamSubobjLayout<D3D12_CACHED_PIPELINE_STATE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            return sizeof(StreamSubobjLayout<D3D12_PIPELINE_STATE_FLAGS>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            return sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC1>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            return sizeof(StreamSubobjLayout<D3D12_VIEW_INSTANCING_DESC>);
        default:
            return 0u;
    }
}

}  // namespace

DeviceX::DeviceX(
    ID3D12Device* host_device,
    const D3D12AdapterInfo& adapter_info,
    HWND default_hwnd)
    : host_dev_(host_device), adapter_info_(adapter_info), default_hwnd_(default_hwnd) {
    if (host_dev_) {
        host_dev_->AddRef();
        host_dev_->QueryInterface(IID_PPV_ARGS(&host_dev1_));
        host_dev_->QueryInterface(IID_PPV_ARGS(&host_dev2_));
        host_dev_->QueryInterface(IID_PPV_ARGS(&host_dev3_));
        host_dev_->QueryInterface(IID_PPV_ARGS(&host_dev4_));
        host_dev_->QueryInterface(IID_PPV_ARGS(&host_dev5_));
        host_dev_->QueryInterface(IID_PPV_ARGS(&host_dev6_));
    }
}

DeviceX::~DeviceX() {
    shader_cache_.WaitForPendingCompiles();
    if (host_dev6_) host_dev6_->Release();
    if (host_dev5_) host_dev5_->Release();
    if (host_dev4_) host_dev4_->Release();
    if (host_dev3_) host_dev3_->Release();
    if (host_dev2_) host_dev2_->Release();
    if (host_dev1_) host_dev1_->Release();
    if (host_dev_) host_dev_->Release();
}

// IUnknown (slots 0..2)
HRESULT STDMETHODCALLTYPE DeviceX::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        RecordDev("ID3D12DeviceX::QueryInterface", 0, E_POINTER);
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (IsEqualIID(riid, __uuidof(IUnknown)) ||
        IsEqualIID(riid, __uuidof(ID3D12Object)) ||
        IsEqualIID(riid, __uuidof(ID3D12Device)) ||
        IsEqualIID(riid, IID_ID3D12DeviceX)) {
        *ppvObject = static_cast<ID3D12DeviceX*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12Device1)) && host_dev1_) {
        *ppvObject = static_cast<ID3D12Device1*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12Device2)) && host_dev2_) {
        *ppvObject = static_cast<ID3D12Device2*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12Device3)) && host_dev3_) {
        *ppvObject = static_cast<ID3D12Device3*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12Device4)) && host_dev4_) {
        *ppvObject = static_cast<ID3D12Device4*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12Device5)) && host_dev5_) {
        *ppvObject = static_cast<ID3D12Device5*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D12Device6)) && host_dev6_) {
        *ppvObject = static_cast<ID3D12Device6*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }

    // Forward auxiliary diagnostic/newer host interfaces (e.g. ID3D12InfoQueue) to host device.
    const HRESULT hr = host_dev_ ? host_dev_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::QueryInterface", 0, hr);
    return hr;
}

ULONG STDMETHODCALLTYPE DeviceX::AddRef() {
    const ULONG refs = ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    RecordDev("ID3D12DeviceX::AddRef", 1, S_OK);
    return refs;
}

ULONG STDMETHODCALLTYPE DeviceX::Release() {
    RecordDev("ID3D12DeviceX::Release", 2, S_OK);
    const ULONG rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete this;
    }
    return rem;
}

// ID3D12Object (slots 3..6)
HRESULT STDMETHODCALLTYPE DeviceX::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    const HRESULT hr = host_dev_->GetPrivateData(guid, pDataSize, pData);
    RecordDev("ID3D12DeviceX::GetPrivateData", 3, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    const HRESULT hr = host_dev_->SetPrivateData(guid, DataSize, pData);
    RecordDev("ID3D12DeviceX::SetPrivateData", 4, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    const HRESULT hr = host_dev_->SetPrivateDataInterface(guid, pData);
    RecordDev("ID3D12DeviceX::SetPrivateDataInterface", 5, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetName(LPCWSTR Name) {
    const HRESULT hr = host_dev_->SetName(Name);
    RecordDev("ID3D12DeviceX::SetName", 6, hr);
    return hr;
}

// ID3D12Device (slots 7..43)
UINT STDMETHODCALLTYPE DeviceX::GetNodeCount() {
    const UINT n = host_dev_->GetNodeCount();
    RecordDev("ID3D12DeviceX::GetNodeCount", 7, S_OK);
    return n;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandQueue(
    const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) {
    if (!ppCommandQueue) {
        RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, E_POINTER);
        return E_POINTER;
    }
    *ppCommandQueue = nullptr;
    ID3D12CommandQueue* host_q = nullptr;
    HRESULT hr = host_dev_->CreateCommandQueue(pDesc, IID_PPV_ARGS(&host_q));
    if (FAILED(hr) || !host_q) {
        RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, hr);
        return hr;
    }
    auto* wrapper = new (std::nothrow) CommandQueueX(this, host_q, default_hwnd_);
    host_q->Release();
    if (!wrapper) {
        RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = wrapper->QueryInterface(riid, ppCommandQueue);
    wrapper->Release();
    RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) {
    const HRESULT hr = host_dev_->CreateCommandAllocator(type, riid, ppCommandAllocator);
    RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    if (!pDesc) {
        const HRESULT hr = host_dev_->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }

    const bool lift_vs = openscarlett::IsXboxShaderBytecode(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
    const bool lift_ps = openscarlett::IsXboxShaderBytecode(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
    const bool lift_ds = openscarlett::IsXboxShaderBytecode(pDesc->DS.pShaderBytecode, pDesc->DS.BytecodeLength);
    const bool lift_hs = openscarlett::IsXboxShaderBytecode(pDesc->HS.pShaderBytecode, pDesc->HS.BytecodeLength);
    const bool lift_gs = openscarlett::IsXboxShaderBytecode(pDesc->GS.pShaderBytecode, pDesc->GS.BytecodeLength);

    if (!lift_vs && !lift_ps && !lift_ds && !lift_hs && !lift_gs) {
        // Standard host PC DXBC/DXIL passthrough with zero overhead
        const HRESULT hr = host_dev_->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lifted_desc = *pDesc;
    std::deque<std::vector<uint8_t>> lifted_blobs;

    openscarlett::ShaderCacheLiftRequest req{};
    req.highest_shader_model = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
    req.mesh_has_payload = false;
    req.prefer_dxil_standin = (req.highest_shader_model >= 0x60u);

    auto lift_stage = [&](D3D12_SHADER_BYTECODE& bc, bool do_lift) -> bool {
        if (!do_lift) return true;
        openscarlett::CachedShaderResult res{};
        std::string diag;
        auto st = shader_cache_.GetOrLiftXboxShader(bc.pShaderBytecode, bc.BytecodeLength, req, res, &diag);
        if (!st.ok()) {
            return false;
        }
        lifted_blobs.push_back(std::move(res.dxil));
        bc.pShaderBytecode = lifted_blobs.back().data();
        bc.BytecodeLength = lifted_blobs.back().size();
        return true;
    };

    if (!lift_stage(lifted_desc.VS, lift_vs) ||
        !lift_stage(lifted_desc.PS, lift_ps) ||
        !lift_stage(lifted_desc.DS, lift_ds) ||
        !lift_stage(lifted_desc.HS, lift_hs) ||
        !lift_stage(lifted_desc.GS, lift_gs)) {
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_INVALIDARG);
        return E_INVALIDARG;
    }

    const HRESULT hr = host_dev_->CreateGraphicsPipelineState(&lifted_desc, riid, ppPipelineState);
    RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateComputePipelineState(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    if (!pDesc || !openscarlett::IsXboxShaderBytecode(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength)) {
        // Standard host PC DXBC/DXIL passthrough with zero overhead
        const HRESULT hr = host_dev_->CreateComputePipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }

    openscarlett::ShaderCacheLiftRequest req{};
    req.highest_shader_model = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
    req.mesh_has_payload = false;
    req.prefer_dxil_standin = false;  // Single-stage CS can use <1ms SM 5.0 DXBC stand-in for instant async PSO

    openscarlett::CachedShaderResult lifted{};
    std::string diag;
    auto st = shader_cache_.GetOrLiftXboxShader(
        pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength, req, lifted, &diag);
    if (!st.ok()) {
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_INVALIDARG);
        return E_INVALIDARG;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC lifted_desc = *pDesc;
    lifted_desc.CS.pShaderBytecode = lifted.dxil.data();
    lifted_desc.CS.BytecodeLength = lifted.dxil.size();

    const HRESULT hr = host_dev_->CreateComputePipelineState(&lifted_desc, riid, ppPipelineState);
    RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandList(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
    ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
    if (!ppCommandList) {
        RecordDev("ID3D12DeviceX::CreateCommandList", 12, E_POINTER);
        return E_POINTER;
    }
    *ppCommandList = nullptr;
    ID3D12GraphicsCommandList* host_cl = nullptr;
    HRESULT hr = host_dev_->CreateCommandList(
        nodeMask, type, pCommandAllocator, pInitialState, IID_PPV_ARGS(&host_cl));
    if (FAILED(hr) || !host_cl) {
        RecordDev("ID3D12DeviceX::CreateCommandList", 12, hr);
        return hr;
    }
    auto* wrapper = new (std::nothrow) CommandListX(this, host_cl);
    host_cl->Release();
    if (!wrapper) {
        RecordDev("ID3D12DeviceX::CreateCommandList", 12, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = wrapper->QueryInterface(riid, ppCommandList);
    wrapper->Release();
    RecordDev("ID3D12DeviceX::CreateCommandList", 12, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CheckFeatureSupport(
    D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) {
    const HRESULT hr = host_dev_->CheckFeatureSupport(
        Feature, pFeatureSupportData, FeatureSupportDataSize);
    RecordDev("ID3D12DeviceX::CheckFeatureSupport", 13, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateDescriptorHeap(
    const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) {
    const HRESULT hr = host_dev_->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);
    RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
    return hr;
}

UINT STDMETHODCALLTYPE DeviceX::GetDescriptorHandleIncrementSize(
    D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) {
    const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapType);
    RecordDev("ID3D12DeviceX::GetDescriptorHandleIncrementSize", 15, S_OK);
    return inc;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateRootSignature(
    UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes,
    REFIID riid, void** ppvRootSignature) {
    const HRESULT hr = host_dev_->CreateRootSignature(
        nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
    RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::CreateConstantBufferView(
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateConstantBufferView(pDesc, DestDescriptor);
    RecordDev("ID3D12DeviceX::CreateConstantBufferView", 17, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateShaderResourceView(
    ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateShaderResourceView(pResource, pDesc, DestDescriptor);
    RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateUnorderedAccessView(
    ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor);
    RecordDev("ID3D12DeviceX::CreateUnorderedAccessView", 19, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateRenderTargetView(
    ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateRenderTargetView(pResource, pDesc, DestDescriptor);
    RecordDev("ID3D12DeviceX::CreateRenderTargetView", 20, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateDepthStencilView(
    ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateDepthStencilView(pResource, pDesc, DestDescriptor);
    RecordDev("ID3D12DeviceX::CreateDepthStencilView", 21, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateSampler(
    const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateSampler(pDesc, DestDescriptor);
    RecordDev("ID3D12DeviceX::CreateSampler", 22, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CopyDescriptors(
    UINT NumDestDescriptorRanges,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
    const UINT* pDestDescriptorRangeSizes,
    UINT NumSrcDescriptorRanges,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
    const UINT* pSrcDescriptorRangeSizes,
    D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    host_dev_->CopyDescriptors(
        NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
        NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
        DescriptorHeapsType);
    RecordDev("ID3D12DeviceX::CopyDescriptors", 23, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CopyDescriptorsSimple(
    UINT NumDescriptors,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
    D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
    D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    host_dev_->CopyDescriptorsSimple(
        NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);
    RecordDev("ID3D12DeviceX::CopyDescriptorsSimple", 24, S_OK);
}

D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo(
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) {
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        host_dev_->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);
    RecordDev("ID3D12DeviceX::GetResourceAllocationInfo", 25, S_OK);
    return info;
}

D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE DeviceX::GetCustomHeapProperties(
    UINT nodeMask, D3D12_HEAP_TYPE heapType) {
    const D3D12_HEAP_PROPERTIES props = host_dev_->GetCustomHeapProperties(nodeMask, heapType);
    RecordDev("ID3D12DeviceX::GetCustomHeapProperties", 26, S_OK);
    return props;
}

D3D12_HEAP_FLAGS SanitizeXboxHeapFlags(D3D12_HEAP_FLAGS flags) noexcept {
    // Xbox GDK titles (e.g. Microsoft Xbox-GDK-Samples DeviceResources) pass
    // D3D12_HEAP_FLAG_ALLOW_DISPLAY on swapchain/scanout committed textures,
    // which desktop D3D12 rejects with E_INVALIDARG.
    constexpr uint32_t kMaskOut = static_cast<uint32_t>(D3D12_HEAP_FLAG_ALLOW_DISPLAY);
    return static_cast<D3D12_HEAP_FLAGS>(static_cast<uint32_t>(flags) & ~kMaskOut);
}

D3D12_RESOURCE_DESC SanitizeXboxResourceDesc(const D3D12_RESOURCE_DESC& desc) noexcept {
    // Strip Xbox-exclusive upper D3D12XBOX_RESOURCE_FLAG_* bits (e.g. ALLOW_DCC = 0x80000000)
    // while keeping all standard desktop D3D12_RESOURCE_FLAGS intact.
    D3D12_RESOURCE_DESC out = desc;
    constexpr uint32_t kDesktopResourceFlagsMask = 0x000000FFu;
    out.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(desc.Flags) & kDesktopResourceFlagsMask);
    return out;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) {
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        desc_ptr = &clean_desc;
    }
    const HRESULT hr = host_dev_->CreateCommittedResource(
        pHeapProperties, SanitizeXboxHeapFlags(HeapFlags), desc_ptr, InitialResourceState,
        pOptimizedClearValue, riidResource, ppvResource);
    RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateHeap(
    const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) {
    D3D12_HEAP_DESC clean_heap{};
    const D3D12_HEAP_DESC* heap_ptr = pDesc;
    if (pDesc) {
        clean_heap = *pDesc;
        clean_heap.Flags = SanitizeXboxHeapFlags(pDesc->Flags);
        heap_ptr = &clean_heap;
    }
    const HRESULT hr = host_dev_->CreateHeap(heap_ptr, riid, ppvHeap);
    RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreatePlacedResource(
    ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource) {
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        desc_ptr = &clean_desc;
    }
    const HRESULT hr = host_dev_->CreatePlacedResource(
        pHeap, HeapOffset, desc_ptr, InitialState, pOptimizedClearValue, riid, ppvResource);
    RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateReservedResource(
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        desc_ptr = &clean_desc;
    }
    const HRESULT hr = host_dev_->CreateReservedResource(
        desc_ptr, InitialState, pOptimizedClearValue, riid, ppvResource);
    RecordDev("ID3D12DeviceX::CreateReservedResource", 30, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateSharedHandle(
    ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
    DWORD Access, LPCWSTR Name, HANDLE* pHandle) {
    const HRESULT hr = host_dev_->CreateSharedHandle(pObject, pAttributes, Access, Name, pHandle);
    RecordDev("ID3D12DeviceX::CreateSharedHandle", 31, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::OpenSharedHandle(
    HANDLE NTHandle, REFIID riid, void** ppvObj) {
    const HRESULT hr = host_dev_->OpenSharedHandle(NTHandle, riid, ppvObj);
    RecordDev("ID3D12DeviceX::OpenSharedHandle", 32, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::OpenSharedHandleByName(
    LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) {
    const HRESULT hr = host_dev_->OpenSharedHandleByName(Name, Access, pNTHandle);
    RecordDev("ID3D12DeviceX::OpenSharedHandleByName", 33, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::MakeResident(
    UINT NumObjects, ID3D12Pageable* const* ppObjects) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev_->MakeResident(NumObjects, unwrapped.data());
    RecordDev("ID3D12DeviceX::MakeResident", 34, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::Evict(
    UINT NumObjects, ID3D12Pageable* const* ppObjects) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev_->Evict(NumObjects, unwrapped.data());
    RecordDev("ID3D12DeviceX::Evict", 35, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateFence(
    UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) {
    const HRESULT hr = host_dev_->CreateFence(InitialValue, Flags, riid, ppFence);
    RecordDev("ID3D12DeviceX::CreateFence", 36, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetDeviceRemovedReason() {
    const HRESULT hr = host_dev_->GetDeviceRemovedReason();
    RecordDev("ID3D12DeviceX::GetDeviceRemovedReason", 37, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::GetCopyableFootprints(
    const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
    UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
    UINT64* pRowSizeInBytes, UINT64* pTotalBytes) {
    host_dev_->GetCopyableFootprints(
        pResourceDesc, FirstSubresource, NumSubresources, BaseOffset,
        pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
    RecordDev("ID3D12DeviceX::GetCopyableFootprints", 38, S_OK);
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateQueryHeap(
    const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) {
    const HRESULT hr = host_dev_->CreateQueryHeap(pDesc, riid, ppvHeap);
    RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetStablePowerState(BOOL Enable) {
    const HRESULT hr = host_dev_->SetStablePowerState(Enable);
    RecordDev("ID3D12DeviceX::SetStablePowerState", 40, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandSignature(
    const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature,
    REFIID riid, void** ppvCommandSignature) {
    const HRESULT hr = host_dev_->CreateCommandSignature(
        pDesc, pRootSignature, riid, ppvCommandSignature);
    RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::GetResourceTiling(
    ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
    D3D12_PACKED_MIP_INFO* pPackedMipDesc,
    D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
    UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
    D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) {
    host_dev_->GetResourceTiling(
        pTiledResource, pNumTilesForEntireResource, pPackedMipDesc,
        pStandardTileShapeForNonPackedMips, pNumSubresourceTilings,
        FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips);
    RecordDev("ID3D12DeviceX::GetResourceTiling", 42, S_OK);
}

LUID STDMETHODCALLTYPE DeviceX::GetAdapterLuid() {
    const LUID luid = host_dev_->GetAdapterLuid();
    RecordDev("ID3D12DeviceX::GetAdapterLuid", 43, S_OK);
    return luid;
}

// ID3D12Device1 (slots 44..46)
HRESULT STDMETHODCALLTYPE DeviceX::CreatePipelineLibrary(
    const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** ppPipelineLibrary) {
    const HRESULT hr = host_dev1_
                           ? host_dev1_->CreatePipelineLibrary(
                                 pLibraryBlob, BlobLength, riid, ppPipelineLibrary)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreatePipelineLibrary", 44, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetEventOnMultipleFenceCompletion(
    ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences,
    D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) {
    const HRESULT hr = host_dev1_
                           ? host_dev1_->SetEventOnMultipleFenceCompletion(
                                 ppFences, pFenceValues, NumFences, Flags, hEvent)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::SetEventOnMultipleFenceCompletion", 45, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetResidencyPriority(
    UINT NumObjects, ID3D12Pageable* const* ppObjects,
    const D3D12_RESIDENCY_PRIORITY* pPriorities) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev1_
                           ? host_dev1_->SetResidencyPriority(
                                 NumObjects, unwrapped.data(), pPriorities)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::SetResidencyPriority", 46, hr);
    return hr;
}

// ID3D12Device2 (slot 47)
HRESULT STDMETHODCALLTYPE DeviceX::CreatePipelineState(
    const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    if (!host_dev2_) {
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    if (!pDesc || !pDesc->pPipelineStateSubobjectStream ||
        pDesc->SizeInBytes < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
        const HRESULT hr = host_dev2_->CreatePipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }

    // Pass 1: zero-allocation scan of subobject stream for Xbox shader bytecode & AS presence
    const auto* raw_stream = static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream);
    const size_t total_bytes = static_cast<size_t>(pDesc->SizeInBytes);
    constexpr size_t kBytecodeOffset = offsetof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>, value);

    bool has_xbox_shader = false;
    bool has_as_stage = false;
    uint32_t shader_stage_count = 0;

    size_t pos = 0;
    while (pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= total_bytes) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE sub_type{};
        std::memcpy(&sub_type, raw_stream + pos, sizeof(sub_type));
        const size_t stride = GetSubobjectStrideBytes(sub_type);
        if (stride == 0u || pos + stride > total_bytes) {
            break;
        }
        if (IsShaderSubobjectType(sub_type)) {
            D3D12_SHADER_BYTECODE bc{};
            std::memcpy(&bc, raw_stream + pos + kBytecodeOffset, sizeof(bc));
            if (bc.pShaderBytecode && bc.BytecodeLength > 0) {
                ++shader_stage_count;
                if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                    has_as_stage = true;
                }
                if (openscarlett::IsXboxShaderBytecode(bc.pShaderBytecode, bc.BytecodeLength)) {
                    has_xbox_shader = true;
                }
            }
        }
        pos += stride;
    }

    if (!has_xbox_shader) {
        // Standard host PC stream passthrough with zero overhead
        const HRESULT hr = host_dev2_->CreatePipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }

    // Pass 2: clone stream into pointer-aligned buffer and auto-lift Xbox shader subobjects
    std::vector<uint64_t> cloned_words((total_bytes + sizeof(uint64_t) - 1u) / sizeof(uint64_t), 0ull);
    auto* cloned_bytes = reinterpret_cast<uint8_t*>(cloned_words.data());
    std::memcpy(cloned_bytes, raw_stream, total_bytes);

    std::deque<std::vector<uint8_t>> lifted_blobs;
    openscarlett::ShaderCacheLiftRequest req{};
    req.highest_shader_model = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
    req.mesh_has_payload = has_as_stage;
    req.prefer_dxil_standin = (shader_stage_count > 1u && req.highest_shader_model >= 0x60u);

    pos = 0;
    while (pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= total_bytes) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE sub_type{};
        std::memcpy(&sub_type, cloned_bytes + pos, sizeof(sub_type));
        const size_t stride = GetSubobjectStrideBytes(sub_type);
        if (stride == 0u || pos + stride > total_bytes) {
            break;
        }
        if (IsShaderSubobjectType(sub_type)) {
            D3D12_SHADER_BYTECODE bc{};
            std::memcpy(&bc, cloned_bytes + pos + kBytecodeOffset, sizeof(bc));
            if (bc.pShaderBytecode && bc.BytecodeLength > 0 &&
                openscarlett::IsXboxShaderBytecode(bc.pShaderBytecode, bc.BytecodeLength)) {
                openscarlett::CachedShaderResult lifted{};
                std::string diag;
                auto st = shader_cache_.GetOrLiftXboxShader(
                    bc.pShaderBytecode, bc.BytecodeLength, req, lifted, &diag);
                if (!st.ok()) {
                    RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_INVALIDARG);
                    return E_INVALIDARG;
                }
                lifted_blobs.push_back(std::move(lifted.dxil));
                bc.pShaderBytecode = lifted_blobs.back().data();
                bc.BytecodeLength = lifted_blobs.back().size();
                std::memcpy(cloned_bytes + pos + kBytecodeOffset, &bc, sizeof(bc));
            }
        }
        pos += stride;
    }

    D3D12_PIPELINE_STATE_STREAM_DESC lifted_stream{};
    lifted_stream.SizeInBytes = pDesc->SizeInBytes;
    lifted_stream.pPipelineStateSubobjectStream = cloned_bytes;

    const HRESULT hr = host_dev2_->CreatePipelineState(&lifted_stream, riid, ppPipelineState);
    RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
    return hr;
}

// ID3D12Device3 (slots 48..50)
HRESULT STDMETHODCALLTYPE DeviceX::OpenExistingHeapFromAddress(
    const void* pAddress, REFIID riid, void** ppvHeap) {
    const HRESULT hr = host_dev3_
                           ? host_dev3_->OpenExistingHeapFromAddress(pAddress, riid, ppvHeap)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::OpenExistingHeapFromFileMapping(
    HANDLE hFileMapping, REFIID riid, void** ppvHeap) {
    const HRESULT hr = host_dev3_
                           ? host_dev3_->OpenExistingHeapFromFileMapping(hFileMapping, riid, ppvHeap)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::EnqueueMakeResident(
    D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects,
    ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev3_
                           ? host_dev3_->EnqueueMakeResident(
                                 Flags, NumObjects, unwrapped.data(),
                                 pFenceToSignal, FenceValueToSignal)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::EnqueueMakeResident", 50, hr);
    return hr;
}

// ID3D12Device4 (slots 51..56)
HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandList1(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
    REFIID riid, void** ppCommandList) {
    if (!ppCommandList) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, E_POINTER);
        return E_POINTER;
    }
    *ppCommandList = nullptr;
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    ID3D12GraphicsCommandList* host_cl = nullptr;
    HRESULT hr = host_dev4_->CreateCommandList1(nodeMask, type, flags, IID_PPV_ARGS(&host_cl));
    if (FAILED(hr) || !host_cl) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, hr);
        return hr;
    }
    auto* wrapper = new (std::nothrow) CommandListX(this, host_cl);
    host_cl->Release();
    if (!wrapper) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = wrapper->QueryInterface(riid, ppCommandList);
    wrapper->Release();
    RecordDev("ID3D12DeviceX::CreateCommandList1", 51, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateProtectedResourceSession(
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid, void** ppSession) {
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateProtectedResourceSession(pDesc, riid, ppSession)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateProtectedResourceSession", 52, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommittedResource1(
    const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riidResource, void** ppvResource) {
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateCommittedResource1(
                                 pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                 pOptimizedClearValue, pProtectedSession, riidResource, ppvResource)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateHeap1(
    const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvHeap) {
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateHeap1(pDesc, pProtectedSession, riid, ppvHeap)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateReservedResource1(
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvResource) {
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateReservedResource1(
                                 pDesc, InitialState, pOptimizedClearValue,
                                 pProtectedSession, riid, ppvResource)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
    return hr;
}

D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo1(
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    if (host_dev4_) {
        info = host_dev4_->GetResourceAllocationInfo1(
            visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, S_OK);
    } else {
        info = host_dev_->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, E_NOINTERFACE);
    }
    return info;
}

// ID3D12Device5 (slots 57..64)
HRESULT STDMETHODCALLTYPE DeviceX::CreateLifetimeTracker(
    ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) {
    const HRESULT hr = host_dev5_
                           ? host_dev5_->CreateLifetimeTracker(pOwner, riid, ppvTracker)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateLifetimeTracker", 57, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::RemoveDevice() {
    if (host_dev5_) host_dev5_->RemoveDevice();
    RecordDev("ID3D12DeviceX::RemoveDevice", 58, host_dev5_ ? S_OK : E_NOINTERFACE);
}

HRESULT STDMETHODCALLTYPE DeviceX::EnumerateMetaCommands(
    UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) {
    const HRESULT hr = host_dev5_
                           ? host_dev5_->EnumerateMetaCommands(pNumMetaCommands, pDescs)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::EnumerateMetaCommands", 59, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::EnumerateMetaCommandParameters(
    REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage,
    UINT* pTotalStructureSizeInBytes, UINT* pParameterCount,
    D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) {
    const HRESULT hr = host_dev5_
                           ? host_dev5_->EnumerateMetaCommandParameters(
                                 CommandId, Stage, pTotalStructureSizeInBytes,
                                 pParameterCount, pParameterDescs)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::EnumerateMetaCommandParameters", 60, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateMetaCommand(
    REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
    SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** ppMetaCommand) {
    const HRESULT hr = host_dev5_
                           ? host_dev5_->CreateMetaCommand(
                                 CommandId, NodeMask, pCreationParametersData,
                                 CreationParametersDataSizeInBytes, riid, ppMetaCommand)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateMetaCommand", 61, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateStateObject(
    const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject) {
    const HRESULT hr = host_dev5_
                           ? host_dev5_->CreateStateObject(pDesc, riid, ppStateObject)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::GetRaytracingAccelerationStructurePrebuildInfo(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) {
    if (host_dev5_) {
        host_dev5_->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
    }
    RecordDev("ID3D12DeviceX::GetRaytracingAccelerationStructurePrebuildInfo",
              63, host_dev5_ ? S_OK : E_NOINTERFACE);
}

D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE DeviceX::CheckDriverMatchingIdentifier(
    D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
    const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) {
    const auto status = host_dev5_
                            ? host_dev5_->CheckDriverMatchingIdentifier(
                                  SerializedDataType, pIdentifierToCheck)
                            : D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
    RecordDev("ID3D12DeviceX::CheckDriverMatchingIdentifier",
              64, host_dev5_ ? S_OK : E_NOINTERFACE);
    return status;
}

// ID3D12Device6 (slot 65)
HRESULT STDMETHODCALLTYPE DeviceX::SetBackgroundProcessingMode(
    D3D12_BACKGROUND_PROCESSING_MODE Mode,
    D3D12_MEASUREMENTS_ACTION MeasurementsAction,
    HANDLE hEventToSignalUponCompletion,
    BOOL* pbFurtherMeasurementsDesired) {
    const HRESULT hr = host_dev6_
                           ? host_dev6_->SetBackgroundProcessingMode(
                                 Mode, MeasurementsAction, hEventToSignalUponCompletion,
                                 pbFurtherMeasurementsDesired)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::SetBackgroundProcessingMode", 65, hr);
    return hr;
}

// ID3D12DeviceX Xbox extensions (slots 66..72)
HRESULT STDMETHODCALLTYPE DeviceX::SetFrameIntervalX(
    IDXGIOutput* pOutput, uint32_t PeriodMicroseconds,
    uint32_t MaxFrameLatency, uint32_t Flags) {
    (void)pOutput;
    (void)Flags;
    frame_interval_us_.store(PeriodMicroseconds, std::memory_order_relaxed);
    max_frame_latency_.store(MaxFrameLatency == 0 ? 1u : MaxFrameLatency, std::memory_order_relaxed);
    RecordDev("ID3D12DeviceX::SetFrameIntervalX", 66, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::ScheduleFrameEventX(
    D3D12XBOX_FRAME_EVENT Event, uint64_t DelayMicroseconds,
    HANDLE hEvent, uint32_t Flags) {
    (void)Event;
    (void)DelayMicroseconds;
    (void)Flags;
    if (hEvent) {
        SetEvent(hEvent);
    }
    RecordDev("ID3D12DeviceX::ScheduleFrameEventX", 67, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::WaitFrameEventX(
    D3D12XBOX_FRAME_EVENT Event, uint32_t TimeoutMilliseconds,
    void* pReserved, D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) {
    (void)Event;
    (void)TimeoutMilliseconds;
    (void)pReserved;
    const uint64_t token = next_frame_token_.fetch_add(1ull, std::memory_order_relaxed);
    if (pOutToken) {
        *pOutToken = token;
    }
    RecordDev("ID3D12DeviceX::WaitFrameEventX", 68, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreatePlacedResourceX(
    D3D12_GPU_VIRTUAL_ADDRESS VirtualAddress, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource) {
    (void)VirtualAddress;
    if (!pDesc || !ppvResource) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 69, E_INVALIDARG);
        return E_INVALIDARG;
    }
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    const HRESULT hr = host_dev_->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, pDesc, InitialState,
        pOptimizedClearValue, riid, ppvResource);
    RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 69, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SuspendX(uint32_t Flags) {
    (void)Flags;
    suspended_.store(true, std::memory_order_release);
    RecordDev("ID3D12DeviceX::SuspendX", 70, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::ResumeX() {
    suspended_.store(false, std::memory_order_release);
    RecordDev("ID3D12DeviceX::ResumeX", 71, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetGpuHardwareInfoX(D3D12XBOX_GPU_HARDWARE_INFO* pOutInfo) {
    if (!pOutInfo) {
        RecordDev("ID3D12DeviceX::GetGpuHardwareInfoX", 72, E_POINTER);
        return E_POINTER;
    }
    std::memset(pOutInfo, 0, sizeof(*pOutInfo));
    pOutInfo->VendorId = adapter_info_.vendor_id;
    pOutInfo->DeviceId = adapter_info_.device_id;
    pOutInfo->DedicatedVideoMemoryBytes = adapter_info_.dedicated_video_memory_bytes;
    pOutInfo->MeshShaderTier = adapter_info_.mesh_shader_tier;
    pOutInfo->RaytracingTier = adapter_info_.raytracing_tier;
    pOutInfo->HighestShaderModel = adapter_info_.highest_shader_model;
    pOutInfo->WaveLaneCountMin = adapter_info_.wave_lane_min;
    pOutInfo->WaveLaneCountMax = adapter_info_.wave_lane_max;
    pOutInfo->IsWarp = adapter_info_.is_warp ? 1u : 0u;
    const size_t copy_len =
        (adapter_info_.description.size() < sizeof(pOutInfo->AdapterName) - 1u)
            ? adapter_info_.description.size()
            : (sizeof(pOutInfo->AdapterName) - 1u);
    if (copy_len > 0) {
        std::memcpy(pOutInfo->AdapterName, adapter_info_.description.data(), copy_len);
    }
    pOutInfo->AdapterName[copy_len] = '\0';
    RecordDev("ID3D12DeviceX::GetGpuHardwareInfoX", 72, S_OK);
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
