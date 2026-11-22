#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

using namespace openscarlett;
#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#endif

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

uint16_t FloatToHalfBitsRef(float f) noexcept {
    const uint32_t bits = FloatToBits(f);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t mant = bits & 0x007FFFFFu;
    if (exp == 255u) {
        if (mant != 0u) return static_cast<uint16_t>(sign | 0x7E00u | (mant >> 13));
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    const int32_t new_exp = static_cast<int32_t>(exp) - 112;
    if (new_exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (new_exp <= 0) {
        if (new_exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = (mant | 0x00800000u) >> static_cast<uint32_t>(1 - new_exp);
        const uint32_t rounded = (m + 0x00001000u + ((m >> 13) & 1u) - 1u) >> 13;
        return static_cast<uint16_t>(sign | rounded);
    }
    const uint32_t half_mant = mant + 0x00000FFFu + ((mant >> 13) & 1u);
    if ((half_mant & 0x00800000u) != 0u) {
        if (new_exp + 1 >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp + 1) << 10));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp) << 10) | (half_mant >> 13));
}

float RoundNearestEvenRef(float x) noexcept {
    const float fl = std::floor(x);
    const float diff = x - fl;
    if (diff < 0.5f) return fl;
    if (diff > 0.5f) return fl + 1.0f;
    const float h = fl * 0.5f;
    return (std::floor(h) == h) ? fl : (fl + 1.0f);
}

uint16_t RefSnorm16(float x) noexcept {
    if (std::isnan(x)) return 0u;
    const float c = (x >= 1.0f) ? 1.0f : ((x <= -1.0f) ? -1.0f : x);
    const int32_t v = std::clamp(static_cast<int32_t>(RoundNearestEvenRef(c * 32767.0f)), -32767, 32767);
    return static_cast<uint16_t>(static_cast<int16_t>(v));
}

uint16_t RefUnorm16(float x) noexcept {
    if (std::isnan(x)) return 0u;
    const float c = (x >= 1.0f) ? 1.0f : ((x <= 0.0f) ? 0.0f : x);
    const int32_t v = std::clamp(static_cast<int32_t>(RoundNearestEvenRef(c * 65535.0f)), 0, 65535);
    return static_cast<uint16_t>(v);
}

uint32_t RefSBfeU32(uint32_t s0, uint32_t s1) noexcept {
    const uint32_t off = s1 & 31u;
    const uint32_t wid = (s1 >> 16) & 0x7Fu;
    if (wid == 0u) return 0u;
    if (off + wid < 32u) return (s0 >> off) & ((1u << wid) - 1u);
    return s0 >> off;
}

uint32_t RefSBfeI32(uint32_t s0, uint32_t s1) noexcept {
    const uint32_t off = s1 & 31u;
    const uint32_t wid = (s1 >> 16) & 0x7Fu;
    if (wid == 0u) return 0u;
    if (off + wid < 32u) {
        return static_cast<uint32_t>(static_cast<int32_t>(s0 << (32u - off - wid)) >> (32u - wid));
    }
    return static_cast<uint32_t>(static_cast<int32_t>(s0) >> off);
}

size_t CountSubstring(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Counts SPIR-V OpControlBarrier (opcode 224) instructions with Workgroup(2), Workgroup(2), AcqRel|Workgroup(0x108).
uint32_t CountSpirvWorkgroupBarriers(const std::vector<uint32_t>& spv) {
    std::unordered_map<uint32_t, uint32_t> const_u32;
    uint32_t count = 0;
    size_t pos = 5;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0 || pos + wc > spv.size()) break;
        if (op == 43u && wc == 4u) {  // OpConstant
            const_u32[spv[pos + 2]] = spv[pos + 3];
        } else if (op == 224u && wc == 4u) {  // OpControlBarrier
            const uint32_t exec = const_u32[spv[pos + 1]];
            const uint32_t mem = const_u32[spv[pos + 2]];
            const uint32_t sem = const_u32[spv[pos + 3]];
            if (exec == 2u && mem == 2u && sem == 0x108u) {
                ++count;
            }
        }
        pos += wc;
    }
    return count;
}

bool HasExternalSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(kDefaultSpirvValPath, ec);
}

void VerifySpirvStrict(const std::vector<uint32_t>& spv, const char* label,
                       uint32_t expect_lx = 0, uint32_t expect_ly = 0, uint32_t expect_lz = 0) {
    SpirvValidationInfo info{};
    auto st_vk = VulkanBackend::ValidateSpirvBinary(spv, &info);
    if (!st_vk.ok()) {
        std::fprintf(stderr, "[FAIL] VulkanBackend::ValidateSpirvBinary (%s): %s\n", label, st_vk.message.c_str());
    }
    OS_REQUIRE(st_vk.ok());
    OS_REQUIRE(ValidateSpirvBinary(spv).ok());
    if (expect_lx != 0u) {
        OS_REQUIRE(info.local_size_x == expect_lx);
        OS_REQUIRE(info.local_size_y == expect_ly);
        OS_REQUIRE(info.local_size_z == expect_lz);
    }
    if (HasExternalSpirvVal()) {
        std::string diag;
        auto st11 = ValidateSpirvExternal(spv, diag, kDefaultSpirvValPath, "vulkan1.1");
        if (!st11.ok()) {
            std::fprintf(stderr, "[FAIL] spirv-val vulkan1.1 (%s): %s\n", label, diag.c_str());
        }
        OS_REQUIRE(st11.ok());
        auto st12 = ValidateSpirvExternal(spv, diag, kDefaultSpirvValPath, "vulkan1.2");
        if (!st12.ok()) {
            std::fprintf(stderr, "[FAIL] spirv-val vulkan1.2 (%s): %s\n", label, diag.c_str());
        }
        OS_REQUIRE(st12.ok());
    }
}

void RunOnVulkanSpirv(const std::vector<uint32_t>& spv,
                      const std::vector<uint32_t>& input_u32,
                      const std::vector<uint32_t>& expected_u32,
                      uint32_t group_x,
                      const char* label) {
    VulkanBackend vk;
    if (!vk.Initialize(/*allow_software_fallback=*/false).ok() || vk.is_fallback_mode()) {
        return;  // Hardware Vulkan ICD unavailable on this host (headless CI / software fallback)
    }
    std::vector<uint32_t> out_u32;
    auto st = vk.ExecuteComputeShader(spv, input_u32, out_u32, group_x);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Vulkan ExecuteComputeShader (%s): %s\n", label, st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(out_u32.size() == expected_u32.size());
    for (size_t i = 0; i < expected_u32.size(); ++i) {
        if (out_u32[i] != expected_u32[i]) {
            std::fprintf(stderr, "[FAIL] Vulkan mismatch (%s) idx=%zu got=0x%08x exp=0x%08x\n",
                         label, i, out_u32[i], expected_u32[i]);
            OS_REQUIRE(false);
        }
    }
    vk.Shutdown();
}

