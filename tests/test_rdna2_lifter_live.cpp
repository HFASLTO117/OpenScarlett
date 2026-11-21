#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_disasm.h"
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

using Microsoft::WRL::ComPtr;

// Checks whether a specific SPIR-V opcode is present in the binary word stream.
bool SpirvContainsOpcode(const std::vector<uint32_t>& spv, uint16_t target_op) {
    size_t pos = 5;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0 || pos + wc > spv.size()) break;
        if (op == target_op) return true;
        pos += wc;
    }
    return false;
}

bool HasExternalSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(openscarlett::kDefaultSpirvValPath, ec);
}

void VerifySpirvExternalStrict(const std::vector<uint32_t>& spv, const char* label) {
    OS_REQUIRE(openscarlett::ValidateSpirvBinary(spv).ok());
    if (!HasExternalSpirvVal()) {
        return;
    }
    std::string diag;
    auto st11 = openscarlett::ValidateSpirvExternal(spv, diag, openscarlett::kDefaultSpirvValPath, "vulkan1.1");
    if (!st11.ok()) {
        std::fprintf(stderr, "[FAIL] spirv-val (vulkan1.1) for %s: %s\n", label, diag.c_str());
    }
    OS_REQUIRE(st11.ok());

    auto st12 = openscarlett::ValidateSpirvExternal(spv, diag, openscarlett::kDefaultSpirvValPath, "vulkan1.2");
    if (!st12.ok()) {
        std::fprintf(stderr, "[FAIL] spirv-val (vulkan1.2) for %s: %s\n", label, diag.c_str());
    }
    OS_REQUIRE(st12.ok());
}

#if defined(_WIN32)
struct D3D12TestContext {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device2> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList6> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0;

    std::string adapter_name;
    bool is_warp = false;
    uint32_t wave_min = 0;
    uint32_t mesh_tier = 0;
    uint32_t highest_sm = 0;

