#include "device_x.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "command_list_x.h"
#include "command_queue_x.h"
#include "proxies_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_disasm.h"

namespace openscarlett::d3d12_xs {

namespace {

constexpr const char* kModule = "d3d12_xs.dll";
std::atomic<bool> g_force_r9g9b9e5_uav_fallback{false};
std::atomic<uint32_t> g_backend_mode{D3D12XBOX_BACKEND_AUTO};

bool EqualsAsciiCaseInsensitive(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

bool EnvVarEqualsIgnoreCase(const char* env_name, const char* expected) noexcept {
    char buf[128]{};
    const DWORD len = GetEnvironmentVariableA(env_name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    // Trim leading/trailing ASCII whitespace.
    size_t start = 0u;
    size_t end = len;
    while (start < end && (buf[start] == ' ' || buf[start] == '\t')) ++start;
    while (end > start && (buf[end - 1u] == ' ' || buf[end - 1u] == '\t' || buf[end - 1u] == '\r' || buf[end - 1u] == '\n')) --end;
    buf[end] = '\0';
    return EqualsAsciiCaseInsensitive(buf + start, expected);
}

bool IsRawSpirvBytecode(const void* data, size_t size) noexcept {
    if (!data || size < 20u || (size % 4u) != 0u) return false;
    uint32_t magic = 0u;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == openscarlett::kSpirvMagicNumber;
}

std::vector<uint32_t> CopySpirvWords(const void* data, size_t size) {
    std::vector<uint32_t> out(size / 4u, 0u);
    if (!out.empty() && data) {
        std::memcpy(out.data(), data, out.size() * sizeof(uint32_t));
    }
    return out;
}

openscarlett::VulkanVertexFormat DxgiToVulkanVertexFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32_UINT: return openscarlett::VulkanVertexFormat::R32Uint;
        case DXGI_FORMAT_R32_FLOAT: return openscarlett::VulkanVertexFormat::R32Sfloat;
        case DXGI_FORMAT_R32G32_FLOAT: return openscarlett::VulkanVertexFormat::R32G32Sfloat;
        case DXGI_FORMAT_R32G32B32_FLOAT: return openscarlett::VulkanVertexFormat::R32G32B32Sfloat;
        case DXGI_FORMAT_R32G32B32A32_UINT: return openscarlett::VulkanVertexFormat::R32G32B32A32Uint;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        default:
            return openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat;
    }
}

uint32_t DxgiVertexFormatBytes(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_FLOAT: return 4u;
        case DXGI_FORMAT_R32G32_FLOAT: return 8u;
        case DXGI_FORMAT_R32G32B32_FLOAT: return 12u;
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        default:
            return 16u;
    }
}

openscarlett::VulkanImageFormat DxgiToVulkanColorFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return openscarlett::VulkanImageFormat::Bgra8Unorm;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return openscarlett::VulkanImageFormat::Rgba16Sfloat;
        default:
            return openscarlett::VulkanImageFormat::Rgba8Unorm;
    }
}

struct FormatFootprintSpec {
    uint32_t block_width = 1u;
    uint32_t block_height = 1u;
    uint32_t bytes_per_block = 4u;
};

FormatFootprintSpec DescribeFormatForFootprint(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return {4u, 4u, 8u};
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
            return {4u, 4u, 16u};
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return {1u, 1u, 16u};
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
            return {1u, 1u, 12u};
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
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            return {1u, 1u, 8u};
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
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            return {1u, 1u, 4u};
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
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return {1u, 1u, 2u};
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            return {1u, 1u, 1u};
        case DXGI_FORMAT_UNKNOWN:
            return {1u, 1u, 1u};
        default:
            // Safe 32-bit fallback for exotic/Xbox-specific DXGI formats refused by desktop D3D12.
            return {1u, 1u, 4u};
    }
}

void ComputeFallbackCopyableFootprints(
    const D3D12_RESOURCE_DESC& desc,
    UINT first_subresource,
    UINT num_subresources,
    UINT64 base_offset,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
    UINT* pNumRows,
    UINT64* pRowSizeInBytes,
    UINT64* pTotalBytes) noexcept {
    const uint32_t mip_levels = (desc.MipLevels > 0u) ? desc.MipLevels : 1u;
    const FormatFootprintSpec spec = DescribeFormatForFootprint(desc.Format);
    uint64_t total = 0ull;

    for (UINT i = 0u; i < num_subresources; ++i) {
        if (i > 0u) {
            total = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1ull) &
                    ~(static_cast<uint64_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1ull);
        }
        const UINT sub = first_subresource + i;
        const uint32_t mip = sub % mip_levels;

        uint64_t mip_w = 1ull;
        uint32_t mip_h = 1u;
        uint32_t mip_d = 1u;
        uint64_t row_bytes = 0ull;
        uint32_t num_rows = 1u;
        uint32_t row_pitch = 0u;

        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            mip_w = std::max<uint64_t>(1ull, desc.Width);
            mip_h = 1u;
            mip_d = 1u;
            row_bytes = mip_w;
            num_rows = 1u;
            row_pitch = static_cast<uint32_t>(
                (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1ull) &
                ~(static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1ull));
        } else {
            mip_w = std::max<uint64_t>(1ull, desc.Width >> mip);
            mip_h = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
                        ? 1u
                        : std::max<uint32_t>(1u, desc.Height >> mip);
            mip_d = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                        ? std::max<uint32_t>(1u, static_cast<uint32_t>(desc.DepthOrArraySize) >> mip)
                        : 1u;

            const uint64_t blocks_x = (mip_w + spec.block_width - 1ull) / spec.block_width;
            const uint32_t blocks_y = (mip_h + spec.block_height - 1u) / spec.block_height;
            row_bytes = std::max<uint64_t>(1ull, blocks_x) * spec.bytes_per_block;
            num_rows = std::max<uint32_t>(1u, blocks_y);
            row_pitch = static_cast<uint32_t>(
                (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1ull) &
                ~(static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1ull));
        }

        if (pLayouts) {
            pLayouts[i].Offset = base_offset + total;
            pLayouts[i].Footprint.Format = desc.Format;
            pLayouts[i].Footprint.Width = static_cast<UINT>(mip_w);
            pLayouts[i].Footprint.Height = mip_h;
            pLayouts[i].Footprint.Depth = mip_d;
            pLayouts[i].Footprint.RowPitch = row_pitch;
        }
        if (pNumRows) {
            pNumRows[i] = num_rows;
        }
        if (pRowSizeInBytes) {
            pRowSizeInBytes[i] = row_bytes;
        }

        const uint64_t sub_bytes =
            (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                ? row_bytes
                : (static_cast<uint64_t>(row_pitch) * static_cast<uint64_t>(num_rows) *
                   static_cast<uint64_t>(mip_d));
        total += sub_bytes;
    }

    if (pTotalBytes) {
        *pTotalBytes = total;
    }
}

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
        dst[i] = CommandQueueX::UnwrapHostPageable(UnwrapProxyPageable(src[i]));
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
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device1), reinterpret_cast<void**>(&host_dev1_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device2), reinterpret_cast<void**>(&host_dev2_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device3), reinterpret_cast<void**>(&host_dev3_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device4), reinterpret_cast<void**>(&host_dev4_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device5), reinterpret_cast<void**>(&host_dev5_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device6), reinterpret_cast<void**>(&host_dev6_));

        D3D12_FEATURE_DATA_FORMAT_SUPPORT fmt_sup{};
        fmt_sup.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        if (SUCCEEDED(host_dev_->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &fmt_sup, sizeof(fmt_sup)))) {
            supports_r9g9b9e5_uav_ =
                ((fmt_sup.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0) &&
                ((fmt_sup.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0);
        }
        adapter_info_.supports_r9g9b9e5_uav = supports_r9g9b9e5_uav_;

        D3D12_FEATURE_DATA_D3D12_OPTIONS2 opts2{};
        if (SUCCEEDED(host_dev_->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS2, &opts2, sizeof(opts2)))) {
            supports_depth_bounds_test_ = (opts2.DepthBoundsTestSupported != FALSE);
            supports_programmable_sample_positions_ =
                (opts2.ProgrammableSamplePositionsTier !=
                 D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS3 opts3{};
        if (SUCCEEDED(host_dev_->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS3, &opts3, sizeof(opts3)))) {
            supports_view_instancing_ =
                (opts3.ViewInstancingTier != D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED);
        }

        // Wire Worker 2 bindless descriptor bridge & Worker 6 per-draw capture (zero-overhead when disabled).
        (void)descriptor_bridge_.InitializeD3D12(host_dev_, 4096u, 256u);
        (void)openscarlett::FrameCaptureRecorder::Global().InitializeFromEnv();
    }

    const uint32_t resolved_mode = ResolveEffectiveBackendMode();
    vulkan_routing_active_ = (resolved_mode == D3D12XBOX_BACKEND_VULKAN);
    if (vulkan_routing_active_) {
        vulkan_backend_ = std::make_unique<openscarlett::VulkanBackend>();
        const bool force_sw =
            EnvVarEqualsIgnoreCase("OPENSCARLETT_VK_FORCE_SOFTWARE", "1") ||
            EnvVarEqualsIgnoreCase("OPENSCARLETT_VK_FORCE_SOFTWARE", "true");
        if (force_sw) {
            vulkan_backend_->InitializeSoftwareFallback();
        } else {
            vulkan_backend_->Initialize(/*allow_software_fallback=*/true);
        }
    }
}

