#define OPENSCARLETT_BUILDING_FFX_UPSCALER 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/framebuffer.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/middleware/ffx_gpu_fsr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

struct FfxUpscalerContext {
    std::mutex mutex;
    openscarlett::FfxGpuFsrPipeline pipeline;
    uint32_t max_render_w = 0;
    uint32_t max_render_h = 0;
    uint32_t max_display_w = 0;
    uint32_t max_display_h = 0;
    float sharpness = 0.8f;
    bool enable_rcas = true;
};

namespace {

constexpr const char* kModName = "amd_fidelityfx_upscaler_xbox.dll";
constexpr uint32_t kUpscalerVersion = 0x02010000u;  // 2.1.0

struct UpscalerState {
    std::mutex mutex;
    std::unordered_map<FfxUpscalerContextHandle, std::shared_ptr<FfxUpscalerContext>> contexts;
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
    ctx->sharpness = (desc && desc->defaultSharpness > 0.0f)
                         ? std::clamp(desc->defaultSharpness, 0.0f, 1.0f)
                         : 0.8f;
    ctx->enable_rcas = true;

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

}  // extern "C"
