// OpenScarlett — Sprint 14 Worker 2 Comprehensive Test Suite
// Validates on BOTH Host D3D12 HW GPU (when available) AND D3D12 WARP:
//  1. Wrapped Enhanced Barriers (CommandListX::Barrier with wrapped ResourceXProxy* buffer &
//     texture barriers — zero crash, clean unwrapping, TitleVAMirror / UnifiedTexture notifications)
//  2. Agility SDK Exports (D3D12SDKVersion, D3D12SDKPath) + graceful inbox fallback when D3D12Core.dll absent
//  3. Xbox Series PSO Stream Subobjects (AS=23->24, MS=24->25, ext 25..30), Mesh PSV0 primitive
//     topology injection + dropping empty legacy VS/HS/DS/GS stages, f16 VS->PS linkage, and
//     ValidateAndMatchStageInterpolation wiring in BOTH CreateGraphicsPipelineState & CreatePipelineState
//  4. ID3D12Resource1, ID3D12Resource2, and ID3D12Heap1 proxy QI & method forwarding (zero raw-host escape)
//  5. Live VramResidencyManager budget tracking, DXGI Adapter3 query, LRU Evict/MakeResident,
//     L0 spill fallback, SetResidencyPriority, EnqueueMakeResident, Series S vs Series X profile
//     auto-selection, and sparse VRAM Window-0 descriptor-heap isolation from title buffers
//  6. Per-hash striped parallel PSO compilation stress (multi-threaded concurrent distinct PSOs)
//  7. FenceWaitDispatcher callback dispatch OUTSIDE mutex (re-entrant callback without deadlock)
//     + device-removal wakeup (DXGI_ERROR_DEVICE_REMOVED)

#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"
#include "gpu/d3d12_xs/command_list_x.h"
#include "gpu/d3d12_xs/command_queue_x.h"
#include "gpu/d3d12_xs/device_x.h"
#include "gpu/d3d12_xs/proxies_x.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace openscarlett::d3d12_xs;

int g_failures = 0;

void Check(bool cond, const char* expr, const std::string& detail = {}) {
    if (!cond) {
        ++g_failures;
        std::cerr << "[FAIL] " << expr;
        if (!detail.empty()) std::cerr << " (" << detail << ")";
        std::cerr << "\n";
    }
}

#define EXPECT(c) Check((c), #c)
#define EXPECT_MSG(c, m) Check((c), #c, (m))

struct ScopedEnv {
    std::string name;
    bool had_old = false;
    std::string old_val;

    ScopedEnv(const char* k, const char* v) : name(k) {
        char buf[512]{};
        const DWORD n = ::GetEnvironmentVariableA(k, buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0u && n < sizeof(buf)) {
            had_old = true;
            old_val.assign(buf, n);
        }
        ::SetEnvironmentVariableA(k, v);
    }
    ~ScopedEnv() {
        ::SetEnvironmentVariableA(name.c_str(), had_old ? old_val.c_str() : nullptr);
    }
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename T>
struct alignas(void*) TestStreamSubobj {
    uint32_t type = 0u;
    T value{};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

ID3D12RootSignature* CreateEmptyRootSig(ID3D12Device* dev) {
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) || !blob) {
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    ID3D12RootSignature* rs = nullptr;
    dev->CreateRootSignature(
        0u, blob->GetBufferPointer(), blob->GetBufferSize(),
        OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&rs));
    blob->Release();
    return rs;
}

bool CompileSm50Shader(const char* hlsl, const char* target, std::vector<uint8_t>& out_bc) {
    out_bc.clear();
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(
        hlsl, std::strlen(hlsl), "s14_test.hlsl", nullptr, nullptr,
        "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0u, &blob, &err);
    if (err) err->Release();
    if (FAILED(hr) || !blob) {
        if (blob) blob->Release();
        return false;
    }
    const auto* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
    out_bc.assign(p, p + blob->GetBufferSize());
    blob->Release();
    return true;
}

// Builds a minimal Xbox DXBC container carrying an XS2 RDNA2 VS or PS with configurable
// interpolation & fp16 min_precision signature elements.
std::vector<uint8_t> BuildXboxGraphicsStageContainer(
    openscarlett::ShaderStage stage,
    bool use_fp16_varying,
    bool flat_shade_interp) {
    openscarlett::Xs2ShaderContainer xs2{};
    xs2.stage = stage;
    xs2.wave_size = 32u;
    xs2.code_words = {0xBF810000u};  // s_endpgm

    openscarlett::XboxFullDxbcBuildDesc desc{};
    desc.stage = stage;
    desc.xs2_blob = openscarlett::BuildXs2Container(xs2);
    desc.xbox_chunk_fourcc = "XBOX";
    desc.use_sig_v1 = true;

    openscarlett::XboxSignatureParamDesc pos{};
    pos.semantic_name = "SV_POSITION";
    pos.semantic_index = 0u;
    pos.system_value_type = 1u;  // SV_POSITION
    pos.component_type = 3u;     // float
    pos.register_index = 0u;
    pos.mask = 0xFu;
    pos.rw_mask = 0xFu;

    openscarlett::XboxSignatureParamDesc uv{};
    uv.semantic_name = "TEXCOORD";
    uv.semantic_index = 0u;
    uv.system_value_type = 0u;
    uv.component_type = 3u;
    uv.register_index = 1u;
    uv.mask = use_fp16_varying ? 0x3u : 0xFu;
    uv.rw_mask = uv.mask;
    uv.min_precision = use_fp16_varying ? 1u : 0u;  // D3D_MIN_PRECISION_FLOAT_16 = 1
    uv.interp_mode = flat_shade_interp
                         ? openscarlett::Rdna2InterpMode::Flat
                         : openscarlett::Rdna2InterpMode::Perspective;

    if (stage == openscarlett::ShaderStage::Vertex) {
        desc.output_signature.push_back(pos);
        desc.output_signature.push_back(uv);
    } else {
        desc.input_signature.push_back(pos);
        // Intentionally mismatch PS mask/precision slightly when testing fp16 linkage reconciliation
        if (use_fp16_varying) {
            uv.mask = 0xCu;  // high half of packed register
            uv.min_precision = 0u;
        }
        desc.input_signature.push_back(uv);

        openscarlett::XboxSignatureParamDesc target0{};
        target0.semantic_name = "SV_Target";
        target0.semantic_index = 0u;
        target0.system_value_type = 0u;
        target0.component_type = 3u;
        target0.register_index = 0u;
        target0.mask = 0xFu;
        target0.rw_mask = 0xFu;
        desc.output_signature.push_back(target0);

        desc.reg_setup.present = true;
        openscarlett::Rdna2PsInputCntl cntl{};
        cntl.attr_index = 1u;
        cntl.offset = 0u;
        cntl.flat_shade = flat_shade_interp;
        cntl.fp16_interp = use_fp16_varying;
        desc.reg_setup.ps_input_cntls.push_back(cntl);
    }

    return openscarlett::BuildFullXboxDxbcContainer(desc);
}

std::vector<uint8_t> BuildXboxMeshOrAmpContainer(openscarlett::ShaderStage stage) {
    using namespace openscarlett;
    XboxFullDxbcBuildDesc desc{};
    desc.stage = stage;
    desc.xbox_chunk_fourcc = "XBOX";
    desc.use_sig_v1 = true;
    desc.reg_setup.present = true;
    desc.reg_setup.inferred_stage = stage;
    desc.reg_setup.vgpr_count = 16u;
    desc.reg_setup.sgpr_count = 16u;

    if (stage == ShaderStage::Amplification) {
        Rdna2Assembler as;
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(1));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);
        desc.xs2_blob = as.BuildXs2(ShaderStage::Amplification, 1u, 1u, 1u, 16u, 16u, 32u);
        desc.reg_setup.threadgroup_x = 1u;
        desc.reg_setup.threadgroup_y = 1u;
        desc.reg_setup.threadgroup_z = 1u;
    } else {
        Rdna2Assembler ms;
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));

        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
        ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
        ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
        ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
        ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);

        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.5f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralF32(0.25f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, false);

        ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
        const uint32_t br_skip = ms.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(1));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(2));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 10, 11, 12, 0, 0x7u, true);
        ms.PatchBranch(br_skip, ms.CurrentWordOffset());
        ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

        desc.xs2_blob = ms.BuildXs2(ShaderStage::Mesh, 3u, 1u, 1u, 16u, 16u, 32u);
        desc.reg_setup.threadgroup_x = 3u;
        desc.reg_setup.threadgroup_y = 1u;
        desc.reg_setup.threadgroup_z = 1u;

        XboxSignatureParamDesc out_pos{};
        out_pos.semantic_name = "SV_Position";
        out_pos.system_value_type = 1u;
        out_pos.component_type = 3u;
        out_pos.register_index = 0u;
        out_pos.mask = 0xFu;
        out_pos.rw_mask = 0xFu;
        desc.output_signature.push_back(out_pos);

        XboxSignatureParamDesc out_tc{};
        out_tc.semantic_name = "TEXCOORD";
        out_tc.semantic_index = 0u;
        out_tc.component_type = 3u;
        out_tc.register_index = 1u;
        out_tc.mask = 0xFu;
        out_tc.rw_mask = 0xFu;
        desc.output_signature.push_back(out_tc);
    }
    return BuildFullXboxDxbcContainer(desc);
}

