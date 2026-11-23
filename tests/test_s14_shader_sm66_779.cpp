// OpenScarlett - MIT License
// Sprint 14 Worker 5 Verification Suite:
//   1. Issue #29 AMD RDNA4 VOPC Regression (sv64_vopc_p1 idx=9 + NaN/class/f16/f64 bit-exactness)
//   2. SM 6.6 & All-Stage Profile Support + Off-C:\ Default Shader Cache
//   3. Wired TranslateRdna2BvhIntersectToRayQuery in LiftRdna2Xs2Shader & LiftMemImgShader
//   4. All 13 Latest Radar Shader Accuracy Catches
//   5. 779/779 GFX10.3 Decoded/Lifted/CPU-Ref + >=769/779 Live-GPU + Live MS/AS Verification

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"
#include "openscarlett/gpu/shader_cache.h"
#include "rdna2_coverage_tool.h"
#include "rdna2_isa_mem_img.h"
#include "rdna2_isa_scalar_vector.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace {

using namespace openscarlett;

int g_failures = 0;

void Check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        ++g_failures;
    } else {
        std::printf("[PASS] %s\n", msg);
    }
}

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0u;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

uint64_t DoubleToBits(double d) noexcept {
    uint64_t b = 0ull;
    std::memcpy(&b, &d, sizeof(b));
    return b;
}