#if defined(_WIN32)
bool EnvPreferWarp() {
    char buf[32]{};
    const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_PREFER_WARP", buf, static_cast<DWORD>(sizeof(buf)));
    return len > 0 && len < sizeof(buf) && (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
}

struct D3D12DiffContext {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device2> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0;
    std::string adapter_name;
    bool is_warp = false;
    uint32_t highest_sm = 0;

    ~D3D12DiffContext() {
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
            if (FAILED(factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
                if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
            }
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        is_warp = use_warp || ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) ||
                  (desc.VendorId == 0x1414u && (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0));
        char name_buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
        adapter_name = name_buf;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            return false;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&cmd_list)))) {
            return false;
        }
        cmd_list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    void WaitGpu() {
        ++fence_val;
        OS_REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), fence_val)));
        if (fence->GetCompletedValue() < fence_val) {
            OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(fence_val, fence_event)));
            WaitForSingleObject(fence_event, 15000);
        }
    }

    ComPtr<ID3D12Resource> CreateBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap_type,
                                        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init_state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> res;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
                                                             IID_PPV_ARGS(&res))));
        return res;
    }

    void RunComputeDualUav(const std::vector<uint8_t>& dxil,
                           const std::vector<uint32_t>& in_data,
                           std::vector<uint32_t>& out_data,
                           uint32_t group_x) {
        const uint64_t in_bytes = static_cast<uint64_t>(in_data.size()) * sizeof(uint32_t);
        const uint64_t out_bytes = static_cast<uint64_t>(out_data.size()) * sizeof(uint32_t);

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor = {0, 0};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {1, 0};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig_blob, err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_sig))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] CreateComputePipelineState on %s hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        auto in_up = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_up = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        void* p = nullptr;
        OS_REQUIRE(SUCCEEDED(in_up->Map(0, nullptr, &p)));
        std::memcpy(p, in_data.data(), static_cast<size_t>(in_bytes));
        in_up->Unmap(0, nullptr);
        OS_REQUIRE(SUCCEEDED(out_up->Map(0, nullptr, &p)));
        std::memset(p, 0, static_cast<size_t>(out_bytes));
        out_up->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));
        cmd_list->CopyResource(in_gpu.Get(), in_up.Get());
        cmd_list->CopyResource(out_gpu.Get(), out_up.Get());

        D3D12_RESOURCE_BARRIER to_uav[2]{};
        to_uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[0].Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        to_uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[1].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(2, to_uav);

        cmd_list->SetComputeRootSignature(root_sig.Get());
        cmd_list->SetComputeRootUnorderedAccessView(0, in_gpu->GetGPUVirtualAddress());
        cmd_list->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
        cmd_list->Dispatch(group_x, 1, 1);

        D3D12_RESOURCE_BARRIER uav_b{};
        uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_b.UAV.pResource = out_gpu.Get();
        cmd_list->ResourceBarrier(1, &uav_b);

        D3D12_RESOURCE_BARRIER to_src{};
        to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_src.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_src);
        cmd_list->CopyResource(rb.Get(), out_gpu.Get());

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &p)));
        std::memcpy(out_data.data(), p, static_cast<size_t>(out_bytes));
        rb->Unmap(0, nullptr);
    }

    // Runs compute shader with t0 (2D Texture SRV with multiple mips & arbitrary DXGI format),
    // u0 (input coords buffer), and u1 (output buffer).
    void RunComputeImageLoad2D(const std::vector<uint8_t>& dxil,
                               DXGI_FORMAT fmt,
                               UINT width,
                               UINT height,
                               const std::vector<std::vector<uint8_t>>& mip_payloads,
                               UINT bytes_per_pixel,
                               const std::vector<uint32_t>& in_uav0,
                               std::vector<uint32_t>& out_uav1,
                               uint32_t group_x) {
        const UINT mip_count = static_cast<UINT>(mip_payloads.size());
        OS_REQUIRE(mip_count >= 1u);

        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor = {0, 0};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {1, 0};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &srv_range};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig_blob, err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_sig))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] ImageLoad PSO failed on %s hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width;
        td.Height = height;
        td.DepthOrArraySize = 1;
        td.MipLevels = static_cast<UINT16>(mip_count);
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&tex))));

        // Upload each mip via 256-byte row-pitch aligned staging buffers
        std::vector<ComPtr<ID3D12Resource>> mip_uploads(mip_count);
        std::vector<UINT> mip_w(mip_count), mip_h(mip_count), mip_pitch(mip_count);
        for (UINT m = 0; m < mip_count; ++m) {
            const UINT mw = std::max(1u, width >> m);
            const UINT mh = std::max(1u, height >> m);
            const UINT row_bytes = mw * bytes_per_pixel;
            const UINT pitch = (row_bytes + 255u) & ~255u;
            mip_w[m] = mw;
            mip_h[m] = mh;
            mip_pitch[m] = pitch;
            mip_uploads[m] = CreateBuffer(static_cast<uint64_t>(pitch) * mh, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p = nullptr;
            OS_REQUIRE(SUCCEEDED(mip_uploads[m]->Map(0, nullptr, &p)));
            auto* dst = static_cast<uint8_t*>(p);
            std::memset(dst, 0, static_cast<size_t>(pitch) * mh);
            OS_REQUIRE(mip_payloads[m].size() == static_cast<size_t>(row_bytes) * mh);
            for (UINT y = 0; y < mh; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * pitch,
                            mip_payloads[m].data() + static_cast<size_t>(y) * row_bytes,
                            row_bytes);
            }
            mip_uploads[m]->Unmap(0, nullptr);
        }

        const uint64_t in_bytes = static_cast<uint64_t>(in_uav0.size()) * sizeof(uint32_t);
        const uint64_t out_bytes = static_cast<uint64_t>(out_uav1.size()) * sizeof(uint32_t);
        auto in_up = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_up = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        void* p = nullptr;
        OS_REQUIRE(SUCCEEDED(in_up->Map(0, nullptr, &p)));
        std::memcpy(p, in_uav0.data(), static_cast<size_t>(in_bytes));
        in_up->Unmap(0, nullptr);
        OS_REQUIRE(SUCCEEDED(out_up->Map(0, nullptr, &p)));
        std::memset(p, 0, static_cast<size_t>(out_bytes));
        out_up->Unmap(0, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = fmt;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = mip_count;
        device->CreateShaderResourceView(tex.Get(), &srvd, heap->GetCPUDescriptorHandleForHeapStart());

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        for (UINT m = 0; m < mip_count; ++m) {
            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = mip_uploads[m].Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src_loc.PlacedFootprint.Offset = 0;
            src_loc.PlacedFootprint.Footprint = {fmt, mip_w[m], mip_h[m], 1, mip_pitch[m]};
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = tex.Get();
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst_loc.SubresourceIndex = m;
            cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        }
        cmd_list->CopyResource(in_gpu.Get(), in_up.Get());
        cmd_list->CopyResource(out_gpu.Get(), out_up.Get());

        D3D12_RESOURCE_BARRIER bars[3]{};
        bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[0].Transition = {tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[1].Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        bars[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[2].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(3, bars);

        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(root_sig.Get());
        cmd_list->SetComputeRootUnorderedAccessView(0, in_gpu->GetGPUVirtualAddress());
        cmd_list->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
        cmd_list->SetComputeRootDescriptorTable(2, heap->GetGPUDescriptorHandleForHeapStart());
        cmd_list->Dispatch(group_x, 1, 1);

        D3D12_RESOURCE_BARRIER uav_b{};
        uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_b.UAV.pResource = out_gpu.Get();
        cmd_list->ResourceBarrier(1, &uav_b);

        D3D12_RESOURCE_BARRIER to_src{};
        to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_src.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_src);
        cmd_list->CopyResource(rb.Get(), out_gpu.Get());

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &p)));
        std::memcpy(out_uav1.data(), p, static_cast<size_t>(out_bytes));
        rb->Unmap(0, nullptr);
    }
};