// ---------------------------------------------------------------------------
// 1. Test Agility SDK Exports & Graceful Fallback
// ---------------------------------------------------------------------------
void TestAgilitySdkExportsAndDiscovery(ID3D12Device* dev, const std::string& tag) {
    HMODULE d3d12_xs_mod = ::GetModuleHandleA("d3d12_xs.dll");
    EXPECT_MSG(d3d12_xs_mod != nullptr, tag + ": d3d12_xs.dll module loaded");
    if (d3d12_xs_mod) {
        const auto* ver_ptr = reinterpret_cast<const UINT*>(
            ::GetProcAddress(d3d12_xs_mod, "D3D12SDKVersion"));
        const auto* path_ptr = reinterpret_cast<const char* const*>(
            ::GetProcAddress(d3d12_xs_mod, "D3D12SDKPath"));
        EXPECT_MSG(ver_ptr != nullptr && *ver_ptr >= 614u,
                   tag + ": d3d12_xs.dll exports D3D12SDKVersion >= 614");
        EXPECT_MSG(path_ptr != nullptr && *path_ptr != nullptr &&
                       std::strstr(*path_ptr, "D3D12") != nullptr,
                   tag + ": d3d12_xs.dll exports D3D12SDKPath containing D3D12");
    }

    IOpenScarlettVramResidencyControl* ctrl = nullptr;
    HRESULT hr = dev->QueryInterface(
        IID_IOpenScarlettVramResidencyControl, reinterpret_cast<void**>(&ctrl));
    EXPECT_MSG(SUCCEEDED(hr) && ctrl != nullptr, tag + ": QI(IOpenScarlettVramResidencyControl)");
    if (ctrl) {
        D3D12XBOX_AGILITY_SDK_INFO ag{};
        EXPECT_MSG(SUCCEEDED(ctrl->GetAgilitySdkInfo(&ag)), tag + ": GetAgilitySdkInfo");
        EXPECT_MSG(ag.exported_sdk_version >= 614u, tag + ": Agility exported_sdk_version >= 614");
        EXPECT_MSG(std::strstr(ag.exported_sdk_path, "D3D12") != nullptr,
                   tag + ": Agility exported_sdk_path valid");
        // Either Agility SDK was loaded cleanly OR inbox fallback is active cleanly (never both false)
        EXPECT_MSG(ag.agility_active == 1u || ag.using_inbox_fallback == 1u,
                   tag + ": Agility SDK active or graceful inbox fallback");
        ctrl->Release();
    }
}

// ---------------------------------------------------------------------------
// 2. Test ID3D12Resource1, ID3D12Resource2, & ID3D12Heap1 Proxy Wrapping
// ---------------------------------------------------------------------------
void TestResource12AndHeap1Proxies(ID3D12Device* dev, const std::string& tag) {
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 256ull * 1024ull;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    ID3D12Heap* heap = nullptr;
    HRESULT hr = dev->CreateHeap(&hd, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&heap));
    EXPECT_MSG(SUCCEEDED(hr) && heap != nullptr, tag + ": CreateHeap");
    if (heap) {
        IOpenScarlettProxyInspect* hinsp = nullptr;
        EXPECT_MSG(SUCCEEDED(heap->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&hinsp))) &&
                       hinsp != nullptr,
                   tag + ": Heap->QI(IOpenScarlettProxyInspect)");
        IUnknown* host_heap_raw = hinsp ? hinsp->GetUnderlyingHostObject() : nullptr;
        EXPECT_MSG(host_heap_raw != nullptr && static_cast<void*>(heap) != static_cast<void*>(host_heap_raw),
                   tag + ": HeapXProxy != raw host ID3D12Heap");
        if (hinsp) hinsp->Release();

