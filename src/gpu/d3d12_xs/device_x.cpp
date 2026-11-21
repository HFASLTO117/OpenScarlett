#include "device_x.h"

#include <algorithm>
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
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL) {
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

DXGI_FORMAT MapTypelessFormatForSrv(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R8_TYPELESS:
            return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_TYPELESS:
            return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_BC1_TYPELESS:
            return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS:
            return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS:
            return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS:
            return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS:
            return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS:
            return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS:
            return DXGI_FORMAT_BC7_UNORM;
        default:
            return fmt;
    }
}

bool IsReadableGuestVaRange(uint64_t va, uint64_t byte_size) noexcept {
    if (va == 0ull || byte_size == 0ull) return false;
    if (va > UINT64_MAX - byte_size) return false;
    uint64_t cur = va;
    const uint64_t end = va + byte_size;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(cur)),
                         &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) return false;
        constexpr DWORD kReadableMask =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kReadableMask) == 0u ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
            return false;
        }
        const uint64_t region_end =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
            static_cast<uint64_t>(mbi.RegionSize);
        if (region_end <= cur) return false;
        cur = region_end;
    }
    return true;
}

uint64_t AlignUp64(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
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
    ReleaseUnifiedMemoryObjects();
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
    if (IsEqualIID(riid, IID_IOpenScarlettUnifiedMemory)) {
        *ppvObject = static_cast<IOpenScarlettUnifiedMemory*>(this);
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
    // ScarlettX #46: Preserve array slices on default (nullptr desc) texture SRVs.
    // Some host drivers / translation paths collapse nullptr SRV descriptors on Texture2D arrays
    // into a single-slice Texture2D view; build an explicit array SRV descriptor instead.
    if (pDesc == nullptr && pResource != nullptr) {
        const D3D12_RESOURCE_DESC res_desc = pResource->GetDesc();
        if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            res_desc.DepthOrArraySize > 1u) {
            D3D12_SHADER_RESOURCE_VIEW_DESC explicit_desc{};
            explicit_desc.Format = MapTypelessFormatForSrv(res_desc.Format);
            explicit_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (res_desc.SampleDesc.Count > 1u) {
                explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                explicit_desc.Texture2DMSArray.FirstArraySlice = 0u;
                explicit_desc.Texture2DMSArray.ArraySize = res_desc.DepthOrArraySize;
            } else {
                explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                explicit_desc.Texture2DArray.MostDetailedMip = 0u;
                explicit_desc.Texture2DArray.MipLevels =
                    (res_desc.MipLevels > 0u) ? res_desc.MipLevels : static_cast<UINT>(-1);
                explicit_desc.Texture2DArray.FirstArraySlice = 0u;
                explicit_desc.Texture2DArray.ArraySize = res_desc.DepthOrArraySize;
                explicit_desc.Texture2DArray.PlaneSlice = 0u;
                explicit_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            }
            host_dev_->CreateShaderResourceView(pResource, &explicit_desc, DestDescriptor);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]", 18, S_OK);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
            return;
        }
        if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
            res_desc.DepthOrArraySize > 1u) {
            D3D12_SHADER_RESOURCE_VIEW_DESC explicit_desc{};
            explicit_desc.Format = MapTypelessFormatForSrv(res_desc.Format);
            explicit_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            explicit_desc.Texture1DArray.MostDetailedMip = 0u;
            explicit_desc.Texture1DArray.MipLevels =
                (res_desc.MipLevels > 0u) ? res_desc.MipLevels : static_cast<UINT>(-1);
            explicit_desc.Texture1DArray.FirstArraySlice = 0u;
            explicit_desc.Texture1DArray.ArraySize = res_desc.DepthOrArraySize;
            explicit_desc.Texture1DArray.ResourceMinLODClamp = 0.0f;
            host_dev_->CreateShaderResourceView(pResource, &explicit_desc, DestDescriptor);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]", 18, S_OK);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
            return;
        }
    }
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

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo(
    D3D12_RESOURCE_ALLOCATION_INFO* pRet,
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) {
    D3D12_RESOURCE_ALLOCATION_INFO* info =
        host_dev_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, pResourceDescs);
    RecordDev("ID3D12DeviceX::GetResourceAllocationInfo", 25, S_OK);
    return info;
}