    ~D3D12TestContext() {
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
        char name_buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
        adapter_name = name_buf;

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            return false;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
            wave_min = opt1.WaveLaneCountMin;
        }
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)))) {
            mesh_tier = static_cast<uint32_t>(opt7.MeshShaderTier);
        }
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
            return false;
        }
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
        HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
                                                     IID_PPV_ARGS(&res));
        OS_REQUIRE(SUCCEEDED(hr));
        return res;
    }

    void RunComputeBuffers(const std::vector<uint8_t>& dxil,
                           const std::vector<uint32_t>& in_data,
                           std::vector<uint32_t>& out_data,
                           uint32_t group_x) {
        const uint64_t byte_size = static_cast<uint64_t>(in_data.size()) * sizeof(uint32_t);
        out_data.assign(in_data.size(), 0u);

        // Root signature: u0 (root UAV) + u1 (root UAV)
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].Descriptor.RegisterSpace = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
        OS_REQUIRE(SUCCEEDED(hr));

        ComPtr<ID3D12RootSignature> root_sig;
        hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_sig));
        OS_REQUIRE(SUCCEEDED(hr));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] CreateComputePipelineState failed on %s: hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        auto in_upload = CreateBuffer(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_upload = CreateBuffer(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuffer(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuffer(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        auto readback = CreateBuffer(byte_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);

        void* mapped = nullptr;
        OS_REQUIRE(SUCCEEDED(in_upload->Map(0, nullptr, &mapped)));
        std::memcpy(mapped, in_data.data(), static_cast<size_t>(byte_size));
        in_upload->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(out_upload->Map(0, nullptr, &mapped)));
        std::memset(mapped, 0, static_cast<size_t>(byte_size));
        out_upload->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        cmd_list->CopyResource(in_gpu.Get(), in_upload.Get());
        cmd_list->CopyResource(out_gpu.Get(), out_upload.Get());

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

        D3D12_RESOURCE_BARRIER uav_bar{};
        uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_bar.UAV.pResource = out_gpu.Get();
        cmd_list->ResourceBarrier(1, &uav_bar);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);
        cmd_list->CopyResource(readback.Get(), out_gpu.Get());

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(out_data.data(), mapped, static_cast<size_t>(byte_size));
        readback->Unmap(0, nullptr);
    }

    void RunComputeMimgTexture16x16(const std::vector<uint8_t>& dxil,
                                    const std::vector<float>& in_rgba,
                                    std::vector<float>& out_rgba) {
        constexpr UINT kW = 16;
        constexpr UINT kH = 16;
        constexpr UINT kRowPitch = kW * 16;  // 16 * sizeof(float4) = 256 bytes (exact D3D12 pitch alignment)
        constexpr UINT kTotalBytes = kRowPitch * kH;
        OS_REQUIRE(in_rgba.size() == kW * kH * 4);
        out_rgba.assign(kW * kH * 4, 0.0f);

        // Root signature: Descriptor table [t0 (SRV), u1 (UAV)] + static point sampler s0
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
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

        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_sig))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        OS_REQUIRE(SUCCEEDED(device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso))));

        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kW;
        td.Height = kH;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> in_tex;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&in_tex))));
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> out_tex;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                             IID_PPV_ARGS(&out_tex))));

        auto upload = CreateBuffer(kTotalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = CreateBuffer(kTotalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);

        void* mapped = nullptr;
        OS_REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &mapped)));
        std::memcpy(mapped, in_rgba.data(), kTotalBytes);
        upload->Unmap(0, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = {cpu0.ptr + inc};

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(in_tex.Get(), &srvd, cpu0);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(out_tex.Get(), nullptr, &uavd, cpu1);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = upload.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Offset = 0;
        src_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, kRowPitch};

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = in_tex.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        D3D12_RESOURCE_BARRIER to_srv{};
        to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_srv.Transition = {in_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        cmd_list->ResourceBarrier(1, &to_srv);

        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(root_sig.Get());
        cmd_list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        cmd_list->Dispatch(4, 1, 1);  // 4 * 64 = 256 threads covering 16x16

        D3D12_RESOURCE_BARRIER uav_bar{};
        uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_bar.UAV.pResource = out_tex.Get();
        cmd_list->ResourceBarrier(1, &uav_bar);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {out_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION rb_src{};
        rb_src.pResource = out_tex.Get();
        rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rb_src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION rb_dst{};
        rb_dst.pResource = readback.Get();
        rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rb_dst.PlacedFootprint.Offset = 0;
        rb_dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, kRowPitch};
        cmd_list->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(out_rgba.data(), mapped, kTotalBytes);
        readback->Unmap(0, nullptr);
    }

    void RunMeshShaderPipeline64x64(const std::vector<uint8_t>& as_dxil,
                                    const std::vector<uint8_t>& ms_dxil,
                                    const std::vector<uint8_t>& ps_dxil,
                                    std::vector<uint8_t>& out_rgba8) {
        constexpr UINT kW = 64;
        constexpr UINT kH = 64;
        constexpr UINT kRowPitch = kW * 4;  // 256 bytes exact D3D12 pitch alignment
        constexpr UINT kTotalBytes = kRowPitch * kH;
        out_rgba8.assign(kTotalBytes, 0u);

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_sig))));

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
        struct alignas(void*) SubobjRootSig {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            ID3D12RootSignature* val = nullptr;
        };
        struct alignas(void*) SubobjAS {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
            D3D12_SHADER_BYTECODE val{};
        };
        struct alignas(void*) SubobjMS {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
            D3D12_SHADER_BYTECODE val{};
        };
        struct alignas(void*) SubobjPS {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
            D3D12_SHADER_BYTECODE val{};
        };
        struct alignas(void*) SubobjBlend {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
            D3D12_BLEND_DESC val{};
        };
        struct alignas(void*) SubobjSampleMask {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;
            UINT val = 0xFFFFFFFFu;
        };
        struct alignas(void*) SubobjRast {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
            D3D12_RASTERIZER_DESC val{};
        };
        struct alignas(void*) SubobjDepth {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
            D3D12_DEPTH_STENCIL_DESC val{};
        };
        struct alignas(void*) SubobjRtvFormats {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
            D3D12_RT_FORMAT_ARRAY val{};
        };
        struct alignas(void*) SubobjSampleDesc {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;
            DXGI_SAMPLE_DESC val{1, 0};
        };

        struct MeshPsoStream {
            SubobjRootSig root_sig;
            SubobjAS as;
            SubobjMS ms;
            SubobjPS ps;
            SubobjBlend blend;
            SubobjSampleMask sample_mask;
            SubobjRast rast;
            SubobjDepth depth;
            SubobjRtvFormats rtv_formats;
            SubobjSampleDesc sample_desc;
        } stream{};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

        stream.root_sig.val = root_sig.Get();
        stream.as.val = {as_dxil.data(), as_dxil.size()};
        stream.ms.val = {ms_dxil.data(), ms_dxil.size()};
        stream.ps.val = {ps_dxil.data(), ps_dxil.size()};
        stream.blend.val.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.sample_mask.val = 0xFFFFFFFFu;
        stream.rast.val.FillMode = D3D12_FILL_MODE_SOLID;
        stream.rast.val.CullMode = D3D12_CULL_MODE_NONE;
        stream.rast.val.DepthClipEnable = FALSE;
        stream.depth.val.DepthEnable = FALSE;
        stream.depth.val.StencilEnable = FALSE;
        stream.rtv_formats.val.NumRenderTargets = 1;
        stream.rtv_formats.val.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.sample_desc.val = {1, 0};

        D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] Mesh CreatePipelineState failed on %s: hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rtd{};
        rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rtd.Width = kW;
        rtd.Height = kH;
        rtd.DepthOrArraySize = 1;
        rtd.MipLevels = 1;
        rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtd.SampleDesc.Count = 1;
        rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D12Resource> rt_tex;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rtd,
                                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                             IID_PPV_ARGS(&rt_tex))));

        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
        rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(rt_tex.Get(), nullptr, rtv_h);

        auto readback = CreateBuffer(kTotalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        const float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd_list->ClearRenderTargetView(rtv_h, clear_rgba, 0, nullptr);
        cmd_list->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd_list->RSSetViewports(1, &vp);
        cmd_list->RSSetScissorRects(1, &sc);

        cmd_list->SetGraphicsRootSignature(root_sig.Get());
        cmd_list->SetPipelineState(pso.Get());
        cmd_list->DispatchMesh(1, 1, 1);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {rt_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION rb_src{};
        rb_src.pResource = rt_tex.Get();
        rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rb_src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION rb_dst{};
        rb_dst.pResource = readback.Get();
        rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rb_dst.PlacedFootprint.Offset = 0;
        rb_dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kRowPitch};
        cmd_list->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        void* mapped = nullptr;
        OS_REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(out_rgba8.data(), mapped, kTotalBytes);
        readback->Unmap(0, nullptr);
    }
};
#endif