#ifdef __ID3D12Heap1_INTERFACE_DEFINED__
        ID3D12Heap1* heap1 = nullptr;
        hr = heap->QueryInterface(OS_D3D12_IID(ID3D12Heap1), reinterpret_cast<void**>(&heap1));
        EXPECT_MSG(SUCCEEDED(hr) && heap1 != nullptr, tag + ": Heap->QI(ID3D12Heap1)");
        if (heap1) {
            EXPECT_MSG(static_cast<void*>(heap1) != static_cast<void*>(host_heap_raw),
                       tag + ": ID3D12Heap1 is wrapped proxy (zero raw-host escape)");
            IOpenScarlettProxyInspect* h1_insp = nullptr;
            EXPECT_MSG(SUCCEEDED(heap1->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&h1_insp))) &&
                           h1_insp && h1_insp->GetUnderlyingHostObject() == host_heap_raw,
                       tag + ": ID3D12Heap1 shares wrapped proxy identity & underlying host heap");
            if (h1_insp) h1_insp->Release();
            ID3D12ProtectedResourceSession* sess = nullptr;
            const HRESULT sess_hr = heap1->GetProtectedResourceSession(
                OS_D3D12_IID(ID3D12ProtectedResourceSession), reinterpret_cast<void**>(&sess));
            EXPECT_MSG(sess_hr == DXGI_ERROR_NOT_FOUND || SUCCEEDED(sess_hr),
                       tag + ": ID3D12Heap1::GetProtectedResourceSession forwarded cleanly");
            if (sess) sess->Release();
            // Pass wrapped ID3D12Heap1 directly into CreatePlacedResource to verify runtime unwrap!
            D3D12_RESOURCE_DESC pbd{};
            pbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            pbd.Width = 65536ull;
            pbd.Height = 1u;
            pbd.DepthOrArraySize = 1u;
            pbd.MipLevels = 1u;
            pbd.SampleDesc.Count = 1u;
            pbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* placed = nullptr;
            EXPECT_MSG(SUCCEEDED(dev->CreatePlacedResource(
                           heap1, 0ull, &pbd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                           OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&placed))) && placed,
                       tag + ": CreatePlacedResource(ID3D12Heap1*) unwraps cleanly");
            if (placed) placed->Release();
            heap1->Release();
        }
#endif
        heap->Release();
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 65536ull;
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* res = nullptr;
    hr = dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
        nullptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&res));
    EXPECT_MSG(SUCCEEDED(hr) && res != nullptr, tag + ": CreateCommittedResource for Resource1/2 test");
    if (res) {
        IOpenScarlettProxyInspect* rinsp = nullptr;
        EXPECT_MSG(SUCCEEDED(res->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&rinsp))) &&
                       rinsp != nullptr,
                   tag + ": Resource->QI(IOpenScarlettProxyInspect)");
        IUnknown* host_res_raw = rinsp ? rinsp->GetUnderlyingHostObject() : nullptr;
        EXPECT_MSG(host_res_raw != nullptr && static_cast<void*>(res) != static_cast<void*>(host_res_raw),
                   tag + ": ResourceXProxy != raw host ID3D12Resource");
        if (rinsp) rinsp->Release();

#ifdef __ID3D12Resource1_INTERFACE_DEFINED__
        ID3D12Resource1* res1 = nullptr;
        hr = res->QueryInterface(OS_D3D12_IID(ID3D12Resource1), reinterpret_cast<void**>(&res1));
        EXPECT_MSG(SUCCEEDED(hr) && res1 != nullptr, tag + ": Resource->QI(ID3D12Resource1)");
        if (res1) {
            EXPECT_MSG(static_cast<void*>(res1) != static_cast<void*>(host_res_raw),
                       tag + ": ID3D12Resource1 is wrapped proxy (zero raw-host escape)");
            IOpenScarlettProxyInspect* r1_insp = nullptr;
            EXPECT_MSG(SUCCEEDED(res1->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&r1_insp))) &&
                           r1_insp && r1_insp->GetUnderlyingHostObject() == host_res_raw,
                       tag + ": ID3D12Resource1 shares wrapped proxy identity & underlying host resource");
            if (r1_insp) r1_insp->Release();
            ID3D12ProtectedResourceSession* sess = nullptr;
            const HRESULT sess_hr = res1->GetProtectedResourceSession(
                OS_D3D12_IID(ID3D12ProtectedResourceSession), reinterpret_cast<void**>(&sess));
            EXPECT_MSG(sess_hr == DXGI_ERROR_NOT_FOUND || SUCCEEDED(sess_hr),
                       tag + ": ID3D12Resource1::GetProtectedResourceSession forwarded cleanly");
            if (sess) sess->Release();
            res1->Release();
        }
#endif

#ifdef __ID3D12Resource2_INTERFACE_DEFINED__
        ID3D12Resource2* res2 = nullptr;
        hr = res->QueryInterface(OS_D3D12_IID(ID3D12Resource2), reinterpret_cast<void**>(&res2));
        EXPECT_MSG(SUCCEEDED(hr) && res2 != nullptr, tag + ": Resource->QI(ID3D12Resource2)");
        if (res2) {
            EXPECT_MSG(static_cast<void*>(res2) != static_cast<void*>(host_res_raw),
                       tag + ": ID3D12Resource2 is wrapped proxy (zero raw-host escape)");
            IOpenScarlettProxyInspect* r2_insp = nullptr;
            EXPECT_MSG(SUCCEEDED(res2->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&r2_insp))) &&
                           r2_insp && r2_insp->GetUnderlyingHostObject() == host_res_raw,
                       tag + ": ID3D12Resource2 shares wrapped proxy identity & underlying host resource");
            if (r2_insp) r2_insp->Release();
            const D3D12_RESOURCE_DESC1 d1 = res2->GetDesc1();
            EXPECT_MSG(d1.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d1.Width == 65536ull,
                       tag + ": ID3D12Resource2::GetDesc1() matches buffer dimensions");
            res2->Release();
        }
#endif
        res->Release();
    }
}