void DeviceX::SetGlobalBackendMode(uint32_t backend_mode) noexcept {
    if (backend_mode > D3D12XBOX_BACKEND_VULKAN) {
        backend_mode = D3D12XBOX_BACKEND_AUTO;
    }
    g_backend_mode.store(backend_mode, std::memory_order_release);
}

uint32_t DeviceX::ResolveEffectiveBackendMode() noexcept {
    const uint32_t explicit_mode = g_backend_mode.load(std::memory_order_acquire);
    if (explicit_mode == D3D12XBOX_BACKEND_VULKAN) return D3D12XBOX_BACKEND_VULKAN;
    if (explicit_mode == D3D12XBOX_BACKEND_D3D12) return D3D12XBOX_BACKEND_D3D12;
    if (EnvVarEqualsIgnoreCase("OPENSCARLETT_BACKEND", "vulkan") ||
        EnvVarEqualsIgnoreCase("OPENSCARLETT_BACKEND", "vk") ||
        EnvVarEqualsIgnoreCase("OPENSCARLETT_GPU_BACKEND", "vulkan") ||
        EnvVarEqualsIgnoreCase("OPENSCARLETT_GPU_BACKEND", "vk")) {
        return D3D12XBOX_BACKEND_VULKAN;
    }
    return D3D12XBOX_BACKEND_D3D12;
}

uint32_t STDMETHODCALLTYPE DeviceX::GetActiveBackendMode() {
    return vulkan_routing_active_ ? D3D12XBOX_BACKEND_VULKAN : D3D12XBOX_BACKEND_D3D12;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetVulkanRoutingStats(
    D3D12XBOX_VULKAN_ROUTING_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    std::memset(out_stats, 0, sizeof(*out_stats));
    out_stats->vulkan_routing_active = vulkan_routing_active_ ? 1u : 0u;
    if (vulkan_backend_ && vulkan_backend_->IsInitialized()) {
        const auto& info = vulkan_backend_->GetDeviceInfo();
        out_stats->executed_on_vulkan_icd = vulkan_backend_->is_fallback_mode() ? 0u : 1u;
        out_stats->vulkan_software_fallback = vulkan_backend_->is_fallback_mode() ? 1u : 0u;
        out_stats->vulkan_api_major = info.api_major;
        out_stats->vulkan_api_minor = info.api_minor;
        out_stats->vulkan_api_patch = info.api_patch;
        out_stats->subgroup_size = info.subgroup_size;
        out_stats->max_push_constants_size = info.max_push_constants_size;
        const size_t n = std::min(info.device_name.size(), sizeof(out_stats->vulkan_device_name) - 1u);
        std::memcpy(out_stats->vulkan_device_name, info.device_name.data(), n);
        out_stats->vulkan_device_name[n] = '\0';
    }
    out_stats->routed_command_lists = vk_routed_cmd_lists_.load(std::memory_order_relaxed);
    out_stats->routed_draws = vk_routed_draws_.load(std::memory_order_relaxed);
    out_stats->routed_indexed_draws = vk_routed_indexed_draws_.load(std::memory_order_relaxed);
    out_stats->routed_dispatches = vk_routed_dispatches_.load(std::memory_order_relaxed);
    out_stats->routed_mesh_dispatches = vk_routed_mesh_dispatches_.load(std::memory_order_relaxed);
    out_stats->routed_copies = vk_routed_copies_.load(std::memory_order_relaxed);
    out_stats->routed_barriers = vk_routed_barriers_.load(std::memory_order_relaxed);
    out_stats->routed_rtv_clears = vk_routed_rtv_clears_.load(std::memory_order_relaxed);
    out_stats->routed_dsv_clears = vk_routed_dsv_clears_.load(std::memory_order_relaxed);
    out_stats->routed_presents = vk_routed_presents_.load(std::memory_order_relaxed);
    out_stats->vulkan_icd_executions = vk_icd_executions_.load(std::memory_order_relaxed);
    out_stats->spirv_direct_draws = vk_spirv_direct_draws_.load(std::memory_order_relaxed);
    out_stats->spirv_direct_dispatches = vk_spirv_direct_dispatches_.load(std::memory_order_relaxed);
    out_stats->spirv_direct_mesh_dispatches = vk_spirv_direct_mesh_dispatches_.load(std::memory_order_relaxed);
    return S_OK;
}

void DeviceX::NotifyResourceDestroyed(ID3D12Resource* host_resource) noexcept {
    if (!host_resource) return;
    {
        std::lock_guard<std::mutex> lock(fallback_mutex_);
        r9g9b9e5_fallback_resources_.erase(host_resource);
    }
    if (vulkan_routing_active_) {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        vk_resources_.erase(host_resource);
        vk_rt_framebuffers_.erase(host_resource);
    }
}

void DeviceX::NotifyHeapDestroyed(ID3D12Heap* host_heap) noexcept {
    if (!host_heap) return;
    std::lock_guard<std::mutex> lock(unified_mutex_);
    address_heaps_.erase(host_heap);
}

void DeviceX::NotifyPipelineDestroyed(ID3D12PipelineState* pso_key) noexcept {
    if (!pso_key || !vulkan_routing_active_) return;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    vk_pipelines_.erase(pso_key);
}

void DeviceX::SetGuestGpuVaTranslatorHook(GuestGpuVaTranslatorFn fn, void* user_ctx) noexcept {
    va_translator_ctx_.store(user_ctx, std::memory_order_release);
    va_translator_hook_.store(fn, std::memory_order_release);
}

void DeviceX::SetDescriptorCreateHook(DescriptorCreateHookFn fn, void* user_ctx) noexcept {
    descriptor_create_ctx_.store(user_ctx, std::memory_order_release);
    descriptor_create_hook_.store(fn, std::memory_order_release);
}

void DeviceX::TrackVulkanResource(
    IUnknown* obj, D3D12_HEAP_TYPE heap_type, const D3D12_RESOURCE_DESC& desc) {
    if (!vulkan_routing_active_ || !obj) return;
    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(obj->QueryInterface(OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&res))) && res) {
        ID3D12Resource* host_res = UnwrapResource(res);
        VulkanTrackedResource tr{};
        tr.resource = host_res;
        tr.heap_type = heap_type;
        tr.desc = desc;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && host_res) {
            tr.gpu_va = host_res->GetGPUVirtualAddress();
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_resources_[host_res] = std::move(tr);
        }
        res->Release();
    }
}

bool DeviceX::LookupVulkanPipeline(ID3D12PipelineState* pso, VulkanPipelineInfo& out_info) const {
    if (!vulkan_routing_active_ || !pso) return false;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_pipelines_.find(pso);
    if (it == vk_pipelines_.end()) return false;
    out_info = it->second;
    return true;
}

bool DeviceX::IsSpirvDirectPipeline(ID3D12PipelineState* pso) const {
    if (!vulkan_routing_active_ || !pso) return false;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_pipelines_.find(pso);
    return (it != vk_pipelines_.end()) && it->second.is_spirv_direct;
}

ID3D12Resource* DeviceX::ResolveVulkanRtvResource(D3D12_CPU_DESCRIPTOR_HANDLE rtv) const {
    if (!vulkan_routing_active_ || rtv.ptr == 0u) return nullptr;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_rtv_map_.find(rtv.ptr);
    return (it != vk_rtv_map_.end()) ? it->second : nullptr;
}

ID3D12Resource* DeviceX::ResolveVulkanDsvResource(D3D12_CPU_DESCRIPTOR_HANDLE dsv) const {
    if (!vulkan_routing_active_ || dsv.ptr == 0u) return nullptr;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_dsv_map_.find(dsv.ptr);
    return (it != vk_dsv_map_.end()) ? it->second : nullptr;
}

ID3D12Resource* DeviceX::ResolveVulkanBufferByGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t* out_offset_bytes) const {
    if (!vulkan_routing_active_ || va == 0ull) return nullptr;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    for (const auto& [res, tr] : vk_resources_) {
        if (tr.gpu_va != 0ull && tr.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (va >= tr.gpu_va && va < tr.gpu_va + tr.desc.Width) {
                if (out_offset_bytes) *out_offset_bytes = va - tr.gpu_va;
                return res;
            }
        }
    }
    return nullptr;
}

