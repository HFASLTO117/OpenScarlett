#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"
#include "rdna2_isa_mem_img.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

using namespace openscarlett;
using namespace openscarlett::rdna2_mem;

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

bool HasExternalSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(kDefaultSpirvValPath, ec);
}

bool HasDxc() {
    std::error_code ec;
    const std::string p = ResolvePortableDxcPath();
    return !p.empty() && std::filesystem::exists(p, ec);
}

Rdna2LiftOptions MakeStrictLiftOptions(bool compile_dxil) {
    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = compile_dxil && HasDxc();
    opts.resign_dxil = true;
    opts.spirv_val_path = kDefaultSpirvValPath;
    opts.spirv_target_env = "vulkan1.2";
    return opts;
}

void EmitSoppWord(std::vector<uint32_t>& words, uint16_t raw_op, int16_t simm16 = 0) {
    const uint32_t w0 = (0x17Fu << 23) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 16) |
                        static_cast<uint32_t>(static_cast<uint16_t>(simm16));
    words.push_back(w0);
}

std::vector<uint32_t> BytesToU32(const std::vector<uint8_t>& b) {
    const size_t n = b.size() / 4u;
    std::vector<uint32_t> out(n, 0u);
    if (n > 0u) std::memcpy(out.data(), b.data(), n * 4u);
    return out;
}