// ---------------------------------------------------------------------------
// 3. Test Wrapped Enhanced Barriers (CommandListX::Barrier)
// ---------------------------------------------------------------------------
void TestWrappedEnhancedBarriers(ID3D12Device* dev, const std::string& tag) {
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    EXPECT_MSG(SUCCEEDED(dev->CreateCommandQueue(&qd, OS_D3D12_IID(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) && queue,
               tag + ": CreateCommandQueue for Enhanced Barriers");

    ID3D12CommandAllocator* alloc = nullptr;
    EXPECT_MSG(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, OS_D3D12_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc))) && alloc,
               tag + ": CreateCommandAllocator for Enhanced Barriers");

    ID3D12GraphicsCommandList* cl = nullptr;
    EXPECT_MSG(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cl))) && cl,
               tag + ": CreateCommandList for Enhanced Barriers");

    ID3D12GraphicsCommandList7* cl7 = nullptr;
    HRESULT hr = cl ? cl->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList7), reinterpret_cast<void**>(&cl7)) : E_FAIL;
    EXPECT_MSG(SUCCEEDED(hr) && cl7 != nullptr, tag + ": CommandListX->QI(ID3D12GraphicsCommandList7)");

    // Create a wrapped buffer (backed by TitleVAMirror via CreatePlacedResourceX!) and wrapped texture.
    void* cpu_mem = ::VirtualAlloc(nullptr, 65536u, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    EXPECT_MSG(cpu_mem != nullptr, tag + ": VirtualAlloc(MEM_WRITE_WATCH) for Enhanced Barrier VA mirror");
    if (cpu_mem) {
        std::memset(cpu_mem, 0x5A, 4096u);
    }

    ID3D12DeviceX* devx = nullptr;
    dev->QueryInterface(IID_ID3D12DeviceX, reinterpret_cast<void**>(&devx));

    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = 65536ull;
    buf_desc.Height = 1u;
    buf_desc.DepthOrArraySize = 1u;
    buf_desc.MipLevels = 1u;
    buf_desc.SampleDesc.Count = 1u;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* wrapped_buf = nullptr;
    if (devx && cpu_mem) {
        hr = devx->CreatePlacedResourceX(
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cpu_mem)),
            &buf_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&wrapped_buf));
    }
    EXPECT_MSG(SUCCEEDED(hr) && wrapped_buf != nullptr, tag + ": CreatePlacedResourceX wrapped buffer");

    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 64u;
    tex_desc.Height = 64u;
    tex_desc.DepthOrArraySize = 1u;
    tex_desc.MipLevels = 1u;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1u;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* wrapped_tex = nullptr;
    hr = dev->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&wrapped_tex));
    EXPECT_MSG(SUCCEEDED(hr) && wrapped_tex != nullptr, tag + ": CreateCommittedResource wrapped texture");

    if (cl7 && wrapped_buf && wrapped_tex && queue) {
        D3D12_GLOBAL_BARRIER gbar{};
        gbar.SyncBefore = D3D12_BARRIER_SYNC_ALL;
        gbar.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        gbar.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        gbar.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;

        D3D12_BUFFER_BARRIER bbar{};
        bbar.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        bbar.SyncAfter = D3D12_BARRIER_SYNC_ALL_SHADING;
        bbar.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        bbar.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        bbar.pResource = wrapped_buf;  // Wrapped ResourceXProxy*!
        bbar.Offset = 0ull;
        bbar.Size = UINT64_MAX;

        D3D12_TEXTURE_BARRIER tbar{};
        tbar.SyncBefore = D3D12_BARRIER_SYNC_ALL;
        tbar.SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
        tbar.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
        tbar.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
        tbar.LayoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
        tbar.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        tbar.pResource = wrapped_tex;  // Wrapped ResourceXProxy*!
        tbar.Subresources.IndexOrFirstMipLevel = 0u;
        tbar.Subresources.NumMipLevels = 1u;
        tbar.Subresources.FirstArraySlice = 0u;
        tbar.Subresources.NumArraySlices = 1u;
        tbar.Subresources.FirstPlane = 0u;
        tbar.Subresources.NumPlanes = 1u;
        tbar.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

        D3D12_BARRIER_GROUP groups[3]{};
        groups[0].Type = D3D12_BARRIER_TYPE_GLOBAL;
        groups[0].NumBarriers = 1u;
        groups[0].pGlobalBarriers = &gbar;
        groups[1].Type = D3D12_BARRIER_TYPE_BUFFER;
        groups[1].NumBarriers = 1u;
        groups[1].pBufferBarriers = &bbar;
        groups[2].Type = D3D12_BARRIER_TYPE_TEXTURE;
        groups[2].NumBarriers = 1u;
        groups[2].pTextureBarriers = &tbar;

        // Must unwrap BOTH ResourceXProxy* without crashing and execute cleanly on GPU/WARP!
        cl7->Barrier(3u, groups);
        EXPECT_MSG(SUCCEEDED(cl->Close()), tag + ": CommandList::Close after Enhanced Barriers");

        ID3D12CommandList* lists[] = {cl};
        queue->ExecuteCommandLists(1u, lists);

        ID3D12Fence* fence = nullptr;
        EXPECT(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), reinterpret_cast<void**>(&fence))));
        if (fence) {
            HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            EXPECT(SUCCEEDED(queue->Signal(fence, 1ull)));
            if (fence->GetCompletedValue() < 1ull) {
                EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1ull, ev)));
                EXPECT_MSG(::WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0,
                           tag + ": GPU/WARP completed Enhanced Barrier submission cleanly");
            }
            if (ev) ::CloseHandle(ev);
            fence->Release();
        }

        IOpenScarlettEnhancedBarrierStats* eb_stats_if = nullptr;
        EXPECT(SUCCEEDED(cl->QueryInterface(IID_IOpenScarlettEnhancedBarrierStats, reinterpret_cast<void**>(&eb_stats_if))));
        if (eb_stats_if) {
            D3D12XBOX_ENHANCED_BARRIER_STATS stats{};
            EXPECT(SUCCEEDED(eb_stats_if->GetEnhancedBarrierStats(&stats)));
            EXPECT_MSG(stats.barrier_calls == 1ull, tag + ": Enhanced Barrier recorded 1 Barrier() call");
            EXPECT_MSG(stats.buffer_barriers_processed == 1ull && stats.texture_barriers_processed == 1ull &&
                           stats.global_barriers_processed == 1ull,
                       tag + ": Enhanced Barrier processed global + buffer + texture groups");
            EXPECT_MSG(stats.proxy_resources_unwrapped == 2ull,
                       tag + ": Enhanced Barrier unwrapped BOTH buffer & texture ResourceXProxy*");
            EXPECT_MSG(stats.va_mirror_notifications >= 1ull,
                       tag + ": Enhanced Barrier notified TitleVAMirror on mirrored buffer");
            EXPECT_MSG((stats.native_barrier_calls + stats.legacy_fallback_calls) == 1ull,
                       tag + ": Enhanced Barrier executed native or legacy fallback cleanly");
            eb_stats_if->Release();
        }
    }

    if (wrapped_tex) wrapped_tex->Release();
    if (wrapped_buf) wrapped_buf->Release();
    if (devx) devx->Release();
    if (cpu_mem) ::VirtualFree(cpu_mem, 0u, MEM_RELEASE);
    if (cl7) cl7->Release();
    if (cl) cl->Release();
    if (alloc) alloc->Release();
    if (queue) queue->Release();
#else
    (void)dev;
    (void)tag;
#endif
}

