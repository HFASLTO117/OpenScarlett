#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"
#include "rdna2_isa_scalar_vector.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

using namespace openscarlett;
using namespace openscarlett::rdna2_sv;

uint32_t FloatBits(float f) noexcept {
    uint32_t b = 0u;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

bool HasSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(kDefaultSpirvValPath, ec);
}

void AppendMubuf(std::vector<uint32_t>& words, Rdna2Opcode op, uint8_t vdata, uint8_t vaddr, uint8_t srsrc, uint16_t off = 0u) {
    Rdna2Assembler a;
    a.EmitMubuf(op, vdata, vaddr, srsrc, off, true, false, 128u);
    words.insert(words.end(), a.Words().begin(), a.Words().end());
}

void Emit(std::vector<uint32_t>& words,
          const char* mnem,
          uint8_t dst,
          const std::vector<Rdna2Operand>& srcs,
          int32_t imm = 0,
          const DecodedSvModifier* mod = nullptr) {
    const bool ok = EncodeScalarVectorInst(mnem, dst, srcs, imm, words, mod);
    if (!ok) {
        std::fprintf(stderr, "[FAIL] EncodeScalarVectorInst failed for %s\n", mnem);
    }
    OS_REQUIRE(ok);
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct D3D12ComputeRunner {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device2> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList6> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0u;
    std::string adapter_name;
    uint32_t highest_sm = 0u;

    ~D3D12ComputeRunner() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    bool Init(bool use_warp) {
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        if (use_warp) {
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
        } else {
            if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
                return false;
            }
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
        adapter_name = buf;
        if (!use_warp &&
            (((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u) ||
             (desc.VendorId == 0x1414u && (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0)))) {
            return false;
        }

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return false;
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd_list)))) return false;
        cmd_list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    ComPtr<ID3D12Resource> CreateBuf(uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> r;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r))));
        return r;
    }

    void Run(const std::vector<uint8_t>& dxil,
             const std::vector<uint32_t>& in_data,
             std::vector<uint32_t>& out_data,
             uint32_t groups_x) {
        const uint64_t bytes = static_cast<uint64_t>(in_data.size()) * sizeof(uint32_t);
        out_data.assign(in_data.size(), 0u);

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor = {0u, 0u};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {1u, 0u};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2u;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig_blob, err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> rs;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&rs))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = rs.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] CreateComputePipelineState on %s hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        auto in_up = CreateBuf(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_up = CreateBuf(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuf(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuf(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuf(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        void* ptr = nullptr;
        OS_REQUIRE(SUCCEEDED(in_up->Map(0, nullptr, &ptr)));
        std::memcpy(ptr, in_data.data(), static_cast<size_t>(bytes));
        in_up->Unmap(0, nullptr);
        OS_REQUIRE(SUCCEEDED(out_up->Map(0, nullptr, &ptr)));
        std::memset(ptr, 0, static_cast<size_t>(bytes));
        out_up->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));
        cmd_list->CopyResource(in_gpu.Get(), in_up.Get());
        cmd_list->CopyResource(out_gpu.Get(), out_up.Get());

        D3D12_RESOURCE_BARRIER b0[2]{};
        b0[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b0[0].Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        b0[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b0[1].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(2u, b0);

        cmd_list->SetComputeRootSignature(rs.Get());
        cmd_list->SetComputeRootUnorderedAccessView(0u, in_gpu->GetGPUVirtualAddress());
        cmd_list->SetComputeRootUnorderedAccessView(1u, out_gpu->GetGPUVirtualAddress());
        cmd_list->Dispatch(groups_x, 1u, 1u);

        D3D12_RESOURCE_BARRIER ubar{};
        ubar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ubar.UAV.pResource = out_gpu.Get();
        cmd_list->ResourceBarrier(1u, &ubar);

        D3D12_RESOURCE_BARRIER b1{};
        b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b1.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1u, &b1);
        cmd_list->CopyResource(rb.Get(), out_gpu.Get());
        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));

        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1u, lists);
        ++fence_val;
        OS_REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), fence_val)));
        if (fence->GetCompletedValue() < fence_val) {
            OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(fence_val, fence_event)));
            WaitForSingleObject(fence_event, 15000u);
        }

        OS_REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &ptr)));
        std::memcpy(out_data.data(), ptr, static_cast<size_t>(bytes));
        rb->Unmap(0, nullptr);
    }
};
#endif

struct LiveAdapters {
#if defined(_WIN32)
    D3D12ComputeRunner d3d12_hw;
    D3D12ComputeRunner d3d12_warp;
    bool has_d3d12_hw = false;
    bool has_d3d12_warp = false;
#endif
    VulkanBackend vk;
    bool has_vk_hw = false;
};