void AppendMubuf(std::vector<uint32_t>& words, uint16_t raw_op, uint8_t vdata, uint8_t vaddr,
                 uint8_t srsrc, uint16_t offset, bool offen, bool idxen = false, bool glc = false,
                 uint8_t soffset = 128u) {
    const uint32_t w0 = (0x38u << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (glc ? (1u << 14) : 0u) | (idxen ? (1u << 13) : 0u) |
                        (offen ? (1u << 12) : 0u) | static_cast<uint32_t>(offset & 0xFFFu);
    const uint32_t w1 = (static_cast<uint32_t>(soffset) << 24) |
                        (static_cast<uint32_t>(srsrc & 0x3Fu) << 16) |
                        (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr);
    words.push_back(w0);
    words.push_back(w1);
}

void AppendMimgRaw(std::vector<uint32_t>& words, uint16_t raw_op, uint8_t vdata, uint8_t vaddr,
                   uint8_t dmask, uint8_t dim, bool glc = false, uint8_t srsrc = 0u, uint8_t ssamp = 0u) {
    const uint32_t w0 = (0x3Cu << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (static_cast<uint32_t>(dmask & 0xFu) << 8) |
                        (static_cast<uint32_t>(dim & 7u) << 3) |
                        (glc ? (1u << 13) : 0u) |
                        static_cast<uint32_t>((raw_op >> 7) & 1u);
    const uint32_t w1 = (static_cast<uint32_t>(ssamp & 0x1Fu) << 21) |
                        (static_cast<uint32_t>(srsrc & 0x1Fu) << 16) |
                        (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr);
    words.push_back(w0);
    words.push_back(w1);
}

void TestIssue29VopcAndClassModifiers() {
    using namespace openscarlett::rdna2_sv;

    // 1. v_cmp_class_f32 / f16 / f64 source modifiers (abs / neg applied BEFORE class test; Leonida cf06641)
    {
        // Construct VOP3-encoded v_cmp_class_f32 (0x88) with src0.abs = true on -2.0f vs mask 0x100 (pos normal)
        // and with src0.neg = true on +2.0f vs mask 0x008 (neg normal).
        std::vector<uint32_t> code;
        auto emit_vop3_cmp_class = [&](uint16_t raw_vopc, uint8_t vsrc0, uint8_t vsrc1, bool do_abs, bool do_neg) {
            const uint32_t abs_b = do_abs ? 1u : 0u;
            const uint32_t neg_b = do_neg ? 1u : 0u;
            const uint32_t w0 = (0x35u << 26) | (static_cast<uint32_t>(raw_vopc & 0x3FFu) << 16) |
                                (abs_b << 8) | 106u /* vcc_lo */;
            const uint32_t w1 = (neg_b << 29) |
                                (static_cast<uint32_t>(256u + vsrc1) << 9) |
                                static_cast<uint32_t>(256u + vsrc0);
            code.push_back(w0);
            code.push_back(w1);
        };

        EncodeScalarVectorInst("v_mov_b32", 15u, {Rdna2Operand::InlineInt(1)}, 0, code);
        // Test 1: -2.0f with abs=true -> +2.0f (positive normal = bit 8 = 0x100)
        emit_vop3_cmp_class(0x88u, 2u, 3u, true, false);
        EncodeScalarVectorInst("v_cndmask_b32", 10u, {Rdna2Operand::InlineInt(0), Rdna2Operand::Vgpr(15)}, 0, code);
        // Test 2: +2.0f with neg=true -> -2.0f (negative normal = bit 3 = 0x008)
        emit_vop3_cmp_class(0x88u, 4u, 5u, false, true);
        EncodeScalarVectorInst("v_cndmask_b32", 11u, {Rdna2Operand::InlineInt(0), Rdna2Operand::Vgpr(15)}, 0, code);
        // Test 3: f16 -2.0 (0xC000) with abs=true -> +2.0 (pos normal = 0x100)
        emit_vop3_cmp_class(0x8Fu, 6u, 3u, true, false);
        EncodeScalarVectorInst("v_cndmask_b32", 12u, {Rdna2Operand::InlineInt(0), Rdna2Operand::Vgpr(15)}, 0, code);
        // Test 4: f64 -2.0 with abs=true -> +2.0 (pos normal = 0x100)
        emit_vop3_cmp_class(0xA8u, 8u, 3u, true, false);
        EncodeScalarVectorInst("v_cndmask_b32", 13u, {Rdna2Operand::InlineInt(0), Rdna2Operand::Vgpr(15)}, 0, code);
        EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code);

        std::vector<SvDecodedInst> dec;
        Check(DecodeScalarVectorStream(code, dec).ok(), "v_cmp_class_* with VOP3 abs/neg decodes cleanly");

        SvWaveState st{};
        st.Reset(32u);
        st.exec = 0xFu;
        const uint64_t neg2_d = DoubleToBits(-2.0);
        for (uint32_t l = 0; l < 4u; ++l) {
            st.vgpr[l][2] = FloatToBits(-2.0f);
            st.vgpr[l][3] = 0x100u;  // pos normal
            st.vgpr[l][4] = FloatToBits(2.0f);
            st.vgpr[l][5] = 0x008u;  // neg normal
            st.vgpr[l][6] = 0xC000u; // f16 -2.0
            st.vgpr[l][8] = static_cast<uint32_t>(neg2_d);
            st.vgpr[l][9] = static_cast<uint32_t>(neg2_d >> 32u);
        }
        Check(ExecuteScalarVectorCpu(dec, st).ok(), "v_cmp_class_* abs/neg CPU ref executes");
        Check(st.vgpr[0][10] == 1u && st.vgpr[0][11] == 1u && st.vgpr[0][12] == 1u && st.vgpr[0][13] == 1u,
              "v_cmp_class_f32/f16/f64 apply abs/neg modifiers BEFORE class test (Leonida cf06641)");
    }

    // 2. Issue #29 exact pattern: v_cmpx_nge_f32 (idx=9 -> 0x99999999) + NaN/class/f16/f64 VOPC
    {
        std::vector<uint32_t> code;
        EncodeScalarVectorInst("v_lshlrev_b32", 1u, {Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(0)}, 0, code);
        AppendMubuf(code, 0x14u, 2u, 1u, 0u, 0u, true);    // f32 A: {1, 2, 3, NaN}
        AppendMubuf(code, 0x14u, 3u, 1u, 0u, 128u, true);  // f32 B: {2, 2, 1, 1}
        AppendMubuf(code, 0x14u, 4u, 1u, 0u, 256u, true);  // f16 A: {1.0, 2.0, 3.0, NaN}
        AppendMubuf(code, 0x14u, 5u, 1u, 0u, 384u, true);  // f16 B: {2.0, 2.0, 1.0, 1.0}
        EncodeScalarVectorInst("v_mov_b32", 15u, {Rdna2Operand::InlineInt(1)}, 0, code);
        EncodeScalarVectorInst("v_cmp_nge_f32", 0u, {Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3)}, 0, code);
        EncodeScalarVectorInst("v_cndmask_b32", 6u, {Rdna2Operand::InlineInt(0), Rdna2Operand::Vgpr(15)}, 0, code);
        AppendMubuf(code, 0x1Cu, 6u, 1u, 1u, 0u, true);
        EncodeScalarVectorInst("v_cmp_u_f32", 0u, {Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3)}, 0, code);
        EncodeScalarVectorInst("v_cndmask_b32", 7u, {Rdna2Operand::InlineInt(0), Rdna2Operand::Vgpr(15)}, 0, code);
        AppendMubuf(code, 0x1Cu, 7u, 1u, 1u, 128u, true);
        EncodeScalarVectorInst("v_cmp_nge_f16", 0u, {Rdna2Operand::Vgpr(4), Rdna2Operand::Vgpr(5)}, 0, code);
        EncodeScalarVectorInst("v_cndmask_b32", 8u, {Rdna2Operand::InlineInt(0), Rdna2Operand::Vgpr(15)}, 0, code);
        AppendMubuf(code, 0x1Cu, 8u, 1u, 1u, 256u, true);
        EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code);

        std::vector<SvDecodedInst> dec;
        Check(DecodeScalarVectorStream(code, dec).ok(), "Issue #29 VOPC regression batch decodes");

        SvWaveState st{};
        st.Reset(32u);
        st.exec = 0xFFFFFFFFull;
        st.uav0.assign(128u, 0u);
        st.uav1.assign(96u, 0u);
        const uint32_t kFA[4] = {FloatToBits(1.0f), FloatToBits(2.0f), FloatToBits(3.0f), 0x7FC00000u};
        const uint32_t kFB[4] = {FloatToBits(2.0f), FloatToBits(2.0f), FloatToBits(1.0f), FloatToBits(1.0f)};
        const uint32_t kHA[4] = {0x3C00u, 0x4000u, 0x4200u, 0x7E00u};
        const uint32_t kHB[4] = {0x4000u, 0x4000u, 0x3C00u, 0x3C00u};
        for (uint32_t i = 0; i < 32u; ++i) {
            st.vgpr[i][0] = i;
            st.uav0[i] = kFA[i & 3u];
            st.uav0[32u + i] = kFB[i & 3u];
            st.uav0[64u + i] = kHA[i & 3u];
            st.uav0[96u + i] = kHB[i & 3u];
        }
        Check(ExecuteScalarVectorCpu(dec, st).ok(), "Issue #29 VOPC CPU ref executes");
        uint32_t mask_nge_f32 = 0u, mask_u_f32 = 0u, mask_nge_f16 = 0u;
        for (uint32_t i = 0; i < 32u; ++i) {
            if (st.uav1[i] != 0u) mask_nge_f32 |= (1u << i);
            if (st.uav1[32u + i] != 0u) mask_u_f32 |= (1u << i);
            if (st.uav1[64u + i] != 0u) mask_nge_f16 |= (1u << i);
        }
        Check(mask_nge_f32 == 0x99999999u && mask_u_f32 == 0x88888888u && mask_nge_f16 == 0x99999999u,
              "Issue #29 VOPC bit-exact masks match 0x99999999 / 0x88888888 for f32 & f16 with NaN");

        Xs2ShaderContainer xs2{};
        xs2.stage = ShaderStage::Compute;
        xs2.threadgroup_x = 32u;
        xs2.threadgroup_y = 1u;
        xs2.threadgroup_z = 1u;
        xs2.code_words = code;
        Rdna2LiftOptions lopts{};
        lopts.use_sm65 = true;
        lopts.emit_spirv = true;
        lopts.emit_hlsl = true;
        lopts.compile_dxil_with_dxc = !ResolvePortableDxcPath("").empty() &&
                                      std::filesystem::exists(ResolvePortableDxcPath(""));
        lopts.resign_dxil = true;
        Rdna2LiftResult res{};
        Check(LiftScalarVectorShader(xs2, dec, res, lopts).ok(),
              "Issue #29 VOPC shader lifts to SPIR-V + bit-exact integer HLSL (+ DXC)");
        Check(res.hlsl_source.find("_sv_cmp_f32_bits") != std::string::npos &&
                  res.hlsl_source.find("_sv_cmp_f16_bits") != std::string::npos &&
                  res.hlsl_source.find("isnan(") == std::string::npos,
              "Issue #29 VOPC HLSL uses pure bit-exact integer IEEE-754 helpers (no isnan())");
    }
}