// ---------------------------------------------------------------------------
// 4. Test Live VramResidencyManager, LRU Evict/MakeResident, L0 Spill,
//    Series S/X Profile Auto-Selection, & Sparse Window-0 Isolation
// ---------------------------------------------------------------------------
void TestLiveVramResidencyAndSparseWindows(ID3D12Device* dev, const std::string& tag) {
    // 4a. Test Series S vs Series X profile auto-selection logic directly.
    constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    {
        ScopedEnv clear_prof("OPENSCARLETT_CONSOLE_PROFILE", nullptr);
        const auto prof_4gb = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(4ull * kGiB, nullptr);
        EXPECT_MSG(prof_4gb.profile_id == 0u && prof_4gb.target_console_vram_bytes == 8ull * kGiB,
                   tag + ": 4 GiB host auto-selects Series S (Lockhart 8 GiB)");

        const auto prof_16gb = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(16ull * kGiB, nullptr);
        EXPECT_MSG(prof_16gb.profile_id == 1u &&
                       prof_16gb.target_console_vram_bytes == (13ull * kGiB + 512ull * 1024ull * 1024ull),
                   tag + ": 16 GiB host auto-selects Series X (Anaconda 13.5 GiB)");

        const auto prof_ov = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(4ull * kGiB, "series_x");
        EXPECT_MSG(prof_ov.profile_id == 1u && prof_ov.overridden_by_env,
                   tag + ": explicit Series X override works on 4 GiB host");
    }

    IOpenScarlettVramResidencyControl* ctrl = nullptr;
    EXPECT_MSG(SUCCEEDED(dev->QueryInterface(IID_IOpenScarlettVramResidencyControl, reinterpret_cast<void**>(&ctrl))) && ctrl,
               tag + ": QI(IOpenScarlettVramResidencyControl) for live residency");
    if (!ctrl) return;

    // 4b. Sparse VRAM Window-0 isolation (Descriptor Heaps Window 0 vs Title Buffers Window 1+).
    D3D12_DESCRIPTOR_HEAP_DESC dhd{};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dhd.NumDescriptors = 128u;
    dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* dheap = nullptr;
    EXPECT_MSG(SUCCEEDED(dev->CreateDescriptorHeap(&dhd, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&dheap))) && dheap,
               tag + ": CreateDescriptorHeap tracked in Window 0");

    uint64_t w0_va1 = 0ull, w0_va2 = 0ull, w1_va1 = 0ull, w1_va2 = 0ull;
    EXPECT(SUCCEEDED(ctrl->AllocateSparseWindowVa(1u, 128ull * 1024ull, &w0_va1)));
    EXPECT(SUCCEEDED(ctrl->AllocateSparseWindowVa(1u, 128ull * 1024ull, &w0_va2)));
    EXPECT(SUCCEEDED(ctrl->AllocateSparseWindowVa(0u, 256ull * 1024ull, &w1_va1)));
    EXPECT(SUCCEEDED(ctrl->AllocateSparseWindowVa(0u, 256ull * 1024ull, &w1_va2)));
    EXPECT_MSG(w0_va1 >= 0x0000000100000000ull && w0_va1 < 0x0000000200000000ull &&
                   w0_va2 >= 0x0000000100000000ull && w0_va2 < 0x0000000200000000ull &&
                   w0_va2 > w0_va1,
               tag + ": Descriptor Heap allocations strictly inside Window 0 [0x1_0000_0000, 0x2_0000_0000)");
    EXPECT_MSG(w1_va1 >= 0x0000000200000000ull && w1_va2 > w1_va1,
               tag + ": Title Buffer allocations strictly isolated in Window 1+ [0x2_0000_0000, ...)");
    EXPECT(SUCCEEDED(ctrl->FreeSparseWindowVa(w0_va1)));
    EXPECT(SUCCEEDED(ctrl->FreeSparseWindowVa(w0_va2)));
    EXPECT(SUCCEEDED(ctrl->FreeSparseWindowVa(w1_va1)));
    EXPECT(SUCCEEDED(ctrl->FreeSparseWindowVa(w1_va2)));

    // 4c. Tight budget override -> verify live LRU eviction, L0 spill, Evict/MakeResident,
    //     SetResidencyPriority, and EnqueueMakeResident!
    EXPECT(SUCCEEDED(ctrl->SetResidencyBudgetOverride(512ull * 1024ull)));  // 512 KiB budget

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256ull * 1024ull;  // 256 KiB each
    bd.Height = 1u;
    bd.DepthOrArraySize = 1u;
    bd.MipLevels = 1u;
    bd.SampleDesc.Count = 1u;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    std::vector<ID3D12Resource*> bufs;
    for (int i = 0; i < 4; ++i) {
        ID3D12Resource* r = nullptr;
        HRESULT hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON,
            nullptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&r));
        EXPECT_MSG(SUCCEEDED(hr) && r != nullptr, tag + ": CreateCommittedResource #" + std::to_string(i) + " under 512 KiB budget");
        if (r) bufs.push_back(r);
    }

    D3D12XBOX_VRAM_RESIDENCY_STATS st_after_alloc{};
    EXPECT(SUCCEEDED(ctrl->GetVramResidencyStats(&st_after_alloc)));
    EXPECT_MSG(st_after_alloc.queried_dxgi_adapter3 == 1u,
               tag + ": IDXGIAdapter3::QueryVideoMemoryInfo succeeded");
    EXPECT_MSG(st_after_alloc.auto_lru_eviction_count >= 1ull,
               tag + ": Tight budget triggered automatic LRU ID3D12Device::Evict");
    EXPECT_MSG(st_after_alloc.window0_descriptor_allocs >= 1ull &&
                   st_after_alloc.window1_title_buffer_allocs >= 4ull,
               tag + ": Sparse Window 0 & Window 1+ stats tracked live");

    // Pin high-priority buffers so next allocation must spill to L0 system RAM!
    ID3D12Device1* dev1 = nullptr;
    ID3D12Device3* dev3 = nullptr;
    dev->QueryInterface(OS_D3D12_IID(ID3D12Device1), reinterpret_cast<void**>(&dev1));
    dev->QueryInterface(OS_D3D12_IID(ID3D12Device3), reinterpret_cast<void**>(&dev3));

    if (dev1 && bufs.size() >= 4u) {
        // Make buffers 2 & 3 resident and mark MAXIMUM priority -> budget (512 KiB) is now full of un-evictable objects!
        ID3D12Pageable* max_objs[2] = {bufs[2], bufs[3]};
        D3D12_RESIDENCY_PRIORITY max_prios[2] = {
            D3D12_RESIDENCY_PRIORITY_MAXIMUM,
            D3D12_RESIDENCY_PRIORITY_MAXIMUM};
        EXPECT_MSG(SUCCEEDED(dev->MakeResident(2u, max_objs)), tag + ": DeviceX::MakeResident(2 wrapped bufs)");
        EXPECT_MSG(SUCCEEDED(dev1->SetResidencyPriority(2u, max_objs, max_prios)),
                   tag + ": DeviceX::SetResidencyPriority(MAXIMUM)");

        // Creating one more 256 KiB DEFAULT buffer cannot evict MAXIMUM buffers -> spills to L0!
        ID3D12Resource* spilled_buf = nullptr;
        HRESULT hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON,
            nullptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&spilled_buf));
        EXPECT_MSG(SUCCEEDED(hr) && spilled_buf != nullptr, tag + ": L0 spill allocation succeeded cleanly");
        if (spilled_buf) {
            D3D12XBOX_VRAM_RESIDENCY_STATS st_spill{};
            EXPECT(SUCCEEDED(ctrl->GetVramResidencyStats(&st_spill)));
            EXPECT_MSG(st_spill.l0_spill_count >= 1ull && st_spill.l0_spilled_bytes >= 256ull * 1024ull,
                       tag + ": L0 system-RAM spill fallback activated under pinned pressure");
            spilled_buf->Release();
        }

        // Verify explicit Evict + EnqueueMakeResident with wrapped fence!
        ID3D12Pageable* one_obj = bufs[0];
        EXPECT_MSG(SUCCEEDED(dev->Evict(1u, &one_obj)), tag + ": DeviceX::Evict(wrapped buf)");
        if (dev3) {
            ID3D12Fence* rfence = nullptr;
            EXPECT(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), reinterpret_cast<void**>(&rfence))));
            if (rfence) {
                // Overbudget + DENY_OVERBUDGET should return E_OUTOFMEMORY cleanly!
                const HRESULT deny_hr = dev3->EnqueueMakeResident(
                    D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET, 1u, &one_obj, rfence, 1ull);
                EXPECT_MSG(deny_hr == E_OUTOFMEMORY,
                           tag + ": EnqueueMakeResident(DENY_OVERBUDGET) returns E_OUTOFMEMORY when full");

                // Restore auto budget and enqueue successfully!
                EXPECT(SUCCEEDED(ctrl->SetResidencyBudgetOverride(0ull)));
                EXPECT_MSG(SUCCEEDED(dev3->EnqueueMakeResident(
                               D3D12_RESIDENCY_FLAG_NONE, 1u, &one_obj, rfence, 2ull)),
                           tag + ": EnqueueMakeResident(NONE) succeeds under auto budget");
                rfence->Release();
            }
        }
    }

    EXPECT(SUCCEEDED(ctrl->SetResidencyBudgetOverride(0ull)));
    if (dev3) dev3->Release();
    if (dev1) dev1->Release();
    for (auto* r : bufs) r->Release();
    if (dheap) dheap->Release();
    ctrl->Release();
}