std::vector<uint8_t> SelectDxilForDevice(const D3D12DiffContext& ctx,
                                         const Rdna2LiftResult& lifted) {
    if (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
        return lifted.pc_dxil;
    }
    const char* target = (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) ? "cs_6_2" : "cs_6_0";
    std::vector<uint8_t> dxil;
    std::string diag;
    auto st = CompileHlslWithDxc(lifted.hlsl_source, target, "main", dxil, diag);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] DXC %s compile for %s failed: %s\n",
                     target, ctx.adapter_name.c_str(), diag.c_str());
    }
    OS_REQUIRE(st.ok());
    return dxil;
}

void RunDualUavOnSelectedAdapters(const Rdna2LiftResult& lifted,
                                  const std::vector<uint32_t>& in_data,
                                  const std::vector<uint32_t>& expected,
                                  uint32_t group_x,
                                  const char* label) {
    const bool prefer_warp = EnvPreferWarp();
    D3D12DiffContext ctx;
    OS_REQUIRE(ctx.Init(prefer_warp));
    std::vector<uint32_t> out(expected.size(), 0u);
    ctx.RunComputeDualUav(SelectDxilForDevice(ctx, lifted), in_data, out, group_x);
    for (size_t i = 0; i < expected.size(); ++i) {
        if (out[i] != expected[i]) {
            std::fprintf(stderr, "[FAIL] D3D12 (%s) mismatch for %s idx=%zu got=0x%08x exp=0x%08x\n",
                         ctx.adapter_name.c_str(), label, i, out[i], expected[i]);
            OS_REQUIRE(false);
        }
    }
}
#endif

// ---------------------------------------------------------------------------
// Test 1: Single-Wave Lockstep LDS Barrier Insertion + PSV0 NumThreads
// ---------------------------------------------------------------------------
void Test1_SingleWaveLockstepLdsBarrier() {
    // 1A. 64-thread Wave64 console compute shader with NO s_barrier between ds_write_b32 and ds_read_b32:
    //     lane tid writes LDS[tid] = uav0[tid], then immediately reads LDS[(tid + 32) % 64]
    //     and writes uav1[tid] = uav0[tid] + LDS[(tid + 32) % 64].
    //     Wrap in Xbox DXBC with PSV0 NumThreads(64,1,1) overriding XS2(32,1,1) to verify PSV0 priority!
    Rdna2Assembler a;
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);   // v1 = tid * 4 (global byte off)
    a.EmitVop2(Rdna2Opcode::V_AND_B32, 7, Rdna2Operand::InlineInt(63), 0);      // v7 = tid & 63 (group-local lane)
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 8, Rdna2Operand::InlineInt(2), 7);   // v8 = local * 4 (LDS write addr)
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);                    // v2 = uav0[tid]
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 8, 2);                               // LDS[local] = v2 (NO s_barrier!)
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 3, Rdna2Operand::InlineInt(32), 7);   // v3 = local + 32
    a.EmitVop2(Rdna2Opcode::V_AND_B32, 3, Rdna2Operand::InlineInt(63), 3);      // v3 = (local + 32) & 63
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4, Rdna2Operand::InlineInt(2), 3);   // v4 = partner * 4
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 5, 4);                                   // v5 = LDS[(local + 32) & 63]
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 6, Rdna2Operand::Vgpr(2), 5);         // v6 = v2 + v5
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 0);                   // uav1[tid] = v6
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    // XS2 header says (32,1,1) Wave32; PSV0 explicitly specifies Wave64 + NumThreads(64,1,1)!
    const auto xs2_raw = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
    XboxDxbcBuildDesc dxbc_desc{};
    dxbc_desc.xs2_blob = xs2_raw;
    dxbc_desc.psv0.present = true;
    dxbc_desc.psv0.min_wave_lane_count = 64u;
    dxbc_desc.psv0.max_wave_lane_count = 64u;
    dxbc_desc.psv0.num_threads_x = 64u;
    dxbc_desc.psv0.num_threads_y = 1u;
    dxbc_desc.psv0.num_threads_z = 1u;
    const auto dxbc_blob = BuildXboxDxbcContainer(dxbc_desc);

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    auto st = LiftRdna2Xs2Shader(dxbc_blob, lifted, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test1A LiftRdna2Xs2Shader: %s\n", st.message.c_str());
    }
    OS_REQUIRE(st.ok());

    // Verify PSV0 compute threadgroup dimensions honored in IR, HLSL, and SPIR-V
    OS_REQUIRE(lifted.ir.wave_size == 64u);
    OS_REQUIRE(lifted.ir.threadgroup_x == 64u);
    OS_REQUIRE(lifted.ir.threadgroup_y == 1u);
    OS_REQUIRE(lifted.ir.threadgroup_z == 1u);
    OS_REQUIRE(lifted.hlsl_source.find("[numthreads(64, 1, 1)]") != std::string::npos);

    // Verify exactly 1 lockstep LDS barrier inserted (entry LDS zero-init barrier + 1 inserted RAW barrier = 2 total)
    OS_REQUIRE(lifted.inserted_lockstep_lds_barriers == 1u);
    OS_REQUIRE(lifted.ir.opt_stats.inserted_lockstep_lds_barriers == 1u);
    OS_REQUIRE(CountSubstring(lifted.hlsl_source, "GroupMemoryBarrierWithGroupSync();") == 2u);
    OS_REQUIRE(CountSpirvWorkgroupBarriers(lifted.spirv_words) == 2u);
    VerifySpirvStrict(lifted.spirv_words, "LockstepLds64x1x1", 64u, 1u, 1u);

    // Differential verification across 16 repeated dispatches of 4 workgroups (256 threads)
    // to stress cross-wave scheduling on 32-lane GTX 1650 Ti, WARP, and Vulkan!
#if defined(_WIN32)
    D3D12DiffContext d3d;
    OS_REQUIRE(d3d.Init(EnvPreferWarp()));
    const auto d3d_dxil = SelectDxilForDevice(d3d, lifted);
