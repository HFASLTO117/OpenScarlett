// MIT Media Foundation HEVC Decoder (mfhevcdec.dll) for OpenScarlett.
// Portable and 0-warning clean across MSVC (/W4), Clang-CL, and MinGW-w64.

#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#endif

#if defined(_WIN32)
#  define OS_MFHEVC_API __declspec(dllexport)
#  define OS_MFHEVC_CALL __stdcall
#  if defined(_MSC_VER)
#    define OS_MFHEVC_COM_API
#  else
#    define OS_MFHEVC_COM_API __declspec(dllexport)
#  endif
#else
#  define OS_MFHEVC_API __attribute__((visibility("default")))
#  define OS_MFHEVC_COM_API __attribute__((visibility("default")))
#  define OS_MFHEVC_CALL
#endif

namespace {

constexpr const char* kMod = "mfhevcdec.dll";
constexpr int32_t kHrOk = 0x00000000;
constexpr int32_t kHrNoInterface = static_cast<int32_t>(0x80004002u);
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHrClassNoAggregation = static_cast<int32_t>(0x80040110u);

int32_t QueryModTelemetry(const char* symbol_name,
                          uint64_t* out_total,
                          uint64_t* out_ok,
                          uint64_t* out_not_impl) {
    if (out_total) *out_total = 0u;
    if (out_ok) *out_ok = 0u;
    if (out_not_impl) *out_not_impl = 0u;
    if (!symbol_name) {
        return 0;
    }
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == kMod && entry.symbol_name == symbol_name) {
            if (out_total) *out_total = entry.total_calls;
            if (out_ok) *out_ok = entry.ok_calls;
            if (out_not_impl) *out_not_impl = entry.not_impl_calls;
            return 1;
        }
    }
    return 0;
}

int32_t QueryModTotals(uint64_t* out_total_calls, uint64_t* out_not_impl_calls) {
    uint64_t total = 0u;
    uint64_t not_impl = 0u;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == kMod) {
            total += entry.total_calls;
            not_impl += entry.not_impl_calls;
        }
    }
    if (out_total_calls) *out_total_calls = total;
    if (out_not_impl_calls) *out_not_impl_calls = not_impl;
    return 1;
}

inline int32_t RecordMod(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

struct HevcGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};

