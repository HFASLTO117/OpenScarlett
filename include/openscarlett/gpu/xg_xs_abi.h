#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <dxgiformat.h>
#include <windows.h>
#if defined(OPENSCARLETT_BUILDING_XG_XS)
#define OPENSCARLETT_XG_API __declspec(dllexport)
#else
#define OPENSCARLETT_XG_API __declspec(dllimport)
#endif
#else
#define OPENSCARLETT_XG_API
#endif

// Clean-room Xbox Scarlett texture swizzle / tiling ABI (xg_xs.dll).
// Implements RDNA2 / D3D12 Standard Swizzle micro-tile & macro-tile layouts.

enum XG_TILE_MODE : uint32_t {
    XG_TILE_MODE_LINEAR = 0u,
    XG_TILE_MODE_2D_THIN_4KB_S = 1u,
    XG_TILE_MODE_2D_THIN_64KB_S = 2u,
    XG_TILE_MODE_2D_THIN_64KB_D = 3u,
    XG_TILE_MODE_2D_THIN_64KB_R_X = 4u,
};

enum XG_RESOURCE_DIMENSION : uint32_t {
    XG_RESOURCE_DIMENSION_TEXTURE2D = 3u,
};

enum XG_BIND_FLAGS : uint32_t {
    XG_BIND_NONE = 0x0u,
    XG_BIND_SHADER_RESOURCE = 0x8u,
    XG_BIND_RENDER_TARGET = 0x20u,
    XG_BIND_DEPTH_STENCIL = 0x40u,
    XG_BIND_UNORDERED_ACCESS = 0x80u,
    XG_BIND_DISPLAYABLE = 0x100u,
};

struct XG_FORMAT_BLOCK_INFO {
    uint32_t dxgi_format;
    uint32_t block_width;
    uint32_t block_height;
    uint32_t bytes_per_block;
    uint32_t is_compressed;
};

struct XG_TEXTURE2D_DESC {
    uint32_t Width;
    uint32_t Height;
    uint32_t MipLevels;
    uint32_t ArraySize;
    uint32_t Format;          // DXGI_FORMAT
    uint32_t SampleCount;
    uint32_t BindFlags;       // XG_BIND_FLAGS
    XG_TILE_MODE TileMode;
};

struct XG_MIPLEVEL_LAYOUT {
    uint32_t WidthElements;
    uint32_t HeightElements;
    uint32_t PaddedWidthElements;
    uint32_t PaddedHeightElements;
    uint32_t TileWidthElements;
    uint32_t TileHeightElements;
    uint32_t TilesX;
    uint32_t TilesY;
    uint32_t TileSizeBytes;
    uint64_t OffsetBytes;
    uint64_t SizeBytes;
    uint64_t SlicePitchBytes;
    uint32_t RowPitchBytes;   // Meaningful for linear layouts
};

constexpr uint32_t XG_MAX_MIP_LEVELS = 15u;

struct XG_RESOURCE_LAYOUT {
    uint64_t SizeBytes;
    uint64_t BaseAlignmentBytes;
    uint32_t MipLevels;
    uint32_t ArraySize;
    XG_TILE_MODE TileMode;
    XG_FORMAT_BLOCK_INFO FormatInfo;
    XG_MIPLEVEL_LAYOUT Mips[XG_MAX_MIP_LEVELS];
};

struct IXGTextureAddressComputer {
    virtual uint32_t __stdcall AddRef() = 0;
    virtual uint32_t __stdcall Release() = 0;
    virtual int32_t __stdcall GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) const = 0;
    virtual uint64_t __stdcall GetTexelElementOffsetBytes(
        uint32_t plane,
        uint32_t mipLevel,
        uint32_t xElement,
        uint32_t yElement,
        uint32_t zOrSlice,
        uint32_t sample) const = 0;
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
    virtual ~IXGTextureAddressComputer() = default;
};

namespace openscarlett::xg {

bool GetFormatBlockInfo(uint32_t dxgi_format, XG_FORMAT_BLOCK_INFO& out_info) noexcept;

XG_TILE_MODE ComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags) noexcept;

bool ComputeTexture2DLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out_layout) noexcept;

uint64_t ComputeElementByteOffset(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    uint32_t slice_index,
    uint32_t element_x,
    uint32_t element_y) noexcept;

bool TileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept;

bool UntileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept;

}  // namespace openscarlett::xg

extern "C" {

OPENSCARLETT_XG_API int32_t __stdcall XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer);

OPENSCARLETT_XG_API XG_TILE_MODE __stdcall XGComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags);

OPENSCARLETT_XG_API int32_t __stdcall XGGetFormatBlockInfo(
    uint32_t dxgi_format,
    XG_FORMAT_BLOCK_INFO* pOutInfo);

OPENSCARLETT_XG_API int32_t __stdcall XGTileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGUntileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGComputeMipLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pOutLayout);

OPENSCARLETT_XG_API void __stdcall OpenScarlettXgResetTelemetry();

OPENSCARLETT_XG_API int __stdcall OpenScarlettXgGetTelemetryEntry(
    const char* symbol_name,
    uint64_t* out_total_calls);

}  // extern "C"
