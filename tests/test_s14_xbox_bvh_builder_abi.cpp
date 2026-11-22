// OpenScarlett - MIT License
// Sprint 14 (Worker 3): Verification Suite for:
//   1. Real MSVC-mangled class XboxBvhBuilder C++ ABI exports (>= 12 methods) + legacy C exports
//   2. Direct C++ ABI invocation (Morton64, PrebuildInfoX, Batched/Batched2X, RehydrateX, CpuValidateResultX)
//   3. Dual-path pre-built BVH4 rehydration (Path A DXR 1.1 VA translation + Path B DXR=0 in-place
//      64-bit pointer patching & live GPU compute ray-triangle + ray-AABB parity on HW GPU & WARP)
//   4. LiftDxrStateObjectDesc (XS2 + Xbox-DXBC -> lib_6_5/lib_6_6 DXIL, PC passthrough, SBT remapping, DXR=0 fallback)

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "openscarlett/gpu/bvh4_decoder.h"
#include "openscarlett/gpu/dxr_hardware.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"
#include "openscarlett/gpu/rt_bvh.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/shims/secondary_shims_abi.h"
#include "os_check.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#endif

namespace {

using namespace openscarlett;
namespace fs = std::filesystem;

fs::path GetExeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0u && len < sizeof(buf)) {
        return fs::path(buf).parent_path();
    }
#endif
    std::error_code ec;
    return fs::current_path(ec);
}

std::vector<uint8_t> BuildSampleXs2ShaderBytes() {
    Rdna2Assembler as;
    as.EmitVop1(Rdna2Opcode::V_MOV_B32, 0u, Rdna2Operand::InlineFloat(1.0f));
    as.EmitVop2(Rdna2Opcode::V_ADD_F32, 1u, Rdna2Operand::Vgpr(0u), 0u);
    as.EmitSopp(Rdna2Opcode::S_ENDPGM, 0);

    Xs2ShaderContainer xs2{};
    xs2.version = kXs2ContainerVersion;
    xs2.stage = ShaderStage::Compute;
    xs2.sgpr_count = 16u;
    xs2.vgpr_count = 16u;
    xs2.threadgroup_x = 64u;
    xs2.threadgroup_y = 1u;
    xs2.threadgroup_z = 1u;
    xs2.wave_size = 32u;
    xs2.code_words = as.Words();
    return BuildXs2Container(xs2);
}

// ---------------------------------------------------------------------------
// Test 1: PE Export Table Verification (>= 12 C++ XboxBvhBuilder methods + 9 C exports)
// ---------------------------------------------------------------------------
void Test1_PeExportsAndLegacyCCompat() {
#if defined(_WIN32)
    const fs::path dll_path = GetExeDir() / "XboxBvhBuilder.dll";
    OS_REQUIRE(fs::exists(dll_path));

    PeInspectionReport report{};
    OS_REQUIRE(InspectPeFile(dll_path.string(), report).ok());
    OS_REQUIRE(report.is_pe64 && report.is_dll);

    std::unordered_set<std::string> export_names;
    for (const auto& exp : report.exports) {
        if (!exp.name.empty()) {
            export_names.insert(exp.name);
        }
    }

    // 1a. Verify ALL 9 legacy C exports remain present for 100% backwards compatibility
    static constexpr const char* kLegacyCExports[9] = {
        "XboxBvhCreateBuilder",
        "XboxBvhDestroyBuilder",
        "XboxBvhGetCapabilities",
        "XboxBvhGetPrebuildInfo",
        "XboxBvhBuildBlas",
        "XboxBvhUpdateBlas",
        "XboxBvhBuildTlas",
        "XboxBvhTraceRay",
        "XboxBvhDispatchRays",
    };
    for (const char* sym : kLegacyCExports) {
        OS_REQUIRE(export_names.count(sym) == 1u);
    }

    // 1b. Verify >= 12 distinct mangled C++ class XboxBvhBuilder method exports
    std::vector<std::string> cpp_exports;
    for (const auto& name : export_names) {
#if defined(_MSC_VER)
        if (name.find("XboxBvhBuilder@@") != std::string::npos) {
            cpp_exports.push_back(name);
        }
#elif defined(__MINGW32__)
        if (name.find("14XboxBvhBuilder") != std::string::npos) {
            cpp_exports.push_back(name);
        }
#else
        if (name.find("XboxBvhBuilder") != std::string::npos) {
            cpp_exports.push_back(name);
        }
#endif
    }
    OS_REQUIRE(cpp_exports.size() >= 12u);

    // Check required method substrings exist in exported mangled names
    static constexpr const char* kRequiredCppMethods[] = {
        "Initialize",
        "Shutdown",
        "GetRaytracingAccelerationStructurePrebuildInfoX",
        "BuildRaytracingAccelerationStructureX",
        "BuildRaytracingAccelerationStructureBatchedX",
        "BuildRaytracingAccelerationStructureBatched2X",
        "RehydrateRaytracingAccelerationStructureX",
        "RehydrateRaytracingAccelerationStructureBatchedX",
        "RehydrateRaytracingAccelerationStructureBatched2X",
        "Set64BitMortonCodes",
        "ValidateRehydrateAccelerationStructureBuildDesc",
        "CpuValidateResultX",
        "GetFlags",
        "SetFlags",
        "Release",
        "Destroy",
    };
    for (const char* method : kRequiredCppMethods) {
        bool found = false;
        for (const auto& mangled : cpp_exports) {
            if (mangled.find(method) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "[FAIL] Missing mangled C++ export for XboxBvhBuilder::%s\n", method);
        }
        OS_REQUIRE(found);
    }

    // 1c. Verify live LoadLibraryA + GetProcAddress resolves every mangled C++ export
    HMODULE mod = LoadLibraryA(dll_path.string().c_str());
    OS_REQUIRE(mod != nullptr);
    for (const auto& mangled : cpp_exports) {
        OS_REQUIRE(GetProcAddress(mod, mangled.c_str()) != nullptr);
    }
    FreeLibrary(mod);

    std::printf("[PASS] Test1: XboxBvhBuilder.dll PE exports verified (%zu mangled C++ methods + 9 legacy C exports)\n",
                cpp_exports.size());
#else
    std::printf("[PASS] Test1: skipped PE export check on non-Windows\n");
#endif
}