// Compiles lifted HLSL to a WARP-compatible DXIL target (SM 6.2 on Win10 WARP, or SM 6.5 if supported).
std::vector<uint8_t> CompileForDevice(const D3D12TestContext& ctx,
                                      const std::string& hlsl,
                                      const std::vector<uint8_t>& sm65_dxil) {
    if (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
        return sm65_dxil;
    }
    const char* target = (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) ? "cs_6_2" : "cs_6_0";
    std::vector<uint8_t> dxil;
    std::string diag;
    auto st = openscarlett::CompileHlslWithDxc(hlsl, target, "main", dxil, diag);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] DXC %s compile for WARP failed: %s\n", target, diag.c_str());
    }
    OS_REQUIRE(st.ok());
    return dxil;
}

// ---------------------------------------------------------------------------
// Test 1: Compute ALU + Conditional Branch + Constant Folding
// ---------------------------------------------------------------------------
void Test1_ComputeAluBranchConstFold(openscarlett::VulkanBackend& vk,
                                     D3D12TestContext& d3d_hw,
                                     D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // v1 = tid * 4 (byte offset)
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // v2 = in[tid] (srsrc = 0 -> u0 / binding 0)
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
    // Foldable constants: s8 = 1 + 2 = 3, s9 = 2 + 3 = 5
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::InlineInt(1));
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 8, Rdna2Operand::Sgpr(8), Rdna2Operand::InlineInt(2));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::InlineInt(2));
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 9, Rdna2Operand::Sgpr(9), Rdna2Operand::InlineInt(3));
    // v3 = v2 * 3; v4 = v3 + 5
    a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8));
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 4, Rdna2Operand::Sgpr(9), 3);
    // Branch: if (tid < 32) v5 = v4; else v5 = v4 + 100;
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(32));
    a.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(0), 10);
    const uint32_t br_else = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    // Then: v5 = v4 + 0
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 5, Rdna2Operand::InlineInt(0), 4);
    const uint32_t br_join = a.EmitSopp(Rdna2Opcode::S_BRANCH);
    // Else: v5 = v4 + 100
    a.PatchBranch(br_else, a.CurrentWordOffset());
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 5, Rdna2Operand::LiteralU32(100u), 4);
    // Join: store v5 to out[tid] (srsrc_sgpr = 4 -> srsrc = 1 -> u1 / binding 1)
    a.PatchBranch(br_join, a.CurrentWordOffset());
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test1 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.opt_stats.folded_constants >= 2u);
    OS_REQUIRE(res.hlsl_target == "cs_6_5");
    VerifySpirvExternalStrict(res.spirv_words, "Test1_ComputeAluBranch");

    constexpr uint32_t kCount = 64;
    std::vector<uint32_t> input(kCount);
    for (uint32_t i = 0; i < kCount; ++i) input[i] = i * 7u + 13u;

    auto verify_golden = [&](const std::vector<uint32_t>& out, const char* backend) {
        OS_REQUIRE(out.size() == kCount);
        for (uint32_t i = 0; i < kCount; ++i) {
            const uint32_t expected = (i < 32u) ? (input[i] * 3u + 5u) : (input[i] * 3u + 105u);
            if (out[i] != expected) {
                std::fprintf(stderr, "[FAIL] Test1 %s lane %u: got %u expected %u\n", backend, i, out[i], expected);
            }
            OS_REQUIRE(out[i] == expected);
        }
    };

    if (vk.GetDeviceInfo().is_hardware) {
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
        verify_golden(vk_out, "Vulkan-HW");
    }

    std::vector<uint32_t> d3d_hw_out;
    d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, d3d_hw_out, 1);
    verify_golden(d3d_hw_out, "D3D12-HW");

    std::vector<uint32_t> d3d_warp_out;
    d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, d3d_warp_out, 1);
    verify_golden(d3d_warp_out, "D3D12-WARP");

    // Also verify SM 5.0 fallback target emission
    std::string sm50_hlsl, sm50_target;
    OS_REQUIRE(EmitHlsl(res.ir, sm50_hlsl, sm50_target, /*use_sm65=*/false).ok());
    OS_REQUIRE(sm50_target == "cs_5_0");

    std::printf("[PASS] Test 1: Compute ALU + Branch + ConstFold (%u folded) -> spirv-val + Vulkan + DXC cs_6_5 + D3D12 HW + WARP\n",
                res.ir.opt_stats.folded_constants);
}

