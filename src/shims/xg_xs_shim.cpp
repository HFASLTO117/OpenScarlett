#define OPENSCARLETT_BUILDING_XG_XS 1
#include "openscarlett/gpu/xg_xs_abi.h"

#include <atomic>
#include <cstring>
#include <new>

#include "openscarlett/core/telemetry.h"

namespace {

constexpr const char* kXgModule = "xg_xs.dll";
constexpr int32_t kHrOk = 0;
constexpr int32_t kHrNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kHrOutOfMemory = static_cast<int32_t>(0x8007000Eu);

class XGTextureAddressComputerImpl final : public IXGTextureAddressComputer {
public:
    XGTextureAddressComputerImpl(const XG_TEXTURE2D_DESC& desc, const XG_RESOURCE_LAYOUT& layout)
        : is_3d_(false), desc2d_(desc), layout_(layout) {}

    XGTextureAddressComputerImpl(const XG_TEXTURE3D_DESC& desc, const XG_RESOURCE_LAYOUT& layout)
        : is_3d_(true), desc3d_(desc), layout_(layout) {}

    uint32_t __stdcall AddRef() override {
        const uint32_t ref = ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "IXGTextureAddressComputer::AddRef", openscarlett::CallOutcome::Ok, kHrOk, 0);
        return ref;
    }

    uint32_t __stdcall Release() override {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "IXGTextureAddressComputer::Release", openscarlett::CallOutcome::Ok, kHrOk, 1);
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    int32_t __stdcall GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) const override {
        if (!pLayout) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "IXGTextureAddressComputer::GetResourceLayout",
                openscarlett::CallOutcome::Error, kHrPointer, 2);
            return kHrPointer;
        }
        *pLayout = layout_;
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "IXGTextureAddressComputer::GetResourceLayout",
            openscarlett::CallOutcome::Ok, kHrOk, 2);
        return kHrOk;
    }

    uint64_t __stdcall GetTexelElementOffsetBytes(
        uint32_t plane,
        uint32_t mipLevel,
        uint32_t xElement,
        uint32_t yElement,
        uint32_t zOrSlice,
        uint32_t sample) const override {
        if (sample > 0u) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "IXGTextureAddressComputer::GetTexelElementOffsetBytes",
                openscarlett::CallOutcome::NotImplemented, kHrNotImpl, 3);
            return UINT64_MAX;
        }
        uint64_t offset = UINT64_MAX;
        if (is_3d_) {
            if (plane == 0u) {
                offset = openscarlett::xg::ComputeElementByteOffset3D(
                    layout_, mipLevel, xElement, yElement, zOrSlice);
            }
        } else {
            offset = openscarlett::xg::ComputeElementByteOffsetPlane(
                layout_, plane, mipLevel, zOrSlice, xElement, yElement);
        }
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "IXGTextureAddressComputer::GetTexelElementOffsetBytes",
            offset == UINT64_MAX ? openscarlett::CallOutcome::Error : openscarlett::CallOutcome::Ok,
            offset == UINT64_MAX ? kHrInvalidArg : kHrOk, 3);
        return offset;
    }

    int32_t __stdcall CopyIntoSubresource(
        void* pDstTiledResource,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcLinearData,
        uint32_t srcRowPitchBytes,
        uint32_t srcSlicePitchBytes) const override {
        bool ok = false;
        if (is_3d_) {
            if (plane == 0u && subresource < layout_.MipLevels) {
                ok = openscarlett::xg::TileSurface3D(
                    desc3d_, subresource, pSrcLinearData,
                    srcRowPitchBytes, srcSlicePitchBytes,
                    pDstTiledResource, layout_.SizeBytes);
            }
        } else if (layout_.MipLevels > 0u) {
            const uint32_t mip = subresource % layout_.MipLevels;
            const uint32_t slice = subresource / layout_.MipLevels;
            ok = openscarlett::xg::TileSurface2DPlane(
                desc2d_, plane, mip, slice, pSrcLinearData, srcRowPitchBytes,
                pDstTiledResource, layout_.SizeBytes);
        }
        const int32_t hr = ok ? kHrOk : kHrInvalidArg;
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "IXGTextureAddressComputer::CopyIntoSubresource",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 4);
        return hr;
    }

    int32_t __stdcall CopyFromSubresource(
        void* pDstLinearData,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcTiledResource,
        uint32_t dstRowPitchBytes,
        uint32_t dstSlicePitchBytes) const override {
        bool ok = false;
        if (is_3d_) {
            if (plane == 0u && subresource < layout_.MipLevels) {
                ok = openscarlett::xg::UntileSurface3D(
                    desc3d_, subresource, pSrcTiledResource, layout_.SizeBytes,
                    pDstLinearData, dstRowPitchBytes, dstSlicePitchBytes);
            }
        } else if (layout_.MipLevels > 0u) {
            const uint32_t mip = subresource % layout_.MipLevels;
            const uint32_t slice = subresource / layout_.MipLevels;
            ok = openscarlett::xg::UntileSurface2DPlane(
                desc2d_, plane, mip, slice, pSrcTiledResource, layout_.SizeBytes,
                pDstLinearData, dstRowPitchBytes);
        }
        const int32_t hr = ok ? kHrOk : kHrInvalidArg;
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "IXGTextureAddressComputer::CopyFromSubresource",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 5);
        return hr;
    }

