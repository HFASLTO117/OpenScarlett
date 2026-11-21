// Clean-room MIT Microsoft Media Foundation Platform (mfplat_xs.dll) shim for OpenScarlett.
// Provides Xbox-specific MFResetDXGIDeviceManagerX plus dynamic forwarding to host
// mfplat.dll with clean local fallbacks and CallTelemetry instrumentation.

#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
#  include <windows.h>
#  include <mfobjects.h>
#  include <mftransform.h>
#  include <objbase.h>
#endif

#if defined(_WIN32)
#  define OS_MFPLAT_API __declspec(dllexport)
#  define OS_MFPLAT_CALL __stdcall
#else
#  define OS_MFPLAT_API __attribute__((visibility("default")))
#  define OS_MFPLAT_CALL
#endif

namespace {

constexpr const char* kModName = "mfplat_xs.dll";

constexpr int32_t kHrOk = 0x00000000;
constexpr int32_t kHrNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kHrNoInterface = static_cast<int32_t>(0x80004002u);
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);
constexpr int32_t kHrOutOfMemory = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);

inline int32_t RecordHr(const char* sym, int32_t hr) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == kHrNotImpl) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (hr < 0) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(kModName, sym, outcome, hr, -1);
    return hr;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

#if defined(_WIN32)

// Inline COM GUIDs (no mfuuid.lib or __uuidof dependency -> portable across MSVC/Clang-CL/MinGW).
constexpr GUID kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidImfDxgiDeviceManager = {
    0xEB533D5Du, 0x2DB6u, 0x40F8u, {0x97, 0xA9, 0x49, 0x46, 0x92, 0x01, 0x4F, 0x07}};

using PfnMFStartup = HRESULT(STDAPICALLTYPE*)(ULONG, DWORD);
using PfnMFShutdown = HRESULT(STDAPICALLTYPE*)(void);
using PfnMFCreateDXGIDeviceManager = HRESULT(STDAPICALLTYPE*)(UINT*, IMFDXGIDeviceManager**);
using PfnMFCreateMediaType = HRESULT(STDAPICALLTYPE*)(IMFMediaType**);
using PfnMFCreateSample = HRESULT(STDAPICALLTYPE*)(IMFSample**);
using PfnMFCreateMemoryBuffer = HRESULT(STDAPICALLTYPE*)(DWORD, IMFMediaBuffer**);
using PfnMFCreateAttributes = HRESULT(STDAPICALLTYPE*)(IMFAttributes**, UINT32);
using PfnMFCreateAsyncResult = HRESULT(STDAPICALLTYPE*)(IUnknown*, IMFAsyncCallback*, IUnknown*, IMFAsyncResult**);
using PfnMFAllocateWorkQueue = HRESULT(STDAPICALLTYPE*)(DWORD*);
using PfnMFUnlockWorkQueue = HRESULT(STDAPICALLTYPE*)(DWORD);
using PfnMFLockPlatform = HRESULT(STDAPICALLTYPE*)(void);
using PfnMFUnlockPlatform = HRESULT(STDAPICALLTYPE*)(void);
using PfnMFGetSystemTime = LONGLONG(STDAPICALLTYPE*)(void);
using PfnMFTEnumEx = HRESULT(STDAPICALLTYPE*)(
    GUID,
    UINT32,
    const MFT_REGISTER_TYPE_INFO*,
    const MFT_REGISTER_TYPE_INFO*,
    IMFActivate***,
    UINT32*);

// Clean-room fallback IMFDXGIDeviceManager used only if host mfplat.dll is absent.
class FallbackDxgiDeviceManager final : public IMFDXGIDeviceManager {
public:
    explicit FallbackDxgiDeviceManager(UINT token) : reset_token_(token) {}