// ---------------------------------------------------------------------------
// Test 2: Direct C++ ABI Invocation of class XboxBvhBuilder Methods
// ---------------------------------------------------------------------------
void Test2_DirectCppAbiInvocation() {
    // 2a. Static Create + Release lifecycle
    XboxBvhBuilder* heap_builder = nullptr;
    OS_REQUIRE(XboxBvhBuilder::Create(nullptr, &heap_builder) == OS_SHIM_S_OK);
    OS_REQUIRE(heap_builder != nullptr);

    OS_REQUIRE(!heap_builder->Get64BitMortonCodes());
    heap_builder->Set64BitMortonCodes(true);
    OS_REQUIRE(heap_builder->Get64BitMortonCodes());
    OS_REQUIRE((heap_builder->GetFlags() & kXboxBvhBuilderFlag64BitMortonCodes) != 0u);

    heap_builder->SetFlags(kXboxBvhBuilderFlagForceSoftwareBvh4);
    OS_REQUIRE(!heap_builder->Get64BitMortonCodes());
    OS_REQUIRE(heap_builder->GetFlags() == kXboxBvhBuilderFlagForceSoftwareBvh4);

#if defined(_WIN32)
    // 2b. PrebuildInfoX with and without 64-bit Morton codes
    D3D12_RAYTRACING_GEOMETRY_DESC gdesc{};
    gdesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    gdesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    gdesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    gdesc.Triangles.VertexCount = 300u;  // 100 triangles
    gdesc.Triangles.VertexBuffer.StrideInBytes = 12u;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS pre_in{};
    pre_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    pre_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    pre_in.NumDescs = 1u;
    pre_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    pre_in.pGeometryDescs = &gdesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info32{};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info64{};
    heap_builder->Set64BitMortonCodes(false);
    OS_REQUIRE(heap_builder->GetRaytracingAccelerationStructurePrebuildInfoX(&pre_in, &info32) == OS_SHIM_S_OK);
    heap_builder->Set64BitMortonCodes(true);
    OS_REQUIRE(heap_builder->GetRaytracingAccelerationStructurePrebuildInfoX(&pre_in, &info64) == OS_SHIM_S_OK);
    OS_REQUIRE(info32.ResultDataMaxSizeInBytes > 0ull && (info32.ResultDataMaxSizeInBytes % 256ull) == 0ull);
    OS_REQUIRE(info64.ScratchDataSizeInBytes > info32.ScratchDataSizeInBytes);
    OS_REQUIRE((info64.ScratchDataSizeInBytes % 256ull) == 0ull);
    OS_REQUIRE(info32.UpdateScratchDataSizeInBytes > 0ull);

    // 2c. BuildRaytracingAccelerationStructureX / BatchedX / Batched2X
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_descs[2]{};
    build_descs[0].DestAccelerationStructureData = 0x0000002000000000ull;
    build_descs[0].ScratchAccelerationStructureData = 0x0000002010000000ull;
    build_descs[0].Inputs = pre_in;
    build_descs[1].DestAccelerationStructureData = 0x0000002000100000ull;
    build_descs[1].ScratchAccelerationStructureData = 0x0000002010100000ull;
    build_descs[1].Inputs = pre_in;

    OS_REQUIRE(heap_builder->BuildRaytracingAccelerationStructureX(nullptr, &build_descs[0], 0u, nullptr) == OS_SHIM_S_OK);
    OS_REQUIRE(heap_builder->BuildRaytracingAccelerationStructureBatchedX(nullptr, 2u, build_descs) == OS_SHIM_S_OK);

    XboxBvhBatchedBuildDesc batch2{};
    batch2.numDescs = 2u;
    batch2.pDescs = build_descs;
    OS_REQUIRE(heap_builder->BuildRaytracingAccelerationStructureBatched2X(nullptr, &batch2) == OS_SHIM_S_OK);

    // Negative check: unaligned destination VA must fail
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bad_build = build_descs[0];
    bad_build.DestAccelerationStructureData = 0x1001ull;
    OS_REQUIRE(heap_builder->BuildRaytracingAccelerationStructureX(nullptr, &bad_build, 0u, nullptr) < 0);
#endif

    // 2d. Build pre-built RDNA2 BVH4 BLAS & TLAS, validate, rehydrate (single + batched + batched2), and trace!
    const auto cube = BuildUnitCubeMesh({0.3f, 0.7f, 0.9f}, 0.1f);
    BvhAccelerationStructure sah;
    OS_REQUIRE(sah.Build(cube).ok());

    Rdna2BvhEncodeOptions enc{};
    enc.use_box16 = false;
    std::vector<uint8_t> blas_buf;
    Rdna2BvhHeader blas_hdr{};
    OS_REQUIRE(EncodeBvh2ToRdna2Bvh4(sah, enc, blas_buf, &blas_hdr).ok());

    constexpr uint64_t kOrigBlasVa = 0x0000003000000000ull;
    constexpr uint64_t kNewBlasVa  = 0x0000003100000000ull;
    constexpr uint64_t kOrigTlasVa = 0x0000003200000000ull;
    constexpr uint64_t kNewTlasVa  = 0x0000003300000000ull;

    Rdna2BvhTlasInstanceInput tinst{};
    tinst.blas_base_va = kOrigBlasVa;
    tinst.blas_root_ptr = blas_hdr.root_node_ptr;
    tinst.blas_local_aabb = blas_hdr.root_aabb;
    MakeTranslation3x4(0.0f, 0.0f, 4.0f, tinst.object_to_world);
    tinst.instance_id = 55u;
    tinst.instance_mask = 0xFFu;
    tinst.sbt_offset = 3u;

    std::vector<uint8_t> tlas_buf;
    Rdna2BvhHeader tlas_hdr{};
    OS_REQUIRE(BuildRdna2Bvh4Tlas({tinst}, {}, enc, tlas_buf, &tlas_hdr).ok());

    // CpuValidateResultX positive + negative check
    XboxBvhCpuValidationResult vres{};
    OS_REQUIRE(heap_builder->CpuValidateResultX(blas_buf.data(), blas_buf.size(), &vres) == OS_SHIM_S_OK);
    OS_REQUIRE(vres.valid && vres.headerValid && vres.crcValid && vres.boundsValid);
    OS_REQUIRE(vres.visitedLeafNodes == cube.size());

    std::vector<uint8_t> corrupt_buf = blas_buf;
    corrupt_buf[kRdna2BvhHeaderSize + 10u] ^= 0xFFu;
    XboxBvhCpuValidationResult bad_vres{};
    OS_REQUIRE(heap_builder->CpuValidateResultX(corrupt_buf.data(), corrupt_buf.size(), &bad_vres) < 0);
    OS_REQUIRE(!bad_vres.valid);

    // ValidateRehydrateAccelerationStructureBuildDesc positive + negative check
    XboxBvhRehydrateDesc rdescs[2]{};
    rdescs[0].sourceAccelerationStructureVa = kOrigBlasVa;
    rdescs[0].destAccelerationStructureVa = kNewBlasVa;
    rdescs[0].cpuBvhBuffer = blas_buf.data();
    rdescs[0].cpuBvhBufferSize = blas_buf.size();
    rdescs[0].buildFlags = kDxrBuildPreferFastTrace;

    rdescs[1].sourceAccelerationStructureVa = kOrigTlasVa;
    rdescs[1].destAccelerationStructureVa = kNewTlasVa;
    rdescs[1].cpuBvhBuffer = tlas_buf.data();
    rdescs[1].cpuBvhBufferSize = tlas_buf.size();
    rdescs[1].buildFlags = kDxrBuildPreferFastTrace;

    OS_REQUIRE(heap_builder->ValidateRehydrateAccelerationStructureBuildDesc(&rdescs[0]) == OS_SHIM_S_OK);
    XboxBvhRehydrateDesc bad_rdesc = rdescs[0];
    bad_rdesc.destAccelerationStructureVa = 0x123ull;  // unaligned
    OS_REQUIRE(heap_builder->ValidateRehydrateAccelerationStructureBuildDesc(&bad_rdesc) < 0);

    // Rehydrate via Batched2X with CPU validation enabled
    OS_REQUIRE(heap_builder->RehydrateRaytracingAccelerationStructureBatched2X(
                   nullptr, 2u, rdescs, kXboxBvhBuilderFlagEnableCpuValidation) == OS_SHIM_S_OK);
    OS_REQUIRE(heap_builder->GetRehydratedWindowCount() == 2u);

    uint64_t native_blas_va = 0ull;
    OS_REQUIRE(heap_builder->LookupNativeBlasVa(kNewBlasVa, &native_blas_va));
    OS_REQUIRE(native_blas_va != 0ull && (native_blas_va % kDxrAsByteAlignment) == 0ull);

    // Trace a ray against the rehydrated relocated TLAS -> relocated BLAS
    Rdna2RayQueryInput rq{};
    rq.origin = {0.1f, -0.1f, 0.0f};
    rq.direction = {0.0f, 0.0f, 1.0f};
    rq.t_min = 0.001f;
    rq.t_max = 100.0f;
    Rdna2RayQueryHit rhit{};
    OS_REQUIRE(heap_builder->TraceRehydratedRayX(kNewTlasVa, &rq, &rhit) == OS_SHIM_S_OK);
    OS_REQUIRE(rhit.committed_status == kRdna2CommittedTriangleHit);
    OS_REQUIRE(std::fabs(rhit.t - 3.5f) < 1e-4f);  // unit cube [-0.5, 0.5] translated to Z=4.0 -> hit at Z=3.5
    OS_REQUIRE(rhit.instance_id == 55u);
    OS_REQUIRE(rhit.sbt_offset == 3u);

    // Also exercise RehydrateRaytracingAccelerationStructureX & BatchedX on a stack-allocated builder
    {
        XboxBvhBuilder stack_builder;
        OS_REQUIRE(stack_builder.Initialize(nullptr, kXboxBvhBuilderFlagPreferHardwareDxr) == OS_SHIM_S_OK);
        OS_REQUIRE(stack_builder.RehydrateRaytracingAccelerationStructureX(nullptr, &rdescs[0]) == OS_SHIM_S_OK);
        OS_REQUIRE(stack_builder.RehydrateRaytracingAccelerationStructureBatchedX(nullptr, 2u, rdescs) == OS_SHIM_S_OK);
        stack_builder.Shutdown();
    }

    // Release destroys heap_builder cleanly
    OS_REQUIRE(heap_builder->Release() == 0u);
    std::printf("[PASS] Test2: Direct C++ XboxBvhBuilder ABI invocation verified (Morton64, PrebuildX, Batched2X, Rehydrate2X, CpuValidateX)\n");
}

