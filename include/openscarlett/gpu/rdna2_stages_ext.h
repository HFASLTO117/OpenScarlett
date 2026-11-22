#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/dxr_hardware.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"

#if defined(_WIN32)
struct ID3D12Device;
#endif

namespace openscarlett {

// ===========================================================================
// 1. Hull (HS), Domain (DS), and Geometry (GS) Stage Contracts & Lifting
// ===========================================================================

enum class TessDomain : uint8_t {
    Tri = 0,
    Quad = 1,
    Isoline = 2,
};

enum class TessPartitioning : uint8_t {
    Integer = 0,
    FractionalEven = 1,
    FractionalOdd = 2,
    Pow2 = 3,
};

enum class TessOutputTopology : uint8_t {
    Point = 0,
    Line = 1,
    TriangleCw = 2,
    TriangleCcw = 3,
};

struct Rdna2HullStageContract {
    TessDomain domain = TessDomain::Tri;
    TessPartitioning partitioning = TessPartitioning::FractionalOdd;
    TessOutputTopology output_topology = TessOutputTopology::TriangleCw;
    uint32_t input_control_points = 3u;
    uint32_t output_control_points = 3u;
    float max_tess_factor = 64.0f;
    float default_outer_tess = 4.0f;
    float default_inner_tess = 4.0f;
};

struct Rdna2DomainStageContract {
    TessDomain domain = TessDomain::Tri;
    TessPartitioning partitioning = TessPartitioning::FractionalOdd;
    TessOutputTopology output_topology = TessOutputTopology::TriangleCw;
    uint32_t input_control_points = 3u;
};

enum class GsInputPrimitive : uint8_t {
    Point = 0,
    Line = 1,
    Triangle = 2,
    LineAdj = 3,
    TriangleAdj = 4,
};

enum class GsOutputPrimitive : uint8_t {
    PointStream = 0,
    LineStream = 1,
    TriangleStream = 2,
};

struct Rdna2GeometryStageContract {
    GsInputPrimitive input_primitive = GsInputPrimitive::Triangle;
    GsOutputPrimitive output_primitive = GsOutputPrimitive::TriangleStream;
    uint32_t max_vertex_count = 3u;
    uint32_t instance_count = 1u;
    bool detect_sendmsg_emit_cut = true;
};

// Public AMD GFX10.3 S_SENDMSG GS_EMIT / GS_CUT decoding.
constexpr uint8_t kRdna2SendMsgIdGs = 2u;
constexpr uint8_t kRdna2SendMsgIdGsDone = 3u;
constexpr uint8_t kRdna2GsOpNop = 0u;
constexpr uint8_t kRdna2GsOpCut = 1u;
constexpr uint8_t kRdna2GsOpEmit = 2u;
constexpr uint8_t kRdna2GsOpEmitCut = 3u;

struct Rdna2GsSendMsgInfo {
    bool is_gs_message = false;
    bool is_done = false;
    bool emit_vertex = false;
    bool cut_primitive = false;
    uint8_t msg_id = 0u;
    uint8_t op = 0u;
    uint8_t stream_id = 0u;
};

inline constexpr int16_t PackRdna2GsSendMsg(
    uint8_t msg_id,
    uint8_t op,
    uint8_t stream_id = 0u) noexcept {
    return static_cast<int16_t>(
        (static_cast<uint16_t>(msg_id & 0xFu)) |
        (static_cast<uint16_t>(op & 0x7u) << 4u) |
        (static_cast<uint16_t>(stream_id & 0x3u) << 8u));
}

Rdna2GsSendMsgInfo DecodeRdna2GsSendMsg(int16_t simm16) noexcept;

struct Rdna2ExtendedLiftOptions {
    bool emit_hlsl = true;
    bool emit_spirv = true;
    bool compile_dxil_with_dxc = true;
    bool resign_dxil = true;
    bool validate_spirv_external = false;
    std::string target_profile_override;  // e.g., "hs_6_5", "ds_6_5", "gs_6_5"
    std::string dxc_path;
    std::string spirv_val_path = kDefaultSpirvValPath;
    std::string spirv_target_env = "vulkan1.2";
};

struct Rdna2ExtendedStageLiftResult {
    ShaderStage stage = ShaderStage::Unknown;
    std::vector<Rdna2Instruction> instructions;
    uint32_t lifted_alu_ops = 0u;
    uint32_t gs_emit_count = 0u;
    uint32_t gs_cut_count = 0u;
    std::string hlsl_source;
    std::string hlsl_target;
    std::vector<uint8_t> pc_dxil;
    bool dxbc_hash_verified = false;
    std::vector<uint32_t> spirv_words;
    std::string dxc_diagnostics;
    std::string spirv_val_diagnostics;
};

Status LiftRdna2HullShader(
    const Xs2ShaderContainer& xs2,
    const Rdna2HullStageContract& contract,
    Rdna2ExtendedStageLiftResult& out_result,
    const Rdna2ExtendedLiftOptions& options = {});

Status LiftRdna2DomainShader(
    const Xs2ShaderContainer& xs2,
    const Rdna2DomainStageContract& contract,
    Rdna2ExtendedStageLiftResult& out_result,
    const Rdna2ExtendedLiftOptions& options = {});

Status LiftRdna2GeometryShader(
    const Xs2ShaderContainer& xs2,
    const Rdna2GeometryStageContract& contract,
    Rdna2ExtendedStageLiftResult& out_result,
    const Rdna2ExtendedLiftOptions& options = {});

// ===========================================================================
// 2. DXR Shader Binding Table (SBT) Mapper
// ===========================================================================

enum class DxrShaderRecordKind : uint8_t {
    RayGen = 0,
    Miss = 1,
    ClosestHit = 2,
    AnyHit = 3,
    Intersection = 4,
    Callable = 5,
};

struct XboxDxrShaderRecord {
    DxrShaderRecordKind kind = DxrShaderRecordKind::RayGen;
    std::string export_name;
    std::string hit_group_name;  // Non-empty for ClosestHit/AnyHit/Intersection
    std::array<uint8_t, kDxrShaderIdentifierSize> xbox_shader_id{};
    std::vector<uint8_t> local_root_args;
    uint32_t record_index = 0u;
};

struct DxrHitGroupBinding {
    std::string hit_group_name;
    uint32_t hit_group_index = 0u;
    std::string closest_hit_export;
    std::string any_hit_export;
    std::string intersection_export;
    std::array<uint8_t, kDxrShaderIdentifierSize> pc_shader_id{};
    uint64_t record_offset_bytes = 0u;
    uint64_t record_stride_bytes = 0u;
    uint64_t record_gpu_va = 0u;
};

struct DxrSbtPatchEntry {
    std::string symbol_name;
    DxrShaderRecordKind kind = DxrShaderRecordKind::RayGen;
    uint64_t buffer_offset_bytes = 0u;
    std::array<uint8_t, kDxrShaderIdentifierSize> xbox_id{};
    std::array<uint8_t, kDxrShaderIdentifierSize> pc_id{};
};

struct MappedDxrSbt {
    std::vector<uint8_t> buffer;
    uint64_t base_gpu_va = 0u;
    uint64_t raygen_offset = 0u;
    uint64_t miss_offset = 0u;
    uint64_t hit_group_offset = 0u;
    uint64_t callable_offset = 0u;
    uint64_t raygen_stride = 0u;
    uint64_t miss_stride = 0u;
    uint64_t hit_group_stride = 0u;
    uint64_t callable_stride = 0u;
    DxrDispatchRaysDesc dispatch_rays_desc{};
    std::vector<DxrHitGroupBinding> hit_groups;
    std::vector<DxrSbtPatchEntry> patch_entries;
};

// Maps Xbox RDNA2 raytracing shader records (RayGen, ClosestHit, AnyHit, Miss,
// Intersection, Callable) into D3D12_DISPATCH_RAYS_DESC-aligned PC SBT buffers
// (64-byte table alignment, 32-byte record stride alignment) with identifier
// patching and DXR spec hit-group lookup.
class DxrSbtMapper {
public:
    // Registers a PC 32-byte shader identifier (from ID3D12StateObjectProperties::GetShaderIdentifier).
    void RegisterPcShaderIdentifier(
        const std::string& symbol_name,
        const std::array<uint8_t, kDxrShaderIdentifierSize>& pc_shader_id);