D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE DeviceX::GetCustomHeapProperties(
    D3D12_HEAP_PROPERTIES* pRet,
    UINT nodeMask, D3D12_HEAP_TYPE heapType) {
    D3D12_HEAP_PROPERTIES* props = host_dev_->GetCustomHeapProperties(pRet, nodeMask, heapType);
    RecordDev("ID3D12DeviceX::GetCustomHeapProperties", 26, S_OK);
    return props;
}
#else
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
#endif

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
    if (SUCCEEDED(hr) && pHeap && desc_ptr && ppvResource && *ppvResource) {
        uint64_t heap_base_va = 0ull;
        {
            std::lock_guard<std::mutex> lock(unified_mutex_);
            auto it = address_heaps_.find(pHeap);
            if (it != address_heaps_.end()) {
                heap_base_va = it->second;
            }
        }
        if (heap_base_va != 0ull) {
            ID3D12Resource* res = nullptr;
            if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res))) && res) {
                AutoRegisterUnifiedResource(res, heap_base_va + HeapOffset, *desc_ptr, InitialState);
                res->Release();
            }
        }
    }
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

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
LUID* STDMETHODCALLTYPE DeviceX::GetAdapterLuid(LUID* pRet) {
    LUID* luid = host_dev_->GetAdapterLuid(pRet);
    RecordDev("ID3D12DeviceX::GetAdapterLuid", 43, S_OK);
    return luid;
}
#else
LUID STDMETHODCALLTYPE DeviceX::GetAdapterLuid() {
    const LUID luid = host_dev_->GetAdapterLuid();
    RecordDev("ID3D12DeviceX::GetAdapterLuid", 43, S_OK);
    return luid;
}
#endif

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
    if (SUCCEEDED(hr) && pAddress && ppvHeap && *ppvHeap) {
        ID3D12Heap* heap = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppvHeap)->QueryInterface(IID_PPV_ARGS(&heap))) && heap) {
            std::lock_guard<std::mutex> lock(unified_mutex_);
            address_heaps_[heap] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pAddress));
            heap->Release();
        }
    }
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
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        desc_ptr = &clean_desc;
    }
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateCommittedResource1(
                                 pHeapProperties, SanitizeXboxHeapFlags(HeapFlags), desc_ptr,
                                 InitialResourceState, pOptimizedClearValue,
                                 pProtectedSession, riidResource, ppvResource)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateHeap1(
    const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvHeap) {
    D3D12_HEAP_DESC clean_heap{};
    const D3D12_HEAP_DESC* heap_ptr = pDesc;
    if (pDesc) {
        clean_heap = *pDesc;
        clean_heap.Flags = SanitizeXboxHeapFlags(pDesc->Flags);
        heap_ptr = &clean_heap;
    }
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateHeap1(heap_ptr, pProtectedSession, riid, ppvHeap)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateReservedResource1(
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvResource) {
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        desc_ptr = &clean_desc;
    }
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateReservedResource1(
                                 desc_ptr, InitialState, pOptimizedClearValue,
                                 pProtectedSession, riid, ppvResource)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
    return hr;
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo1(
    D3D12_RESOURCE_ALLOCATION_INFO* pRet,
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    D3D12_RESOURCE_ALLOCATION_INFO* info = nullptr;
    if (host_dev4_) {
        info = host_dev4_->GetResourceAllocationInfo1(
            pRet, visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, S_OK);
    } else {
        info = host_dev_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, pResourceDescs);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, E_NOINTERFACE);
    }
    return info;
}
#else
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
#endif

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
    if (!pDesc || !ppvResource) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 69, E_INVALIDARG);
        return E_INVALIDARG;
    }
    const D3D12_RESOURCE_DESC clean_desc = SanitizeXboxResourceDesc(*pDesc);
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    const HRESULT hr = host_dev_->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &clean_desc, InitialState,
        pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && VirtualAddress != 0ull && *ppvResource) {
        ID3D12Resource* res = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res))) && res) {
            AutoRegisterUnifiedResource(res, VirtualAddress, clean_desc, InitialState);
            res->Release();
        }
    }
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