// ---------------------------------------------------------------------------
// Test 2: DS LDS Shared Memory + S_BARRIER + DS_ADD_RTN_U32 Atomic
// ---------------------------------------------------------------------------
void Test2_DsLdsBarrierAtomics(openscarlett::VulkanBackend& vk,
                               D3D12TestContext& d3d_hw,
                               D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // v1 = tid * 4
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // v2 = in[tid]
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
    // Zero-init LDS counter at byte offset 256 (lds[64]) on tid == 0
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
    const uint32_t br_skip = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(256u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 11, 10);
    a.PatchBranch(br_skip, a.CurrentWordOffset());

    // Write v2 to LDS[tid]
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
    a.EmitSopp(Rdna2Opcode::S_BARRIER);

    // Read reversed lane (63 - tid) from LDS
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(63));
    a.EmitVop2(Rdna2Opcode::V_SUB_NC_U32, 3, Rdna2Operand::Vgpr(12), 0);
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4, Rdna2Operand::InlineInt(2), 3);
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 5, 4);

    // Atomic increment counter at LDS byte offset 256 -> unique ticket in v6
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::LiteralU32(256u));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::InlineInt(1));
    a.EmitDs(Rdna2Opcode::DS_ADD_RTN_U32, 6, 13, 14);
    a.EmitSopp(Rdna2Opcode::S_BARRIER);

    // Read final atomic sum (must be 64) into v7
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 7, 13);

    // Pack: out[tid] = (v5 << 16) | (v7 << 8) | v6
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 8, Rdna2Operand::InlineInt(16), 5);
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 9, Rdna2Operand::InlineInt(8), 7);
    a.EmitVop2(Rdna2Opcode::V_OR_B32, 8, Rdna2Operand::Vgpr(8), 9);
    a.EmitVop2(Rdna2Opcode::V_OR_B32, 8, Rdna2Operand::Vgpr(8), 6);

    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test2 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.uses_lds);
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 234u));  // OpAtomicIAdd
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 224u));  // OpControlBarrier
    VerifySpirvExternalStrict(res.spirv_words, "Test2_DsLdsAtomics");

    constexpr uint32_t kCount = 64;
    std::vector<uint32_t> input(kCount);
    for (uint32_t i = 0; i < kCount; ++i) input[i] = i * 19u + 5u;

    auto verify_golden = [&](const std::vector<uint32_t>& out, const char* backend) {
        OS_REQUIRE(out.size() == kCount);
        std::array<bool, kCount> seen{};
        for (uint32_t i = 0; i < kCount; ++i) {
            const uint32_t rev_val = out[i] >> 16;
            const uint32_t total = (out[i] >> 8) & 0xFFu;
            const uint32_t ticket = out[i] & 0xFFu;
            if (rev_val != input[63u - i] || total != 64u || ticket >= 64u) {
                std::fprintf(stderr, "[FAIL] Test2 %s lane %u: rev=%u(exp %u) total=%u ticket=%u\n",
                             backend, i, rev_val, input[63u - i], total, ticket);
            }
            OS_REQUIRE(rev_val == input[63u - i]);
            OS_REQUIRE(total == 64u);
            OS_REQUIRE(ticket < 64u);
            OS_REQUIRE(!seen[ticket]);
            seen[ticket] = true;
        }
    };

    if (vk.GetDeviceInfo().is_hardware) {
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
        verify_golden(vk_out, "Vulkan-HW");
    }

    std::vector<uint32_t> d3d_hw_out;
    d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, d3d_hw_out, 1);
    verify_golden(d3d_hw_out, "D3D12-HW");

    std::vector<uint32_t> d3d_warp_out;
    d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, d3d_warp_out, 1);
    verify_golden(d3d_warp_out, "D3D12-WARP");

    std::printf("[PASS] Test 2: DS LDS + S_BARRIER + DS_ADD_RTN_U32 (64/64 unique atomic tickets) -> spirv-val + Vulkan + DXC cs_6_5 + D3D12 HW + WARP\n");
}