    // Explicitly associates a hit-group name with ClosestHit / AnyHit / Intersection exports.
    void RegisterHitGroup(
        const std::string& hit_group_name,
        const std::string& closest_hit_export,
        const std::string& any_hit_export = {},
        const std::string& intersection_export = {});

    // Maps Xbox shader records into a 64B-table / 32B-record aligned PC SBT buffer.
    Status MapXboxSbt(
        const std::vector<XboxDxrShaderRecord>& xbox_records,
        uint64_t base_gpu_va,
        uint32_t dispatch_width,
        uint32_t dispatch_height,
        uint32_t dispatch_depth,
        MappedDxrSbt& out_sbt) const;

    // Patches a 32-byte PC shader identifier in-place inside a mapped SBT buffer.
    Status PatchShaderIdentifierInPlace(
        MappedDxrSbt& sbt,
        const std::string& symbol_name,
        const std::array<uint8_t, kDxrShaderIdentifierSize>& new_pc_shader_id) const;

    // Resolves hit-group binding using the DXR specification indexing formula:
    //   index = ray_contribution + geometry_stride * geometry_index + instance_contribution.
    Status LookupHitGroup(
        const MappedDxrSbt& sbt,
        uint32_t instance_contribution,
        uint32_t geometry_index,
        uint32_t ray_contribution,
        uint32_t geometry_stride,
        DxrHitGroupBinding& out_binding) const;