// ---------------------------------------------------------------------------
// Title Unified-Memory Texture Remap Tracking (ivall/xcl fd74662/bdae5a4)
// ---------------------------------------------------------------------------
void DeviceX::NotifyResourceStateTransition(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES new_state) noexcept {
    if (!resource || !HasRegisteredUnifiedTextures()) return;
    std::lock_guard<std::mutex> lock(unified_mutex_);
    auto it = unified_textures_.find(resource);
    if (it != unified_textures_.end()) {
        it->second.current_state = new_state;
    }
}

void DeviceX::AutoRegisterUnifiedResource(
    ID3D12Resource* resource,
    uint64_t guest_va,
    const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES initial_state) {
    if (!resource || guest_va == 0ull) return;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return;
    }

    uint64_t linear_size = 0ull;
    uint32_t row_pitch = 0u;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        linear_size = desc.Width;
        row_pitch = static_cast<uint32_t>(std::min<uint64_t>(desc.Width, UINT32_MAX));
    } else {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT num_rows = 0u;
        UINT64 row_bytes = 0ull;
        UINT64 total_bytes = 0ull;
        host_dev_->GetCopyableFootprints(&desc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
        row_pitch = static_cast<uint32_t>(row_bytes);
        const uint32_t slices = (desc.DepthOrArraySize > 0u) ? desc.DepthOrArraySize : 1u;
        linear_size = row_bytes * static_cast<uint64_t>(num_rows) * static_cast<uint64_t>(slices);
    }

    D3D12XBOX_UNIFIED_TEXTURE_DESC udesc{};
    udesc.Resource = resource;
    udesc.GuestVirtualAddress = guest_va;
    udesc.ByteSize = linear_size;
    udesc.RowPitchBytes = row_pitch;
    udesc.ResourceState = initial_state;
    RegisterUnifiedTexture(&udesc);
}

HRESULT DeviceX::EnsureUnifiedUploadObjectsLocked() {
    if (unified_alloc_ && unified_cmd_ && unified_fence_ && unified_fence_event_) {
        return S_OK;
    }
    if (!unified_alloc_) {
        HRESULT hr = host_dev_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&unified_alloc_));
        if (FAILED(hr)) return hr;
    }
    if (!unified_cmd_) {
        HRESULT hr = host_dev_->CreateCommandList(
            0u, D3D12_COMMAND_LIST_TYPE_DIRECT, unified_alloc_, nullptr,
            IID_PPV_ARGS(&unified_cmd_));
        if (FAILED(hr)) return hr;
        unified_cmd_->Close();
    }
    if (!unified_fence_) {
        HRESULT hr = host_dev_->CreateFence(
            0ull, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&unified_fence_));
        if (FAILED(hr)) return hr;
    }
    if (!unified_fence_event_) {
        unified_fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!unified_fence_event_) return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT DeviceX::EnsureUnifiedUploadBufferLocked(uint64_t required_bytes) {
    if (required_bytes == 0ull) return S_OK;
    if (unified_upload_buf_ && unified_upload_capacity_ >= required_bytes) {
        return S_OK;
    }
    if (unified_upload_buf_) {
        unified_upload_buf_->Release();
        unified_upload_buf_ = nullptr;
        unified_upload_capacity_ = 0ull;
    }

    const uint64_t alloc_bytes = AlignUp64(std::max<uint64_t>(required_bytes, 65536ull), 4096ull);
    D3D12_HEAP_PROPERTIES up_heap{};
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = alloc_bytes;
    up_desc.Height = 1u;
    up_desc.DepthOrArraySize = 1u;
    up_desc.MipLevels = 1u;
    up_desc.Format = DXGI_FORMAT_UNKNOWN;
    up_desc.SampleDesc.Count = 1u;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = host_dev_->CreateCommittedResource(
        &up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&unified_upload_buf_));
    if (SUCCEEDED(hr)) {
        unified_upload_capacity_ = alloc_bytes;
    }
    return hr;
}

