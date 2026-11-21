#include "openscarlett/gpu/xg_xs_abi.h"

#include <algorithm>
#include <cstring>

namespace openscarlett::xg {

namespace {

constexpr uint32_t AlignUpU32(uint32_t value, uint32_t alignment) noexcept {
    return (alignment <= 1u) ? value : ((value + alignment - 1u) & ~(alignment - 1u));
}

constexpr uint64_t AlignUpU64(uint64_t value, uint64_t alignment) noexcept {
    return (alignment <= 1ull) ? value : ((value + alignment - 1ull) & ~(alignment - 1ull));
}

constexpr uint32_t Log2Pow2(uint32_t v) noexcept {
    uint32_t r = 0u;
    while (v > 1u) {
        v >>= 1u;
        ++r;
    }
    return r;
}

// Rectangular 2D Morton (Z-order) encoder:
// Strictly bijective over [0, 2^bits_x) x [0, 2^bits_y) -> [0, 2^(bits_x + bits_y)).
// Interleaves (x, y) bit pairs up to min(bits_x, bits_y), then places remaining upper bits.
constexpr uint32_t MortonEncode2D(
    uint32_t x,
    uint32_t y,
    uint32_t bits_x,
    uint32_t bits_y,
    bool y_first = false) noexcept {
    uint32_t result = 0u;
    uint32_t out_bit = 0u;
    const uint32_t common = (bits_x < bits_y) ? bits_x : bits_y;
    for (uint32_t i = 0u; i < common; ++i) {
        const uint32_t bx = (x >> i) & 1u;
        const uint32_t by = (y >> i) & 1u;
        if (!y_first) {
            result |= (bx << out_bit++);
            result |= (by << out_bit++);
        } else {
            result |= (by << out_bit++);
            result |= (bx << out_bit++);
        }
    }
    for (uint32_t i = common; i < bits_x; ++i) {
        result |= (((x >> i) & 1u) << out_bit++);
    }
    for (uint32_t i = common; i < bits_y; ++i) {
        result |= (((y >> i) & 1u) << out_bit++);
    }
    return result;
}

struct TileGeometry {
    uint32_t tile_size_bytes = 256u;
    uint32_t tile_w_elems = 1u;
    uint32_t tile_h_elems = 1u;
    uint32_t micro_w_elems = 1u;
    uint32_t micro_h_elems = 1u;
    uint32_t micro_size_bytes = 64u;
};

// Standard RDNA2 / D3D12 Standard Swizzle tile dimensions per bytes-per-element.
// Invariant: tile_w_elems * tile_h_elems * bpe == tile_size_bytes,
//            micro_w_elems * micro_h_elems * bpe == micro_size_bytes.
bool GetTileGeometry(XG_TILE_MODE mode, uint32_t bpe, TileGeometry& out) noexcept {
    if (mode == XG_TILE_MODE_LINEAR) {
        out.tile_size_bytes = 256u;
        out.tile_w_elems = 1u;
        out.tile_h_elems = 1u;
        out.micro_w_elems = 1u;
        out.micro_h_elems = 1u;
        out.micro_size_bytes = bpe;
        return true;
    }

    if (mode == XG_TILE_MODE_2D_THIN_4KB_S) {
        out.tile_size_bytes = 4096u;
        out.micro_size_bytes = 64u;
        switch (bpe) {
            case 1u:  out.tile_w_elems = 64u; out.tile_h_elems = 64u; out.micro_w_elems = 8u; out.micro_h_elems = 8u; return true;
            case 2u:  out.tile_w_elems = 64u; out.tile_h_elems = 32u; out.micro_w_elems = 8u; out.micro_h_elems = 4u; return true;
            case 4u:  out.tile_w_elems = 32u; out.tile_h_elems = 32u; out.micro_w_elems = 4u; out.micro_h_elems = 4u; return true;
            case 8u:  out.tile_w_elems = 32u; out.tile_h_elems = 16u; out.micro_w_elems = 4u; out.micro_h_elems = 2u; return true;
            case 16u: out.tile_w_elems = 16u; out.tile_h_elems = 16u; out.micro_w_elems = 2u; out.micro_h_elems = 2u; return true;
            default: return false;
        }
    }

    if (mode == XG_TILE_MODE_2D_THIN_64KB_S ||
        mode == XG_TILE_MODE_2D_THIN_64KB_D ||
        mode == XG_TILE_MODE_2D_THIN_64KB_R_X) {
        out.tile_size_bytes = 65536u;
        out.micro_size_bytes = 256u;
        switch (bpe) {
            case 1u:  out.tile_w_elems = 256u; out.tile_h_elems = 256u; out.micro_w_elems = 16u; out.micro_h_elems = 16u; return true;
            case 2u:  out.tile_w_elems = 256u; out.tile_h_elems = 128u; out.micro_w_elems = 16u; out.micro_h_elems = 8u;  return true;
            case 4u:  out.tile_w_elems = 128u; out.tile_h_elems = 128u; out.micro_w_elems = 8u;  out.micro_h_elems = 8u;  return true;
            case 8u:  out.tile_w_elems = 128u; out.tile_h_elems = 64u;  out.micro_w_elems = 8u;  out.micro_h_elems = 4u;  return true;
            case 16u: out.tile_w_elems = 64u;  out.tile_h_elems = 64u;  out.micro_w_elems = 4u;  out.micro_h_elems = 4u;  return true;
            default: return false;
        }
    }

    return false;
}

uint32_t ComputeMaxMips(uint32_t width, uint32_t height) noexcept {
    uint32_t dim = (width > height) ? width : height;
    uint32_t mips = 1u;
    while (dim > 1u && mips < XG_MAX_MIP_LEVELS) {
        dim >>= 1u;
        ++mips;
    }
    return mips;
}

}  // namespace

bool GetFormatBlockInfo(uint32_t dxgi_format, XG_FORMAT_BLOCK_INFO& out_info) noexcept {
    out_info = XG_FORMAT_BLOCK_INFO{};
    out_info.dxgi_format = dxgi_format;

    switch (static_cast<DXGI_FORMAT>(dxgi_format)) {
        // 8 bpp (1 byte)
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 1u;
            out_info.is_compressed = 0u;
            return true;

        // 16 bpp (2 bytes)
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 2u;
            out_info.is_compressed = 0u;
            return true;

        // 32 bpp (4 bytes)
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 4u;
            out_info.is_compressed = 0u;
            return true;

        // 64 bpp (8 bytes)
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 8u;
            out_info.is_compressed = 0u;
            return true;

        // 128 bpp (16 bytes)
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 16u;
            out_info.is_compressed = 0u;
            return true;

        // 4x4 Block-Compressed 8-byte blocks (BC1, BC4)
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            out_info.block_width = 4u;
            out_info.block_height = 4u;
            out_info.bytes_per_block = 8u;
            out_info.is_compressed = 1u;
            return true;

        // 4x4 Block-Compressed 16-byte blocks (BC2, BC3, BC5, BC6H, BC7)
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            out_info.block_width = 4u;
            out_info.block_height = 4u;
            out_info.bytes_per_block = 16u;
            out_info.is_compressed = 1u;
            return true;

        default:
            return false;
    }
}

XG_TILE_MODE ComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags) noexcept {
    XG_FORMAT_BLOCK_INFO info{};
    if (!GetFormatBlockInfo(dxgi_format, info)) {
        return XG_TILE_MODE_LINEAR;
    }
    if ((bind_flags & XG_BIND_DISPLAYABLE) != 0u) {
        return XG_TILE_MODE_2D_THIN_64KB_D;
    }
    if ((bind_flags & (XG_BIND_RENDER_TARGET | XG_BIND_DEPTH_STENCIL)) != 0u) {
        return XG_TILE_MODE_2D_THIN_64KB_R_X;
    }
    const uint64_t elems_x = (width + info.block_width - 1u) / info.block_width;
    const uint64_t elems_y = (height + info.block_height - 1u) / info.block_height;
    const uint64_t raw_bytes = elems_x * elems_y * info.bytes_per_block;
    if (raw_bytes <= 16384ull) {
        return XG_TILE_MODE_2D_THIN_4KB_S;
    }
    return XG_TILE_MODE_2D_THIN_64KB_S;
}