#endif
    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());

    constexpr uint32_t kGroups = 4u;
    constexpr uint32_t kThreads = kGroups * 64u;
    for (uint32_t iter = 0; iter < 16u; ++iter) {
        std::vector<uint32_t> input(kThreads);
        std::vector<uint32_t> math_ref(kThreads);
        for (uint32_t i = 0; i < kThreads; ++i) {
            input[i] = ((iter + 1u) * 0x10001u) ^ (i * 2654435761u + 0x13579BDFu);
        }
        for (uint32_t g = 0; g < kGroups; ++g) {
            for (uint32_t l = 0; l < 64u; ++l) {
                const uint32_t idx = g * 64u + l;
                const uint32_t partner = g * 64u + ((l + 32u) & 63u);
                math_ref[idx] = input[idx] + input[partner];
            }
        }

        // Wave64 CPU interpreter reference
        Rdna2CpuExecState cpu{};
        cpu.dispatch_x = kGroups;
        cpu.uav0 = input;
        cpu.uav1.assign(kThreads, 0u);
        OS_REQUIRE(ExecuteRdna2BlobCpu(dxbc_blob, cpu).ok());
        OS_REQUIRE(cpu.uav1 == math_ref);

        // Live D3D12 (GTX 1650 Ti or WARP)
#if defined(_WIN32)
        std::vector<uint32_t> d3d_out(kThreads, 0u);
        d3d.RunComputeDualUav(d3d_dxil, input, d3d_out, kGroups);
        OS_REQUIRE(d3d_out == math_ref);
#endif

        // Live Vulkan SPIR-V
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(lifted.spirv_words, input, vk_out, kGroups).ok());
        OS_REQUIRE(vk_out == math_ref);
    }
    vk.Shutdown();

    // 1B. 2D PSV0 threadgroup (8x8x1 = 64 threads) + LDS atomic add followed by LDS read (no s_barrier)
    {
        Rdna2Assembler a2;
        a2.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineInt(0));
        a2.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineInt(1));
        a2.EmitDs(Rdna2Opcode::DS_ADD_U32, 0, 1, 2);      // atomic add LDS[0] += 1 (no s_barrier!)
        a2.EmitDs(Rdna2Opcode::DS_READ_B32, 3, 1);        // read LDS[0] -> needs lockstep barrier!
        a2.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4, Rdna2Operand::InlineInt(2), 0);
        a2.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 4, 4, 0);
        a2.EmitSopp(Rdna2Opcode::S_ENDPGM);

        XboxDxbcBuildDesc d2{};
        d2.xs2_blob = a2.BuildXs2(ShaderStage::Compute, 1, 1, 1, 16, 16, 64);
        d2.psv0.present = true;
        d2.psv0.min_wave_lane_count = 64u;
        d2.psv0.max_wave_lane_count = 64u;
        d2.psv0.num_threads_x = 8u;
        d2.psv0.num_threads_y = 8u;
        d2.psv0.num_threads_z = 1u;

        Rdna2LiftResult r2{};
        OS_REQUIRE(LiftRdna2Xs2Shader(BuildXboxDxbcContainer(d2), r2, opts).ok());
        OS_REQUIRE(r2.inserted_lockstep_lds_barriers == 1u);
        OS_REQUIRE(r2.hlsl_source.find("[numthreads(8, 8, 1)]") != std::string::npos);
        OS_REQUIRE(CountSubstring(r2.hlsl_source, "GroupMemoryBarrierWithGroupSync();") == 2u);
        OS_REQUIRE(CountSpirvWorkgroupBarriers(r2.spirv_words) == 2u);
        VerifySpirvStrict(r2.spirv_words, "LockstepLdsAtomic8x8x1", 8u, 8u, 1u);
    }

    // 1C. Non-spurious insertion checks:
    // (i) Explicit s_barrier already present -> 0 inserted lockstep barriers
    {
        Rdna2Assembler ex;
        ex.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        ex.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        ex.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
        ex.EmitSopp(Rdna2Opcode::S_BARRIER);  // explicit barrier!
        ex.EmitDs(Rdna2Opcode::DS_READ_B32, 3, 1);
        ex.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);
        ex.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult rx{};
        OS_REQUIRE(LiftRdna2Xs2Shader(ex.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 64), rx, opts).ok());
        OS_REQUIRE(rx.inserted_lockstep_lds_barriers == 0u);
        OS_REQUIRE(CountSubstring(rx.hlsl_source, "GroupMemoryBarrierWithGroupSync();") == 2u);  // init + explicit
    }
    // (ii) LDS write-only (no LDS read) -> 0 inserted lockstep barriers
    {
        Rdna2Assembler wo;
        wo.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        wo.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 0);
        wo.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 0, 1, 4, 0);
        wo.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult rwo{};
        OS_REQUIRE(LiftRdna2Xs2Shader(wo.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 64), rwo, opts).ok());
        OS_REQUIRE(rwo.inserted_lockstep_lds_barriers == 0u);
    }
    // (iii) Multi-wave (> 64 threads, e.g. 128x1x1) -> 0 inserted lockstep barriers
    {
        Rdna2LiftResult r128{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 128, 1, 1, 16, 16, 64), r128, opts).ok());
        OS_REQUIRE(r128.inserted_lockstep_lds_barriers == 0u);
    }

    std::printf("[PASS] Test 1: Single-wave lockstep LDS barrier insertion + PSV0 [numthreads] (16x live GPU dispatches bit-exact)\n");
}

// ---------------------------------------------------------------------------
// Test 2: All 5 Explicit No-Op Opcodes
// ---------------------------------------------------------------------------
void Test2_ExplicitNoOpOpcodes() {
    Rdna2Assembler a;
    a.EmitSopk(Rdna2Opcode::S_VERSION, 0, 0x0103);             // 0xB0800103
    a.EmitSopp(Rdna2Opcode::S_ICACHE_INV, 0);                  // 0xBF930000
    a.EmitSopp(Rdna2Opcode::S_TTRACEDATA, 0);                  // 0xBF960000
    a.EmitSopp(Rdna2Opcode::S_TTRACEDATA_IMM, 0x1234);         // 0xBFA81234
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
    a.EmitVop2(Rdna2Opcode::V_XOR_B32, 3, Rdna2Operand::LiteralU32(0xA5A55A5Au), 2);
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 4, Rdna2Operand::InlineInt(17), 3);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);
    a.EmitSopp(Rdna2Opcode::S_CODE_END, 0);                    // 0xBF9F0000 (inline before s_endpgm)
    a.EmitSopp(Rdna2Opcode::S_ENDPGM, 0);                      // 0xBF810000
    a.EmitSopp(Rdna2Opcode::S_CODE_END, 0);                    // 0xBF9F0000 (trailing cache-line padding after s_endpgm)

    // Verify exact AMD RDNA2/GFX10.3 binary encodings
    const auto& words = a.Words();
    OS_REQUIRE(words[0] == 0xB0800103u);  // s_version 0x0103 (SOPK op=0x01)
    OS_REQUIRE(words[1] == 0xBF930000u);  // s_icache_inv (SOPP op=0x13)
    OS_REQUIRE(words[2] == 0xBF960000u);  // s_ttracedata (SOPP op=0x16)
    OS_REQUIRE(words[3] == 0xBFA81234u);  // s_ttracedata_imm 0x1234 (SOPP op=0x28)
    OS_REQUIRE(words[words.size() - 3] == 0xBF9F0000u);  // s_code_end (SOPP op=0x1F)
    OS_REQUIRE(words[words.size() - 2] == 0xBF810000u);  // s_endpgm
    OS_REQUIRE(words[words.size() - 1] == 0xBF9F0000u);  // trailing s_code_end

    std::vector<Rdna2Instruction> decoded;
    OS_REQUIRE(DisassembleRdna2(words, decoded).ok());
    OS_REQUIRE(decoded[0].opcode == Rdna2Opcode::S_VERSION && decoded[0].mnemonic == "s_version");
    OS_REQUIRE(decoded[1].opcode == Rdna2Opcode::S_ICACHE_INV && decoded[1].mnemonic == "s_icache_inv");
    OS_REQUIRE(decoded[2].opcode == Rdna2Opcode::S_TTRACEDATA && decoded[2].mnemonic == "s_ttracedata");
    OS_REQUIRE(decoded[3].opcode == Rdna2Opcode::S_TTRACEDATA_IMM && decoded[3].mnemonic == "s_ttracedata_imm");
    OS_REQUIRE(decoded[decoded.size() - 3].opcode == Rdna2Opcode::S_CODE_END &&
               decoded[decoded.size() - 3].mnemonic == "s_code_end");
    OS_REQUIRE(decoded.back().opcode == Rdna2Opcode::S_CODE_END && decoded.back().mnemonic == "s_code_end");

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
    std::vector<uint32_t> in_data(32), expected(32);
    for (uint32_t i = 0; i < 32u; ++i) {
        in_data[i] = 0x01020304u * (i + 1u);
        expected[i] = (in_data[i] ^ 0xA5A55A5Au) + 17u;
    }

    Rdna2CpuExecState cpu{};
    cpu.uav0 = in_data;
    cpu.uav1.assign(32, 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());
    OS_REQUIRE(cpu.uav1 == expected);

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
    VerifySpirvStrict(lifted.spirv_words, "ExplicitNoOpOpcodes", 32u, 1u, 1u);

    RunOnVulkanSpirv(lifted.spirv_words, in_data, expected, 1u, "ExplicitNoOpOpcodes");