void VerifyShaderLiveParity(
    const char* name,
    const std::vector<uint32_t>& words,
    const std::vector<uint32_t>& in_uav0,
    uint32_t out_count,
    uint32_t tg_x,
    LiveAdapters& adapters,
    bool is_float_approx = false,
    float float_eps = 1e-4f) {
    std::vector<SvDecodedInst> insts;
    Status st = DecodeScalarVectorStream(words, insts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] DecodeScalarVectorStream(%s): %s\n", name, st.message.c_str());
    }
    OS_REQUIRE(st.ok());

    // 1. CPU reference execution
    SvWaveState wave{};
    wave.Reset(32u);
    wave.SeedThreadIds(0u);
    wave.uav0 = in_uav0;
    wave.uav1.assign(out_count, 0u);
    st = ExecuteScalarVectorCpu(insts, wave);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] ExecuteScalarVectorCpu(%s): %s\n", name, st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(wave.uav1.size() >= out_count);

    // 2. Lift to SPIR-V 1.3 + SM 6.5 DXIL
    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.threadgroup_x = tg_x;
    c.threadgroup_y = 1u;
    c.threadgroup_z = 1u;
    c.wave_size = 32u;
    c.code_words = words;

    Rdna2LiftOptions opt{};
    opt.use_sm65 = true;
    opt.emit_spirv = true;
    opt.emit_hlsl = true;
    opt.validate_spirv_external = HasSpirvVal();
    opt.compile_dxil_with_dxc = true;
    opt.resign_dxil = true;

    Rdna2LiftResult lifted{};
    st = LiftScalarVectorShader(c, insts, lifted, opt);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] LiftScalarVectorShader(%s): %s\nDXC: %s\nSPV: %s\nHLSL:\n%s\n",
                     name, st.message.c_str(), lifted.dxc_diagnostics.c_str(),
                     lifted.spirv_val_diagnostics.c_str(), lifted.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(ValidateSpirvBinary(lifted.spirv_words).ok());
    if (HasSpirvVal()) {
        std::string diag;
        OS_REQUIRE(ValidateSpirvExternal(lifted.spirv_words, diag, kDefaultSpirvValPath, "vulkan1.1").ok());
        OS_REQUIRE(ValidateSpirvExternal(lifted.spirv_words, diag, kDefaultSpirvValPath, "vulkan1.2").ok());
    }
    OS_REQUIRE(!lifted.pc_dxil.empty());
    OS_REQUIRE(VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));

    auto compare_outputs = [&](const char* backend_label, const std::vector<uint32_t>& got) {
        OS_REQUIRE(got.size() >= out_count);
        for (uint32_t i = 0u; i < out_count; ++i) {
            if (is_float_approx) {
                const float exp_f = BitsFloat(wave.uav1[i]);
                const float got_f = BitsFloat(got[i]);
                if (std::isnan(exp_f)) {
                    OS_REQUIRE(std::isnan(got_f));
                } else {
                    const float diff = std::fabs(exp_f - got_f);
                    const float tol = float_eps * std::max(1.0f, std::fabs(exp_f));
                    if (diff > tol) {
                        std::fprintf(stderr, "[FAIL] %s (%s) idx=%u: expected=%.7g (0x%08X) got=%.7g (0x%08X) diff=%.7g\n",
                                     name, backend_label, i, exp_f, wave.uav1[i], got_f, got[i], diff);
                        OS_REQUIRE(false);
                    }
                }
            } else {
                if (got[i] != wave.uav1[i]) {
                    std::fprintf(stderr, "[FAIL] %s (%s) idx=%u: expected=0x%08X got=0x%08X\n",
                                 name, backend_label, i, wave.uav1[i], got[i]);
                    OS_REQUIRE(false);
                }
            }
        }
    };

    // 3. Live D3D12 HW + D3D12 WARP execution
    std::vector<uint32_t> padded_in = in_uav0;
    if (padded_in.size() < out_count) padded_in.resize(out_count, 0u);
#if defined(_WIN32)
    if (adapters.has_d3d12_hw) {
        std::vector<uint8_t> hw_dxil = lifted.pc_dxil;
        if (adapters.d3d12_hw.highest_sm < static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
            const char* tgt = (adapters.d3d12_hw.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) ? "cs_6_2" : "cs_6_0";
            std::string diag;
            OS_REQUIRE(CompileHlslWithDxc(lifted.hlsl_source, tgt, "main", hw_dxil, diag).ok());
        }
        std::vector<uint32_t> hw_out;
        adapters.d3d12_hw.Run(hw_dxil, padded_in, hw_out, 1u);
        compare_outputs("D3D12-HW", hw_out);
    }
    if (adapters.has_d3d12_warp) {
        std::vector<uint8_t> warp_dxil = lifted.pc_dxil;
        if (adapters.d3d12_warp.highest_sm < static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
            const char* tgt = (adapters.d3d12_warp.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) ? "cs_6_2" : "cs_6_0";
            std::string diag;
            OS_REQUIRE(CompileHlslWithDxc(lifted.hlsl_source, tgt, "main", warp_dxil, diag).ok());
        }
        std::vector<uint32_t> warp_out;
        adapters.d3d12_warp.Run(warp_dxil, padded_in, warp_out, 1u);
        compare_outputs("D3D12-WARP", warp_out);
    }
#endif

    // 4. Live Vulkan HW execution (gracefully guarded when no ICD is present)
    if (adapters.has_vk_hw) {
        std::vector<uint32_t> vk_out;
        Status vst = adapters.vk.ExecuteComputeShader(lifted.spirv_words, padded_in, vk_out, 1u);
        if (!vst.ok()) {
            std::fprintf(stderr, "[FAIL] Vulkan ExecuteComputeShader(%s): %s\n", name, vst.message.c_str());
        }
        OS_REQUIRE(vst.ok());
        compare_outputs("Vulkan-HW", vk_out);
    }

    std::printf("[PASS] %s: CPU == D3D12-HW == D3D12-WARP%s (%u outputs, spv=%zu words, dxil=%zu bytes)\n",
                name,
                adapters.has_vk_hw ? " == Vulkan-HW" : " (Vulkan ICD N/A)",
                out_count,
                lifted.spirv_words.size(),
                lifted.pc_dxil.size());
}