    Status LookupHitGroupByName(
        const MappedDxrSbt& sbt,
        const std::string& hit_group_name,
        DxrHitGroupBinding& out_binding) const;

    static std::array<uint8_t, kDxrShaderIdentifierSize> DeriveDeterministicPcId(
        const std::string& symbol_name) noexcept;

private:
    struct HitGroupSpec {
        std::string closest_hit;
        std::string any_hit;
        std::string intersection;
    };

    std::vector<std::pair<std::string, std::array<uint8_t, kDxrShaderIdentifierSize>>> pc_ids_;
    std::vector<std::pair<std::string, HitGroupSpec>> hit_group_specs_;
};

// ===========================================================================
// 3. Exact Cross-Half Wave64-on-Wave32 Emulation (Wave64On32Emulator)
// ===========================================================================

struct Wave64OpTestInput {
    std::array<uint32_t, 64> lane_values{};      // Input VGPR per guest lane 0..63
    std::array<uint32_t, 64> readlane_targets{}; // Per-lane target_lane (0..63) for cross-half v_readlane_b32
    uint32_t permlane_sel_lo = 0x76543210u;      // 8x4-bit selectors for row lanes 0..7
    uint32_t permlane_sel_hi = 0xFEDCBA98u;      // 8x4-bit selectors for row lanes 8..15
    uint32_t ballot_mask_lo = 0xA55A1234u;       // Active/predicate mask for lanes 0..31
    uint32_t ballot_mask_hi = 0x5AA5F00Du;       // Active/predicate mask for lanes 32..63
};

struct Wave64OpTestOutput {
    std::array<uint32_t, 64> permlane16{};       // Exact v_permlane16_b32 across lanes 0..63
    std::array<uint32_t, 64> permlanex16{};      // Exact v_permlanex16_b32 across lanes 0..63
    std::array<uint32_t, 64> readlane{};         // Exact cross-half v_readlane_b32 across lanes 0..63
    std::array<uint32_t, 64> wqm_lo{};           // Exact s_wqm_b64 low 32-bit mask (quads 0..7)
    std::array<uint32_t, 64> wqm_hi{};           // Exact s_wqm_b64 high 32-bit mask (quads 8..15)
    std::array<uint32_t, 64> mbcnt_lo{};         // Exact v_mbcnt_lo_u32_b32(ballot_lo, 0)
    std::array<uint32_t, 64> mbcnt_total{};      // Exact v_mbcnt_hi_u32_b32(ballot_hi, mbcnt_lo)
};

struct Wave64GpuRunInfo {
    std::string backend_name;
    std::string adapter_name;
    std::string shader_target;
    uint32_t hw_wave_lane_min = 0u;
    uint32_t hw_wave_lane_max = 0u;
    bool executed_on_hardware_gpu = false;
    bool used_warp = false;
};

class Wave64On32Emulator {
public:
    // Bit-exact CPU Wave64 reference over all 64 lanes.
    static Wave64OpTestOutput ExecuteCpuReference(const Wave64OpTestInput& input) noexcept;

    // Reusable HLSL library functions implementing exact cross-half Wave64-on-Wave32 lowering
    // using workgroup LDS half-wave exchange + explicit GroupMemoryBarrierWithGroupSync().
    static std::string EmitHlslHelperLibrary(uint32_t max_guest_waves_per_group = 4u);

    // Complete runnable HLSL compute shader (cs_6_5 / cs_6_2) exercising all 5 Wave64 ops.
    static std::string EmitTestComputeHlsl();