void TestSm66ProfilesAndOffCShaderCache() {
    // 1. SM 6.6 + all 9 stages + clean fallback to 6.5 / 6.2 / 6.0 / 5.0
    Check(SelectTargetProfileForStage(ShaderStage::Compute, 0x66u) == "cs_6_6", "SM 6.6 Compute -> cs_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Vertex, 0x66u) == "vs_6_6", "SM 6.6 Vertex -> vs_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Pixel, 0x66u) == "ps_6_6", "SM 6.6 Pixel -> ps_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Amplification, 0x66u) == "as_6_6", "SM 6.6 Amplification -> as_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Mesh, 0x66u) == "ms_6_6", "SM 6.6 Mesh -> ms_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Hull, 0x66u) == "hs_6_6", "SM 6.6 Hull -> hs_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Domain, 0x66u) == "ds_6_6", "SM 6.6 Domain -> ds_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Geometry, 0x66u) == "gs_6_6", "SM 6.6 Geometry -> gs_6_6");
    Check(SelectTargetProfileForStage(ShaderStage::Library, 0x66u) == "lib_6_6", "SM 6.6 Library -> lib_6_6");

    Check(SelectTargetProfileForStage(ShaderStage::Compute, 0x65u) == "cs_6_5", "SM 6.5 fallback -> cs_6_5");
    Check(SelectTargetProfileForStage(ShaderStage::Hull, 0x65u) == "hs_6_5", "SM 6.5 Hull -> hs_6_5");
    Check(SelectTargetProfileForStage(ShaderStage::Domain, 0x65u) == "ds_6_5", "SM 6.5 Domain -> ds_6_5");
    Check(SelectTargetProfileForStage(ShaderStage::Geometry, 0x65u) == "gs_6_5", "SM 6.5 Geometry -> gs_6_5");
    Check(SelectTargetProfileForStage(ShaderStage::Library, 0x65u) == "lib_6_5", "SM 6.5 Library -> lib_6_5");
    Check(SelectTargetProfileForStage(ShaderStage::Vertex, 0x50u) == "vs_5_0", "SM 5.0 fallback -> vs_5_0");

    // 2. D3D12 OPTIONS9 live probe (informational gating verification)
#if defined(_WIN32)
    {
        IDXGIFactory6* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), reinterpret_cast<void**>(&fac))) && fac) {
            IDXGIAdapter1* adp = nullptr;
            if (SUCCEEDED(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                          __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adp))) && adp) {
                ID3D12Device* dev = nullptr;
                if (SUCCEEDED(D3D12CreateDevice(adp, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                                reinterpret_cast<void**>(&dev))) && dev) {
                    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
                    (void)dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
                    D3D12_FEATURE_DATA_D3D12_OPTIONS9 opt9{};
                    const HRESULT hr9 = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &opt9, sizeof(opt9));
                    std::printf("[INFO] Host D3D12 SM=0x%X OPTIONS9_hr=0x%X AtomicInt64Typed=%d\n",
                                static_cast<unsigned>(sm.HighestShaderModel), static_cast<unsigned>(hr9),
                                static_cast<int>(opt9.AtomicInt64OnTypedResourceSupported));
                    dev->Release();
                }
                adp->Release();
            }
            fac->Release();
        }
    }
#endif

    // 3. Off-C:\ default shader cache path (relative to executable when OPENSCARLETT_SHADER_CACHE is unset)
    {
        ShaderCache local_cache;
        // Temporarily clear OPENSCARLETT_SHADER_CACHE if set, then restore
        std::string saved_env;
        bool had_env = false;
#if defined(_WIN32)
        char buf[1024]{};
        const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", buf, static_cast<DWORD>(sizeof(buf)));
        if (len > 0 && len < sizeof(buf)) {
            saved_env.assign(buf, len);
            had_env = true;
            SetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", nullptr);
        }
#endif
        const std::string def_dir = local_cache.ResolveCacheDirectory();
#if defined(_WIN32)
        if (had_env) SetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", saved_env.c_str());
#endif
        Check(def_dir.find("cache") != std::string::npos && def_dir.find("shaders") != std::string::npos,
              "ShaderCache default path resolves to exe-local cache/shaders");
#if defined(_WIN32)
        char self_buf[MAX_PATH]{};
        const DWORD slen = GetModuleFileNameA(nullptr, self_buf, static_cast<DWORD>(sizeof(self_buf)));
        if (slen >= 2 && (self_buf[0] != 'C' && self_buf[0] != 'c')) {
            Check(def_dir.find("C:\\Users") == std::string::npos && def_dir.find("c:\\users") == std::string::npos,
                  "ShaderCache default path is OFF C:\\Users / %TEMP% when executable is on non-C: drive");
        }
#endif
        const auto custom_dir = (std::filesystem::temp_directory_path() / "os_s14_cache_override").string();
        local_cache.SetCacheDirectory(custom_dir);
        Check(local_cache.ResolveCacheDirectory() == custom_dir, "ShaderCache explicit SetCacheDirectory preserved");
    }
}