// ---------------------------------------------------------------------------
// 5. Test Xbox Series PSO Stream Subobject Remapping (AS=23, MS=24, ext 25..30),
//    Mesh PSV0 Topology + Legacy Stage Drop, f16 Linkage, & Interp Validation
// ---------------------------------------------------------------------------
void TestXboxPsoStreamsAndStageLinkage(ID3D12Device* dev, const std::string& tag) {
    ID3D12Device2* dev2 = nullptr;
    IOpenScarlettVramResidencyControl* ctrl = nullptr;
    EXPECT(SUCCEEDED(dev->QueryInterface(OS_D3D12_IID(ID3D12Device2), reinterpret_cast<void**>(&dev2))) && dev2);
    EXPECT(SUCCEEDED(dev->QueryInterface(IID_IOpenScarlettVramResidencyControl, reinterpret_cast<void**>(&ctrl))) && ctrl);
    if (!dev2 || !ctrl) {
        if (dev2) dev2->Release();
        if (ctrl) ctrl->Release();
        return;
    }

    ID3D12RootSignature* rs = CreateEmptyRootSig(dev);
    EXPECT_MSG(rs != nullptr, tag + ": CreateEmptyRootSig for PSO stream test");

    // 5a. Graphics PSO (CreateGraphicsPipelineState) with Xbox VS + PS having fp16 & flat interpolation!
    const auto xbox_vs = BuildXboxGraphicsStageContainer(openscarlett::ShaderStage::Vertex, true, true);
    const auto xbox_ps = BuildXboxGraphicsStageContainer(openscarlett::ShaderStage::Pixel, true, true);

    D3D12XBOX_PSO_STREAM_DIAG_STATS before{};
    EXPECT(SUCCEEDED(ctrl->GetPsoStreamDiagStats(&before)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpso{};
    gpso.pRootSignature = rs;
    gpso.VS = {xbox_vs.data(), xbox_vs.size()};
    gpso.PS = {xbox_ps.data(), xbox_ps.size()};
    gpso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gpso.SampleMask = UINT_MAX;
    gpso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpso.NumRenderTargets = 1u;
    gpso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpso.SampleDesc.Count = 1u;

    ID3D12PipelineState* gfx_pso = nullptr;
    HRESULT hr = dev->CreateGraphicsPipelineState(
        &gpso, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&gfx_pso));
    EXPECT_MSG(SUCCEEDED(hr) && gfx_pso != nullptr,
               tag + ": CreateGraphicsPipelineState(Xbox VS+PS fp16+flat interp)");
    if (gfx_pso) gfx_pso->Release();

    D3D12XBOX_PSO_STREAM_DIAG_STATS after_gfx{};
    EXPECT(SUCCEEDED(ctrl->GetPsoStreamDiagStats(&after_gfx)));
    EXPECT_MSG(after_gfx.f16_stage_links_reconciled > before.f16_stage_links_reconciled,
               tag + ": CreateGraphicsPipelineState reconciled f16 VS->PS linkage");
    EXPECT_MSG(after_gfx.interp_validations_run > before.interp_validations_run &&
                   after_gfx.interp_attributes_matched > before.interp_attributes_matched,
               tag + ": CreateGraphicsPipelineState ran ValidateAndMatchStageInterpolation");

    // 5b. CreatePipelineState stream with Xbox Series VS+PS + Xbox ext tag 29 (stripped cleanly)
    {
        struct alignas(void*) XboxGfxStream {
            TestStreamSubobj<ID3D12RootSignature*> root_sig;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> vs;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> ps;
            TestStreamSubobj<D3D12_PRIMITIVE_TOPOLOGY_TYPE> topo;
            TestStreamSubobj<D3D12_RT_FORMAT_ARRAY> rts;
            TestStreamSubobj<UINT> xbox_ext_29;
        } stream{};
        stream.root_sig.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE);
        stream.root_sig.value = rs;
        stream.vs.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS);
        stream.vs.value = {xbox_vs.data(), xbox_vs.size()};
        stream.ps.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS);
        stream.ps.value = {xbox_ps.data(), xbox_ps.size()};
        stream.topo.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY);
        stream.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        stream.rts.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS);
        stream.rts.value.NumRenderTargets = 1u;
        stream.rts.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.xbox_ext_29.type = 29u;  // Xbox-only extension subobject tag
        stream.xbox_ext_29.value = 0x1u;

        D3D12_PIPELINE_STATE_STREAM_DESC sdesc{sizeof(stream), &stream};
        ID3D12PipelineState* stream_pso = nullptr;
        hr = dev2->CreatePipelineState(&sdesc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&stream_pso));
        EXPECT_MSG(SUCCEEDED(hr) && stream_pso != nullptr,
                   tag + ": CreatePipelineState(Xbox stream + ext tag 29 + VS/PS fp16/interp)");
        if (stream_pso) stream_pso->Release();

        D3D12XBOX_PSO_STREAM_DIAG_STATS after_stream{};
        EXPECT(SUCCEEDED(ctrl->GetPsoStreamDiagStats(&after_stream)));
        EXPECT_MSG(after_stream.remapped_ext_tags > after_gfx.remapped_ext_tags,
                   tag + ": CreatePipelineState stripped/remapped Xbox ext tag 29");
        EXPECT_MSG(after_stream.f16_stage_links_reconciled > after_gfx.f16_stage_links_reconciled,
                   tag + ": CreatePipelineState reconciled f16 VS->PS linkage");
        EXPECT_MSG(after_stream.interp_validations_run > after_gfx.interp_validations_run,
                   tag + ": CreatePipelineState ran ValidateAndMatchStageInterpolation");
    }

    // 5c. Xbox Series Mesh PSO Stream with Xbox tags AS=23, MS=24, Xbox ext=28,
    //     empty legacy VS/HS/DS/GS stages (dropped!), and omitted PRIMITIVE_TOPOLOGY (inferred from PSV0!).
    {
        const auto xbox_as = BuildXboxMeshOrAmpContainer(openscarlett::ShaderStage::Amplification);
        const auto xbox_ms = BuildXboxMeshOrAmpContainer(openscarlett::ShaderStage::Mesh);

        D3D12XBOX_PSO_STREAM_DIAG_STATS pre_mesh{};
        EXPECT(SUCCEEDED(ctrl->GetPsoStreamDiagStats(&pre_mesh)));

        struct alignas(void*) XboxMeshStream {
            TestStreamSubobj<ID3D12RootSignature*> root_sig;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> empty_vs;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> empty_hs;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> empty_ds;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> empty_gs;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> xbox_as_23;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> xbox_ms_24;
            TestStreamSubobj<D3D12_SHADER_BYTECODE> ps;
            TestStreamSubobj<D3D12_RT_FORMAT_ARRAY> rts;
            TestStreamSubobj<UINT> xbox_ext_28;
        } mstream{};
        mstream.root_sig.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE);
        mstream.root_sig.value = rs;
        mstream.empty_vs.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS);
        mstream.empty_hs.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS);
        mstream.empty_ds.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS);
        mstream.empty_gs.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS);
        mstream.xbox_as_23.type = 23u;  // Xbox Series AS tag (23 -> Desktop 24)
        mstream.xbox_as_23.value = {xbox_as.data(), xbox_as.size()};
        mstream.xbox_ms_24.type = 24u;  // Xbox Series MS tag (24 -> Desktop 25)
        mstream.xbox_ms_24.value = {xbox_ms.data(), xbox_ms.size()};
        mstream.ps.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS);
        mstream.ps.value = {xbox_ps.data(), xbox_ps.size()};
        mstream.rts.type = static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS);
        mstream.rts.value.NumRenderTargets = 1u;
        mstream.rts.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        mstream.xbox_ext_28.type = 28u;
        mstream.xbox_ext_28.value = 0u;

        D3D12_PIPELINE_STATE_STREAM_DESC ms_desc{sizeof(mstream), &mstream};
        ID3D12PipelineState* mesh_pso = nullptr;
        // Note: on adapters with MeshShaderTier == 0 (e.g. Win10 inbox WARP), host CreatePipelineState
        // may return E_INVALIDARG/DXGI_ERROR_UNSUPPORTED after normalization, which is expected;
        // on Mesh-capable GPUs (e.g. GTX 1650 Ti) it succeeds live! Either way, normalization counters MUST increment.
        hr = dev2->CreatePipelineState(&ms_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&mesh_pso));
        if (SUCCEEDED(hr) && mesh_pso) {
            mesh_pso->Release();
        }

        D3D12XBOX_PSO_STREAM_DIAG_STATS post_mesh{};
        EXPECT(SUCCEEDED(ctrl->GetPsoStreamDiagStats(&post_mesh)));
        EXPECT_MSG(post_mesh.remapped_as_tags > pre_mesh.remapped_as_tags,
                   tag + ": Xbox Series AS tag (23 -> Desktop 24) remapped");
        EXPECT_MSG(post_mesh.remapped_ms_tags > pre_mesh.remapped_ms_tags,
                   tag + ": Xbox Series MS tag (24 -> Desktop 25) remapped");
        EXPECT_MSG(post_mesh.remapped_ext_tags > pre_mesh.remapped_ext_tags,
                   tag + ": Xbox Series ext tag (28) remapped/stripped");
        EXPECT_MSG(post_mesh.dropped_legacy_stages >= pre_mesh.dropped_legacy_stages + 4ull,
                   tag + ": Dropped all 4 empty legacy VS/HS/DS/GS stages on Mesh PSO");
        EXPECT_MSG(post_mesh.inferred_mesh_topologies > pre_mesh.inferred_mesh_topologies,
                   tag + ": Inferred Mesh PSO primitive topology from PSV0 metadata");
    }

    if (rs) rs->Release();
    ctrl->Release();
    dev2->Release();
}