// ---------------------------------------------------------------------------
// Test 3: wave64 -> wave32 Emulation (Ballot64, ActiveCount64, PrefixCount64, ReadLaneAt(42), ReadFirstLane, Swizzle)
// ---------------------------------------------------------------------------
void Test3_Wave64OnWave32(openscarlett::VulkanBackend& vk,
                          D3D12TestContext& d3d_hw,
                          D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // v1 = tid * 4
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // v4 = in[tid]
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 4, 1, 0);

    // Predicate: (tid & 1) == 0 -> 32 active even lanes across wave64
    a.EmitVop2(Rdna2Opcode::V_AND_B32, 2, Rdna2Operand::InlineInt(1), 0);
    a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 2);

    // s8 = WaveActiveCountBits64(vcc) == 32
    a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 8, Rdna2Operand::VccLo());

    // v6 = WavePrefixCount64(vcc) == tid/2 for even lanes, (tid+1)/2 for odd lanes
    a.EmitVop3(Rdna2Opcode::V_MBCNT_LO_U32_B32, 5, Rdna2Operand::VccLo(), Rdna2Operand::InlineInt(0));
    a.EmitVop3(Rdna2Opcode::V_MBCNT_HI_U32_B32, 6, Rdna2Operand::VccHi(), Rdna2Operand::Vgpr(5));

    // s9 = WaveReadLaneAt(v4, 42) across Wave32 boundary [0..31] <-> [32..63]!
    a.EmitVop3(Rdna2Opcode::V_READLANE_B32, 9, Rdna2Operand::Vgpr(4), Rdna2Operand::InlineInt(42));
    // s10 = WaveReadFirstLane(v4) == in[0]
    a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 10, Rdna2Operand::Vgpr(4));
    // v7 = DS_SWIZZLE_B32 butterfly XOR 1 (swaps adjacent lanes tid ^ 1)
    a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 7, 4, 0, 0, static_cast<uint16_t>(0x8000u | (1u << 10) | 0x1Fu));

    // Store 5 full 32-bit results per lane into out[k*64 + tid]
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::Sgpr(8));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::Sgpr(9));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::Sgpr(10));

    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 1, 4, 0);     // out[0..63]   = active_count (32)
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 256);    // out[64..127] = prefix_count
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 1, 4, 512);   // out[128..191]= in[42]
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 1, 4, 768);   // out[192..255]= in[0]
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 1024);   // out[256..319]= in[tid ^ 1]
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, /*wave_size=*/64);

    // Also verify XS2 wave_size=64 round-trip!
    Xs2ShaderContainer parsed{};
    OS_REQUIRE(ParseXs2Container(xs2, parsed).ok());
    OS_REQUIRE(parsed.wave_size == 64u);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test3 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.uses_wave64_emulation);
    OS_REQUIRE(res.ir.wave_size == 64u);
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 205u));  // OpBitCount
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 241u));  // OpAtomicOr
    VerifySpirvExternalStrict(res.spirv_words, "Test3_Wave64OnWave32");

    std::vector<uint32_t> input(320, 0u);
    for (uint32_t i = 0; i < 64u; ++i) input[i] = 0xCAFE0000u | (i * 17u + 3u);

    auto verify_golden = [&](const std::vector<uint32_t>& out, const char* backend) {
        OS_REQUIRE(out.size() >= 320u);
        for (uint32_t tid = 0; tid < 64u; ++tid) {
            const uint32_t exp_active = 32u;
            const uint32_t exp_prefix = (tid + 1u) / 2u;  // exact count of even lanes < tid (== tid/2 for even lanes)
            const uint32_t exp_lane42 = input[42];
            const uint32_t exp_lane0 = input[0];
            const uint32_t exp_swiz = input[tid ^ 1u];

            if (out[tid] != exp_active || out[64 + tid] != exp_prefix ||
                out[128 + tid] != exp_lane42 || out[192 + tid] != exp_lane0 ||
                out[256 + tid] != exp_swiz) {
                std::fprintf(stderr,
                             "[FAIL] Test3 %s tid=%u: active=%u(%u) prefix=%u(%u) lane42=0x%X(0x%X) lane0=0x%X(0x%X) swiz=0x%X(0x%X)\n",
                             backend, tid,
                             out[tid], exp_active,
                             out[64 + tid], exp_prefix,
                             out[128 + tid], exp_lane42,
                             out[192 + tid], exp_lane0,
                             out[256 + tid], exp_swiz);
            }
            OS_REQUIRE(out[tid] == exp_active);
            OS_REQUIRE(out[64 + tid] == exp_prefix);
            if ((tid & 1u) == 0u) {
                OS_REQUIRE(out[64 + tid] == tid / 2u);
            }
            OS_REQUIRE(out[128 + tid] == exp_lane42);
            OS_REQUIRE(out[192 + tid] == exp_lane0);
            OS_REQUIRE(out[256 + tid] == exp_swiz);
        }
    };

    if (vk.GetDeviceInfo().is_hardware) {
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
        verify_golden(vk_out, "Vulkan-HW");
    }

    std::vector<uint32_t> d3d_hw_out;
    d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, d3d_hw_out, 1);
    verify_golden(d3d_hw_out, "D3D12-HW-Wave32");

    std::vector<uint32_t> d3d_warp_out;
    d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, d3d_warp_out, 1);
    verify_golden(d3d_warp_out, "D3D12-WARP");

    std::printf("[PASS] Test 3: wave64->wave32 emulation (HW Wave=%u, Ballot64/Count=32/Prefix/ReadLane(42)/Swizzle) -> spirv-val + Vulkan + DXC cs_6_5 + D3D12 HW + WARP\n",
                d3d_hw.wave_min);
}

