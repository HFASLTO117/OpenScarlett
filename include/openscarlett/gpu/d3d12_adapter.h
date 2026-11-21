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
};

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