// ---------------------------------------------------------------------------
// Test 1: Full Opcode Table Completeness & 100% Encode -> Decode Round-Trip
// ---------------------------------------------------------------------------
void TestOpcodeTableRoundTrip() {
    const auto& table = GetScalarVectorOpcodeTable();
    const size_t uniq = GetUniqueScalarVectorMnemonicCount();
    std::printf("[INFO] Scalar+Vector table entries=%zu, unique mnemonics=%zu (target >= 435)\n",
                table.size(), uniq);
    OS_REQUIRE(uniq >= 435u);

    std::unordered_map<uint8_t, size_t> per_enc;
    for (const auto& e : table) {
        ++per_enc[static_cast<uint8_t>(e.encoding)];
        OS_REQUIRE(LookupScalarVectorOpcode(e.encoding, e.raw_opcode) == &e);
        OS_REQUIRE(LookupScalarVectorByName(e.mnemonic) != nullptr);

        std::vector<Rdna2Operand> srcs;
        int32_t imm = 0;
        if (e.encoding == Rdna2Encoding::SOP2 || e.encoding == Rdna2Encoding::SOPC) {
            srcs = {Rdna2Operand::Sgpr(2u), Rdna2Operand::Sgpr(4u)};
        } else if (e.encoding == Rdna2Encoding::SOP1) {
            if (e.src_count > 0u) srcs = {Rdna2Operand::Sgpr(2u)};
        } else if (e.encoding == Rdna2Encoding::SOPK) {
            imm = 0x1234;
            if (e.has_mandatory_literal) srcs = {Rdna2Operand::LiteralU32(0xCAFEBABEu)};
        } else if (e.encoding == Rdna2Encoding::SOPP) {
            imm = 3;
        } else if (e.encoding == Rdna2Encoding::SMEM) {
            srcs = {Rdna2Operand::Sgpr(4u)};
            imm = 16;
        } else if (e.encoding == Rdna2Encoding::VOP1) {
            if (e.src_count > 0u) srcs = {Rdna2Operand::Vgpr(2u)};
        } else if (e.encoding == Rdna2Encoding::VOP2) {
            if (e.has_mandatory_literal) {
                if (e.raw_opcode == 0x21u || e.raw_opcode == 0x2Cu || e.raw_opcode == 0x37u) {
                    srcs = {Rdna2Operand::Vgpr(1u), Rdna2Operand::LiteralU32(0x3F800000u), Rdna2Operand::Vgpr(2u)};
                } else {
                    srcs = {Rdna2Operand::Vgpr(1u), Rdna2Operand::Vgpr(2u), Rdna2Operand::LiteralU32(0x3F800000u)};
                }
            } else {
                srcs = {Rdna2Operand::Vgpr(1u), Rdna2Operand::Vgpr(2u)};
            }
        } else if (e.encoding == Rdna2Encoding::VOPC) {
            srcs = {Rdna2Operand::Vgpr(1u), Rdna2Operand::Vgpr(2u)};
        } else if (e.encoding == Rdna2Encoding::VOP3 || e.encoding == Rdna2Encoding::VOP3P) {
            srcs = {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(6u)};
        } else if (e.encoding == Rdna2Encoding::VINTRP) {
            srcs = {Rdna2Operand::Vgpr(2u)};
            imm = (5 << 2) | 2;
        }

        std::vector<uint32_t> words;
        const bool enc_ok = EncodeScalarVectorInst(e.mnemonic, 6u, srcs, imm, words);
        if (!enc_ok) {
            std::fprintf(stderr, "[FAIL] Encode failed for %s\n", e.mnemonic);
        }
        OS_REQUIRE(enc_ok);

        SvDecodedInst dec{};
        const Status dst = DecodeScalarVectorAt(words.data(), words.size(), 0u, dec);
        if (!dst.ok() || !dec.entry || std::strcmp(dec.entry->mnemonic, e.mnemonic) != 0 ||
            dec.base.encoding != e.encoding || dec.base.word_count != words.size()) {
            std::fprintf(stderr, "[FAIL] Round-trip mismatch for %s: got=%s enc=%u wc=%u/%zu\n",
                         e.mnemonic, dec.entry ? dec.entry->mnemonic : "<null>",
                         static_cast<uint32_t>(dec.base.encoding), dec.base.word_count, words.size());
            OS_REQUIRE(false);
        }
    }

    std::printf("[PASS] Round-trip encode->decode: %zu/%zu entries (100%%)!\n", table.size(), table.size());
    std::printf("       Breakdown: SOP2=%zu SOP1=%zu SOPK=%zu SOPC=%zu SOPP=%zu SMEM=%zu VOP1=%zu VOP2=%zu VOPC=%zu VOP3=%zu VOP3P=%zu VINTRP=%zu\n",
                per_enc[static_cast<uint8_t>(Rdna2Encoding::SOP2)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::SOP1)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::SOPK)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::SOPC)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::SOPP)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::SMEM)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::VOP1)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::VOP2)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::VOPC)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::VOP3)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::VOP3P)],
                per_enc[static_cast<uint8_t>(Rdna2Encoding::VINTRP)]);
}