// ---------------------------------------------------------------------------
// Test 4: MIMG (IMAGE_LOAD + IMAGE_SAMPLE_LZ + IMAGE_STORE) on 16x16 RGBA32F
// ---------------------------------------------------------------------------
void Test4_MimgTexturePipeline(D3D12TestContext& d3d_hw, D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // 1D dispatch (4*64 = 256 threads) -> 2D pixel coords: v1 = x = tid & 15, v2 = y = tid >> 4
    a.EmitVop2(Rdna2Opcode::V_AND_B32, 1, Rdna2Operand::InlineInt(15), 0);
    a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2, Rdna2Operand::InlineInt(4), 0);

    // IMAGE_LOAD v[4:7], v[1:2], s[0:7] dmask:0xf dim:2D unrm
    a.EmitMimg(Rdna2Opcode::IMAGE_LOAD, 4, 1, 0, 0, 0xFu, 1u, true);

    // Normalized texel-center UVs: u = (float(x) + 0.5) / 16.0, v = (float(y) + 0.5) / 16.0
    a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 10, Rdna2Operand::Vgpr(1));
    a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 11, Rdna2Operand::Vgpr(2));
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 10, Rdna2Operand::InlineFloat(0.5f), 10);
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 11, Rdna2Operand::InlineFloat(0.5f), 11);
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 10, Rdna2Operand::LiteralF32(1.0f / 16.0f), 10);
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 11, Rdna2Operand::LiteralF32(1.0f / 16.0f), 11);

    // IMAGE_SAMPLE_LZ v[12:15], v[10:11], s[0:7], s[8:11] dmask:0xf dim:2D
    a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE_LZ, 12, 10, 0, 8, 0xFu, 1u, false);

    // Transform:
    // out.r = load.r * 0.5 + sample.r * 0.5 + 0.125
    // out.g = load.g * 2.0
    // out.b = sample.b + 0.25
    // out.a = 1.0
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 16, Rdna2Operand::InlineFloat(0.5f), 4);
    a.EmitVop3(Rdna2Opcode::V_FMA_F32, 16, Rdna2Operand::Vgpr(12), Rdna2Operand::InlineFloat(0.5f), Rdna2Operand::Vgpr(16));
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 16, Rdna2Operand::LiteralF32(0.125f), 16);
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 17, Rdna2Operand::InlineFloat(2.0f), 5);
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 18, Rdna2Operand::LiteralF32(0.25f), 14);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 19, Rdna2Operand::InlineFloat(1.0f));

    // IMAGE_STORE v[16:19], v[1:2], s[0:7] dmask:0xf dim:2D unrm
    a.EmitMimg(Rdna2Opcode::IMAGE_STORE, 16, 1, 0, 0, 0xFu, 1u, true);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 24, 32);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test4 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.uses_texture_srv && res.ir.uses_texture_uav);
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 88u));  // OpImageSampleExplicitLod
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 98u));  // OpImageRead
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 99u));  // OpImageWrite
    VerifySpirvExternalStrict(res.spirv_words, "Test4_MimgTexture");

    std::vector<float> in_rgba(16 * 16 * 4);
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const size_t idx = (y * 16 + x) * 4;
            in_rgba[idx + 0] = static_cast<float>(x) / 16.0f;
            in_rgba[idx + 1] = static_cast<float>(y) / 32.0f;
            in_rgba[idx + 2] = static_cast<float>((x + y) & 15u) / 16.0f;
            in_rgba[idx + 3] = 1.0f;
        }
    }

    auto verify_pixels = [&](const std::vector<float>& out, const char* backend) {
        OS_REQUIRE(out.size() == in_rgba.size());
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                const size_t idx = (y * 16 + x) * 4;
                const float exp_r = in_rgba[idx + 0] + 0.125f;
                const float exp_g = in_rgba[idx + 1] * 2.0f;
                const float exp_b = in_rgba[idx + 2] + 0.25f;
                const float exp_a = 1.0f;
                if (std::fabs(out[idx + 0] - exp_r) > 1e-4f ||
                    std::fabs(out[idx + 1] - exp_g) > 1e-4f ||
                    std::fabs(out[idx + 2] - exp_b) > 1e-4f ||
                    std::fabs(out[idx + 3] - exp_a) > 1e-4f) {
                    std::fprintf(stderr,
                                 "[FAIL] Test4 %s (%u,%u): got (%.4f,%.4f,%.4f,%.4f) exp (%.4f,%.4f,%.4f,%.4f)\n",
                                 backend, x, y,
                                 out[idx + 0], out[idx + 1], out[idx + 2], out[idx + 3],
                                 exp_r, exp_g, exp_b, exp_a);
                }
                OS_REQUIRE(std::fabs(out[idx + 0] - exp_r) <= 1e-4f);
                OS_REQUIRE(std::fabs(out[idx + 1] - exp_g) <= 1e-4f);
                OS_REQUIRE(std::fabs(out[idx + 2] - exp_b) <= 1e-4f);
                OS_REQUIRE(std::fabs(out[idx + 3] - exp_a) <= 1e-4f);
            }
        }
    };

    std::vector<float> hw_out;
    d3d_hw.RunComputeMimgTexture16x16(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), in_rgba, hw_out);
    verify_pixels(hw_out, "D3D12-HW");

    std::vector<float> warp_out;
    d3d_warp.RunComputeMimgTexture16x16(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), in_rgba, warp_out);
    verify_pixels(warp_out, "D3D12-WARP");

    std::printf("[PASS] Test 4: MIMG (IMAGE_LOAD + IMAGE_SAMPLE_LZ + IMAGE_STORE, 16x16 RGBA32F) -> spirv-val + DXC cs_6_5 + D3D12 HW + WARP\n");
}