#if defined(_WIN32)
    RunDualUavOnSelectedAdapters(lifted, in_data, expected, 1u, "ExplicitNoOpOpcodes");
#endif

    std::printf("[PASS] Test 2: All 5 explicit no-op opcodes (s_icache_inv, s_ttracedata, s_ttracedata_imm, s_code_end, s_version)\n");
}

// ---------------------------------------------------------------------------
// Test 3: s_bfe_u32 & s_bfe_i32 Edge Cases (CPU Reference vs Live GPU)
// ---------------------------------------------------------------------------
void Test3_SBfeEdgeCases() {
    struct BfeCase {
        uint32_t src0;
        uint32_t raw_off;
        uint32_t raw_wid;
    };
    const std::vector<BfeCase> cases = {
        {0xFFFFFFFFu, 0u,  0u},   // width=0, offset=0 -> 0
        {0xDEADBEEFu, 15u, 0u},   // width=0, offset=15 -> 0
        {0x80000001u, 0u,  32u},  // width=32, offset=0, MSB=1 -> full 32 bits
        {0x7FFFFFFFu, 0u,  32u},  // width=32, offset=0, MSB=0 -> full 32 bits
        {0x8001CAFEu, 16u, 32u},  // width=32, offset=16 -> off+wid >= 32
        {0x80000000u, 31u, 32u},  // width=32, offset=31 -> MSB bit
        {0x80000000u, 31u, 1u},   // offset=31, width=1, MSB=1 -> U=1, I=-1
        {0x7FFFFFFFu, 31u, 1u},   // offset=31, width=1, MSB=0 -> U=0, I=0
        {0x00000001u, 0u,  1u},   // offset=0, width=1, LSB=1 -> U=1, I=-1
        {0x123456F8u, 32u, 8u},   // offset>=32 (32 -> masks to 0), negative 8-bit
        {0x800007F0u, 35u, 8u},   // offset>=32 (35 -> masks to 3), negative 8-bit
        {0xC0000000u, 0u,  31u},  // width=31, offset=0, bit30=1 -> sign extends to 0xC0000000
        {0x80000000u, 1u,  31u},  // width=31, offset=1 (off+wid==32) -> U=0x40000000, I=0xC0000000
        {0xF0000010u, 4u,  40u},  // width>32 (40), offset=4 -> off+wid >= 32
        {0x90000000u, 28u, 4u},   // MSB boundary negative nibble (9 -> -7)
        {0x70000000u, 28u, 4u},   // MSB boundary positive nibble (7 -> +7)
    };

    // Pack inputs into uav0 starting at byte offset 64 (dword 16) so s_load_dword reads UAV0 (not root constants),
    // preventing constant folding so the live GPU executes every single s_bfe_u32 & s_bfe_i32 case!
    const uint32_t num_cases = static_cast<uint32_t>(cases.size());
    std::vector<uint32_t> uav0(16u + num_cases * 2u, 0u);
    std::vector<uint32_t> math_ref(num_cases * 2u, 0u);
    for (uint32_t i = 0; i < num_cases; ++i) {
        const uint32_t desc = (cases[i].raw_off & 0xFFFFu) | ((cases[i].raw_wid & 0x7Fu) << 16);
        uav0[16u + i * 2u + 0u] = cases[i].src0;
        uav0[16u + i * 2u + 1u] = desc;
        math_ref[i * 2u + 0u] = RefSBfeU32(cases[i].src0, desc);
        math_ref[i * 2u + 1u] = RefSBfeI32(cases[i].src0, desc);
    }

    Rdna2Assembler a;
    for (uint32_t i = 0; i < num_cases; ++i) {
        const int32_t in_off = static_cast<int32_t>((16u + i * 2u) * 4u);
        a.EmitSmem(Rdna2Opcode::S_LOAD_DWORD, 20, 0, in_off, 0x7Du);       // s20 = src0
        a.EmitSmem(Rdna2Opcode::S_LOAD_DWORD, 21, 0, in_off + 4, 0x7Du);   // s21 = desc
        a.EmitSop2(Rdna2Opcode::S_BFE_U32, 22, Rdna2Operand::Sgpr(20), Rdna2Operand::Sgpr(21));
        a.EmitSop2(Rdna2Opcode::S_BFE_I32, 23, Rdna2Operand::Sgpr(20), Rdna2Operand::Sgpr(21));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::Sgpr(22));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(23));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineInt(static_cast<int32_t>((i * 2u) * 4u)));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 1, 3, 4, 0);          // uav1[i*2] = bfe_u32
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 3, 4, 4);          // uav1[i*2+1] = bfe_i32
    }
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 1, 1, 1, 32, 8, 32);

    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1.assign(math_ref.size(), 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());
    OS_REQUIRE(cpu.uav1 == math_ref);

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
    VerifySpirvStrict(lifted.spirv_words, "SBfeEdgeCases", 1u, 1u, 1u);

    // Pad expected to uav0 size for Vulkan ExecuteComputeShader (which sizes output == input)
    std::vector<uint32_t> vk_out;
    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());
    OS_REQUIRE(vk.ExecuteComputeShader(lifted.spirv_words, uav0, vk_out, 1u).ok());
    OS_REQUIRE(vk_out.size() >= math_ref.size());
    for (size_t i = 0; i < math_ref.size(); ++i) {
        if (vk_out[i] != math_ref[i]) {
            std::fprintf(stderr, "[FAIL] Vulkan s_bfe mismatch idx=%zu got=0x%08x exp=0x%08x\n",
                         i, vk_out[i], math_ref[i]);
            OS_REQUIRE(false);
        }
    }
    vk.Shutdown();

#if defined(_WIN32)
    RunDualUavOnSelectedAdapters(lifted, uav0, math_ref, 1u, "SBfeEdgeCases");
#endif

    std::printf("[PASS] Test 3: s_bfe_u32 & s_bfe_i32 (%u edge cases: width=0/32/40, off=0/31/>=32, MSB/sign-ext bit-exact)\n",
                num_cases);
}