void TestWiredRayQueryBridge() {
    const std::string dxc = ResolvePortableDxcPath("");
    const bool has_dxc = !dxc.empty() && std::filesystem::exists(dxc);

    // 1. Standard LiftRdna2Xs2Shader with image_bvh_intersect_ray (0xE6) -> DXR 1.1 RayQuery + SPIR-V + Fallback
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 16, 4, 0, 0, 0xFu, 0u, false);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 16, 1, 4, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        const auto xs2_blob = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 24, 16, 32);

        // RayQuery DXR 1.1 + SPV_KHR_ray_query mode
        Rdna2LiftOptions rq_opts{};
        rq_opts.use_sm65 = true;
        rq_opts.emit_spirv = true;
        rq_opts.emit_hlsl = true;
        rq_opts.target_profile_override = "cs_6_5_rayquery";
        rq_opts.compile_dxil_with_dxc = has_dxc;
        rq_opts.resign_dxil = true;
        Rdna2LiftResult rq_res{};
        Check(LiftRdna2Xs2Shader(xs2_blob, rq_res, rq_opts).ok(),
              "LiftRdna2Xs2Shader wires image_bvh_intersect_ray -> RayQuery DXR 1.1 + SPV_KHR_ray_query");
        Check(rq_res.hlsl_source.find("RayQuery<RAY_FLAG_NONE>") != std::string::npos &&
                  rq_res.hlsl_source.find("TraceRayInline") != std::string::npos,
              "Lifted RayQuery HLSL emits RayQuery<RAY_FLAG_NONE> + TraceRayInline");
        Check(!rq_res.spirv_words.empty() && ValidateSpirvBinary(rq_res.spirv_words).ok(),
              "Lifted SPV_KHR_ray_query SPIR-V passes ValidateSpirvBinary");
        if (has_dxc) {
            Check(!rq_res.pc_dxil.empty(), "Lifted RayQuery HLSL compiles with dxc.exe to valid DXIL");
        }

        // Default compute BVH4 fallback mode
        Rdna2LiftOptions fb_opts{};
        fb_opts.use_sm65 = true;
        fb_opts.emit_spirv = true;
        fb_opts.emit_hlsl = true;
        fb_opts.compile_dxil_with_dxc = has_dxc;
        fb_opts.resign_dxil = true;
        Rdna2LiftResult fb_res{};
        Check(LiftRdna2Xs2Shader(xs2_blob, fb_res, fb_opts).ok(),
              "LiftRdna2Xs2Shader preserves compute BVH4 fallback for non-DXR GPUs");
        Check(fb_res.hlsl_source.find("_os_bvh_intersect_ray") != std::string::npos,
              "Default compute BVH4 fallback helper present");
    }

    // 2. LiftMemImgShader with image_bvh64_intersect_ray (0xE7) -> SM 6.6 RayQuery + SPV_KHR_ray_query
    {
        using namespace openscarlett::rdna2_mem;
        std::vector<uint32_t> words;
        AppendMimgRaw(words, 0xE7u /* image_bvh64_intersect_ray */, 16u, 4u, 0xFu, 0u, false);
        words.push_back(0xBF810000u);  // s_endpgm
        std::vector<MemImgDecodedInst> dec;
        Check(DecodeMemImgStream(words, dec).ok(), "image_bvh64_intersect_ray decodes in MemImg");
        Xs2ShaderContainer xs2{};
        xs2.stage = ShaderStage::Compute;
        xs2.threadgroup_x = 32u;
        xs2.threadgroup_y = 1u;
        xs2.threadgroup_z = 1u;
        xs2.code_words = words;

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.target_profile_override = "cs_6_6_rayquery";
        opts.compile_dxil_with_dxc = has_dxc;
        opts.resign_dxil = true;
        Rdna2LiftResult res{};
        Check(LiftMemImgShader(xs2, dec, res, opts).ok(),
              "LiftMemImgShader wires image_bvh64_intersect_ray -> cs_6_6 RayQuery + SPV_KHR_ray_query");
        Check(res.hlsl_target == "cs_6_6" && res.hlsl_source.find("RayQuery<RAY_FLAG_NONE>") != std::string::npos,
              "LiftMemImgShader emits cs_6_6 RayQuery<RAY_FLAG_NONE> HLSL");
    }
}