// ---------------------------------------------------------------------------
// Test 3: Dual-Path Pre-Built BVH4 Rehydration (Path A VA Table + Path B DXR=0
//         In-Place 64-bit Pointer Patching & LIVE GPU Compute Triangle + AABB Parity)
// ---------------------------------------------------------------------------
void Test3_DualPathBvh4RehydrationAndLiveGpuParity() {
    // Build pre-built RDNA2 BVH4 BLAS 0 (Triangle quad at Z=0)
    const std::vector<RtTriangle> quad_tris = {
        {{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.9f, 0.2f, 0.2f}, 0.0f},
        {{-1.0f, -1.0f, 0.0f}, {1.0f,  1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f}, {0.9f, 0.2f, 0.2f}, 0.0f},
    };
    BvhAccelerationStructure quad_sah;
    OS_REQUIRE(quad_sah.Build(quad_tris).ok());
    Rdna2BvhEncodeOptions tri_enc{};
    tri_enc.use_box16 = false;
    tri_enc.geometry_index = 1u;
    tri_enc.geometry_flags = 1u;
    std::vector<uint8_t> tri_blas_buf;
    Rdna2BvhHeader tri_blas_hdr{};
    OS_REQUIRE(EncodeBvh2ToRdna2Bvh4(quad_sah, tri_enc, tri_blas_buf, &tri_blas_hdr).ok());

    // Build pre-built RDNA2 BVH4 BLAS 1 (Procedural AABBs: 2 disjoint boxes in object space)
    const std::vector<Rdna2BvhProceduralInput> proc_aabbs = {
        {{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}, 10u, 3u, 1u, {}},
        {{{ 1.0f, -0.4f, -0.4f}, {1.8f, 0.4f, 0.4f}}, 11u, 3u, 1u, {}},
    };
    Rdna2BvhEncodeOptions proc_enc{};
    proc_enc.use_box16 = false;
    std::vector<uint8_t> proc_blas_buf;
    Rdna2BvhHeader proc_blas_hdr{};
    OS_REQUIRE(BuildRdna2Bvh4ProceduralBlas(proc_aabbs, proc_enc, proc_blas_buf, &proc_blas_hdr).ok());

    // Original serialized Xbox GPU VAs vs relocated destination VAs
    constexpr uint64_t kOrigTriBlasVa  = 0x0000001000000000ull;
    constexpr uint64_t kOrigProcBlasVa = 0x0000001200000000ull;
    constexpr uint64_t kOrigTlasVa     = 0x0000001400000000ull;

    constexpr uint64_t kNewTriBlasVa   = 0x0000004000000000ull;
    constexpr uint64_t kNewProcBlasVa  = 0x0000004200000000ull;
    constexpr uint64_t kNewTlasVa      = 0x0000004400000000ull;

    // Build pre-built RDNA2 BVH4 TLAS referencing ORIGINAL Xbox BLAS VAs
    std::vector<Rdna2BvhTlasInstanceInput> orig_insts(2);
    // Instance 0: Triangle quad translated to Z = +3.0 (id=101, mask=0x01, sbt=7)
    orig_insts[0].blas_base_va = kOrigTriBlasVa;
    orig_insts[0].blas_root_ptr = tri_blas_hdr.root_node_ptr;
    orig_insts[0].blas_local_aabb = tri_blas_hdr.root_aabb;
    MakeTranslation3x4(0.0f, 0.0f, 3.0f, orig_insts[0].object_to_world);
    orig_insts[0].instance_id = 101u;
    orig_insts[0].instance_mask = 0x01u;
    orig_insts[0].sbt_offset = 7u;
    orig_insts[0].instance_flags = kDxrInstanceNone;

    // Instance 1: Procedural AABB BLAS translated to (2.5, 0.0, 4.0) (id=202, mask=0x02, sbt=13)
    orig_insts[1].blas_base_va = kOrigProcBlasVa;
    orig_insts[1].blas_root_ptr = proc_blas_hdr.root_node_ptr;
    orig_insts[1].blas_local_aabb = proc_blas_hdr.root_aabb;
    MakeTranslation3x4(2.5f, 0.0f, 4.0f, orig_insts[1].object_to_world);
    orig_insts[1].instance_id = 202u;
    orig_insts[1].instance_mask = 0x02u;
    orig_insts[1].sbt_offset = 13u;
    orig_insts[1].instance_flags = kDxrInstanceNone;

    std::vector<uint8_t> tlas_buf;
    Rdna2BvhHeader orig_tlas_hdr{};
    OS_REQUIRE(BuildRdna2Bvh4Tlas(orig_insts, {}, tri_enc, tlas_buf, &orig_tlas_hdr).ok());

    // Negative bounds/alignment checks on PatchRdna2Bvh4PointersInPlace
    {
        std::vector<uint8_t> tmp = tlas_buf;
        OS_REQUIRE(!PatchRdna2Bvh4PointersInPlace(tmp.data(), tmp.size(), kOrigTlasVa, 0x101ull, {}).ok());
        OS_REQUIRE(!PatchRdna2Bvh4PointersInPlace(tmp.data(), 32u, kOrigTlasVa, kNewTlasVa, {}).ok());
    }

    // Initialize DxrHardwareEngine + XboxBvh4RehydrationManager on live host GPU
    DxrHardwareEngine engine;
    DxrInitOptions dxr_opts{};
    dxr_opts.prefer_warp = false;
    OS_REQUIRE(engine.Initialize(dxr_opts).ok());

    XboxBvh4RehydrationManager mgr;
    OS_REQUIRE(mgr.Initialize(&engine, false).ok());

    const Bvh4RehydrationPath expected_path =
        engine.using_hardware_path() ? Bvh4RehydrationPath::HardwareDxr11
                                     : Bvh4RehydrationPath::ComputeBvh4Fallback;
    OS_REQUIRE(mgr.ActivePath() == expected_path);

    // Rehydrate both BLASes (relocating Xbox VAs) and TLAS (in-place 64-bit pointer patching + VA translation!)
    XboxBvh4RehydrateResult tri_res{}, proc_res{}, tlas_res{};
    OS_REQUIRE(mgr.RehydrateBlas(kOrigTriBlasVa, kNewTriBlasVa, tri_blas_buf,
                                 kDxrBuildPreferFastTrace, tri_res).ok());
    OS_REQUIRE(tri_res.decoded_triangle_count == 2u);
    OS_REQUIRE(tri_res.native_pc_gpu_va != 0ull && (tri_res.native_pc_gpu_va % kDxrAsByteAlignment) == 0ull);
    OS_REQUIRE(tri_res.host_as_id != 0ull);
    OS_REQUIRE(tri_res.hardware_dxr_built == engine.using_hardware_path());

    OS_REQUIRE(mgr.RehydrateBlas(kOrigProcBlasVa, kNewProcBlasVa, proc_blas_buf,
                                 kDxrBuildPreferFastTrace, proc_res).ok());
    OS_REQUIRE(proc_res.decoded_procedural_count == 2u);
    OS_REQUIRE(proc_res.native_pc_gpu_va != 0ull && (proc_res.native_pc_gpu_va % kDxrAsByteAlignment) == 0ull);

    OS_REQUIRE(mgr.RehydrateTlas(kOrigTlasVa, kNewTlasVa, tlas_buf,
                                 kDxrBuildPreferFastTrace, tlas_res).ok());
    OS_REQUIRE(tlas_res.decoded_instance_count == 2u);
    OS_REQUIRE(tlas_res.patched_instance_pointers == 2u);  // BOTH 64-bit BLAS VAs patched in-place!
    OS_REQUIRE(tlas_res.translated_tlas_instances.size() == 2u);

    // Verify in-place patched TLAS buffer passes strict validation & carries relocated 64-bit BLAS VAs!
    Rdna2Bvh4DecodedLeaves patched_leaves{};
    OS_REQUIRE(ExtractRdna2Bvh4Leaves(tlas_buf.data(), tlas_buf.size(), patched_leaves).ok());
    OS_REQUIRE(patched_leaves.instances.size() == 2u);
    bool found_tri_reloc = false;
    bool found_proc_reloc = false;
    for (const auto& inode : patched_leaves.instances) {
        if (inode.instance_id == 101u) {
            OS_REQUIRE(inode.blas_base_va == kNewTriBlasVa);
            OS_REQUIRE(inode.instance_mask == 0x01u);
            OS_REQUIRE(inode.sbt_offset == 7u);
            found_tri_reloc = true;
        } else if (inode.instance_id == 202u) {
            OS_REQUIRE(inode.blas_base_va == kNewProcBlasVa);
            OS_REQUIRE(inode.instance_mask == 0x02u);
            OS_REQUIRE(inode.sbt_offset == 13u);
            found_proc_reloc = true;
        }
    }
    OS_REQUIRE(found_tri_reloc && found_proc_reloc);

    // Verify translated TLAS instances preserved SBT offsets, instance IDs, masks, and transforms
    bool found_t0 = false;
    bool found_t1 = false;
    for (const auto& di : tlas_res.translated_tlas_instances) {
        if (di.instance_id == 101u) {
            OS_REQUIRE(di.blas_id == tri_res.host_as_id);
            OS_REQUIRE(di.instance_mask == 0x01u);
            OS_REQUIRE(di.instance_contribution_to_hit_group_index == 7u);
            OS_REQUIRE(std::fabs(di.transform[2][3] - 3.0f) < 1e-5f);
            found_t0 = true;
        } else if (di.instance_id == 202u) {
            OS_REQUIRE(di.instance_mask == 0x02u);
            OS_REQUIRE(di.instance_contribution_to_hit_group_index == 13u);
            OS_REQUIRE(std::fabs(di.transform[0][3] - 2.5f) < 1e-5f);
            OS_REQUIRE(std::fabs(di.transform[2][3] - 4.0f) < 1e-5f);
            found_t1 = true;
        }
    }
    OS_REQUIRE(found_t0 && found_t1);

    // Thread-safety stress check on the 64-bit Xbox GPU VA -> native PC VA translation table
    {
        std::vector<std::thread> readers;
        for (int th = 0; th < 4; ++th) {
            readers.emplace_back([&]() {
                for (int iter = 0; iter < 128; ++iter) {
                    XboxVaTranslationEntry e0{}, e1{};
                    OS_REQUIRE(mgr.TranslateXboxVaToNativePcVa(kNewTriBlasVa, e0));
                    OS_REQUIRE(e0.native_pc_gpu_va == tri_res.native_pc_gpu_va);
                    OS_REQUIRE(mgr.TranslateXboxVaToNativePcVa(kNewProcBlasVa, e1));
                    OS_REQUIRE(e1.native_pc_gpu_va == proc_res.native_pc_gpu_va);
                }
            });
        }
        for (auto& th : readers) {
            th.join();
        }
    }

    // Export rehydrated multi-window BVH4 table and dispatch LIVE on Host GPU AND D3D12 WARP!
    Rdna2Bvh64MultiWindow rehydrated_windows;
    OS_REQUIRE(mgr.CopyMultiWindow(rehydrated_windows).ok());
    OS_REQUIRE(rehydrated_windows.WindowCount() == 3u);

    std::vector<Rdna2RayQueryInput> rays;
    // Ray 0: hits Triangle Instance 0 at (0.25, -0.25, 3.0) -> exact t=3.0, u=0.25, v=0.375
    rays.push_back({{0.25f, -0.25f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u});
    // Ray 1: hits Procedural AABB 0 of Instance 1 (world X=[2.0,3.0], Y=[-0.5,0.5], Z=[3.5,4.5]) -> exact t=3.5
    rays.push_back({{2.5f, 0.0f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u});
    // Ray 2: hits Procedural AABB 1 of Instance 1 (world X=[3.5,4.3], Y=[-0.4,0.4], Z=[3.6,4.4]) -> exact t=3.6
    rays.push_back({{3.9f, 0.1f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u});
    // Ray 3: masked out procedural ray (mask=0x01 skips Instance 1 with mask=0x02) -> miss!
    rays.push_back({{2.5f, 0.0f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0x01u, 0u, 0u});
    // Ray 4: clean miss in empty gap between instances
    rays.push_back({{1.5f, 0.0f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u});

    // Dense 8x8 ray sweep across both triangle quad and both procedural AABBs
    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            const float ox = -1.2f + 5.8f * (static_cast<float>(x) + 0.5f) / 8.0f + 0.011f;
            const float oy = -1.1f + 2.2f * (static_cast<float>(y) + 0.5f) / 8.0f - 0.017f;
            rays.push_back({{ox, oy, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u});
        }
    }

    // Build exact CPU SAH + procedural AABB reference for every ray!
    std::vector<RtTriangle> world_quad_tris;
    for (const auto& t : quad_tris) {
        world_quad_tris.push_back(TransformTriangle3x4(orig_insts[0].object_to_world, t));
    }
    BvhAccelerationStructure world_quad_bvh;
    OS_REQUIRE(world_quad_bvh.Build(world_quad_tris).ok());

    std::vector<Rdna2BvhProceduralInput> world_procs = proc_aabbs;
    for (auto& p : world_procs) {
        p.bounds.min = TransformPoint3x4(orig_insts[1].object_to_world, p.bounds.min);
        p.bounds.max = TransformPoint3x4(orig_insts[1].object_to_world, p.bounds.max);
    }

    std::vector<Rdna2RayQueryHit> cpu_hits(rays.size());
    uint32_t tri_hit_count = 0u;
    uint32_t proc_hit_count = 0u;
    for (size_t i = 0; i < rays.size(); ++i) {
        OS_REQUIRE(rehydrated_windows.TraceRay64(kNewTlasVa, rays[i], cpu_hits[i]).ok());

        // Cross-check against independent CPU SAH + TraceRayProceduralAabbs reference!
        RtRay rr{rays[i].origin, rays[i].direction, rays[i].t_min, rays[i].t_max};
        RtHit sah_hit{};
        const bool hit_tri = ((rays[i].instance_mask & 0x01u) != 0u) && world_quad_bvh.TraceRay(rr, sah_hit);
        RtProceduralHit aabb_hit{};
        const bool hit_proc = ((rays[i].instance_mask & 0x02u) != 0u) &&
                              TraceRayProceduralAabbs(world_procs, rr, hit_tri ? sah_hit.t : rr.t_max, false, aabb_hit);

        if (hit_proc) {
            OS_REQUIRE(cpu_hits[i].committed_status == kRdna2CommittedProceduralHit);
            OS_REQUIRE(std::fabs(cpu_hits[i].t - aabb_hit.t) < 1e-4f);
            OS_REQUIRE(cpu_hits[i].primitive_index == aabb_hit.primitive_index);
            OS_REQUIRE(cpu_hits[i].geometry_index == aabb_hit.geometry_index);
            OS_REQUIRE(cpu_hits[i].instance_id == 202u);
            OS_REQUIRE(cpu_hits[i].sbt_offset == 13u);
            ++proc_hit_count;
        } else if (hit_tri) {
            OS_REQUIRE(cpu_hits[i].committed_status == kRdna2CommittedTriangleHit);
            OS_REQUIRE(std::fabs(cpu_hits[i].t - sah_hit.t) < 1e-4f);
            OS_REQUIRE(cpu_hits[i].instance_id == 101u);
            OS_REQUIRE(cpu_hits[i].sbt_offset == 7u);
            ++tri_hit_count;
        } else {
            OS_REQUIRE(cpu_hits[i].committed_status == kRdna2CommittedNothing);
        }
    }
    OS_REQUIRE(tri_hit_count >= 5u && proc_hit_count >= 4u);

    // Analytic closed-form checks on rays 0..4
    OS_REQUIRE(cpu_hits[0].committed_status == kRdna2CommittedTriangleHit && std::fabs(cpu_hits[0].t - 3.0f) < 1e-5f);
    OS_REQUIRE(cpu_hits[1].committed_status == kRdna2CommittedProceduralHit &&
               std::fabs(cpu_hits[1].t - 3.5f) < 1e-5f && cpu_hits[1].primitive_index == 10u);
    OS_REQUIRE(cpu_hits[2].committed_status == kRdna2CommittedProceduralHit &&
               std::fabs(cpu_hits[2].t - 3.6f) < 1e-5f && cpu_hits[2].primitive_index == 11u);
    OS_REQUIRE(cpu_hits[3].committed_status == kRdna2CommittedNothing);
    OS_REQUIRE(cpu_hits[4].committed_status == kRdna2CommittedNothing);

    // Execute LIVE on BOTH Host Hardware GPU AND D3D12 WARP!
    auto run_gpu_parity = [&](bool prefer_warp, const char* label) {
        Rdna2RayQueryExecutor exec;
        OS_REQUIRE(exec.Initialize(prefer_warp, true).ok());
        std::vector<Rdna2RayQueryHit> gpu_hits;
        Rdna2RayQueryDispatchReport rep{};
        OS_REQUIRE(exec.DispatchRayQueries(rehydrated_windows, kNewTlasVa, rays, gpu_hits, &rep).ok());
        OS_REQUIRE(rep.executed_on_gpu);
        OS_REQUIRE(gpu_hits.size() == cpu_hits.size());

        for (size_t i = 0; i < rays.size(); ++i) {
            OS_REQUIRE(gpu_hits[i].committed_status == cpu_hits[i].committed_status);
            if (cpu_hits[i].committed_status != kRdna2CommittedNothing) {
                OS_REQUIRE(std::fabs(gpu_hits[i].t - cpu_hits[i].t) < 1e-4f);
                OS_REQUIRE(gpu_hits[i].primitive_index == cpu_hits[i].primitive_index);
                OS_REQUIRE(gpu_hits[i].instance_id == cpu_hits[i].instance_id);
                OS_REQUIRE(gpu_hits[i].geometry_index == cpu_hits[i].geometry_index);
                OS_REQUIRE(gpu_hits[i].sbt_offset == cpu_hits[i].sbt_offset);
                if (cpu_hits[i].committed_status == kRdna2CommittedTriangleHit) {
                    OS_REQUIRE(std::fabs(gpu_hits[i].bary_u - cpu_hits[i].bary_u) < 1e-4f);
                    OS_REQUIRE(std::fabs(gpu_hits[i].bary_v - cpu_hits[i].bary_v) < 1e-4f);
                }
            }
        }
        std::printf("  [%s] Adapter='%s' Tier=%u rehydrated multi-window BVH4: %u tri + %u AABB hits == CPU SAH exact!\n",
                    label, rep.adapter_description.c_str(), rep.hardware_dxr_tier, tri_hit_count, proc_hit_count);
        exec.Shutdown();
    };

    run_gpu_parity(false, "HW-GPU");
    run_gpu_parity(true, "WARP");

    engine.Shutdown();
    std::printf("[PASS] Test3: Dual-path BVH4 rehydration (VA table + in-place 64-bit patch + live GPU tri & AABB parity)\n");
}

// ---------------------------------------------------------------------------
// Test 4: LiftDxrStateObjectDesc (XS2 + Xbox-DXBC -> lib_6_5 / lib_6_6 DXIL,
//         native PC passthrough, SBT identifier remapping, DXR=0 fallback plan)
// ---------------------------------------------------------------------------
void Test4_LiftDxrStateObjectDesc() {
#if defined(_WIN32)
    // Build 1) Raw XS2\0 blob, 2) Xbox-wrapped DXBC ("XBOX" chunk) blob, 3) Native PC DXBC blob
    const std::vector<uint8_t> xs2_raw = BuildSampleXs2ShaderBytes();
    OS_REQUIRE(IsXboxShaderBytecode(xs2_raw.data(), xs2_raw.size()));

    XboxDxbcBuildDesc xdxbc_desc{};
    xdxbc_desc.xs2_blob = xs2_raw;
    xdxbc_desc.xbox_chunk_fourcc = "XBOX";
    const std::vector<uint8_t> xbox_dxbc = BuildXboxDxbcContainer(xdxbc_desc);
    OS_REQUIRE(IsXboxShaderBytecode(xbox_dxbc.data(), xbox_dxbc.size()));

    // Synthetic standard PC DXBC container (no "XBOX"/"XS2M" chunk -> native passthrough)
    std::vector<uint8_t> pc_dxil_passthrough(64u, 0u);
    pc_dxil_passthrough[0] = 'D';
    pc_dxil_passthrough[1] = 'X';
    pc_dxil_passthrough[2] = 'B';
    pc_dxil_passthrough[3] = 'C';
    OS_REQUIRE(!IsXboxShaderBytecode(pc_dxil_passthrough.data(), pc_dxil_passthrough.size()));

    D3D12_EXPORT_DESC lib0_exports[2]{};
    lib0_exports[0].Name = L"XboxRayGen";
    lib0_exports[1].Name = L"XboxMissSky";
    D3D12_DXIL_LIBRARY_DESC lib0{};
    lib0.DXILLibrary.pShaderBytecode = xs2_raw.data();
    lib0.DXILLibrary.BytecodeLength = xs2_raw.size();
    lib0.NumExports = 2u;
    lib0.pExports = lib0_exports;

    D3D12_EXPORT_DESC lib1_exports[3]{};
    lib1_exports[0].Name = L"XboxChsMesh";
    lib1_exports[1].Name = L"XboxAhsAlpha";
    lib1_exports[2].Name = L"XboxCallableShade";
    D3D12_DXIL_LIBRARY_DESC lib1{};
    lib1.DXILLibrary.pShaderBytecode = xbox_dxbc.data();
    lib1.DXILLibrary.BytecodeLength = xbox_dxbc.size();
    lib1.NumExports = 3u;
    lib1.pExports = lib1_exports;

    D3D12_EXPORT_DESC lib2_exports[1]{};
    lib2_exports[0].Name = L"PcMissFallback";
    D3D12_DXIL_LIBRARY_DESC lib2{};
    lib2.DXILLibrary.pShaderBytecode = pc_dxil_passthrough.data();
    lib2.DXILLibrary.BytecodeLength = pc_dxil_passthrough.size();
    lib2.NumExports = 1u;
    lib2.pExports = lib2_exports;

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"HitGroupMesh";
    hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = L"XboxChsMesh";
    hg.AnyHitShaderImport = L"XboxAhsAlpha";

    D3D12_RAYTRACING_SHADER_CONFIG shader_cfg{};
    shader_cfg.MaxPayloadSizeInBytes = 48u;
    shader_cfg.MaxAttributeSizeInBytes = 8u;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipe_cfg{};
    pipe_cfg.MaxTraceRecursionDepth = 2u;

    D3D12_STATE_SUBOBJECT subs[7]{};
    subs[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib0};
    subs[1] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib1};
    subs[2] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib2};
    subs[3] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg};
    subs[4] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_cfg};
    subs[5] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipe_cfg};

    LPCWSTR assoc_exports[2] = {L"XboxRayGen", L"HitGroupMesh"};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc{};
    assoc.pSubobjectToAssociate = &subs[4];
    assoc.NumExports = 2u;
    assoc.pExports = assoc_exports;
    subs[6] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &assoc};

    D3D12_STATE_OBJECT_DESC so_desc{};
    so_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    so_desc.NumSubobjects = 7u;
    so_desc.pSubobjects = subs;

    const bool has_dxc = !ResolvePortableDxcPath().empty();

    // Test both lib_6_5 (DXR=0 graceful fallback, 32B IDs) and lib_6_6 (DXR 1.1, 64B IDs)
    for (int mode = 0; mode < 2; ++mode) {
        DxrStateObjectLiftOptions lift_opts{};
        lift_opts.prefer_sm66 = (mode == 1);
        lift_opts.host_dxr_tier = (mode == 0) ? 0u : static_cast<uint32_t>(DxrTier::Tier1_1);
        lift_opts.compile_with_dxc = true;
        lift_opts.xbox_identifier_size_bytes = (mode == 0) ? 32u : 64u;

        DxrLiftedStateObjectResult lifted{};
        Status st = LiftDxrStateObjectDesc(&so_desc, lift_opts, lifted);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] LiftDxrStateObjectDesc mode=%d failed: %s\n", mode, st.message.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(lifted.modified);
        OS_REQUIRE(lifted.total_subobjects == 7u);
        OS_REQUIRE(lifted.lifted_xbox_libraries == 2u);
        OS_REQUIRE(lifted.passthrough_pc_libraries == 1u);
        OS_REQUIRE(lifted.discovered_hit_groups == 1u);
        OS_REQUIRE(lifted.libraries.size() == 3u);

        const std::string expected_profile = (mode == 1) ? "lib_6_6" : "lib_6_5";
        OS_REQUIRE(lifted.libraries[0].was_xbox_shader && lifted.libraries[0].target_profile == expected_profile);
        OS_REQUIRE(lifted.libraries[1].was_xbox_shader && lifted.libraries[1].target_profile == expected_profile);
        OS_REQUIRE(!lifted.libraries[2].was_xbox_shader);  // Native PC passthrough!

        const D3D12_STATE_OBJECT_DESC* rewritten = lifted.LiftedDesc();
        OS_REQUIRE(rewritten != nullptr && rewritten->NumSubobjects == 7u);

        // Verify SUBOBJECT_TO_EXPORTS_ASSOCIATION pointer was fixed up into rewritten subobjects array!
        const auto* rewritten_assoc = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(
            rewritten->pSubobjects[6].pDesc);
        OS_REQUIRE(rewritten_assoc != nullptr);
        OS_REQUIRE(rewritten_assoc->pSubobjectToAssociate == &rewritten->pSubobjects[4]);

        if (has_dxc) {
            for (size_t li = 0u; li < 2u; ++li) {
                OS_REQUIRE(lifted.libraries[li].compiled_to_dxil);
                OS_REQUIRE(lifted.libraries[li].pc_dxil_bytes.size() > 64u);
                OS_REQUIRE(lifted.libraries[li].pc_dxil_bytes[0] == 'D' &&
                           lifted.libraries[li].pc_dxil_bytes[1] == 'X' &&
                           lifted.libraries[li].pc_dxil_bytes[2] == 'B' &&
                           lifted.libraries[li].pc_dxil_bytes[3] == 'C');
                OS_REQUIRE(VerifyDxbcContainerHash(lifted.libraries[li].pc_dxil_bytes.data(),
                                                   lifted.libraries[li].pc_dxil_bytes.size()));

                // Confirm rewritten D3D12_DXIL_LIBRARY_DESC points to lifted DXIL bytes (not original Xbox bytes!)
                const auto* rlib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(
                    rewritten->pSubobjects[li].pDesc);
                OS_REQUIRE(rlib != nullptr);
                OS_REQUIRE(rlib->DXILLibrary.BytecodeLength == lifted.libraries[li].pc_dxil_bytes.size());
                OS_REQUIRE(!IsXboxShaderBytecode(rlib->DXILLibrary.pShaderBytecode,
                                                 rlib->DXILLibrary.BytecodeLength));
            }
        }

        // Verify native PC DXIL library remained untouched passthrough
        const auto* rlib_pc = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(rewritten->pSubobjects[2].pDesc);
        OS_REQUIRE(rlib_pc->DXILLibrary.pShaderBytecode == pc_dxil_passthrough.data());
        OS_REQUIRE(rlib_pc->DXILLibrary.BytecodeLength == pc_dxil_passthrough.size());

        // Verify DXR=0 graceful fallback plan metadata
        OS_REQUIRE(lifted.fallback_plan.max_payload_size_bytes == 48u);
        OS_REQUIRE(lifted.fallback_plan.max_attribute_size_bytes == 8u);
        OS_REQUIRE(lifted.fallback_plan.max_recursion_depth == 2u);
        OS_REQUIRE(lifted.fallback_plan.raygen_count == 1u);
        OS_REQUIRE(lifted.fallback_plan.miss_count == 2u);  // XboxMissSky + PcMissFallback
        OS_REQUIRE(lifted.fallback_plan.hit_group_count == 1u);
        OS_REQUIRE(lifted.fallback_plan.callable_count == 1u);
        if (mode == 0) {
            OS_REQUIRE(lifted.fallback_plan.requires_software_fallback);
            OS_REQUIRE(lifted.fallback_plan.fallback_backend == Rdna2RayQueryBackend::ComputeBvh4Fallback);
            OS_REQUIRE(!lifted.fallback_plan.reason.empty());
        } else {
            OS_REQUIRE(!lifted.fallback_plan.requires_software_fallback);
            OS_REQUIRE(lifted.fallback_plan.fallback_backend == Rdna2RayQueryBackend::HardwareDxr11);
        }

        // Verify lifted SBT identifier mappings wire directly into DxrStateObjectSbtRemapper!
        DxrHardwareEngine sbt_eng;
        DxrInitOptions sopts{};
        sopts.prefer_warp = true;
        OS_REQUIRE(sbt_eng.Initialize(sopts).ok());

        DxrStateObjectSbtRemapper remapper;
        uint64_t so_id = 0ull;
        OS_REQUIRE(remapper.CreateStateObject(lifted.BuildSbtRemapperDesc(), so_id).ok());

        auto make_rec = [&](const std::string& name, const std::vector<uint8_t>& args) {
            std::vector<uint8_t> xid;
            OS_REQUIRE(remapper.GetXboxShaderIdentifier(so_id, name, xid).ok());
            OS_REQUIRE(xid.size() == lift_opts.xbox_identifier_size_bytes);
            XboxSbtRecordInput r{};
            r.raw_record_bytes = xid;
            r.raw_record_bytes.insert(r.raw_record_bytes.end(), args.begin(), args.end());
            return r;
        };

        XboxDispatchRaysInput din{};
        din.raygen_record = make_rec("XboxRayGen", {0x11, 0x22, 0x33, 0x44});
        din.miss_records = {make_rec("XboxMissSky", {}), make_rec("PcMissFallback", {})};
        din.hit_group_records = {make_rec("HitGroupMesh", {0xAA, 0xBB, 0xCC, 0xDD})};
        din.callable_records = {make_rec("XboxCallableShade", {0x77, 0x88})};
        din.width = 16u;
        din.height = 16u;

        DxrRemappedSbtResult remapped{};
        OS_REQUIRE(remapper.RemapDispatchRaysSbt(so_id, sbt_eng, din, remapped).ok());
        OS_REQUIRE(remapped.remapped_record_count == 5u);
        OS_REQUIRE(remapped.sw_raygen.export_name == "XboxRayGen");
        OS_REQUIRE(remapped.sw_hit_groups.size() == 1u &&
                   remapped.sw_hit_groups[0].export_name == "HitGroupMesh");
        OS_REQUIRE(remapped.sw_callables.size() == 1u &&
                   remapped.sw_callables[0].export_name == "XboxCallableShade");
        OS_REQUIRE(remapper.DestroyStateObject(so_id).ok());
        sbt_eng.Shutdown();
    }

    // Negative check: null pDesc returns InvalidArgument cleanly
    DxrLiftedStateObjectResult dummy{};
    OS_REQUIRE(LiftDxrStateObjectDesc(nullptr, {}, dummy).code == StatusCode::InvalidArgument);

    std::printf("[PASS] Test4: LiftDxrStateObjectDesc verified (XS2 + Xbox-DXBC -> lib_6_5/lib_6_6 dxc=%s, PC passthrough, SBT remap, DXR=0 fallback)\n",
                has_dxc ? "yes" : "no");
#else
    std::printf("[PASS] Test4: skipped Win32 D3D12_STATE_OBJECT_DESC check on non-Windows\n");
#endif
}

}  // namespace

int main() {
    std::printf("=== OpenScarlett Sprint 14 XboxBvhBuilder C++ ABI, Dual-Path BVH4 & DXR StateObject Lifter Suite ===\n");
    Test1_PeExportsAndLegacyCCompat();
    Test2_DirectCppAbiInvocation();
    Test3_DualPathBvh4RehydrationAndLiveGpuParity();
    Test4_LiftDxrStateObjectDesc();
    std::printf("ALL SPRINT 14 XBOXBVHBUILDER ABI / DUAL-PATH BVH4 / DXR LIFTER TESTS PASSED!\n");
    return 0;
}