// ---------------------------------------------------------------------------
// Test 2: SDWA, DPP16, DPP8, DPP8FI Modifier Round-Trip Verification
// ---------------------------------------------------------------------------
void TestModifierEncodeDecodeRoundTrip() {
    // SDWA
    {
        DecodedSvModifier m = DecodedSvModifier::MakeSdwa(3u, kSdwaSelByte1, kSdwaSelWord0, kSdwaSelWord1, kSdwaUnusedPreserve);
        m.src0_sext = true;
        m.src1_neg = true;
        std::vector<uint32_t> words;
        OS_REQUIRE(EncodeScalarVectorInst("v_add_nc_u32", 5u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u)}, 0, words, &m));
        OS_REQUIRE(words.size() == 2u);
        SvDecodedInst dec{};
        OS_REQUIRE(DecodeScalarVectorAt(words.data(), words.size(), 0u, dec).ok());
        OS_REQUIRE(dec.modifier.kind == SvModifierKind::Sdwa);
        OS_REQUIRE(dec.modifier.vsrc0 == 3u);
        OS_REQUIRE(dec.modifier.src0_sel == kSdwaSelByte1);
        OS_REQUIRE(dec.modifier.src0_sext);
        OS_REQUIRE(dec.modifier.src1_sel == kSdwaSelWord0);
        OS_REQUIRE(dec.modifier.src1_neg);
        OS_REQUIRE(dec.modifier.dst_sel == kSdwaSelWord1);
        OS_REQUIRE(dec.modifier.dst_unused == kSdwaUnusedPreserve);
    }
    // DPP16 (row_shl, row_shr, quad_perm, row_bcast15)
    for (uint16_t ctrl : {kDppRowShl1, kDppRowShr1, MakeDppQuadPerm(3u, 2u, 1u, 0u), kDppRowBcast15}) {
        DecodedSvModifier m = DecodedSvModifier::MakeDpp16(2u, ctrl, 0xFu, 0xFu, true, false);
        std::vector<uint32_t> words;
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 7u, {Rdna2Operand::Vgpr(2u)}, 0, words, &m));
        OS_REQUIRE(words.size() == 2u);
        SvDecodedInst dec{};
        OS_REQUIRE(DecodeScalarVectorAt(words.data(), words.size(), 0u, dec).ok());
        OS_REQUIRE(dec.modifier.kind == SvModifierKind::Dpp16);
        OS_REQUIRE(dec.modifier.vsrc0 == 2u);
        OS_REQUIRE(dec.modifier.dpp_ctrl == ctrl);
        OS_REQUIRE(dec.modifier.bound_ctrl);
    }
    // DPP8 & DPP8FI
    for (bool fi : {false, true}) {
        std::array<uint8_t, 8> sel = {7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u};
        DecodedSvModifier m = DecodedSvModifier::MakeDpp8(4u, sel, fi);
        std::vector<uint32_t> words;
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 9u, {Rdna2Operand::Vgpr(4u)}, 0, words, &m));
        OS_REQUIRE(words.size() == 2u);
        SvDecodedInst dec{};
        OS_REQUIRE(DecodeScalarVectorAt(words.data(), words.size(), 0u, dec).ok());
        OS_REQUIRE(dec.modifier.kind == (fi ? SvModifierKind::Dpp8Fi : SvModifierKind::Dpp8));
        OS_REQUIRE(dec.modifier.vsrc0 == 4u);
        OS_REQUIRE(dec.modifier.dpp8_sel == sel);
    }
    std::printf("[PASS] SDWA / DPP16 (row_shl, row_shr, quad_perm, row_bcast15) / DPP8 / DPP8FI encode->decode round-trip!\n");
}