private:
    std::atomic<uint32_t> ref_count_{1u};
    bool is_3d_ = false;
    XG_TEXTURE2D_DESC desc2d_{};
    XG_TEXTURE3D_DESC desc3d_{};
    XG_RESOURCE_LAYOUT layout_{};
};

}  // namespace

extern "C" {

OPENSCARLETT_XG_API int32_t __stdcall XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer) {
    if (!ppComputer) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture2DComputer", openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    *ppComputer = nullptr;
    if (!pDesc) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture2DComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    XG_RESOURCE_LAYOUT layout{};
    if (!openscarlett::xg::ComputeTexture2DLayout(*pDesc, layout)) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture2DComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    auto* comp = new (std::nothrow) XGTextureAddressComputerImpl(*pDesc, layout);
    if (!comp) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture2DComputer", openscarlett::CallOutcome::Error, kHrOutOfMemory);
        return kHrOutOfMemory;
    }

    *ppComputer = comp;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGCreateTexture2DComputer", openscarlett::CallOutcome::Ok, kHrOk);
    return kHrOk;
}

OPENSCARLETT_XG_API int32_t __stdcall XGCreateTexture3DComputer(
    const XG_TEXTURE3D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer) {
    if (!ppComputer) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture3DComputer", openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    *ppComputer = nullptr;
    if (!pDesc) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture3DComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    XG_RESOURCE_LAYOUT layout{};
    if (!openscarlett::xg::ComputeTexture3DLayout(*pDesc, layout)) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture3DComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    auto* comp = new (std::nothrow) XGTextureAddressComputerImpl(*pDesc, layout);
    if (!comp) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture3DComputer", openscarlett::CallOutcome::Error, kHrOutOfMemory);
        return kHrOutOfMemory;
    }

    *ppComputer = comp;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGCreateTexture3DComputer", openscarlett::CallOutcome::Ok, kHrOk);
    return kHrOk;
}

OPENSCARLETT_XG_API XG_TILE_MODE __stdcall XGComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags) {
    const XG_TILE_MODE mode = openscarlett::xg::ComputeOptimalTileMode(
        dxgi_format, width, height, bind_flags);
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGComputeOptimalTileMode", openscarlett::CallOutcome::Ok, kHrOk);
    return mode;
}