std::vector<uint8_t> U32ToBytes(const std::vector<uint32_t>& u) {
    std::vector<uint8_t> out(u.size() * 4u, 0u);
    if (!u.empty()) std::memcpy(out.data(), u.data(), out.size());
    return out;
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct D3D12LiveHarness {
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

    ~D3D12LiveHarness() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    bool Init(bool use_warp) {
        is_warp = use_warp;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        if (use_warp) {
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
        } else {
            if (FAILED(factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
                return false;
            }
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
        adapter_name = buf;

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
                                             IID_PPV_ARGS(&cmd_list)))) return false;
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

    ComPtr<ID3D12Resource> CreateBuf(uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
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
        ComPtr<ID3D12Resource> r;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))));
        return r;
    }

    void RunCompute2Uav(const std::vector<uint8_t>& dxil,
                        const std::vector<uint32_t>& uav0_init,
                        const std::vector<uint32_t>& uav1_init,
                        std::vector<uint32_t>& uav1_out,
                        uint32_t group_x) {
        const uint64_t sz0 = std::max<uint64_t>(256u, static_cast<uint64_t>(uav0_init.size()) * 4u);
        const uint64_t sz1 = std::max<uint64_t>(256u, static_cast<uint64_t>(uav1_init.size()) * 4u);
        uav1_out.assign(uav1_init.size(), 0u);

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
        ComPtr<ID3DBlob> sig, err;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)));
        ComPtr<ID3D12RootSignature> rs;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = rs.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] CreateComputePipelineState on %s: hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        auto up0 = CreateBuf(sz0, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto up1 = CreateBuf(sz1, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto gpu0 = CreateBuf(sz0, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto gpu1 = CreateBuf(sz1, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb1 = CreateBuf(sz1, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        void* p = nullptr;
        OS_REQUIRE(SUCCEEDED(up0->Map(0, nullptr, &p)));
        std::memset(p, 0, static_cast<size_t>(sz0));
        if (!uav0_init.empty()) std::memcpy(p, uav0_init.data(), uav0_init.size() * 4u);
        up0->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(up1->Map(0, nullptr, &p)));
        std::memset(p, 0, static_cast<size_t>(sz1));
        if (!uav1_init.empty()) std::memcpy(p, uav1_init.data(), uav1_init.size() * 4u);
        up1->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));
        cmd_list->CopyResource(gpu0.Get(), up0.Get());
        cmd_list->CopyResource(gpu1.Get(), up1.Get());

        D3D12_RESOURCE_BARRIER b2u[2]{};
        b2u[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b2u[0].Transition = {gpu0.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        b2u[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b2u[1].Transition = {gpu1.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(2, b2u);

        cmd_list->SetComputeRootSignature(rs.Get());
        cmd_list->SetComputeRootUnorderedAccessView(0, gpu0->GetGPUVirtualAddress());
        cmd_list->SetComputeRootUnorderedAccessView(1, gpu1->GetGPUVirtualAddress());
        cmd_list->Dispatch(group_x, 1, 1);

        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = gpu1.Get();
        cmd_list->ResourceBarrier(1, &uavb);

        D3D12_RESOURCE_BARRIER b2c{};
        b2c.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b2c.Transition = {gpu1.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &b2c);
        cmd_list->CopyResource(rb1.Get(), gpu1.Get());

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(rb1->Map(0, nullptr, &p)));
        if (!uav1_out.empty()) std::memcpy(uav1_out.data(), p, uav1_out.size() * 4u);
        rb1->Unmap(0, nullptr);
    }

    void RunComputeTex2DLoadStore(const std::vector<uint8_t>& dxil,
                                  const std::vector<float>& in_rgba_16x16,
                                  std::vector<float>& out_rgba_16x16,
                                  std::vector<uint32_t>& out_u32_16x16) {
        constexpr UINT kW = 16, kH = 16, kPitch = kW * 16, kBytes = kPitch * kH;
        out_rgba_16x16.assign(kW * kH * 4, 0.0f);
        out_u32_16x16.assign(kW * kH, 0u);

        // Descriptor table: t0 (SRV), u2 (RWTexture2D<float4>), u3 (RWTexture2D<uint>)
        D3D12_DESCRIPTOR_RANGE ranges[3]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0, 1};
        ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 0, 2};

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {3, ranges};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        samp.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;

        ComPtr<ID3DBlob> sig, err;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)));
        ComPtr<ID3D12RootSignature> rs;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = rs.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        OS_REQUIRE(SUCCEEDED(device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso))));

        D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kW;
        td.Height = kH;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> in_tex, rw_tex, rw_u32;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&in_tex))));
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&rw_tex))));
        td.Format = DXGI_FORMAT_R32_UINT;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rw_u32))));

        auto up_f4 = CreateBuf(kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto up_u32 = CreateBuf(kH * 256u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto rb_f4 = CreateBuf(kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb_u32 = CreateBuf(kH * 256u, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        void* p = nullptr;
        OS_REQUIRE(SUCCEEDED(up_f4->Map(0, nullptr, &p)));
        std::memcpy(p, in_rgba_16x16.data(), kBytes);
        up_f4->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(up_u32->Map(0, nullptr, &p)));
        std::memset(p, 0, kH * 256u);
        for (UINT y = 0; y < kH; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(p) + y * 256u);
            for (UINT x = 0; x < kW; ++x) row[x] = 100u + x;
        }
        up_u32->Unmap(0, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h0 = heap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(in_tex.Get(), &srvd, h0);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h1 = {h0.ptr + inc};
        device->CreateUnorderedAccessView(rw_tex.Get(), nullptr, &uavd, h1);

        uavd.Format = DXGI_FORMAT_R32_UINT;
        D3D12_CPU_DESCRIPTOR_HANDLE h2 = {h0.ptr + 2 * inc};
        device->CreateUnorderedAccessView(rw_u32.Get(), nullptr, &uavd, h2);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        D3D12_TEXTURE_COPY_LOCATION dst_loc{}, src_loc{};
        dst_loc = {in_tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        src_loc = {up_f4.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, kPitch};
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        dst_loc = {rw_u32.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        src_loc = {up_u32.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, kW, kH, 1, 256u};
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        D3D12_RESOURCE_BARRIER tr[2]{};
        tr[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tr[0].Transition = {in_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        tr[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tr[1].Transition = {rw_u32.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(2, tr);

        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(rs.Get());
        cmd_list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        cmd_list->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER post[2]{};
        post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[0].Transition = {rw_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[1].Transition = {rw_u32.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(2, post);

        dst_loc = {rb_f4.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, kPitch};
        src_loc = {rw_tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        dst_loc = {rb_u32.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, kW, kH, 1, 256u};
        src_loc = {rw_u32.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(rb_f4->Map(0, nullptr, &p)));
        std::memcpy(out_rgba_16x16.data(), p, kBytes);
        rb_f4->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(rb_u32->Map(0, nullptr, &p)));
        for (UINT y = 0; y < kH; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + y * 256u);
            for (UINT x = 0; x < kW; ++x) out_u32_16x16[y * kW + x] = row[x];
        }
        rb_u32->Unmap(0, nullptr);
    }
};
#endif

// ---------------------------------------------------------------------------
// Test 1: Complete Table Count (>= 325) & 100% Encode -> Decode Round-Trip
// ---------------------------------------------------------------------------
void TestCompleteOpcodeTableRoundTrip() {
    const auto& table = GetMemImgOpcodeTable();
    const size_t unique_count = GetUniqueMemImgMnemonicCount();
    std::printf("[TEST 1] GFX10.3 Mem/Img/LDS/FLAT/EXP Table: %zu entries, %zu unique mnemonics\n",
                table.size(), unique_count);
    OS_REQUIRE(unique_count >= 325u);
    OS_REQUIRE(table.size() == unique_count);

    size_t c_mubuf = 0, c_mtbuf = 0, c_mimg = 0, c_ds = 0, c_flat = 0, c_exp = 0;
    for (const auto& entry : table) {
        OS_REQUIRE(LookupMemImgByName(entry.mnemonic) == &entry);
        OS_REQUIRE(LookupMemImgOpcode(entry.encoding, entry.raw_opcode, entry.flat_seg) == &entry);

        MemImgEncodeParams p{};
        p.vdst = 7;
        p.vdata = 10;
        p.vaddr = 2;
        p.data0 = 12;
        p.data1 = 14;
        p.srsrc_sgpr = 8;
        p.ssamp_sgpr = 12;
        p.soffset = 132;
        p.saddr = 4;
        p.offset = (entry.encoding == Rdna2Encoding::FLAT && entry.flat_seg != 0u) ? -16 : 64;
        p.idxen = true;
        p.offen = true;
        p.glc = true;
        p.slc = true;
        p.dlc = true;
        p.lds = (entry.encoding == Rdna2Encoding::MUBUF);
        p.tfe = true;
        p.lwe = true;
        p.d16 = true;
        p.a16 = true;
        p.r128 = true;
        p.unrm = true;
        p.gds = true;
        p.dmask = 0xBu;
        p.dim = kMimgDim2DArray;
        p.mtbuf_format = Rdna2MtbufFormat::R16G16B16A16_FLOAT;
        p.exp_target = 35;  // param3
        p.exp_en = 0xDu;
        p.exp_vsrc[0] = 4; p.exp_vsrc[1] = 5; p.exp_vsrc[2] = 6; p.exp_vsrc[3] = 7;
        p.exp_done = true;
        p.exp_compr = true;
        p.exp_vm = true;

        std::vector<uint32_t> words;
        OS_REQUIRE(EncodeMemImgInst(entry.mnemonic, p, words));
        OS_REQUIRE(words.size() == 2u);

        MemImgDecodedInst dec{};
        OS_REQUIRE(DecodeMemImgAt(words.data(), words.size(), 0u, dec).ok());
        OS_REQUIRE(dec.entry == &entry);
        OS_REQUIRE(dec.base.encoding == entry.encoding);
        OS_REQUIRE(dec.base.raw_opcode == entry.raw_opcode);
        OS_REQUIRE(dec.base.mnemonic == entry.mnemonic);

        switch (entry.encoding) {
            case Rdna2Encoding::MUBUF:
                ++c_mubuf;
                OS_REQUIRE(dec.base.idxen && dec.base.offen && dec.base.glc && dec.base.slc && dec.base.dlc);
                OS_REQUIRE(dec.lds && dec.tfe);
                OS_REQUIRE(dec.base.mem_offset == 64);
                OS_REQUIRE(dec.base.srsrc == 2u && dec.base.soffset == 132u);
                break;
            case Rdna2Encoding::MTBUF:
                ++c_mtbuf;
                OS_REQUIRE(dec.base.idxen && dec.base.offen && dec.base.glc && dec.base.slc && dec.base.dlc);
                OS_REQUIRE(dec.base.mtbuf_format == static_cast<uint8_t>(Rdna2MtbufFormat::R16G16B16A16_FLOAT));
                OS_REQUIRE(dec.base.mem_offset == 64);
                break;
            case Rdna2Encoding::MIMG:
                ++c_mimg;
                OS_REQUIRE(dec.base.mimg_dmask == 0xBu && dec.base.mimg_dim == kMimgDim2DArray);
                OS_REQUIRE(dec.base.mimg_unrm && dec.base.glc && dec.base.slc && dec.base.dlc);
                OS_REQUIRE(dec.d16 && dec.a16 && dec.r128 && dec.tfe && dec.lwe);
                OS_REQUIRE(dec.base.srsrc == 2u && dec.base.ssamp == 3u);
                break;
            case Rdna2Encoding::DS:
                ++c_ds;
                OS_REQUIRE(dec.base.ds_gds && dec.base.mem_offset == 64);
                break;
            case Rdna2Encoding::FLAT:
                ++c_flat;
                OS_REQUIRE(dec.base.flat_seg == entry.flat_seg);
                OS_REQUIRE(dec.base.glc && dec.base.slc && dec.base.dlc);
                OS_REQUIRE(dec.base.saddr == 4u);
                OS_REQUIRE(dec.base.mem_offset == ((entry.flat_seg != 0u) ? -16 : 64));
                break;
            case Rdna2Encoding::EXP:
                ++c_exp;
                OS_REQUIRE(dec.base.exp_target == 35u && dec.base.exp_en == 0xDu);
                OS_REQUIRE(dec.base.exp_done && dec.base.exp_compr && dec.base.exp_vm);
                break;
            default:
                OS_REQUIRE(false);
        }
    }
    std::printf("  [OK] 100%% encode->decode round-trip passed (MUBUF=%zu, MTBUF=%zu, MIMG=%zu, DS=%zu, FLAT/GLOBAL/SCRATCH=%zu, EXP=%zu)\n",
                c_mubuf, c_mtbuf, c_mimg, c_ds, c_flat, c_exp);
}

// ---------------------------------------------------------------------------
// Test 2: MUBUF / MTBUF Loads, Stores, Sub-dword D16 & Atomics (CPU vs Live GPU)
// ---------------------------------------------------------------------------
void TestMubufMtbufCpuAndLiveGpu(
#if defined(_WIN32)
    D3D12LiveHarness* hw_d3d12,
    D3D12LiveHarness* warp_d3d12,
#endif
    VulkanBackend* vk_hw) {
    std::printf("[TEST 2] MUBUF & MTBUF loads/stores/sub-dword/D16/atomics CPU vs Live GPU\n");

    // 2A. CPU reference test covering byte/short/dwordx4/d16 + all 32-bit & 64-bit atomics + MTBUF formats
    {
        std::vector<uint32_t> code;
        MemImgEncodeParams p{};
        p.offen = true;
        // Load ubyte/sbyte/ushort/sshort/dwordx4 from uav0
        p.srsrc_sgpr = 0; p.vaddr = 1; p.vdata = 10;
        OS_REQUIRE(EncodeMemImgInst("buffer_load_ubyte", p, code));
        p.vdata = 11; p.offset = 1;
        OS_REQUIRE(EncodeMemImgInst("buffer_load_sbyte", p, code));
        p.vdata = 12; p.offset = 0;
        OS_REQUIRE(EncodeMemImgInst("buffer_load_ushort", p, code));
        p.vdata = 13; p.offset = 2;
        OS_REQUIRE(EncodeMemImgInst("buffer_load_sshort", p, code));
        p.vdata = 14; p.offset = 0;
        OS_REQUIRE(EncodeMemImgInst("buffer_load_ubyte_d16_hi", p, code));
        // MTBUF R8G8B8A8_UNORM load_format_d16_xyzw
        p.vaddr = 1; p.vdata = 16; p.mtbuf_format = Rdna2MtbufFormat::R8G8B8A8_UNORM;
        OS_REQUIRE(EncodeMemImgInst("tbuffer_load_format_d16_xyzw", p, code));
        // Buffer atomics on uav1[0..36]: add, sub, csub, umin, umax, smin, smax, and, or, xor, cmpswap, swap, inc, dec
        const char* atomic_ops[] = {
            "buffer_atomic_add", "buffer_atomic_sub", "buffer_atomic_csub",
            "buffer_atomic_umin", "buffer_atomic_umax", "buffer_atomic_smin", "buffer_atomic_smax",
            "buffer_atomic_and", "buffer_atomic_or", "buffer_atomic_xor",
            "buffer_atomic_cmpswap", "buffer_atomic_swap", "buffer_atomic_inc", "buffer_atomic_dec",
        };
        for (uint32_t i = 0; i < 14u; ++i) {
            MemImgEncodeParams ap{};
            ap.srsrc_sgpr = 4;  // uav1
            ap.offen = false;
            ap.offset = static_cast<int32_t>(i * 4u);
            ap.vdata = 20;
            ap.glc = true;
            OS_REQUIRE(EncodeMemImgInst(atomic_ops[i], ap, code));
        }
        // 64-bit atomic add_x2 at uav1[64]
        {
            MemImgEncodeParams ap{};
            ap.srsrc_sgpr = 4;
            ap.offset = 64;
            ap.vdata = 24;
            ap.glc = true;
            OS_REQUIRE(EncodeMemImgInst("buffer_atomic_add_x2", ap, code));
        }

        std::vector<MemImgDecodedInst> insts;
        OS_REQUIRE(DecodeMemImgStream(code, insts).ok());

        MemImgExecState st{};
        st.wave_size = 4;
        st.exec_mask = 0xFull;
        SeedDefaultComputeVgprs(st);
        st.uav0.assign(256u, 0u);
        st.uav0[0] = 0xFFu; st.uav0[1] = 0xFEu; st.uav0[2] = 0x00u; st.uav0[3] = 0x80u;
        st.uav1.assign(256u, 0u);
        std::vector<uint32_t> init_u1(64u, 10u);
        init_u1[10] = 5u;  // cmpswap target
        init_u1[16] = 0xFFFFFFFEu; init_u1[17] = 0u;  // 64-bit add_x2 carry test
        st.uav1 = U32ToBytes(init_u1);
        for (uint32_t l = 0; l < 4u; ++l) {
            st.vgpr[l][20] = 3u;  // src0
            st.vgpr[l][21] = 5u;  // cmp
            st.vgpr[l][24] = 4u;  // x2 lo
            st.vgpr[l][25] = 0u;  // x2 hi
        }
        OS_REQUIRE(ExecuteMemImgCpu(insts, st).ok());
        OS_REQUIRE(st.vgpr[0][10] == 0xFFu);
        OS_REQUIRE(static_cast<int32_t>(st.vgpr[0][11]) == -2);
        OS_REQUIRE(st.vgpr[0][12] == 0xFEFFu);
        OS_REQUIRE(static_cast<int32_t>(st.vgpr[0][13]) == -32768);
        OS_REQUIRE((st.vgpr[0][14] >> 16) == 0xFFu);
        // Check 64-bit add_x2 carried across 32-bit boundary: 0xFFFFFFFE + 4*4 = 0x10000000E
        auto out_u1 = BytesToU32(st.uav1);
        OS_REQUIRE(out_u1[16] == 0x0Eu && out_u1[17] == 1u);
        // Check add (10 + 4*3 = 22)
        OS_REQUIRE(out_u1[0] == 22u);
        std::printf("  [OK] MUBUF/MTBUF CPU reference (sub-dword, D16, MTBUF format, 32b & 64b atomics)\n");
    }

    // 2B. Live GPU parity shader: MUBUF dwordx4 load + MTBUF format load + buffer_atomic_add/umin/umax/and/or/xor/cmpswap -> uav1
    {
        std::vector<uint32_t> code;
        // Load dwordx4 from uav0 at v2 (tid*16) -> v[10..13], store to uav1 at v2 (tid*16)
        {
            MemImgEncodeParams p{};
            p.srsrc_sgpr = 0; p.offen = true; p.vaddr = 2; p.vdata = 10;
            OS_REQUIRE(EncodeMemImgInst("buffer_load_dwordx4", p, code));
            p.srsrc_sgpr = 4;
            OS_REQUIRE(EncodeMemImgInst("buffer_store_dwordx4", p, code));
        }
        // Load ubyte from uav0 at v1 -> v20, store dword to uav1 at 256 + v1
        {
            MemImgEncodeParams p{};
            p.srsrc_sgpr = 0; p.offen = true; p.vaddr = 1; p.vdata = 20;
            OS_REQUIRE(EncodeMemImgInst("buffer_load_ubyte", p, code));
            p.srsrc_sgpr = 4; p.offset = 256;
            OS_REQUIRE(EncodeMemImgInst("buffer_store_dword", p, code));
        }
        // Atomic add v[10] across all 16 lanes into uav1[512], atomic or into uav1[516], atomic xor into uav1[520]
        {
            MemImgEncodeParams p{};
            p.srsrc_sgpr = 4; p.offen = false; p.glc = false; p.vdata = 10;
            p.offset = 512; OS_REQUIRE(EncodeMemImgInst("buffer_atomic_add", p, code));
            p.offset = 516; OS_REQUIRE(EncodeMemImgInst("buffer_atomic_or", p, code));
            p.offset = 520; OS_REQUIRE(EncodeMemImgInst("buffer_atomic_xor", p, code));
            p.offset = 524; OS_REQUIRE(EncodeMemImgInst("buffer_atomic_umax", p, code));
        }
        EmitSoppWord(code, 0x01u);  // s_endpgm

        std::vector<MemImgDecodedInst> insts;
        OS_REQUIRE(DecodeMemImgStream(code, insts).ok());

        Xs2ShaderContainer sc{};
        sc.stage = ShaderStage::Compute;
        sc.threadgroup_x = 16; sc.threadgroup_y = 1; sc.threadgroup_z = 1;
        sc.code_words = code;

        Rdna2LiftResult lifted{};
        auto lift_st = LiftMemImgShader(sc, insts, lifted, MakeStrictLiftOptions(true));
        if (!lift_st.ok()) {
            std::fprintf(stderr, "[FAIL] Test 2B LiftMemImgShader: %s\nSPV: %s\nDXC: %s\n",
                         lift_st.message.c_str(), lifted.spirv_val_diagnostics.c_str(), lifted.dxc_diagnostics.c_str());
        }
        OS_REQUIRE(lift_st.ok());
        OS_REQUIRE(ValidateSpirvBinary(lifted.spirv_words).ok());
        if (HasDxc()) {
            OS_REQUIRE(VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));
        }

        // Prepare input data (16 threads * 4 dwords = 64 dwords, total buffer = 160 dwords = 640 bytes)
        std::vector<uint32_t> in_u32(160u, 0u);
        for (uint32_t t = 0; t < 16u; ++t) {
            in_u32[t * 4u + 0u] = (t + 1u) * 3u;
            in_u32[t * 4u + 1u] = 0x100u + t;
            in_u32[t * 4u + 2u] = 0x200u + t * 5u;
            in_u32[t * 4u + 3u] = 0xA5A50000u | t;
        }
        std::vector<uint32_t> init_out(160u, 0u);

        // CPU Reference
        MemImgExecState cpu{};
        cpu.wave_size = 16;
        cpu.exec_mask = 0xFFFFull;
        SeedDefaultComputeVgprs(cpu);
        cpu.uav0 = U32ToBytes(in_u32);
        cpu.uav1 = U32ToBytes(init_out);
        OS_REQUIRE(ExecuteMemImgCpu(insts, cpu).ok());
        const auto cpu_out = BytesToU32(cpu.uav1);

#if defined(_WIN32)
        if (HasDxc() && hw_d3d12) {
            std::vector<uint32_t> gpu_out;
            hw_d3d12->RunCompute2Uav(lifted.pc_dxil, in_u32, init_out, gpu_out, 1);
            OS_REQUIRE(gpu_out == cpu_out);
            std::printf("  [LIVE] D3D12 HW (%s): MUBUF + atomics 100%% bit-exact parity!\n", hw_d3d12->adapter_name.c_str());
        }
        if (HasDxc() && warp_d3d12) {
            std::vector<uint8_t> warp_dxil = lifted.pc_dxil;
            if (warp_d3d12->highest_sm < static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
                Rdna2LiftOptions wopts = MakeStrictLiftOptions(true);
                wopts.emit_spirv = false;
                wopts.target_profile_override = "cs_6_2";
                Rdna2LiftResult wl{};
                OS_REQUIRE(LiftMemImgShader(sc, insts, wl, wopts).ok());
                OS_REQUIRE(VerifyDxbcContainerHash(wl.pc_dxil.data(), wl.pc_dxil.size()));
                warp_dxil = std::move(wl.pc_dxil);
            }
            std::vector<uint32_t> warp_out;
            warp_d3d12->RunCompute2Uav(warp_dxil, in_u32, init_out, warp_out, 1);
            OS_REQUIRE(warp_out == cpu_out);
            std::printf("  [LIVE] D3D12 WARP (%s, SM 0x%x): MUBUF + atomics 100%% bit-exact parity!\n",
                        warp_d3d12->adapter_name.c_str(), warp_d3d12->highest_sm);
        }
#endif
        if (vk_hw && vk_hw->IsInitialized() && !vk_hw->is_fallback_mode()) {
            std::vector<uint32_t> vk_out(160u, 0u);
            auto vst = vk_hw->ExecuteComputeShader(lifted.spirv_words, in_u32, vk_out, 1);
            OS_REQUIRE(vst.ok());
            OS_REQUIRE(vk_out == cpu_out);
            std::printf("  [LIVE] Vulkan HW (%s): MUBUF + atomics 100%% bit-exact parity!\n",
                        vk_hw->GetDeviceInfo().device_name.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Test 3: MIMG (2D/3D/Cube/Array/MSAA, Sample, Gather4, Resinfo, Atomics, BVH4)
// ---------------------------------------------------------------------------
void TestMimgAndBvhCpuAndLiveGpu(
#if defined(_WIN32)
    D3D12LiveHarness* hw_d3d12,
    D3D12LiveHarness* warp_d3d12,
#endif
    VulkanBackend* vk_hw) {
    std::printf("[TEST 3] MIMG (2D/3D/Cube/Array/MSAA/Sample/Gather4/Resinfo/Atomics) & BVH4 Ray Intersect\n");

    // 3A. CPU reference + SPIR-V/DXIL validation for 2D/3D/Cube/Array/MSAA + sample/gather4/resinfo/lod
    {
        std::vector<uint32_t> code;
        MemImgEncodeParams p{};
        p.dmask = 0xFu;
        // 2D load, 3D load, Cube sample_lz, 2DArray load, 2D gather4, 2D resinfo, 2D get_lod
        p.dim = kMimgDim2D; p.vdata = 10; p.vaddr = 4;
        OS_REQUIRE(EncodeMemImgInst("image_load", p, code));
        p.dim = kMimgDim3D; p.vdata = 14; p.vaddr = 4;
        OS_REQUIRE(EncodeMemImgInst("image_load", p, code));
        p.dim = kMimgDimCube; p.vdata = 18; p.vaddr = 8;
        OS_REQUIRE(EncodeMemImgInst("image_sample_lz", p, code));
        p.dim = kMimgDim2DArray; p.vdata = 22; p.vaddr = 4;
        OS_REQUIRE(EncodeMemImgInst("image_load", p, code));
        p.dim = kMimgDim2D; p.vdata = 26; p.vaddr = 8; p.dmask = 0x1u;
        OS_REQUIRE(EncodeMemImgInst("image_gather4", p, code));
        p.dim = kMimgDim2D; p.vdata = 30; p.vaddr = 4; p.dmask = 0xFu;
        OS_REQUIRE(EncodeMemImgInst("image_get_resinfo", p, code));
        p.dim = kMimgDim2D; p.vdata = 34; p.vaddr = 8; p.dmask = 0x3u;
        OS_REQUIRE(EncodeMemImgInst("image_get_lod", p, code));
        p.dim = kMimgDim2DMsaa; p.vdata = 36; p.vaddr = 4; p.dmask = 0xFu;
        OS_REQUIRE(EncodeMemImgInst("image_msaa_load", p, code));
        EmitSoppWord(code, 0x01u);

        std::vector<MemImgDecodedInst> insts;
        OS_REQUIRE(DecodeMemImgStream(code, insts).ok());

        MemImgExecState st{};
        st.wave_size = 4; st.exec_mask = 0xFull;
        st.textures[kMimgDim2D].AllocateRgba32f(kMimgDim2D, 4, 4, 1, 2);
        st.textures[kMimgDim2D].StoreRgba32f(1, 2, 0, {0.25f, 0.5f, 0.75f, 1.0f});
        st.textures[kMimgDim3D].AllocateRgba32f(kMimgDim3D, 4, 4, 4, 1);
        st.textures[kMimgDim3D].StoreRgba32f(1, 2, 3, {1.0f, 2.0f, 3.0f, 4.0f});
        st.textures[kMimgDimCube].AllocateRgba32f(kMimgDimCube, 4, 4, 6, 1);
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                st.textures[kMimgDimCube].StoreRgba32f(x, y, 0, {9.0f, 8.0f, 7.0f, 6.0f});
        st.textures[kMimgDim2DArray].AllocateRgba32f(kMimgDim2DArray, 4, 4, 4, 1);
        st.textures[kMimgDim2DArray].StoreRgba32f(1, 2, 3, {0.1f, 0.2f, 0.3f, 0.4f});
        st.textures[kMimgDim2DMsaa].AllocateRgba32f(kMimgDim2DMsaa, 4, 4, 1, 1, 4);
        st.textures[kMimgDim2DMsaa].StoreRgba32f(1, 2, 0, {0.9f, 0.8f, 0.7f, 0.6f}, 0, 3);

        for (uint32_t l = 0; l < 4u; ++l) {
            st.vgpr[l][4] = 1; st.vgpr[l][5] = 2; st.vgpr[l][6] = 3;  // integer coords (1,2,3)
            st.vgpr[l][8] = FloatToBits(1.0f); st.vgpr[l][9] = FloatToBits(0.0f); st.vgpr[l][10] = FloatToBits(0.0f);
        }
        OS_REQUIRE(ExecuteMemImgCpu(insts, st).ok());
        OS_REQUIRE(std::fabs(BitsToFloat(st.vgpr[0][10]) - 0.25f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(st.vgpr[0][14]) - 1.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(st.vgpr[0][18]) - 9.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(st.vgpr[0][22]) - 0.1f) < 1e-5f);
        OS_REQUIRE(st.vgpr[0][30] == 2u && st.vgpr[0][31] == 2u && st.vgpr[0][33] == 2u);  // mip1=2x2, num_mips=2
        OS_REQUIRE(std::fabs(BitsToFloat(st.vgpr[0][36]) - 0.9f) < 1e-5f);

        Xs2ShaderContainer sc{};
        sc.stage = ShaderStage::Compute; sc.threadgroup_x = 4; sc.code_words = code;
        Rdna2LiftResult lifted{};
        auto st_lift = LiftMemImgShader(sc, insts, lifted, MakeStrictLiftOptions(true));
        if (!st_lift.ok()) {
            std::fprintf(stderr, "[FAIL] Test 3A lift: %s\nSPV: %s\nDXC: %s\n",
                         st_lift.message.c_str(), lifted.spirv_val_diagnostics.c_str(), lifted.dxc_diagnostics.c_str());
        }
        OS_REQUIRE(st_lift.ok());
        if (HasDxc()) OS_REQUIRE(VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));
        std::printf("  [OK] MIMG multi-dim (2D/3D/Cube/Array/MSAA/Gather4/Resinfo/LOD) CPU + SPIR-V + DXIL validated\n");
    }

    // 3B. Live GPU Texture 2D image_load + image_store + image_atomic_add
    {
        std::vector<uint32_t> code;
        // v0 = tid (0..15), v4 = 0 -> coord (v0, v4) = (x, 0)
        // Wait: v[0] = tid, v[1] = tid*4, v[4] = 0 -> let's use v[0] as x and v[4] (0) as y!
        // Actually in MIMG vaddr = 0 reads v[0] (x) and v[1] (y=tid*4) -- better: load at (v[4]=0, v[5]=0) or
        // let's make v[0]=tid, use image_load with vaddr=0 (wait: v[1]=tid*4 would exceed 16 if tid>=4!).
        // Even better: with 4 threads (tid = 0..3), v[0]=tid (0..3), v[1]=tid*4 (0,4,8,12) — ALL strictly inside 16x16!!
        MemImgEncodeParams p{};
        p.dim = kMimgDim2D; p.dmask = 0xFu; p.vaddr = 0; p.vdata = 10;
        OS_REQUIRE(EncodeMemImgInst("image_load", p, code));
        OS_REQUIRE(EncodeMemImgInst("image_store", p, code));
        p.vdata = 0; p.glc = true;  // add v[0] (tid) to rw_tex_u32[(tid, tid*4)]
        OS_REQUIRE(EncodeMemImgInst("image_atomic_add", p, code));
        EmitSoppWord(code, 0x01u);

        std::vector<MemImgDecodedInst> insts;
        OS_REQUIRE(DecodeMemImgStream(code, insts).ok());

        Xs2ShaderContainer sc{};
        sc.stage = ShaderStage::Compute; sc.threadgroup_x = 4; sc.code_words = code;
        Rdna2LiftResult lifted{};
        auto st_lift = LiftMemImgShader(sc, insts, lifted, MakeStrictLiftOptions(true));
        if (!st_lift.ok()) {
            std::fprintf(stderr, "[FAIL] Test 3B lift: %s\nSPV: %s\nDXC: %s\n",
                         st_lift.message.c_str(), lifted.spirv_val_diagnostics.c_str(), lifted.dxc_diagnostics.c_str());
        }
        OS_REQUIRE(st_lift.ok());

        std::vector<float> in_rgba(16 * 16 * 4, 0.0f);
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                const size_t idx = (y * 16 + x) * 4;
                in_rgba[idx + 0] = static_cast<float>(x) * 0.25f + 1.0f;
                in_rgba[idx + 1] = static_cast<float>(y) * 0.125f + 0.5f;
                in_rgba[idx + 2] = 0.75f;
                in_rgba[idx + 3] = 1.0f;
            }
        }

        MemImgExecState cpu{};
        cpu.wave_size = 4; cpu.exec_mask = 0xFull;
        SeedDefaultComputeVgprs(cpu);
        cpu.textures[kMimgDim2D].AllocateRgba32f(kMimgDim2D, 16, 16);
        cpu.textures[kMimgDim2D].mips[0].rgba32f = in_rgba;
        cpu.rw_texture.AllocateRgba32f(kMimgDim2D, 16, 16);
        cpu.rw_texture.mips[0].r32u.resize(16 * 16, 0u);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
                cpu.rw_texture.StoreR32u(x, y, 0, 100u + x);
        OS_REQUIRE(ExecuteMemImgCpu(insts, cpu).ok());

#if defined(_WIN32)
        if (HasDxc() && hw_d3d12) {
            std::vector<float> gpu_f4; std::vector<uint32_t> gpu_u32;
            hw_d3d12->RunComputeTex2DLoadStore(lifted.pc_dxil, in_rgba, gpu_f4, gpu_u32);
            for (uint32_t t = 0; t < 4; ++t) {
                const uint32_t x = t, y = t * 4;
                const auto exp_c = cpu.rw_texture.LoadRgba32f(x, y, 0);
                const size_t idx = (y * 16 + x) * 4;
                for (int c = 0; c < 4; ++c) OS_REQUIRE(std::fabs(gpu_f4[idx + c] - exp_c[c]) < 1e-5f);
                OS_REQUIRE(gpu_u32[y * 16 + x] == cpu.rw_texture.LoadR32u(x, y, 0));
            }
            std::printf("  [LIVE] D3D12 HW (%s): MIMG image_load + image_store + image_atomic_add parity!\n",
                        hw_d3d12->adapter_name.c_str());
        }
        if (HasDxc() && warp_d3d12) {
            std::vector<uint8_t> warp_dxil = lifted.pc_dxil;
            if (warp_d3d12->highest_sm < static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
                Rdna2LiftOptions wopts = MakeStrictLiftOptions(true);
                wopts.emit_spirv = false;
                wopts.target_profile_override = "cs_6_2";
                Rdna2LiftResult wl{};
                OS_REQUIRE(LiftMemImgShader(sc, insts, wl, wopts).ok());
                warp_dxil = std::move(wl.pc_dxil);
            }
            std::vector<float> warp_f4; std::vector<uint32_t> warp_u32;
            warp_d3d12->RunComputeTex2DLoadStore(warp_dxil, in_rgba, warp_f4, warp_u32);
            for (uint32_t t = 0; t < 4; ++t) {
                const uint32_t x = t, y = t * 4;
                const auto exp_c = cpu.rw_texture.LoadRgba32f(x, y, 0);
                const size_t idx = (y * 16 + x) * 4;
                for (int c = 0; c < 4; ++c) OS_REQUIRE(std::fabs(warp_f4[idx + c] - exp_c[c]) < 1e-5f);
                OS_REQUIRE(warp_u32[y * 16 + x] == cpu.rw_texture.LoadR32u(x, y, 0));
            }
            std::printf("  [LIVE] D3D12 WARP (%s): MIMG image_load + image_store + image_atomic_add parity!\n",
                        warp_d3d12->adapter_name.c_str());
        }
#endif
    }

    // 3C. Live GPU BVH4 Ray-Triangle Intersection (image_bvh_intersect_ray) on D3D12 HW, WARP, & Vulkan HW!
    {
        std::vector<uint32_t> code;
        // Load ray descriptor (node_ptr, tmax, ox, oy, oz, dx, dy, dz) from uav0 at offset 64 + tid*32 (two dwordx4 loads!)
        // Wait: threadgroup_x = 4, v[2] = tid*16; let's put rays at uav0[64..192] using 2x buffer_load_dwordx4 with vaddr=2 (wait: stride 32 -> put 1 ray per lane using vaddr=2 -> stride 16? No, 8 dwords = 32 bytes; let's test 1 primary ray per lane where thread 0 reads uav0[64..96] -> v[10..17]!)
        // Better: load v[10..13] from uav0[64 + v[2]*2]? We don't have ALU here, so with 4 threads:
        // Put ray0 at uav0[64..96] (vaddr=0, offen=false, offset=64 -> v[10..13], offset=80 -> v[14..17]),
        // OR put 4 triangles at uav0[0..192] (node_ptr = 0,1,2,3 = v[0]!!), and shared ray at uav0[256..288]!
        // That tests 4 DIFFERENT triangles across 4 lanes in parallel with ZERO ALU needed!!
        {
            MemImgEncodeParams p{};
            p.srsrc_sgpr = 0; p.offen = false;
            p.offset = 256; p.vdata = 11;  // v[11]=tmax, v[12..14]=orig
            OS_REQUIRE(EncodeMemImgInst("buffer_load_dwordx4", p, code));
            p.offset = 272; p.vdata = 15;  // v[15..17]=dir
            OS_REQUIRE(EncodeMemImgInst("buffer_load_dwordx4", p, code));
            // Copy v[0] (lane = node_ptr 0..3) into v[10] via scratch store/load!
            MemImgEncodeParams sc_s{}; sc_s.vaddr = 1; sc_s.vdata = 0;
            OS_REQUIRE(EncodeMemImgInst("scratch_store_dword", sc_s, code));
            MemImgEncodeParams sc_l{}; sc_l.vaddr = 1; sc_l.vdst = 10;
            OS_REQUIRE(EncodeMemImgInst("scratch_load_dword", sc_l, code));
            // image_bvh_intersect_ray v[20..23], v[10..17]
            MemImgEncodeParams bvh{};
            bvh.vdata = 20; bvh.vaddr = 10; bvh.dmask = 0xFu;
            OS_REQUIRE(EncodeMemImgInst("image_bvh_intersect_ray", bvh, code));
            // Store hit result (t, u, v, hit_id) to uav1 at v[2] (tid * 16)
            MemImgEncodeParams stp{};
            stp.srsrc_sgpr = 4; stp.offen = true; stp.vaddr = 2; stp.vdata = 20;
            OS_REQUIRE(EncodeMemImgInst("buffer_store_dwordx4", stp, code));
        }
        EmitSoppWord(code, 0x01u);

        std::vector<MemImgDecodedInst> insts;
        OS_REQUIRE(DecodeMemImgStream(code, insts).ok());

        Xs2ShaderContainer sc{};
        sc.stage = ShaderStage::Compute; sc.threadgroup_x = 4; sc.code_words = code;
        Rdna2LiftResult lifted{};
        auto st_lift = LiftMemImgShader(sc, insts, lifted, MakeStrictLiftOptions(true));
        if (!st_lift.ok()) {
            std::fprintf(stderr, "[FAIL] Test 3C BVH lift: %s\nSPV: %s\nDXC: %s\n",
                         st_lift.message.c_str(), lifted.spirv_val_diagnostics.c_str(), lifted.dxc_diagnostics.c_str());
        }
        OS_REQUIRE(st_lift.ok());

        // Populate 4 triangles (12 dwords each) in uav0[0..47] + ray at uav0[64..71] (bytes 256..287)
        std::vector<uint32_t> in_u32(80u, 0u);
        auto set_tri = [&](uint32_t idx, float z, float x_shift, uint32_t prim_id) {
            const uint32_t b = idx * 12u;
            in_u32[b + 0] = FloatToBits(-1.0f + x_shift); in_u32[b + 1] = FloatToBits(-1.0f); in_u32[b + 2] = FloatToBits(z);
            in_u32[b + 3] = FloatToBits( 1.0f + x_shift); in_u32[b + 4] = FloatToBits(-1.0f); in_u32[b + 5] = FloatToBits(z);
            in_u32[b + 6] = FloatToBits( 0.0f + x_shift); in_u32[b + 7] = FloatToBits( 1.0f); in_u32[b + 8] = FloatToBits(z);
            in_u32[b + 9] = prim_id;
        };
        set_tri(0, 5.0f, 0.0f, 101u);   // Hit at t=5.0
        set_tri(1, 2.5f, 0.0f, 202u);   // Hit at t=2.5
        set_tri(2, 5.0f, 10.0f, 303u);  // Miss (shifted far right)
        set_tri(3, 8.0f, 0.0f, 404u);   // Hit at t=8.0
        // Ray: tmax=100, orig=(0,0,0), dir=(0,0,1)
        in_u32[64] = FloatToBits(100.0f);
        in_u32[65] = FloatToBits(0.0f); in_u32[66] = FloatToBits(0.0f); in_u32[67] = FloatToBits(0.0f);
        in_u32[68] = FloatToBits(0.0f); in_u32[69] = FloatToBits(0.0f); in_u32[70] = FloatToBits(1.0f);

        std::vector<uint32_t> init_out(80u, 0u);
        MemImgExecState cpu{};
        cpu.wave_size = 4; cpu.exec_mask = 0xFull;
        SeedDefaultComputeVgprs(cpu);
        cpu.uav0 = U32ToBytes(in_u32);
        cpu.uav1 = U32ToBytes(init_out);
        OS_REQUIRE(ExecuteMemImgCpu(insts, cpu).ok());
        const auto cpu_out = BytesToU32(cpu.uav1);

        // Verify expected hits/misses
        OS_REQUIRE(std::fabs(BitsToFloat(cpu_out[0]) - 5.0f) < 1e-4f && cpu_out[3] == (101u | 0x80000000u));
        OS_REQUIRE(std::fabs(BitsToFloat(cpu_out[4]) - 2.5f) < 1e-4f && cpu_out[7] == (202u | 0x80000000u));
        OS_REQUIRE(cpu_out[11] == 0u);  // miss
        OS_REQUIRE(std::fabs(BitsToFloat(cpu_out[12]) - 8.0f) < 1e-4f && cpu_out[15] == (404u | 0x80000000u));

        auto check_bvh_parity = [&](const std::vector<uint32_t>& got) {
            for (uint32_t t = 0; t < 4u; ++t) {
                for (uint32_t c = 0; c < 3u; ++c) {
                    OS_REQUIRE(std::fabs(BitsToFloat(got[t * 4u + c]) - BitsToFloat(cpu_out[t * 4u + c])) < 1e-4f);
                }
                OS_REQUIRE(got[t * 4u + 3u] == cpu_out[t * 4u + 3u]);
            }
        };

#if defined(_WIN32)
        if (HasDxc() && hw_d3d12) {
            std::vector<uint32_t> gpu_out;
            hw_d3d12->RunCompute2Uav(lifted.pc_dxil, in_u32, init_out, gpu_out, 1);
            check_bvh_parity(gpu_out);
            std::printf("  [LIVE] D3D12 HW (%s): image_bvh_intersect_ray 4-lane parity!\n", hw_d3d12->adapter_name.c_str());
        }
        if (HasDxc() && warp_d3d12) {
            std::vector<uint8_t> warp_dxil = lifted.pc_dxil;
            if (warp_d3d12->highest_sm < static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
                Rdna2LiftOptions wopts = MakeStrictLiftOptions(true);
                wopts.emit_spirv = false;
                wopts.target_profile_override = "cs_6_2";
                Rdna2LiftResult wl{};
                OS_REQUIRE(LiftMemImgShader(sc, insts, wl, wopts).ok());
                warp_dxil = std::move(wl.pc_dxil);
            }
            std::vector<uint32_t> warp_out;
            warp_d3d12->RunCompute2Uav(warp_dxil, in_u32, init_out, warp_out, 1);
            check_bvh_parity(warp_out);
            std::printf("  [LIVE] D3D12 WARP (%s): image_bvh_intersect_ray 4-lane parity!\n", warp_d3d12->adapter_name.c_str());
        }
#endif
        if (vk_hw && vk_hw->IsInitialized() && !vk_hw->is_fallback_mode()) {
            std::vector<uint32_t> vk_out(80u, 0u);
            OS_REQUIRE(vk_hw->ExecuteComputeShader(lifted.spirv_words, in_u32, vk_out, 1).ok());
            check_bvh_parity(vk_out);
            std::printf("  [LIVE] Vulkan HW (%s): image_bvh_intersect_ray 4-lane parity!\n",
                        vk_hw->GetDeviceInfo().device_name.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Test 4: DS (LDS, Read2/Write2, Atomics, Swizzle, Permute, Bpermute, Append/Consume)
//         + FLAT / GLOBAL / SCRATCH + EXP (All Targets & Flags)
// ---------------------------------------------------------------------------
void TestDsFlatGlobalScratchExpCpuAndLiveGpu(
#if defined(_WIN32)
    D3D12LiveHarness* hw_d3d12,
    D3D12LiveHarness* warp_d3d12,
#endif
    VulkanBackend* vk_hw) {
    std::printf("[TEST 4] DS (LDS/Atomics/Swizzle/Permute/Append), FLAT/GLOBAL/SCRATCH & EXP\n");

    // 4A. Live GPU parity shader: DS write_b32/read_b32 + write2/read2 + ds_add_u32 + ds_swizzle_b32 + ds_bpermute_b32 + ds_append + global/flat/scratch
    {
        std::vector<uint32_t> code;
        // 1. global_load_dword v[10] from uav0 at v[1] (tid*4)
        {
            MemImgEncodeParams p{}; p.vaddr = 1; p.vdst = 10; p.saddr = 0x7Fu;
            OS_REQUIRE(EncodeMemImgInst("global_load_dword", p, code));
        }
        // 2. ds_write_b32 lds[v[1]] = v[10], barrier, ds_read_b32 v[11] = lds[v[1]]
        {
            MemImgEncodeParams p{}; p.vaddr = 1; p.data0 = 10;
            OS_REQUIRE(EncodeMemImgInst("ds_write_b32", p, code));
            EmitSoppWord(code, 0x0Au);  // s_barrier
            p.vdst = 11;
            OS_REQUIRE(EncodeMemImgInst("ds_read_b32", p, code));
        }
        // 3. ds_swizzle_b32 (quad-perm reverse within each quad: 0x801B -> [3,2,1,0]) on v[11] -> v[12]
        {
            MemImgEncodeParams p{}; p.vdst = 12; p.vaddr = 11; p.offset = 0x801B;
            OS_REQUIRE(EncodeMemImgInst("ds_swizzle_b32", p, code));
        }
        // 4. ds_bpermute_b32: gather v[11] using lane index addr=v[1] (identity tid*4 -> tid) -> v[13]
        {
            MemImgEncodeParams p{}; p.vdst = 13; p.vaddr = 1; p.data0 = 11;
            OS_REQUIRE(EncodeMemImgInst("ds_bpermute_b32", p, code));
        }
        // 5. ds_add_u32 all 16 lanes' v[10] into lds[1024], barrier, ds_read_b32 v[14] = lds[1024]
        {
            MemImgEncodeParams p{}; p.vaddr = 60; p.data0 = 10; p.offset = 1024;  // v[60] == 0
            OS_REQUIRE(EncodeMemImgInst("ds_add_u32", p, code));
            EmitSoppWord(code, 0x0Au);  // s_barrier
            p.vdst = 14;
            OS_REQUIRE(EncodeMemImgInst("ds_read_b32", p, code));
        }
        // 6. scratch_store_dword & scratch_load_dword round-trip of v[12] -> v[15]
        {
            MemImgEncodeParams p{}; p.vaddr = 60; p.vdata = 12; p.offset = 16;
            OS_REQUIRE(EncodeMemImgInst("scratch_store_dword", p, code));
            p.vdst = 15;
            OS_REQUIRE(EncodeMemImgInst("scratch_load_dword", p, code));
        }
        // 7. Store v[12..15] (swizzle, bpermute, lds_sum, scratch_roundtrip) to uav1 via global_store_dwordx4 (saddr=1)
        {
            MemImgEncodeParams p{}; p.vaddr = 2; p.vdata = 12; p.saddr = 1;
            OS_REQUIRE(EncodeMemImgInst("global_store_dwordx4", p, code));
        }
        EmitSoppWord(code, 0x01u);

        std::vector<MemImgDecodedInst> insts;
        OS_REQUIRE(DecodeMemImgStream(code, insts).ok());

        Xs2ShaderContainer sc{};
        sc.stage = ShaderStage::Compute; sc.threadgroup_x = 16; sc.code_words = code;
        Rdna2LiftResult lifted{};
        auto st_lift = LiftMemImgShader(sc, insts, lifted, MakeStrictLiftOptions(true));
        if (!st_lift.ok()) {
            std::fprintf(stderr, "[FAIL] Test 4A lift: %s\nSPV: %s\nDXC: %s\n",
                         st_lift.message.c_str(), lifted.spirv_val_diagnostics.c_str(), lifted.dxc_diagnostics.c_str());
        }
        OS_REQUIRE(st_lift.ok());
        OS_REQUIRE(ValidateSpirvBinary(lifted.spirv_words).ok());
        if (HasDxc()) OS_REQUIRE(VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));

        std::vector<uint32_t> in_u32(64u, 0u);
        for (uint32_t t = 0; t < 16u; ++t) in_u32[t] = (t + 1u) * 10u;
        std::vector<uint32_t> init_out(64u, 0u);

        MemImgExecState cpu{};
        cpu.wave_size = 16; cpu.exec_mask = 0xFFFFull;
        SeedDefaultComputeVgprs(cpu);
        cpu.uav0 = U32ToBytes(in_u32);
        cpu.uav1 = U32ToBytes(init_out);
        OS_REQUIRE(ExecuteMemImgCpu(insts, cpu).ok());
        const auto cpu_out = BytesToU32(cpu.uav1);

        // Verify expected swizzle (lane 0 gets lane 3 = 40), bpermute (=10), sum (10+20+..+160 = 1360), scratch (=40)
        OS_REQUIRE(cpu_out[0] == 40u && cpu_out[1] == 10u && cpu_out[2] == 1360u && cpu_out[3] == 40u);

#if defined(_WIN32)
        if (HasDxc() && hw_d3d12) {
            std::vector<uint32_t> gpu_out;
            hw_d3d12->RunCompute2Uav(lifted.pc_dxil, in_u32, init_out, gpu_out, 1);
            OS_REQUIRE(gpu_out == cpu_out);
            std::printf("  [LIVE] D3D12 HW (%s): DS + Swizzle + Bpermute + LDS Atomic + Scratch + Global parity!\n",
                        hw_d3d12->adapter_name.c_str());
        }
        if (HasDxc() && warp_d3d12) {
            std::vector<uint8_t> warp_dxil = lifted.pc_dxil;
            if (warp_d3d12->highest_sm < static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
                Rdna2LiftOptions wopts = MakeStrictLiftOptions(true);
                wopts.emit_spirv = false;
                wopts.target_profile_override = "cs_6_2";
                Rdna2LiftResult wl{};
                OS_REQUIRE(LiftMemImgShader(sc, insts, wl, wopts).ok());
                warp_dxil = std::move(wl.pc_dxil);
            }
            std::vector<uint32_t> warp_out;
            warp_d3d12->RunCompute2Uav(warp_dxil, in_u32, init_out, warp_out, 1);
            OS_REQUIRE(warp_out == cpu_out);
            std::printf("  [LIVE] D3D12 WARP (%s): DS + Swizzle + Bpermute + LDS Atomic + Scratch + Global parity!\n",
                        warp_d3d12->adapter_name.c_str());
        }
#endif
        if (vk_hw && vk_hw->IsInitialized() && !vk_hw->is_fallback_mode()) {
            std::vector<uint32_t> vk_out(64u, 0u);
            OS_REQUIRE(vk_hw->ExecuteComputeShader(lifted.spirv_words, in_u32, vk_out, 1).ok());
            OS_REQUIRE(vk_out == cpu_out);
            std::printf("  [LIVE] Vulkan HW (%s): DS + Swizzle + Bpermute + LDS Atomic + Scratch + Global parity!\n",
                        vk_hw->GetDeviceInfo().device_name.c_str());
        }
    }

    // 4B. DS b8/b16/b64/b96/b128, write2/read2, write2st64/read2st64, permute, append/consume, cmpst_rtn CPU reference
    {
        std::vector<uint32_t> code;
        MemImgEncodeParams p{};
        // ds_write_b8 / ds_read_i8 & u8
        p.vaddr = 1; p.data0 = 10; p.offset = 0;
        OS_REQUIRE(EncodeMemImgInst("ds_write_b8", p, code));
        p.vdst = 20; OS_REQUIRE(EncodeMemImgInst("ds_read_u8", p, code));
        p.vdst = 21; OS_REQUIRE(EncodeMemImgInst("ds_read_i8", p, code));
        // ds_write_b128 / ds_read_b128
        p.vaddr = 2; p.data0 = 12; p.offset = 256;
        OS_REQUIRE(EncodeMemImgInst("ds_write_b128", p, code));
        p.vdst = 24; OS_REQUIRE(EncodeMemImgInst("ds_read_b128", p, code));
        // ds_write2_b32 / ds_read2_b32
        p.vaddr = 60; p.data0 = 12; p.data1 = 13; p.offset = 0; p.offset0 = 1; p.offset1 = 3;
        OS_REQUIRE(EncodeMemImgInst("ds_write2_b32", p, code));
        p.vdst = 28; OS_REQUIRE(EncodeMemImgInst("ds_read2_b32", p, code));
        // ds_write2st64_b32 / ds_read2st64_b32
        p.offset0 = 2; p.offset1 = 4;
        OS_REQUIRE(EncodeMemImgInst("ds_write2st64_b32", p, code));
        p.vdst = 30; OS_REQUIRE(EncodeMemImgInst("ds_read2st64_b32", p, code));
        // ds_append & ds_consume
        p.vdst = 32; OS_REQUIRE(EncodeMemImgInst("ds_append", p, code));
        p.vdst = 33; OS_REQUIRE(EncodeMemImgInst("ds_consume", p, code));
        // ds_permute_b32
        p.vdst = 34; p.vaddr = 1; p.data0 = 12;
        OS_REQUIRE(EncodeMemImgInst("ds_permute_b32", p, code));
        // ds_cmpst_rtn_b32
        p.vaddr = 60; p.offset = 2048; p.data0 = 0; p.data1 = 12; p.vdst = 35;
        OS_REQUIRE(EncodeMemImgInst("ds_cmpst_rtn_b32", p, code));

        std::vector<MemImgDecodedInst> insts;
        OS_REQUIRE(DecodeMemImgStream(code, insts).ok());
        MemImgExecState st{};
        st.wave_size = 4; st.exec_mask = 0xFull;
        SeedDefaultComputeVgprs(st);
        for (uint32_t l = 0; l < 4u; ++l) {
            st.vgpr[l][10] = 0xF0u;
            st.vgpr[l][12] = 0x11110000u + l;
            st.vgpr[l][13] = 0x22220000u + l;
            st.vgpr[l][14] = 0x33330000u + l;
            st.vgpr[l][15] = 0x44440000u + l;
        }
        OS_REQUIRE(ExecuteMemImgCpu(insts, st).ok());
        OS_REQUIRE(st.vgpr[0][20] == 0xF0u && static_cast<int32_t>(st.vgpr[0][21]) == -16);
        OS_REQUIRE(st.vgpr[1][24] == 0x11110001u && st.vgpr[1][27] == 0x44440001u);
        OS_REQUIRE(st.vgpr[0][28] == 0x11110003u && st.vgpr[0][29] == 0x22220003u);
        OS_REQUIRE(st.vgpr[0][30] == 0x11110003u && st.vgpr[0][31] == 0x22220003u);
        OS_REQUIRE(st.vgpr[0][32] == 0u && st.vgpr[3][32] == 3u && st.append_consume_counter == 0u);
        OS_REQUIRE(st.vgpr[2][34] == 0x11110002u);
        OS_REQUIRE(st.vgpr[0][35] == 0u && st.vgpr[1][35] == 0x11110000u);
        std::printf("  [OK] DS b8/b16/b128/read2/write2/st64/permute/append/consume/cmpst_rtn verified\n");
    }

    // 4C. EXP all targets (pos0..3, param0..31, prim, mrt0..7, mrtz, null, compr, vm, done) + VS & PS SPIR-V/DXIL lift
    {
        // Vertex shader exporting pos0, param0, param1, prim
        std::vector<uint32_t> vs_code;
        MemImgEncodeParams ep{};
        ep.exp_vsrc[0] = 4; ep.exp_vsrc[1] = 5; ep.exp_vsrc[2] = 6; ep.exp_vsrc[3] = 7;
        ep.exp_en = 0xFu;
        ep.exp_target = 12; OS_REQUIRE(EncodeMemImgInst("exp", ep, vs_code));  // pos0
        ep.exp_target = 32; OS_REQUIRE(EncodeMemImgInst("exp", ep, vs_code));  // param0
        ep.exp_target = 33; OS_REQUIRE(EncodeMemImgInst("exp", ep, vs_code));  // param1
        ep.exp_target = 20; ep.exp_done = true; OS_REQUIRE(EncodeMemImgInst("exp", ep, vs_code));  // prim
        EmitSoppWord(vs_code, 0x01u);

        std::vector<MemImgDecodedInst> vs_insts;
        OS_REQUIRE(DecodeMemImgStream(vs_code, vs_insts).ok());
        Xs2ShaderContainer vs_sc{}; vs_sc.stage = ShaderStage::Vertex; vs_sc.code_words = vs_code;
        Rdna2LiftResult vs_lift{};
        auto st_vs = LiftMemImgShader(vs_sc, vs_insts, vs_lift, MakeStrictLiftOptions(true));
        if (!st_vs.ok()) {
            std::fprintf(stderr, "[FAIL] VS EXP lift: %s\nSPV: %s\nDXC: %s\n",
                         st_vs.message.c_str(), vs_lift.spirv_val_diagnostics.c_str(), vs_lift.dxc_diagnostics.c_str());
        }
        OS_REQUIRE(st_vs.ok());
        if (HasDxc()) OS_REQUIRE(VerifyDxbcContainerHash(vs_lift.pc_dxil.data(), vs_lift.pc_dxil.size()));

        // Pixel shader exporting mrt0 (compr fp16), mrt1, mrtz, null with vm + done
        std::vector<uint32_t> ps_code;
        MemImgEncodeParams pp{};
        pp.exp_vsrc[0] = 4; pp.exp_vsrc[1] = 5; pp.exp_vsrc[2] = 6; pp.exp_vsrc[3] = 7;
        pp.exp_en = 0xFu;
        pp.exp_target = 0; pp.exp_compr = true; OS_REQUIRE(EncodeMemImgInst("exp", pp, ps_code));  // mrt0 compr
        pp.exp_target = 1; pp.exp_compr = false; OS_REQUIRE(EncodeMemImgInst("exp", pp, ps_code));  // mrt1
        pp.exp_target = 8; OS_REQUIRE(EncodeMemImgInst("exp", pp, ps_code));  // mrtz
        pp.exp_target = 9; pp.exp_vm = true; pp.exp_done = true; OS_REQUIRE(EncodeMemImgInst("exp", pp, ps_code));  // null
        EmitSoppWord(ps_code, 0x01u);

        std::vector<MemImgDecodedInst> ps_insts;
        OS_REQUIRE(DecodeMemImgStream(ps_code, ps_insts).ok());
        Xs2ShaderContainer ps_sc{}; ps_sc.stage = ShaderStage::Pixel; ps_sc.code_words = ps_code;
        Rdna2LiftResult ps_lift{};
        auto st_ps = LiftMemImgShader(ps_sc, ps_insts, ps_lift, MakeStrictLiftOptions(true));
        if (!st_ps.ok()) {
            std::fprintf(stderr, "[FAIL] PS EXP lift: %s\nSPV: %s\nDXC: %s\n",
                         st_ps.message.c_str(), ps_lift.spirv_val_diagnostics.c_str(), ps_lift.dxc_diagnostics.c_str());
        }
        OS_REQUIRE(st_ps.ok());
        if (HasDxc()) OS_REQUIRE(VerifyDxbcContainerHash(ps_lift.pc_dxil.data(), ps_lift.pc_dxil.size()));

        // CPU reference verify compr FP16 unpack (0x3C00 = 1.0f, 0x3800 = 0.5f)
        MemImgExecState est{}; est.wave_size = 2; est.exec_mask = 3ull;
        est.vgpr[0][4] = (0x3800u << 16) | 0x3C00u;  // (1.0, 0.5)
        est.vgpr[0][5] = (0x4000u << 16) | 0x0000u;  // (0.0, 2.0)
        est.vgpr[0][6] = FloatToBits(0.25f);
        est.vgpr[0][7] = FloatToBits(1.0f);
        OS_REQUIRE(ExecuteMemImgCpu(ps_insts, est).ok());
        OS_REQUIRE(std::fabs(est.exports[0].mrt[0][0] - 1.0f) < 1e-4f);
        OS_REQUIRE(std::fabs(est.exports[0].mrt[0][1] - 0.5f) < 1e-4f);
        OS_REQUIRE(std::fabs(est.exports[0].mrt[0][2] - 0.0f) < 1e-4f);
        OS_REQUIRE(std::fabs(est.exports[0].mrt[0][3] - 2.0f) < 1e-4f);
        OS_REQUIRE(est.exports[0].mrtz_written && est.exports[0].null_exported && est.exports[0].done_seen && est.exports[0].last_vm);
        std::printf("  [OK] EXP (pos0..3, param0..31, prim, mrt0..7, mrtz, null, compr/vm/done) CPU + SPIR-V + DXIL validated\n");
    }
}

}  // namespace

int main() {
    std::printf("=== OpenScarlett Sprint 8 Worker 2: GFX10.3 Mem/Img/LDS/FLAT/EXP Test Suite ===\n");
    std::printf("  External spirv-val.exe: %s\n", HasExternalSpirvVal() ? "FOUND" : "not found");
    std::printf("  External dxc.exe:       %s\n", HasDxc() ? "FOUND" : "not found");

    TestCompleteOpcodeTableRoundTrip();

#if defined(_WIN32)
    D3D12LiveHarness hw_d3d12{};
    D3D12LiveHarness warp_d3d12{};
    const bool has_hw_d3d12 = hw_d3d12.Init(false);
    const bool has_warp_d3d12 = warp_d3d12.Init(true);
    std::printf("  D3D12 HW Adapter:   %s\n", has_hw_d3d12 ? hw_d3d12.adapter_name.c_str() : "unavailable");
    std::printf("  D3D12 WARP Adapter: %s\n", has_warp_d3d12 ? warp_d3d12.adapter_name.c_str() : "unavailable");
    OS_REQUIRE(has_warp_d3d12);
#endif

    VulkanBackend vk{};
    const bool vk_ok = vk.Initialize(true).ok();
    std::printf("  Vulkan Backend:     %s (%s)\n",
                vk_ok ? vk.GetDeviceInfo().device_name.c_str() : "unavailable",
                (vk_ok && !vk.is_fallback_mode()) ? "LIVE HARDWARE ICD" : "software fallback");

    TestMubufMtbufCpuAndLiveGpu(
#if defined(_WIN32)
        has_hw_d3d12 ? &hw_d3d12 : nullptr,
        has_warp_d3d12 ? &warp_d3d12 : nullptr,
#endif
        vk_ok ? &vk : nullptr);

    TestMimgAndBvhCpuAndLiveGpu(
#if defined(_WIN32)
        has_hw_d3d12 ? &hw_d3d12 : nullptr,
        has_warp_d3d12 ? &warp_d3d12 : nullptr,
#endif
        vk_ok ? &vk : nullptr);

    TestDsFlatGlobalScratchExpCpuAndLiveGpu(
#if defined(_WIN32)
        has_hw_d3d12 ? &hw_d3d12 : nullptr,
        has_warp_d3d12 ? &warp_d3d12 : nullptr,
#endif
        vk_ok ? &vk : nullptr);

    std::printf("=== ALL TESTS PASSED (unique mnemonics = %zu) ===\n", GetUniqueMemImgMnemonicCount());
    return 0;
}