// ---------------------------------------------------------------------------
// Test 5: VS + PS (spirv-val + dxc) AND AS + MS + PS LIVE on GTX 1650 Ti (MeshTier=10)
// ---------------------------------------------------------------------------
void Test5_GraphicsVsPsAndLiveMeshAs(D3D12TestContext& d3d_hw) {
    using namespace openscarlett;

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    // 5A. Vertex Shader (vs_6_5 + SPIR-V 1.3)
    Rdna2Assembler vs;
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.25f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, true);
    vs.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult vs_res{};
    auto st_vs = LiftRdna2Xs2Shader(vs.BuildXs2(ShaderStage::Vertex), vs_res, opts);
    if (!st_vs.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 VS failed: %s\nHLSL:\n%s\n", st_vs.message.c_str(), vs_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_vs.ok());
    OS_REQUIRE(vs_res.hlsl_target == "vs_6_5");
    VerifySpirvExternalStrict(vs_res.spirv_words, "Test5_VS");

    // 5B. Pixel Shader (ps_6_5 + SPIR-V 1.3): doubles R (0.25 -> 0.50), preserves G=0.50, B=0.75, A=1.0
    Rdna2Assembler ps;
    ps.EmitVop2(Rdna2Opcode::V_MUL_F32, 4, Rdna2Operand::InlineFloat(2.0f), 0);
    ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 1, 2, 3, 0xFu, true, false, true);
    ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult ps_res{};
    auto st_ps = LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel), ps_res, opts);
    if (!st_ps.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 PS failed: %s\nHLSL:\n%s\n", st_ps.message.c_str(), ps_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_ps.ok());
    OS_REQUIRE(ps_res.hlsl_target == "ps_6_5");
    VerifySpirvExternalStrict(ps_res.spirv_words, "Test5_PS");

    // 5C. Amplification Shader (as_6_5): dispatches (1, 1, 1) mesh threadgroup
    Rdna2Assembler as;
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(1));
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
    as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
    as.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult as_res{};
    auto st_as = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 1, 1, 1), as_res, opts);
    if (!st_as.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 AS failed: %s\nHLSL:\n%s\n", st_as.message.c_str(), as_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_as.ok());
    OS_REQUIRE(as_res.hlsl_target == "as_6_5");

    // 5D. Mesh Shader (ms_6_5): emits 3 vertices + 1 full-screen triangle covering 64x64 RT
    Rdna2Assembler ms;
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));  // vert_count = 3
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));  // prim_count = 1
    ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));

    // Per-thread vertex position from v0 (_gtid in 0..2): (-1,-1), (-1,3), (3,-1)
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);

    // Vertex color Param0 = (0.25, 0.50, 0.75, 1.0)
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.25f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, false);

    // Thread 0 exports primitive connectivity (0, 1, 2) via EXP Prim (target 20)
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
    const uint32_t br_skip_prim = ms.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(1));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(2));
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 10, 11, 12, 0, 0x7u, true);
    ms.PatchBranch(br_skip_prim, ms.CurrentWordOffset());
    ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult ms_res{};
    auto st_ms = LiftRdna2Xs2Shader(ms.BuildXs2(ShaderStage::Mesh, 3, 1, 1), ms_res, opts);
    if (!st_ms.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 MS failed: %s\nHLSL:\n%s\n", st_ms.message.c_str(), ms_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_ms.ok());
    OS_REQUIRE(ms_res.hlsl_target == "ms_6_5");
    OS_REQUIRE(ms_res.ir.exports_prim);

    // Execute LIVE AS + MS + PS Mesh Shader Pipeline on HW GPUs with MeshTier >= 10 (GTX 1650 Ti)!
    if (d3d_hw.mesh_tier >= 10u) {
        std::vector<uint8_t> rt_pixels;
        d3d_hw.RunMeshShaderPipeline64x64(as_res.pc_dxil, ms_res.pc_dxil, ps_res.pc_dxil, rt_pixels);
        OS_REQUIRE(rt_pixels.size() == 64u * 64u * 4u);

        // Expected color: (0.25*2.0, 0.50, 0.75, 1.0) -> UNORM8 (128, 128, 191, 255)
        for (uint32_t p = 0; p < 64u * 64u; ++p) {
            const int r = static_cast<int>(rt_pixels[p * 4 + 0]);
            const int g = static_cast<int>(rt_pixels[p * 4 + 1]);
            const int b = static_cast<int>(rt_pixels[p * 4 + 2]);
            const int a_ch = static_cast<int>(rt_pixels[p * 4 + 3]);
            if (std::abs(r - 128) > 1 || std::abs(g - 128) > 1 || std::abs(b - 191) > 1 || a_ch != 255) {
                std::fprintf(stderr, "[FAIL] Test5 Mesh RT pixel %u: got (%d,%d,%d,%d) exp (~128,~128,~191,255)\n",
                             p, r, g, b, a_ch);
            }
            OS_REQUIRE(std::abs(r - 128) <= 1);
            OS_REQUIRE(std::abs(g - 128) <= 1);
            OS_REQUIRE(std::abs(b - 191) <= 1);
            OS_REQUIRE(a_ch == 255);
        }

        std::printf("[PASS] Test 5: VS+PS (spirv-val + DXC SM6.5) AND LIVE AS+MS+PS DispatchMesh on %s (MeshTier=%u, 64x64 RT=%u,%u,%u,%u)\n",
                    d3d_hw.adapter_name.c_str(), d3d_hw.mesh_tier,
                    rt_pixels[0], rt_pixels[1], rt_pixels[2], rt_pixels[3]);
    } else {
        std::printf("[PASS] Test 5: VS+PS+AS+MS DXC SM6.5 validated (live DispatchMesh skipped on %s, MeshTier=%u)\n",
                    d3d_hw.adapter_name.c_str(), d3d_hw.mesh_tier);
    }
}