OPENSCARLETT_XG_API int32_t __stdcall XGGetFormatBlockInfo(
    uint32_t dxgi_format,
    XG_FORMAT_BLOCK_INFO* pOutInfo) {
    if (!pOutInfo) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGGetFormatBlockInfo", openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    const bool ok = openscarlett::xg::GetFormatBlockInfo(dxgi_format, *pOutInfo);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGGetFormatBlockInfo",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API uint32_t __stdcall XGGetFormatPlaneCount(uint32_t dxgi_format) {
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGGetFormatPlaneCount", openscarlett::CallOutcome::Ok, kHrOk);
    return openscarlett::xg::GetFormatPlaneCount(dxgi_format);
}

OPENSCARLETT_XG_API int32_t __stdcall XGGetFormatPlaneBlockInfo(
    uint32_t dxgi_format,
    uint32_t plane,
    XG_FORMAT_BLOCK_INFO* pOutInfo) {
    if (!pOutInfo) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGGetFormatPlaneBlockInfo", openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    const bool ok = openscarlett::xg::GetFormatPlaneBlockInfo(dxgi_format, plane, *pOutInfo);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGGetFormatPlaneBlockInfo",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGTileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes) {
    if (!pDesc || !pSrcLinear || !pDstTiled) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTileSurface2D", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::TileSurface2D(
        *pDesc, mip_level, slice_index, pSrcLinear, srcRowPitchBytes, pDstTiled, dstTiledSizeBytes);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGTileSurface2D",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGUntileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes) {
    if (!pDesc || !pSrcTiled || !pDstLinear) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGUntileSurface2D", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::UntileSurface2D(
        *pDesc, mip_level, slice_index, pSrcTiled, srcTiledSizeBytes, pDstLinear, dstRowPitchBytes);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGUntileSurface2D",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGTileSurface2DPlane(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes) {
    if (!pDesc || !pSrcLinear || !pDstTiled) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTileSurface2DPlane", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::TileSurface2DPlane(
        *pDesc, plane, mip_level, slice_index, pSrcLinear, srcRowPitchBytes, pDstTiled, dstTiledSizeBytes);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGTileSurface2DPlane",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGUntileSurface2DPlane(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes) {
    if (!pDesc || !pSrcTiled || !pDstLinear) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGUntileSurface2DPlane", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::UntileSurface2DPlane(
        *pDesc, plane, mip_level, slice_index, pSrcTiled, srcTiledSizeBytes, pDstLinear, dstRowPitchBytes);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGUntileSurface2DPlane",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGTileSurface3D(
    const XG_TEXTURE3D_DESC* pDesc,
    uint32_t mip_level,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    uint32_t srcSlicePitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes) {
    if (!pDesc || !pSrcLinear || !pDstTiled) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTileSurface3D", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::TileSurface3D(
        *pDesc, mip_level, pSrcLinear, srcRowPitchBytes, srcSlicePitchBytes, pDstTiled, dstTiledSizeBytes);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGTileSurface3D",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGUntileSurface3D(
    const XG_TEXTURE3D_DESC* pDesc,
    uint32_t mip_level,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes,
    uint32_t dstSlicePitchBytes) {
    if (!pDesc || !pSrcTiled || !pDstLinear) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGUntileSurface3D", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::UntileSurface3D(
        *pDesc, mip_level, pSrcTiled, srcTiledSizeBytes, pDstLinear, dstRowPitchBytes, dstSlicePitchBytes);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGUntileSurface3D",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGComputeMipLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pOutLayout) {
    if (!pDesc || !pOutLayout) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeMipLayout", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::ComputeTexture2DLayout(*pDesc, *pOutLayout);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGComputeMipLayout",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGComputeMipLayout3D(
    const XG_TEXTURE3D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pOutLayout) {
    if (!pDesc || !pOutLayout) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeMipLayout3D", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::ComputeTexture3DLayout(*pDesc, *pOutLayout);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGComputeMipLayout3D",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API void __stdcall OpenScarlettXgResetTelemetry() {
    openscarlett::CallTelemetry::Global().Reset();
}

OPENSCARLETT_XG_API int __stdcall OpenScarlettXgGetTelemetryEntry(
    const char* symbol_name,
    uint64_t* out_total_calls) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_total_calls) *out_total_calls = entry.total_calls;
            return 1;
        }
    }
    if (out_total_calls) *out_total_calls = 0;
    return 0;
}

// DCC metadata layout API: returns E_NOTIMPL when probed with null/zero output buffer
// (preserving fail-closed telemetry contract) and computes GFX10 DCC metadata layout when
// given a valid XG_METADATA_PLANE_INFO output buffer.
OPENSCARLETT_XG_API int32_t __stdcall XGComputeDccMetadataLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    void* pOutDccLayout,
    uint64_t outDccLayoutBytes) {
    if (!pOutDccLayout || outDccLayoutBytes == 0ull) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeDccMetadataLayout",
            openscarlett::CallOutcome::NotImplemented, kHrNotImpl);
        return kHrNotImpl;
    }
    if (!pDesc || outDccLayoutBytes < sizeof(XG_METADATA_PLANE_INFO)) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeDccMetadataLayout",
            openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    XG_RESOURCE_METADATA_LAYOUT full_meta{};
    if (!openscarlett::xg::ComputeTexture2DMetadataLayout(
            *pDesc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, full_meta)) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeDccMetadataLayout",
            openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    std::memcpy(pOutDccLayout, &full_meta.Dcc, sizeof(XG_METADATA_PLANE_INFO));
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGComputeDccMetadataLayout",
        openscarlett::CallOutcome::Ok, kHrOk);
    return kHrOk;
}

