#define OPENSCARLETT_BUILDING_XG_XS 1
#include "openscarlett/gpu/xg_xs_abi.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

#include "openscarlett/core/telemetry.h"

namespace {

constexpr const char* kXgModule = "xg_xs.dll";
constexpr int32_t kHrOk = 0;
constexpr int32_t kHrNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kHrNoInterface = static_cast<int32_t>(0x80004002u);
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kHrOutOfMemory = static_cast<int32_t>(0x8007000Eu);

// Standard D3D12 / Xbox GDK resource layout & tiling ABI structures for
// XGCreateTextureComputer / XGTextureAddressComputer (preserving 100% compatibility
// with existing IXGTextureAddressComputer & XGCreateTexture2D/3DComputer exports).
struct XG_GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
static_assert(sizeof(XG_GUID) == 16u, "XG_GUID must be 16 bytes");

constexpr XG_GUID kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};
constexpr XG_GUID kIidXGTextureAddressComputer = {
    0x7B59B520u, 0x8F43u, 0x4C2Au, {0x9Du, 0x2Eu, 0x5Au, 0x1Cu, 0x91u, 0xA8u, 0x4Du, 0x3Bu}};

inline bool XgGuidEquals(const XG_GUID* a, const XG_GUID& b) noexcept {
    return a != nullptr && std::memcmp(a, &b, sizeof(XG_GUID)) == 0;
}

// Matches D3D12_RESOURCE_DESC / Xbox Scarlett XG_RESOURCE_DESC (56 bytes on x64).
struct XG_RESOURCE_DESC {
    uint32_t Dimension;         // 1=BUFFER, 2=TEXTURE1D, 3=TEXTURE2D, 4=TEXTURE3D
    uint64_t Alignment;
    uint64_t Width;
    uint32_t Height;
    uint16_t DepthOrArraySize;
    uint16_t MipLevels;
    uint32_t Format;            // DXGI_FORMAT
    uint32_t SampleCount;       // DXGI_SAMPLE_DESC.Count
    uint32_t SampleQuality;     // DXGI_SAMPLE_DESC.Quality
    uint32_t Layout;            // D3D12_TEXTURE_LAYOUT or XG_TILE_MODE
    uint32_t Flags;             // D3D12_RESOURCE_FLAGS
};
static_assert(sizeof(XG_RESOURCE_DESC) == 56u, "XG_RESOURCE_DESC must match D3D12_RESOURCE_DESC (56 B)");

struct XG_TEXTURE1D_DESC {
    uint32_t Width;
    uint32_t MipLevels;
    uint32_t ArraySize;
    uint32_t Format;            // DXGI_FORMAT
    uint32_t BindFlags;         // XG_BIND_FLAGS
    XG_TILE_MODE TileMode;
};

struct XG_PACKED_MIP_INFO {
    uint8_t NumStandardMips;
    uint8_t NumPackedMips;
    uint32_t NumTilesForPackedMips;
    uint32_t StartTileIndexInOverallResource;
};

struct XG_TILE_SHAPE {
    uint32_t WidthInTexels;
    uint32_t HeightInTexels;
    uint32_t DepthInTexels;
};

struct XG_SUBRESOURCE_TILING {
    uint32_t WidthInTiles;
    uint16_t HeightInTiles;
    uint16_t DepthInTiles;
    uint32_t StartTileIndexInOverallResource;
};

