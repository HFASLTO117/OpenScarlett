#pragma once
#include <d3d12_xs.h>

// Clean-room MIT D3DX12 helper subset for untouched Xbox-GDK-Samples.

struct CD3DX12_DEFAULT {};
extern const DECLSPEC_SELECTANY CD3DX12_DEFAULT D3D12_DEFAULT;

struct CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES {
    explicit CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type) noexcept {
        Type = type;
        CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        CreationNodeMask = 1;
        VisibleNodeMask = 1;
    }
};

struct CD3DX12_RESOURCE_DESC : public D3D12_RESOURCE_DESC {
    CD3DX12_RESOURCE_DESC() = default;
    CD3DX12_RESOURCE_DESC(const D3D12_RESOURCE_DESC& o) noexcept : D3D12_RESOURCE_DESC(o) {}

    static CD3DX12_RESOURCE_DESC Tex2D(
        DXGI_FORMAT fmt, UINT64 w, UINT h, UINT16 arr = 1, UINT16 mips = 0,
        UINT sampleCount = 1, UINT sampleQuality = 0,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        UINT64 alignment = 0) noexcept {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Alignment = alignment;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = arr;
        d.MipLevels = mips;
        d.Format = fmt;
        d.SampleDesc.Count = sampleCount;
        d.SampleDesc.Quality = sampleQuality;
        d.Layout = layout;
        d.Flags = flags;
        return CD3DX12_RESOURCE_DESC(d);
    }

    static CD3DX12_RESOURCE_DESC Buffer(
        UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        UINT64 alignment = 0) noexcept {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Alignment = alignment;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return CD3DX12_RESOURCE_DESC(d);
    }
};

struct CD3DX12_CLEAR_VALUE : public D3D12_CLEAR_VALUE {
    CD3DX12_CLEAR_VALUE(DXGI_FORMAT fmt, const FLOAT color[4]) noexcept {
        Format = fmt;
        for (int i = 0; i < 4; ++i) Color[i] = color[i];
    }
    CD3DX12_CLEAR_VALUE(DXGI_FORMAT fmt, FLOAT depth, UINT8 stencil) noexcept {
        Format = fmt;
        DepthStencil.Depth = depth;
        DepthStencil.Stencil = stencil;
    }
};

struct CD3DX12_CPU_DESCRIPTOR_HANDLE : public D3D12_CPU_DESCRIPTOR_HANDLE {
    CD3DX12_CPU_DESCRIPTOR_HANDLE() = default;
    explicit CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { ptr = h.ptr; }
    CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE h, INT offset, UINT inc) noexcept {
        ptr = static_cast<SIZE_T>(
            static_cast<INT64>(h.ptr) + static_cast<INT64>(offset) * static_cast<INT64>(inc));
    }
};

struct CD3DX12_RESOURCE_BARRIER : public D3D12_RESOURCE_BARRIER {
    static D3D12_RESOURCE_BARRIER Transition(
        ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE) noexcept {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = flags;
        b.Transition.pResource = r;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = subresource;
        return b;
    }
};

struct CD3DX12_RASTERIZER_DESC : public D3D12_RASTERIZER_DESC {
    explicit CD3DX12_RASTERIZER_DESC(CD3DX12_DEFAULT) noexcept {
        FillMode = D3D12_FILL_MODE_SOLID;
        CullMode = D3D12_CULL_MODE_BACK;
        FrontCounterClockwise = FALSE;
        DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        DepthClipEnable = TRUE;
        MultisampleEnable = FALSE;
        AntialiasedLineEnable = FALSE;
        ForcedSampleCount = 0;
        ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    }
};

struct CD3DX12_BLEND_DESC : public D3D12_BLEND_DESC {
    explicit CD3DX12_BLEND_DESC(CD3DX12_DEFAULT) noexcept {
        AlphaToCoverageEnable = FALSE;
        IndependentBlendEnable = FALSE;
        for (int i = 0; i < 8; ++i) {
            RenderTarget[i] = {
                FALSE, FALSE,
                D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                D3D12_LOGIC_OP_NOOP,
                D3D12_COLOR_WRITE_ENABLE_ALL};
        }
    }
};

struct CD3DX12_DEPTH_STENCIL_DESC : public D3D12_DEPTH_STENCIL_DESC {
    explicit CD3DX12_DEPTH_STENCIL_DESC(CD3DX12_DEFAULT) noexcept {
        DepthEnable = TRUE;
        DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        StencilEnable = FALSE;
        StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        const D3D12_DEPTH_STENCILOP_DESC defaultOp = {
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_COMPARISON_FUNC_ALWAYS};
        FrontFace = defaultOp;
        BackFace = defaultOp;
    }
};

struct CD3DX12_RANGE : public D3D12_RANGE {
    CD3DX12_RANGE(SIZE_T b, SIZE_T e) noexcept {
        Begin = b;
        End = e;
    }
};

template <typename T>
inline ID3D12CommandList* const* CommandListCast(T* const* pp) noexcept {
    return reinterpret_cast<ID3D12CommandList* const*>(pp);
}
