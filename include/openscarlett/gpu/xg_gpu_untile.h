#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/xg_xs_abi.h"

#if defined(_WIN32)
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
#endif

namespace openscarlett {

// Direction of GPU compute surface conversion.
enum class XgGpuDirection : uint32_t {
    Untile = 0u,  // Xbox GFX10 tiled -> PC linear
    Retile = 1u,  // PC linear -> Xbox GFX10 tiled
};

// Optional AMD GFX10 fast-clear metadata passthrough mode (active during Untile).
enum class XgGpuMetadataMode : uint32_t {
    None = 0u,
    Dcc = 1u,    // 1 byte per 256B block: 0x00/0x40/0x80/0xC0/0x20 fast-clear sentinels
    Htile = 2u,  // 4 bytes per 8x8 pixel block: zmask == 0 fast depth-clear sentinel
    Cmask = 3u,  // 4 bits per 8x8 pixel block: nibble == 0 fast color-clear sentinel
};

// GPU compute dispatch constant parameters (exact 128-byte / 32-dword ABI shared by HLSL CB & SPIR-V header).
struct XgGpuUntileParams {
    uint32_t direction = 0u;                // 0 = Untile, 1 = Retile
    uint32_t tile_mode = 0u;                // XG_TILE_MODE
    uint32_t is_3d = 0u;                    // 0 = 2D, 1 = 3D
    uint32_t bpe = 4u;                      // Bytes per element/block (1, 2, 4, 8, 16)

    uint32_t width_elems = 1u;              // Width in elements (or 4x4 BC blocks)
    uint32_t height_elems = 1u;             // Height in elements (or 4x4 BC blocks)
    uint32_t depth_elems = 1u;              // Depth in elements (1 for 2D)
    uint32_t linear_row_pitch = 4u;         // Linear row pitch in bytes

    uint32_t linear_slice_pitch = 4u;       // Linear Z-slice pitch in bytes (3D)
    uint32_t tiled_base_offset = 0u;        // Byte offset in tiled buffer for this subresource
    uint32_t in_mip_tail = 0u;              // 1 if mip resides inside GFX10 mip tail
    uint32_t mip_level = 0u;                // Mip level index

    uint32_t slice_index = 0u;              // Array slice / cube face index (2D)
    uint32_t tiles_x = 1u;                  // Macro tiles along X
    uint32_t tiles_y = 1u;                  // Macro tiles along Y
    uint32_t tile_w_elems = 1u;             // Elements per tile X

    uint32_t tile_h_elems = 1u;             // Elements per tile Y
    uint32_t tile_d_elems = 1u;             // Elements per tile Z
    uint32_t tile_size_bytes = 256u;        // Macro tile size in bytes (256, 4096, 65536)
    uint32_t micro_w_elems = 1u;            // Micro tile width in elements

    uint32_t micro_h_elems = 1u;            // Micro tile height in elements
    uint32_t micro_size_bytes = 64u;        // Micro tile size in bytes (64 or 256)
    uint32_t swizzle_family = 0u;           // 0=Linear, 1=Std, 2=Disp, 3=DepthZ, 4=Rot
    uint32_t has_xor = 0u;                  // 1 for _X pipe/bank XOR swizzle modes

    uint32_t mip_tail_bits_x = 0u;          // log2(MipTailPaddedWidthElements)
    uint32_t mip_tail_bits_y = 0u;          // log2(MipTailPaddedHeightElements)
    uint32_t mip_tail_bits_z = 0u;          // log2(MipTailPaddedDepthElements)
    uint32_t mip_tail_sub_block_bytes = 0u; // MipTailSubBlockBytes

    // Linear tiled pitches (used when tile_mode == XG_TILE_MODE_LINEAR) OR metadata controls
    uint32_t tiled_row_pitch = 0u;          // ml.RowPitchBytes for linear tile mode
    uint32_t tiled_depth_pitch = 0u;        // ml.DepthPitchBytes for 3D linear tile mode
    uint32_t meta_mode = 0u;                // XgGpuMetadataMode
    uint32_t meta_blocks_x = 1u;            // Metadata block grid width