// ---------------------------------------------------------------------------
// Test 3: Live GPU Parity Shaders (D3D12 HW + D3D12 WARP + Vulkan HW vs CPU)
// ---------------------------------------------------------------------------
void TestScalarOpsShader(LiveAdapters& adapters) {
    // 16 outputs written by lane 0 (and replicated/combined with lane id so all 32 lanes produce verifiable outputs)
    std::vector<uint32_t> w;
    // Compute byte offset v1 = v0 << 2
    Emit(w, "v_lshlrev_b32", 1u, {Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(0u)});
    // Load input[tid] into v2
    AppendMubuf(w, Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u);

    // Scalar bit & arithmetic torture sequence into s0..s15
    Emit(w, "s_mov_b32", 0u, {Rdna2Operand::LiteralU32(0xA5C30F12u)});
    Emit(w, "s_mov_b32", 1u, {Rdna2Operand::LiteralU32(0x00080004u)});  // offset=4, width=8
    Emit(w, "s_bfe_u32", 2u, {Rdna2Operand::Sgpr(0u), Rdna2Operand::Sgpr(1u)});
    Emit(w, "s_bfe_i32", 3u, {Rdna2Operand::Sgpr(0u), Rdna2Operand::Sgpr(1u)});
    Emit(w, "s_bfm_b32", 4u, {Rdna2Operand::InlineInt(12), Rdna2Operand::InlineInt(5)});
    Emit(w, "s_bcnt1_i32_b32", 5u, {Rdna2Operand::Sgpr(0u)});
    Emit(w, "s_bcnt0_i32_b32", 6u, {Rdna2Operand::Sgpr(0u)});
    Emit(w, "s_ff1_i32_b32", 7u, {Rdna2Operand::Sgpr(0u)});
    Emit(w, "s_ff0_i32_b32", 8u, {Rdna2Operand::Sgpr(0u)});
    Emit(w, "s_flbit_i32_b32", 9u, {Rdna2Operand::LiteralU32(0x00180000u)});
    Emit(w, "s_flbit_i32", 10u, {Rdna2Operand::LiteralU32(0xFFFF0000u)});
    Emit(w, "s_bitreplicate_b64_b32", 12u, {Rdna2Operand::LiteralU32(0x00030005u)});  // writes s12, s13
    Emit(w, "s_abs_i32", 14u, {Rdna2Operand::LiteralU32(static_cast<uint32_t>(-12345))});
    Emit(w, "s_mul_hi_u32", 15u, {Rdna2Operand::LiteralU32(0x80000001u), Rdna2Operand::LiteralU32(0x00000004u)});
    Emit(w, "s_mul_hi_i32", 16u, {Rdna2Operand::LiteralU32(static_cast<uint32_t>(-1000000)), Rdna2Operand::LiteralU32(1000000u)});
    Emit(w, "s_movk_i32", 17u, {}, 25);
    Emit(w, "s_addk_i32", 17u, {}, 17);       // 42
    Emit(w, "s_mulk_i32", 17u, {}, 3);        // 126
    Emit(w, "s_cmpk_gt_i32", 17u, {}, 100);   // scc = true
    Emit(w, "s_cmovk_i32", 18u, {}, 777);     // 777
    Emit(w, "s_cselect_b32", 19u, {Rdna2Operand::Sgpr(18u), Rdna2Operand::Sgpr(17u)});
    Emit(w, "s_brev_b32", 20u, {Rdna2Operand::Sgpr(0u)});
    Emit(w, "s_pack_ll_b32_b16", 21u, {Rdna2Operand::Sgpr(17u), Rdna2Operand::Sgpr(18u)});
    Emit(w, "s_lshl2_add_u32", 22u, {Rdna2Operand::Sgpr(17u), Rdna2Operand::Sgpr(14u)});

    // Combine all scalar results with per-lane input v2 via XOR/ADD chain so every single scalar op affects output!
    Emit(w, "v_add_nc_u32", 3u, {Rdna2Operand::Sgpr(2u), Rdna2Operand::Vgpr(2u)});
    for (uint32_t r : {3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u}) {
        Emit(w, "v_xor_b32", 3u, {Rdna2Operand::Sgpr(static_cast<uint8_t>(r)), Rdna2Operand::Vgpr(3u)});
        Emit(w, "v_add_nc_u32", 3u, {Rdna2Operand::Sgpr(static_cast<uint8_t>(r)), Rdna2Operand::Vgpr(3u)});
    }
    AppendMubuf(w, Rdna2Opcode::BUFFER_STORE_DWORD, 3u, 1u, 4u, 0u);
    Emit(w, "s_endpgm", 0u, {}, 0);

    std::vector<uint32_t> in(32u);
    for (uint32_t i = 0u; i < 32u; ++i) in[i] = 0x1000u + i * 37u;
    VerifyShaderLiveParity("Shader1_ScalarBitAndArith", w, in, 32u, 32u, adapters, false);
}

void TestVectorTranscendentalsShader(LiveAdapters& adapters) {
    std::vector<uint32_t> w;
    Emit(w, "v_lshlrev_b32", 1u, {Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(0u)});
    AppendMubuf(w, Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u);  // v2 = positive float input x

    Emit(w, "v_rcp_f32", 3u, {Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_rsq_f32", 4u, {Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_sqrt_f32", 5u, {Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_log_f32", 6u, {Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_exp_f32", 7u, {Rdna2Operand::Vgpr(6u)});               // exp2(log2(x)) ~= x
    Emit(w, "v_sin_f32", 8u, {Rdna2Operand::Vgpr(3u)});               // sin(2pi/x)
    Emit(w, "v_cos_f32", 9u, {Rdna2Operand::Vgpr(3u)});               // cos(2pi/x)
    Emit(w, "v_frexp_mant_f32", 10u, {Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_frexp_exp_i32_f32", 11u, {Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_ldexp_f32", 12u, {Rdna2Operand::Vgpr(10u), Rdna2Operand::Vgpr(11u)});  // reconstructs x
    Emit(w, "v_div_fixup_f32", 13u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(2u), Rdna2Operand::InlineFloat(1.0f)});
    Emit(w, "v_floor_f32", 14u, {Rdna2Operand::Vgpr(5u)});
    Emit(w, "v_fract_f32", 15u, {Rdna2Operand::Vgpr(5u)});

    // Sum all transcendental results into v16
    Emit(w, "v_add_f32", 16u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u)});
    for (uint32_t r : {5u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u, 15u}) {
        Emit(w, "v_add_f32", 16u, {Rdna2Operand::Vgpr(static_cast<uint8_t>(r)), Rdna2Operand::Vgpr(16u)});
    }
    AppendMubuf(w, Rdna2Opcode::BUFFER_STORE_DWORD, 16u, 1u, 4u, 0u);
    Emit(w, "s_endpgm", 0u, {}, 0);

    std::vector<uint32_t> in(32u);
    for (uint32_t i = 0u; i < 32u; ++i) in[i] = FloatBits(1.25f + static_cast<float>(i) * 0.375f);
    VerifyShaderLiveParity("Shader2_VectorTranscendentals", w, in, 32u, 32u, adapters, true, 1e-4f);
}