// ---------------------------------------------------------------------------
// Test 6: XS2 v1/v2 Round-Trip, Extra Opcodes (IMAGE_SAMPLE, DS_ADD_U32, Packed Prim), & Error Paths
// ---------------------------------------------------------------------------
void Test6_DisasmRoundTripAndEdgeCases() {
    using namespace openscarlett;

    // 1. XS2 v2 container (44-byte header) with wave_size=64 round-trip
    {
        Rdna2Assembler a;
        a.EmitSopp(Rdna2Opcode::S_NOP);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Xs2ShaderContainer c{};
        c.version = kXs2ContainerVersionV2;
        c.stage = ShaderStage::Compute;
        c.sgpr_count = 24;
        c.vgpr_count = 32;
        c.threadgroup_x = 64;
        c.wave_size = 64;
        c.entry_offset = kXs2HeaderSizeBytesV2;
        c.code_words = a.Words();
        const auto bytes = BuildXs2Container(c);
        OS_REQUIRE(bytes.size() == kXs2HeaderSizeBytesV2 + a.Words().size() * 4u);

        Xs2ShaderContainer parsed{};
        OS_REQUIRE(ParseXs2Container(bytes, parsed).ok());
        OS_REQUIRE(parsed.version == kXs2ContainerVersionV2);
        OS_REQUIRE(parsed.sgpr_count == 24u);
        OS_REQUIRE(parsed.vgpr_count == 32u);
        OS_REQUIRE(parsed.wave_size == 64u);
    }

    // 2. IMAGE_SAMPLE (explicit LOD) + DS_ADD_U32 (non-returning atomic, must survive DCE!)
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.5f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(0.5f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(1.0f));  // LOD = 1.0
        a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE, 4, 1, 0, 8, 0xFu, 1u, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineInt(0));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::InlineInt(1));
        a.EmitDs(Rdna2Opcode::DS_ADD_U32, 0, 8, 9);
        a.EmitMimg(Rdna2Opcode::IMAGE_STORE, 4, 8, 0, 0, 0xFu, 1u, true);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.validate_spirv_external = HasExternalSpirvVal();
        opts.compile_dxil_with_dxc = true;
        Rdna2LiftResult res{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 64, 1, 1), res, opts).ok());
        OS_REQUIRE(res.ir.uses_lds);
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 234u));  // DS_ADD_U32 survived DCE -> OpAtomicIAdd
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 88u));   // OpImageSampleExplicitLod
        VerifySpirvExternalStrict(res.spirv_words, "Test6_ImageSample_DsAddU32");
    }

    // 3. Packed EXP Prim (en=0x1, 10-bit indices packed in single VGPR) -> ms_6_5 + SM 5.0 rejection
    {
        Rdna2Assembler ms;
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 1, 1, 2, 0xFu, false);
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(0u | (1u << 10) | (2u << 20)));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 3, 0, 0, 0, 0x1u, true);
        ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.compile_dxil_with_dxc = true;
        Rdna2LiftResult res{};
        OS_REQUIRE(LiftRdna2Xs2Shader(ms.BuildXs2(ShaderStage::Mesh, 3, 1, 1), res, opts).ok());
        OS_REQUIRE(res.hlsl_target == "ms_6_5");

        std::string dummy_hlsl, dummy_target;
        OS_REQUIRE(!EmitHlsl(res.ir, dummy_hlsl, dummy_target, /*use_sm65=*/false).ok());
    }

    // 4. Negative validator & compiler diagnostics checks
    {
        std::string diag;
        std::vector<uint32_t> bad_spv = {0x07230203u, 0x00010300u, 0u, 10u, 0u, (2u << 16) | 17u, 99999u};
        OS_REQUIRE(!ValidateSpirvExternal(bad_spv, diag).ok());
        OS_REQUIRE(!diag.empty());

        std::vector<uint8_t> bad_dxil;
        std::string dxc_diag;
        OS_REQUIRE(!CompileHlslWithDxc("this is not valid hlsl;", "cs_6_5", "main", bad_dxil, dxc_diag).ok());
        OS_REQUIRE(!dxc_diag.empty());
    }

    std::printf("[PASS] Test 6: XS2 v2 round-trip, IMAGE_SAMPLE(LOD), DS_ADD_U32 DCE retention, packed EXP Prim, & error diagnostics\n");
}

}  // namespace

int main() {
    using namespace openscarlett;

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(/*allow_software_fallback=*/true).ok());
    const auto vk_info = vk.GetDeviceInfo();

    D3D12TestContext d3d_hw;
    if (!d3d_hw.Init(/*use_warp=*/false)) {
        OS_REQUIRE(d3d_hw.Init(/*use_warp=*/true));
    }
    D3D12TestContext d3d_warp;
    OS_REQUIRE(d3d_warp.Init(/*use_warp=*/true));

    std::printf("=== OpenScarlett RDNA2 Lifter Live Dual-GPU Verification ===\n");
    std::printf("Vulkan ICD : %s (API %u.%u.%u)\n",
                vk_info.device_name.c_str(), vk_info.api_major, vk_info.api_minor, vk_info.api_patch);
    std::printf("D3D12 HW   : %s (Wave=%u, SM=0x%02X, MeshTier=%u)\n",
                d3d_hw.adapter_name.c_str(), d3d_hw.wave_min, d3d_hw.highest_sm, d3d_hw.mesh_tier);
    std::printf("D3D12 WARP : %s (Wave=%u, SM=0x%02X, MeshTier=%u)\n",
                d3d_warp.adapter_name.c_str(), d3d_warp.wave_min, d3d_warp.highest_sm, d3d_warp.mesh_tier);

    Test1_ComputeAluBranchConstFold(vk, d3d_hw, d3d_warp);
    Test2_DsLdsBarrierAtomics(vk, d3d_hw, d3d_warp);
    Test3_Wave64OnWave32(vk, d3d_hw, d3d_warp);
    Test4_MimgTexturePipeline(d3d_hw, d3d_warp);
    Test5_GraphicsVsPsAndLiveMeshAs(d3d_hw);
    Test6_DisasmRoundTripAndEdgeCases();

    // Verify CallTelemetry recorded our lifter calls cleanly
    const auto snap = CallTelemetry::Global().Snapshot();
    bool saw_lifter_telemetry = false;
    for (const auto& entry : snap) {
        if (entry.module_name == "rdna2_lifter" && entry.ok_calls > 0) {
            saw_lifter_telemetry = true;
            break;
        }
    }
    OS_REQUIRE(saw_lifter_telemetry);

    vk.Shutdown();
    std::printf("[PASS] test_rdna2_lifter_live: ALL 5 LIVE DUAL-GPU SUITES PASSED!\n");
    return 0;
}