void DeviceX::ReleaseUnifiedMemoryObjects() {
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (auto& [res, mapping] : unified_textures_) {
            if (mapping.resource) {
                mapping.resource->Release();
            }
        }
        unified_textures_.clear();
        address_heaps_.clear();
        registered_unified_count_.store(0u, std::memory_order_release);
        dirty_unified_count_.store(0u, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(unified_upload_mutex_);
        if (unified_upload_buf_) { unified_upload_buf_->Release(); unified_upload_buf_ = nullptr; }
        if (unified_cmd_) { unified_cmd_->Release(); unified_cmd_ = nullptr; }
        if (unified_alloc_) { unified_alloc_->Release(); unified_alloc_ = nullptr; }
        if (unified_fence_) { unified_fence_->Release(); unified_fence_ = nullptr; }
        if (unified_fence_event_) { CloseHandle(unified_fence_event_); unified_fence_event_ = nullptr; }
        unified_upload_capacity_ = 0ull;
    }
}

HRESULT STDMETHODCALLTYPE DeviceX::RegisterUnifiedTexture(
    const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc) {
    if (!desc || !desc->Resource || desc->GuestVirtualAddress == 0ull) {
        RecordDev("ID3D12DeviceX::RegisterUnifiedTexture", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }

    const D3D12_RESOURCE_DESC rdesc = desc->Resource->GetDesc();
    uint64_t byte_size = desc->ByteSize;
    uint32_t row_pitch = desc->RowPitchBytes;
    if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (byte_size == 0ull) byte_size = rdesc.Width;
        if (row_pitch == 0u) row_pitch = static_cast<uint32_t>(std::min<uint64_t>(byte_size, UINT32_MAX));
    } else if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT num_rows = 0u;
        UINT64 row_bytes = 0ull;
        UINT64 total_bytes = 0ull;
        host_dev_->GetCopyableFootprints(&rdesc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
        if (row_pitch == 0u) row_pitch = static_cast<uint32_t>(row_bytes);
        if (byte_size == 0ull) {
            const uint32_t slices = (rdesc.DepthOrArraySize > 0u) ? rdesc.DepthOrArraySize : 1u;
            byte_size = static_cast<uint64_t>(row_pitch) *
                        static_cast<uint64_t>(num_rows) *
                        static_cast<uint64_t>(slices);
        }
    }

    const bool readable = IsReadableGuestVaRange(desc->GuestVirtualAddress, byte_size);

    std::lock_guard<std::mutex> lock(unified_mutex_);
    auto it = unified_textures_.find(desc->Resource);
    if (it != unified_textures_.end()) {
        if (it->second.dirty) {
            dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
        }
        it->second.guest_va = desc->GuestVirtualAddress;
        it->second.byte_size = byte_size;
        it->second.row_pitch_bytes = row_pitch;
        it->second.current_state = desc->ResourceState;
        it->second.cpu_readable = readable;
        it->second.dirty = readable;
        ++it->second.page_generation;
        if (it->second.dirty) {
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        }
    } else {
        UnifiedTextureMapping m{};
        m.resource = desc->Resource;
        m.resource->AddRef();
        m.guest_va = desc->GuestVirtualAddress;
        m.byte_size = byte_size;
        m.width = static_cast<uint32_t>(rdesc.Width);
        m.height = rdesc.Height;
        m.depth_or_array_size = (rdesc.DepthOrArraySize > 0u) ? rdesc.DepthOrArraySize : 1u;
        m.mip_levels = (rdesc.MipLevels > 0u) ? rdesc.MipLevels : 1u;
        m.format = rdesc.Format;
        m.dimension = rdesc.Dimension;
        m.current_state = desc->ResourceState;
        m.row_pitch_bytes = row_pitch;
        m.page_generation = 1ull;
        m.uploaded_generation = 0ull;
        m.cpu_readable = readable;
        m.dirty = readable;
        unified_textures_.emplace(desc->Resource, m);
        registered_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        if (m.dirty) {
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        }
    }

    RecordDev("ID3D12DeviceX::RegisterUnifiedTexture", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::UnregisterUnifiedTexture(ID3D12Resource* resource) {
    if (!resource) {
        RecordDev("ID3D12DeviceX::UnregisterUnifiedTexture", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    ID3D12Resource* to_release = nullptr;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        auto it = unified_textures_.find(resource);
        if (it != unified_textures_.end()) {
            if (it->second.dirty) {
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
            }
            to_release = it->second.resource;
            unified_textures_.erase(it);
            registered_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
        }
    }
    if (to_release) {
        to_release->Release();
    }
    RecordDev("ID3D12DeviceX::UnregisterUnifiedTexture", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::NotifyVirtualRemap(
    uint64_t guest_va_base,
    uint64_t byte_size,
    uint64_t new_backing_va,
    uint32_t* out_flagged_count) {
    if (out_flagged_count) *out_flagged_count = 0u;
    if (guest_va_base == 0ull || byte_size == 0ull) {
        RecordDev("ID3D12DeviceX::NotifyVirtualRemap", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    const uint64_t range_end =
        (guest_va_base > UINT64_MAX - byte_size) ? UINT64_MAX : (guest_va_base + byte_size);

    uint32_t flagged = 0u;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        ++total_remap_notifications_;
        for (auto& [res, m] : unified_textures_) {
            const uint64_t m_end =
                (m.guest_va > UINT64_MAX - m.byte_size) ? UINT64_MAX : (m.guest_va + m.byte_size);
            const bool overlaps = (m.guest_va < range_end) && (guest_va_base < m_end);
            if (!overlaps) continue;

            if (new_backing_va != 0ull) {
                const uint64_t rel_offset =
                    (m.guest_va >= guest_va_base) ? (m.guest_va - guest_va_base) : 0ull;
                m.guest_va = new_backing_va + rel_offset;
            }
            m.cpu_readable = IsReadableGuestVaRange(m.guest_va, m.byte_size);
            ++m.page_generation;
            if (!m.dirty && m.cpu_readable) {
                m.dirty = true;
                dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
            }
            ++flagged;
        }
    }

    if (out_flagged_count) *out_flagged_count = flagged;
    RecordDev("ID3D12DeviceX::NotifyVirtualRemap", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::FlushDirtyUnifiedTextures(
    ID3D12CommandQueue* queue,
    uint32_t* out_flushed_count) {
    if (out_flushed_count) *out_flushed_count = 0u;
    if (!queue) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (!HasDirtyUnifiedTextures()) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, S_OK);
        return S_OK;
    }

    ID3D12CommandQueue* host_queue = CommandQueueX::UnwrapHostQueue(queue);
    if (!host_queue) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }

    struct PendingUpload {
        UnifiedTextureMapping mapping;
        D3D12_RESOURCE_DESC res_desc{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        std::vector<UINT> num_rows;
        std::vector<UINT64> row_sizes;
        uint64_t buffer_upload_offset = 0ull;
        uint64_t copy_bytes = 0ull;
    };

    std::vector<PendingUpload> pending;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (auto& [res, m] : unified_textures_) {
            if (!m.dirty || !m.cpu_readable || !m.resource) continue;
            PendingUpload item{};
            item.mapping = m;
            item.mapping.resource->AddRef();
            item.res_desc = m.resource->GetDesc();
            pending.push_back(std::move(item));
        }
    }

    if (pending.empty()) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, S_OK);
        return S_OK;
    }

    // Compute staging buffer layout (512-byte aligned per subresource).
    uint64_t staging_cursor = 0ull;
    for (auto& item : pending) {
        staging_cursor = AlignUp64(staging_cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            item.buffer_upload_offset = staging_cursor;
            item.copy_bytes = std::min<uint64_t>(item.mapping.byte_size, item.res_desc.Width);
            staging_cursor += item.copy_bytes;
        } else if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            const uint32_t slices =
                (item.res_desc.DepthOrArraySize > 0u) ? item.res_desc.DepthOrArraySize : 1u;
            const uint32_t mips = (item.res_desc.MipLevels > 0u) ? item.res_desc.MipLevels : 1u;
            item.footprints.resize(slices);
            item.num_rows.resize(slices, 0u);
            item.row_sizes.resize(slices, 0ull);
            uint64_t total_copy = 0ull;
            for (uint32_t s = 0u; s < slices; ++s) {
                staging_cursor = AlignUp64(staging_cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                UINT64 sub_total = 0ull;
                const UINT sub_idx = s * mips;  // mip 0 of each slice
                host_dev_->GetCopyableFootprints(
                    &item.res_desc, sub_idx, 1u, staging_cursor,
                    &item.footprints[s], &item.num_rows[s], &item.row_sizes[s], &sub_total);
                staging_cursor += sub_total;
                total_copy += item.row_sizes[s] * static_cast<uint64_t>(item.num_rows[s]);
            }
            item.copy_bytes = total_copy;
        }
    }

    auto release_pending = [&]() {
        for (auto& item : pending) {
            if (item.mapping.resource) {
                item.mapping.resource->Release();
                item.mapping.resource = nullptr;
            }
        }
    };

    std::lock_guard<std::mutex> upload_lock(unified_upload_mutex_);
    HRESULT hr = EnsureUnifiedUploadObjectsLocked();
    if (SUCCEEDED(hr)) {
        hr = EnsureUnifiedUploadBufferLocked(staging_cursor);
    }
    if (FAILED(hr)) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
        return hr;
    }

    void* mapped_ptr = nullptr;
    D3D12_RANGE read_none{0u, 0u};
    hr = unified_upload_buf_->Map(0u, &read_none, &mapped_ptr);
    if (FAILED(hr) || !mapped_ptr) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, FAILED(hr) ? hr : E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }

    auto* dst_base = static_cast<uint8_t*>(mapped_ptr);
    std::vector<size_t> uploaded_indices;
    uploaded_indices.reserve(pending.size());

    for (size_t idx = 0; idx < pending.size(); ++idx) {
        const auto& item = pending[idx];
        if (item.copy_bytes == 0ull ||
            !IsReadableGuestVaRange(item.mapping.guest_va, item.mapping.byte_size)) {
            continue;
        }
        const auto* src_base =
            reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(item.mapping.guest_va));
        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            std::memcpy(dst_base + item.buffer_upload_offset, src_base,
                        static_cast<size_t>(item.copy_bytes));
            uploaded_indices.push_back(idx);
        } else if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            const size_t slices = item.footprints.size();
            uint64_t src_slice_offset = 0ull;
            for (size_t s = 0; s < slices; ++s) {
                const uint64_t row_bytes = item.row_sizes[s];
                const uint64_t src_row_pitch =
                    (item.mapping.row_pitch_bytes >= row_bytes)
                        ? static_cast<uint64_t>(item.mapping.row_pitch_bytes)
                        : row_bytes;
                const uint64_t dst_row_pitch = item.footprints[s].Footprint.RowPitch;
                uint8_t* dst_slice = dst_base + item.footprints[s].Offset;
                for (UINT y = 0u; y < item.num_rows[s]; ++y) {
                    const uint64_t src_off = src_slice_offset + static_cast<uint64_t>(y) * src_row_pitch;
                    if (src_off + row_bytes <= item.mapping.byte_size) {
                        std::memcpy(
                            dst_slice + static_cast<size_t>(y) * static_cast<size_t>(dst_row_pitch),
                            src_base + src_off,
                            static_cast<size_t>(row_bytes));
                    }
                }
                src_slice_offset += src_row_pitch * static_cast<uint64_t>(item.num_rows[s]);
            }
            uploaded_indices.push_back(idx);
        }
    }

    D3D12_RANGE written_range{0u, static_cast<SIZE_T>(staging_cursor)};
    unified_upload_buf_->Unmap(0u, &written_range);

    if (uploaded_indices.empty()) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, S_OK);
        return S_OK;
    }

    hr = unified_alloc_->Reset();
    if (SUCCEEDED(hr)) {
        hr = unified_cmd_->Reset(unified_alloc_, nullptr);
    }
    if (FAILED(hr)) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
        return hr;
    }

    uint64_t bytes_uploaded = 0ull;
    for (size_t idx : uploaded_indices) {
        const auto& item = pending[idx];
        const D3D12_RESOURCE_STATES st = item.mapping.current_state;
        if (st != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = item.mapping.resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = st;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            unified_cmd_->ResourceBarrier(1u, &b);
        }

        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            unified_cmd_->CopyBufferRegion(
                item.mapping.resource, 0ull,
                unified_upload_buf_, item.buffer_upload_offset,
                item.copy_bytes);
        } else if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            const uint32_t mips = (item.res_desc.MipLevels > 0u) ? item.res_desc.MipLevels : 1u;
            for (size_t s = 0; s < item.footprints.size(); ++s) {
                D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                dst_loc.pResource = item.mapping.resource;
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = static_cast<UINT>(s) * mips;

                D3D12_TEXTURE_COPY_LOCATION src_loc{};
                src_loc.pResource = unified_upload_buf_;
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = item.footprints[s];

                unified_cmd_->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
            }
        }

        if (st != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = item.mapping.resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = st;
            unified_cmd_->ResourceBarrier(1u, &b);
        }
        bytes_uploaded += item.copy_bytes;
    }

    hr = unified_cmd_->Close();
    if (FAILED(hr)) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
        return hr;
    }

    ID3D12CommandList* cmd_lists[] = {unified_cmd_};
    host_queue->ExecuteCommandLists(1u, cmd_lists);

    const uint64_t fence_target = ++unified_fence_val_;
    hr = host_queue->Signal(unified_fence_, fence_target);
    if (SUCCEEDED(hr) && unified_fence_->GetCompletedValue() < fence_target) {
        hr = unified_fence_->SetEventOnCompletion(fence_target, unified_fence_event_);
        if (SUCCEEDED(hr)) {
            const DWORD wr = WaitForSingleObject(unified_fence_event_, 15000u);
            if (wr != WAIT_OBJECT_0) {
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
    }

    uint32_t flushed_count = 0u;
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (size_t idx : uploaded_indices) {
            const auto& item = pending[idx];
            auto it = unified_textures_.find(item.mapping.resource);
            if (it != unified_textures_.end() &&
                it->second.dirty &&
                it->second.page_generation == item.mapping.page_generation) {
                it->second.dirty = false;
                it->second.uploaded_generation = item.mapping.page_generation;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                ++flushed_count;
            }
        }
        ++total_unified_flushes_;
        total_unified_bytes_uploaded_ += bytes_uploaded;
    }

    release_pending();
    if (out_flushed_count) *out_flushed_count = flushed_count;
    RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetUnifiedTextureStats(
    D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats) {
    if (!out_stats) {
        RecordDev("ID3D12DeviceX::GetUnifiedTextureStats", -1, E_POINTER);
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(unified_mutex_);
    out_stats->RegisteredCount = static_cast<uint32_t>(unified_textures_.size());
    out_stats->DirtyCount = dirty_unified_count_.load(std::memory_order_acquire);
    out_stats->TotalRemapNotifications = total_remap_notifications_;
    out_stats->TotalFlushes = total_unified_flushes_;
    out_stats->TotalBytesUploaded = total_unified_bytes_uploaded_;
    RecordDev("ID3D12DeviceX::GetUnifiedTextureStats", -1, S_OK);
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