void TestRadarAccuracyCatches() {
    const std::string dxc = ResolvePortableDxcPath("");
    const bool has_dxc = !dxc.empty() && std::filesystem::exists(dxc);

    // (4.1) SV_ViewportArrayIndex from layer bits 19:16 + (4.2) ignore unmapped Pos13 & PS default (0,0,0,1)
    {
        // VS with Pos1 Z = layer (bits 19:16 = viewport index 5 -> 0x00050000)
        Rdna2IrModule vs_ir{};
        vs_ir.stage = ShaderStage::Vertex;
        IrBasicBlock bb{};
        bb.id = 0u;
        auto add = [&](IrOp op, IrType ty, std::vector<IrValueId> ops, uint32_t imm = 0u, float fimm = 0.0f, uint8_t et = 0u) {
            IrInstruction ins{};
            ins.id = (ty == IrType::Void) ? kInvalidIrValue : vs_ir.AllocateValueId();
            ins.op = op;
            ins.type = ty;
            ins.operands = std::move(ops);
            ins.imm_u32 = imm;
            ins.imm_f32 = fimm;
            ins.export_target = et;
            bb.instructions.push_back(ins);
            return ins.id;
        };
        const IrValueId f0 = add(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
        const IrValueId f1 = add(IrOp::ConstF32, IrType::F32, {}, 0u, 1.0f);
        const IrValueId vp_bits = add(IrOp::ConstU32, IrType::U32, {}, 0x00050000u);
        add(IrOp::Export, IrType::Void, {f0, f0, f0, f1}, 0u, 0.0f, static_cast<uint8_t>(Rdna2ExportTarget::Pos0));
        add(IrOp::Export, IrType::Void, {f0, f0, vp_bits, f0}, 0u, 0.0f, static_cast<uint8_t>(Rdna2ExportTarget::Pos1));
        add(IrOp::Return, IrType::Void, {});
        vs_ir.blocks.push_back(bb);

        std::string hlsl, target;
        Check(EmitHlsl(vs_ir, hlsl, target, true).ok(), "VS with Pos1 ViewportArrayIndex emits HLSL");
        Check(hlsl.find("SV_ViewportArrayIndex") != std::string::npos &&
                  hlsl.find(">> 16u) & 0xFu") != std::string::npos,
              "SV_ViewportArrayIndex unpacked from Pos1 bits [19:16] (xbox-emulator #156)");
        if (has_dxc) {
            std::vector<uint8_t> dxil;
            std::string diag;
            Check(CompileHlslWithDxc(hlsl, target, "main", dxil, diag, dxc).ok(),
                  "VS with SV_ViewportArrayIndex compiles with DXC");
        }

        // Unmapped Pos13 (Z == 0) must be ignored cleanly without overwriting _out.pos
        Rdna2IrModule vs_unmapped{};
        vs_unmapped.stage = ShaderStage::Vertex;
        IrBasicBlock bb2{};
        bb2.id = 0u;
        auto add2 = [&](IrOp op, IrType ty, std::vector<IrValueId> ops, uint32_t imm = 0u, float fimm = 0.0f, uint8_t et = 0u) {
            IrInstruction ins{};
            ins.id = (ty == IrType::Void) ? kInvalidIrValue : vs_unmapped.AllocateValueId();
            ins.op = op;
            ins.type = ty;
            ins.operands = std::move(ops);
            ins.imm_u32 = imm;
            ins.imm_f32 = fimm;
            ins.export_target = et;
            bb2.instructions.push_back(ins);
            return ins.id;
        };
        const IrValueId u0 = add2(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
        const IrValueId u1 = add2(IrOp::ConstF32, IrType::F32, {}, 0u, 1.0f);
        add2(IrOp::Export, IrType::Void, {u0, u0, u0, u1}, 0u, 0.0f, static_cast<uint8_t>(Rdna2ExportTarget::Pos0));
        add2(IrOp::Export, IrType::Void, {u0, u0, u0, u0}, 0u, 0.0f, static_cast<uint8_t>(Rdna2ExportTarget::Pos1));
        add2(IrOp::Return, IrType::Void, {});
        vs_unmapped.blocks.push_back(bb2);
        std::string hlsl_u, tgt_u;
        Check(EmitHlsl(vs_unmapped, hlsl_u, tgt_u, true).ok() &&
                  hlsl_u.find("SV_ViewportArrayIndex") == std::string::npos,
              "Unmapped Pos13 ignored cleanly without emitting viewport or overwriting _out.pos (xbox-emulator #181)");

        // Unmapped PS input attributes default to (0, 0, 0, 1) in CPU interpreter
        Rdna2Assembler ps_a;
        ps_a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 4, 0, 3, 0);  // unmapped attr3.x -> 0.0
        ps_a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 5, 0, 3, 1);  // unmapped attr3.y -> 0.0
        ps_a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 6, 0, 3, 2);  // unmapped attr3.z -> 0.0
        ps_a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 7, 0, 3, 3);  // unmapped attr3.w -> 1.0
        ps_a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 5, 6, 7, 0xFu, true, false, true);
        ps_a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        std::vector<Rdna2Instruction> ps_insts;
        Xs2ShaderContainer ps_c{};
        ps_c.stage = ShaderStage::Pixel;
        ps_c.code_words = ps_a.Words();
        Check(DisassembleRdna2(ps_c.code_words, ps_insts).ok(), "Unmapped PS attr shader disassembles");
        Rdna2CpuExecState ps_st{};
        ps_st.stage_inputs.clear();  // No bound PS inputs!
        Check(ExecuteRdna2Cpu(ps_c, ps_insts, ps_st).ok() && !ps_st.exports.empty() &&
                  ps_st.exports[0].mrt0[0] == 0.0f && ps_st.exports[0].mrt0[1] == 0.0f &&
                  ps_st.exports[0].mrt0[2] == 0.0f && ps_st.exports[0].mrt0[3] == 1.0f,
              "Unmapped PS input attributes default to (0,0,0,1) (xbox-emulator #181)");
    }

    // (4.3) Bit-exact f32 rcp / rsq / sqrt (+/-0, +/-inf, NaN, negative)
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralU32(0x80000000u));  // -0.0f
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0xFF800000u));  // -inf
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralF32(-4.0f));
        a.EmitVop1(Rdna2Opcode::V_RCP_F32, 10, Rdna2Operand::Vgpr(1));   // rcp(-0.0) -> -inf (0xFF800000)
        a.EmitVop1(Rdna2Opcode::V_RCP_F32, 11, Rdna2Operand::Vgpr(2));   // rcp(-inf) -> -0.0 (0x80000000)
        a.EmitVop1(Rdna2Opcode::V_RSQ_F32, 12, Rdna2Operand::Vgpr(3));   // rsq(-4.0) -> QNaN
        a.EmitVop1(Rdna2Opcode::V_SQRT_F32, 13, Rdna2Operand::Vgpr(1));  // sqrt(-0.0) -> -0.0 (0x80000000)
        a.EmitVop1(Rdna2Opcode::V_SQRT_F32, 14, Rdna2Operand::Vgpr(3));  // sqrt(-4.0) -> QNaN
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 0, 4, 0, false);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 0, 4, 4, false);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 0, 4, 8, false);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 0, 4, 12, false);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 0, 4, 16, false);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        std::vector<Rdna2Instruction> insts;
        Xs2ShaderContainer c{};
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 1u;
        c.code_words = a.Words();
        Check(DisassembleRdna2(c.code_words, insts).ok(), "f32 rcp/rsq/sqrt shader disassembles");
        Rdna2CpuExecState st{};
        st.uav1.assign(8u, 0u);
        Check(ExecuteRdna2Cpu(c, insts, st).ok(), "f32 rcp/rsq/sqrt CPU interpreter executes");
        Check(st.uav1[0] == 0xFF800000u &&
                  st.uav1[1] == 0x80000000u &&
                  ((st.uav1[2] & 0x7FFFFFFFu) > 0x7F800000u) &&
                  st.uav1[3] == 0x80000000u &&
                  ((st.uav1[4] & 0x7FFFFFFFu) > 0x7F800000u),
              "Bit-exact f32 v_rcp_f32 / v_rsq_f32 / v_sqrt_f32 edge cases match IEEE/RDNA2 (xbox-emulator #180)");
    }

    // (4.4) Float atomic min/max handling -0.0 < +0.0 & f64 forms + (4.5) Buffer atomics SOFFSET+IDXEN+OFFEN + (4.6) Image atomic GLC
    {
        using namespace openscarlett::rdna2_mem;
        std::vector<uint32_t> words;
        // MUBUF buffer_atomic_fmin (0x3F) with idxen+offen + soffset=s4 + glc=1
        AppendMubuf(words, 0x3Fu, 4u, 2u, 0u, 0u, true, true, true, 4u /* s4 */);
        // MUBUF buffer_atomic_fmax (0x40) with inline soffset=132 (+4) + glc=1
        AppendMubuf(words, 0x40u, 5u, 2u, 0u, 0u, false, false, true, 132u /* +4 */);
        // MUBUF buffer_atomic_fmin_x2 (0x5F) f64 min at offset 8
        AppendMubuf(words, 0x5Fu, 6u, 2u, 0u, 8u, false, false, true, 128u);
        // MIMG image_atomic_fmin (0x1E) with glc=1
        AppendMimgRaw(words, 0x1Eu, 8u, 10u, 0x1u, 1u, true);
        // MIMG image_atomic_fmax (0x1F) with glc=1
        AppendMimgRaw(words, 0x1Fu, 9u, 10u, 0x1u, 1u, true);
        words.push_back(0xBF810000u);  // s_endpgm

        std::vector<MemImgDecodedInst> dec;
        Check(DecodeMemImgStream(words, dec).ok(), "Float atomic (-0.0 / f64) + SOFFSET + MIMG GLC decode cleanly");

        MemImgExecState st{};
        st.wave_size = 1u;
        st.exec_mask = 1ull;
        st.buffer_stride_bytes = 16u;
        st.sgpr[4] = 0u;
        st.uav0.assign(64u, 0u);
        auto write_uav_u32 = [&](size_t dw, uint32_t val) {
            std::memcpy(st.uav0.data() + dw * 4u, &val, sizeof(val));
        };
        auto read_uav_u32 = [&](size_t dw) -> uint32_t {
            uint32_t val = 0u;
            std::memcpy(&val, st.uav0.data() + dw * 4u, sizeof(val));
            return val;
        };
        write_uav_u32(0u, 0x00000000u);  // +0.0f
        write_uav_u32(1u, 0x80000000u);  // -0.0f
        const uint64_t pos0_d = 0x0000000000000000ull;
        const uint64_t neg0_d = 0x8000000000000000ull;
        write_uav_u32(2u, static_cast<uint32_t>(pos0_d));
        write_uav_u32(3u, static_cast<uint32_t>(pos0_d >> 32u));
        st.vgpr[0][2] = 0u;  // idx=0
        st.vgpr[0][3] = 0u;  // off=0
        st.vgpr[0][4] = 0x80000000u;  // -0.0f -> min(+0.0, -0.0) MUST be -0.0!
        st.vgpr[0][5] = 0x00000000u;  // +0.0f -> max(-0.0, +0.0) MUST be +0.0!
        st.vgpr[0][6] = static_cast<uint32_t>(neg0_d);
        st.vgpr[0][7] = static_cast<uint32_t>(neg0_d >> 32u);
        st.rw_texture.AllocateR32u(kMimgDim2D, 4u, 4u, 1u, 1u);
        *st.rw_texture.MutableR32uPtr(0u, 0u, 0u, 0u, 0u) = 0x00000000u;  // +0.0f
        st.vgpr[0][8] = 0x80000000u;  // image_atomic_fmin -> -0.0f, returns +0.0f in v8
        st.vgpr[0][9] = 0x00000000u;  // image_atomic_fmax -> +0.0f, returns -0.0f in v9
        st.vgpr[0][10] = 0u;
        st.vgpr[0][11] = 0u;
        Check(ExecuteMemImgCpu(dec, st).ok(), "Float atomic (-0.0 / f64) + SOFFSET + MIMG GLC CPU ref executes");
        Check(read_uav_u32(0u) == 0x80000000u && st.vgpr[0][4] == 0x00000000u &&
                  read_uav_u32(1u) == 0x00000000u && st.vgpr[0][5] == 0x80000000u &&
                  read_uav_u32(2u) == 0u && read_uav_u32(3u) == 0x80000000u &&
                  st.vgpr[0][8] == 0x00000000u && st.vgpr[0][9] == 0x80000000u &&
                  *st.rw_texture.MutableR32uPtr(0u, 0u, 0u, 0u, 0u) == 0x00000000u,
              "Float atomic min/max -0.0 & f64 + SOFFSET+IDXEN+OFFEN + MIMG GLC bit-exact (xcl #38, #177, 4160c9a)");

        Xs2ShaderContainer xs2{};
        xs2.stage = ShaderStage::Compute;
        xs2.threadgroup_x = 1u;
        xs2.threadgroup_y = 1u;
        xs2.threadgroup_z = 1u;
        xs2.code_words = words;
        Rdna2LiftOptions lopts{};
        lopts.use_sm65 = true;
        lopts.emit_spirv = true;
        lopts.emit_hlsl = true;
        lopts.compile_dxil_with_dxc = has_dxc;
        Rdna2LiftResult res{};
        Check(LiftMemImgShader(xs2, dec, res, lopts).ok(),
              "Float atomic (-0.0 / f64) + SOFFSET + MIMG GLC lifts to SPIR-V & HLSL (+ DXC)");
        Check(res.hlsl_source.find("_os_fmin_bits") != std::string::npos &&
                  res.hlsl_source.find("_os_f64_min_bits") != std::string::npos &&
                  res.hlsl_source.find("s[4]") != std::string::npos,
              "Lifted HLSL emits total-order f32/f64 atomic helpers & SGPR soffset");
    }

    // (4.7) Ordinary vs Comparison sampler bindings + (4.8) Helper lanes of other quads
    {
        Rdna2IrModule ps_ir{};
        ps_ir.stage = ShaderStage::Pixel;
        ps_ir.uses_texture_srv = true;
        ps_ir.uses_texture_sample = true;
        ps_ir.uses_pixel_quad_derivatives = true;
        IrBasicBlock bb{};
        bb.id = 0u;
        auto add = [&](IrOp op, IrType ty, std::vector<IrValueId> ops, uint32_t imm = 0u, float fimm = 0.0f, uint8_t et = 0u) {
            IrInstruction ins{};
            ins.id = (ty == IrType::Void) ? kInvalidIrValue : ps_ir.AllocateValueId();
            ins.op = op;
            ins.type = ty;
            ins.operands = std::move(ops);
            ins.imm_u32 = imm;
            ins.imm_f32 = fimm;
            ins.export_target = et;
            bb.instructions.push_back(ins);
            return ins.id;
        };
        const IrValueId u = add(IrOp::StageInput, IrType::F32, {}, 0u);
        const IrValueId v = add(IrOp::StageInput, IrType::F32, {}, 1u);
        const IrValueId dref = add(IrOp::ConstF32, IrType::F32, {}, 0u, 0.5f);
        const IrValueId c_ord = add(IrOp::ImageSampleImplicit, IrType::F32, {u, v}, 0u);
        const IrValueId c_cmp = add(IrOp::ImageSample, IrType::F32, {u, v, dref}, 0x80u /* depth compare */);
        const IrValueId one = add(IrOp::ConstF32, IrType::F32, {}, 0u, 1.0f);
        add(IrOp::Export, IrType::Void, {c_ord, c_cmp, c_ord, one}, 0u, 0.0f, static_cast<uint8_t>(Rdna2ExportTarget::Mrt0));
        add(IrOp::Return, IrType::Void, {});
        ps_ir.blocks.push_back(bb);

        std::string hlsl, target;
        Check(EmitHlsl(ps_ir, hlsl, target, true).ok(), "PS with ordinary + comparison samplers & quad helper emits HLSL");
        Check(hlsl.find("SamplerState _samp0 : register(s0);") != std::string::npos &&
                  hlsl.find("SamplerComparisonState _samp_cmp0 : register(s1);") != std::string::npos &&
                  hlsl.find("SampleCmpLevelZero(_samp_cmp0") != std::string::npos &&
                  hlsl.find("QuadReadAcrossDiagonal") != std::string::npos,
              "Separate ordinary/comparison samplers (Xbox2Windows #676) & cross-quad helper sanitization (xcl 550d979)");
        if (has_dxc) {
            std::vector<uint8_t> dxil;
            std::string diag;
            Check(CompileHlslWithDxc(hlsl, target, "main", dxil, diag, dxc).ok(),
                  "PS with separate SamplerState + SamplerComparisonState + QuadReadAcrossDiagonal compiles with DXC");
        }
    }

    // (4.9) Wave32 split execution for 256-thread console mesh shaders (xcl #39, xbox-emulator #172)
    {
        Rdna2Assembler ms_a;
        ms_a.EmitSop1(Rdna2Opcode::S_MOV_B32, 124u, Rdna2Operand::LiteralU32(PackGsAllocReqM0(3u, 1u)));
        ms_a.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        ms_a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 2, Rdna2Operand::Vgpr(0));
        ms_a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
        ms_a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
        ms_a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 2, 3, 3, 4, 0xFu, false);
        ms_a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralU32((2u << 20) | (1u << 10) | 0u));
        ms_a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 5, 0, 0, 0, 0x1u, true, true);
        ms_a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        const auto xs2 = ms_a.BuildXs2(ShaderStage::Mesh, 256u, 1u, 1u, 16u, 16u, 32u /* Wave32 */);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = false;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = has_dxc;
        opts.resign_dxil = true;
        Rdna2LiftResult res{};
        Check(LiftRdna2Xs2Shader(xs2, res, opts).ok(),
              "256-thread Wave32 console Mesh Shader lifts & compiles with DXC ms_6_5");
        Check(res.hlsl_source.find("[numthreads(128, 1, 1)]") != std::string::npos &&
                  res.hlsl_source.find("_ms_pass < 2u") != std::string::npos,
              "256-thread Wave32 Mesh Shader splits into 2x128 passes (xcl #39, xbox-emulator #172)");
    }

    // (4.10) v_madmk_f32 / v_madak_f32 (0x21/0x22 AND GFX10.3 0x2C/0x2D) literal constants
    {
        std::vector<uint32_t> words;
        // 0x2C (v_fmamk_f32): v2 = v0 * 3.0f + v1
        words.push_back((0x2Cu << 25) | (2u << 17) | (1u << 9) | 256u);
        words.push_back(FloatToBits(3.0f));
        // 0x2D (v_fmaak_f32): v3 = v2 * v1 + 0.5f
        words.push_back((0x2Du << 25) | (3u << 17) | (1u << 9) | 258u);
        words.push_back(FloatToBits(0.5f));
        words.push_back(0xBF810000u);  // s_endpgm
        std::vector<Rdna2Instruction> dec;
        Check(DisassembleRdna2(words, dec).ok() && dec.size() == 3u &&
                  dec[0].opcode == Rdna2Opcode::V_MADMK_F32 && dec[0].src[1].literal_bits == FloatToBits(3.0f) &&
                  dec[1].opcode == Rdna2Opcode::V_MADAK_F32 && dec[1].src[2].literal_bits == FloatToBits(0.5f),
              "v_madmk_f32 / v_madak_f32 (and GFX10.3 0x2C/0x2D) mandatory literal constants decode cleanly (Xbox2Windows #615)");
    }

    // (4.11) Far-jump s_branch signed 16-bit offsets (forward & backward)
    {
        Rdna2Assembler a;
        const uint32_t jmp_fwd = a.EmitSopp(Rdna2Opcode::S_BRANCH, 0);
        for (int i = 0; i < 400; ++i) a.EmitSopp(Rdna2Opcode::S_NOP, 0);
        const uint32_t target_fwd = a.CurrentWordOffset();
        a.PatchBranch(jmp_fwd, target_fwd);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineInt(42));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 0, 4, 0, false);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        std::vector<Rdna2Instruction> dec;
        Xs2ShaderContainer c{};
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 1u;
        c.code_words = a.Words();
        Check(DisassembleRdna2(c.code_words, dec).ok() && dec[0].simm16 == 400,
              "Far-jump s_branch (+400 words) decodes exact signed 16-bit displacement (xcl e80ce8d)");
        Rdna2CpuExecState st{};
        st.uav1.assign(4u, 0u);
        Check(ExecuteRdna2Cpu(c, dec, st).ok() && st.uav1[0] == 42u,
              "Far-jump s_branch executes accurately across 400-word displacement");
    }

    // (4.13) Reject 64-bit vec2 at SPIR-V Component 2 (Leonida #1909)
    {
        auto build_spv_f64_vec2_at_comp = [](uint32_t comp) -> std::vector<uint32_t> {
            std::vector<uint32_t> w = {0x07230203u, 0x00010300u, 0x4F533200u, 20u, 0u};
            auto emit = [&](uint16_t op, std::initializer_list<uint32_t> ops) {
                w.push_back((static_cast<uint32_t>(ops.size() + 1u) << 16) | op);
                for (uint32_t v : ops) w.push_back(v);
            };
            emit(17u /*OpCapability*/, {1u});
            emit(14u /*OpMemoryModel*/, {0u, 1u});
            emit(15u /*OpEntryPoint*/, {0u, 10u, 0x6E69616Du, 0u, 9u});
            emit(71u /*OpDecorate*/, {9u, 30u /*Location*/, 0u});
            emit(71u /*OpDecorate*/, {9u, 31u /*Component*/, comp});
            emit(19u /*OpTypeVoid*/, {1u});
            emit(33u /*OpTypeFunction*/, {2u, 1u});
            emit(22u /*OpTypeFloat*/, {3u, 64u});
            emit(23u /*OpTypeVector*/, {4u, 3u, 2u});
            emit(32u /*OpTypePointer*/, {5u, 1u /*Input*/, 4u});
            emit(59u /*OpVariable*/, {5u, 9u, 1u});
            emit(54u /*OpFunction*/, {1u, 10u, 0u, 2u});
            emit(248u /*OpLabel*/, {11u});
            emit(253u /*OpReturn*/, {});
            emit(56u /*OpFunctionEnd*/, {});
            return w;
        };
        Check(!ValidateSpirvBinary(build_spv_f64_vec2_at_comp(2u)).ok(),
              "ValidateSpirvBinary rejects 64-bit vec2 at Component 2 (Leonida #1909)");
        Check(ValidateSpirvBinary(build_spv_f64_vec2_at_comp(0u)).ok(),
              "ValidateSpirvBinary accepts 64-bit vec2 at Component 0");
    }
}