void TestThreeSourceVop3Shader(LiveAdapters& adapters) {
    std::vector<uint32_t> w;
    Emit(w, "v_lshlrev_b32", 1u, {Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(0u)});
    AppendMubuf(w, Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u);
    Emit(w, "v_add_nc_u32", 3u, {Rdna2Operand::LiteralU32(0x13579BDFu), Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_xor_b32", 4u, {Rdna2Operand::LiteralU32(0x2468ACE0u), Rdna2Operand::Vgpr(2u)});

    Emit(w, "v_lerp_u8", 5u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u)});
    Emit(w, "v_sad_u8", 6u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(5u)});
    Emit(w, "v_sad_hi_u8", 7u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(6u)});
    Emit(w, "v_sad_u16", 8u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(7u)});
    Emit(w, "v_sad_u32", 9u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(8u)});
    Emit(w, "v_msad_u8", 10u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(9u)});
    Emit(w, "v_med3_i32", 11u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u)});
    Emit(w, "v_min3_u32", 12u, {Rdna2Operand::Vgpr(5u), Rdna2Operand::Vgpr(6u), Rdna2Operand::Vgpr(7u)});
    Emit(w, "v_max3_u32", 13u, {Rdna2Operand::Vgpr(8u), Rdna2Operand::Vgpr(9u), Rdna2Operand::Vgpr(10u)});
    Emit(w, "v_alignbit_b32", 14u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::InlineInt(7)});
    Emit(w, "v_alignbyte_b32", 15u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u), Rdna2Operand::InlineInt(2)});
    Emit(w, "v_bfe_u32", 16u, {Rdna2Operand::Vgpr(14u), Rdna2Operand::InlineInt(5), Rdna2Operand::InlineInt(11)});
    Emit(w, "v_bfe_i32", 17u, {Rdna2Operand::Vgpr(15u), Rdna2Operand::InlineInt(3), Rdna2Operand::InlineInt(13)});
    Emit(w, "v_bfi_b32", 18u, {Rdna2Operand::Vgpr(16u), Rdna2Operand::Vgpr(17u), Rdna2Operand::Vgpr(11u)});
    Emit(w, "v_perm_b32", 19u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::LiteralU32(0x06040200u)});
    Emit(w, "v_mbcnt_lo_u32_b32", 20u, {Rdna2Operand::LiteralU32(0xAAAAAAAAu), Rdna2Operand::Vgpr(19u)});
    Emit(w, "v_add3_u32", 21u, {Rdna2Operand::Vgpr(12u), Rdna2Operand::Vgpr(13u), Rdna2Operand::Vgpr(18u)});
    Emit(w, "v_lshl_add_u32", 22u, {Rdna2Operand::Vgpr(20u), Rdna2Operand::InlineInt(3), Rdna2Operand::Vgpr(21u)});
    Emit(w, "v_add_lshl_u32", 23u, {Rdna2Operand::Vgpr(21u), Rdna2Operand::Vgpr(22u), Rdna2Operand::InlineInt(1)});
    Emit(w, "v_lshl_or_b32", 24u, {Rdna2Operand::Vgpr(16u), Rdna2Operand::InlineInt(4), Rdna2Operand::Vgpr(23u)});
    Emit(w, "v_and_or_b32", 25u, {Rdna2Operand::Vgpr(24u), Rdna2Operand::Vgpr(19u), Rdna2Operand::Vgpr(17u)});
    Emit(w, "v_or3_b32", 26u, {Rdna2Operand::Vgpr(25u), Rdna2Operand::Vgpr(14u), Rdna2Operand::Vgpr(15u)});
    Emit(w, "v_xad_u32", 27u, {Rdna2Operand::Vgpr(26u), Rdna2Operand::Vgpr(20u), Rdna2Operand::Vgpr(10u)});

    AppendMubuf(w, Rdna2Opcode::BUFFER_STORE_DWORD, 27u, 1u, 4u, 0u);
    Emit(w, "s_endpgm", 0u, {}, 0);

    std::vector<uint32_t> in(32u);
    for (uint32_t i = 0u; i < 32u; ++i) in[i] = 0x01020304u * (i + 1u) + 0x00112233u;
    VerifyShaderLiveParity("Shader3_Vop3ThreeSourceOps", w, in, 32u, 32u, adapters, false);
}

