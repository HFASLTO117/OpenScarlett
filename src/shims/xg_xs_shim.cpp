#define OPENSCARLETT_BUILDING_XG_XS 1
#include "openscarlett/gpu/xg_xs_abi.h"

#include <atomic>
#include <cstring>
#include <new>

#include "openscarlett/core/telemetry.h"

namespace {

constexpr const char* kXgModule = "xg_xs.dll";
constexpr int32_t kHrOk = 0;
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kHrOutOfMemory = static_cast<int32_t>(0x8007000Eu);

class XGTextureAddressComputerImpl final : public IXGTextureAddressComputer {
public:
    XGTextureAddressComputerImpl(const XG_TEXTURE2D_DESC& desc, const XG_RESOURCE_LAYOUT& layout)
        : desc_(desc), layout_(layout) {}

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
        (void)plane;
        (void)sample;
        const uint64_t offset = openscarlett::xg::ComputeElementByteOffset(
            layout_, mipLevel, zOrSlice, xElement, yElement);
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
        (void)plane;
        (void)srcSlicePitchBytes;
        const uint32_t mip = subresource % layout_.MipLevels;
        const uint32_t slice = subresource / layout_.MipLevels;
        const bool ok = openscarlett::xg::TileSurface2D(
            desc_, mip, slice, pSrcLinearData, srcRowPitchBytes, pDstTiledResource, layout_.SizeBytes);
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
        (void)plane;
        (void)dstSlicePitchBytes;
        const uint32_t mip = subresource % layout_.MipLevels;
        const uint32_t slice = subresource / layout_.MipLevels;
        const bool ok = openscarlett::xg::UntileSurface2D(
            desc_, mip, slice, pSrcTiledResource, layout_.SizeBytes, pDstLinearData, dstRowPitchBytes);
        const int32_t hr = ok ? kHrOk : kHrInvalidArg;
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "IXGTextureAddressComputer::CopyFromSubresource",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 5);
        return hr;
    }

private:
    std::atomic<uint32_t> ref_count_{1u};
    XG_TEXTURE2D_DESC desc_{};
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

}  // extern "C"