// ---------------------------------------------------------------------------
// Test 4: v_cvt_pknorm_{i16,u16}_f32 & v_cvt_norm_{i16,u16}_f16 Edge Cases
// ---------------------------------------------------------------------------
void Test4_CvtNormEdgeCases() {
    // All required edge cases: -2.0, -1.0, -0.5, 0.0, +0.5, +1.0, +2.0, -0.0, NaN, +Inf, -Inf + extras
    const std::array<uint32_t, 16> f32_bits_a = {
        FloatToBits(-2.0f),
        FloatToBits(-1.0f),
        FloatToBits(-0.5f),
        FloatToBits(0.0f),
        FloatToBits(0.5f),
        FloatToBits(1.0f),
        FloatToBits(2.0f),
        0x80000000u,  // -0.0f
        0x7FC00000u,  // qNaN
        0x7F800000u,  // +Inf
        0xFF800000u,  // -Inf
        0x7F800001u,  // sNaN
        FloatToBits(-0.25f),
        FloatToBits(0.25f),
        FloatToBits(-0.75f),
        FloatToBits(0.75f),
    };
    const std::array<uint32_t, 16> f32_bits_b = {
        FloatToBits(2.0f),
        FloatToBits(1.0f),
        FloatToBits(0.5f),
        0x80000000u,  // -0.0f
        FloatToBits(-0.5f),
        FloatToBits(-1.0f),
        FloatToBits(-2.0f),
        FloatToBits(0.0f),
        0x7F800000u,  // +Inf
        0xFF800000u,  // -Inf
        0x7FC00000u,  // qNaN
        FloatToBits(0.5f),
        FloatToBits(0.75f),
        FloatToBits(-0.75f),
        FloatToBits(0.25f),
        FloatToBits(-0.25f),
    };

    // Per lane i in [0..15]:
    //   uav0[i*4 + 0] = f32_a
    //   uav0[i*4 + 1] = f32_b
    //   uav0[i*4 + 2] = f16_a (in lo16) | (f16_b << 16)
    //   uav0[i*4 + 3] = 0
    // Outputs in uav1[i*4 + 0..3]:
    //   [0] = v_cvt_pknorm_i16_f32(f32_a, f32_b)
    //   [1] = v_cvt_pknorm_u16_f32(f32_a, f32_b)
    //   [2] = v_cvt_norm_i16_f16(f16_a)
    //   [3] = v_cvt_norm_u16_f16(f16_a)
    std::vector<uint32_t> uav0(16u * 4u, 0u);
    std::vector<uint32_t> math_ref(16u * 4u, 0u);
    for (uint32_t i = 0; i < 16u; ++i) {
        const float fa = BitsToFloat(f32_bits_a[i]);
        const float fb = BitsToFloat(f32_bits_b[i]);
        const uint16_t ha = FloatToHalfBitsRef(fa);
        const uint16_t hb = FloatToHalfBitsRef(fb);
        uav0[i * 4u + 0u] = f32_bits_a[i];
        uav0[i * 4u + 1u] = f32_bits_b[i];
        uav0[i * 4u + 2u] = static_cast<uint32_t>(ha) | (static_cast<uint32_t>(hb) << 16);

        math_ref[i * 4u + 0u] = static_cast<uint32_t>(RefSnorm16(fa)) | (static_cast<uint32_t>(RefSnorm16(fb)) << 16);
        math_ref[i * 4u + 1u] = static_cast<uint32_t>(RefUnorm16(fa)) | (static_cast<uint32_t>(RefUnorm16(fb)) << 16);
        // For f16 inputs, exact value represented by ha is same for all our test values (-2,-1,-0.5,0,+0.5,+1,+2,-0,NaN,+-Inf,+-0.25,+-0.75)!
        math_ref[i * 4u + 2u] = static_cast<uint32_t>(RefSnorm16(fa));
        math_ref[i * 4u + 3u] = static_cast<uint32_t>(RefUnorm16(fa));
    }

    Rdna2Assembler a;
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(4), 0);  // v1 = tid * 16 bytes
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);                   // v2 = f32_a
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 1, 0, 4);                   // v3 = f32_b
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 4, 1, 0, 8);                   // v4 = f16_a | (f16_b << 16)
    a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_I16_F32, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
    a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_U16_F32, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
    a.EmitVop1(Rdna2Opcode::V_CVT_NORM_I16_F16, 7, Rdna2Operand::Vgpr(4));
    a.EmitVop1(Rdna2Opcode::V_CVT_NORM_U16_F16, 8, Rdna2Operand::Vgpr(4));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 4);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 8);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 12);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 16, 1, 1, 16, 16, 32);

    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1.assign(math_ref.size(), 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());
    OS_REQUIRE(cpu.uav1 == math_ref);

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
    VerifySpirvStrict(lifted.spirv_words, "CvtNormEdgeCases", 16u, 1u, 1u);

    RunOnVulkanSpirv(lifted.spirv_words, uav0, math_ref, 1u, "CvtNormEdgeCases");
#if defined(_WIN32)
    RunDualUavOnSelectedAdapters(lifted, uav0, math_ref, 1u, "CvtNormEdgeCases");
#endif

    std::printf("[PASS] Test 4: v_cvt_pknorm_{i16,u16}_f32 & v_cvt_norm_{i16,u16}_f16 (16 edge values including +-2/+-1/+-0.5/+-0/NaN/+-Inf bit-exact)\n");
}

// ---------------------------------------------------------------------------
// Test 5: image_load & image_load_mip Edge Cases (OOB=0, Mips, Partial dmask, Formats)
// ---------------------------------------------------------------------------
void Test5_ImageLoadEdgeCases() {
    // Helper: builds a compute shader that loads (x, y, mip) from uav0[tid*4 + 0..2],
    // executes IMAGE_LOAD or IMAGE_LOAD_MIP with `dmask`, and stores the compacted destination
    // VGPRs (plus sentinel 0x55AA55AAu for unmasked slots) to uav1[tid*4 + 0..3].
    auto build_img_load_shader = [](bool use_mip, uint8_t dmask) -> std::vector<uint8_t> {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(4), 0);  // v1 = tid * 16 bytes
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);                   // v2 = x
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 1, 0, 4);                   // v3 = y
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 4, 1, 0, 8);                   // v4 = mip
        // Pre-fill output VGPRs v8..v11 with sentinel so partial dmask compaction is verified!
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8,  Rdna2Operand::LiteralU32(0x55AA55AAu));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9,  Rdna2Operand::LiteralU32(0x55AA55AAu));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::LiteralU32(0x55AA55AAu));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(0x55AA55AAu));
        a.EmitMimg(use_mip ? Rdna2Opcode::IMAGE_LOAD_MIP : Rdna2Opcode::IMAGE_LOAD,
                   8, 2, 0, 0, dmask, 1u, true, false);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8,  1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9,  1, 4, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 1, 4, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 1, 4, 12);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        return a.BuildXs2(ShaderStage::Compute, 8, 1, 1, 16, 16, 32);
    };

    // 8 query coordinates covering in-bounds, OOB (+X, +Y, negative -1), mip 0, mip 1, mip 2, OOB mip
    const std::array<std::array<uint32_t, 3>, 8> queries = {{
        {0u, 0u, 0u},            // in-bounds mip0 (0,0)
        {3u, 2u, 0u},            // in-bounds mip0 (3,2)
        {4u, 0u, 0u},            // OOB x >= width -> 0
        {0u, 4u, 0u},            // OOB y >= height -> 0
        {0xFFFFFFFFu, 1u, 0u},   // OOB x = -1 -> 0
        {1u, 0u, 1u},            // in-bounds mip1 (1,0)
        {0u, 0u, 2u},            // in-bounds mip2 (0,0)
        {0u, 0u, 3u},            // OOB mip >= mip_count -> 0
    }};
    std::vector<uint32_t> query_uav0(8u * 4u, 0u);
    for (uint32_t i = 0; i < 8u; ++i) {
        query_uav0[i * 4u + 0u] = queries[i][0];
        query_uav0[i * 4u + 1u] = queries[i][1];
        query_uav0[i * 4u + 2u] = queries[i][2];
    }