bool DeviceX::ReadVulkanBufferData(
    ID3D12Resource* res, uint64_t offset, uint64_t size, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    res = UnwrapResource(res);
    if (!res) return false;
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
    uint64_t res_width = res->GetDesc().Width;
    {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        auto it = vk_resources_.find(res);
        if (it != vk_resources_.end()) {
            heap_type = it->second.heap_type;
            if (!it->second.shadow_bytes.empty()) {
                const uint64_t avail = (offset < it->second.shadow_bytes.size())
                                           ? (it->second.shadow_bytes.size() - offset)
                                           : 0ull;
                const uint64_t to_copy = (size == 0ull) ? avail : std::min<uint64_t>(size, avail);
                out_bytes.assign(it->second.shadow_bytes.begin() + static_cast<ptrdiff_t>(offset),
                                 it->second.shadow_bytes.begin() + static_cast<ptrdiff_t>(offset + to_copy));
                return !out_bytes.empty();
            }
        }
    }
    if (offset >= res_width) return false;
    const uint64_t avail = res_width - offset;
    const uint64_t to_copy = (size == 0ull) ? avail : std::min<uint64_t>(size, avail);
    if (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK) {
        void* mapped = nullptr;
        D3D12_RANGE range{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + to_copy)};
        if (SUCCEEDED(res->Map(0u, &range, &mapped)) && mapped) {
            out_bytes.resize(static_cast<size_t>(to_copy));
            std::memcpy(out_bytes.data(), static_cast<const uint8_t*>(mapped) + offset, static_cast<size_t>(to_copy));
            D3D12_RANGE write_none{0u, 0u};
            res->Unmap(0u, &write_none);
            return true;
        }
    }
    return false;
}

bool DeviceX::WriteVulkanBufferData(
    ID3D12Resource* res, uint64_t offset, const void* data, size_t size) {
    res = UnwrapResource(res);
    if (!res || !data || size == 0u) return false;
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
    uint64_t res_width = res->GetDesc().Width;
    {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        auto& tr = vk_resources_[res];
        tr.resource = res;
        heap_type = tr.heap_type;
        if (tr.shadow_bytes.size() < offset + size) {
            tr.shadow_bytes.resize(static_cast<size_t>(offset + size), 0u);
        }
        std::memcpy(tr.shadow_bytes.data() + offset, data, size);
    }
    if (offset + size <= res_width &&
        (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK)) {
        void* mapped = nullptr;
        D3D12_RANGE read_none{0u, 0u};
        if (SUCCEEDED(res->Map(0u, &read_none, &mapped)) && mapped) {
            std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
            D3D12_RANGE written{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + size)};
            res->Unmap(0u, &written);
        }
    }
    return true;
}

void DeviceX::StoreVulkanRenderTargetFramebuffer(
    ID3D12Resource* rt_res, const openscarlett::FrameBufferRgba8& fb) {
    rt_res = UnwrapResource(rt_res);
    if (!vulkan_routing_active_ || !rt_res) return;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    vk_rt_framebuffers_[rt_res] = fb;
}

bool DeviceX::TryGetVulkanRenderTargetFramebuffer(
    ID3D12Resource* rt_res, openscarlett::FrameBufferRgba8& out_fb) const {
    rt_res = UnwrapResource(rt_res);
    if (!vulkan_routing_active_ || !rt_res) return false;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_rt_framebuffers_.find(rt_res);
    if (it == vk_rt_framebuffers_.end() || it->second.pixels.empty()) return false;
    out_fb = it->second;
    return true;
}

void DeviceX::RecordVulkanRoutingCounters(
    uint64_t cmd_lists,
    uint64_t draws,
    uint64_t indexed_draws,
    uint64_t dispatches,
    uint64_t mesh_dispatches,
    uint64_t copies,
    uint64_t barriers,
    uint64_t rtv_clears,
    uint64_t dsv_clears,
    uint64_t presents,
    uint64_t icd_execs,
    uint64_t spirv_draws,
    uint64_t spirv_dispatches,
    uint64_t spirv_mesh_dispatches) noexcept {
    if (cmd_lists) vk_routed_cmd_lists_.fetch_add(cmd_lists, std::memory_order_relaxed);
    if (draws) vk_routed_draws_.fetch_add(draws, std::memory_order_relaxed);
    if (indexed_draws) vk_routed_indexed_draws_.fetch_add(indexed_draws, std::memory_order_relaxed);
    if (dispatches) vk_routed_dispatches_.fetch_add(dispatches, std::memory_order_relaxed);
    if (mesh_dispatches) vk_routed_mesh_dispatches_.fetch_add(mesh_dispatches, std::memory_order_relaxed);
    if (copies) vk_routed_copies_.fetch_add(copies, std::memory_order_relaxed);
    if (barriers) vk_routed_barriers_.fetch_add(barriers, std::memory_order_relaxed);
    if (rtv_clears) vk_routed_rtv_clears_.fetch_add(rtv_clears, std::memory_order_relaxed);
    if (dsv_clears) vk_routed_dsv_clears_.fetch_add(dsv_clears, std::memory_order_relaxed);
    if (presents) vk_routed_presents_.fetch_add(presents, std::memory_order_relaxed);
    if (icd_execs) vk_icd_executions_.fetch_add(icd_execs, std::memory_order_relaxed);
    if (spirv_draws) vk_spirv_direct_draws_.fetch_add(spirv_draws, std::memory_order_relaxed);
    if (spirv_dispatches) vk_spirv_direct_dispatches_.fetch_add(spirv_dispatches, std::memory_order_relaxed);
    if (spirv_mesh_dispatches) vk_spirv_direct_mesh_dispatches_.fetch_add(spirv_mesh_dispatches, std::memory_order_relaxed);
}

bool DeviceX::IsR9G9B9E5UavFallbackActive() const noexcept {
    return !supports_r9g9b9e5_uav_ ||
           g_force_r9g9b9e5_uav_fallback.load(std::memory_order_acquire);
}

void DeviceX::SetForceR9G9B9E5UavFallback(bool force) noexcept {
    g_force_r9g9b9e5_uav_fallback.store(force, std::memory_order_release);
}

bool DeviceX::ShouldRewriteR9G9B9E5Uav(const D3D12_RESOURCE_DESC& desc) const noexcept {
    return IsR9G9B9E5UavFallbackActive() &&
           desc.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
           (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
}

void DeviceX::TrackFallbackResource(IUnknown* obj, bool is_fallback) {
    if (!obj) return;
    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(obj->QueryInterface(OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&res))) && res) {
        ID3D12Resource* host_res = UnwrapResource(res);
        {
            std::lock_guard<std::mutex> lock(fallback_mutex_);
            if (is_fallback) {
                r9g9b9e5_fallback_resources_.insert(host_res);
            } else {
                r9g9b9e5_fallback_resources_.erase(host_res);
            }
        }
        res->Release();
    }
}