constexpr HevcGuid kIidUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr HevcGuid kIidClassFactory = {0x00000001u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

bool GuidEq(const void* a, const HevcGuid& b) noexcept {
    return a && std::memcmp(a, &b, sizeof(HevcGuid)) == 0;
}

struct HevcStubObject {
    virtual int32_t OS_MFHEVC_CALL QueryInterface(const void* riid, void** ppv) {
        if (!ppv) return RecordMod("IMFHevcDecoder::QueryInterface", kHrPointer);
        *ppv = nullptr;
        if (!riid) return RecordMod("IMFHevcDecoder::QueryInterface", kHrInvalidArg);
        AddRef();
        *ppv = this;
        return RecordMod("IMFHevcDecoder::QueryInterface", kHrOk);
    }
    virtual uint32_t OS_MFHEVC_CALL AddRef() {
        RecordMod("IMFHevcDecoder::AddRef", kHrOk);
        return ref_count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    virtual uint32_t OS_MFHEVC_CALL Release() {
        RecordMod("IMFHevcDecoder::Release", kHrOk);
        const uint32_t rem = ref_count.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }
    virtual ~HevcStubObject() = default;
    std::atomic<uint32_t> ref_count{1u};
};

struct HevcClassFactory {
    virtual int32_t OS_MFHEVC_CALL QueryInterface(const void* riid, void** ppv) {
        if (!ppv) return RecordMod("HevcClassFactory::QueryInterface", kHrPointer);
        *ppv = nullptr;
        if (!riid) return RecordMod("HevcClassFactory::QueryInterface", kHrInvalidArg);
        if (GuidEq(riid, kIidUnknown) || GuidEq(riid, kIidClassFactory)) {
            AddRef();
            *ppv = this;
            return RecordMod("HevcClassFactory::QueryInterface", kHrOk);
        }
        return RecordMod("HevcClassFactory::QueryInterface", kHrNoInterface);
    }
    virtual uint32_t OS_MFHEVC_CALL AddRef() {
        RecordMod("HevcClassFactory::AddRef", kHrOk);
        return ref_count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    virtual uint32_t OS_MFHEVC_CALL Release() {
        RecordMod("HevcClassFactory::Release", kHrOk);
        const uint32_t cur = ref_count.load(std::memory_order_acquire);
        return cur > 1u ? ref_count.fetch_sub(1u, std::memory_order_acq_rel) - 1u : 1u;
    }
    virtual int32_t OS_MFHEVC_CALL CreateInstance(void* pUnkOuter, const void* riid, void** ppvObject) {
        if (!ppvObject) return RecordMod("HevcClassFactory::CreateInstance", kHrPointer);
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) {
            return RecordMod("HevcClassFactory::CreateInstance", kHrClassNoAggregation);
        }
        auto* obj = new HevcStubObject();
        const int32_t hr = obj->QueryInterface(riid ? riid : &kIidUnknown, ppvObject);
        obj->Release();
        return RecordMod("HevcClassFactory::CreateInstance", hr);
    }
    virtual int32_t OS_MFHEVC_CALL LockServer(int32_t fLock) {
        if (fLock) {
            lock_count.fetch_add(1u, std::memory_order_relaxed);
        } else {
            uint32_t cur = lock_count.load(std::memory_order_relaxed);
            if (cur > 0u) {
                lock_count.fetch_sub(1u, std::memory_order_relaxed);
            }
        }
        return RecordMod("HevcClassFactory::LockServer", kHrOk);
    }
    std::atomic<uint32_t> ref_count{1u};
    std::atomic<uint32_t> lock_count{0u};
};

HevcClassFactory& GetFactory() {
    static HevcClassFactory* s = new HevcClassFactory();
    return *s;
}

std::atomic<uint32_t> g_hevc_init_count{0u};

}  // namespace

extern "C" {

#if defined(_MSC_VER)
#  pragma comment(linker, "/export:DllGetClassObject,PRIVATE")
#  pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#  pragma comment(linker, "/export:DllRegisterServer,PRIVATE")
#  pragma comment(linker, "/export:DllUnregisterServer,PRIVATE")
#endif

OS_MFHEVC_COM_API int32_t OS_MFHEVC_CALL DllGetClassObject(const void* rclsid, const void* riid, void** ppv) {
    (void)rclsid;
    if (!ppv) {
        return RecordMod("DllGetClassObject", kHrPointer);
    }
    *ppv = nullptr;
    auto& factory = GetFactory();
    const int32_t hr = factory.QueryInterface(riid ? riid : &kIidClassFactory, ppv);
    return RecordMod("DllGetClassObject", hr);
}

OS_MFHEVC_COM_API int32_t OS_MFHEVC_CALL DllCanUnloadNow(void) {
    return RecordMod("DllCanUnloadNow", kHrOk);
}

OS_MFHEVC_COM_API int32_t OS_MFHEVC_CALL DllRegisterServer(void) {
    return RecordMod("DllRegisterServer", kHrOk);
}

OS_MFHEVC_COM_API int32_t OS_MFHEVC_CALL DllUnregisterServer(void) {
    return RecordMod("DllUnregisterServer", kHrOk);
}

OS_MFHEVC_API int32_t OS_MFHEVC_CALL MFHevcDecoderInitialize(uint32_t flags) {
    (void)flags;
    g_hevc_init_count.fetch_add(1u, std::memory_order_relaxed);
    return RecordMod("MFHevcDecoderInitialize", kHrOk);
}

OS_MFHEVC_API int32_t OS_MFHEVC_CALL MFHevcDecoderShutdown(void) {
    uint32_t cur = g_hevc_init_count.load(std::memory_order_relaxed);
    if (cur > 0u) {
        g_hevc_init_count.fetch_sub(1u, std::memory_order_relaxed);
    }
    return RecordMod("MFHevcDecoderShutdown", kHrOk);
}

OS_MFHEVC_API int32_t OS_MFHEVC_CALL MFHevcDecoderGetCapabilities(
    uint32_t* outProfileFlags,
    uint32_t* outMaxWidth,
    uint32_t* outMaxHeight) {
    if (outProfileFlags) *outProfileFlags = 0x3u;
    if (outMaxWidth) *outMaxWidth = 3840u;
    if (outMaxHeight) *outMaxHeight = 2160u;
    return RecordMod("MFHevcDecoderGetCapabilities", kHrOk);
}

OS_MFHEVC_API int32_t OS_MFHEVC_CALL OpenScarlettMfHevcDecGetTelemetry(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls,
    uint64_t* outNotImplCalls) {
    if (!symbolName) {
        return QueryModTotals(outTotalCalls, outNotImplCalls);
    }
    return QueryModTelemetry(symbolName, outTotalCalls, outOkCalls, outNotImplCalls);
}

}  // extern "C"