bool ComputeTexture2DLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out_layout) noexcept {
    std::memset(&out_layout, 0, sizeof(out_layout));
    if (desc.Width == 0u || desc.Height == 0u) {
        return false;
    }

    XG_FORMAT_BLOCK_INFO fmt{};
    if (!GetFormatBlockInfo(desc.Format, fmt)) {
        return false;
    }

    TileGeometry geom{};
    if (!GetTileGeometry(desc.TileMode, fmt.bytes_per_block, geom)) {
        return false;
    }

    const uint32_t max_mips = ComputeMaxMips(desc.Width, desc.Height);
    const uint32_t mip_levels = (desc.MipLevels == 0u) ? max_mips : std::min(desc.MipLevels, max_mips);
    const uint32_t array_size = (desc.ArraySize == 0u) ? 1u : desc.ArraySize;

    out_layout.FormatInfo = fmt;
    out_layout.TileMode = desc.TileMode;
    out_layout.MipLevels = mip_levels;
    out_layout.ArraySize = array_size;
    out_layout.BaseAlignmentBytes = geom.tile_size_bytes;

    uint64_t cursor = 0ull;
    for (uint32_t m = 0u; m < mip_levels; ++m) {
        const uint32_t mip_w = std::max(1u, desc.Width >> m);
        const uint32_t mip_h = std::max(1u, desc.Height >> m);
        const uint32_t elems_x = (mip_w + fmt.block_width - 1u) / fmt.block_width;
        const uint32_t elems_y = (mip_h + fmt.block_height - 1u) / fmt.block_height;

        XG_MIPLEVEL_LAYOUT& ml = out_layout.Mips[m];
        ml.WidthElements = elems_x;
        ml.HeightElements = elems_y;
        ml.TileWidthElements = geom.tile_w_elems;
        ml.TileHeightElements = geom.tile_h_elems;
        ml.TileSizeBytes = geom.tile_size_bytes;

        if (desc.TileMode == XG_TILE_MODE_LINEAR) {
            cursor = AlignUpU64(cursor, 256ull);
            const uint32_t row_pitch = AlignUpU32(elems_x * fmt.bytes_per_block, 256u);
            const uint64_t slice_bytes = static_cast<uint64_t>(row_pitch) * elems_y;
            ml.PaddedWidthElements = row_pitch / fmt.bytes_per_block;
            ml.PaddedHeightElements = elems_y;
            ml.TilesX = 1u;
            ml.TilesY = 1u;
            ml.RowPitchBytes = row_pitch;
            ml.OffsetBytes = cursor;
            ml.SlicePitchBytes = AlignUpU64(slice_bytes, 256ull);
            ml.SizeBytes = ml.SlicePitchBytes * array_size;
            cursor += ml.SizeBytes;
        } else {
            cursor = AlignUpU64(cursor, geom.tile_size_bytes);
            const uint32_t padded_x = AlignUpU32(elems_x, geom.tile_w_elems);
            const uint32_t padded_y = AlignUpU32(elems_y, geom.tile_h_elems);
            const uint32_t tiles_x = padded_x / geom.tile_w_elems;
            const uint32_t tiles_y = padded_y / geom.tile_h_elems;
            const uint64_t slice_bytes =
                static_cast<uint64_t>(tiles_x) * tiles_y * geom.tile_size_bytes;
            ml.PaddedWidthElements = padded_x;
            ml.PaddedHeightElements = padded_y;
            ml.TilesX = tiles_x;
            ml.TilesY = tiles_y;
            ml.RowPitchBytes = 0u;
            ml.OffsetBytes = cursor;
            ml.SlicePitchBytes = slice_bytes;
            ml.SizeBytes = slice_bytes * array_size;
            cursor += ml.SizeBytes;
        }
    }

    out_layout.SizeBytes = AlignUpU64(cursor, out_layout.BaseAlignmentBytes);
    return true;
}

