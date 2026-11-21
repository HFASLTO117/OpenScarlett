#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/dxr_hardware.h"
#include "openscarlett/gpu/rt_bvh.h"
#include "os_check.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

namespace {

using namespace openscarlett;

struct PipelineRunResult {
    DxrCapabilities caps{};
    uint64_t initial_hash = 0;
    uint64_t updated_hash = 0;
    FrameBufferRgba8 initial_frame{};
    FrameBufferRgba8 updated_frame{};
    RtStats initial_stats{};
    RtStats updated_stats{};
    DxrShaderBindingTable sbt{};
};

size_t CountLitPixels(const FrameBufferRgba8& frame, size_t& out_unique_colors) {
    size_t lit = 0;
    std::unordered_set<uint32_t> unique;
    const size_t total = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    for (size_t i = 0; i < total; ++i) {
        const uint8_t r = frame.pixels[i * 4u + 0u];
        const uint8_t g = frame.pixels[i * 4u + 1u];
        const uint8_t b = frame.pixels[i * 4u + 2u];
        const uint8_t a = frame.pixels[i * 4u + 3u];
        OS_REQUIRE(a == 255u);
        if (r > 16u || g > 16u || b > 16u) {
            ++lit;
        }
        const uint32_t packed = (static_cast<uint32_t>(r) << 16) |
                                (static_cast<uint32_t>(g) << 8) |
                                static_cast<uint32_t>(b);
        unique.insert(packed);
    }
    out_unique_colors = unique.size();
    return lit;
}

PipelineRunResult RunDxrPipelineSession(DxrHardwareEngine& engine, const char* label) {
    PipelineRunResult result{};
    result.caps = engine.Capabilities();

    std::printf("  [%s] Adapter='%s' (VID=0x%04X DID=0x%04X VRAM=%llu MB WARP=%s) "
                "Device5=%s Tier=%u StateObj=%s InlineRT=%s Path=%s\n",
                label,
                result.caps.adapter_description.c_str(),
                result.caps.vendor_id,
                result.caps.device_id,
                static_cast<unsigned long long>(result.caps.dedicated_video_memory / (1024ull * 1024ull)),
                result.caps.adapter_is_warp ? "yes" : "no",
                result.caps.supports_device5 ? "yes" : "no",
                result.caps.hardware_tier,
                result.caps.supports_state_objects ? "yes" : "no",
                result.caps.supports_inline_rt ? "yes" : "no",
                result.caps.using_hardware_path ? "hardware-DXR" : "software-BVH");

    // 1. Prebuild info queries (BLAS + TLAS)
    DxrAsInputs blas_inputs{};
    blas_inputs.type = DxrAsType::BottomLevel;
    blas_inputs.build_flags = kDxrBuildAllowUpdate | kDxrBuildPreferFastTrace;
    blas_inputs.primitive_count = 24;
    blas_inputs.geometry_count = 1;
    blas_inputs.vertex_stride = 12;

    DxrPrebuildInfo blas_prebuild{};
    OS_REQUIRE(engine.QueryPrebuildInfo(blas_inputs, blas_prebuild).ok());
    OS_REQUIRE(blas_prebuild.result_data_max_size_bytes > 0);
    OS_REQUIRE(blas_prebuild.scratch_data_size_bytes > 0);
    OS_REQUIRE(blas_prebuild.update_scratch_data_size_bytes > 0);
    OS_REQUIRE((blas_prebuild.result_data_max_size_bytes % kDxrAsByteAlignment) == 0);
    OS_REQUIRE((blas_prebuild.scratch_data_size_bytes % kDxrAsByteAlignment) == 0);
    OS_REQUIRE((blas_prebuild.update_scratch_data_size_bytes % kDxrAsByteAlignment) == 0);

    DxrAsInputs tlas_inputs{};
    tlas_inputs.type = DxrAsType::TopLevel;
    tlas_inputs.build_flags = kDxrBuildPreferFastTrace;
    tlas_inputs.primitive_count = 4;

    DxrPrebuildInfo tlas_prebuild{};
    OS_REQUIRE(engine.QueryPrebuildInfo(tlas_inputs, tlas_prebuild).ok());
    OS_REQUIRE(tlas_prebuild.result_data_max_size_bytes > 0);
    OS_REQUIRE((tlas_prebuild.result_data_max_size_bytes % kDxrAsByteAlignment) == 0);

    // 2. Build BLAS #1 (static Cornell-style room + pyramid, non-updatable)
    const auto room_tris = BuildDefaultRtTestScene();
    uint64_t room_blas = 0;
    OS_REQUIRE(engine.BuildBlas(room_tris, kDxrBuildPreferFastTrace, room_blas).ok());
    OS_REQUIRE(room_blas != 0);

    // Build BLAS #2 (updatable glossy cube mesh)
    auto cube_tris = BuildUnitCubeMesh({0.20f, 0.72f, 0.96f}, 0.35f);
    uint64_t cube_blas = 0;
    OS_REQUIRE(engine.BuildBlas(cube_tris, kDxrBuildAllowUpdate | kDxrBuildPreferFastTrace, cube_blas).ok());
    OS_REQUIRE(cube_blas != 0);
    OS_REQUIRE(cube_blas != room_blas);

    DxrAsRecordInfo room_info{};
    OS_REQUIRE(engine.GetAsInfo(room_blas, room_info).ok());
    OS_REQUIRE(room_info.type == DxrAsType::BottomLevel);
    OS_REQUIRE(room_info.primitive_count == room_tris.size());
    OS_REQUIRE(room_info.bvh_node_count > 0);
    OS_REQUIRE(room_info.gpu_virtual_address != 0);
    OS_REQUIRE((room_info.gpu_virtual_address % kDxrAsByteAlignment) == 0);

    // Negative check: updating a BLAS built without kDxrBuildAllowUpdate must fail.
    OS_REQUIRE(engine.UpdateBlas(room_blas, room_tris).code == StatusCode::InvalidArgument);
    // Negative check: updating with mismatched triangle count must fail.
    std::vector<RtTriangle> bad_tris(cube_tris.begin(), cube_tris.begin() + 4);
    OS_REQUIRE(engine.UpdateBlas(cube_blas, bad_tris).code == StatusCode::InvalidArgument);

    // 3. Build multi-instance TLAS with affine transforms + one masked-out instance
    std::vector<DxrInstanceDesc> instances(4);
    instances[0].blas_id = room_blas;
    MakeIdentity3x4(instances[0].transform);
    instances[0].instance_id = 0;
    instances[0].instance_mask = 0xFFu;
    instances[0].instance_contribution_to_hit_group_index = 0;

    instances[1].blas_id = cube_blas;
    MakeScaleTranslation3x4(0.65f, 0.65f, 0.65f, -1.15f, 0.45f, 0.35f, instances[1].transform);
    instances[1].instance_id = 1;
    instances[1].instance_mask = 0xFFu;
    instances[1].instance_contribution_to_hit_group_index = 1;

    instances[2].blas_id = cube_blas;
    MakeRotationY3x4(0.55f, 1.15f, 0.55f, 0.50f, instances[2].transform);
    instances[2].instance_id = 2;
    instances[2].instance_mask = 0xFFu;
    instances[2].instance_contribution_to_hit_group_index = 1;

    // Hidden instance (instance_mask == 0) must be excluded from visible geometry.
    instances[3].blas_id = cube_blas;
    MakeScaleTranslation3x4(4.0f, 4.0f, 4.0f, 0.0f, 1.0f, -2.5f, instances[3].transform);
    instances[3].instance_id = 3;
    instances[3].instance_mask = 0x00u;

    uint64_t tlas_initial = 0;
    OS_REQUIRE(engine.BuildTlas(instances, kDxrBuildPreferFastTrace, tlas_initial).ok());
    OS_REQUIRE(tlas_initial != 0);

    DxrAsRecordInfo tlas_info{};
    OS_REQUIRE(engine.GetAsInfo(tlas_initial, tlas_info).ok());
    OS_REQUIRE(tlas_info.type == DxrAsType::TopLevel);
    OS_REQUIRE(tlas_info.primitive_count == 4u);
    OS_REQUIRE(tlas_info.bvh_node_count > 0);
    OS_REQUIRE(tlas_info.gpu_virtual_address != 0);
    OS_REQUIRE((tlas_info.gpu_virtual_address % kDxrAsByteAlignment) == 0);

    // 4. Build aligned Shader Binding Table (RayGen + Miss + HitGroup + Callable)
    DxrShaderTableDesc sbt_desc{};
    sbt_desc.raygen.export_name = "RayGenMain";
    sbt_desc.raygen.local_root_arguments = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    DxrShaderRecordDesc miss_primary{};
    miss_primary.export_name = "MissSkyGradient";
    DxrShaderRecordDesc miss_shadow{};
    miss_shadow.export_name = "MissShadow";
    miss_shadow.local_root_arguments.resize(12, 0xAAu);  // Tests 32B stride rounding -> 64B
    sbt_desc.miss_records = {miss_primary, miss_shadow};

    DxrShaderRecordDesc hit_diffuse{};
    hit_diffuse.export_name = "HitGroupDiffuse";
    hit_diffuse.local_root_arguments.resize(16, 0x55u);
    DxrShaderRecordDesc hit_glossy{};
    hit_glossy.export_name = "HitGroupGlossy";
    hit_glossy.local_root_arguments.resize(40, 0x77u);  // 32 + 40 = 72 -> aligned to 96B
    sbt_desc.hit_group_records = {hit_diffuse, hit_glossy};

    DxrShaderRecordDesc callable_brdf{};
    callable_brdf.export_name = "CallableEvalBrdf";
    sbt_desc.callable_records = {callable_brdf};

    sbt_desc.dispatch_width = 160;
    sbt_desc.dispatch_height = 120;
    sbt_desc.dispatch_depth = 1;

    OS_REQUIRE(engine.BuildShaderBindingTable(sbt_desc, result.sbt).ok());
    OS_REQUIRE(result.sbt.base_gpu_va != 0);
    OS_REQUIRE((result.sbt.base_gpu_va % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE((result.sbt.raygen_offset % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE((result.sbt.miss_offset % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE((result.sbt.hit_group_offset % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE((result.sbt.callable_offset % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE(result.sbt.raygen_record_size == 64u);
    OS_REQUIRE(result.sbt.miss_record_stride == 64u);
    OS_REQUIRE(result.sbt.hit_group_record_stride == 96u);
    OS_REQUIRE(result.sbt.callable_record_stride == 32u);
    OS_REQUIRE((result.sbt.miss_record_stride % kDxrShaderRecordAlignment) == 0);
    OS_REQUIRE((result.sbt.hit_group_record_stride % kDxrShaderRecordAlignment) == 0);
    OS_REQUIRE((result.sbt.callable_record_stride % kDxrShaderRecordAlignment) == 0);
    OS_REQUIRE(result.sbt.uploaded_to_gpu);

    const auto& drd = result.sbt.dispatch_rays_desc;
    OS_REQUIRE(drd.RayGenerationShaderRecord.StartAddress == result.sbt.base_gpu_va + result.sbt.raygen_offset);
    OS_REQUIRE(drd.RayGenerationShaderRecord.SizeInBytes == 64u);
    OS_REQUIRE((drd.MissShaderTable.StartAddress % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE(drd.MissShaderTable.SizeInBytes == 128u);
    OS_REQUIRE(drd.MissShaderTable.StrideInBytes == 64u);
    OS_REQUIRE((drd.HitGroupTable.StartAddress % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE(drd.HitGroupTable.SizeInBytes == 192u);
    OS_REQUIRE(drd.HitGroupTable.StrideInBytes == 96u);
    OS_REQUIRE((drd.CallableShaderTable.StartAddress % kDxrShaderTableAlignment) == 0);
    OS_REQUIRE(drd.CallableShaderTable.SizeInBytes == 32u);
    OS_REQUIRE(drd.CallableShaderTable.StrideInBytes == 32u);
    OS_REQUIRE(drd.Width == 160u && drd.Height == 120u && drd.Depth == 1u);

    // Negative check: misaligned SBT must be rejected by DispatchRays.
    DxrShaderBindingTable bad_sbt = result.sbt;
    bad_sbt.dispatch_rays_desc.HitGroupTable.StartAddress += 16u;
    RtDispatchDesc bad_dispatch{};
    bad_dispatch.width = 16;
    bad_dispatch.height = 16;
    bad_dispatch.sbt = &bad_sbt;
    FrameBufferRgba8 dummy_frame{};
    OS_REQUIRE(engine.DispatchRays(tlas_initial, bad_dispatch, dummy_frame).code == StatusCode::RaytracingError);

    // 5. Execute DispatchRays at 160x120
    RtDispatchDesc dispatch{};
    dispatch.width = 160;
    dispatch.height = 120;
    dispatch.depth = 1;
    dispatch.max_bounces = 2;
    dispatch.sbt = &result.sbt;

    OS_REQUIRE(engine.DispatchRays(tlas_initial,
                                   dispatch,
                                   result.initial_frame,
                                   &result.initial_stats,
                                   &result.initial_hash)
                   .ok());
    OS_REQUIRE(result.initial_frame.width == 160u);
    OS_REQUIRE(result.initial_frame.height == 120u);
    OS_REQUIRE(result.initial_frame.pixels.size() == 160u * 120u * 4u);
    OS_REQUIRE(result.initial_hash != 0);
    OS_REQUIRE(result.initial_hash == engine.LastFrameHash());
    OS_REQUIRE(result.initial_hash == result.initial_frame.ComputeFnv1aHash());
    OS_REQUIRE(result.initial_stats.primary_rays == 160u * 120u);
    OS_REQUIRE(result.initial_stats.primary_hits > 0);
    OS_REQUIRE(result.initial_stats.shadow_rays > 0);
    OS_REQUIRE(result.initial_stats.reflection_rays > 0);
    OS_REQUIRE(result.initial_stats.aabb_tests > 0);
    OS_REQUIRE(result.initial_stats.triangle_tests > 0);

    const double hit_rate = static_cast<double>(result.initial_stats.primary_hits) /
                            static_cast<double>(result.initial_stats.primary_rays);
    OS_REQUIRE(hit_rate > 0.50);

    size_t unique_colors = 0;
    const size_t lit_pixels = CountLitPixels(result.initial_frame, unique_colors);
    OS_REQUIRE(lit_pixels > (160u * 120u) / 2u);
    OS_REQUIRE(unique_colors > 100u);

    // 6. UpdateBlas -> rebuild TLAS -> verify scene updates deterministically
    auto updated_cube_tris = BuildUnitCubeMesh({0.98f, 0.35f, 0.12f}, 0.15f);
    for (auto& tri : updated_cube_tris) {
        tri.v0.y += 0.45f;
        tri.v1.y += 0.45f;
        tri.v2.y += 0.45f;
    }
    OS_REQUIRE(engine.UpdateBlas(cube_blas, updated_cube_tris).ok());

    uint64_t tlas_updated = 0;
    OS_REQUIRE(engine.BuildTlas(instances, kDxrBuildPreferFastTrace, tlas_updated).ok());
    OS_REQUIRE(engine.DispatchRays(tlas_updated,
                                   dispatch,
                                   result.updated_frame,
                                   &result.updated_stats,
                                   &result.updated_hash)
                   .ok());
    OS_REQUIRE(result.updated_hash != 0);
    OS_REQUIRE(result.updated_hash != result.initial_hash);
    OS_REQUIRE(engine.TotalDispatches() == 2u);

    // 7. Destroy AS lifecycle checks
    OS_REQUIRE(engine.DestroyAs(tlas_initial).ok());
    OS_REQUIRE(engine.GetAsInfo(tlas_initial, tlas_info).code == StatusCode::NotFound);
    OS_REQUIRE(engine.DestroyAs(tlas_updated).ok());
    OS_REQUIRE(engine.DestroyAs(cube_blas).ok());
    OS_REQUIRE(engine.DestroyAs(room_blas).ok());

    std::printf("  [%s] Dispatch 160x120: hit_rate=%.2f%% lit=%zu/%u unique_rgb=%zu "
                "hash=0x%016llX updated_hash=0x%016llX\n",
                label,
                hit_rate * 100.0,
                lit_pixels,
                160u * 120u,
                unique_colors,
                static_cast<unsigned long long>(result.initial_hash),
                static_cast<unsigned long long>(result.updated_hash));

    return result;
}

}  // namespace

int main() {
    CallTelemetry::Global().Reset();

    // 1. Direct SAH BVH unit check
    {
        BvhAccelerationStructure bvh;
        const auto tris = BuildDefaultRtTestScene();
        OS_REQUIRE(bvh.Build(tris).ok());
        OS_REQUIRE(bvh.TriangleCount() == tris.size());
        OS_REQUIRE(bvh.NodeCount() > 0);
        OS_REQUIRE(bvh.LeafNodeCount() > 0);
        OS_REQUIRE(bvh.MaxTreeDepth() > 0);

        RtRay ray{};
        ray.origin = {0.0f, 1.0f, -3.5f};
        ray.direction = {0.0f, -0.1f, 0.994987f};
        RtHit hit{};
        RtStats stats{};
        OS_REQUIRE(bvh.TraceRay(ray, hit, &stats));
        OS_REQUIRE(hit.hit && hit.t > 0.0f);
        OS_REQUIRE(stats.aabb_tests > 0 && stats.triangle_tests > 0);
    }

    // 2. ABI constants compile-time verification
    static_assert(kDxrShaderIdentifierSize == 32u, "Shader identifier must be 32 bytes");
    static_assert(kDxrShaderRecordAlignment == 32u, "Shader record alignment must be 32 bytes");
    static_assert(kDxrShaderTableAlignment == 64u, "Shader table alignment must be 64 bytes");
    static_assert(kDxrAsByteAlignment == 256u, "AS alignment must be 256 bytes");
    static_assert(kDxrInstanceDescSize == 64u, "Instance desc must be 64 bytes");
    static_assert(sizeof(DxrGpuVirtualAddressRange) == 16u, "VA range size mismatch");
    static_assert(sizeof(DxrGpuVirtualAddressRangeAndStride) == 24u, "VA range+stride size mismatch");
    static_assert(sizeof(DxrDispatchRaysDesc) == 104u, "DispatchRaysDesc size mismatch");

    // 3. Live High-Performance Hardware Adapter Test (NVIDIA GeForce GTX 1650 Ti on host)
    DxrHardwareEngine hw_engine;
    DxrInitOptions hw_opts{};
    hw_opts.prefer_warp = false;
    OS_REQUIRE(hw_engine.Initialize(hw_opts).ok());
    OS_REQUIRE(hw_engine.IsInitialized());

    const auto& hw_caps = hw_engine.Capabilities();
    OS_REQUIRE(!hw_caps.adapter_description.empty());
    OS_REQUIRE(hw_caps.supports_device5);
    if (hw_caps.hardware_tier == 0u) {
        OS_REQUIRE(!hw_caps.supports_state_objects);
        OS_REQUIRE(!hw_caps.supports_inline_rt);
        OS_REQUIRE(!hw_caps.using_hardware_path);
    } else if (!hw_caps.adapter_is_warp) {
        OS_REQUIRE(hw_caps.supports_state_objects);
        OS_REQUIRE(hw_caps.using_hardware_path);
    }

    const PipelineRunResult hw_res = RunDxrPipelineSession(hw_engine, "HW");
    hw_engine.Shutdown();
    OS_REQUIRE(!hw_engine.IsInitialized());

    // 4. Live D3D12 WARP Adapter Test (prefer_warp = true)
    DxrHardwareEngine warp_engine;
    DxrInitOptions warp_opts{};
    warp_opts.prefer_warp = true;
    OS_REQUIRE(warp_engine.Initialize(warp_opts).ok());
    OS_REQUIRE(warp_engine.IsInitialized());

    const auto& warp_caps = warp_engine.Capabilities();
    OS_REQUIRE(warp_caps.adapter_is_warp);
    OS_REQUIRE(warp_caps.supports_device5);
    OS_REQUIRE(!warp_caps.using_hardware_path);

    const PipelineRunResult warp_res = RunDxrPipelineSession(warp_engine, "WARP");
    warp_engine.Shutdown();

    // 5. Bit-exact deterministic frame & hash parity across Hardware GPU and WARP!
    OS_REQUIRE(hw_res.initial_hash == warp_res.initial_hash);
    OS_REQUIRE(hw_res.updated_hash == warp_res.updated_hash);
    OS_REQUIRE(hw_res.initial_frame.pixels == warp_res.initial_frame.pixels);
    OS_REQUIRE(hw_res.updated_frame.pixels == warp_res.updated_frame.pixels);

    // 6. Direct ID3D12Device* attachment path test
#if defined(_WIN32)
    {
        IDXGIFactory6* factory = nullptr;
        OS_REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) && factory != nullptr);
        IDXGIAdapter1* warp_adapter = nullptr;
        OS_REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter))) && warp_adapter != nullptr);
        ID3D12Device* raw_device = nullptr;
        OS_REQUIRE(SUCCEEDED(D3D12CreateDevice(warp_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&raw_device))) &&
                   raw_device != nullptr);

        DxrHardwareEngine attached_engine;
        OS_REQUIRE(attached_engine.Initialize(raw_device).ok());
        OS_REQUIRE(attached_engine.Capabilities().supports_device5);
        OS_REQUIRE(attached_engine.Capabilities().adapter_is_warp);
        OS_REQUIRE(!attached_engine.Capabilities().adapter_description.empty());

        const PipelineRunResult attached_res = RunDxrPipelineSession(attached_engine, "ATTACHED");
        OS_REQUIRE(attached_res.initial_hash == hw_res.initial_hash);
        OS_REQUIRE(attached_res.updated_hash == hw_res.updated_hash);

        attached_engine.Shutdown();
        raw_device->Release();
        warp_adapter->Release();
        factory->Release();
    }
#endif

    // 7. CallTelemetry verification for dxr_hardware module
    const auto snapshot = CallTelemetry::Global().Snapshot();
    std::unordered_map<std::string, CallTraceEntry> dxr_calls;
    for (const auto& entry : snapshot) {
        if (entry.module_name == "dxr_hardware") {
            dxr_calls[entry.symbol_name] = entry;
            OS_REQUIRE(entry.not_impl_calls == 0);
        }
    }
    const char* required_symbols[] = {
        "Initialize",
        "Shutdown",
        "QueryPrebuildInfo",
        "BuildBlas",
        "UpdateBlas",
        "BuildTlas",
        "GetAsInfo",
        "DestroyAs",
        "BuildShaderBindingTable",
        "DispatchRays",
    };
    for (const char* sym : required_symbols) {
        auto it = dxr_calls.find(sym);
        OS_REQUIRE(it != dxr_calls.end());
        OS_REQUIRE(it->second.ok_calls > 0);
    }

    std::printf("[PASS] test_dxr_live (HW='%s' tier=%u + WARP bit-exact hash=0x%016llX, %zu telemetry ops)\n",
                hw_res.caps.adapter_description.c_str(),
                hw_res.caps.hardware_tier,
                static_cast<unsigned long long>(hw_res.initial_hash),
                dxr_calls.size());
    return 0;
}