bool DeviceX::IsTrackedFallbackResource(ID3D12Resource* resource) const noexcept {
    resource = UnwrapResource(resource);
    if (!resource) return false;
    std::lock_guard<std::mutex> lock(fallback_mutex_);
    return r9g9b9e5_fallback_resources_.find(resource) != r9g9b9e5_fallback_resources_.end();
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::TranslateGuestGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS address) const noexcept {
    if (address == 0ull) return 0ull;
    if (const auto hook = va_translator_hook_.load(std::memory_order_acquire)) {
        const D3D12_GPU_VIRTUAL_ADDRESS hooked =
            hook(address, va_translator_ctx_.load(std::memory_order_acquire));
        if (hooked != address && hooked != 0ull) {
            return hooked;
        }
    }
    {
        uint64_t mirrored_va = 0ull;
        uint64_t mirrored_off = 0ull;
        if (title_va_mirror_.ResolveTitleVa(address, mirrored_va, mirrored_off) && mirrored_va != 0ull) {
            return mirrored_va;
        }
    }
    if (xmem_allocation_count_.load(std::memory_order_acquire) == 0u) {
        return address;
    }
    std::lock_guard<std::mutex> lock(unified_mutex_);
    for (const auto& rec : xmem_allocations_) {
        if (address >= rec.guest_va && address < rec.guest_va + rec.aligned_size) {
            return rec.desktop_gpu_va + (address - rec.guest_va);
        }
    }
    return address;
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

    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device)) ||
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
    if (IsEqualIID(riid, IID_IOpenScarlettVulkanRouting)) {
        *ppvObject = static_cast<IOpenScarlettVulkanRouting*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device1)) && host_dev1_) {
        *ppvObject = static_cast<ID3D12Device1*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device2)) && host_dev2_) {
        *ppvObject = static_cast<ID3D12Device2*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device3)) && host_dev3_) {
        *ppvObject = static_cast<ID3D12Device3*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device4)) && host_dev4_) {
        *ppvObject = static_cast<ID3D12Device4*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device5)) && host_dev5_) {
        *ppvObject = static_cast<ID3D12Device5*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device6)) && host_dev6_) {
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
    if (!ppCommandAllocator) {
        const HRESULT hr = host_dev_->CreateCommandAllocator(type, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, hr);
        return hr;
    }
    *ppCommandAllocator = nullptr;
    ID3D12CommandAllocator* host_alloc = nullptr;
    HRESULT hr = host_dev_->CreateCommandAllocator(
        type, OS_D3D12_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(&host_alloc));
    if (FAILED(hr) || !host_alloc) {
        RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) CommandAllocatorXProxy(this, host_alloc, type);
    host_alloc->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppCommandAllocator);
    proxy->Release();
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

    // Direct SPIR-V graphics PSO path when Vulkan routing is active.
    if (vulkan_routing_active_ &&
        IsRawSpirvBytecode(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength) &&
        IsRawSpirvBytecode(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength)) {
        if (!ppPipelineState) {
            RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_POINTER);
            return E_POINTER;
        }
        *ppPipelineState = nullptr;
        VulkanPipelineInfo pi{};
        pi.is_spirv_direct = true;
        pi.is_compute = false;
        pi.is_mesh = false;
        pi.vs_spirv = CopySpirvWords(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
        pi.ps_spirv = CopySpirvWords(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
        openscarlett::SpirvValidationInfo vs_val{}, ps_val{};
        if (!openscarlett::VulkanBackend::ValidateSpirvBinary(pi.vs_spirv, &vs_val).ok() ||
            !openscarlett::VulkanBackend::ValidateSpirvBinary(pi.ps_spirv, &ps_val).ok()) {
            RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_INVALIDARG);
            return E_INVALIDARG;
        }
        if (!vs_val.entry_point_name.empty()) pi.vs_entry = vs_val.entry_point_name;
        if (!ps_val.entry_point_name.empty()) pi.ps_entry = ps_val.entry_point_name;
        pi.enable_depth = (pDesc->DepthStencilState.DepthEnable != FALSE);
        pi.depth_write = (pDesc->DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO);
        if (pDesc->NumRenderTargets > 0u) {
            pi.color_format = DxgiToVulkanColorFormat(pDesc->RTVFormats[0]);
        }
        uint32_t running_off = 0u;
        for (UINT i = 0; i < pDesc->InputLayout.NumElements; ++i) {
            const auto& el = pDesc->InputLayout.pInputElementDescs[i];
            const uint32_t off = (el.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT)
                                     ? running_off
                                     : el.AlignedByteOffset;
            openscarlett::VulkanVertexAttribute attr{};
            attr.location = i;
            attr.binding = el.InputSlot;
            attr.format = DxgiToVulkanVertexFormat(el.Format);
            attr.offset = off;
            pi.vertex_attrs.push_back(attr);
            running_off = off + DxgiVertexFormatBytes(el.Format);
        }
        pi.vertex_stride_hint = running_off;

        auto* vk_pso = new (std::nothrow) PipelineStateXProxy(this, nullptr);
        if (!vk_pso) {
            RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(vk_pso)] = std::move(pi);
        }
        const HRESULT hr = vk_pso->QueryInterface(riid, ppPipelineState);
        vk_pso->Release();
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }

    const bool lift_vs = openscarlett::IsXboxShaderBytecode(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
    const bool lift_ps = openscarlett::IsXboxShaderBytecode(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
    const bool lift_ds = openscarlett::IsXboxShaderBytecode(pDesc->DS.pShaderBytecode, pDesc->DS.BytecodeLength);
    const bool lift_hs = openscarlett::IsXboxShaderBytecode(pDesc->HS.pShaderBytecode, pDesc->HS.BytecodeLength);
    const bool lift_gs = openscarlett::IsXboxShaderBytecode(pDesc->GS.pShaderBytecode, pDesc->GS.BytecodeLength);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lifted_desc = *pDesc;
    lifted_desc.pRootSignature = UnwrapRootSignature(pDesc->pRootSignature);
    std::deque<std::vector<uint8_t>> lifted_blobs;

    if (lift_vs || lift_ps || lift_ds || lift_hs || lift_gs) {
        openscarlett::ShaderCacheLiftRequest req{};
        req.highest_shader_model = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
        req.mesh_has_payload = false;
        req.prefer_dxil_standin = (req.highest_shader_model >= 0x60u);

        auto lift_stage = [&](D3D12_SHADER_BYTECODE& bc, bool do_lift) -> bool {
            if (!do_lift) return true;
            openscarlett::CachedShaderResult res{};
            std::string diag;
            openscarlett::Status st;
            {
                std::lock_guard<std::mutex> lift_lock(shader_lift_mutex_);
                st = shader_cache_.GetOrLiftXboxShader(bc.pShaderBytecode, bc.BytecodeLength, req, res, &diag);
            }
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
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev_->CreateGraphicsPipelineState(&lifted_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = host_dev_->CreateGraphicsPipelineState(
        &lifted_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    if (FAILED(hr) || !host_pso) {
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) PipelineStateXProxy(this, host_pso);
    host_pso->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppPipelineState);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateComputePipelineState(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    if (vulkan_routing_active_ && pDesc &&
        IsRawSpirvBytecode(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength)) {
        if (!ppPipelineState) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_POINTER);
            return E_POINTER;
        }
        *ppPipelineState = nullptr;
        VulkanPipelineInfo pi{};
        pi.is_spirv_direct = true;
        pi.is_compute = true;
        pi.cs_spirv = CopySpirvWords(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength);
        openscarlett::SpirvValidationInfo cs_val{};
        if (!openscarlett::VulkanBackend::ValidateSpirvBinary(pi.cs_spirv, &cs_val).ok() ||
            !cs_val.has_compute_entry) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_INVALIDARG);
            return E_INVALIDARG;
        }
        if (!cs_val.entry_point_name.empty()) pi.cs_entry = cs_val.entry_point_name;

        auto* vk_pso = new (std::nothrow) PipelineStateXProxy(this, nullptr);
        if (!vk_pso) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(vk_pso)] = std::move(pi);
        }
        const HRESULT hr = vk_pso->QueryInterface(riid, ppPipelineState);
        vk_pso->Release();
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }

    if (!pDesc) {
        const HRESULT hr = host_dev_->CreateComputePipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC lifted_desc = *pDesc;
    lifted_desc.pRootSignature = UnwrapRootSignature(pDesc->pRootSignature);
    openscarlett::CachedShaderResult lifted{};

    if (openscarlett::IsXboxShaderBytecode(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength)) {
        openscarlett::ShaderCacheLiftRequest req{};
        req.highest_shader_model = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
        req.mesh_has_payload = false;
        req.prefer_dxil_standin = false;  // Single-stage CS can use <1ms SM 5.0 DXBC stand-in for instant async PSO

        std::string diag;
        openscarlett::Status st;
        {
            std::lock_guard<std::mutex> lift_lock(shader_lift_mutex_);
            st = shader_cache_.GetOrLiftXboxShader(
                pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength, req, lifted, &diag);
        }
        if (!st.ok()) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_INVALIDARG);
            return E_INVALIDARG;
        }
        lifted_desc.CS.pShaderBytecode = lifted.dxil.data();
        lifted_desc.CS.BytecodeLength = lifted.dxil.size();
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev_->CreateComputePipelineState(&lifted_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = host_dev_->CreateComputePipelineState(
        &lifted_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    if (FAILED(hr) || !host_pso) {
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) PipelineStateXProxy(this, host_pso);
    host_pso->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppPipelineState);
    proxy->Release();
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
    const bool initial_is_spirv =
        vulkan_routing_active_ && IsSpirvDirectPipeline(pInitialState);
    ID3D12PipelineState* host_initial =
        initial_is_spirv ? nullptr : UnwrapPipelineState(pInitialState);
    ID3D12GraphicsCommandList* host_cl = nullptr;
    HRESULT hr = host_dev_->CreateCommandList(
        nodeMask, type, UnwrapCommandAllocator(pCommandAllocator), host_initial,
        OS_D3D12_IID(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&host_cl));
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
    if (initial_is_spirv) {
        wrapper->SetPipelineState(pInitialState);
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
    if (!ppvHeap) {
        const HRESULT hr = host_dev_->CreateDescriptorHeap(pDescriptorHeapDesc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12DescriptorHeap* host_heap = nullptr;
    HRESULT hr = host_dev_->CreateDescriptorHeap(
        pDescriptorHeapDesc, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) DescriptorHeapXProxy(this, host_heap);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
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
    if (!ppvRootSignature) {
        const HRESULT hr = host_dev_->CreateRootSignature(
            nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
        return hr;
    }
    *ppvRootSignature = nullptr;
    ID3D12RootSignature* host_rs = nullptr;
    HRESULT hr = host_dev_->CreateRootSignature(
        nodeMask, pBlobWithRootSignature, blobLengthInBytes,
        OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
    if (FAILED(hr) || !host_rs) {
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) RootSignatureXProxy(this, host_rs);
    host_rs->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvRootSignature);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::CreateConstantBufferView(
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC translated{};
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc_ptr = pDesc;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_va = pDesc ? pDesc->BufferLocation : 0ull;
    if (pDesc && pDesc->BufferLocation != 0ull) {
        const D3D12_GPU_VIRTUAL_ADDRESS mapped_va = TranslateGuestGpuVa(pDesc->BufferLocation);
        if (mapped_va != pDesc->BufferLocation) {
            translated = *pDesc;
            translated.BufferLocation = mapped_va;
            desc_ptr = &translated;
            gpu_va = mapped_va;
        }
    }
    host_dev_->CreateConstantBufferView(desc_ptr, DestDescriptor);
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DestDescriptor, nullptr, gpu_va,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateConstantBufferView", 17, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateShaderResourceView(
    ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    const auto notify_hook = [&]() {
        if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
            const D3D12_GPU_VIRTUAL_ADDRESS va =
                (host_res && host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                    ? host_res->GetGPUVirtualAddress()
                    : 0ull;
            hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DestDescriptor, host_res, va,
                 descriptor_create_ctx_.load(std::memory_order_acquire));
        }
    };
    // R9G9B9E5_SHAREDEXP UAV fallback: rewrite explicit R9G9B9E5 SRV views to R16G16B16A16_FLOAT
    // when the backing resource was promoted to RGBA16F.
    if (pDesc != nullptr && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        host_res != nullptr &&
        (IsTrackedFallbackResource(host_res) ||
         host_res->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC rw_desc = *pDesc;
        rw_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        host_dev_->CreateShaderResourceView(host_res, &rw_desc, DestDescriptor);
        notify_hook();
        RecordDev("ID3D12DeviceX::CreateShaderResourceView[R9G9B9E5Fallback]", 18, S_OK);
        RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
        return;
    }
    // ScarlettX #46: Preserve array slices on default (nullptr desc) texture SRVs.
    if (pDesc == nullptr && host_res != nullptr) {
        const D3D12_RESOURCE_DESC res_desc = host_res->GetDesc();
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
            host_dev_->CreateShaderResourceView(host_res, &explicit_desc, DestDescriptor);
            notify_hook();
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
            host_dev_->CreateShaderResourceView(host_res, &explicit_desc, DestDescriptor);
            notify_hook();
            RecordDev("ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]", 18, S_OK);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
            return;
        }
    }
    host_dev_->CreateShaderResourceView(host_res, pDesc, DestDescriptor);
    notify_hook();
    RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateUnorderedAccessView(
    ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    ID3D12Resource* host_counter = UnwrapResource(pCounterResource);
    D3D12_UNORDERED_ACCESS_VIEW_DESC rw_desc{};
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc_ptr = pDesc;
    if (pDesc != nullptr && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        (IsR9G9B9E5UavFallbackActive() ||
         (host_res != nullptr &&
          (IsTrackedFallbackResource(host_res) ||
           host_res->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)))) {
        rw_desc = *pDesc;
        rw_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc_ptr = &rw_desc;
        RecordDev("ID3D12DeviceX::CreateUnorderedAccessView[R9G9B9E5Fallback]", 19, S_OK);
    }
    host_dev_->CreateUnorderedAccessView(host_res, host_counter, desc_ptr, DestDescriptor);
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        const D3D12_GPU_VIRTUAL_ADDRESS va =
            (host_res && host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                ? host_res->GetGPUVirtualAddress()
                : 0ull;
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DestDescriptor, host_res, va,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateUnorderedAccessView", 19, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateRenderTargetView(
    ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    D3D12_RENDER_TARGET_VIEW_DESC rw_desc{};
    const D3D12_RENDER_TARGET_VIEW_DESC* desc_ptr = pDesc;
    if (pDesc != nullptr && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        host_res != nullptr &&
        (IsTrackedFallbackResource(host_res) ||
         host_res->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        rw_desc = *pDesc;
        rw_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc_ptr = &rw_desc;
    }
    host_dev_->CreateRenderTargetView(host_res, desc_ptr, DestDescriptor);
    if (vulkan_routing_active_ && host_res) {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        vk_rtv_map_[DestDescriptor.ptr] = host_res;
    }
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, DestDescriptor, host_res, 0ull,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateRenderTargetView", 20, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateDepthStencilView(
    ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    host_dev_->CreateDepthStencilView(host_res, pDesc, DestDescriptor);
    if (vulkan_routing_active_ && host_res) {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        vk_dsv_map_[DestDescriptor.ptr] = host_res;
    }
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DestDescriptor, host_res, 0ull,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateDepthStencilView", 21, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateSampler(
    const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateSampler(pDesc, DestDescriptor);
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, DestDescriptor, nullptr, 0ull,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
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
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    D3D12_RESOURCE_ALLOCATION_INFO* info =
        host_dev_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, descs_ptr);
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
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        host_dev_->GetResourceAllocationInfo(visibleMask, numResourceDescs, descs_ptr);
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
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    const D3D12_HEAP_FLAGS clean_heap_flags = SanitizeXboxHeapFlags(HeapFlags);
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreateCommittedResource(
            pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, riidResource, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev_->CreateCommittedResource(
        pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
        clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (vulkan_routing_active_ && pHeapProperties && desc_ptr) {
        TrackVulkanResource(host_res, pHeapProperties->Type, *desc_ptr);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 27, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, pHeapProperties, clean_heap_flags, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riidResource, ppvResource);
    proxy->Release();
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
    if (!ppvHeap) {
        const HRESULT hr = host_dev_->CreateHeap(heap_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev_->CreateHeap(
        heap_ptr, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, 0ull);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateHeap", 28, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreatePlacedResource(
    ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource) {
    ID3D12Heap* host_heap = UnwrapHeap(pHeap);
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreatePlacedResource(
            host_heap, HeapOffset, desc_ptr, InitialState, clear_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev_->CreatePlacedResource(
        host_heap, HeapOffset, desc_ptr, InitialState, clear_ptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 29, S_OK);
    }
    uint64_t tracked_va = 0ull;
    if (host_heap && desc_ptr) {
        if (vulkan_routing_active_) {
            const D3D12_HEAP_DESC hd = host_heap->GetDesc();
            TrackVulkanResource(host_res, hd.Properties.Type, *desc_ptr);
        }
        uint64_t heap_base_va = 0ull;
        {
            std::lock_guard<std::mutex> lock(unified_mutex_);
            auto it = address_heaps_.find(host_heap);
            if (it != address_heaps_.end()) {
                heap_base_va = it->second;
            }
        }
        if (heap_base_va != 0ull) {
            tracked_va = heap_base_va + HeapOffset;
            AutoRegisterUnifiedResource(host_res, tracked_va, *desc_ptr, InitialState);
        }
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, nullptr, D3D12_HEAP_FLAG_NONE, tracked_va);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateReservedResource(
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreateReservedResource(
            desc_ptr, InitialState, clear_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateReservedResource", 30, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev_->CreateReservedResource(
        desc_ptr, InitialState, clear_ptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateReservedResource", 30, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 30, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, nullptr, D3D12_HEAP_FLAG_NONE, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateReservedResource", 30, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateReservedResource", 30, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateSharedHandle(
    ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
    DWORD Access, LPCWSTR Name, HANDLE* pHandle) {
    const HRESULT hr = host_dev_->CreateSharedHandle(
        UnwrapProxyDeviceChild(pObject), pAttributes, Access, Name, pHandle);
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
    if (!ppFence) {
        const HRESULT hr = host_dev_->CreateFence(InitialValue, Flags, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateFence", 36, hr);
        return hr;
    }
    *ppFence = nullptr;
    ID3D12Fence* host_fence = nullptr;
    HRESULT hr = host_dev_->CreateFence(
        InitialValue, Flags, OS_D3D12_IID(ID3D12Fence), reinterpret_cast<void**>(&host_fence));
    if (FAILED(hr) || !host_fence) {
        RecordDev("ID3D12DeviceX::CreateFence", 36, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) FenceXProxy(this, host_fence, Flags);
    host_fence->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateFence", 36, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppFence);
    proxy->Release();
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
    if (pLayouts && NumSubresources > 0u) {
        std::memset(pLayouts, 0, sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) * NumSubresources);
    }
    if (pNumRows && NumSubresources > 0u) {
        std::memset(pNumRows, 0, sizeof(UINT) * NumSubresources);
    }
    if (pRowSizeInBytes && NumSubresources > 0u) {
        std::memset(pRowSizeInBytes, 0, sizeof(UINT64) * NumSubresources);
    }
    if (pTotalBytes) {
        *pTotalBytes = 0ull;
    }
    if (!pResourceDesc) {
        RecordDev("ID3D12DeviceX::GetCopyableFootprints", 38, E_INVALIDARG);
        return;
    }

    D3D12_RESOURCE_DESC clean_desc = SanitizeXboxResourceDesc(*pResourceDesc);
    if (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        clean_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (clean_desc.Height == 0u) clean_desc.Height = 1u;
        if (clean_desc.DepthOrArraySize == 0u) clean_desc.DepthOrArraySize = 1u;
        if (clean_desc.MipLevels == 0u) clean_desc.MipLevels = 1u;
        if (clean_desc.SampleDesc.Count == 0u) clean_desc.SampleDesc.Count = 1u;
    }
    if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
        clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    UINT64 host_total = UINT64_MAX;
    host_dev_->GetCopyableFootprints(
        &clean_desc, FirstSubresource, NumSubresources, BaseOffset,
        pLayouts, pNumRows, pRowSizeInBytes, &host_total);

    if (host_total == UINT64_MAX && NumSubresources > 0u) {
        // Host D3D12 refused this Xbox-specific format/dimension/flag combination;
        // compute exact D3D12-spec aligned footprints so callers never observe UINT64_MAX or garbage.
        ComputeFallbackCopyableFootprints(
            clean_desc, FirstSubresource, NumSubresources, BaseOffset,
            pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
        RecordDev("ID3D12DeviceX::GetCopyableFootprints[XboxFallback]", 38, S_OK);
    } else if (pTotalBytes) {
        *pTotalBytes = (host_total == UINT64_MAX) ? 0ull : host_total;
    }
    RecordDev("ID3D12DeviceX::GetCopyableFootprints", 38, S_OK);
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateQueryHeap(
    const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) {
    if (!ppvHeap) {
        const HRESULT hr = host_dev_->CreateQueryHeap(pDesc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12QueryHeap* host_qheap = nullptr;
    HRESULT hr = host_dev_->CreateQueryHeap(
        pDesc, OS_D3D12_IID(ID3D12QueryHeap), reinterpret_cast<void**>(&host_qheap));
    if (FAILED(hr) || !host_qheap) {
        RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) QueryHeapXProxy(this, host_qheap, pDesc);
    host_qheap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
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
    ID3D12RootSignature* host_rs = UnwrapRootSignature(pRootSignature);
    if (!ppvCommandSignature) {
        const HRESULT hr = host_dev_->CreateCommandSignature(pDesc, host_rs, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, hr);
        return hr;
    }
    *ppvCommandSignature = nullptr;
    ID3D12CommandSignature* host_sig = nullptr;
    HRESULT hr = host_dev_->CreateCommandSignature(
        pDesc, host_rs, OS_D3D12_IID(ID3D12CommandSignature), reinterpret_cast<void**>(&host_sig));
    if (FAILED(hr) || !host_sig) {
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, hr);
        return hr;
    }
    const UINT stride = pDesc ? pDesc->ByteStride : 0u;
    const UINT num_args = pDesc ? pDesc->NumArgumentDescs : 0u;
    auto* proxy = new (std::nothrow) CommandSignatureXProxy(this, host_sig, stride, num_args);
    host_sig->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvCommandSignature);
    proxy->Release();
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
        UnwrapResource(pTiledResource), pNumTilesForEntireResource, pPackedMipDesc,
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
    if (!host_dev1_) {
        RecordDev("ID3D12DeviceX::SetEventOnMultipleFenceCompletion", 45, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    std::vector<ID3D12Fence*> unwrapped;
    ID3D12Fence* const* fence_ptr = ppFences;
    if (ppFences && NumFences > 0u) {
        unwrapped.resize(NumFences);
        for (UINT i = 0u; i < NumFences; ++i) {
            unwrapped[i] = UnwrapFence(ppFences[i]);
        }
        fence_ptr = unwrapped.data();
    }
    const HRESULT hr = host_dev1_->SetEventOnMultipleFenceCompletion(
        fence_ptr, pFenceValues, NumFences, Flags, hEvent);
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

    // Pass 1: zero-allocation scan of subobject stream for Xbox shaders, SPIR-V, and wrapped root signatures.
    const auto* raw_stream = static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream);
    const size_t total_bytes = static_cast<size_t>(pDesc->SizeInBytes);
    constexpr size_t kBytecodeOffset = offsetof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>, value);
    constexpr size_t kRootSigOffset = offsetof(StreamSubobjLayout<ID3D12RootSignature*>, value);

    bool has_xbox_shader = false;
    bool has_spirv_shader = false;
    bool has_wrapped_root_sig = false;
    bool has_as_stage = false;
    uint32_t shader_stage_count = 0;
    VulkanPipelineInfo spirv_stream_pi{};

    size_t pos = 0;
    while (pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= total_bytes) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE sub_type{};
        std::memcpy(&sub_type, raw_stream + pos, sizeof(sub_type));
        const size_t stride = GetSubobjectStrideBytes(sub_type);
        if (stride == 0u || pos + stride > total_bytes) {
            break;
        }
        if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            ID3D12RootSignature* rs = nullptr;
            std::memcpy(&rs, raw_stream + pos + kRootSigOffset, sizeof(rs));
            if (rs && UnwrapRootSignature(rs) != rs) {
                has_wrapped_root_sig = true;
            }
        } else if (IsShaderSubobjectType(sub_type)) {
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
                if (vulkan_routing_active_ && IsRawSpirvBytecode(bc.pShaderBytecode, bc.BytecodeLength)) {
                    has_spirv_shader = true;
                    auto words = CopySpirvWords(bc.pShaderBytecode, bc.BytecodeLength);
                    openscarlett::SpirvValidationInfo val{};
                    if (openscarlett::VulkanBackend::ValidateSpirvBinary(words, &val).ok()) {
                        if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                            spirv_stream_pi.as_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.as_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                            spirv_stream_pi.is_mesh = true;
                            spirv_stream_pi.ms_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.ms_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                            spirv_stream_pi.vs_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.vs_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                            spirv_stream_pi.ps_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.ps_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
                            spirv_stream_pi.is_compute = true;
                            spirv_stream_pi.cs_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.cs_entry = val.entry_point_name;
                        }
                    }
                }
            }
        } else if (vulkan_routing_active_ &&
                   sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
            D3D12_RT_FORMAT_ARRAY rts{};
            std::memcpy(&rts, raw_stream + pos + offsetof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>, value), sizeof(rts));
            if (rts.NumRenderTargets > 0u) {
                spirv_stream_pi.color_format = DxgiToVulkanColorFormat(rts.RTFormats[0]);
            }
        }
        pos += stride;
    }

    if (vulkan_routing_active_ && has_spirv_shader) {
        if (!ppPipelineState) {
            RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_POINTER);
            return E_POINTER;
        }
        *ppPipelineState = nullptr;
        spirv_stream_pi.is_spirv_direct = true;
        auto* vk_pso = new (std::nothrow) PipelineStateXProxy(this, nullptr);
        if (!vk_pso) {
            RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(vk_pso)] = std::move(spirv_stream_pi);
        }
        const HRESULT hr = vk_pso->QueryInterface(riid, ppPipelineState);
        vk_pso->Release();
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }

    const D3D12_PIPELINE_STATE_STREAM_DESC* final_stream_desc = pDesc;
    D3D12_PIPELINE_STATE_STREAM_DESC lifted_stream{};
    std::vector<uint64_t> cloned_words;
    std::deque<std::vector<uint8_t>> lifted_blobs;

    if (has_xbox_shader || has_wrapped_root_sig) {
        // Pass 2: clone stream into pointer-aligned buffer, unwrap root sigs & lift Xbox shaders.
        cloned_words.assign((total_bytes + sizeof(uint64_t) - 1u) / sizeof(uint64_t), 0ull);
        auto* cloned_bytes = reinterpret_cast<uint8_t*>(cloned_words.data());
        std::memcpy(cloned_bytes, raw_stream, total_bytes);

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
            if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
                ID3D12RootSignature* rs = nullptr;
                std::memcpy(&rs, cloned_bytes + pos + kRootSigOffset, sizeof(rs));
                ID3D12RootSignature* unwrapped_rs = UnwrapRootSignature(rs);
                if (unwrapped_rs != rs) {
                    std::memcpy(cloned_bytes + pos + kRootSigOffset, &unwrapped_rs, sizeof(unwrapped_rs));
                }
            } else if (has_xbox_shader && IsShaderSubobjectType(sub_type)) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, cloned_bytes + pos + kBytecodeOffset, sizeof(bc));
                if (bc.pShaderBytecode && bc.BytecodeLength > 0 &&
                    openscarlett::IsXboxShaderBytecode(bc.pShaderBytecode, bc.BytecodeLength)) {
                    openscarlett::CachedShaderResult lifted{};
                    std::string diag;
                    openscarlett::Status st;
                    {
                        std::lock_guard<std::mutex> lift_lock(shader_lift_mutex_);
                        st = shader_cache_.GetOrLiftXboxShader(
                            bc.pShaderBytecode, bc.BytecodeLength, req, lifted, &diag);
                    }
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

        lifted_stream.SizeInBytes = pDesc->SizeInBytes;
        lifted_stream.pPipelineStateSubobjectStream = cloned_bytes;
        final_stream_desc = &lifted_stream;
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev2_->CreatePipelineState(final_stream_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = host_dev2_->CreatePipelineState(
        final_stream_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    if (FAILED(hr) || !host_pso) {
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) PipelineStateXProxy(this, host_pso);
    host_pso->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppPipelineState);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
    return hr;
}

// ID3D12Device3 (slots 48..50)
HRESULT STDMETHODCALLTYPE DeviceX::OpenExistingHeapFromAddress(
    const void* pAddress, REFIID riid, void** ppvHeap) {
    if (!host_dev3_) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev3_->OpenExistingHeapFromAddress(pAddress, riid, nullptr);
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev3_->OpenExistingHeapFromAddress(
        pAddress, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, hr);
        return hr;
    }
    const uint64_t addr_va = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pAddress));
    if (pAddress) {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        address_heaps_[host_heap] = addr_va;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, addr_va);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::OpenExistingHeapFromFileMapping(
    HANDLE hFileMapping, REFIID riid, void** ppvHeap) {
    if (!host_dev3_) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev3_->OpenExistingHeapFromFileMapping(hFileMapping, riid, nullptr);
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev3_->OpenExistingHeapFromFileMapping(
        hFileMapping, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, 0ull);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
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
                                 UnwrapFence(pFenceToSignal), FenceValueToSignal)
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
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    const D3D12_HEAP_FLAGS clean_heap_flags = SanitizeXboxHeapFlags(HeapFlags);
    if (!ppvResource) {
        const HRESULT hr = host_dev4_->CreateCommittedResource1(
            pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, pProtectedSession, riidResource, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev4_->CreateCommittedResource1(
        pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
        clear_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (vulkan_routing_active_ && pHeapProperties && desc_ptr) {
        TrackVulkanResource(host_res, pHeapProperties->Type, *desc_ptr);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 53, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, pHeapProperties, clean_heap_flags, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riidResource, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateHeap1(
    const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvHeap) {
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    D3D12_HEAP_DESC clean_heap{};
    const D3D12_HEAP_DESC* heap_ptr = pDesc;
    if (pDesc) {
        clean_heap = *pDesc;
        clean_heap.Flags = SanitizeXboxHeapFlags(pDesc->Flags);
        heap_ptr = &clean_heap;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev4_->CreateHeap1(heap_ptr, pProtectedSession, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev4_->CreateHeap1(
        heap_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, 0ull);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateReservedResource1(
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvResource) {
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev4_->CreateReservedResource1(
            desc_ptr, InitialState, clear_ptr, pProtectedSession, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev4_->CreateReservedResource1(
        desc_ptr, InitialState, clear_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 55, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, nullptr, D3D12_HEAP_FLAG_NONE, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
    return hr;
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo1(
    D3D12_RESOURCE_ALLOCATION_INFO* pRet,
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    D3D12_RESOURCE_ALLOCATION_INFO* info = nullptr;
    if (host_dev4_) {
        info = host_dev4_->GetResourceAllocationInfo1(
            pRet, visibleMask, numResourceDescs, descs_ptr, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, S_OK);
    } else {
        info = host_dev_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, descs_ptr);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, E_NOINTERFACE);
    }
    return info;
}
#else
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo1(
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    if (host_dev4_) {
        info = host_dev4_->GetResourceAllocationInfo1(
            visibleMask, numResourceDescs, descs_ptr, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, S_OK);
    } else {
        info = host_dev_->GetResourceAllocationInfo(visibleMask, numResourceDescs, descs_ptr);
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
    if (!host_dev5_) {
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, E_NOINTERFACE);
        return E_NOINTERFACE;
    }

    const D3D12_STATE_OBJECT_DESC* desc_ptr = pDesc;
    D3D12_STATE_OBJECT_DESC cloned_desc{};
    std::vector<D3D12_STATE_SUBOBJECT> cloned_subs;
    std::deque<D3D12_GLOBAL_ROOT_SIGNATURE> cloned_global_rs;
    std::deque<D3D12_LOCAL_ROOT_SIGNATURE> cloned_local_rs;
    std::deque<D3D12_EXISTING_COLLECTION_DESC> cloned_collections;
    std::deque<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> cloned_assocs;

    if (pDesc && pDesc->pSubobjects && pDesc->NumSubobjects > 0u) {
        bool needs_clone = false;
        for (UINT i = 0u; i < pDesc->NumSubobjects; ++i) {
            const auto& sub = pDesc->pSubobjects[i];
            if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE && sub.pDesc) {
                const auto* grs = static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(sub.pDesc);
                if (grs->pGlobalRootSignature &&
                    UnwrapRootSignature(grs->pGlobalRootSignature) != grs->pGlobalRootSignature) {
                    needs_clone = true;
                    break;
                }
            } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && sub.pDesc) {
                const auto* lrs = static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(sub.pDesc);
                if (lrs->pLocalRootSignature &&
                    UnwrapRootSignature(lrs->pLocalRootSignature) != lrs->pLocalRootSignature) {
                    needs_clone = true;
                    break;
                }
            } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION && sub.pDesc) {
                const auto* ec = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(sub.pDesc);
                if (ec->pExistingCollection &&
                    UnwrapStateObject(ec->pExistingCollection) != ec->pExistingCollection) {
                    needs_clone = true;
                    break;
                }
            }
        }

        if (needs_clone) {
            cloned_subs.assign(pDesc->pSubobjects, pDesc->pSubobjects + pDesc->NumSubobjects);
            for (UINT i = 0u; i < pDesc->NumSubobjects; ++i) {
                auto& sub = cloned_subs[i];
                if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE && sub.pDesc) {
                    D3D12_GLOBAL_ROOT_SIGNATURE grs =
                        *static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(sub.pDesc);
                    grs.pGlobalRootSignature = UnwrapRootSignature(grs.pGlobalRootSignature);
                    cloned_global_rs.push_back(grs);
                    sub.pDesc = &cloned_global_rs.back();
                } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && sub.pDesc) {
                    D3D12_LOCAL_ROOT_SIGNATURE lrs =
                        *static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(sub.pDesc);
                    lrs.pLocalRootSignature = UnwrapRootSignature(lrs.pLocalRootSignature);
                    cloned_local_rs.push_back(lrs);
                    sub.pDesc = &cloned_local_rs.back();
                } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION && sub.pDesc) {
                    D3D12_EXISTING_COLLECTION_DESC ec =
                        *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(sub.pDesc);
                    ec.pExistingCollection = UnwrapStateObject(ec.pExistingCollection);
                    cloned_collections.push_back(ec);
                    sub.pDesc = &cloned_collections.back();
                } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION &&
                           sub.pDesc) {
                    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc =
                        *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(sub.pDesc);
                    if (assoc.pSubobjectToAssociate >= pDesc->pSubobjects &&
                        assoc.pSubobjectToAssociate < pDesc->pSubobjects + pDesc->NumSubobjects) {
                        const ptrdiff_t idx = assoc.pSubobjectToAssociate - pDesc->pSubobjects;
                        assoc.pSubobjectToAssociate = &cloned_subs[static_cast<size_t>(idx)];
                    }
                    cloned_assocs.push_back(assoc);
                    sub.pDesc = &cloned_assocs.back();
                }
            }
            cloned_desc.Type = pDesc->Type;
            cloned_desc.NumSubobjects = pDesc->NumSubobjects;
            cloned_desc.pSubobjects = cloned_subs.data();
            desc_ptr = &cloned_desc;
        }
    }

    if (!ppStateObject) {
        const HRESULT hr = host_dev5_->CreateStateObject(desc_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
        return hr;
    }
    *ppStateObject = nullptr;
    ID3D12StateObject* host_so = nullptr;
    HRESULT hr = host_dev5_->CreateStateObject(
        desc_ptr, OS_D3D12_IID(ID3D12StateObject), reinterpret_cast<void**>(&host_so));
    if ((FAILED(hr) || !host_so) &&
        adapter_info_.raytracing_tier == 0u &&
        pDesc != nullptr &&
        pDesc->pSubobjects != nullptr &&
        pDesc->NumSubobjects > 0u &&
        (pDesc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION ||
         pDesc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)) {
        // Non-DXR adapter (e.g. GTX 1650 Ti / Win10 WARP): provide clean-room software StateObjectXProxy
        // supporting ID3D12StateObject + ID3D12StateObjectProperties shader identifier queries.
        auto* sw_proxy = new (std::nothrow) StateObjectXProxy(this, nullptr, pDesc);
        if (!sw_proxy) {
            RecordDev("ID3D12DeviceX::CreateStateObject", 62, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        hr = sw_proxy->QueryInterface(riid, ppStateObject);
        sw_proxy->Release();
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
        return hr;
    }
    if (FAILED(hr) || !host_so) {
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) StateObjectXProxy(this, host_so, pDesc);
    host_so->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppStateObject);
    proxy->Release();
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
    *ppvResource = nullptr;
    D3D12_RESOURCE_DESC clean_desc = SanitizeXboxResourceDesc(*pDesc);
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
        clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        did_r9g9b9e5_fallback = true;
        if (pOptimizedClearValue &&
            pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
            clean_clear = *pOptimizedClearValue;
            clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            clear_ptr = &clean_clear;
        }
    }

    // If VirtualAddress falls inside an attached XMem graphics allocation, place directly
    // into its backing desktop ID3D12Heap so GPU virtual addresses bind cleanly on desktop!
    HRESULT hr = E_FAIL;
    ID3D12Resource* host_res = nullptr;
    if (VirtualAddress != 0ull && xmem_allocation_count_.load(std::memory_order_acquire) > 0u) {
        ID3D12Heap* backing_heap = nullptr;
        uint64_t heap_offset = 0ull;
        uint64_t heap_size = 0ull;
        {
            std::lock_guard<std::mutex> lock(unified_mutex_);
            for (const auto& rec : xmem_allocations_) {
                if (rec.heap && VirtualAddress >= rec.guest_va &&
                    VirtualAddress < rec.guest_va + rec.aligned_size) {
                    backing_heap = rec.heap;
                    backing_heap->AddRef();
                    heap_offset = VirtualAddress - rec.guest_va;
                    heap_size = rec.aligned_size;
                    break;
                }
            }
        }
        if (backing_heap) {
            const D3D12_RESOURCE_ALLOCATION_INFO alloc_info =
                host_dev_->GetResourceAllocationInfo(0u, 1u, &clean_desc);
            const uint64_t req_align = std::max<uint64_t>(
                alloc_info.Alignment ? alloc_info.Alignment : 65536ull, 65536ull);
            if (alloc_info.SizeInBytes > 0ull &&
                (heap_offset % req_align) == 0ull &&
                heap_offset + alloc_info.SizeInBytes <= heap_size) {
                hr = host_dev_->CreatePlacedResource(
                    backing_heap, heap_offset, &clean_desc, InitialState,
                    clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            }
            backing_heap->Release();
        }
    }

    // Fallback to committed default-heap resource if not attached or heap placement incompatible.
    D3D12_HEAP_PROPERTIES default_heap_props{};
    default_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(hr) || !host_res) {
        hr = host_dev_->CreateCommittedResource(
            &default_heap_props, D3D12_HEAP_FLAG_NONE, &clean_desc, InitialState,
            clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }

    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 69, hr);
        return hr;
    }

    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (vulkan_routing_active_) {
        TrackVulkanResource(host_res, D3D12_HEAP_TYPE_DEFAULT, clean_desc);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 69, S_OK);
    }
    if (VirtualAddress != 0ull) {
        AutoRegisterUnifiedResource(host_res, VirtualAddress, clean_desc, InitialState);
        openscarlett::TitleVaMappingDesc mdesc{};
        mdesc.title_va = VirtualAddress;
        mdesc.size_bytes = clean_desc.Width;
        mdesc.kind = (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                         ? openscarlett::TitleVaResourceKind::Buffer
                         : openscarlett::TitleVaResourceKind::Texture;
        mdesc.existing_write_watch_cpu_ptr = reinterpret_cast<void*>(VirtualAddress);
        mdesc.gpu_shadow_resource = host_res;
        mdesc.initial_state = InitialState;
        openscarlett::TitleVaRegionInfo reg_info{};
        (void)title_va_mirror_.RegisterRegion(mdesc, host_dev_, reg_info);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, &clean_desc, &default_heap_props, D3D12_HEAP_FLAG_NONE, VirtualAddress);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 69, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
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
    resource = UnwrapResource(resource);
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
        for (auto& xmem : xmem_allocations_) {
            if (xmem.anchor_buffer) xmem.anchor_buffer->Release();
            if (xmem.heap) xmem.heap->Release();
        }
        xmem_allocations_.clear();
        xmem_allocation_count_.store(0u, std::memory_order_release);
        registered_unified_count_.store(0u, std::memory_order_release);
        dirty_unified_count_.store(0u, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(fallback_mutex_);
        r9g9b9e5_fallback_resources_.clear();
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

    ID3D12Resource* host_res = UnwrapResource(desc->Resource);
    const D3D12_RESOURCE_DESC rdesc = host_res->GetDesc();
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
    auto it = unified_textures_.find(host_res);
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
        m.resource = host_res;
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
        unified_textures_.emplace(host_res, m);
        registered_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        if (m.dirty) {
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        }
    }

    RecordDev("ID3D12DeviceX::RegisterUnifiedTexture", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::UnregisterUnifiedTexture(ID3D12Resource* resource) {
    resource = UnwrapResource(resource);
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

HRESULT STDMETHODCALLTYPE DeviceX::AttachXMemAllocation(
    uint64_t guest_va,
    uint64_t byte_size,
    uint64_t* out_desktop_gpu_va) {
    if (out_desktop_gpu_va) *out_desktop_gpu_va = 0ull;
    if (guest_va == 0ull || byte_size == 0ull || !host_dev_) {
        RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }

    const uint64_t aligned_size = AlignUp64(byte_size, 65536ull);

    // Check if this guest_va is already attached with sufficient size.
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (const auto& rec : xmem_allocations_) {
            if (rec.guest_va == guest_va && rec.aligned_size >= aligned_size) {
                if (out_desktop_gpu_va) *out_desktop_gpu_va = rec.desktop_gpu_va;
                RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, S_OK);
                return S_OK;
            }
        }
    }

    // Create a desktop D3D12 default heap and anchor buffer so every attached xmem graphics
    // allocation binds immediately to a real, non-zero desktop GPU virtual address.
    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = aligned_size;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Flags = D3D12_HEAP_FLAG_NONE;

    ID3D12Heap* heap = nullptr;
    HRESULT hr = host_dev_->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr) || !heap) {
        // Fallback for ResourceHeapTier 1 devices where D3D12_HEAP_FLAG_NONE is disallowed.
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        hr = host_dev_->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
        if (FAILED(hr) || !heap) {
            RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, hr);
            return hr;
        }
    }

    D3D12_RESOURCE_DESC anchor_desc{};
    anchor_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    anchor_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    anchor_desc.Width = aligned_size;
    anchor_desc.Height = 1u;
    anchor_desc.DepthOrArraySize = 1u;
    anchor_desc.MipLevels = 1u;
    anchor_desc.Format = DXGI_FORMAT_UNKNOWN;
    anchor_desc.SampleDesc.Count = 1u;
    anchor_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    anchor_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* anchor_buf = nullptr;
    hr = host_dev_->CreatePlacedResource(
        heap, 0ull, &anchor_desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&anchor_buf));
    if (FAILED(hr) || !anchor_buf) {
        heap->Release();
        RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, hr);
        return hr;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS desktop_va = anchor_buf->GetGPUVirtualAddress();
    if (desktop_va == 0ull) {
        anchor_buf->Release();
        heap->Release();
        RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, E_FAIL);
        return E_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        // Replace any previous record at the exact same guest_va.
        for (auto it = xmem_allocations_.begin(); it != xmem_allocations_.end(); ) {
            if (it->guest_va == guest_va) {
                if (it->anchor_buffer) it->anchor_buffer->Release();
                if (it->heap) it->heap->Release();
                it = xmem_allocations_.erase(it);
            } else {
                ++it;
            }
        }
        XMemAllocationRecord rec{};
        rec.guest_va = guest_va;
        rec.byte_size = byte_size;
        rec.aligned_size = aligned_size;
        rec.heap = heap;
        rec.anchor_buffer = anchor_buf;
        rec.desktop_gpu_va = desktop_va;
        xmem_allocations_.push_back(rec);
        xmem_allocation_count_.store(
            static_cast<uint32_t>(xmem_allocations_.size()), std::memory_order_release);
    }

    if (out_desktop_gpu_va) *out_desktop_gpu_va = desktop_va;
    RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::DetachXMemAllocation(uint64_t guest_va) {
    if (guest_va == 0ull) {
        RecordDev("ID3D12DeviceX::DetachXMemAllocation", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    ID3D12Heap* old_heap = nullptr;
    ID3D12Resource* old_anchor = nullptr;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (auto it = xmem_allocations_.begin(); it != xmem_allocations_.end(); ++it) {
            if (it->guest_va == guest_va) {
                old_heap = it->heap;
                old_anchor = it->anchor_buffer;
                xmem_allocations_.erase(it);
                xmem_allocation_count_.store(
                    static_cast<uint32_t>(xmem_allocations_.size()), std::memory_order_release);
                found = true;
                break;
            }
        }
    }
    if (old_anchor) old_anchor->Release();
    if (old_heap) old_heap->Release();
    const HRESULT hr = found ? S_OK : S_FALSE;
    RecordDev("ID3D12DeviceX::DetachXMemAllocation", -1, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::ResolveXMemGpuVa(
    uint64_t guest_va,
    uint64_t* out_desktop_gpu_va) {
    if (!out_desktop_gpu_va) {
        RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, E_POINTER);
        return E_POINTER;
    }
    *out_desktop_gpu_va = 0ull;
    if (guest_va == 0ull) {
        RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(unified_mutex_);
    for (const auto& rec : xmem_allocations_) {
        if (guest_va >= rec.guest_va && guest_va < rec.guest_va + rec.aligned_size) {
            *out_desktop_gpu_va = rec.desktop_gpu_va + (guest_va - rec.guest_va);
            RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, S_OK);
            return S_OK;
        }
    }
    RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT STDMETHODCALLTYPE DeviceX::GetR9G9B9E5UavFallbackActive(int* out_active) {
    if (!out_active) {
        RecordDev("ID3D12DeviceX::GetR9G9B9E5UavFallbackActive", -1, E_POINTER);
        return E_POINTER;
    }
    *out_active = IsR9G9B9E5UavFallbackActive() ? 1 : 0;
    RecordDev("ID3D12DeviceX::GetR9G9B9E5UavFallbackActive", -1, S_OK);
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