// ---------------------------------------------------------------------------
// 6. Test Per-Hash Striped Parallel PSO Compilation Stress
// ---------------------------------------------------------------------------
void TestParallelStripedPsoCompilation(ID3D12Device* dev, const std::string& tag) {
    ID3D12RootSignature* rs = CreateEmptyRootSig(dev);
    EXPECT_MSG(rs != nullptr, tag + ": CreateEmptyRootSig for parallel PSO stress");
    if (!rs) return;

    constexpr int kNumThreads = 6;
    std::atomic<int> ok_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t]() {
            const std::string vs_src =
                "float4 main(uint id : SV_VertexID) : SV_Position { return float4(float(id + " +
                std::to_string(t) + "), 0.0f, 0.0f, 1.0f); }";
            const std::string ps_src =
                "float4 main() : SV_Target { return float4(0.1f * " + std::to_string(t + 1) +
                ", 0.5f, 0.25f, 1.0f); }";
            std::vector<uint8_t> vs_bc, ps_bc;
            if (!CompileSm50Shader(vs_src.c_str(), "vs_5_0", vs_bc) ||
                !CompileSm50Shader(ps_src.c_str(), "ps_5_0", ps_bc)) {
                return;
            }
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = rs;
            desc.VS = {vs_bc.data(), vs_bc.size()};
            desc.PS = {ps_bc.data(), ps_bc.size()};
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1u;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1u;

            ID3D12PipelineState* pso = nullptr;
            if (SUCCEEDED(dev->CreateGraphicsPipelineState(
                    &desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&pso))) &&
                pso != nullptr) {
                ok_count.fetch_add(1, std::memory_order_relaxed);
                pso->Release();
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_MSG(ok_count.load() == kNumThreads,
               tag + ": Parallel striped PSO compilation succeeded on all " + std::to_string(kNumThreads) + " threads");
    rs->Release();
}

// ---------------------------------------------------------------------------
// 7. Test FenceWaitDispatcher Re-Entrant Callback (Outside Mutex) + Device Removal Wakeup
// ---------------------------------------------------------------------------
struct ReentrantCbCtx {
    IOpenScarlettFenceWaitDispatcher* dispatcher = nullptr;
    ID3D12Fence* fence = nullptr;
    std::atomic<int> primary_fired{0};
    std::atomic<int> secondary_fired{0};
    std::atomic<HRESULT> last_status{E_FAIL};
};

void SecondaryFenceCallback(ID3D12Fence*, UINT64, HRESULT status, void* ctx) {
    auto* c = static_cast<ReentrantCbCtx*>(ctx);
    c->last_status.store(status, std::memory_order_release);
    c->secondary_fired.fetch_add(1, std::memory_order_acq_rel);
}

void PrimaryReentrantFenceCallback(ID3D12Fence* fence, UINT64 val, HRESULT status, void* ctx) {
    auto* c = static_cast<ReentrantCbCtx*>(ctx);
    c->primary_fired.fetch_add(1, std::memory_order_acq_rel);
    // Re-entrantly call RegisterFenceCallback & PumpFenceCallbacks inside callback!
    // If the dispatcher held its mutex across the callback, this would DEADLOCK immediately!
    if (SUCCEEDED(status) && c->dispatcher && fence) {
        uint64_t reg2 = 0ull;
        (void)c->dispatcher->RegisterFenceCallback(fence, val, &SecondaryFenceCallback, ctx, &reg2);
        uint32_t pumped = 0u;
        (void)c->dispatcher->PumpFenceCallbacks(&pumped);
    }
}