void TestPackedVop3pAndDotShader(LiveAdapters& adapters) {
    std::vector<uint32_t> w;
    Emit(w, "v_lshlrev_b32", 1u, {Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(0u)});
    AppendMubuf(w, Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u);
    Emit(w, "v_mov_b32", 3u, {Rdna2Operand::LiteralU32(0x0003FFFEu)});  // (+3, -2) as i16x2
    Emit(w, "v_mov_b32", 4u, {Rdna2Operand::LiteralU32(0x00050004u)});  // (+5, +4) as i16x2

    Emit(w, "v_pk_add_u16", 5u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u)});
    Emit(w, "v_pk_sub_u16", 6u, {Rdna2Operand::Vgpr(5u), Rdna2Operand::Vgpr(3u)});
    Emit(w, "v_pk_mul_lo_u16", 7u, {Rdna2Operand::Vgpr(6u), Rdna2Operand::Vgpr(4u)});
    Emit(w, "v_pk_mad_i16", 8u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_pk_lshlrev_b16", 9u, {Rdna2Operand::LiteralU32(0x00020001u), Rdna2Operand::Vgpr(7u)});
    Emit(w, "v_pk_lshrrev_b16", 10u, {Rdna2Operand::LiteralU32(0x00010002u), Rdna2Operand::Vgpr(8u)});
    Emit(w, "v_pk_max_i16", 11u, {Rdna2Operand::Vgpr(9u), Rdna2Operand::Vgpr(10u)});
    Emit(w, "v_pk_min_u16", 12u, {Rdna2Operand::Vgpr(11u), Rdna2Operand::Vgpr(5u)});

    // Packed f16 arithmetic: 0x3C00 = 1.0h, 0x4000 = 2.0h, 0x4200 = 3.0h
    Emit(w, "v_mov_b32", 13u, {Rdna2Operand::LiteralU32(0x40003C00u)});  // (1.0h, 2.0h)
    Emit(w, "v_mov_b32", 14u, {Rdna2Operand::LiteralU32(0x42004000u)});  // (2.0h, 3.0h)
    Emit(w, "v_pk_add_f16", 15u, {Rdna2Operand::Vgpr(13u), Rdna2Operand::Vgpr(14u)});
    Emit(w, "v_pk_mul_f16", 16u, {Rdna2Operand::Vgpr(15u), Rdna2Operand::Vgpr(13u)});
    Emit(w, "v_pk_fma_f16", 17u, {Rdna2Operand::Vgpr(13u), Rdna2Operand::Vgpr(14u), Rdna2Operand::Vgpr(16u)});
    Emit(w, "v_dot2_f32_f16", 18u, {Rdna2Operand::Vgpr(13u), Rdna2Operand::Vgpr(14u), Rdna2Operand::InlineFloat(1.5f)});
    Emit(w, "v_cvt_u32_f32", 19u, {Rdna2Operand::Vgpr(18u)});

    // Integer dot2/dot4/dot8 products
    Emit(w, "v_dot2_i32_i16", 20u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(12u)});
    Emit(w, "v_dot2_u32_u16", 21u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(20u)});
    Emit(w, "v_dot4_i32_i8", 22u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(21u)});
    Emit(w, "v_dot4_u32_u8", 23u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(22u)});
    Emit(w, "v_dot8_i32_i4", 24u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(23u)});
    Emit(w, "v_dot8_u32_u4", 25u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u), Rdna2Operand::Vgpr(24u)});
    Emit(w, "v_add3_u32", 26u, {Rdna2Operand::Vgpr(25u), Rdna2Operand::Vgpr(17u), Rdna2Operand::Vgpr(19u)});

    AppendMubuf(w, Rdna2Opcode::BUFFER_STORE_DWORD, 26u, 1u, 4u, 0u);
    Emit(w, "s_endpgm", 0u, {}, 0);

    std::vector<uint32_t> in(32u);
    for (uint32_t i = 0u; i < 32u; ++i) in[i] = 0x00100020u + i * 0x00010003u;
    VerifyShaderLiveParity("Shader4_Vop3pPackedAndDotOps", w, in, 32u, 32u, adapters, false);
}

void TestCrossLaneSdwaDppShader(LiveAdapters& adapters) {
    std::vector<uint32_t> w;
    Emit(w, "v_lshlrev_b32", 1u, {Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(0u)});
    AppendMubuf(w, Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u);

    // 1. SDWA: extract byte1(sext) of v2 + byte0 of v2 -> write into word0 preserving word1 of v3
    Emit(w, "v_mov_b32", 3u, {Rdna2Operand::LiteralU32(0xABCD0000u)});
    {
        DecodedSvModifier sdwa = DecodedSvModifier::MakeSdwa(2u, kSdwaSelByte1, kSdwaSelByte0, kSdwaSelWord0, kSdwaUnusedPreserve);
        sdwa.src0_sext = true;
        Emit(w, "v_add_nc_u32", 3u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(2u)}, 0, &sdwa);
    }

    // 2. DPP16: row_shl:1, row_shr:1, quad_perm:[3,2,1,0], row_bcast15
    {
        DecodedSvModifier d_shl = DecodedSvModifier::MakeDpp16(2u, kDppRowShl1, 0xFu, 0xFu, true);
        Emit(w, "v_mov_b32", 4u, {Rdna2Operand::Vgpr(2u)}, 0, &d_shl);

        DecodedSvModifier d_shr = DecodedSvModifier::MakeDpp16(2u, kDppRowShr1, 0xFu, 0xFu, true);
        Emit(w, "v_add_nc_u32", 5u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(4u)}, 0, &d_shr);

        DecodedSvModifier d_qp = DecodedSvModifier::MakeDpp16(2u, MakeDppQuadPerm(3u, 2u, 1u, 0u), 0xFu, 0xFu, true);
        Emit(w, "v_xor_b32", 6u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(5u)}, 0, &d_qp);

        DecodedSvModifier d_bc = DecodedSvModifier::MakeDpp16(2u, kDppRowBcast15, 0xFu, 0xFu, true);
        Emit(w, "v_add_nc_u32", 7u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(6u)}, 0, &d_bc);
    }

    // 3. DPP8: reverse 8-lane group [7,6,5,4,3,2,1,0]
    {
        DecodedSvModifier d8 = DecodedSvModifier::MakeDpp8(2u, {7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u}, false);
        Emit(w, "v_add_nc_u32", 8u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::Vgpr(7u)}, 0, &d8);
    }

    // 4. Wave cross-lane ops: v_readfirstlane, v_readlane, v_writelane, v_permlane16, v_permlanex16
    Emit(w, "v_readfirstlane_b32", 0u, {Rdna2Operand::Vgpr(2u)});
    Emit(w, "v_readlane_b32", 1u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::InlineInt(19)});
    Emit(w, "v_mov_b32", 9u, {Rdna2Operand::Vgpr(8u)});
    Emit(w, "v_writelane_b32", 9u, {Rdna2Operand::Sgpr(1u), Rdna2Operand::InlineInt(5)});
    Emit(w, "v_permlane16_b32", 10u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::LiteralU32(0x76543210u), Rdna2Operand::LiteralU32(0xFEDCBA98u)});
    Emit(w, "v_permlanex16_b32", 11u, {Rdna2Operand::Vgpr(2u), Rdna2Operand::LiteralU32(0x01234567u), Rdna2Operand::LiteralU32(0x89ABCDEFu)});

    // Combine all cross-lane + SDWA + DPP results into v12
    Emit(w, "v_add3_u32", 12u, {Rdna2Operand::Vgpr(3u), Rdna2Operand::Vgpr(9u), Rdna2Operand::Vgpr(10u)});
    Emit(w, "v_add3_u32", 12u, {Rdna2Operand::Vgpr(12u), Rdna2Operand::Vgpr(11u), Rdna2Operand::Sgpr(0u)});
    AppendMubuf(w, Rdna2Opcode::BUFFER_STORE_DWORD, 12u, 1u, 4u, 0u);
    Emit(w, "s_endpgm", 0u, {}, 0);

    std::vector<uint32_t> in(32u);
    for (uint32_t i = 0u; i < 32u; ++i) in[i] = 0x00810005u + i * 0x00030107u;
    VerifyShaderLiveParity("Shader5_CrossLaneSdwaDpp16Dpp8", w, in, 32u, 32u, adapters, false);
}