OPENSCARLETT_XG_API int32_t __stdcall XGQueryScarlettPipeConfig(
    XG_CONSOLE_TYPE console_type,
    XG_SCARLETT_PIPE_CONFIG* pOutConfig) {
    if (!pOutConfig) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGQueryScarlettPipeConfig",
            openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    const bool ok = openscarlett::xg::QueryScarlettPipeConfig(console_type, *pOutConfig);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGQueryScarlettPipeConfig",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGComputeTexture2DMetadataLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_CONSOLE_TYPE console_type,
    XG_RESOURCE_METADATA_LAYOUT* pOutMeta) {
    if (!pOutMeta) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeTexture2DMetadataLayout",
            openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    if (!pDesc) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeTexture2DMetadataLayout",
            openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::ComputeTexture2DMetadataLayout(*pDesc, console_type, *pOutMeta);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGComputeTexture2DMetadataLayout",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGComputeHtileLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_CONSOLE_TYPE console_type,
    XG_METADATA_PLANE_INFO* pOutHtile) {
    if (!pOutHtile) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeHtileLayout",
            openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    if (!pDesc) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeHtileLayout",
            openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    XG_RESOURCE_METADATA_LAYOUT full_meta{};
    const bool ok = openscarlett::xg::ComputeTexture2DMetadataLayout(*pDesc, console_type, full_meta);
    if (ok) *pOutHtile = full_meta.Htile;
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGComputeHtileLayout",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGComputeCmaskLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_CONSOLE_TYPE console_type,
    XG_METADATA_PLANE_INFO* pOutCmask) {
    if (!pOutCmask) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeCmaskLayout",
            openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    if (!pDesc) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGComputeCmaskLayout",
            openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    XG_RESOURCE_METADATA_LAYOUT full_meta{};
    const bool ok = openscarlett::xg::ComputeTexture2DMetadataLayout(*pDesc, console_type, full_meta);
    if (ok) *pOutCmask = full_meta.Cmask;
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGComputeCmaskLayout",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGEncodeDCC(
    const XG_DCC_ENCODE_PARAMS* pParams,
    void* pDstDccBuffer,
    uint64_t dstDccSizeBytes,
    XG_DCC_ENCODE_RESULT* pOutResult) {
    if (!pParams || !pDstDccBuffer || dstDccSizeBytes == 0ull) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGEncodeDCC",
            openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::EncodeDccMetadata(
        *pParams, pDstDccBuffer, dstDccSizeBytes, pOutResult);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGEncodeDCC",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGDecodeDCCKey(
    uint8_t dccKeyByte,
    uint32_t* pOutBlockState) {
    if (!pOutBlockState) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGDecodeDCCKey",
            openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    XG_DCC_BLOCK_STATE state = XG_DCC_STATE_UNCOMPRESSED;
    const bool ok = openscarlett::xg::DecodeDccKeyByte(dccKeyByte, state);
    if (ok) {
        *pOutBlockState = static_cast<uint32_t>(state);
    }
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGDecodeDCCKey",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int32_t __stdcall XGVerifyDCC(
    const XG_DCC_ENCODE_PARAMS* pParams,
    const void* pDccBuffer,
    uint64_t dccSizeBytes,
    XG_DCC_ENCODE_RESULT* pOutResult) {
    if (!pParams || !pDccBuffer || dccSizeBytes == 0ull) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGVerifyDCC",
            openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }
    const bool ok = openscarlett::xg::VerifyDccMetadata(
        *pParams, pDccBuffer, dccSizeBytes, pOutResult);
    const int32_t hr = ok ? kHrOk : kHrInvalidArg;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGVerifyDCC",
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr);
    return hr;
}

OPENSCARLETT_XG_API int __stdcall OpenScarlettXgGetTelemetryDetail(
    const char* symbol_name,
    uint64_t* out_total_calls,
    uint64_t* out_not_impl_calls,
    int32_t* out_last_hr) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_total_calls) *out_total_calls = entry.total_calls;
            if (out_not_impl_calls) *out_not_impl_calls = entry.not_impl_calls;
            if (out_last_hr) *out_last_hr = entry.last_hr;
            return 1;
        }
    }
    if (out_total_calls) *out_total_calls = 0;
    if (out_not_impl_calls) *out_not_impl_calls = 0;
    if (out_last_hr) *out_last_hr = 0;
    return 0;
}

}  // extern "C"