    ~FallbackDxgiDeviceManager() {
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (std::memcmp(&riid, &kIidIUnknown, sizeof(GUID)) == 0 ||
            std::memcmp(&riid, &kIidImfDxgiDeviceManager, sizeof(GUID)) == 0) {
            AddRef();
            *ppvObject = static_cast<IMFDXGIDeviceManager*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    HRESULT STDMETHODCALLTYPE CloseDeviceHandle(HANDLE hDevice) override {
        if (!hDevice) return E_HANDLE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetVideoService(HANDLE hDevice, REFIID riid, void** ppService) override {
        if (!hDevice || !ppService) return E_INVALIDARG;
        *ppService = nullptr;
        std::lock_guard<std::mutex> lk(mutex_);
        if (!device_) return E_HANDLE;
        return device_->QueryInterface(riid, ppService);
    }

    HRESULT STDMETHODCALLTYPE LockDevice(
        HANDLE hDevice, REFIID riid, void** ppUnkDevice, BOOL /*fBlock*/) override {
        if (!hDevice || !ppUnkDevice) return E_INVALIDARG;
        *ppUnkDevice = nullptr;
        std::lock_guard<std::mutex> lk(mutex_);
        if (!device_) return E_HANDLE;
        return device_->QueryInterface(riid, ppUnkDevice);
    }

    HRESULT STDMETHODCALLTYPE OpenDeviceHandle(HANDLE* phDevice) override {
        if (!phDevice) return E_POINTER;
        *phDevice = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5CA00001u));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResetDevice(IUnknown* pUnkDevice, UINT resetToken) override {
        if (!pUnkDevice) return E_INVALIDARG;
        if (resetToken != reset_token_) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(mutex_);
        pUnkDevice->AddRef();
        if (device_) {
            device_->Release();
        }
        device_ = pUnkDevice;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE TestDevice(HANDLE hDevice) override {
        if (!hDevice) return E_HANDLE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockDevice(HANDLE hDevice, BOOL /*fSaveState*/) override {
        if (!hDevice) return E_HANDLE;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    UINT reset_token_ = 1u;
    std::mutex mutex_;
    IUnknown* device_ = nullptr;
};

struct HostMfPlat {
    std::mutex mutex;
    bool attempted = false;
    HMODULE mod = nullptr;

    PfnMFStartup mf_startup = nullptr;
    PfnMFShutdown mf_shutdown = nullptr;
    PfnMFCreateDXGIDeviceManager mf_create_dxgi_mgr = nullptr;
    PfnMFCreateMediaType mf_create_media_type = nullptr;
    PfnMFCreateSample mf_create_sample = nullptr;
    PfnMFCreateMemoryBuffer mf_create_memory_buffer = nullptr;
    PfnMFCreateAttributes mf_create_attributes = nullptr;
    PfnMFCreateAsyncResult mf_create_async_result = nullptr;
    PfnMFAllocateWorkQueue mf_allocate_wq = nullptr;
    PfnMFUnlockWorkQueue mf_unlock_wq = nullptr;
    PfnMFLockPlatform mf_lock_platform = nullptr;
    PfnMFUnlockPlatform mf_unlock_platform = nullptr;
    PfnMFGetSystemTime mf_get_system_time = nullptr;
    PfnMFTEnumEx mft_enum_ex = nullptr;

    std::atomic<uint32_t> fallback_token_gen{0x5CA01000u};
    std::atomic<uint32_t> fallback_wq_gen{0x4000u};
    std::atomic<uint32_t> startup_refcount{0u};
    std::unordered_set<uint32_t> fallback_wqs;

    void EnsureLoaded() {
        std::lock_guard<std::mutex> lk(mutex);
        if (attempted) return;
        attempted = true;

        wchar_t sys_dir[MAX_PATH]{};
        const UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
        if (len > 0u && len < MAX_PATH - 16u) {
            std::wstring full_path(sys_dir, len);
            if (!full_path.empty() && full_path.back() != L'\\') {
                full_path.push_back(L'\\');
            }
            full_path += L"mfplat.dll";
            mod = LoadLibraryW(full_path.c_str());
        }
        if (!mod) {
            mod = LoadLibraryW(L"mfplat.dll");
        }
        if (!mod) {
            return;
        }

        auto load = [&](const char* name) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(mod, name));
        };
        mf_startup = reinterpret_cast<PfnMFStartup>(load("MFStartup"));
        mf_shutdown = reinterpret_cast<PfnMFShutdown>(load("MFShutdown"));
        mf_create_dxgi_mgr = reinterpret_cast<PfnMFCreateDXGIDeviceManager>(load("MFCreateDXGIDeviceManager"));
        mf_create_media_type = reinterpret_cast<PfnMFCreateMediaType>(load("MFCreateMediaType"));
        mf_create_sample = reinterpret_cast<PfnMFCreateSample>(load("MFCreateSample"));
        mf_create_memory_buffer = reinterpret_cast<PfnMFCreateMemoryBuffer>(load("MFCreateMemoryBuffer"));
        mf_create_attributes = reinterpret_cast<PfnMFCreateAttributes>(load("MFCreateAttributes"));
        mf_create_async_result = reinterpret_cast<PfnMFCreateAsyncResult>(load("MFCreateAsyncResult"));
        mf_allocate_wq = reinterpret_cast<PfnMFAllocateWorkQueue>(load("MFAllocateWorkQueue"));
        mf_unlock_wq = reinterpret_cast<PfnMFUnlockWorkQueue>(load("MFUnlockWorkQueue"));
        mf_lock_platform = reinterpret_cast<PfnMFLockPlatform>(load("MFLockPlatform"));
        mf_unlock_platform = reinterpret_cast<PfnMFUnlockPlatform>(load("MFUnlockPlatform"));
        mf_get_system_time = reinterpret_cast<PfnMFGetSystemTime>(load("MFGetSystemTime"));
        mft_enum_ex = reinterpret_cast<PfnMFTEnumEx>(load("MFTEnumEx"));
    }
};

HostMfPlat& GetHostMf() {
    static HostMfPlat* s = new HostMfPlat();
    return *s;
}

#endif  // _WIN32

}  // namespace

extern "C" {

// Xbox Series X|S / GDK-specific DXGI Device Manager reset API.
OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFResetDXGIDeviceManagerX(
    void* pDeviceManager,
    void* pUnkDevice,
    uint32_t resetToken) {
    if (!pDeviceManager) {
        return RecordHr("MFResetDXGIDeviceManagerX", kHrPointer);
    }
    if (!pUnkDevice) {
        return RecordHr("MFResetDXGIDeviceManagerX", kHrInvalidArg);
    }
#if defined(_WIN32)
    auto* mgr = static_cast<IMFDXGIDeviceManager*>(pDeviceManager);
    auto* dev = static_cast<IUnknown*>(pUnkDevice);
    const HRESULT hr = mgr->ResetDevice(dev, static_cast<UINT>(resetToken));
    return RecordHr("MFResetDXGIDeviceManagerX", static_cast<int32_t>(hr));
#else
    (void)resetToken;
    return RecordHr("MFResetDXGIDeviceManagerX", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFStartup(uint32_t Version, uint32_t dwFlags) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_startup) {
        const HRESULT hr = host.mf_startup(static_cast<ULONG>(Version), static_cast<DWORD>(dwFlags));
        if (SUCCEEDED(hr)) {
            host.startup_refcount.fetch_add(1u, std::memory_order_relaxed);
        }
        return RecordHr("MFStartup", static_cast<int32_t>(hr));
    }
    host.startup_refcount.fetch_add(1u, std::memory_order_relaxed);
    return RecordHr("MFStartup", kHrOk);
#else
    (void)Version;
    (void)dwFlags;
    return RecordHr("MFStartup", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFShutdown(void) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_shutdown) {
        const HRESULT hr = host.mf_shutdown();
        if (SUCCEEDED(hr)) {
            uint32_t cur = host.startup_refcount.load(std::memory_order_relaxed);
            while (cur > 0u &&
                   !host.startup_refcount.compare_exchange_weak(cur, cur - 1u)) {
            }
        }
        return RecordHr("MFShutdown", static_cast<int32_t>(hr));
    }
    return RecordHr("MFShutdown", kHrOk);
#else
    return RecordHr("MFShutdown", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateDXGIDeviceManager(
    uint32_t* resetToken,
    void** ppDeviceManager) {
    if (!resetToken || !ppDeviceManager) {
        return RecordHr("MFCreateDXGIDeviceManager", kHrPointer);
    }
    *ppDeviceManager = nullptr;
    *resetToken = 0u;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_dxgi_mgr) {
        UINT tok = 0u;
        IMFDXGIDeviceManager* mgr = nullptr;
        const HRESULT hr = host.mf_create_dxgi_mgr(&tok, &mgr);
        if (SUCCEEDED(hr) && mgr) {
            *resetToken = static_cast<uint32_t>(tok);
            *ppDeviceManager = mgr;
            return RecordHr("MFCreateDXGIDeviceManager", static_cast<int32_t>(hr));
        }
    }
    const UINT tok = host.fallback_token_gen.fetch_add(1u, std::memory_order_relaxed);
    auto* fb = new (std::nothrow) FallbackDxgiDeviceManager(tok);
    if (!fb) {
        return RecordHr("MFCreateDXGIDeviceManager", kHrOutOfMemory);
    }
    *resetToken = static_cast<uint32_t>(tok);
    *ppDeviceManager = static_cast<IMFDXGIDeviceManager*>(fb);
    return RecordHr("MFCreateDXGIDeviceManager", kHrOk);
#else
    return RecordHr("MFCreateDXGIDeviceManager", kHrFail);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateMediaType(void** ppMFType) {
    if (!ppMFType) {
        return RecordHr("MFCreateMediaType", kHrPointer);
    }
    *ppMFType = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_media_type) {
        IMFMediaType* mt = nullptr;
        const HRESULT hr = host.mf_create_media_type(&mt);
        *ppMFType = mt;
        return RecordHr("MFCreateMediaType", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateMediaType", kHrNotImpl);
#else
    return RecordHr("MFCreateMediaType", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateSample(void** ppIMFSample) {
    if (!ppIMFSample) {
        return RecordHr("MFCreateSample", kHrPointer);
    }
    *ppIMFSample = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_sample) {
        IMFSample* smp = nullptr;
        const HRESULT hr = host.mf_create_sample(&smp);
        *ppIMFSample = smp;
        return RecordHr("MFCreateSample", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateSample", kHrNotImpl);
#else
    return RecordHr("MFCreateSample", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateMemoryBuffer(
    uint32_t cbMaxLength,
    void** ppBuffer) {
    if (!ppBuffer) {
        return RecordHr("MFCreateMemoryBuffer", kHrPointer);
    }
    *ppBuffer = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_memory_buffer) {
        IMFMediaBuffer* buf = nullptr;
        const HRESULT hr = host.mf_create_memory_buffer(static_cast<DWORD>(cbMaxLength), &buf);
        *ppBuffer = buf;
        return RecordHr("MFCreateMemoryBuffer", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateMemoryBuffer", kHrNotImpl);
#else
    (void)cbMaxLength;
    return RecordHr("MFCreateMemoryBuffer", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateAttributes(
    void** ppMFAttributes,
    uint32_t cInitialSize) {
    if (!ppMFAttributes) {
        return RecordHr("MFCreateAttributes", kHrPointer);
    }
    *ppMFAttributes = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_attributes) {
        IMFAttributes* attrs = nullptr;
        const HRESULT hr = host.mf_create_attributes(&attrs, static_cast<UINT32>(cInitialSize));
        *ppMFAttributes = attrs;
        return RecordHr("MFCreateAttributes", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateAttributes", kHrNotImpl);
#else
    (void)cInitialSize;
    return RecordHr("MFCreateAttributes", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateAsyncResult(
    void* punkObject,
    void* pCallback,
    void* punkState,
    void** ppAsyncResult) {
    if (!ppAsyncResult) {
        return RecordHr("MFCreateAsyncResult", kHrPointer);
    }
    *ppAsyncResult = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_async_result) {
        IMFAsyncResult* res = nullptr;
        const HRESULT hr = host.mf_create_async_result(
            static_cast<IUnknown*>(punkObject),
            static_cast<IMFAsyncCallback*>(pCallback),
            static_cast<IUnknown*>(punkState),
            &res);
        *ppAsyncResult = res;
        return RecordHr("MFCreateAsyncResult", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateAsyncResult", kHrNotImpl);
#else
    (void)punkObject;
    (void)pCallback;
    (void)punkState;
    return RecordHr("MFCreateAsyncResult", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFAllocateWorkQueue(uint32_t* pdwWorkQueue) {
    if (!pdwWorkQueue) {
        return RecordHr("MFAllocateWorkQueue", kHrPointer);
    }
    *pdwWorkQueue = 0u;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_allocate_wq) {
        DWORD wq = 0u;
        const HRESULT hr = host.mf_allocate_wq(&wq);
        *pdwWorkQueue = static_cast<uint32_t>(wq);
        return RecordHr("MFAllocateWorkQueue", static_cast<int32_t>(hr));
    }
    const uint32_t id = host.fallback_wq_gen.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        host.fallback_wqs.insert(id);
    }
    *pdwWorkQueue = id;
    return RecordHr("MFAllocateWorkQueue", kHrOk);
#else
    return RecordHr("MFAllocateWorkQueue", kHrFail);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFUnlockWorkQueue(uint32_t dwWorkQueue) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_unlock_wq) {
        const HRESULT hr = host.mf_unlock_wq(static_cast<DWORD>(dwWorkQueue));
        return RecordHr("MFUnlockWorkQueue", static_cast<int32_t>(hr));
    }
    std::lock_guard<std::mutex> lk(host.mutex);
    const bool erased = host.fallback_wqs.erase(dwWorkQueue) > 0u;
    return RecordHr("MFUnlockWorkQueue", erased ? kHrOk : kHrInvalidArg);
#else
    (void)dwWorkQueue;
    return RecordHr("MFUnlockWorkQueue", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFLockPlatform(void) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_lock_platform) {
        return RecordHr("MFLockPlatform", static_cast<int32_t>(host.mf_lock_platform()));
    }
#endif
    return RecordHr("MFLockPlatform", kHrOk);
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFUnlockPlatform(void) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_unlock_platform) {
        return RecordHr("MFUnlockPlatform", static_cast<int32_t>(host.mf_unlock_platform()));
    }
#endif
    return RecordHr("MFUnlockPlatform", kHrOk);
}

OS_MFPLAT_API int64_t OS_MFPLAT_CALL MFGetSystemTime(void) {
    RecordOk("MFGetSystemTime");
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_get_system_time) {
        return static_cast<int64_t>(host.mf_get_system_time());
    }
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32u) |
                       static_cast<uint64_t>(ft.dwLowDateTime);
    return static_cast<int64_t>(t);
#else
    return 0;
#endif
}

#if defined(_WIN32)
OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFTEnumEx(
    GUID guidCategory,
    uint32_t Flags,
    const void* pInputType,
    const void* pOutputType,
    void*** pppMFTActivate,
    uint32_t* pnumMFTActivate) {
    if (!pppMFTActivate || !pnumMFTActivate) {
        return RecordHr("MFTEnumEx", kHrPointer);
    }
    *pppMFTActivate = nullptr;
    *pnumMFTActivate = 0u;
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mft_enum_ex) {
        IMFActivate** acts = nullptr;
        UINT32 count = 0u;
        const HRESULT hr = host.mft_enum_ex(
            guidCategory,
            static_cast<UINT32>(Flags),
            static_cast<const MFT_REGISTER_TYPE_INFO*>(pInputType),
            static_cast<const MFT_REGISTER_TYPE_INFO*>(pOutputType),
            &acts,
            &count);
        *pppMFTActivate = reinterpret_cast<void**>(acts);
        *pnumMFTActivate = static_cast<uint32_t>(count);
        return RecordHr("MFTEnumEx", static_cast<int32_t>(hr));
    }
    return RecordHr("MFTEnumEx", kHrOk);
}
#endif

OS_MFPLAT_API int32_t OS_MFPLAT_CALL OpenScarlettMfPlatGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    if (outTotalCalls) *outTotalCalls = 0u;
    if (outOkCalls) *outOkCalls = 0u;
    if (!symbolName) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == kModName && entry.symbol_name == symbolName) {
            if (outTotalCalls) *outTotalCalls = entry.total_calls;
            if (outOkCalls) *outOkCalls = entry.ok_calls;
            return 1;
        }
    }
    return 0;
}

}  // extern "C"