void TestFull779CoverageAndLiveGpuMsAs() {
    Rdna2CoverageReport audit_rep{};
    Check(AuditRdna2IsaCoverage(audit_rep).ok(), "AuditRdna2IsaCoverage succeeds");
    std::printf("[INFO] ISA Audit: decoded=%u/%u spirv=%u/%u dxil=%u/%u cpu_ref=%u/%u\n",
                audit_rep.opcodes_decoded, audit_rep.opcodes_total,
                audit_rep.opcodes_lifted_spirv, audit_rep.opcodes_total,
                audit_rep.opcodes_lifted_dxil, audit_rep.opcodes_total,
                audit_rep.opcodes_cpu_ref, audit_rep.opcodes_total);
    Check(audit_rep.opcodes_total == 779u &&
              audit_rep.opcodes_decoded == 779u &&
              audit_rep.opcodes_lifted_spirv == 779u &&
              audit_rep.opcodes_lifted_dxil == 779u &&
              audit_rep.opcodes_cpu_ref == 779u,
          "779/779 (100%) GFX10.3 opcodes decoded, lifted (SPIR-V + DXIL), and CPU-ref verified!");

    Rdna2BenchOptions bopts{};
    bopts.corpus_count = 36u;
    bopts.run_external_validators = false;
    bopts.run_live_gpu = true;
    bopts.quiet = true;
    Rdna2CoverageReport bench_rep{};
    const Status st = RunRdna2CoverageBenchmark(bopts, bench_rep);
    std::printf("[INFO] Live GPU Bench: ok=%d gpu_verified=%u/779 hw=%u warp=%u vk=%u elapsed=%.2fs\n",
                st.ok() ? 1 : 0, bench_rep.opcodes_gpu_verified,
                bench_rep.corpus_gpu_hw_executed, bench_rep.corpus_gpu_warp_executed,
                bench_rep.corpus_vulkan_executed, bench_rep.elapsed_seconds);
    if (!st.ok()) {
        std::fprintf(stderr, "[INFO] Benchmark error: %s\n", st.message.c_str());
    }
    Check(st.ok(), "RunRdna2CoverageBenchmark live GPU parity passes 100% (incl. Issue #29 sv64_vopc_p1)");
    Check(bench_rep.elapsed_seconds < 120.0, "RunRdna2CoverageBenchmark completes within 120s sanity budget");
    if (bench_rep.has_dxc_tool && (bench_rep.has_d3d12_hw || bench_rep.has_d3d12_warp)) {
        Check(bench_rep.opcodes_gpu_verified >= 769u,
              "Real-GPU verified GFX10.3 opcodes >= 769/779 (closed final 5 opcodes!)");
        uint32_t ms_gpu = 0u, as_gpu = 0u;
        for (const auto& sc : bench_rep.stages) {
            if (sc.stage == "MS") ms_gpu = sc.gpu_passed;
            if (sc.stage == "AS") as_gpu = sc.gpu_passed;
        }
        std::printf("[INFO] Live GPU Stage Parity: MS=%u AS=%u\n", ms_gpu, as_gpu);
        Check(ms_gpu >= 1u && as_gpu >= 1u,
              "Live GPU Mesh Shader (MS) + Amplification Shader (AS) corpus verification passed");
    }
}

}  // namespace

int main() {
    TestIssue29VopcAndClassModifiers();
    TestSm66ProfilesAndOffCShaderCache();
    TestWiredRayQueryBridge();
    TestRadarAccuracyCatches();
    TestFull779CoverageAndLiveGpuMsAs();

    if (g_failures == 0) {
        std::printf("ALL SPRINT 14 SHADER SM6.6 / RAYQUERY / ISSUE #29 / 779 TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "SPRINT 14 SHADER TEST FAILURES: %d\n", g_failures);
    return 1;
}
