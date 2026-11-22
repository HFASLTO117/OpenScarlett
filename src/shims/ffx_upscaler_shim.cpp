#define OPENSCARLETT_BUILDING_FFX_UPSCALER 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/framebuffer.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/middleware/ffx_gpu_fsr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#  include <d3d12.h>
#  include <d3dcompiler.h>
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "amd_fidelityfx_upscaler_xbox.dll";
constexpr uint32_t kUpscalerVersion = 0x02010000u;  // 2.1.0
constexpr const char* kProviderVersionName =
    "OpenScarlett FidelityFX Upscaler 2.1 (GPU FSR1/FFX2 D3D12)";

// Standard AMD FidelityFX API 2.x struct type identifiers.
constexpr uint64_t kFfx2DescTypeOverrideVersion = 0x00000001ull;
constexpr uint64_t kFfx2DescTypeBackendDx12 = 0x00000002ull;
constexpr uint64_t kFfx2QueryDescTypeGetVersions = 0x00000004ull;
constexpr uint64_t kFfx2CreateDescTypeUpscale = 0x00010000ull;
constexpr uint64_t kFfx2DispatchDescTypeUpscale = 0x00010001ull;
constexpr uint64_t kFfx2QueryDescTypeUpscaleGetRatio = 0x00010002ull;
constexpr uint64_t kFfx2QueryDescTypeUpscaleGetRenderRes = 0x00010003ull;
constexpr uint64_t kFfx2QueryDescTypeUpscaleGetJitterPhaseCount = 0x00010004ull;
constexpr uint64_t kFfx2QueryDescTypeUpscaleGetJitterOffset = 0x00010005ull;
constexpr uint64_t kFfx2ConfigureDescTypeUpscaleKeyValue = 0x00010007ull;

// Standard AMD FidelityFX API 2.x resource state bits.
constexpr uint32_t kFfx2StateCommon = 0x0000u;
constexpr uint32_t kFfx2StateUnorderedAccess = 0x0001u;
constexpr uint32_t kFfx2StateComputeRead = 0x0002u;
constexpr uint32_t kFfx2StateCopySrc = 0x0004u;
constexpr uint32_t kFfx2StateCopyDest = 0x0008u;
constexpr uint32_t kFfx2StateGenericRead = 0x0010u;
constexpr uint32_t kFfx2StatePixelRead = 0x0020u;
constexpr uint32_t kFfx2StatePixelComputeRead = 0x0040u;
constexpr uint32_t kFfx2StateIndirectArg = 0x0080u;
constexpr uint32_t kFfx2StateRenderTarget = 0x0100u;

struct Ffx2Dimensions2D {
    uint32_t width;
    uint32_t height;
};

struct Ffx2FloatCoords2D {
    float x;
    float y;
};

struct Ffx2ResourceDescription {
    uint32_t type;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct Ffx2Resource {
    void* resource;
    Ffx2ResourceDescription description;
    uint32_t state;
};

struct Ffx2DispatchDescUpscale {
    FfxApiHeader header;
    void* commandList;
    Ffx2Resource color;
    Ffx2Resource depth;
    Ffx2Resource motionVectors;
    Ffx2Resource exposure;
    Ffx2Resource reactive;
    Ffx2Resource transparencyAndComposition;
    Ffx2Resource output;
    Ffx2FloatCoords2D jitterOffset;
    Ffx2FloatCoords2D motionVectorScale;
    Ffx2Dimensions2D renderSize;
    Ffx2Dimensions2D upscaleSize;
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    uint32_t flags;
};

struct Ffx2QueryDescUpscaleGetRatio {
    FfxApiHeader header;
    uint32_t qualityMode;
    uint32_t _pad;
    float* pOutUpscaleRatio;
};

struct Ffx2QueryDescUpscaleGetJitterPhaseCount {
    FfxApiHeader header;
    int32_t renderWidth;
    int32_t displayWidth;
    int32_t* pOutPhaseCount;
};

struct Ffx2QueryDescUpscaleGetJitterOffset {
    FfxApiHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

#if defined(_WIN32)
constexpr const char* kFfx2UpscaleHlsl = R"(
cbuffer Ffx2UpscaleConstants : register(b0) {
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float rcpDstWidth;
    float rcpDstHeight;
    float sharpness;
    float enableSharpen;
};

Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTex : register(u0);

float4 LoadClamped(int2 p) {
    int2 c = clamp(p, int2(0, 0), int2((int)srcWidth - 1, (int)srcHeight - 1));
    return SrcTex.Load(int3(c, 0));
}

[numthreads(8, 8, 1)]
void MainCS(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dstWidth || tid.y >= dstHeight) {
        return;
    }

    float2 scale = float2((float)srcWidth * rcpDstWidth, (float)srcHeight * rcpDstHeight);
    float2 pp = (float2(tid.xy) + 0.5) * scale - 0.5;
    float2 fp = floor(pp);
    float2 f = pp - fp;
    int2 base = int2((int)fp.x, (int)fp.y);

    float4 c00 = LoadClamped(base + int2(0, 0));
    float4 c10 = LoadClamped(base + int2(1, 0));
    float4 c01 = LoadClamped(base + int2(0, 1));
    float4 c11 = LoadClamped(base + int2(1, 1));
    float4 top = lerp(c00, c10, f.x);
    float4 bot = lerp(c01, c11, f.x);
    float4 col = lerp(top, bot, f.y);

    if (enableSharpen > 0.5 && sharpness > 1e-4) {
        float4 n = LoadClamped(base + int2(0, -1));
        float4 s = LoadClamped(base + int2(0, 1));
        float4 w = LoadClamped(base + int2(-1, 0));
        float4 e = LoadClamped(base + int2(1, 0));
        float3 avg = (n.rgb + s.rgb + w.rgb + e.rgb) * 0.25;
        float3 mn = min(min(c00.rgb, c10.rgb), min(c01.rgb, c11.rgb));
        float3 mx = max(max(c00.rgb, c10.rgb), max(c01.rgb, c11.rgb));
        float3 sharp = col.rgb + (col.rgb - avg) * (0.35 * saturate(sharpness));
        col.rgb = clamp(sharp, mn, mx);
    }