    uint32_t meta_pipe_aligned = 0u;        // 1 if DCC metadata is pipe-aligned
    uint32_t meta_num_pipes = 16u;          // Scarlett pipe count (16 Anaconda, 8 Lockhart)
    uint32_t meta_slice_pitch = 0u;         // Metadata slice pitch in bytes
    uint32_t meta_base_offset = 0u;         // Byte offset into metadata buffer

    // Pre-encoded format-native fast-clear element payloads (up to 16 bytes / 4 dwords each).
    uint32_t clear_0000[4] = {0u, 0u, 0u, 0u};
    uint32_t clear_0001[4] = {0u, 0u, 0u, 0u};
    uint32_t clear_1110[4] = {0u, 0u, 0u, 0u};
    uint32_t clear_1111[4] = {0u, 0u, 0u, 0u};
    uint32_t clear_reg[4] = {0u, 0u, 0u, 0u};

    // SPIR-V packed storage buffer word offsets (ignored by D3D12).
    uint32_t spv_src_word_offset = 64u;
    uint32_t spv_meta_word_offset = 64u;
    uint32_t spv_dst_byte_size = 0u;
    uint32_t spv_reserved = 0u;
};

static_assert(sizeof(XgGpuUntileParams) == 240u, "XgGpuUntileParams must be 60 dwords (240 bytes)");

// High-level 2D GPU untile/retile request.
struct XgGpuSurface2DRequest {
    XG_TEXTURE2D_DESC desc{};
    uint32_t plane = 0u;
    uint32_t mip_level = 0u;
    uint32_t slice_index = 0u;
    XgGpuDirection direction = XgGpuDirection::Untile;
    uint32_t linear_row_pitch_bytes = 0u;  // 0 = tightly packed WidthElements * bpe

    // Optional fast-clear metadata passthrough (active when direction == Untile).
    XgGpuMetadataMode metadata_mode = XgGpuMetadataMode::None;
    XG_CONSOLE_TYPE console_type = XG_CONSOLE_TYPE_SCARLETT_ANACONDA;
    uint32_t dcc_pipe_aligned = 1u;
    std::array<uint32_t, 4> clear_reg_value{0u, 0u, 0u, 0u};
};

// High-level 3D GPU untile/retile request.
struct XgGpuSurface3DRequest {
    XG_TEXTURE3D_DESC desc{};
    uint32_t mip_level = 0u;
    XgGpuDirection direction = XgGpuDirection::Untile;
    uint32_t linear_row_pitch_bytes = 0u;    // 0 = tightly packed
    uint32_t linear_slice_pitch_bytes = 0u;  // 0 = tightly packed
};

struct XgGpuDispatchMetrics {
    std::string backend_name;
    std::string adapter_name;
    std::string shader_target;
    uint32_t dispatch_groups_x = 0u;
    uint32_t total_elements = 0u;
    uint32_t bytes_per_element = 0u;
    bool executed_on_hardware_gpu = false;
    bool used_warp = false;
    bool used_vulkan_icd = false;
};

// Populates format-aware fast-clear 16-byte payloads (0000, 0001, 1110, 1111) for a DXGI format.
void PopulateFormatFastClearPayloads(
    uint32_t dxgi_format,
    uint32_t bpe,
    uint32_t out_0000[4],
    uint32_t out_0001[4],
    uint32_t out_1110[4],
    uint32_t out_1111[4]) noexcept;

// Builds bit-exact XgGpuUntileParams from XG_TEXTURE2D_DESC matching src/gpu/xg_layout.cpp.
Status BuildXgGpuUntileParams2D(
    const XgGpuSurface2DRequest& req,
    XgGpuUntileParams& out_params,
    uint64_t* out_linear_bytes = nullptr,
    uint64_t* out_tiled_bytes = nullptr);

// Builds bit-exact XgGpuUntileParams from XG_TEXTURE3D_DESC matching src/gpu/xg_layout.cpp.
Status BuildXgGpuUntileParams3D(
    const XgGpuSurface3DRequest& req,
    XgGpuUntileParams& out_params,
    uint64_t* out_linear_bytes = nullptr,
    uint64_t* out_tiled_bytes = nullptr);

// Generates the HLSL compute shader source (compatible with cs_6_2 and cs_6_5) implementing
// full GFX10 2D/3D untile/retile, mip-tails, BC1-BC7, and DCC/HTILE/CMASK fast-clear passthrough.
std::string GenerateXgGpuUntileHlsl();

// Compiles the GFX10 GPU untiler HLSL to DXIL using CompileHlslWithDxc (cs_6_5 or cs_6_2).
Status CompileXgGpuUntileDxil(
    const std::string& target_profile,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path = {});

// Emits a valid SPIR-V 1.3 compute shader binary implementing GFX10 2D/3D untile/retile,
// mip-tails, BC1-BC7, and DCC/HTILE/CMASK fast-clear passthrough.
// Passes ValidateSpirvBinary + Khronos spirv-val.exe and executes live on VulkanBackend.
Status EmitXgGpuUntileSpirv(std::vector<uint32_t>& out_spirv);

// Real-time GPU Compute Untiler/Retiler supporting:
//  - Live D3D12 command list recording (ID3D12Device* + ID3D12GraphicsCommandList*)
//  - Self-contained D3D12 hardware GPU AND D3D12 WARP execution
//  - Live Vulkan 1.2/1.3/1.4 SPIR-V compute execution via VulkanBackend
class XgGpuUntiler {
public:
    XgGpuUntiler();
    ~XgGpuUntiler();