uint64_t ComputeElementByteOffset(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    uint32_t slice_index,
    uint32_t element_x,
    uint32_t element_y) noexcept {
    if (mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) {
        return UINT64_MAX;
    }
    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    if (element_x >= ml.PaddedWidthElements || element_y >= ml.PaddedHeightElements) {
        return UINT64_MAX;
    }

    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint64_t slice_base = ml.OffsetBytes + static_cast<uint64_t>(slice_index) * ml.SlicePitchBytes;

    if (layout.TileMode == XG_TILE_MODE_LINEAR) {
        return slice_base +
               static_cast<uint64_t>(element_y) * ml.RowPitchBytes +
               static_cast<uint64_t>(element_x) * bpe;
    }

    TileGeometry geom{};
    if (!GetTileGeometry(layout.TileMode, bpe, geom)) {
        return UINT64_MAX;
    }

    const uint32_t tile_x = element_x / geom.tile_w_elems;
    const uint32_t tile_y = element_y / geom.tile_h_elems;
    const uint32_t in_tile_x = element_x % geom.tile_w_elems;
    const uint32_t in_tile_y = element_y % geom.tile_h_elems;
    const uint64_t tile_index = static_cast<uint64_t>(tile_y) * ml.TilesX + tile_x;

    uint32_t offset_in_tile = 0u;

    if (layout.TileMode == XG_TILE_MODE_2D_THIN_64KB_D) {
        // Displayable 64 KiB layout: full-tile Y-first Morton interleaving.
        const uint32_t bits_x = Log2Pow2(geom.tile_w_elems);
        const uint32_t bits_y = Log2Pow2(geom.tile_h_elems);
        const uint32_t elem_index = MortonEncode2D(in_tile_x, in_tile_y, bits_x, bits_y, true);
        offset_in_tile = elem_index * bpe;
    } else {
        // Standard Swizzle (4KB_S / 64KB_S) & Rotated/XOR Render-Target (64KB_R_X):
        // Two-level Morton addressing: micro-tile within macro-tile + element within micro-tile.
        const uint32_t micro_x = in_tile_x / geom.micro_w_elems;
        const uint32_t micro_y = in_tile_y / geom.micro_h_elems;
        const uint32_t local_x = in_tile_x % geom.micro_w_elems;
        const uint32_t local_y = in_tile_y % geom.micro_h_elems;

        const uint32_t micro_grid_w = geom.tile_w_elems / geom.micro_w_elems;
        const uint32_t micro_grid_h = geom.tile_h_elems / geom.micro_h_elems;

        const uint32_t micro_idx = MortonEncode2D(
            micro_x, micro_y, Log2Pow2(micro_grid_w), Log2Pow2(micro_grid_h), false);
        const uint32_t elem_in_micro = MortonEncode2D(
            local_x, local_y, Log2Pow2(geom.micro_w_elems), Log2Pow2(geom.micro_h_elems), false);

        offset_in_tile = micro_idx * geom.micro_size_bytes + elem_in_micro * bpe;

        if (layout.TileMode == XG_TILE_MODE_2D_THIN_64KB_R_X) {
            // Invertible RDNA2-style pipe/bank XOR swizzle on bits [12..15] of the 64 KiB tile.
            const uint32_t bank_xor = ((tile_x ^ (tile_y << 1u)) & 0xFu) << 12u;
            offset_in_tile ^= bank_xor;
        }
    }

    return slice_base + tile_index * geom.tile_size_bytes + offset_in_tile;
}