void TestMultiStageLiftValidation() {
    std::vector<uint32_t> w;
    Emit(w, "v_cvt_f32_u32", 1u, {Rdna2Operand::Vgpr(0u)});
    Emit(w, "v_fma_f32", 2u, {Rdna2Operand::Vgpr(1u), Rdna2Operand::InlineFloat(0.5f), Rdna2Operand::InlineFloat(1.0f)});
    Emit(w, "s_endpgm", 0u, {}, 0);

    std::vector<SvDecodedInst> insts;
    OS_REQUIRE(DecodeScalarVectorStream(w, insts).ok());

    for (ShaderStage stg : {ShaderStage::Vertex, ShaderStage::Pixel, ShaderStage::Mesh, ShaderStage::Amplification}) {
        Xs2ShaderContainer c{};
        c.stage = stg;
        c.threadgroup_x = 32u;
        c.code_words = w;

        Rdna2LiftOptions opt{};
        opt.use_sm65 = true;
        opt.emit_spirv = (stg == ShaderStage::Vertex || stg == ShaderStage::Pixel);
        opt.validate_spirv_external = opt.emit_spirv && HasSpirvVal();
        opt.emit_hlsl = true;
        opt.compile_dxil_with_dxc = true;
        opt.resign_dxil = true;

        Rdna2LiftResult res{};
        Status st = LiftScalarVectorShader(c, insts, res, opt);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] MultiStage stage=%u: %s\nDXC: %s\nSPV: %s\n",
                         static_cast<uint32_t>(stg), st.message.c_str(),
                         res.dxc_diagnostics.c_str(), res.spirv_val_diagnostics.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(!res.pc_dxil.empty());
        OS_REQUIRE(VerifyDxbcContainerHash(res.pc_dxil.data(), res.pc_dxil.size()));
    }
    std::printf("[PASS] Multi-stage SM 6.5 DXIL + SPIR-V 1.3 lifting validated (VS/PS/MS/AS)!\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("=== OpenScarlett Sprint 8 Worker 1: RDNA2 Scalar & Vector ISA Suite ===\n");

    TestOpcodeTableRoundTrip();
    TestModifierEncodeDecodeRoundTrip();

    LiveAdapters adapters{};
#if defined(_WIN32)
    adapters.has_d3d12_hw = adapters.d3d12_hw.Init(false);
    adapters.has_d3d12_warp = adapters.d3d12_warp.Init(true);
    std::printf("[INFO] D3D12 HW   : %s\n", adapters.has_d3d12_hw ? adapters.d3d12_hw.adapter_name.c_str() : "unavailable");
    std::printf("[INFO] D3D12 WARP : %s\n", adapters.has_d3d12_warp ? adapters.d3d12_warp.adapter_name.c_str() : "unavailable");
    OS_REQUIRE(adapters.has_d3d12_warp);
#endif
    if (adapters.vk.Initialize(false).ok() && !adapters.vk.is_fallback_mode()) {
        adapters.has_vk_hw = true;
        const auto& info = adapters.vk.GetDeviceInfo();
        std::printf("[INFO] Vulkan HW  : %s (Vulkan %u.%u.%u)\n",
                    info.device_name.c_str(), info.api_major, info.api_minor, info.api_patch);
    } else {
        std::printf("[INFO] Vulkan HW  : no hardware ICD detected (guarded cleanly for CI)\n");
    }

    TestScalarOpsShader(adapters);
    TestVectorTranscendentalsShader(adapters);
    TestThreeSourceVop3Shader(adapters);
    TestPackedVop3pAndDotShader(adapters);
    TestCrossLaneSdwaDppShader(adapters);
    TestMultiStageLiftValidation();

    std::printf("=== ALL SCALAR & VECTOR ISA TESTS PASSED ===\n");
    return 0;
}
