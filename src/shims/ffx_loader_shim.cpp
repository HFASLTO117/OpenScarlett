#define OPENSCARLETT_BUILDING_FFX_LOADER 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"

#include <cstring>
#include <mutex>
#include <new>
#include <unordered_set>

namespace {

constexpr const char* kModName = "amd_fidelityfx_loader_xbox.dll";
constexpr const char* kProviderVersionName =
    "OpenScarlett FidelityFX Upscaler 2.1 (GPU FSR1 EASU+RCAS)";

struct FfxLoaderContext {
    FfxUpscalerContextHandle upscaler = nullptr;
    float sharpness = 0.8f;
    uint32_t enable_rcas = 1u;
    bool custom_allocated = false;
};

struct LoaderState {
    std::mutex mutex;
    std::unordered_set<FfxLoaderContext*> live_contexts;
};

LoaderState& GetLoaderState() {
    static LoaderState* s = new LoaderState();
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

bool IsLiveContext(FfxLoaderContext* ctx) {
    if (!ctx) return false;
    auto& st = GetLoaderState();
    std::lock_guard<std::mutex> lk(st.mutex);
    return st.live_contexts.find(ctx) != st.live_contexts.end();
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CALL ffxQuery(FfxContext* context, FfxQueryDescHeader* header) {
    if (!header) {
        return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
    }

    FfxLoaderContext* loader_ctx = nullptr;
    if (context && *context) {
        loader_ctx = static_cast<FfxLoaderContext*>(*context);
        if (!IsLiveContext(loader_ctx)) {
            return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
        }
    }

    bool handled_any = false;
    for (FfxQueryDescHeader* cur = header; cur != nullptr; cur = cur->pNext) {
        switch (cur->type) {
            case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
                auto* q = reinterpret_cast<FfxQueryDescGetVersions*>(cur);
                if (!q->outputCount) {
                    return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                }
                const uint64_t capacity = *q->outputCount;
                *q->outputCount = 1ull;
                if (q->versionIds && capacity >= 1ull) {
                    q->versionIds[0] = static_cast<uint64_t>(ffxUpscalerGetVersion());
                }
                if (q->versionNames && capacity >= 1ull) {
                    q->versionNames[0] = kProviderVersionName;
                }
                handled_any = true;
                break;
            }
            case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
                auto* q = reinterpret_cast<FfxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(cur);
                if (!q->pOutRenderWidth || !q->pOutRenderHeight) {
                    return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                }
                FfxUpscalerQueryDesc uq{};
                uq.queryType = FFX_UPSCALER_QUERY_RENDER_RESOLUTION;
                uq.displayWidth = q->displayWidth;
                uq.displayHeight = q->displayHeight;
                uq.qualityMode = q->qualityMode;
                const int32_t rc = ffxUpscalerQuery(
                    loader_ctx ? loader_ctx->upscaler : nullptr, &uq);
                if (rc != FFX_API_RETURN_OK) {
                    return RecordRet("ffxQuery", rc);
                }
                *q->pOutRenderWidth = uq.outRenderWidth;
                *q->pOutRenderHeight = uq.outRenderHeight;
                handled_any = true;
                break;
            }
            case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETADAPTERINFO: {
                if (!loader_ctx || !loader_ctx->upscaler) {
                    return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_PARAMETER);
                }
                auto* q = reinterpret_cast<FfxQueryDescUpscaleGetAdapterInfo*>(cur);
                FfxUpscalerQueryDesc uq{};
                uq.queryType = FFX_UPSCALER_QUERY_ADAPTER_INFO;
                const int32_t rc = ffxUpscalerQuery(loader_ctx->upscaler, &uq);
                if (rc != FFX_API_RETURN_OK) {
                    return RecordRet("ffxQuery", rc);
                }
                q->outIsWarp = uq.isWarp;
                std::memcpy(q->adapterDescription, uq.adapterDescription,
                            sizeof(q->adapterDescription));
                handled_any = true;
                break;
            }
            default:
                return RecordRet("ffxQuery", FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
        }
    }

    return RecordRet("ffxQuery", handled_any ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_PARAMETER);
}

int32_t OS_SHIM_CALL ffxCreateContext(
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
        if (cur->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
            upscale_desc = reinterpret_cast<const FfxCreateContextDescUpscale*>(cur);
        } else if (cur->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12) {
            dx12_desc = reinterpret_cast<const FfxCreateBackendDX12Desc*>(cur);
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
    udesc.defaultSharpness = upscale_desc->defaultSharpness;
    udesc.nativeD3D12Device = dx12_desc ? dx12_desc->device : nullptr;

    FfxUpscalerContextHandle upscaler = nullptr;
    const int32_t rc = ffxUpscalerCreateContext(&upscaler, &udesc);
    if (rc != FFX_API_RETURN_OK || !upscaler) {
        return RecordRet("ffxCreateContext", rc != FFX_API_RETURN_OK ? rc : FFX_API_RETURN_ERROR_RUNTIME_ERROR);
    }

    FfxLoaderContext* ctx = nullptr;
    bool custom_alloc = false;
    if (memCb && memCb->alloc) {
        void* raw = memCb->alloc(memCb->pUserData, sizeof(FfxLoaderContext));
        if (raw) {
            ctx = new (raw) FfxLoaderContext();
            custom_alloc = true;
        }
    } else {
        ctx = new (std::nothrow) FfxLoaderContext();
    }

    if (!ctx) {
        ffxUpscalerDestroyContext(upscaler);
        return RecordRet("ffxCreateContext", FFX_API_RETURN_ERROR_MEMORY);
    }

    ctx->upscaler = upscaler;
    ctx->sharpness = upscale_desc->defaultSharpness > 0.0f ? upscale_desc->defaultSharpness : 0.8f;
    ctx->enable_rcas = 1u;
    ctx->custom_allocated = custom_alloc;

    {
        auto& st = GetLoaderState();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.live_contexts.insert(ctx);
    }

    *context = ctx;
    return RecordRet("ffxCreateContext", FFX_API_RETURN_OK);
}

int32_t OS_SHIM_CALL ffxDestroyContext(
    FfxContext* context,
    const FfxAllocationCallbacks* memCb) {
    if (!context || !*context) {
        return RecordRet("ffxDestroyContext", FFX_API_RETURN_ERROR_PARAMETER);
    }
    auto* ctx = static_cast<FfxLoaderContext*>(*context);
    {
        auto& st = GetLoaderState();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.live_contexts.find(ctx);
        if (it == st.live_contexts.end()) {
            return RecordRet("ffxDestroyContext", FFX_API_RETURN_ERROR_PARAMETER);
        }
        st.live_contexts.erase(it);
    }

    const int32_t rc = ffxUpscalerDestroyContext(ctx->upscaler);
    const bool was_custom = ctx->custom_allocated;
    if (was_custom && memCb && memCb->dealloc) {
        ctx->~FfxLoaderContext();
        memCb->dealloc(memCb->pUserData, ctx);
    } else if (!was_custom) {
        delete ctx;
    }
    *context = nullptr;
    return RecordRet("ffxDestroyContext", rc);
}

int32_t OS_SHIM_CALL ffxConfigure(
    FfxContext* context,
    const FfxConfigureDescHeader* desc) {
    if (!context || !*context || !desc) {
        return RecordRet("ffxConfigure", FFX_API_RETURN_ERROR_PARAMETER);
    }
    auto* ctx = static_cast<FfxLoaderContext*>(*context);
    if (!IsLiveContext(ctx)) {
        return RecordRet("ffxConfigure", FFX_API_RETURN_ERROR_PARAMETER);
    }

    bool handled_any = false;
    for (const FfxConfigureDescHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type != FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE) {
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

int32_t OS_SHIM_CALL ffxDispatch(
    FfxContext* context,
    const FfxDispatchDescHeader* desc) {
    if (!context || !*context || !desc) {
        return RecordRet("ffxDispatch", FFX_API_RETURN_ERROR_PARAMETER);
    }
    auto* ctx = static_cast<FfxLoaderContext*>(*context);
    if (!IsLiveContext(ctx)) {
        return RecordRet("ffxDispatch", FFX_API_RETURN_ERROR_PARAMETER);
    }

    bool dispatched = false;
    for (const FfxDispatchDescHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
            return RecordRet("ffxDispatch", FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
        }
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
    }

    return RecordRet("ffxDispatch", dispatched ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_PARAMETER);
}

}  // extern "C"