struct XGTextureAddressComputer {
    virtual int32_t __stdcall QueryInterface(const XG_GUID* riid, void** ppvObject) = 0;
    virtual uint32_t __stdcall AddRef() = 0;
    virtual uint32_t __stdcall Release() = 0;
    virtual int32_t __stdcall GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) const = 0;
    virtual uint64_t __stdcall GetResourceSizeBytes() const = 0;
    virtual uint64_t __stdcall GetResourceBaseAlignmentBytes() const = 0;
    virtual uint64_t __stdcall GetMipLevelOffsetBytes(uint32_t plane, uint32_t mipLevel) const = 0;
    virtual uint64_t __stdcall GetTexelElementOffsetBytes(
        uint32_t plane,
        uint32_t mipLevel,
        uint64_t xElement,
        uint32_t yElement,
        uint32_t zOrSlice,
        uint32_t sample) const = 0;
    virtual int32_t __stdcall GetTexelCoordinate(
        uint64_t resourceOffsetBytes,
        uint32_t plane,
        uint32_t mipLevel,
        uint64_t* pOutX,
        uint32_t* pOutY,
        uint32_t* pOutZOrSlice,
        uint32_t* pOutSample) const = 0;
    virtual int32_t __stdcall CopyIntoSubresource(
        void* pDstTiledResource,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcLinearData,
        uint32_t srcRowPitchBytes,
        uint32_t srcSlicePitchBytes) const = 0;
    virtual int32_t __stdcall CopyFromSubresource(
        void* pDstLinearData,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcTiledResource,
        uint32_t dstRowPitchBytes,
        uint32_t dstSlicePitchBytes) const = 0;
    virtual void __stdcall GetResourceTiling(
        uint32_t* pNumTilesForEntireResource,
        XG_PACKED_MIP_INFO* pPackedMipDesc,
        XG_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
        uint32_t* pNumSubresourceTilings,
        uint32_t firstSubresourceTilingToGet,
        XG_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) const = 0;
    virtual ~XGTextureAddressComputer() = default;
};

constexpr uint32_t kD3D12DimTexture1D = 2u;
constexpr uint32_t kD3D12DimTexture2D = 3u;
constexpr uint32_t kD3D12DimTexture3D = 4u;

constexpr uint32_t kD3D12LayoutUnknown = 0u;
constexpr uint32_t kD3D12LayoutRowMajor = 1u;
constexpr uint32_t kD3D12Layout64KBUndefinedSwizzle = 2u;
constexpr uint32_t kD3D12Layout64KBStandardSwizzle = 3u;

constexpr uint32_t kD3D12FlagAllowRenderTarget = 0x1u;
constexpr uint32_t kD3D12FlagAllowDepthStencil = 0x2u;
constexpr uint32_t kD3D12FlagAllowUnorderedAccess = 0x4u;

inline uint32_t TranslateD3D12ResourceFlagsToXgBind(uint32_t flags) noexcept {
    uint32_t bind = XG_BIND_SHADER_RESOURCE;
    if ((flags & kD3D12FlagAllowRenderTarget) != 0u) bind |= XG_BIND_RENDER_TARGET;
    if ((flags & kD3D12FlagAllowDepthStencil) != 0u) bind |= XG_BIND_DEPTH_STENCIL;
    if ((flags & kD3D12FlagAllowUnorderedAccess) != 0u) bind |= XG_BIND_UNORDERED_ACCESS;
    return bind;
}

inline bool ResolveTileModeFromResourceDesc(
    const XG_RESOURCE_DESC& desc,
    uint32_t bind_flags,
    XG_TILE_MODE& out_mode) noexcept {
    const bool is_3d = (desc.Dimension == kD3D12DimTexture3D);
    if (desc.Layout == kD3D12LayoutUnknown) {
        if (is_3d) {
            out_mode = XG_TILE_MODE_3D_THICK_64KB_S;
        } else {
            const uint32_t h = (desc.Dimension == kD3D12DimTexture1D) ? 1u : desc.Height;
            out_mode = openscarlett::xg::ComputeOptimalTileMode(
                desc.Format, static_cast<uint32_t>(desc.Width), h, bind_flags);
        }
        return true;
    }
    if (desc.Layout == kD3D12LayoutRowMajor) {
        out_mode = XG_TILE_MODE_LINEAR;
        return true;
    }
    if (desc.Layout == kD3D12Layout64KBUndefinedSwizzle ||
        desc.Layout == kD3D12Layout64KBStandardSwizzle) {
        out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_S : XG_TILE_MODE_2D_THIN_64KB_S;
        return true;
    }

    // Xbox extended D3D12_TEXTURE_LAYOUT (0x100 + XG_TILE_MODE) or direct XG_TILE_MODE enum.
    const uint32_t raw_mode =
        ((desc.Layout & 0x100u) != 0u) ? (desc.Layout & 0xFFu) : desc.Layout;
    if (raw_mode > static_cast<uint32_t>(XG_TILE_MODE_3D_THICK_64KB_R_X)) {
        return false;
    }
    const auto candidate = static_cast<XG_TILE_MODE>(raw_mode);
    if (is_3d) {
        if (candidate != XG_TILE_MODE_LINEAR && !openscarlett::xg::IsTileMode3D(candidate)) {
            return false;
        }
    } else {
        if (openscarlett::xg::IsTileMode3D(candidate)) {
            return false;
        }
    }
    out_mode = candidate;
    return true;
}