    XgGpuUntiler(const XgGpuUntiler&) = delete;
    XgGpuUntiler& operator=(const XgGpuUntiler&) = delete;

#if defined(_WIN32)
    // Initializes D3D12 compute PSO on an explicit host ID3D12Device (for live command list dispatch).
    Status InitializeD3D12WithDevice(
        ID3D12Device* device,
        const std::string& target_profile_override = {},
        const std::string& dxc_path = {});

    // Initializes standalone D3D12 compute runtime on either Hardware GPU (prefer_warp=false,
    // strictly rejecting software/WARP adapters) or D3D12 WARP (prefer_warp=true).
    Status InitializeD3D12(
        bool prefer_warp = false,
        const std::string& target_profile_override = {},
        const std::string& dxc_path = {});

    // Records a GPU untile/retile compute dispatch directly into a caller's D3D12 command list.
    // `cbv_gpu_va` must point to a 256-byte-aligned upload buffer holding XgGpuUntileParams.
    // `meta_srv_gpu_va` may be 0 when metadata_mode == None (binds src_srv_gpu_va safely).
    Status RecordDispatchD3D12(
        ID3D12GraphicsCommandList* cmd_list,
        const XgGpuUntileParams& params,
        uint64_t cbv_gpu_va,
        uint64_t src_srv_gpu_va,
        uint64_t meta_srv_gpu_va,
        uint64_t dst_uav_gpu_va) const;

    // Self-contained synchronous D3D12 GPU compute untile/retile execution.
    // Note: for Retile when `dst_bytes` is non-empty, existing bytes outside the target subresource
    // (e.g., sibling mip-tail levels) are preserved!
    Status ExecuteD3D12(
        const XgGpuUntileParams& params,
        const std::vector<uint8_t>& src_bytes,
        std::vector<uint8_t>& dst_bytes,
        const std::vector<uint8_t>* meta_bytes = nullptr,
        XgGpuDispatchMetrics* out_metrics = nullptr);

    bool IsD3D12Ready() const noexcept;
    bool IsD3D12Warp() const noexcept;
    const std::string& D3D12AdapterName() const noexcept;
    const std::string& D3D12ShaderTarget() const noexcept;
#endif

    // Initializes Vulkan SPIR-V compute backend (gracefully handles missing ICD on headless CI).
    Status InitializeVulkan(bool allow_software_fallback = false);
    bool IsVulkanReady() const noexcept;
    bool IsVulkanHardwareIcd() const noexcept;
    const std::string& VulkanDeviceName() const noexcept;

    // Self-contained Vulkan SPIR-V 1.3 compute untile/retile execution.
    Status ExecuteVulkan(
        const XgGpuUntileParams& params,
        const std::vector<uint8_t>& src_bytes,
        std::vector<uint8_t>& dst_bytes,
        const std::vector<uint8_t>* meta_bytes = nullptr,
        XgGpuDispatchMetrics* out_metrics = nullptr);

    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openscarlett