    DstTex[tid.xy] = float4(saturate(col.rgb), saturate(col.a));
}
)";

struct Ffx2RootConstants {
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
    float rcpDstWidth;
    float rcpDstHeight;
    float sharpness;
    float enableSharpen;
};

struct Ffx2Dx12Pipeline {
    static constexpr uint32_t kRingPairs = 64u;

    ID3D12Device* device = nullptr;
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT descriptor_inc = 0u;
    uint32_t pair_cursor = 0u;

    ~Ffx2Dx12Pipeline() { Release(); }

    void Release() {
        if (heap) { heap->Release(); heap = nullptr; }
        if (pso) { pso->Release(); pso = nullptr; }
        if (root_sig) { root_sig->Release(); root_sig = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        descriptor_inc = 0u;
        pair_cursor = 0u;
    }

    bool EnsureInitialized(ID3D12Device* dev) {
        if (!dev) return false;
        if (device == dev && root_sig && pso && heap) {
            return true;
        }
        Release();

        ID3DBlob* cs_blob = nullptr;
        ID3DBlob* err_blob = nullptr;
        HRESULT hr = D3DCompile(
            kFfx2UpscaleHlsl,
            std::strlen(kFfx2UpscaleHlsl),
            "openscarlett_ffx2_upscale.hlsl",
            nullptr,
            nullptr,
            "MainCS",
            "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL1,
            0,
            &cs_blob,
            &err_blob);
        if (err_blob) err_blob->Release();
        if (FAILED(hr) || !cs_blob) {
            if (cs_blob) cs_blob->Release();
            return false;
        }

        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;
        srv_range.RegisterSpace = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors = 1;
        uav_range.BaseShaderRegister = 0;
        uav_range.RegisterSpace = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srv_range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uav_range;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 3;
        rs_desc.pParameters = params;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        using PfnSerializeRS = HRESULT(WINAPI*)(
            const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        auto serialize_fn = reinterpret_cast<PfnSerializeRS>(
            openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
        if (!serialize_fn) {
            cs_blob->Release();
            return false;
        }

        ID3DBlob* sig_blob = nullptr;
        ID3DBlob* sig_err = nullptr;
        hr = serialize_fn(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &sig_err);
        if (sig_err) sig_err->Release();
        if (FAILED(hr) || !sig_blob) {
            if (sig_blob) sig_blob->Release();
            cs_blob->Release();
            return false;
        }

        hr = dev->CreateRootSignature(
            0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
            IID_PPV_ARGS(&root_sig));
        sig_blob->Release();
        if (FAILED(hr) || !root_sig) {
            cs_blob->Release();
            Release();
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_sig;
        pso_desc.CS = {cs_blob->GetBufferPointer(), cs_blob->GetBufferSize()};
        hr = dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
        cs_blob->Release();
        if (FAILED(hr) || !pso) {
            Release();
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hdesc{};
        hdesc.NumDescriptors = kRingPairs * 2u;
        hdesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hdesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hdesc, IID_PPV_ARGS(&heap));
        if (FAILED(hr) || !heap) {
            Release();
            return false;
        }

        descriptor_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dev->AddRef();
        device = dev;
        pair_cursor = 0u;
        return true;
    }
};

D3D12_RESOURCE_STATES MapFfx2ResourceState(uint32_t ffx_state) noexcept {
    switch (ffx_state) {
        case kFfx2StateCommon:
            return D3D12_RESOURCE_STATE_COMMON;
        case kFfx2StateUnorderedAccess:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case kFfx2StateComputeRead:
            return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case kFfx2StateCopySrc:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case kFfx2StateCopyDest:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case kFfx2StateGenericRead:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case kFfx2StatePixelRead:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case kFfx2StatePixelComputeRead:
            return static_cast<D3D12_RESOURCE_STATES>(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        case kFfx2StateIndirectArg:
            return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case kFfx2StateRenderTarget:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

DXGI_FORMAT ResolveTypedFormatForView(DXGI_FORMAT fmt, bool for_uav) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return for_uav ? DXGI_FORMAT_R8G8B8A8_UNORM : fmt;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return for_uav ? DXGI_FORMAT_B8G8R8A8_UNORM : fmt;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return fmt;
    }
}
#endif

}  // namespace

struct FfxUpscalerContext {
    std::mutex mutex;
    openscarlett::FfxGpuFsrPipeline pipeline;
    uint32_t max_render_w = 0;
    uint32_t max_render_h = 0;
    uint32_t max_display_w = 0;
    uint32_t max_display_h = 0;
    float sharpness = 0.8f;
    bool enable_rcas = true;
#if defined(_WIN32)
    ID3D12Device* native_device = nullptr;
    Ffx2Dx12Pipeline ffx2_dx12;
    ~FfxUpscalerContext() {
        ffx2_dx12.Release();
        if (native_device) {
            native_device->Release();
            native_device = nullptr;
        }
    }
#endif
};

namespace {

struct FfxApi2Context {
    FfxUpscalerContextHandle upscaler = nullptr;
    float sharpness = 0.8f;
    uint32_t enable_rcas = 1u;
    bool custom_allocated = false;
};

struct UpscalerState {
    std::mutex mutex;
    std::unordered_map<FfxUpscalerContextHandle, std::shared_ptr<FfxUpscalerContext>> contexts;
    std::unordered_set<FfxApi2Context*> api2_contexts;
};

UpscalerState& GetUpscalerState() {
    static UpscalerState* s = new UpscalerState();
    return *s;
}

inline int32_t RecordRet(const char* sym, int32_t rc) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        rc == FFX_API_RETURN_OK ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        rc,
        -1);
    return rc;
}

std::shared_ptr<FfxUpscalerContext> LookupContext(FfxUpscalerContextHandle handle) {
    if (!handle) return nullptr;
    auto& st = GetUpscalerState();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.contexts.find(handle);
    return it == st.contexts.end() ? nullptr : it->second;
}

bool IsLiveApi2Context(FfxApi2Context* ctx) {
    if (!ctx) return false;
    auto& st = GetUpscalerState();
    std::lock_guard<std::mutex> lk(st.mutex);
    return st.api2_contexts.find(ctx) != st.api2_contexts.end();
}

float ScaleRatioForQualityMode(uint32_t qualityMode) noexcept {
    switch (qualityMode) {
        case FFX_UPSCALER_QUALITY_NATIVEAA: return 1.0f;
        case FFX_UPSCALER_QUALITY_ULTRA_QUALITY: return 1.3f;
        case FFX_UPSCALER_QUALITY_QUALITY: return 1.5f;
        case FFX_UPSCALER_QUALITY_BALANCED: return 1.7f;
        case FFX_UPSCALER_QUALITY_PERFORMANCE: return 2.0f;
        case FFX_UPSCALER_QUALITY_ULTRA_PERFORMANCE: return 3.0f;
        default: return 1.5f;
    }
}

float ComputeHalton(uint32_t index, uint32_t base) noexcept {
    float f = 1.0f;
    float r = 0.0f;
    uint32_t current = index;
    while (current > 0u) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(current % base);
        current /= base;
    }
    return r;
}

bool LooksLikeLegacyRgba8Dispatch(const FfxDispatchDescHeader* cur) noexcept {
    const auto* legacy = reinterpret_cast<const FfxDispatchDescUpscale*>(cur);
    if (!legacy->inputRgba8 || !legacy->outputRgba8) {
        return false;
    }
    if (legacy->inputWidth == 0u || legacy->inputWidth > 16384u ||
        legacy->inputHeight == 0u || legacy->inputHeight > 16384u ||
        legacy->outputWidth == 0u || legacy->outputWidth > 16384u ||
        legacy->outputHeight == 0u || legacy->outputHeight > 16384u) {
        return false;
    }
    const uint64_t required =
        static_cast<uint64_t>(legacy->outputWidth) *
        static_cast<uint64_t>(legacy->outputHeight) * 4ull;
    if (legacy->outputBufferBytes < required || legacy->outputBufferBytes > (1ull << 32)) {
        return false;
    }
    const uintptr_t out_ptr_val = reinterpret_cast<uintptr_t>(legacy->outputRgba8);
    const uint32_t out_lo = static_cast<uint32_t>(out_ptr_val & 0xFFFFFFFFull);
    const uint32_t out_hi = static_cast<uint32_t>((out_ptr_val >> 32) & 0xFFFFFFFFull);
    if (out_ptr_val < 0x10000u || (out_lo <= 16u && out_hi <= 128u)) {
        return false;
    }
    return true;
}

int32_t DispatchFfx2Dx12(
    FfxUpscalerContext& ctx,
    const Ffx2DispatchDescUpscale& d) {
#if !defined(_WIN32)
    (void)ctx;
    (void)d;
    return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
#else
    if (!d.commandList || !d.color.resource || !d.output.resource) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(d.commandList);
    auto* color_res = static_cast<ID3D12Resource*>(d.color.resource);
    auto* out_res = static_cast<ID3D12Resource*>(d.output.resource);

    std::lock_guard<std::mutex> lk(ctx.mutex);

    ID3D12Device* dev = ctx.native_device;
    ID3D12Device* temp_dev = nullptr;
    if (!dev) {
        if (SUCCEEDED(cmd->GetDevice(IID_PPV_ARGS(&temp_dev))) && temp_dev) {
            dev = temp_dev;
        }
    }
    if (!dev) {
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }

    const D3D12_RESOURCE_DESC color_desc = color_res->GetDesc();
    const D3D12_RESOURCE_DESC out_desc = out_res->GetDesc();
    const uint32_t tex_in_w = static_cast<uint32_t>(std::max<UINT64>(1ull, color_desc.Width));
    const uint32_t tex_in_h = std::max<uint32_t>(1u, color_desc.Height);
    const uint32_t tex_out_w = static_cast<uint32_t>(std::max<UINT64>(1ull, out_desc.Width));
    const uint32_t tex_out_h = std::max<uint32_t>(1u, out_desc.Height);

    const uint32_t src_w = (d.renderSize.width > 0u) ? std::min(d.renderSize.width, tex_in_w) : tex_in_w;
    const uint32_t src_h = (d.renderSize.height > 0u) ? std::min(d.renderSize.height, tex_in_h) : tex_in_h;
    const uint32_t dst_w = (d.upscaleSize.width > 0u) ? std::min(d.upscaleSize.width, tex_out_w) : tex_out_w;
    const uint32_t dst_h = (d.upscaleSize.height > 0u) ? std::min(d.upscaleSize.height, tex_out_h) : tex_out_h;

    const D3D12_RESOURCE_STATES orig_color_state = MapFfx2ResourceState(d.color.state);
    const D3D12_RESOURCE_STATES orig_out_state = MapFfx2ResourceState(d.output.state);

    const bool out_allows_uav = (out_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    const bool use_compute = out_allows_uav && ctx.ffx2_dx12.EnsureInitialized(dev);

    if (use_compute) {
        const bool color_already_srv =
            (orig_color_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
            (orig_color_state == D3D12_RESOURCE_STATE_GENERIC_READ) ||
            ((orig_color_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0);
        const bool out_already_uav = (orig_out_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_RESOURCE_BARRIER pre_barriers[2]{};
        UINT pre_count = 0u;
        if (!color_already_srv) {
            pre_barriers[pre_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre_barriers[pre_count].Transition.pResource = color_res;
            pre_barriers[pre_count].Transition.StateBefore = orig_color_state;
            pre_barriers[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            pre_barriers[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++pre_count;
        }
        if (!out_already_uav) {
            pre_barriers[pre_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre_barriers[pre_count].Transition.pResource = out_res;
            pre_barriers[pre_count].Transition.StateBefore = orig_out_state;
            pre_barriers[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            pre_barriers[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++pre_count;
        }
        if (pre_count > 0u) {
            cmd->ResourceBarrier(pre_count, pre_barriers);
        }

        const uint32_t pair_idx = ctx.ffx2_dx12.pair_cursor;
        ctx.ffx2_dx12.pair_cursor = (ctx.ffx2_dx12.pair_cursor + 1u) % Ffx2Dx12Pipeline::kRingPairs;
        const uint32_t srv_slot = pair_idx * 2u;
        const uint32_t uav_slot = srv_slot + 1u;

        const D3D12_CPU_DESCRIPTOR_HANDLE cpu_base =
            ctx.ffx2_dx12.heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_base =
            ctx.ffx2_dx12.heap->GetGPUDescriptorHandleForHeapStart();

        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = {
            cpu_base.ptr + static_cast<SIZE_T>(ctx.ffx2_dx12.descriptor_inc) * srv_slot};
        D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu = {
            cpu_base.ptr + static_cast<SIZE_T>(ctx.ffx2_dx12.descriptor_inc) * uav_slot};
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = {
            gpu_base.ptr + static_cast<UINT64>(ctx.ffx2_dx12.descriptor_inc) * srv_slot};
        D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = {
            gpu_base.ptr + static_cast<UINT64>(ctx.ffx2_dx12.descriptor_inc) * uav_slot};

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = ResolveTypedFormatForView(color_desc.Format, false);
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(color_res, &srv_desc, srv_cpu);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = ResolveTypedFormatForView(out_desc.Format, true);
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(out_res, nullptr, &uav_desc, uav_cpu);

        Ffx2RootConstants consts{};
        consts.srcWidth = src_w;
        consts.srcHeight = src_h;
        consts.dstWidth = dst_w;
        consts.dstHeight = dst_h;
        consts.rcpDstWidth = 1.0f / static_cast<float>(dst_w);
        consts.rcpDstHeight = 1.0f / static_cast<float>(dst_h);
        consts.sharpness = std::clamp(
            std::isfinite(d.sharpness) && d.sharpness >= 0.0f ? d.sharpness : ctx.sharpness,
            0.0f, 1.0f);
        consts.enableSharpen = (d.enableSharpening && ctx.enable_rcas) ? 1.0f : 0.0f;

        ID3D12DescriptorHeap* heaps[] = {ctx.ffx2_dx12.heap};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(ctx.ffx2_dx12.root_sig);
        cmd->SetPipelineState(ctx.ffx2_dx12.pso);
        cmd->SetComputeRoot32BitConstants(0, 8, &consts, 0);
        cmd->SetComputeRootDescriptorTable(1, srv_gpu);
        cmd->SetComputeRootDescriptorTable(2, uav_gpu);

        const uint32_t gx = (dst_w + 7u) / 8u;
        const uint32_t gy = (dst_h + 7u) / 8u;
        cmd->Dispatch(gx, gy, 1u);

        D3D12_RESOURCE_BARRIER post_barriers[3]{};
        UINT post_count = 0u;
        post_barriers[post_count].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        post_barriers[post_count].UAV.pResource = out_res;
        ++post_count;

        if (!color_already_srv) {
            post_barriers[post_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post_barriers[post_count].Transition.pResource = color_res;
            post_barriers[post_count].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            post_barriers[post_count].Transition.StateAfter = orig_color_state;
            post_barriers[post_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++post_count;
        }
        if (!out_already_uav) {
            post_barriers[post_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post_barriers[post_count].Transition.pResource = out_res;
            post_barriers[post_count].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            post_barriers[post_count].Transition.StateAfter = orig_out_state;
            post_barriers[post_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++post_count;
        }
        cmd->ResourceBarrier(post_count, post_barriers);
    } else {
        // Fallback copy path for non-UAV output textures.
        D3D12_RESOURCE_BARRIER pre_barriers[2]{};
        UINT pre_count = 0u;
        if (orig_color_state != D3D12_RESOURCE_STATE_COPY_SOURCE &&
            orig_color_state != D3D12_RESOURCE_STATE_GENERIC_READ) {
            pre_barriers[pre_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre_barriers[pre_count].Transition.pResource = color_res;
            pre_barriers[pre_count].Transition.StateBefore = orig_color_state;
            pre_barriers[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            pre_barriers[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++pre_count;
        }
        if (orig_out_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            pre_barriers[pre_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pre_barriers[pre_count].Transition.pResource = out_res;
            pre_barriers[pre_count].Transition.StateBefore = orig_out_state;
            pre_barriers[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            pre_barriers[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++pre_count;
        }
        if (pre_count > 0u) {
            cmd->ResourceBarrier(pre_count, pre_barriers);
        }

        const uint32_t copy_w = std::min(src_w, dst_w);
        const uint32_t copy_h = std::min(src_h, dst_h);
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = color_res;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = out_res;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;

        D3D12_BOX box{0u, 0u, 0u, copy_w, copy_h, 1u};
        cmd->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, &box);

        D3D12_RESOURCE_BARRIER post_barriers[2]{};
        UINT post_count = 0u;
        if (orig_color_state != D3D12_RESOURCE_STATE_COPY_SOURCE &&
            orig_color_state != D3D12_RESOURCE_STATE_GENERIC_READ) {
            post_barriers[post_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post_barriers[post_count].Transition.pResource = color_res;
            post_barriers[post_count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            post_barriers[post_count].Transition.StateAfter = orig_color_state;
            post_barriers[post_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++post_count;
        }
        if (orig_out_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            post_barriers[post_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post_barriers[post_count].Transition.pResource = out_res;
            post_barriers[post_count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            post_barriers[post_count].Transition.StateAfter = orig_out_state;
            post_barriers[post_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++post_count;
        }
        if (post_count > 0u) {
            cmd->ResourceBarrier(post_count, post_barriers);
        }
    }

    if (temp_dev) {
        temp_dev->Release();
    }
    return FFX_API_RETURN_OK;
#endif
}

}  // namespace

extern "C" {

uint32_t OS_SHIM_CALL ffxUpscalerGetVersion(void) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, "ffxUpscalerGetVersion", openscarlett::CallOutcome::Ok, 0, -1);
    return kUpscalerVersion;
}

int32_t OS_SHIM_CALL ffxUpscalerCreateContext(
    FfxUpscalerContextHandle* outContext,
    const FfxUpscalerCreateDesc* desc) {
    if (!outContext) {
        return RecordRet("ffxUpscalerCreateContext", FFX_API_RETURN_ERROR_PARAMETER);
    }
    *outContext = nullptr;

    auto ctx = std::make_shared<FfxUpscalerContext>();
    const uint32_t flags = desc ? desc->flags : FFX_UPSCALER_FLAG_ENABLE_RCAS;
    const bool prefer_warp = (flags & FFX_UPSCALER_FLAG_PREFER_WARP) != 0u;
    const bool enable_debug = (flags & FFX_UPSCALER_FLAG_ENABLE_DEBUG) != 0u;

    ctx->max_render_w = desc ? desc->maxRenderWidth : 0u;
    ctx->max_render_h = desc ? desc->maxRenderHeight : 0u;
    ctx->max_display_w = desc ? desc->maxDisplayWidth : 0u;
    ctx->max_display_h = desc ? desc->maxDisplayHeight : 0u;
    ctx->sharpness = (desc && std::isfinite(desc->defaultSharpness) &&
                      desc->defaultSharpness > 0.0f && desc->defaultSharpness <= 1.0f)
                         ? desc->defaultSharpness
                         : 0.8f;
    ctx->enable_rcas = true;

#if defined(_WIN32)
    if (desc && desc->nativeD3D12Device != nullptr) {
        auto* dev = static_cast<ID3D12Device*>(desc->nativeD3D12Device);
        dev->AddRef();
        ctx->native_device = dev;
    }
#endif

    openscarlett::Status st = (desc && desc->nativeD3D12Device != nullptr)
        ? ctx->pipeline.InitializeWithDevice(desc->nativeD3D12Device)
        : ctx->pipeline.Initialize(prefer_warp, enable_debug);

    if (!st.ok()) {
        return RecordRet("ffxUpscalerCreateContext", FFX_API_RETURN_ERROR_RUNTIME_ERROR);
    }

    FfxUpscalerContextHandle raw = ctx.get();
    {
        auto& g = GetUpscalerState();
        std::lock_guard<std::mutex> lk(g.mutex);
        g.contexts[raw] = std::move(ctx);
    }
    *outContext = raw;
    return RecordRet("ffxUpscalerCreateContext", FFX_API_RETURN_OK);
}

int32_t OS_SHIM_CALL ffxUpscalerDestroyContext(FfxUpscalerContextHandle context) {
    if (!context) {
        return RecordRet("ffxUpscalerDestroyContext", FFX_API_RETURN_ERROR_PARAMETER);
    }
    std::shared_ptr<FfxUpscalerContext> removed;
    {
        auto& g = GetUpscalerState();
        std::lock_guard<std::mutex> lk(g.mutex);
        auto it = g.contexts.find(context);
        if (it == g.contexts.end()) {
            return RecordRet("ffxUpscalerDestroyContext", FFX_API_RETURN_ERROR_PARAMETER);
        }
        removed = std::move(it->second);
        g.contexts.erase(it);
    }
    if (removed) {
        std::lock_guard<std::mutex> lk(removed->mutex);
        removed->pipeline.Shutdown();
#if defined(_WIN32)
        removed->ffx2_dx12.Release();
        if (removed->native_device) {
            removed->native_device->Release();
            removed->native_device = nullptr;
        }
#endif
    }
    return RecordRet("ffxUpscalerDestroyContext", FFX_API_RETURN_OK);
}

int32_t OS_SHIM_CALL ffxUpscalerDispatchRgba8(
    FfxUpscalerContextHandle context,
    const FfxUpscalerDispatchDesc* desc,
    FfxUpscalerDispatchResult* outResult) {
    if (!context || !desc || !desc->inputRgba8 || !desc->outputRgba8 ||
        desc->inputWidth == 0u || desc->inputHeight == 0u ||
        desc->outputWidth == 0u || desc->outputHeight == 0u) {
        return RecordRet("ffxUpscalerDispatchRgba8", FFX_API_RETURN_ERROR_PARAMETER);
    }

    const size_t required_bytes =
        static_cast<size_t>(desc->outputWidth) * static_cast<size_t>(desc->outputHeight) * 4u;
    if (desc->outputBufferBytes < required_bytes) {
        return RecordRet("ffxUpscalerDispatchRgba8", FFX_API_RETURN_ERROR_PARAMETER);
    }

    auto ctx = LookupContext(context);
    if (!ctx) {
        return RecordRet("ffxUpscalerDispatchRgba8", FFX_API_RETURN_ERROR_PARAMETER);
    }

    openscarlett::FrameBufferRgba8 out_frame{};
    openscarlett::Fsr1DispatchStats stats{};
    openscarlett::Status st{};
    {
        std::lock_guard<std::mutex> lk(ctx->mutex);
        openscarlett::Fsr1DispatchParams params{};
        params.input_rgba8 = desc->inputRgba8;
        params.input_width = desc->inputWidth;
        params.input_height = desc->inputHeight;
        params.output_width = desc->outputWidth;
        params.output_height = desc->outputHeight;
        params.sharpness = (desc->sharpness >= 0.0f)
                               ? std::clamp(desc->sharpness, 0.0f, 1.0f)
                               : ctx->sharpness;
        params.enable_rcas = (desc->enableRcas == 0xFFFFFFFFu)
                                 ? ctx->enable_rcas
                                 : (desc->enableRcas != 0u);
        st = ctx->pipeline.Dispatch(params, out_frame, &stats);
    }

    if (!st.ok() || out_frame.pixels.size() < required_bytes) {
        return RecordRet("ffxUpscalerDispatchRgba8", FFX_API_RETURN_ERROR_RUNTIME_ERROR);
    }

    std::memcpy(desc->outputRgba8, out_frame.pixels.data(), required_bytes);

    if (outResult) {
        std::memset(outResult, 0, sizeof(FfxUpscalerDispatchResult));
        outResult->dispatchCount = stats.dispatch_count;
        outResult->outputFnv1aHash = stats.output_fnv1a_hash;
        outResult->easuGroupsX = stats.easu_groups_x;
        outResult->easuGroupsY = stats.easu_groups_y;
        outResult->rcasGroupsX = stats.rcas_groups_x;
        outResult->rcasGroupsY = stats.rcas_groups_y;
        outResult->usedGpuCompute = stats.used_gpu_compute ? 1u : 0u;
        outResult->isWarp = stats.is_warp ? 1u : 0u;
        std::snprintf(outResult->adapterDescription, sizeof(outResult->adapterDescription),
                      "%s", stats.adapter_description.c_str());
    }

    return RecordRet("ffxUpscalerDispatchRgba8", FFX_API_RETURN_OK);
}

int32_t OS_SHIM_CALL ffxUpscalerConfigure(
    FfxUpscalerContextHandle context,
    const FfxUpscalerConfigDesc* config) {
    if (!context || !config) {
        return RecordRet("ffxUpscalerConfigure", FFX_API_RETURN_ERROR_PARAMETER);
    }
    auto ctx = LookupContext(context);
    if (!ctx) {
        return RecordRet("ffxUpscalerConfigure", FFX_API_RETURN_ERROR_PARAMETER);
    }
    std::lock_guard<std::mutex> lk(ctx->mutex);
    ctx->sharpness = std::clamp(config->sharpness, 0.0f, 1.0f);
    ctx->enable_rcas = (config->enableRcas != 0u);
    return RecordRet("ffxUpscalerConfigure", FFX_API_RETURN_OK);
}

int32_t OS_SHIM_CALL ffxUpscalerQuery(
    FfxUpscalerContextHandle context,
    FfxUpscalerQueryDesc* query) {
    if (!query) {
        return RecordRet("ffxUpscalerQuery", FFX_API_RETURN_ERROR_PARAMETER);
    }

    switch (query->queryType) {
        case FFX_UPSCALER_QUERY_VERSION: {
            query->version = kUpscalerVersion;
            return RecordRet("ffxUpscalerQuery", FFX_API_RETURN_OK);
        }
        case FFX_UPSCALER_QUERY_RENDER_RESOLUTION: {
            if (query->displayWidth == 0u || query->displayHeight == 0u) {
                return RecordRet("ffxUpscalerQuery", FFX_API_RETURN_ERROR_PARAMETER);
            }
            const float ratio = ScaleRatioForQualityMode(query->qualityMode);
            query->outRenderWidth = std::max<uint32_t>(
                1u, static_cast<uint32_t>(std::lround(static_cast<float>(query->displayWidth) / ratio)));
            query->outRenderHeight = std::max<uint32_t>(
                1u, static_cast<uint32_t>(std::lround(static_cast<float>(query->displayHeight) / ratio)));
            return RecordRet("ffxUpscalerQuery", FFX_API_RETURN_OK);
        }
        case FFX_UPSCALER_QUERY_ADAPTER_INFO: {
            auto ctx = LookupContext(context);
            if (!ctx) {
                return RecordRet("ffxUpscalerQuery", FFX_API_RETURN_ERROR_PARAMETER);
            }
            std::lock_guard<std::mutex> lk(ctx->mutex);
            const auto& info = ctx->pipeline.AdapterInfo();
            query->version = kUpscalerVersion;
            query->isWarp = info.is_warp ? 1u : 0u;
            std::snprintf(query->adapterDescription, sizeof(query->adapterDescription),
                          "%s", info.description.c_str());
            return RecordRet("ffxUpscalerQuery", FFX_API_RETURN_OK);
        }
        default:
            return RecordRet("ffxUpscalerQuery", FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
    }
}

// ===========================================================================
// Real AMD FidelityFX API 2.0 C ABI Exports (aliased in PE export directory
// as ffxCreateContext / ffxDestroyContext / ffxConfigure / ffxQuery / ffxDispatch)
// ===========================================================================
int32_t OS_SHIM_CALL FfxUpscaler_Api2_Query(
    FfxContext* context,
    FfxQueryDescHeader* header) {
    if (!header) {
        return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
    }

    FfxApi2Context* api_ctx = nullptr;
    if (context && *context) {
        api_ctx = static_cast<FfxApi2Context*>(*context);
        if (!IsLiveApi2Context(api_ctx)) {
            return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
        }
    }

    bool handled_any = false;
    for (FfxQueryDescHeader* cur = header; cur != nullptr; cur = cur->pNext) {
        switch (cur->type) {
            case kFfx2QueryDescTypeGetVersions: {
                auto* q = reinterpret_cast<FfxQueryDescGetVersions*>(cur);
                if (!q->outputCount) {
                    return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                }
                const uint64_t capacity = *q->outputCount;
                *q->outputCount = 1ull;
                if (q->versionIds && capacity >= 1ull) {
                    q->versionIds[0] = static_cast<uint64_t>(kUpscalerVersion);
                }
                if (q->versionNames && capacity >= 1ull) {
                    q->versionNames[0] = kProviderVersionName;
                }
                handled_any = true;
                break;
            }
            case kFfx2QueryDescTypeUpscaleGetRatio: {
                // Disambiguate legacy OpenScarlett FFX_API_QUERY_DESC_TYPE_GET_VERSIONS (0x00010002)
                // where offset 16 is createDescType (>= 0x10000) vs real AMD FFX 2.0
                // FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE (0x00010002)
                // where offset 16 is uint32_t qualityMode (< 0x100).
                const auto* as_ver = reinterpret_cast<const FfxQueryDescGetVersions*>(cur);
                if (as_ver->createDescType >= 0x10000ull) {
                    auto* q = reinterpret_cast<FfxQueryDescGetVersions*>(cur);
                    if (!q->outputCount) {
                        return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                    }
                    const uint64_t capacity = *q->outputCount;
                    *q->outputCount = 1ull;
                    if (q->versionIds && capacity >= 1ull) {
                        q->versionIds[0] = static_cast<uint64_t>(kUpscalerVersion);
                    }
                    if (q->versionNames && capacity >= 1ull) {
                        q->versionNames[0] = kProviderVersionName;
                    }
                } else {
                    auto* q = reinterpret_cast<Ffx2QueryDescUpscaleGetRatio*>(cur);
                    if (!q->pOutUpscaleRatio) {
                        return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                    }
                    *q->pOutUpscaleRatio = ScaleRatioForQualityMode(q->qualityMode);
                }
                handled_any = true;
                break;
            }
            case kFfx2QueryDescTypeUpscaleGetRenderRes: {
                auto* q = reinterpret_cast<FfxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(cur);
                if (!q->pOutRenderWidth || !q->pOutRenderHeight ||
                    q->displayWidth == 0u || q->displayHeight == 0u) {
                    return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                }
                const float ratio = ScaleRatioForQualityMode(q->qualityMode);
                *q->pOutRenderWidth = std::max<uint32_t>(
                    1u, static_cast<uint32_t>(std::lround(static_cast<float>(q->displayWidth) / ratio)));
                *q->pOutRenderHeight = std::max<uint32_t>(
                    1u, static_cast<uint32_t>(std::lround(static_cast<float>(q->displayHeight) / ratio)));
                handled_any = true;
                break;
            }
            case kFfx2QueryDescTypeUpscaleGetJitterPhaseCount: {
                // Disambiguate real AMD FFX 2.0 GETJITTERPHASECOUNT (0x00010004)
                // where renderWidth > 0 && displayWidth > 0 && pOutPhaseCount != nullptr
                // vs legacy OpenScarlett GETADAPTERINFO (0x00010004) where fields are zero-initialized.
                auto* jq = reinterpret_cast<Ffx2QueryDescUpscaleGetJitterPhaseCount*>(cur);
                if (jq->renderWidth > 0 && jq->displayWidth > 0 && jq->pOutPhaseCount != nullptr) {
                    const double ratio =
                        static_cast<double>(jq->displayWidth) / static_cast<double>(jq->renderWidth);
                    int32_t phases = static_cast<int32_t>(std::ceil(8.0 * ratio * ratio));
                    if (phases < 1) phases = 1;
                    *jq->pOutPhaseCount = phases;
                    handled_any = true;
                    break;
                }
                if (!api_ctx || !api_ctx->upscaler) {
                    return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                }
                auto* aq = reinterpret_cast<FfxQueryDescUpscaleGetAdapterInfo*>(cur);
                FfxUpscalerQueryDesc uq{};
                uq.queryType = FFX_UPSCALER_QUERY_ADAPTER_INFO;
                const int32_t rc = ffxUpscalerQuery(api_ctx->upscaler, &uq);
                if (rc != FFX_API_RETURN_OK) {
                    return RecordRet("ffxQuery", rc);
                }
                aq->outIsWarp = uq.isWarp;
                std::memcpy(aq->adapterDescription, uq.adapterDescription,
                            sizeof(aq->adapterDescription));
                handled_any = true;
                break;
            }
            case kFfx2QueryDescTypeUpscaleGetJitterOffset: {
                auto* q = reinterpret_cast<Ffx2QueryDescUpscaleGetJitterOffset*>(cur);
                if (q->phaseCount <= 0 || !q->pOutX || !q->pOutY) {
                    return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                }
                const int32_t wrapped = ((q->index % q->phaseCount) + q->phaseCount) % q->phaseCount;
                const uint32_t sample = static_cast<uint32_t>(wrapped + 1);
                *q->pOutX = ComputeHalton(sample, 2u) - 0.5f;
                *q->pOutY = ComputeHalton(sample, 3u) - 0.5f;
                handled_any = true;
                break;
            }
            default:
                return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
        }
    }

    return RecordRet("ffxQuery", handled_any ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_PARAMETER);
}

int32_t OS_SHIM_CALL FfxUpscaler_Api2_CreateContext(
    FfxContext* context,
    FfxCreateContextDescHeader* desc,
    const FfxAllocationCallbacks* memCb) {
    if (!context || !desc) {
        return RecordRet("ffxCreateContext", FFX_API_RETURN_ERROR_PARAMETER);
    }
    *context = nullptr;

    const FfxCreateContextDescUpscale* upscale_desc = nullptr;
    const FfxCreateBackendDX12Desc* dx12_desc = nullptr;

    for (const FfxCreateContextDescHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type == kFfx2CreateDescTypeUpscale) {
            upscale_desc = reinterpret_cast<const FfxCreateContextDescUpscale*>(cur);
        } else if (cur->type == kFfx2DescTypeBackendDx12) {
            dx12_desc = reinterpret_cast<const FfxCreateBackendDX12Desc*>(cur);
        } else if (cur->type == kFfx2DescTypeOverrideVersion) {
            // Optional version override header accepted cleanly.
        } else {
            return RecordRet("ffxCreateContext", FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
        }
    }

    if (!upscale_desc) {
        return RecordRet("ffxCreateContext", FFX_API_RETURN_NO_PROVIDER);
    }

    FfxUpscalerCreateDesc udesc{};
    udesc.maxRenderWidth = upscale_desc->maxRenderWidth;
    udesc.maxRenderHeight = upscale_desc->maxRenderHeight;
    udesc.maxDisplayWidth = upscale_desc->maxUpscaleWidth;
    udesc.maxDisplayHeight = upscale_desc->maxUpscaleHeight;
    udesc.flags = upscale_desc->flags;
    udesc.defaultSharpness =
        (std::isfinite(upscale_desc->defaultSharpness) &&
         upscale_desc->defaultSharpness > 0.0f && upscale_desc->defaultSharpness <= 1.0f)
            ? upscale_desc->defaultSharpness
            : 0.8f;
    udesc.nativeD3D12Device = dx12_desc ? dx12_desc->device : nullptr;

    FfxUpscalerContextHandle upscaler = nullptr;
    const int32_t rc = ffxUpscalerCreateContext(&upscaler, &udesc);
    if (rc != FFX_API_RETURN_OK || !upscaler) {
        return RecordRet("ffxCreateContext", rc != FFX_API_RETURN_OK ? rc : FFX_API_RETURN_ERROR_RUNTIME_ERROR);
    }

    FfxApi2Context* ctx = nullptr;
    bool custom_alloc = false;
    if (memCb && memCb->alloc) {
        void* raw = memCb->alloc(memCb->pUserData, sizeof(FfxApi2Context));
        if (raw) {
            ctx = new (raw) FfxApi2Context();
            custom_alloc = true;
        }
    } else {
        ctx = new (std::nothrow) FfxApi2Context();
    }

    if (!ctx) {
        ffxUpscalerDestroyContext(upscaler);
        return RecordRet("ffxCreateContext", FFX_API_RETURN_ERROR_MEMORY);
    }

    ctx->upscaler = upscaler;
    ctx->sharpness = udesc.defaultSharpness;
    ctx->enable_rcas = 1u;
    ctx->custom_allocated = custom_alloc;

    {
        auto& st = GetUpscalerState();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.api2_contexts.insert(ctx);
    }

    *context = ctx;
    return RecordRet("ffxCreateContext", FFX_API_RETURN_OK);
}

int32_t OS_SHIM_CALL FfxUpscaler_Api2_DestroyContext(
    FfxContext* context,
    const FfxAllocationCallbacks* memCb) {
    if (!context || !*context) {
        return RecordRet("ffxDestroyContext", FFX_API_RETURN_ERROR_PARAMETER);
    }
    auto* ctx = static_cast<FfxApi2Context*>(*context);
    {
        auto& st = GetUpscalerState();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.api2_contexts.find(ctx);
        if (it == st.api2_contexts.end()) {
            return RecordRet("ffxDestroyContext", FFX_API_RETURN_ERROR_PARAMETER);
        }
        st.api2_contexts.erase(it);
    }

    const int32_t rc = ffxUpscalerDestroyContext(ctx->upscaler);
    const bool was_custom = ctx->custom_allocated;
    if (was_custom && memCb && memCb->dealloc) {
        ctx->~FfxApi2Context();
        memCb->dealloc(memCb->pUserData, ctx);
    } else if (!was_custom) {
        delete ctx;
    }
    *context = nullptr;
    return RecordRet("ffxDestroyContext", rc);
}

int32_t OS_SHIM_CALL FfxUpscaler_Api2_Configure(
    FfxContext* context,
    const FfxConfigureDescHeader* desc) {
    if (!context || !*context || !desc) {
        return RecordRet("ffxConfigure", FFX_API_RETURN_ERROR_PARAMETER);
    }
    auto* ctx = static_cast<FfxApi2Context*>(*context);
    if (!IsLiveApi2Context(ctx)) {
        return RecordRet("ffxConfigure", FFX_API_RETURN_ERROR_PARAMETER);
    }

    bool handled_any = false;
    for (const FfxConfigureDescHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type != FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE &&
            cur->type != kFfx2ConfigureDescTypeUpscaleKeyValue) {
            return RecordRet("ffxConfigure", FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
        }
        const auto* kv = reinterpret_cast<const FfxConfigureDescUpscaleKeyValue*>(cur);
        if (kv->key == FFX_UPSCALE_CONFIGURE_KEY_SHARPNESS) {
            if (kv->ptr) {
                ctx->sharpness = *static_cast<const float*>(kv->ptr);
            } else {
                const uint32_t bits = static_cast<uint32_t>(kv->u64Value & 0xFFFFFFFFull);
                std::memcpy(&ctx->sharpness, &bits, sizeof(float));
            }
            handled_any = true;
        } else if (kv->key == FFX_UPSCALE_CONFIGURE_KEY_ENABLE_RCAS) {
            ctx->enable_rcas = kv->u64Value != 0ull ? 1u : 0u;
            handled_any = true;
        } else if (cur->type == kFfx2ConfigureDescTypeUpscaleKeyValue && kv->key <= 16ull) {
            // Standard AMD FFX 2.x upscale key-value keys (e.g. reactive mask / debug flags).
            handled_any = true;
        } else {
            return RecordRet("ffxConfigure", FFX_API_RETURN_ERROR_PARAMETER);
        }
    }

    if (!handled_any) {
        return RecordRet("ffxConfigure", FFX_API_RETURN_ERROR_PARAMETER);
    }

    FfxUpscalerConfigDesc cfg{};
    cfg.sharpness = ctx->sharpness;
    cfg.enableRcas = ctx->enable_rcas;
    const int32_t rc = ffxUpscalerConfigure(ctx->upscaler, &cfg);
    return RecordRet("ffxConfigure", rc);
}

int32_t OS_SHIM_CALL FfxUpscaler_Api2_Dispatch(
    FfxContext* context,
    const FfxDispatchDescHeader* desc) {
    if (!context || !*context || !desc) {
        return RecordRet("ffxDispatch", FFX_API_RETURN_ERROR_PARAMETER);
    }
    auto* ctx = static_cast<FfxApi2Context*>(*context);
    if (!IsLiveApi2Context(ctx)) {
        return RecordRet("ffxDispatch", FFX_API_RETURN_ERROR_PARAMETER);
    }

    bool dispatched = false;
    for (const FfxDispatchDescHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type != kFfx2DispatchDescTypeUpscale) {
            return RecordRet("ffxDispatch", FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
        }

        if (LooksLikeLegacyRgba8Dispatch(cur)) {
            const auto* d = reinterpret_cast<const FfxDispatchDescUpscale*>(cur);
            FfxUpscalerDispatchDesc udisp{};
            udisp.inputRgba8 = d->inputRgba8;
            udisp.inputWidth = d->inputWidth;
            udisp.inputHeight = d->inputHeight;
            udisp.outputRgba8 = d->outputRgba8;
            udisp.outputBufferBytes = d->outputBufferBytes;
            udisp.outputWidth = d->outputWidth;
            udisp.outputHeight = d->outputHeight;
            udisp.sharpness = d->sharpness;
            udisp.enableRcas = d->enableRcas;

            const int32_t rc = ffxUpscalerDispatchRgba8(ctx->upscaler, &udisp, d->outResult);
            if (rc != FFX_API_RETURN_OK) {
                return RecordRet("ffxDispatch", rc);
            }
            dispatched = true;
        } else {
            const auto* d2 = reinterpret_cast<const Ffx2DispatchDescUpscale*>(cur);
            auto upscaler_ctx = LookupContext(ctx->upscaler);
            if (!upscaler_ctx) {
                return RecordRet("ffxDispatch", FFX_API_RETURN_ERROR_PARAMETER);
            }
            const int32_t rc = DispatchFfx2Dx12(*upscaler_ctx, *d2);
            if (rc != FFX_API_RETURN_OK) {
                return RecordRet("ffxDispatch", rc);
            }
            dispatched = true;
        }
    }

    return RecordRet("ffxDispatch", dispatched ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_PARAMETER);
}

}  // extern "C"