bool TileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept {
    if (!src_linear || !dst_tiled) return false;

    XG_RESOURCE_LAYOUT layout{};
    if (!ComputeTexture2DLayout(desc, layout)) return false;
    if (mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) return false;

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint32_t min_row_bytes = ml.WidthElements * bpe;
    if (src_row_pitch_bytes == 0u) src_row_pitch_bytes = min_row_bytes;
    if (src_row_pitch_bytes < min_row_bytes) return false;

    const uint64_t required_end =
        ml.OffsetBytes + static_cast<uint64_t>(slice_index + 1u) * ml.SlicePitchBytes;
    if (dst_tiled_size_bytes < required_end) return false;

    auto* dst_bytes = static_cast<uint8_t*>(dst_tiled);
    const auto* src_bytes = static_cast<const uint8_t*>(src_linear);

    // Zero-initialize the target subresource slice so padded tile slack is deterministic.
    const uint64_t slice_base =
        ml.OffsetBytes + static_cast<uint64_t>(slice_index) * ml.SlicePitchBytes;
    std::memset(dst_bytes + slice_base, 0, static_cast<size_t>(ml.SlicePitchBytes));

    if (layout.TileMode == XG_TILE_MODE_LINEAR) {
        for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
            std::memcpy(
                dst_bytes + slice_base + static_cast<uint64_t>(y) * ml.RowPitchBytes,
                src_bytes + static_cast<uint64_t>(y) * src_row_pitch_bytes,
                min_row_bytes);
        }
        return true;
    }

    for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
        const uint8_t* src_row = src_bytes + static_cast<uint64_t>(y) * src_row_pitch_bytes;
        for (uint32_t x = 0u; x < ml.WidthElements; ++x) {
            const uint64_t dst_off = ComputeElementByteOffset(layout, mip_level, slice_index, x, y);
            if (dst_off == UINT64_MAX || dst_off + bpe > dst_tiled_size_bytes) {
                return false;
            }
            std::memcpy(dst_bytes + dst_off, src_row + static_cast<uint64_t>(x) * bpe, bpe);
        }
    }
    return true;
}