class XGResourceAddressComputerImpl final : public XGTextureAddressComputer {
public:
    XGResourceAddressComputerImpl(
        const XG_TEXTURE2D_DESC& desc,
        const XG_RESOURCE_LAYOUT& layout)
        : is_3d_(false), desc2d_(desc), layout_(layout) {}

    XGResourceAddressComputerImpl(
        const XG_TEXTURE3D_DESC& desc,
        const XG_RESOURCE_LAYOUT& layout)
        : is_3d_(true), desc3d_(desc), layout_(layout) {}

    int32_t __stdcall QueryInterface(const XG_GUID* riid, void** ppvObject) override {
        if (!ppvObject) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::QueryInterface",
                openscarlett::CallOutcome::Error, kHrPointer, 0);
            return kHrPointer;
        }
        *ppvObject = nullptr;
        if (!riid) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::QueryInterface",
                openscarlett::CallOutcome::Error, kHrInvalidArg, 0);
            return kHrInvalidArg;
        }
        if (XgGuidEquals(riid, kIidIUnknown) || XgGuidEquals(riid, kIidXGTextureAddressComputer)) {
            AddRef();
            *ppvObject = static_cast<XGTextureAddressComputer*>(this);
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::QueryInterface",
                openscarlett::CallOutcome::Ok, kHrOk, 0);
            return kHrOk;
        }
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::QueryInterface",
            openscarlett::CallOutcome::Error, kHrNoInterface, 0);
        return kHrNoInterface;
    }

    uint32_t __stdcall AddRef() override {
        const uint32_t ref = ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::AddRef",
            openscarlett::CallOutcome::Ok, kHrOk, 1);
        return ref;
    }

    uint32_t __stdcall Release() override {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::Release",
            openscarlett::CallOutcome::Ok, kHrOk, 2);
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    int32_t __stdcall GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) const override {
        if (!pLayout) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::GetResourceLayout",
                openscarlett::CallOutcome::Error, kHrPointer, 3);
            return kHrPointer;
        }
        *pLayout = layout_;
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::GetResourceLayout",
            openscarlett::CallOutcome::Ok, kHrOk, 3);
        return kHrOk;
    }

    uint64_t __stdcall GetResourceSizeBytes() const override {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::GetResourceSizeBytes",
            openscarlett::CallOutcome::Ok, kHrOk, 4);
        return layout_.SizeBytes;
    }

    uint64_t __stdcall GetResourceBaseAlignmentBytes() const override {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::GetResourceBaseAlignmentBytes",
            openscarlett::CallOutcome::Ok, kHrOk, 5);
        return layout_.BaseAlignmentBytes;
    }

    uint64_t __stdcall GetMipLevelOffsetBytes(uint32_t plane, uint32_t mipLevel) const override {
        const uint32_t plane_count = (layout_.PlaneCount == 0u) ? 1u : layout_.PlaneCount;
        if (plane >= plane_count || mipLevel >= layout_.MipLevels) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::GetMipLevelOffsetBytes",
                openscarlett::CallOutcome::Error, kHrInvalidArg, 6);
            return UINT64_MAX;
        }
        const XG_MIPLEVEL_LAYOUT& ml =
            (plane == 0u) ? layout_.Mips[mipLevel] : layout_.Plane1Mips[mipLevel];
        uint64_t offset = ml.OffsetBytes;
        if (ml.InMipTail != 0u) {
            const uint64_t tail_base =
                (plane == 0u) ? layout_.MipTailOffsetBytes : layout_.Plane1MipTailOffsetBytes;
            offset = tail_base + ml.MipTailSubTileOffsetBytes;
        }
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::GetMipLevelOffsetBytes",
            openscarlett::CallOutcome::Ok, kHrOk, 6);
        return offset;
    }

    uint64_t __stdcall GetTexelElementOffsetBytes(
        uint32_t plane,
        uint32_t mipLevel,
        uint64_t xElement,
        uint32_t yElement,
        uint32_t zOrSlice,
        uint32_t sample) const override {
        if (sample > 0u) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::GetTexelElementOffsetBytes",
                openscarlett::CallOutcome::NotImplemented, kHrNotImpl, 7);
            return UINT64_MAX;
        }
        if (xElement > UINT32_MAX) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::GetTexelElementOffsetBytes",
                openscarlett::CallOutcome::Error, kHrInvalidArg, 7);
            return UINT64_MAX;
        }
        uint64_t offset = UINT64_MAX;
        if (is_3d_) {
            if (plane == 0u) {
                offset = openscarlett::xg::ComputeElementByteOffset3D(
                    layout_, mipLevel, static_cast<uint32_t>(xElement), yElement, zOrSlice);
            }
        } else {
            offset = openscarlett::xg::ComputeElementByteOffsetPlane(
                layout_, plane, mipLevel, zOrSlice, static_cast<uint32_t>(xElement), yElement);
        }
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::GetTexelElementOffsetBytes",
            offset == UINT64_MAX ? openscarlett::CallOutcome::Error : openscarlett::CallOutcome::Ok,
            offset == UINT64_MAX ? kHrInvalidArg : kHrOk, 7);
        return offset;
    }

    int32_t __stdcall GetTexelCoordinate(
        uint64_t resourceOffsetBytes,
        uint32_t plane,
        uint32_t mipLevel,
        uint64_t* pOutX,
        uint32_t* pOutY,
        uint32_t* pOutZOrSlice,
        uint32_t* pOutSample) const override {
        const uint32_t plane_count = (layout_.PlaneCount == 0u) ? 1u : layout_.PlaneCount;
        if (plane >= plane_count || mipLevel >= layout_.MipLevels ||
            resourceOffsetBytes >= layout_.SizeBytes) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::GetTexelCoordinate",
                openscarlett::CallOutcome::Error, kHrInvalidArg, 8);
            return kHrInvalidArg;
        }

        const XG_MIPLEVEL_LAYOUT& ml =
            (plane == 0u) ? layout_.Mips[mipLevel] : layout_.Plane1Mips[mipLevel];
        const uint32_t bpe = (plane == 0u) ? layout_.FormatInfo.bytes_per_block
                                           : layout_.PlaneFormatInfo[1].bytes_per_block;
        if (bpe == 0u) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::GetTexelCoordinate",
                openscarlett::CallOutcome::Error, kHrInvalidArg, 8);
            return kHrInvalidArg;
        }

        uint32_t found_x = 0u;
        uint32_t found_y = 0u;
        uint32_t found_z = 0u;
        bool found = false;

        if (is_3d_) {
            if (plane != 0u) {
                openscarlett::CallTelemetry::Global().Record(
                    kXgModule, "XGTextureAddressComputer::GetTexelCoordinate",
                    openscarlett::CallOutcome::Error, kHrInvalidArg, 8);
                return kHrInvalidArg;
            }
            if (layout_.TileMode == XG_TILE_MODE_LINEAR) {
                if (resourceOffsetBytes >= ml.OffsetBytes && ml.DepthPitchBytes > 0ull &&
                    ml.RowPitchBytes > 0u) {
                    const uint64_t rel = resourceOffsetBytes - ml.OffsetBytes;
                    const uint64_t z = rel / ml.DepthPitchBytes;
                    const uint64_t rem_z = rel % ml.DepthPitchBytes;
                    const uint64_t y = rem_z / ml.RowPitchBytes;
                    const uint64_t x = (rem_z % ml.RowPitchBytes) / bpe;
                    if (z < ml.PaddedDepthElements && y < ml.PaddedHeightElements &&
                        x < ml.PaddedWidthElements) {
                        found_x = static_cast<uint32_t>(x);
                        found_y = static_cast<uint32_t>(y);
                        found_z = static_cast<uint32_t>(z);
                        found = true;
                    }
                }
            } else if (ml.InMipTail == 0u) {
                if (resourceOffsetBytes >= ml.OffsetBytes && ml.TileSizeBytes > 0u &&
                    ml.TilesX > 0u && ml.TilesY > 0u) {
                    const uint64_t rel = resourceOffsetBytes - ml.OffsetBytes;
                    const uint64_t tile_idx = rel / ml.TileSizeBytes;
                    const uint64_t tiles_xy = static_cast<uint64_t>(ml.TilesX) * ml.TilesY;
                    const uint32_t tz = static_cast<uint32_t>(tile_idx / tiles_xy);
                    const uint64_t rem_xy = tile_idx % tiles_xy;
                    const uint32_t ty = static_cast<uint32_t>(rem_xy / ml.TilesX);
                    const uint32_t tx = static_cast<uint32_t>(rem_xy % ml.TilesX);
                    if (tz < ml.TilesZ && ty < ml.TilesY && tx < ml.TilesX) {
                        const uint32_t base_x = tx * ml.TileWidthElements;
                        const uint32_t base_y = ty * ml.TileHeightElements;
                        const uint32_t base_z = tz * ml.TileDepthElements;
                        for (uint32_t lz = 0u; lz < ml.TileDepthElements && !found; ++lz) {
                            for (uint32_t ly = 0u; ly < ml.TileHeightElements && !found; ++ly) {
                                for (uint32_t lx = 0u; lx < ml.TileWidthElements; ++lx) {
                                    const uint64_t off = openscarlett::xg::ComputeElementByteOffset3D(
                                        layout_, mipLevel, base_x + lx, base_y + ly, base_z + lz);
                                    if (off != UINT64_MAX && resourceOffsetBytes >= off &&
                                        resourceOffsetBytes < off + bpe) {
                                        found_x = base_x + lx;
                                        found_y = base_y + ly;
                                        found_z = base_z + lz;
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                for (uint32_t z = 0u; z < ml.PaddedDepthElements && !found; ++z) {
                    for (uint32_t y = 0u; y < ml.PaddedHeightElements && !found; ++y) {
                        for (uint32_t x = 0u; x < ml.PaddedWidthElements; ++x) {
                            const uint64_t off = openscarlett::xg::ComputeElementByteOffset3D(
                                layout_, mipLevel, x, y, z);
                            if (off != UINT64_MAX && resourceOffsetBytes >= off &&
                                resourceOffsetBytes < off + bpe) {
                                found_x = x;
                                found_y = y;
                                found_z = z;
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
        } else {
            if (layout_.TileMode == XG_TILE_MODE_LINEAR) {
                if (resourceOffsetBytes >= ml.OffsetBytes && ml.SlicePitchBytes > 0ull &&
                    ml.RowPitchBytes > 0u) {
                    const uint64_t rel = resourceOffsetBytes - ml.OffsetBytes;
                    const uint64_t slice = rel / ml.SlicePitchBytes;
                    const uint64_t rem_s = rel % ml.SlicePitchBytes;
                    const uint64_t y = rem_s / ml.RowPitchBytes;
                    const uint64_t x = (rem_s % ml.RowPitchBytes) / bpe;
                    if (slice < layout_.ArraySize && y < ml.PaddedHeightElements &&
                        x < ml.PaddedWidthElements) {
                        found_x = static_cast<uint32_t>(x);
                        found_y = static_cast<uint32_t>(y);
                        found_z = static_cast<uint32_t>(slice);
                        found = true;
                    }
                }
            } else if (ml.InMipTail == 0u) {
                if (resourceOffsetBytes >= ml.OffsetBytes && ml.SlicePitchBytes > 0ull &&
                    ml.TileSizeBytes > 0u && ml.TilesX > 0u) {
                    const uint64_t rel = resourceOffsetBytes - ml.OffsetBytes;
                    const uint64_t slice = rel / ml.SlicePitchBytes;
                    const uint64_t in_slice = rel % ml.SlicePitchBytes;
                    const uint64_t tile_idx = in_slice / ml.TileSizeBytes;
                    const uint32_t ty = static_cast<uint32_t>(tile_idx / ml.TilesX);
                    const uint32_t tx = static_cast<uint32_t>(tile_idx % ml.TilesX);
                    if (slice < layout_.ArraySize && ty < ml.TilesY && tx < ml.TilesX) {
                        const uint32_t base_x = tx * ml.TileWidthElements;
                        const uint32_t base_y = ty * ml.TileHeightElements;
                        const uint32_t s32 = static_cast<uint32_t>(slice);
                        for (uint32_t ly = 0u; ly < ml.TileHeightElements && !found; ++ly) {
                            for (uint32_t lx = 0u; lx < ml.TileWidthElements; ++lx) {
                                const uint64_t off =
                                    openscarlett::xg::ComputeElementByteOffsetPlane(
                                        layout_, plane, mipLevel, s32, base_x + lx, base_y + ly);
                                if (off != UINT64_MAX && resourceOffsetBytes >= off &&
                                    resourceOffsetBytes < off + bpe) {
                                    found_x = base_x + lx;
                                    found_y = base_y + ly;
                                    found_z = s32;
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            } else {
                const uint64_t tail_base =
                    (plane == 0u) ? layout_.MipTailOffsetBytes : layout_.Plane1MipTailOffsetBytes;
                const uint64_t tail_pitch =
                    (plane == 0u) ? layout_.MipTailSlicePitchBytes
                                  : layout_.Plane1MipTailSlicePitchBytes;
                if (resourceOffsetBytes >= tail_base && tail_pitch > 0ull) {
                    const uint64_t slice = (resourceOffsetBytes - tail_base) / tail_pitch;
                    if (slice < layout_.ArraySize) {
                        const uint32_t s32 = static_cast<uint32_t>(slice);
                        for (uint32_t y = 0u; y < ml.PaddedHeightElements && !found; ++y) {
                            for (uint32_t x = 0u; x < ml.PaddedWidthElements; ++x) {
                                const uint64_t off =
                                    openscarlett::xg::ComputeElementByteOffsetPlane(
                                        layout_, plane, mipLevel, s32, x, y);
                                if (off != UINT64_MAX && resourceOffsetBytes >= off &&
                                    resourceOffsetBytes < off + bpe) {
                                    found_x = x;
                                    found_y = y;
                                    found_z = s32;
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!found) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGTextureAddressComputer::GetTexelCoordinate",
                openscarlett::CallOutcome::Error, kHrInvalidArg, 8);
            return kHrInvalidArg;
        }

        if (pOutX) *pOutX = found_x;
        if (pOutY) *pOutY = found_y;
        if (pOutZOrSlice) *pOutZOrSlice = found_z;
        if (pOutSample) *pOutSample = 0u;

        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::GetTexelCoordinate",
            openscarlett::CallOutcome::Ok, kHrOk, 8);
        return kHrOk;
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
            kXgModule, "XGTextureAddressComputer::CopyIntoSubresource",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 9);
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
            kXgModule, "XGTextureAddressComputer::CopyFromSubresource",
            ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 10);
        return hr;
    }

    void __stdcall GetResourceTiling(
        uint32_t* pNumTilesForEntireResource,
        XG_PACKED_MIP_INFO* pPackedMipDesc,
        XG_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
        uint32_t* pNumSubresourceTilings,
        uint32_t firstSubresourceTilingToGet,
        XG_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) const override {
        constexpr uint64_t kTileBytes64K = 65536ull;
        constexpr uint32_t kPackedTileSentinel = 0xFFFFFFFFu;

        const uint32_t total_tiles =
            static_cast<uint32_t>((layout_.SizeBytes + kTileBytes64K - 1ull) / kTileBytes64K);
        if (pNumTilesForEntireResource) {
            *pNumTilesForEntireResource = total_tiles;
        }

        const uint32_t num_std = std::min(layout_.MipTailStartLevel, layout_.MipLevels);
        const uint32_t num_packed = layout_.MipLevels - num_std;
        if (pPackedMipDesc) {
            pPackedMipDesc->NumStandardMips = static_cast<uint8_t>(num_std);
            pPackedMipDesc->NumPackedMips = static_cast<uint8_t>(num_packed);
            if (num_packed > 0u) {
                const uint32_t tiles_per_slice = static_cast<uint32_t>(
                    (layout_.MipTailSlicePitchBytes + kTileBytes64K - 1ull) / kTileBytes64K);
                pPackedMipDesc->NumTilesForPackedMips =
                    std::max(1u, tiles_per_slice) * std::max(1u, layout_.ArraySize);
                pPackedMipDesc->StartTileIndexInOverallResource =
                    static_cast<uint32_t>(layout_.MipTailOffsetBytes / kTileBytes64K);
            } else {
                pPackedMipDesc->NumTilesForPackedMips = 0u;
                pPackedMipDesc->StartTileIndexInOverallResource = 0u;
            }
        }

        if (pStandardTileShapeForNonPackedMips) {
            const XG_MIPLEVEL_LAYOUT& m0 = layout_.Mips[0];
            pStandardTileShapeForNonPackedMips->WidthInTexels =
                std::max(1u, m0.TileWidthElements * std::max(1u, layout_.FormatInfo.block_width));
            pStandardTileShapeForNonPackedMips->HeightInTexels =
                std::max(1u, m0.TileHeightElements * std::max(1u, layout_.FormatInfo.block_height));
            pStandardTileShapeForNonPackedMips->DepthInTexels =
                std::max(1u, m0.TileDepthElements);
        }

        const uint32_t total_subresources =
            is_3d_ ? layout_.MipLevels : (layout_.MipLevels * std::max(1u, layout_.ArraySize));
        if (pNumSubresourceTilings) {
            const uint32_t requested = *pNumSubresourceTilings;
            const uint32_t avail = (firstSubresourceTilingToGet < total_subresources)
                                       ? (total_subresources - firstSubresourceTilingToGet)
                                       : 0u;
            const uint32_t count = std::min(requested, avail);
            *pNumSubresourceTilings = count;
            if (pSubresourceTilingsForNonPackedMips) {
                for (uint32_t i = 0u; i < count; ++i) {
                    const uint32_t sub = firstSubresourceTilingToGet + i;
                    const uint32_t mip = is_3d_ ? sub : (sub % layout_.MipLevels);
                    const uint32_t slice = is_3d_ ? 0u : (sub / layout_.MipLevels);
                    XG_SUBRESOURCE_TILING& dst = pSubresourceTilingsForNonPackedMips[i];
                    if (mip < num_std) {
                        const XG_MIPLEVEL_LAYOUT& ml = layout_.Mips[mip];
                        dst.WidthInTiles = ml.TilesX;
                        dst.HeightInTiles = static_cast<uint16_t>(ml.TilesY);
                        dst.DepthInTiles = static_cast<uint16_t>(std::max(1u, ml.TilesZ));
                        const uint64_t off =
                            ml.OffsetBytes + static_cast<uint64_t>(slice) * ml.SlicePitchBytes;
                        dst.StartTileIndexInOverallResource =
                            static_cast<uint32_t>(off / kTileBytes64K);
                    } else {
                        dst.WidthInTiles = 0u;
                        dst.HeightInTiles = 0u;
                        dst.DepthInTiles = 0u;
                        dst.StartTileIndexInOverallResource = kPackedTileSentinel;
                    }
                }
            }
        }

        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGTextureAddressComputer::GetResourceTiling",
            openscarlett::CallOutcome::Ok, kHrOk, 11);
    }

private:
    std::atomic<uint32_t> ref_count_{1u};
    bool is_3d_ = false;
    XG_TEXTURE2D_DESC desc2d_{};
    XG_TEXTURE3D_DESC desc3d_{};
    XG_RESOURCE_LAYOUT layout_{};
};

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

OPENSCARLETT_XG_API int32_t __stdcall XGCreateTextureComputer(
    const XG_RESOURCE_DESC* pDesc,
    XGTextureAddressComputer** ppComputer) {
    if (!ppComputer) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    *ppComputer = nullptr;
    if (!pDesc || pDesc->Width == 0ull || pDesc->Width > UINT32_MAX) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    const uint32_t bind_flags = TranslateD3D12ResourceFlagsToXgBind(pDesc->Flags);
    XG_TILE_MODE tile_mode = XG_TILE_MODE_LINEAR;
    if (!ResolveTileModeFromResourceDesc(*pDesc, bind_flags, tile_mode)) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    XG_RESOURCE_LAYOUT layout{};
    XGResourceAddressComputerImpl* comp = nullptr;

    if (pDesc->Dimension == kD3D12DimTexture3D) {
        XG_TEXTURE3D_DESC d3d{};
        d3d.Width = static_cast<uint32_t>(pDesc->Width);
        d3d.Height = pDesc->Height;
        d3d.Depth = (pDesc->DepthOrArraySize == 0u) ? 1u : pDesc->DepthOrArraySize;
        d3d.MipLevels = pDesc->MipLevels;
        d3d.Format = pDesc->Format;
        d3d.BindFlags = bind_flags;
        d3d.TileMode = tile_mode;
        if (!openscarlett::xg::ComputeTexture3DLayout(d3d, layout)) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
            return kHrInvalidArg;
        }
        comp = new (std::nothrow) XGResourceAddressComputerImpl(d3d, layout);
    } else if (pDesc->Dimension == kD3D12DimTexture1D || pDesc->Dimension == kD3D12DimTexture2D) {
        XG_TEXTURE2D_DESC d2d{};
        d2d.Width = static_cast<uint32_t>(pDesc->Width);
        d2d.Height = (pDesc->Dimension == kD3D12DimTexture1D) ? 1u : pDesc->Height;
        d2d.MipLevels = pDesc->MipLevels;
        d2d.ArraySize = (pDesc->DepthOrArraySize == 0u) ? 1u : pDesc->DepthOrArraySize;
        d2d.Format = pDesc->Format;
        d2d.SampleCount = (pDesc->SampleCount == 0u) ? 1u : pDesc->SampleCount;
        d2d.BindFlags = bind_flags;
        d2d.TileMode = tile_mode;
        if (!openscarlett::xg::ComputeTexture2DLayout(d2d, layout)) {
            openscarlett::CallTelemetry::Global().Record(
                kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
            return kHrInvalidArg;
        }
        comp = new (std::nothrow) XGResourceAddressComputerImpl(d2d, layout);
    } else {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    if (!comp) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Error, kHrOutOfMemory);
        return kHrOutOfMemory;
    }

    *ppComputer = comp;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGCreateTextureComputer", openscarlett::CallOutcome::Ok, kHrOk);
    return kHrOk;
}

OPENSCARLETT_XG_API int32_t __stdcall XGCreateTexture1DComputer(
    const XG_TEXTURE1D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer) {
    if (!ppComputer) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture1DComputer", openscarlett::CallOutcome::Error, kHrPointer);
        return kHrPointer;
    }
    *ppComputer = nullptr;
    if (!pDesc) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture1DComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    XG_TEXTURE2D_DESC d2d{};
    d2d.Width = pDesc->Width;
    d2d.Height = 1u;
    d2d.MipLevels = pDesc->MipLevels;
    d2d.ArraySize = (pDesc->ArraySize == 0u) ? 1u : pDesc->ArraySize;
    d2d.Format = pDesc->Format;
    d2d.SampleCount = 1u;
    d2d.BindFlags = pDesc->BindFlags;
    d2d.TileMode = pDesc->TileMode;

    XG_RESOURCE_LAYOUT layout{};
    if (!openscarlett::xg::ComputeTexture2DLayout(d2d, layout)) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture1DComputer", openscarlett::CallOutcome::Error, kHrInvalidArg);
        return kHrInvalidArg;
    }

    auto* comp = new (std::nothrow) XGTextureAddressComputerImpl(d2d, layout);
    if (!comp) {
        openscarlett::CallTelemetry::Global().Record(
            kXgModule, "XGCreateTexture1DComputer", openscarlett::CallOutcome::Error, kHrOutOfMemory);
        return kHrOutOfMemory;
    }

    *ppComputer = comp;
    openscarlett::CallTelemetry::Global().Record(
        kXgModule, "XGCreateTexture1DComputer", openscarlett::CallOutcome::Ok, kHrOk);
    return kHrOk;
}

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