void TestFenceWaitDispatcherLockSafetyAndDeviceRemoval(bool prefer_warp) {
    const std::string tag = prefer_warp ? "WARP" : "GPU";
    ScopedEnv warp_env("OPENSCARLETT_PREFER_WARP", prefer_warp ? "1" : "0");

    // Create a dedicated device so RemoveDevice() does not affect earlier tests.
    ID3D12Device5* dev5 = nullptr;
    HRESULT hr = D3D12CreateDeviceX(
        nullptr, D3D_FEATURE_LEVEL_12_0,
        OS_D3D12_IID(ID3D12Device5), reinterpret_cast<void**>(&dev5));
    EXPECT_MSG(SUCCEEDED(hr) && dev5 != nullptr, tag + ": Dedicated Device5 for FenceWaitDispatcher test");
    if (!dev5) return;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    EXPECT(SUCCEEDED(dev5->CreateCommandQueue(&qd, OS_D3D12_IID(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) && queue);

    IOpenScarlettFenceWaitDispatcher* disp = nullptr;
    EXPECT_MSG(queue && SUCCEEDED(queue->QueryInterface(IID_IOpenScarlettFenceWaitDispatcher, reinterpret_cast<void**>(&disp))) && disp,
               tag + ": CommandQueueX->QI(IOpenScarlettFenceWaitDispatcher)");

    ID3D12Fence* fence = nullptr;
    EXPECT(SUCCEEDED(dev5->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), reinterpret_cast<void**>(&fence))) && fence);

    if (disp && fence && queue) {
        // 7a. Re-entrant callback test (MUST complete without deadlock!).
        ReentrantCbCtx ctx{};
        ctx.dispatcher = disp;
        ctx.fence = fence;
        uint64_t reg1 = 0ull;
        EXPECT_MSG(SUCCEEDED(disp->RegisterFenceCallback(fence, 1ull, &PrimaryReentrantFenceCallback, &ctx, &reg1)),
                   tag + ": RegisterFenceCallback(target=1)");
        EXPECT(SUCCEEDED(queue->Signal(fence, 1ull)));

        // Wait up to 3 seconds for worker / pump to fire both primary + re-entrant secondary callback.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (ctx.secondary_fired.load(std::memory_order_acquire) == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            uint32_t pumped = 0u;
            (void)disp->PumpFenceCallbacks(&pumped);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_MSG(ctx.primary_fired.load() == 1 && ctx.secondary_fired.load() == 1,
                   tag + ": Re-entrant FenceWaitDispatcher callback executed OUTSIDE mutex with zero deadlock");

        // 7b. Blocked WaitForFenceValue(target=999999) + pending callback MUST wake up immediately on RemoveDevice()!
        std::atomic<HRESULT> wait_result{S_OK};
        std::atomic<bool> wait_started{false};
        std::atomic<bool> wait_finished{false};

        std::atomic<HRESULT> rm_cb_status{S_OK};
        std::atomic<int> rm_cb_fired{0};
        auto on_rm_cb = [](ID3D12Fence*, UINT64, HRESULT st, void* uctx) {
            auto* pair = static_cast<std::pair<std::atomic<HRESULT>*, std::atomic<int>*>*>(uctx);
            pair->first->store(st, std::memory_order_release);
            pair->second->fetch_add(1, std::memory_order_acq_rel);
        };
        std::pair<std::atomic<HRESULT>*, std::atomic<int>*> rm_pair{&rm_cb_status, &rm_cb_fired};
        uint64_t reg_rm = 0ull;
        EXPECT(SUCCEEDED(disp->RegisterFenceCallback(fence, 999999ull, on_rm_cb, &rm_pair, &reg_rm)));

        std::thread waiter([&]() {
            wait_started.store(true, std::memory_order_release);
            const HRESULT wr = disp->WaitForFenceValue(fence, 999999ull, 10000u);
            wait_result.store(wr, std::memory_order_release);
            wait_finished.store(true, std::memory_order_release);
        });

        while (!wait_started.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const auto t0 = std::chrono::steady_clock::now();
        dev5->RemoveDevice();
        waiter.join();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        EXPECT_MSG(wait_finished.load() && FAILED(wait_result.load()),
                   tag + ": Blocked WaitForFenceValue woke cleanly with error HRESULT on RemoveDevice()");
        EXPECT_MSG(elapsed_ms < 2000,
                   tag + ": Device-removal wakeup completed promptly (" + std::to_string(elapsed_ms) + " ms)");
        EXPECT_MSG(rm_cb_fired.load() >= 1 && FAILED(rm_cb_status.load()),
                   tag + ": Pending fence callback invoked with device-removed HRESULT on RemoveDevice()");
    }

    if (fence) fence->Release();
    if (disp) disp->Release();
    if (queue) queue->Release();
    dev5->Release();
}

void RunAllForBackend(bool prefer_warp) {
    const std::string tag = prefer_warp ? "WARP" : "GPU";
    std::cout << "=== Sprint 14 D3D12 Agility / Barriers / Residency Suite (" << tag << ") ===\n";

    ScopedEnv warp_env("OPENSCARLETT_PREFER_WARP", prefer_warp ? "1" : "0");
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDeviceX(
        nullptr, D3D_FEATURE_LEVEL_12_0,
        OS_D3D12_IID(ID3D12Device), reinterpret_cast<void**>(&dev));
    EXPECT_MSG(SUCCEEDED(hr) && dev != nullptr, tag + ": D3D12CreateDeviceX");
    if (!dev) return;

    TestAgilitySdkExportsAndDiscovery(dev, tag);
    TestResource12AndHeap1Proxies(dev, tag);
    TestWrappedEnhancedBarriers(dev, tag);
    TestLiveVramResidencyAndSparseWindows(dev, tag);
    TestXboxPsoStreamsAndStageLinkage(dev, tag);
    TestParallelStripedPsoCompilation(dev, tag);

    dev->Release();

    // Separate device for RemoveDevice() test
    TestFenceWaitDispatcherLockSafetyAndDeviceRemoval(prefer_warp);
}

}  // namespace

int main() {
    // Portable-bundle safe: no OPENSCARLETT_SOURCE_DIR dependency; temp_directory_path() if needed.
    (void)std::filesystem::temp_directory_path();

    // Run on Host HW GPU (falls back to WARP automatically if no HW adapter) AND explicitly on D3D12 WARP!
    RunAllForBackend(/*prefer_warp=*/false);
    RunAllForBackend(/*prefer_warp=*/true);

    if (g_failures == 0) {
        std::cout << "[PASS] test_s14_d3d12_barriers_residency (0 failures)\n";
        return 0;
    }
    std::cerr << "[FAIL] test_s14_d3d12_barriers_residency (" << g_failures << " failures)\n";
    return 1;
}
