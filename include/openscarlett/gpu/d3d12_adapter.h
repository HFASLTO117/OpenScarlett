#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/core/status.h"

#if defined(_WIN32)
struct ID3D12Device;
struct IDXGIAdapter1;
#endif

namespace openscarlett {

struct D3D12AdapterInfo {
    std::string description;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t subsys_id = 0;
    uint32_t revision = 0;
    uint64_t dedicated_video_memory_bytes = 0;
    uint64_t dedicated_video_memory_mb = 0;
    uint64_t shared_system_memory_bytes = 0;
    uint32_t luid_low = 0;
    int32_t luid_high = 0;
    bool is_warp = false;
    uint32_t mesh_shader_tier = 0;      // D3D12_MESH_SHADER_TIER (0 = unsupported, 10 = Tier 1)
    uint32_t raytracing_tier = 0;       // D3D12_RAYTRACING_TIER
    uint32_t highest_shader_model = 0;  // D3D_SHADER_MODEL (e.g. 0x65 = SM 6.5)
    uint32_t wave_lane_min = 0;
    uint32_t wave_lane_max = 0;
    uint32_t feature_level = 0;         // D3D_FEATURE_LEVEL
    bool supports_r9g9b9e5_uav = false; // Typed UAV store support for DXGI_FORMAT_R9G9B9E5_SHAREDEXP
};

enum class GpuVendorClass : uint32_t {
    Unknown = 0u,
    Nvidia = 1u,        // 0x10DE (Turing - Blackwell, Wave32)
    Amd = 2u,           // 0x1002 (RDNA2/3/4, Wave32..64)
    Intel = 3u,         // 0x8086 (Arc Alchemist/Battlemage & iGPUs, Wave16..32)
    MicrosoftWarp = 4u, // 0x1414 (WARP software rasterizer)
    Qualcomm = 5u,      // 0x5143 / 0x4D4F
};

struct SanitizedIhvCapabilities {
    GpuVendorClass vendor_class = GpuVendorClass::Unknown;
    uint32_t vendor_id = 0u;
    uint32_t device_id = 0u;
    std::string description;
    bool is_warp = false;
    bool is_uma_low_vram = false;  // e.g. 128 MB UMA iGPU
    uint64_t dedicated_video_memory_bytes = 0ull;
    uint64_t shared_system_memory_bytes = 0ull;
    uint64_t effective_vram_budget_bytes = 0ull;
    uint32_t wave_lane_min = 32u;
    uint32_t wave_lane_max = 32u;
    uint32_t preferred_wave_size = 32u;
    uint32_t raytracing_tier = 0u;      // Sanitized strictly to {0, 10, 11, 12}
    uint32_t mesh_shader_tier = 0u;     // Sanitized strictly to {0, 10}
    uint32_t highest_shader_model = 0x51u;
    uint32_t feature_level = 0xC000u;
    bool supports_r9g9b9e5_uav = false;
    bool requires_wave64_emulation = false;
    bool requires_page_isolated_descriptor_heaps = false;
    bool promote_large_placed_textures_to_committed = false;
    uint64_t large_placed_texture_threshold_bytes = 0ull;
    bool recommend_series_x_profile = false;
};

// Deterministically sanitizes raw D3D12 adapter capabilities across vendors (Intel Arc Battlemage
// Wave=16..32, NVIDIA Turing-Blackwell Wave=32, AMD RDNA2/3/4 Wave=32..64, 128 MB UMA iGPUs,
// DXR 0/10/11/12, MeshTier 0/10) so capability queries never crash or over-report.
SanitizedIhvCapabilities SanitizeCrossIhvCapabilities(const D3D12AdapterInfo& raw_info) noexcept;

// Classifies a PCI vendor ID + WARP flag into GpuVendorClass.
GpuVendorClass ClassifyGpuVendor(uint32_t vendor_id, bool is_warp) noexcept;

struct D3D12AdapterSelectionOptions {
    bool prefer_warp = false;
    bool allow_warp_fallback = true;
    bool enable_debug_layer = false;
    uint32_t min_feature_level = 0xC000u;  // D3D_FEATURE_LEVEL_12_0
};

#if defined(_WIN32)

// Selects a D3D12 adapter using IDXGIFactory6::EnumAdapterByGpuPreference
// (DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE) by default, skipping software adapters
// unless prefer_warp is true, and creates a real host ID3D12Device directly from
// system d3d12.dll (avoiding any recursion with d3d12_xs.dll exports).
Status SelectD3D12AdapterAndCreateDevice(
    const D3D12AdapterSelectionOptions& options,
    ID3D12Device** out_device,
    IDXGIAdapter1** out_adapter,
    D3D12AdapterInfo* out_info);

// Queries hardware capabilities (Mesh Shader tier, DXR tier, Shader Model, wave size)
// for an existing host ID3D12Device and optional IDXGIAdapter1.
Status QueryD3D12DeviceAdapterInfo(
    ID3D12Device* device,
    IDXGIAdapter1* adapter,
    D3D12AdapterInfo& out_info);

// Resolves a function pointer directly from the host OS system d3d12.dll.
void* ResolveHostD3D12Proc(const char* proc_name);

#endif

// Enumerates all D3D12-capable adapters on the host ordered by high-performance preference.
Status EnumerateD3D12Adapters(std::vector<D3D12AdapterInfo>& out_adapters);

}  // namespace openscarlett