    // Complete valid SPIR-V 1.3 compute shader exercising all 5 Wave64 ops via workgroup LDS
    // exchange + OpControlBarrier (passes ValidateSpirvBinary + spirv-val.exe + VulkanBackend).
    static Status EmitTestComputeSpirv(std::vector<uint32_t>& out_spirv);

#if defined(_WIN32)
    // Executes the Wave64-on-Wave32 compute shader live on D3D12 HW GPU (prefer_warp=false,
    // strictly rejecting software/WARP adapters) or D3D12 WARP (prefer_warp=true).
    static Status ExecuteTestOnD3D12(
        const Wave64OpTestInput& input,
        Wave64OpTestOutput& out_output,
        bool prefer_warp = false,
        Wave64GpuRunInfo* out_info = nullptr,
        const std::string& dxc_path = {});
#endif

    // Executes the SPIR-V 1.3 Wave64-on-Wave32 compute shader on Vulkan HW ICD (if present).
    static Status ExecuteTestOnVulkan(
        const Wave64OpTestInput& input,
        Wave64OpTestOutput& out_output,
        bool allow_software_fallback = false,
        Wave64GpuRunInfo* out_info = nullptr);
};

// ===========================================================================
// 4. Async Background PSO Pre-Compilation Worker Pool (AsyncPsoCompilePool)
// ===========================================================================

enum class PsoCompilePriority : uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3,
};

struct AsyncPsoJobDesc {
    std::string debug_name;
    std::vector<uint8_t> shader_blob;  // Raw XS2\0, Xbox-wrapped DXBC, or UTF-8 HLSL if is_hlsl_source=true
    bool is_hlsl_source = false;
    std::string hlsl_entry = "main";
    std::string target_profile;        // e.g. "cs_6_5", "cs_6_2"; empty = auto from stage
    PsoCompilePriority priority = PsoCompilePriority::Normal;
    bool emit_spirv = true;
    bool compile_dxil = true;
    bool create_d3d12_pso = false;     // If pool has ID3D12Device*, also creates live Compute PSO
    std::string dxc_path;
};

struct AsyncPsoJobResult {
    Status status = Status::Ok();
    uint64_t content_hash = 0u;
    bool cache_hit = false;
    bool cancelled = false;
    std::string hlsl_source;
    std::string target_profile;
    std::vector<uint8_t> dxil;
    std::vector<uint32_t> spirv;
    bool d3d12_pso_created = false;
    uint64_t compile_time_us = 0u;
};

struct AsyncPsoPoolStats {
    uint64_t jobs_submitted = 0u;
    uint64_t jobs_queued = 0u;
    uint64_t jobs_compiled = 0u;
    uint64_t jobs_cache_hits = 0u;
    uint64_t jobs_dedup_coalesced = 0u;
    uint64_t jobs_failed = 0u;
    uint64_t jobs_cancelled = 0u;
    uint32_t worker_count = 0u;
    uint32_t pending_queue_depth = 0u;
};

class AsyncPsoCompilePool {
public:
#if defined(_WIN32)
    explicit AsyncPsoCompilePool(uint32_t worker_count = 0u, ID3D12Device* d3d12_device = nullptr);
#else
    explicit AsyncPsoCompilePool(uint32_t worker_count = 0u);
#endif
    ~AsyncPsoCompilePool();

    AsyncPsoCompilePool(const AsyncPsoCompilePool&) = delete;
    AsyncPsoCompilePool& operator=(const AsyncPsoCompilePool&) = delete;

    // Deterministic content hash used for cache and in-flight deduplication.
    static uint64_t ComputeJobContentHash(const AsyncPsoJobDesc& desc) noexcept;

    // Submits a shader/PSO compile job asynchronously with priority + content-hash deduplication.
    std::future<AsyncPsoJobResult> Submit(
        const AsyncPsoJobDesc& desc,
        std::function<void(const AsyncPsoJobResult&)> callback = nullptr);

    // Non-blocking lookup in the completed compilation cache.
    bool TryGetCached(uint64_t content_hash, AsyncPsoJobResult& out_result) const;

    // Cancels all queued jobs that have not yet started executing on a worker thread.
    void CancelPending();

    // Blocks until all queued and active jobs finish.
    void WaitForIdle();

    // Gracefully shuts down all worker threads (idempotent).
    void Shutdown(bool cancel_pending = false);

    AsyncPsoPoolStats Stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openscarlett