#if defined(_WIN32)
    D3D12DiffContext d3d;
    OS_REQUIRE(d3d.Init(EnvPreferWarp()));
#endif

    // ----- 5A. RGBA32F (4x4, 3 mips): dmask 0xF, 0x5 (R+B), 0x9 (R+A), IMAGE_LOAD & IMAGE_LOAD_MIP -----
    {
        Rdna2CpuTexture tex_f32;
        tex_f32.AllocateRgba32f(1u, 4u, 4u, 1u, 3u);
        std::vector<std::vector<uint8_t>> mip_bytes(3);
        for (uint32_t m = 0; m < 3u; ++m) {
            auto& lv = tex_f32.mips[m];
            mip_bytes[m].resize(static_cast<size_t>(lv.width) * lv.height * 16u);
            for (uint32_t y = 0; y < lv.height; ++y) {
                for (uint32_t x = 0; x < lv.width; ++x) {
                    const size_t idx = (static_cast<size_t>(y) * lv.width + x) * 4u;
                    lv.rgba32f[idx + 0u] = static_cast<float>(m * 100u + y * 10u + x) + 0.25f;
                    lv.rgba32f[idx + 1u] = static_cast<float>(m * 100u + y * 10u + x) + 0.50f;
                    lv.rgba32f[idx + 2u] = static_cast<float>(m * 100u + y * 10u + x) + 0.75f;
                    lv.rgba32f[idx + 3u] = 1.0f + static_cast<float>(m) * 0.125f;
                }
            }
            std::memcpy(mip_bytes[m].data(), lv.rgba32f.data(), mip_bytes[m].size());
        }

        const std::array<std::pair<bool, uint8_t>, 4> configs = {{
            {false, static_cast<uint8_t>(0xFu)},  // IMAGE_LOAD RGBA
            {false, static_cast<uint8_t>(0x5u)},  // IMAGE_LOAD R+B partial dmask
            {false, static_cast<uint8_t>(0x9u)},  // IMAGE_LOAD R+A partial dmask
            {true,  static_cast<uint8_t>(0xFu)},  // IMAGE_LOAD_MIP RGBA across mips 0,1,2 + OOB mip 3
        }};
        for (const auto& cfg : configs) {
            const auto xs2 = build_img_load_shader(cfg.first, cfg.second);
            Rdna2CpuExecState cpu{};
            cpu.tex2d_srv = tex_f32;
            cpu.uav0 = query_uav0;
            cpu.uav1.assign(8u * 4u, 0u);
            OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());

            // Verify OOB lanes returned exact 0.0f and partial dmask preserved sentinel in unused VGPRs!
            OS_REQUIRE(cpu.uav1[2 * 4 + 0] == 0u);  // (4,0) OOB -> 0
            OS_REQUIRE(cpu.uav1[3 * 4 + 0] == 0u);  // (0,4) OOB -> 0
            OS_REQUIRE(cpu.uav1[4 * 4 + 0] == 0u);  // (-1,1) OOB -> 0
            if (cfg.second == 0x5u || cfg.second == 0x9u) {
                OS_REQUIRE(cpu.uav1[0 * 4 + 2] == 0x55AA55AAu);
                OS_REQUIRE(cpu.uav1[0 * 4 + 3] == 0x55AA55AAu);
            }
            if (cfg.first) {
                OS_REQUIRE(cpu.uav1[7 * 4 + 0] == 0u);  // mip=3 OOB -> 0
                OS_REQUIRE(BitsToFloat(cpu.uav1[5 * 4 + 0]) == 101.25f);  // mip1 (1,0).r
                OS_REQUIRE(BitsToFloat(cpu.uav1[6 * 4 + 0]) == 200.25f);  // mip2 (0,0).r
            }

            Rdna2LiftOptions opts{};
            opts.emit_spirv = true;
            opts.emit_hlsl = true;
            opts.compile_dxil_with_dxc = true;
            Rdna2LiftResult lifted{};
            OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
            VerifySpirvStrict(lifted.spirv_words, "ImageLoadRgba32f", 8u, 1u, 1u);

#if defined(_WIN32)
            std::vector<uint32_t> gpu_out(8u * 4u, 0u);
            d3d.RunComputeImageLoad2D(SelectDxilForDevice(d3d, lifted), DXGI_FORMAT_R32G32B32A32_FLOAT,
                                      4u, 4u, mip_bytes, 16u, query_uav0, gpu_out, 1u);
            for (size_t i = 0; i < gpu_out.size(); ++i) {
                if (gpu_out[i] != cpu.uav1[i]) {
                    std::fprintf(stderr, "[FAIL] RGBA32F image_load mismatch (mip=%d dmask=0x%x) idx=%zu gpu=0x%08x cpu=0x%08x\n",
                                 cfg.first ? 1 : 0, cfg.second, i, gpu_out[i], cpu.uav1[i]);
                    OS_REQUIRE(false);
                }
            }
#endif
        }
    }

    // ----- 5B. RGBA8_UNORM (4x4, 2 mips): dmask 0x5 (R+B) & IMAGE_LOAD_MIP dmask 0x9 (R+A) -----
    {
        Rdna2CpuTexture tex_u8;
        tex_u8.AllocateRgba32f(1u, 4u, 4u, 1u, 2u);
        std::vector<std::vector<uint8_t>> mip_bytes(2);
        for (uint32_t m = 0; m < 2u; ++m) {
            auto& lv = tex_u8.mips[m];
            mip_bytes[m].resize(static_cast<size_t>(lv.width) * lv.height * 4u);
            for (uint32_t y = 0; y < lv.height; ++y) {
                for (uint32_t x = 0; x < lv.width; ++x) {
                    const uint8_t r = static_cast<uint8_t>((m * 85u + y * 51u + x * 17u) & 0xFFu);
                    const uint8_t g = static_cast<uint8_t>((255u - r) & 0xFFu);
                    const uint8_t b = static_cast<uint8_t>((m * 102u + x * 64u) & 0xFFu);
                    const uint8_t a = static_cast<uint8_t>(m == 0u ? 255u : 128u);
                    const size_t idx = (static_cast<size_t>(y) * lv.width + x) * 4u;
                    mip_bytes[m][idx + 0u] = r;
                    mip_bytes[m][idx + 1u] = g;
                    mip_bytes[m][idx + 2u] = b;
                    mip_bytes[m][idx + 3u] = a;
                    lv.rgba32f[idx + 0u] = static_cast<float>(r) / 255.0f;
                    lv.rgba32f[idx + 1u] = static_cast<float>(g) / 255.0f;
                    lv.rgba32f[idx + 2u] = static_cast<float>(b) / 255.0f;
                    lv.rgba32f[idx + 3u] = static_cast<float>(a) / 255.0f;
                }
            }
        }

        const std::array<std::pair<bool, uint8_t>, 2> configs = {{
            {false, static_cast<uint8_t>(0x5u)},  // IMAGE_LOAD R+B
            {true,  static_cast<uint8_t>(0x9u)},  // IMAGE_LOAD_MIP R+A (mip 0, mip 1, OOB mip 2/3)
        }};
        for (const auto& cfg : configs) {
            const auto xs2 = build_img_load_shader(cfg.first, cfg.second);
            Rdna2CpuExecState cpu{};
            cpu.tex2d_srv = tex_u8;
            cpu.uav0 = query_uav0;
            cpu.uav1.assign(8u * 4u, 0u);
            OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());

            Rdna2LiftOptions opts{};
            opts.emit_spirv = true;
            opts.emit_hlsl = true;
            opts.compile_dxil_with_dxc = true;
            Rdna2LiftResult lifted{};
            OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
            VerifySpirvStrict(lifted.spirv_words, "ImageLoadRgba8Unorm", 8u, 1u, 1u);

#if defined(_WIN32)
            std::vector<uint32_t> gpu_out(8u * 4u, 0u);
            d3d.RunComputeImageLoad2D(SelectDxilForDevice(d3d, lifted), DXGI_FORMAT_R8G8B8A8_UNORM,
                                      4u, 4u, mip_bytes, 4u, query_uav0, gpu_out, 1u);
            for (size_t i = 0; i < gpu_out.size(); ++i) {
                if ((i & 3u) >= 2u) {
                    OS_REQUIRE(gpu_out[i] == 0x55AA55AAu && cpu.uav1[i] == 0x55AA55AAu);
                } else {
                    const float gf = BitsToFloat(gpu_out[i]);
                    const float cf = BitsToFloat(cpu.uav1[i]);
                    if (std::fabs(gf - cf) > 1e-6f) {
                        std::fprintf(stderr, "[FAIL] RGBA8_UNORM image_load mismatch idx=%zu gpu=%f cpu=%f\n",
                                     i, static_cast<double>(gf), static_cast<double>(cf));
                        OS_REQUIRE(false);
                    }
                }
            }
#endif
        }
    }

    // ----- 5C. R32_UINT (4x4, 2 mips): bit-exact uint32 including NaN/subnormal bit patterns, OOB=0, dmask 0x1 & 0x9 -----
    {
        Rdna2CpuTexture tex_u32;
        tex_u32.AllocateR32u(1u, 4u, 4u, 1u, 2u);
        std::vector<std::vector<uint8_t>> mip_bytes(2);
        for (uint32_t m = 0; m < 2u; ++m) {
            auto& lv = tex_u32.mips[m];
            mip_bytes[m].resize(static_cast<size_t>(lv.width) * lv.height * 4u);
            for (uint32_t y = 0; y < lv.height; ++y) {
                for (uint32_t x = 0; x < lv.width; ++x) {
                    const size_t idx = static_cast<size_t>(y) * lv.width + x;
                    // Include patterns that would be corrupted if treated as float (sNaN, subnormal, etc.)
                    uint32_t val = 0x7FC01000u ^ (m * 0x11110000u) ^ (y * 0x0100u + x);
                    if (m == 0u && x == 0u && y == 0u) val = 0x00000001u;  // FP32 subnormal bit pattern!
                    if (m == 0u && x == 3u && y == 2u) val = 0x7F800001u;  // FP32 sNaN bit pattern!
                    if (m == 1u && x == 1u && y == 0u) val = 0xDEADBEEFu;
                    lv.r32u[idx] = val;
                }
            }
            std::memcpy(mip_bytes[m].data(), lv.r32u.data(), mip_bytes[m].size());
        }

        const std::array<std::pair<bool, uint8_t>, 2> configs = {{
            {false, static_cast<uint8_t>(0x1u)},  // IMAGE_LOAD R only
            {true,  static_cast<uint8_t>(0x9u)},  // IMAGE_LOAD_MIP R+A (in-bounds=(val,1), OOB=(0,0))
        }};
        for (const auto& cfg : configs) {
            const auto xs2 = build_img_load_shader(cfg.first, cfg.second);
            Rdna2CpuExecState cpu{};
            cpu.tex2d_srv = tex_u32;
            cpu.uav0 = query_uav0;
            cpu.uav1.assign(8u * 4u, 0u);
            OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());

            // Verify bit-exact subnormal / sNaN preservation and OOB=0 on CPU
            OS_REQUIRE(cpu.uav1[0 * 4 + 0] == 0x00000001u);
            OS_REQUIRE(cpu.uav1[1 * 4 + 0] == 0x7F800001u);
            OS_REQUIRE(cpu.uav1[2 * 4 + 0] == 0u);
            if (cfg.first) {
                OS_REQUIRE(cpu.uav1[5 * 4 + 0] == 0xDEADBEEFu);
                OS_REQUIRE(cpu.uav1[5 * 4 + 1] == 1u);  // R32_UINT A channel in-bounds == 1
                OS_REQUIRE(cpu.uav1[6 * 4 + 0] == 0u && cpu.uav1[6 * 4 + 1] == 1u);  // mip=2 OOB -> R=0, missing A=1
            }

            Rdna2LiftOptions opts{};
            opts.emit_spirv = true;
            opts.emit_hlsl = true;
            opts.compile_dxil_with_dxc = true;
            opts.stage_contract.texture_srv_format = Rdna2TextureFormat::Uint4;
            Rdna2LiftResult lifted{};
            OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
            VerifySpirvStrict(lifted.spirv_words, "ImageLoadR32Uint", 8u, 1u, 1u);

#if defined(_WIN32)
            std::vector<uint32_t> gpu_out(8u * 4u, 0u);
            d3d.RunComputeImageLoad2D(SelectDxilForDevice(d3d, lifted), DXGI_FORMAT_R32_UINT,
                                      4u, 4u, mip_bytes, 4u, query_uav0, gpu_out, 1u);
            for (size_t i = 0; i < gpu_out.size(); ++i) {
                if (gpu_out[i] != cpu.uav1[i]) {
                    std::fprintf(stderr, "[FAIL] R32_UINT image_load mismatch (mip=%d dmask=0x%x) idx=%zu gpu=0x%08x cpu=0x%08x\n",
                                 cfg.first ? 1 : 0, cfg.second, i, gpu_out[i], cpu.uav1[i]);
                    OS_REQUIRE(false);
                }
            }
#endif
        }
    }

    std::printf("[PASS] Test 5: image_load / image_load_mip edge cases (in-bounds, OOB=0, mips, dmask 0x1/0x5/0x9/0xF, RGBA32F/RGBA8_UNORM/R32_UINT)\n");
}

}  // namespace

int main() {
#if defined(_WIN32)
    D3D12DiffContext probe;
    OS_REQUIRE(probe.Init(EnvPreferWarp()));
    std::printf("[INFO] test_rdna2_lockstep_isa D3D12 adapter: %s (warp=%s)\n",
                probe.adapter_name.c_str(), probe.is_warp ? "yes" : "no");
#endif
    Test1_SingleWaveLockstepLdsBarrier();
    Test2_ExplicitNoOpOpcodes();
    Test3_SBfeEdgeCases();
    Test4_CvtNormEdgeCases();
    Test5_ImageLoadEdgeCases();
    std::printf("[PASS] test_rdna2_lockstep_isa: ALL 5 DIFFERENTIAL ISA & LOCKSTEP LDS SUITES PASSED!\n");
    return 0;
}