bool UntileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept {
    if (!src_tiled || !dst_linear) return false;

    XG_RESOURCE_LAYOUT layout{};
    if (!ComputeTexture2DLayout(desc, layout)) return false;
    if (mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) return false;

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint32_t min_row_bytes = ml.WidthElements * bpe;
    if (dst_row_pitch_bytes == 0u) dst_row_pitch_bytes = min_row_bytes;
    if (dst_row_pitch_bytes < min_row_bytes) return false;

    const uint64_t required_end =
        ml.OffsetBytes + static_cast<uint64_t>(slice_index + 1u) * ml.SlicePitchBytes;
    if (src_tiled_size_bytes < required_end) return false;

    const auto* src_bytes = static_cast<const uint8_t*>(src_tiled);
    auto* dst_bytes = static_cast<uint8_t*>(dst_linear);

    const uint64_t slice_base =
        ml.OffsetBytes + static_cast<uint64_t>(slice_index) * ml.SlicePitchBytes;

    if (layout.TileMode == XG_TILE_MODE_LINEAR) {
        for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
            std::memcpy(
                dst_bytes + static_cast<uint64_t>(y) * dst_row_pitch_bytes,
                src_bytes + slice_base + static_cast<uint64_t>(y) * ml.RowPitchBytes,
                min_row_bytes);
        }
        return true;
    }

    for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
        uint8_t* dst_row = dst_bytes + static_cast<uint64_t>(y) * dst_row_pitch_bytes;
        for (uint32_t x = 0u; x < ml.WidthElements; ++x) {
            const uint64_t src_off = ComputeElementByteOffset(layout, mip_level, slice_index, x, y);
            if (src_off == UINT64_MAX || src_off + bpe > src_tiled_size_bytes) {
                return false;
            }
            std::memcpy(dst_row + static_cast<uint64_t>(x) * bpe, src_bytes + src_off, bpe);
        }
    }
    return true;
}

}  // namespace openscarlett::xg
