#define OPENSCARLETT_BUILDING_WINRT 1
#include "openscarlett/winrt/winrt_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
// Minimal Win32 imports without pulling WinRT SDK headers (keeps MSVC/Clang-CL/MinGW clean).
extern "C" __declspec(dllimport) void* __stdcall CoTaskMemAlloc(size_t cb);
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t* lpModuleName);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* hModule, const char* lpProcName);
extern "C" __declspec(dllimport) uint32_t __stdcall GetEnvironmentVariableA(
    const char* lpName, char* lpBuffer, uint32_t nSize);
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kWinRtMod = "winrt_xs.dll";

inline int32_t RecordWinRtHr(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kWinRtMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

inline void RecordWinRtOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kWinRtMod, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

// ---------------------------------------------------------------------------
// HSTRING Internal Representation (exact 24-byte x64 ABI header)
// ---------------------------------------------------------------------------
constexpr uint32_t kHStringMagicMask = 0xFFFF0000u;
constexpr uint32_t kHStringMagic = 0x53580000u;
constexpr uint32_t kHStringFlagRef = 0x00000001u;

struct SxHStringImpl {
    uint32_t flags;
    uint32_t length;
    std::atomic<int32_t> ref_count;
    uint32_t reserved;
    const uint16_t* buffer;
};

static_assert(sizeof(SxHStringImpl) == sizeof(SX_HSTRING_HEADER),
              "SxHStringImpl must fit in 24-byte SX_HSTRING_HEADER");
static_assert(alignof(SxHStringImpl) <= alignof(SX_HSTRING_HEADER),
              "SxHStringImpl alignment must fit SX_HSTRING_HEADER");

const uint16_t kEmptyUtf16[1] = {0};

const SxHStringImpl* AsImpl(SX_HSTRING h) {
    return reinterpret_cast<const SxHStringImpl*>(h);
}

SxHStringImpl* AsImplMut(SX_HSTRING h) {
    return reinterpret_cast<SxHStringImpl*>(h);
}

std::u16string HStringToU16(SX_HSTRING h) {
    uint32_t len = 0;
    const uint16_t* raw = WindowsGetStringRawBuffer(h, &len);
    std::u16string out;
    out.resize(len);
    if (len > 0) {
        std::memcpy(out.data(), raw, static_cast<size_t>(len) * sizeof(uint16_t));
    }
    return out;
}

int32_t CreateHStringFromU16(const std::u16string& text, SX_HSTRING* out) {
    return WindowsCreateString(
        reinterpret_cast<const uint16_t*>(text.data()),
        static_cast<uint32_t>(text.size()),
        out);
}

std::u16string PathToU16(const fs::path& p) {
    const std::u16string u16 = p.u16string();
    return u16;
}

// ---------------------------------------------------------------------------
// COM / GUID Helpers & Refused XAML/ReferenceTracker Interfaces
// ---------------------------------------------------------------------------
// Refused COM interfaces: commercial C++/WinRT & XAML interop hosts QI for
// IReferenceTracker / IReferenceTrackerTarget and crash if an object blindly
// returns S_OK or a non-null pointer. All OpenScarlett WinRT objects MUST
// return E_NOINTERFACE with *ppv == nullptr for these IIDs.
constexpr SX_GUID kIidIReferenceTracker = {
    0x11D3B13Au, 0x180Eu, 0x4789u, {0xA8, 0xBE, 0x77, 0x12, 0x88, 0x28, 0x93, 0xE6}};
constexpr SX_GUID kIidIReferenceTrackerTarget = {
    0x64BD43F8u, 0xBFEEu, 0x4EC4u, {0xB7, 0xEB, 0x29, 0x35, 0x15, 0x8D, 0xAE, 0x21}};
constexpr SX_GUID kIidIReferenceTrackerHost = {
    0x29A71C6Au, 0x3C42u, 0x4416u, {0xA3, 0x9D, 0xE2, 0x82, 0x5A, 0x07, 0xA7, 0x73}};

// App lifecycle, CoreWindow, ApplicationView, DisplayInformation, GamingDevice IIDs
constexpr SX_GUID kIidICoreApplication = {
    0x0AACF7A4u, 0x5E1Du, 0x49DFu, {0x80, 0x34, 0xFB, 0x6A, 0x68, 0xBC, 0x5E, 0xD1}};
constexpr SX_GUID kIidICoreApplication2 = {
    0x998681FBu, 0x1AB6u, 0x4B7Fu, {0xBE, 0x4A, 0x9A, 0x06, 0x45, 0x22, 0x4C, 0x04}};
constexpr SX_GUID kIidICoreApplicationExit = {
    0xCF86461Du, 0x261Eu, 0x4B72u, {0x9A, 0xCD, 0x44, 0xED, 0x2A, 0xCE, 0x6A, 0x29}};
constexpr SX_GUID kIidICoreApplicationUseCount = {
    0x518DC408u, 0xC077u, 0x475Bu, {0x80, 0x9E, 0x0B, 0xC0, 0xC5, 0x7E, 0x4B, 0x74}};
constexpr SX_GUID kIidICoreApplicationView = {
    0x638BB2DBu, 0x451Du, 0x4661u, {0xB0, 0x99, 0x41, 0x4F, 0x34, 0xFF, 0xB9, 0xF1}};
constexpr SX_GUID kIidISuspendingEventArgs = {
    0x96061C05u, 0x2DBAu, 0x4D08u, {0xB0, 0xBD, 0x2B, 0x30, 0xA1, 0x31, 0xC6, 0xAA}};
constexpr SX_GUID kIidISuspendingOperation = {
    0x9DA4CA41u, 0x20E1u, 0x4E9Bu, {0x9F, 0x65, 0xA9, 0xF4, 0x35, 0x34, 0x0C, 0x3A}};
constexpr SX_GUID kIidISuspendingDeferral = {
    0x59140509u, 0x8BC9u, 0x4EB4u, {0xB6, 0x36, 0xDA, 0xBD, 0xC4, 0xF4, 0x6F, 0x66}};
constexpr SX_GUID kIidIEnteredBackgroundEventArgs = {
    0xF722DCC2u, 0x9827u, 0x403Du, {0xAA, 0xED, 0xEC, 0xCA, 0x9A, 0xC1, 0x73, 0x98}};
constexpr SX_GUID kIidILeavingBackgroundEventArgs = {
    0x39C6EC9Au, 0xAE6Eu, 0x46F9u, {0xA0, 0x7A, 0xCF, 0xC2, 0x3F, 0x88, 0x73, 0x3E}};
constexpr SX_GUID kIidIDeferral = {
    0xD6269732u, 0x3B7Fu, 0x46A7u, {0xB4, 0x0B, 0x4F, 0xDC, 0xA2, 0xA2, 0xC6, 0x93}};
constexpr SX_GUID kIidICoreWindowStatic = {
    0x4D239005u, 0x3C2Au, 0x41B1u, {0x90, 0x22, 0x53, 0x6B, 0xB9, 0xCF, 0x93, 0xB1}};
constexpr SX_GUID kIidICoreWindow = {
    0x79B9D5F2u, 0x879Eu, 0x4B89u, {0xB7, 0x98, 0x79, 0xE4, 0x75, 0x98, 0x03, 0x0C}};
constexpr SX_GUID kIidICoreDispatcher = {
    0x60DB2FA8u, 0xB705u, 0x4FDEu, {0xA7, 0xD6, 0xEB, 0xBB, 0x18, 0x91, 0xD3, 0x9E}};
constexpr SX_GUID kIidIApplicationViewStatics2 = {
    0xAF338AE5u, 0xCF64u, 0x423Cu, {0x85, 0xE5, 0xF3, 0xE7, 0x24, 0x48, 0xFB, 0x23}};
constexpr SX_GUID kIidIApplicationView = {
    0xD222D519u, 0x4361u, 0x451Eu, {0x96, 0xC4, 0x60, 0xF4, 0xF9, 0x74, 0x2D, 0xB0}};
constexpr SX_GUID kIidIApplicationView2 = {
    0xE876B196u, 0xA545u, 0x40DCu, {0xB5, 0x94, 0x45, 0x0C, 0xBA, 0x68, 0xCC, 0x00}};
constexpr SX_GUID kIidIApplicationView3 = {
    0x903C9CE5u, 0x793Au, 0x4FDFu, {0xA2, 0xB2, 0xAF, 0x1A, 0xC2, 0x1E, 0x31, 0x08}};
constexpr SX_GUID kIidIDisplayInformationStatics = {
    0xC6A02A6Cu, 0xD452u, 0x44DCu, {0xBA, 0x07, 0x96, 0xF3, 0xC6, 0xAD, 0xF9, 0xD1}};
constexpr SX_GUID kIidIDisplayInformation = {
    0xBED112AEu, 0xADC3u, 0x4DC9u, {0xAE, 0x65, 0x85, 0x1F, 0x4D, 0x7D, 0x47, 0x99}};
constexpr SX_GUID kIidIDisplayInformation2 = {
    0x4DCD0021u, 0xFAD1u, 0x4B8Eu, {0x8E, 0xDF, 0x77, 0x58, 0x87, 0xB8, 0xBF, 0x19}};
constexpr SX_GUID kIidIGamingDeviceInformationStatics = {
    0x3F0D9C56u, 0x0A0Bu, 0x4B99u, {0x90, 0x72, 0x7C, 0x8F, 0x90, 0x4A, 0x7A, 0x31}};
constexpr SX_GUID kIidIGameListStatics = {
    0x2DDD0F6Fu, 0x9366u, 0x4F55u, {0x9D, 0x2B, 0xA3, 0x1B, 0x0E, 0x8A, 0xC3, 0x8C}};

bool GuidEquals(const SX_GUID* a, const SX_GUID& b) {
    if (!a) return false;
    return std::memcmp(a, &b, sizeof(SX_GUID)) == 0;
}

bool IsRefusedComInterface(const SX_GUID* riid) {
    return GuidEquals(riid, kIidIReferenceTracker) ||
           GuidEquals(riid, kIidIReferenceTrackerTarget) ||
           GuidEquals(riid, kIidIReferenceTrackerHost);
}

void* AllocComMemory(size_t bytes) {
#if defined(_WIN32)
    return CoTaskMemAlloc(bytes);
#else
    return std::malloc(bytes);
#endif
}

int32_t CopyIids(const SX_GUID* list, uint32_t count, uint32_t* outCount, SX_GUID** outIids) {
    if (!outCount || !outIids) {
        return SX_WINRT_E_POINTER;
    }
    *outCount = 0;
    *outIids = nullptr;
    if (count == 0) {
        return SX_WINRT_S_OK;
    }
    auto* mem = static_cast<SX_GUID*>(AllocComMemory(sizeof(SX_GUID) * count));
    if (!mem) {
        return SX_WINRT_E_OUTOFMEMORY;
    }
    std::memcpy(mem, list, sizeof(SX_GUID) * count);
    *outCount = count;
    *outIids = mem;
    return SX_WINRT_S_OK;
}

// ---------------------------------------------------------------------------
// Per-Thread WinRT Apartment State (RoInitialize / RoUninitialize)
// ---------------------------------------------------------------------------
struct ThreadRoState {
    uint32_t init_count = 0;
    uint32_t init_type = SX_RO_INIT_MULTITHREADED;
};

thread_local ThreadRoState g_tls_ro_state;

// ---------------------------------------------------------------------------
// Custom Activation Factory Registry
// ---------------------------------------------------------------------------
struct RegisteredFactoryEntry {
    uint64_t cookie = 0;
    std::u16string class_id;
    SX_PFNGETACTIVATIONFACTORY callback = nullptr;
};

struct WinRtRegistry {
    std::mutex mutex;
    uint64_t next_cookie = 1;
    std::vector<RegisteredFactoryEntry> custom_factories;
};

WinRtRegistry& GetRegistry() {
    static WinRtRegistry r;
    return r;
}

// ---------------------------------------------------------------------------
// Built-in Class 1: Windows.System.Profile.AnalyticsInfo & AnalyticsVersionInfo
// ---------------------------------------------------------------------------
struct AnalyticsVersionInfoObj {
    const SX_IAnalyticsVersionInfoVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string runtime_class_name;
};

int32_t SCARLETTX_WINRT_CALL Avi_QueryInterface(
    SX_IAnalyticsVersionInfo* self_iface,
    const SX_GUID* riid,
    void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self_iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IAnalyticsVersionInfo)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}

uint32_t SCARLETTX_WINRT_CALL Avi_AddRef(SX_IAnalyticsVersionInfo* self_iface) {
    if (!self_iface) return 0;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_WINRT_CALL Avi_Release(SX_IAnalyticsVersionInfo* self_iface) {
    if (!self_iface) return 0;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete self;
    }
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Avi_GetIids(
    SX_IAnalyticsVersionInfo*,
    uint32_t* count,
    SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IAnalyticsVersionInfo};
    return CopyIids(kIids, 1, count, iids);
}

int32_t SCARLETTX_WINRT_CALL Avi_GetRuntimeClassName(
    SX_IAnalyticsVersionInfo* self_iface,
    SX_HSTRING* className) {
    if (!self_iface || !className) return SX_WINRT_E_POINTER;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    return CreateHStringFromU16(self->runtime_class_name, className);
}

int32_t SCARLETTX_WINRT_CALL Avi_GetTrustLevel(
    SX_IAnalyticsVersionInfo*,
    SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}

int32_t SCARLETTX_WINRT_CALL Avi_GetDeviceFamily(
    SX_IAnalyticsVersionInfo*,
    SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Xbox", value);
}

int32_t SCARLETTX_WINRT_CALL Avi_GetDeviceFamilyVersion(
    SX_IAnalyticsVersionInfo*,
    SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    // Encodes Xbox OS 10.0.26100.2499 as ((10<<48)|(0<<32)|(26100<<16)|2499).
    return CreateHStringFromU16(u"2814751477203968", value);
}

const SX_IAnalyticsVersionInfoVtbl kAviVtbl = {
    &Avi_QueryInterface,
    &Avi_AddRef,
    &Avi_Release,
    &Avi_GetIids,
    &Avi_GetRuntimeClassName,
    &Avi_GetTrustLevel,
    &Avi_GetDeviceFamily,
    &Avi_GetDeviceFamilyVersion,
};

AnalyticsVersionInfoObj* CreateAnalyticsObj(std::u16string class_name) {
    auto* obj = new (std::nothrow) AnalyticsVersionInfoObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kAviVtbl;
    obj->runtime_class_name = std::move(class_name);
    return obj;
}

struct AnalyticsInfoFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_IAnalyticsInfoStaticsVtbl* statics_vtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t AnalyticsFactory_QI_Common(AnalyticsInfoFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, SX_IID_IAnalyticsInfoStatics)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}

uint32_t AnalyticsFactory_AddRef_Common(AnalyticsInfoFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t AnalyticsFactory_Release_Common(AnalyticsInfoFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Af_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return AnalyticsFactory_QI_Common(reinterpret_cast<AnalyticsInfoFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Af_AddRef(SX_IActivationFactory* iface) {
    return AnalyticsFactory_AddRef_Common(reinterpret_cast<AnalyticsInfoFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Af_Release(SX_IActivationFactory* iface) {
    return AnalyticsFactory_Release_Common(reinterpret_cast<AnalyticsInfoFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Af_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IAnalyticsInfoStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Af_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.System.Profile.AnalyticsInfo", name);
}
int32_t SCARLETTX_WINRT_CALL Af_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Af_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = CreateAnalyticsObj(u"Windows.System.Profile.AnalyticsInfo");
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

AnalyticsInfoFactoryObj* FromAnalyticsStatics(SX_IAnalyticsInfoStatics* iface) {
    return reinterpret_cast<AnalyticsInfoFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(AnalyticsInfoFactoryObj, statics_vtbl));
}
int32_t SCARLETTX_WINRT_CALL As_QueryInterface(SX_IAnalyticsInfoStatics* iface, const SX_GUID* riid, void** ppv) {
    return AnalyticsFactory_QI_Common(FromAnalyticsStatics(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL As_AddRef(SX_IAnalyticsInfoStatics* iface) {
    return AnalyticsFactory_AddRef_Common(FromAnalyticsStatics(iface));
}
uint32_t SCARLETTX_WINRT_CALL As_Release(SX_IAnalyticsInfoStatics* iface) {
    return AnalyticsFactory_Release_Common(FromAnalyticsStatics(iface));
}
int32_t SCARLETTX_WINRT_CALL As_GetIids(SX_IAnalyticsInfoStatics*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IAnalyticsInfoStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL As_GetRuntimeClassName(SX_IAnalyticsInfoStatics*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.System.Profile.AnalyticsInfo", name);
}
int32_t SCARLETTX_WINRT_CALL As_GetTrustLevel(SX_IAnalyticsInfoStatics*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL As_GetVersionInfo(SX_IAnalyticsInfoStatics*, SX_IAnalyticsVersionInfo** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* obj = CreateAnalyticsObj(u"Windows.System.Profile.AnalyticsVersionInfo");
    if (!obj) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IAnalyticsVersionInfo*>(obj);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL As_GetDeviceForm(SX_IAnalyticsInfoStatics*, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Xbox Series X", value);
}

const SX_IActivationFactoryVtbl kAnalyticsFactoryVtbl = {
    &Af_QueryInterface, &Af_AddRef, &Af_Release,
    &Af_GetIids, &Af_GetRuntimeClassName, &Af_GetTrustLevel,
    &Af_ActivateInstance,
};

const SX_IAnalyticsInfoStaticsVtbl kAnalyticsStaticsVtbl = {
    &As_QueryInterface, &As_AddRef, &As_Release,
    &As_GetIids, &As_GetRuntimeClassName, &As_GetTrustLevel,
    &As_GetVersionInfo, &As_GetDeviceForm,
};

// ---------------------------------------------------------------------------
// Built-in Class 2: Windows.Storage.ApplicationData & StorageFolder
// ---------------------------------------------------------------------------
fs::path ResolveWinRtAppDataFolder(const char* leaf) {
    fs::path base;
#if defined(_WIN32)
    char env_buf[1024]{};
    const uint32_t env_len = GetEnvironmentVariableA(
        "OPENSCARLETT_SAVE_ROOT", env_buf, static_cast<uint32_t>(sizeof(env_buf)));
    if (env_len > 0u && env_len < sizeof(env_buf)) {
        base = fs::path(std::string(env_buf, env_len));
    } else {
        base = fs::temp_directory_path() / "scarlettx_saves";
    }
#else
    const char* env_root = std::getenv("OPENSCARLETT_SAVE_ROOT");
    base = (env_root && *env_root)
               ? fs::path(env_root)
               : (fs::temp_directory_path() / "scarlettx_saves");
#endif
    fs::path dir = base / "winrt_appdata" / leaf;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

struct StorageFolderObj {
    const SX_IStorageItemVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string name;
    std::u16string path;
};

int32_t SCARLETTX_WINRT_CALL Sf_QueryInterface(SX_IStorageItem* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<StorageFolderObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IStorageItem)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Sf_AddRef(SX_IStorageItem* iface) {
    return reinterpret_cast<StorageFolderObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Sf_Release(SX_IStorageItem* iface) {
    auto* self = reinterpret_cast<StorageFolderObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Sf_GetIids(SX_IStorageItem*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IStorageItem};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Sf_GetRuntimeClassName(SX_IStorageItem*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.StorageFolder", name);
}
int32_t SCARLETTX_WINRT_CALL Sf_GetTrustLevel(SX_IStorageItem*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl2(SX_IStorageItem*, SX_HSTRING, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl3(SX_IStorageItem*, SX_HSTRING, uint32_t, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl1(SX_IStorageItem*, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl1u(SX_IStorageItem*, uint32_t, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_GetName(SX_IStorageItem* iface, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<StorageFolderObj*>(iface)->name, value);
}
int32_t SCARLETTX_WINRT_CALL Sf_GetPath(SX_IStorageItem* iface, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<StorageFolderObj*>(iface)->path, value);
}

const SX_IStorageItemVtbl kStorageFolderVtbl = {
    &Sf_QueryInterface, &Sf_AddRef, &Sf_Release,
    &Sf_GetIids, &Sf_GetRuntimeClassName, &Sf_GetTrustLevel,
    &Sf_NotImpl2, &Sf_NotImpl3, &Sf_NotImpl1, &Sf_NotImpl1u, &Sf_NotImpl1,
    &Sf_GetName, &Sf_GetPath,
};

StorageFolderObj* CreateStorageFolder(const char* leaf) {
    auto* obj = new (std::nothrow) StorageFolderObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kStorageFolderVtbl;
    const fs::path p = ResolveWinRtAppDataFolder(leaf);
    obj->name = PathToU16(fs::path(leaf));
    obj->path = PathToU16(p);
    return obj;
}

struct GenericInspectableObj {
    const SX_IInspectableVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string class_name;
};

int32_t SCARLETTX_WINRT_CALL Gi_QueryInterface(SX_IInspectable* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<GenericInspectableObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Gi_AddRef(SX_IInspectable* iface) {
    return reinterpret_cast<GenericInspectableObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Gi_Release(SX_IInspectable* iface) {
    auto* self = reinterpret_cast<GenericInspectableObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Gi_GetIids(SX_IInspectable*, uint32_t* count, SX_GUID** iids) {
    return CopyIids(nullptr, 0, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Gi_GetRuntimeClassName(SX_IInspectable* iface, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<GenericInspectableObj*>(iface)->class_name, name);
}
int32_t SCARLETTX_WINRT_CALL Gi_GetTrustLevel(SX_IInspectable*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
const SX_IInspectableVtbl kGenericInspectableVtbl = {
    &Gi_QueryInterface, &Gi_AddRef, &Gi_Release,
    &Gi_GetIids, &Gi_GetRuntimeClassName, &Gi_GetTrustLevel,
};

struct ApplicationDataObj {
    const SX_IApplicationDataVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t SCARLETTX_WINRT_CALL Ad_QueryInterface(SX_IApplicationData* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<ApplicationDataObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IApplicationData)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Ad_AddRef(SX_IApplicationData* iface) {
    return reinterpret_cast<ApplicationDataObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Ad_Release(SX_IApplicationData* iface) {
    auto* self = reinterpret_cast<ApplicationDataObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetIids(SX_IApplicationData*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IApplicationData};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Ad_GetRuntimeClassName(SX_IApplicationData*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.ApplicationData", name);
}
int32_t SCARLETTX_WINRT_CALL Ad_GetTrustLevel(SX_IApplicationData*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetVersion(SX_IApplicationData*, uint32_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = 1;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_SetVersionAsync(SX_IApplicationData*, uint32_t, void*, void**) {
    return SX_WINRT_E_NOTIMPL;
}
int32_t SCARLETTX_WINRT_CALL Ad_ClearAllAsync(SX_IApplicationData*, void**) {
    return SX_WINRT_E_NOTIMPL;
}
int32_t SCARLETTX_WINRT_CALL Ad_ClearAsync(SX_IApplicationData*, uint32_t, void**) {
    return SX_WINRT_E_NOTIMPL;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetSettings(SX_IApplicationData*, SX_IInspectable** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* obj = new (std::nothrow) GenericInspectableObj();
    if (!obj) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    obj->lpVtbl = &kGenericInspectableVtbl;
    obj->class_name = u"Windows.Storage.ApplicationDataContainer";
    *value = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetLocalFolder(SX_IApplicationData*, SX_IStorageItem** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* f = CreateStorageFolder("LocalState");
    if (!f) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IStorageItem*>(f);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetRoamingFolder(SX_IApplicationData*, SX_IStorageItem** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* f = CreateStorageFolder("RoamingState");
    if (!f) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IStorageItem*>(f);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetTempFolder(SX_IApplicationData*, SX_IStorageItem** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* f = CreateStorageFolder("TempState");
    if (!f) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IStorageItem*>(f);
    return SX_WINRT_S_OK;
}

const SX_IApplicationDataVtbl kApplicationDataVtbl = {
    &Ad_QueryInterface, &Ad_AddRef, &Ad_Release,
    &Ad_GetIids, &Ad_GetRuntimeClassName, &Ad_GetTrustLevel,
    &Ad_GetVersion, &Ad_SetVersionAsync, &Ad_ClearAllAsync, &Ad_ClearAsync,
    &Ad_GetSettings, &Ad_GetSettings,
    &Ad_GetLocalFolder, &Ad_GetRoamingFolder, &Ad_GetTempFolder,
};

ApplicationDataObj* CreateApplicationData() {
    auto* obj = new (std::nothrow) ApplicationDataObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kApplicationDataVtbl;
    return obj;
}

struct ApplicationDataFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_IApplicationDataStaticsVtbl* statics_vtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t AdFactory_QI_Common(ApplicationDataFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, SX_IID_IApplicationDataStatics)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t AdFactory_AddRef_Common(ApplicationDataFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t AdFactory_Release_Common(ApplicationDataFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Adf_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return AdFactory_QI_Common(reinterpret_cast<ApplicationDataFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Adf_AddRef(SX_IActivationFactory* iface) {
    return AdFactory_AddRef_Common(reinterpret_cast<ApplicationDataFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Adf_Release(SX_IActivationFactory* iface) {
    return AdFactory_Release_Common(reinterpret_cast<ApplicationDataFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Adf_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IApplicationDataStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Adf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.ApplicationData", name);
}
int32_t SCARLETTX_WINRT_CALL Adf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Adf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = CreateApplicationData();
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

ApplicationDataFactoryObj* FromAdStatics(SX_IApplicationDataStatics* iface) {
    return reinterpret_cast<ApplicationDataFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(ApplicationDataFactoryObj, statics_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Ads_QueryInterface(SX_IApplicationDataStatics* iface, const SX_GUID* riid, void** ppv) {
    return AdFactory_QI_Common(FromAdStatics(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Ads_AddRef(SX_IApplicationDataStatics* iface) {
    return AdFactory_AddRef_Common(FromAdStatics(iface));
}
uint32_t SCARLETTX_WINRT_CALL Ads_Release(SX_IApplicationDataStatics* iface) {
    return AdFactory_Release_Common(FromAdStatics(iface));
}
int32_t SCARLETTX_WINRT_CALL Ads_GetIids(SX_IApplicationDataStatics*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IApplicationDataStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Ads_GetRuntimeClassName(SX_IApplicationDataStatics*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.ApplicationData", name);
}
int32_t SCARLETTX_WINRT_CALL Ads_GetTrustLevel(SX_IApplicationDataStatics*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ads_GetCurrent(SX_IApplicationDataStatics*, SX_IApplicationData** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* obj = CreateApplicationData();
    if (!obj) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IApplicationData*>(obj);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kAppDataFactoryVtbl = {
    &Adf_QueryInterface, &Adf_AddRef, &Adf_Release,
    &Adf_GetIids, &Adf_GetRuntimeClassName, &Adf_GetTrustLevel,
    &Adf_ActivateInstance,
};
const SX_IApplicationDataStaticsVtbl kAppDataStaticsVtbl = {
    &Ads_QueryInterface, &Ads_AddRef, &Ads_Release,
    &Ads_GetIids, &Ads_GetRuntimeClassName, &Ads_GetTrustLevel,
    &Ads_GetCurrent,
};

// ---------------------------------------------------------------------------
// Built-in Class 3: Windows.Gaming.Input.Gamepad
// ---------------------------------------------------------------------------
struct GamepadObj {
    const SX_IGamepadVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::mutex mutex;
    SX_GamepadVibration vibration{};
};

int32_t SCARLETTX_WINRT_CALL Gp_QueryInterface(SX_IGamepad* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IGamepad)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Gp_AddRef(SX_IGamepad* iface) {
    return reinterpret_cast<GamepadObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Gp_Release(SX_IGamepad* iface) {
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Gp_GetIids(SX_IGamepad*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IGamepad};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Gp_GetRuntimeClassName(SX_IGamepad*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Gaming.Input.Gamepad", name);
}
int32_t SCARLETTX_WINRT_CALL Gp_GetTrustLevel(SX_IGamepad*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gp_GetVibration(SX_IGamepad* iface, SX_GamepadVibration* value) {
    if (!iface || !value) return SX_WINRT_E_POINTER;
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    std::lock_guard<std::mutex> lk(self->mutex);
    *value = self->vibration;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gp_PutVibration(SX_IGamepad* iface, SX_GamepadVibration value) {
    if (!iface) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    std::lock_guard<std::mutex> lk(self->mutex);
    self->vibration = value;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gp_GetCurrentReading(SX_IGamepad*, SX_GamepadReading* value) {
    if (!value) return SX_WINRT_E_POINTER;
    SX_GamepadReading r{};
    r.Timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    *value = r;
    return SX_WINRT_S_OK;
}

const SX_IGamepadVtbl kGamepadVtbl = {
    &Gp_QueryInterface, &Gp_AddRef, &Gp_Release,
    &Gp_GetIids, &Gp_GetRuntimeClassName, &Gp_GetTrustLevel,
    &Gp_GetVibration, &Gp_PutVibration, &Gp_GetCurrentReading,
};

struct GamepadFactoryObj {
    const SX_IActivationFactoryVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t SCARLETTX_WINRT_CALL Gpf_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<GamepadFactoryObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Gpf_AddRef(SX_IActivationFactory* iface) {
    return reinterpret_cast<GamepadFactoryObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Gpf_Release(SX_IActivationFactory* iface) {
    auto* self = reinterpret_cast<GamepadFactoryObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Gpf_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Gpf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Gaming.Input.Gamepad", name);
}
int32_t SCARLETTX_WINRT_CALL Gpf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gpf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = new (std::nothrow) GamepadObj();
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    obj->lpVtbl = &kGamepadVtbl;
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kGamepadFactoryVtbl = {
    &Gpf_QueryInterface, &Gpf_AddRef, &Gpf_Release,
    &Gpf_GetIids, &Gpf_GetRuntimeClassName, &Gpf_GetTrustLevel,
    &Gpf_ActivateInstance,
};

// ---------------------------------------------------------------------------
// Built-in Class 4: Windows.Globalization.Language
// ---------------------------------------------------------------------------
struct LanguageObj {
    const SX_ILanguageVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string tag = u"en-US";
};

int32_t SCARLETTX_WINRT_CALL Lang_QueryInterface(SX_ILanguage* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<LanguageObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_ILanguage)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Lang_AddRef(SX_ILanguage* iface) {
    return reinterpret_cast<LanguageObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Lang_Release(SX_ILanguage* iface) {
    auto* self = reinterpret_cast<LanguageObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Lang_GetIids(SX_ILanguage*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_ILanguage};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetRuntimeClassName(SX_ILanguage*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Globalization.Language", name);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetTrustLevel(SX_ILanguage*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Lang_GetLanguageTag(SX_ILanguage* iface, SX_HSTRING* value) {
    if (!iface || !value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<LanguageObj*>(iface)->tag, value);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetDisplayName(SX_ILanguage* iface, SX_HSTRING* value) {
    if (!iface || !value) return SX_WINRT_E_POINTER;
    auto* self = reinterpret_cast<LanguageObj*>(iface);
    if (self->tag == u"en-US") {
        return CreateHStringFromU16(u"English (United States)", value);
    }
    return CreateHStringFromU16(self->tag, value);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetNativeName(SX_ILanguage* iface, SX_HSTRING* value) {
    return Lang_GetDisplayName(iface, value);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetScript(SX_ILanguage*, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Latn", value);
}

const SX_ILanguageVtbl kLanguageVtbl = {
    &Lang_QueryInterface, &Lang_AddRef, &Lang_Release,
    &Lang_GetIids, &Lang_GetRuntimeClassName, &Lang_GetTrustLevel,
    &Lang_GetLanguageTag, &Lang_GetDisplayName, &Lang_GetNativeName, &Lang_GetScript,
};

LanguageObj* CreateLanguageObj(std::u16string tag) {
    auto* obj = new (std::nothrow) LanguageObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kLanguageVtbl;
    obj->tag = tag.empty() ? u"en-US" : std::move(tag);
    return obj;
}

struct LanguageFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_ILanguageFactoryVtbl* lang_factory_vtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t LangFactory_QI_Common(LanguageFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, SX_IID_ILanguageFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->lang_factory_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t LangFactory_AddRef_Common(LanguageFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t LangFactory_Release_Common(LanguageFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Lf_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return LangFactory_QI_Common(reinterpret_cast<LanguageFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Lf_AddRef(SX_IActivationFactory* iface) {
    return LangFactory_AddRef_Common(reinterpret_cast<LanguageFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Lf_Release(SX_IActivationFactory* iface) {
    return LangFactory_Release_Common(reinterpret_cast<LanguageFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Lf_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_ILanguageFactory};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Lf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Globalization.Language", name);
}
int32_t SCARLETTX_WINRT_CALL Lf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Lf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = CreateLanguageObj(u"en-US");
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

LanguageFactoryObj* FromLangFactory(SX_ILanguageFactory* iface) {
    return reinterpret_cast<LanguageFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(LanguageFactoryObj, lang_factory_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Lfc_QueryInterface(SX_ILanguageFactory* iface, const SX_GUID* riid, void** ppv) {
    return LangFactory_QI_Common(FromLangFactory(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Lfc_AddRef(SX_ILanguageFactory* iface) {
    return LangFactory_AddRef_Common(FromLangFactory(iface));
}
uint32_t SCARLETTX_WINRT_CALL Lfc_Release(SX_ILanguageFactory* iface) {
    return LangFactory_Release_Common(FromLangFactory(iface));
}
int32_t SCARLETTX_WINRT_CALL Lfc_GetIids(SX_ILanguageFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_ILanguageFactory};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Lfc_GetRuntimeClassName(SX_ILanguageFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Globalization.Language", name);
}
int32_t SCARLETTX_WINRT_CALL Lfc_GetTrustLevel(SX_ILanguageFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Lfc_CreateLanguage(
    SX_ILanguageFactory*,
    SX_HSTRING languageTag,
    SX_ILanguage** result) {
    if (!result) return SX_WINRT_E_POINTER;
    *result = nullptr;
    if (WindowsIsStringEmpty(languageTag)) {
        return SX_WINRT_E_INVALIDARG;
    }
    auto* obj = CreateLanguageObj(HStringToU16(languageTag));
    if (!obj) return SX_WINRT_E_OUTOFMEMORY;
    *result = reinterpret_cast<SX_ILanguage*>(obj);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kLanguageFactoryVtbl = {
    &Lf_QueryInterface, &Lf_AddRef, &Lf_Release,
    &Lf_GetIids, &Lf_GetRuntimeClassName, &Lf_GetTrustLevel,
    &Lf_ActivateInstance,
};
const SX_ILanguageFactoryVtbl kLanguageCustomFactoryVtbl = {
    &Lfc_QueryInterface, &Lfc_AddRef, &Lfc_Release,
    &Lfc_GetIids, &Lfc_GetRuntimeClassName, &Lfc_GetTrustLevel,
    &Lfc_CreateLanguage,
};

// ---------------------------------------------------------------------------
// Built-in Class 5..10: WinRT App Lifecycle, CoreWindow, ApplicationView,
// DisplayInformation, GamingDeviceInformation, GamesEnumeration
// ---------------------------------------------------------------------------
struct SX_EventRegistrationToken {
    int64_t value;
};

struct SX_Rect {
    float X;
    float Y;
    float Width;
    float Height;
};

struct SX_Point {
    float X;
    float Y;
};

struct SX_Size {
    float Width;
    float Height;
};

struct SX_DateTime {
    int64_t UniversalTime;
};

struct SX_GamingDeviceModelInfo {
    uint32_t vendorId;
    uint32_t deviceId;
};

struct SX_IEventHandler;
struct SX_IEventHandlerVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IEventHandler*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IEventHandler*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IEventHandler*);
    int32_t(SCARLETTX_WINRT_CALL* Invoke)(SX_IEventHandler*, void* sender, void* args);
};
struct SX_IEventHandler {
    const SX_IEventHandlerVtbl* lpVtbl;
};

struct SX_IDispatchedHandler;
struct SX_IDispatchedHandlerVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IDispatchedHandler*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IDispatchedHandler*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IDispatchedHandler*);
    int32_t(SCARLETTX_WINRT_CALL* Invoke)(SX_IDispatchedHandler*);
};
struct SX_IDispatchedHandler {
    const SX_IDispatchedHandlerVtbl* lpVtbl;
};

struct EventSlot {
    int64_t token = 0;
    SX_IEventHandler* handler = nullptr;
};

struct LifecycleGlobalState {
    std::mutex mutex;
    int64_t next_token = 1;
    std::vector<EventSlot> suspending_handlers;
    std::vector<EventSlot> resuming_handlers;
    std::vector<EventSlot> entered_bg_handlers;
    std::vector<EventSlot> leaving_bg_handlers;
    std::vector<EventSlot> exiting_handlers;
    std::vector<EventSlot> view_activated_handlers;
    std::vector<EventSlot> window_activated_handlers;
    std::vector<EventSlot> window_visibility_handlers;
    std::vector<EventSlot> window_size_handlers;
    std::atomic<bool> constrained_mode{false};
    std::atomic<uint8_t> window_visible{1};
    std::atomic<uint8_t> appview_fullscreen{0};
    std::atomic<int32_t> use_count{0};
    std::atomic<uint64_t> process_events_count{0};
    SX_Rect window_bounds{0.0f, 0.0f, 1920.0f, 1080.0f};
    std::u16string appview_title = u"OpenScarlett";
};

LifecycleGlobalState& GetLifecycleState() {
    static LifecycleGlobalState* s = new LifecycleGlobalState();
    return *s;
}

int32_t AddEventHandlerLocked(std::vector<EventSlot>& list,
                              SX_IEventHandler* handler,
                              SX_EventRegistrationToken* token) {
    if (!handler || !token) return SX_WINRT_E_INVALIDARG;
    handler->lpVtbl->AddRef(handler);
    auto& ls = GetLifecycleState();
    const int64_t tok = ls.next_token++;
    list.push_back(EventSlot{tok, handler});
    token->value = tok;
    return SX_WINRT_S_OK;
}

int32_t RemoveEventHandlerLocked(std::vector<EventSlot>& list,
                                 SX_EventRegistrationToken token) {
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->token == token.value) {
            SX_IEventHandler* h = it->handler;
            list.erase(it);
            if (h) h->lpVtbl->Release(h);
            break;
        }
    }
    return SX_WINRT_S_OK;
}

int32_t AddEventHandler(std::vector<EventSlot>& list,
                        SX_IEventHandler* handler,
                        SX_EventRegistrationToken* token) {
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    return AddEventHandlerLocked(list, handler, token);
}

int32_t RemoveEventHandler(std::vector<EventSlot>& list,
                           SX_EventRegistrationToken token) {
    SX_IEventHandler* to_release = nullptr;
    {
        auto& ls = GetLifecycleState();
        std::lock_guard<std::mutex> lk(ls.mutex);
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->token == token.value) {
                to_release = it->handler;
                list.erase(it);
                break;
            }
        }
    }
    if (to_release) {
        to_release->lpVtbl->Release(to_release);
    }
    return SX_WINRT_S_OK;
}

int32_t DispatchEventList(std::vector<EventSlot>& list, void* sender, void* args) {
    std::vector<SX_IEventHandler*> snapshot;
    {
        auto& ls = GetLifecycleState();
        std::lock_guard<std::mutex> lk(ls.mutex);
        snapshot.reserve(list.size());
        for (const auto& s : list) {
            if (s.handler) {
                s.handler->lpVtbl->AddRef(s.handler);
                snapshot.push_back(s.handler);
            }
        }
    }
    int32_t invoked = 0;
    for (SX_IEventHandler* h : snapshot) {
        h->lpVtbl->Invoke(h, sender, args);
        h->lpVtbl->Release(h);
        ++invoked;
    }
    return invoked;
}

// --- SuspendingDeferral / IDeferral ---
struct SX_ISuspendingDeferral;
struct SX_ISuspendingDeferralVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ISuspendingDeferral*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ISuspendingDeferral*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ISuspendingDeferral*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ISuspendingDeferral*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ISuspendingDeferral*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ISuspendingDeferral*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* Complete)(SX_ISuspendingDeferral*);
};
struct SX_ISuspendingDeferral {
    const SX_ISuspendingDeferralVtbl* lpVtbl;
};

struct DeferralObj {
    const SX_ISuspendingDeferralVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::atomic<bool> completed{false};
};

int32_t SCARLETTX_WINRT_CALL Def_QI(SX_ISuspendingDeferral* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<DeferralObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidISuspendingDeferral) ||
        GuidEquals(riid, kIidIDeferral)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Def_AddRef(SX_ISuspendingDeferral* iface) {
    return reinterpret_cast<DeferralObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Def_Release(SX_ISuspendingDeferral* iface) {
    auto* self = reinterpret_cast<DeferralObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Def_GetIids(SX_ISuspendingDeferral*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {kIidISuspendingDeferral, kIidIDeferral};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Def_GetRuntimeClassName(SX_ISuspendingDeferral*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.ApplicationModel.SuspendingDeferral", name);
}
int32_t SCARLETTX_WINRT_CALL Def_GetTrustLevel(SX_ISuspendingDeferral*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Def_Complete(SX_ISuspendingDeferral* iface) {
    if (!iface) return SX_WINRT_E_INVALIDARG;
    reinterpret_cast<DeferralObj*>(iface)->completed.store(true, std::memory_order_release);
    return SX_WINRT_S_OK;
}
const SX_ISuspendingDeferralVtbl kDeferralVtbl = {
    &Def_QI, &Def_AddRef, &Def_Release,
    &Def_GetIids, &Def_GetRuntimeClassName, &Def_GetTrustLevel,
    &Def_Complete,
};
DeferralObj* CreateDeferralObj() {
    auto* obj = new (std::nothrow) DeferralObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kDeferralVtbl;
    return obj;
}

// --- SuspendingOperation & SuspendingEventArgs ---
struct SX_ISuspendingOperation;
struct SX_ISuspendingOperationVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ISuspendingOperation*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ISuspendingOperation*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ISuspendingOperation*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ISuspendingOperation*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ISuspendingOperation*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ISuspendingOperation*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetDeferral)(SX_ISuspendingOperation*, SX_ISuspendingDeferral**);
    int32_t(SCARLETTX_WINRT_CALL* get_Deadline)(SX_ISuspendingOperation*, SX_DateTime*);
};
struct SX_ISuspendingOperation {
    const SX_ISuspendingOperationVtbl* lpVtbl;
};
struct SuspendingOpObj {
    const SX_ISuspendingOperationVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};
int32_t SCARLETTX_WINRT_CALL Sop_QI(SX_ISuspendingOperation* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<SuspendingOpObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidISuspendingOperation)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Sop_AddRef(SX_ISuspendingOperation* iface) {
    return reinterpret_cast<SuspendingOpObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Sop_Release(SX_ISuspendingOperation* iface) {
    auto* self = reinterpret_cast<SuspendingOpObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Sop_GetIids(SX_ISuspendingOperation*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {kIidISuspendingOperation};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Sop_GetRuntimeClassName(SX_ISuspendingOperation*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.ApplicationModel.SuspendingOperation", name);
}
int32_t SCARLETTX_WINRT_CALL Sop_GetTrustLevel(SX_ISuspendingOperation*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Sop_GetDeferral(SX_ISuspendingOperation*, SX_ISuspendingDeferral** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* d = CreateDeferralObj();
    if (!d) {
        *out = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *out = reinterpret_cast<SX_ISuspendingDeferral*>(d);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Sop_GetDeadline(SX_ISuspendingOperation*, SX_DateTime* value) {
    if (!value) return SX_WINRT_E_POINTER;
    value->UniversalTime = 134000000000000000LL;
    return SX_WINRT_S_OK;
}
const SX_ISuspendingOperationVtbl kSuspendingOpVtbl = {
    &Sop_QI, &Sop_AddRef, &Sop_Release,
    &Sop_GetIids, &Sop_GetRuntimeClassName, &Sop_GetTrustLevel,
    &Sop_GetDeferral, &Sop_GetDeadline,
};

struct SX_ISuspendingEventArgs;
struct SX_ISuspendingEventArgsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ISuspendingEventArgs*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ISuspendingEventArgs*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ISuspendingEventArgs*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ISuspendingEventArgs*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ISuspendingEventArgs*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ISuspendingEventArgs*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_SuspendingOperation)(SX_ISuspendingEventArgs*, SX_ISuspendingOperation**);
};
struct SX_ISuspendingEventArgs {
    const SX_ISuspendingEventArgsVtbl* lpVtbl;
};
struct SuspendingArgsObj {
    const SX_ISuspendingEventArgsVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};
int32_t SCARLETTX_WINRT_CALL Sargs_QI(SX_ISuspendingEventArgs* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<SuspendingArgsObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidISuspendingEventArgs)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Sargs_AddRef(SX_ISuspendingEventArgs* iface) {
    return reinterpret_cast<SuspendingArgsObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Sargs_Release(SX_ISuspendingEventArgs* iface) {
    auto* self = reinterpret_cast<SuspendingArgsObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Sargs_GetIids(SX_ISuspendingEventArgs*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {kIidISuspendingEventArgs};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Sargs_GetRuntimeClassName(SX_ISuspendingEventArgs*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.ApplicationModel.SuspendingEventArgs", name);
}
int32_t SCARLETTX_WINRT_CALL Sargs_GetTrustLevel(SX_ISuspendingEventArgs*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Sargs_GetOp(SX_ISuspendingEventArgs*, SX_ISuspendingOperation** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* op = new (std::nothrow) SuspendingOpObj();
    if (!op) {
        *out = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    op->lpVtbl = &kSuspendingOpVtbl;
    *out = reinterpret_cast<SX_ISuspendingOperation*>(op);
    return SX_WINRT_S_OK;
}
const SX_ISuspendingEventArgsVtbl kSuspendingArgsVtbl = {
    &Sargs_QI, &Sargs_AddRef, &Sargs_Release,
    &Sargs_GetIids, &Sargs_GetRuntimeClassName, &Sargs_GetTrustLevel,
    &Sargs_GetOp,
};

// --- BackgroundEventArgs (EnteredBackground / LeavingBackground) ---
struct SX_IBackgroundEventArgs;
struct SX_IBackgroundEventArgsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IBackgroundEventArgs*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IBackgroundEventArgs*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IBackgroundEventArgs*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IBackgroundEventArgs*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IBackgroundEventArgs*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IBackgroundEventArgs*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetDeferral)(SX_IBackgroundEventArgs*, SX_ISuspendingDeferral**);
};
struct SX_IBackgroundEventArgs {
    const SX_IBackgroundEventArgsVtbl* lpVtbl;
};
struct BackgroundArgsObj {
    const SX_IBackgroundEventArgsVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    bool is_entering = true;
};
int32_t SCARLETTX_WINRT_CALL BgArgs_QI(SX_IBackgroundEventArgs* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<BackgroundArgsObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        (self->is_entering && GuidEquals(riid, kIidIEnteredBackgroundEventArgs)) ||
        (!self->is_entering && GuidEquals(riid, kIidILeavingBackgroundEventArgs))) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL BgArgs_AddRef(SX_IBackgroundEventArgs* iface) {
    return reinterpret_cast<BackgroundArgsObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL BgArgs_Release(SX_IBackgroundEventArgs* iface) {
    auto* self = reinterpret_cast<BackgroundArgsObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL BgArgs_GetIids(SX_IBackgroundEventArgs* iface, uint32_t* count, SX_GUID** iids) {
    auto* self = reinterpret_cast<BackgroundArgsObj*>(iface);
    const SX_GUID iid = self->is_entering ? kIidIEnteredBackgroundEventArgs : kIidILeavingBackgroundEventArgs;
    return CopyIids(&iid, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL BgArgs_GetRuntimeClassName(SX_IBackgroundEventArgs* iface, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    auto* self = reinterpret_cast<BackgroundArgsObj*>(iface);
    return CreateHStringFromU16(
        self->is_entering ? u"Windows.ApplicationModel.EnteredBackgroundEventArgs"
                          : u"Windows.ApplicationModel.LeavingBackgroundEventArgs",
        name);
}
int32_t SCARLETTX_WINRT_CALL BgArgs_GetTrustLevel(SX_IBackgroundEventArgs*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL BgArgs_GetDeferral(SX_IBackgroundEventArgs*, SX_ISuspendingDeferral** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* d = CreateDeferralObj();
    if (!d) {
        *out = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *out = reinterpret_cast<SX_ISuspendingDeferral*>(d);
    return SX_WINRT_S_OK;
}
const SX_IBackgroundEventArgsVtbl kBgArgsVtbl = {
    &BgArgs_QI, &BgArgs_AddRef, &BgArgs_Release,
    &BgArgs_GetIids, &BgArgs_GetRuntimeClassName, &BgArgs_GetTrustLevel,
    &BgArgs_GetDeferral,
};

// --- Built-in Class 5: Windows.UI.Core.CoreDispatcher & CoreWindow ---
struct SX_ICoreDispatcher;
struct SX_ICoreDispatcherVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreDispatcher*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreDispatcher*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreDispatcher*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreDispatcher*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreDispatcher*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreDispatcher*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_HasThreadAccess)(SX_ICoreDispatcher*, uint8_t* value);
    int32_t(SCARLETTX_WINRT_CALL* ProcessEvents)(SX_ICoreDispatcher*, uint32_t options);
    int32_t(SCARLETTX_WINRT_CALL* RunAsync)(SX_ICoreDispatcher*, int32_t priority, SX_IDispatchedHandler* callback, SX_IInspectable** asyncAction);
    int32_t(SCARLETTX_WINRT_CALL* RunIdleAsync)(SX_ICoreDispatcher*, SX_IDispatchedHandler* callback, SX_IInspectable** asyncAction);
};
struct SX_ICoreDispatcher {
    const SX_ICoreDispatcherVtbl* lpVtbl;
};

struct CoreDispatcherObj {
    const SX_ICoreDispatcherVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

void PumpWin32MessagesOnce() {
#if defined(_WIN32)
    struct MinMsg {
        void* hwnd;
        uint32_t message;
        uintptr_t wParam;
        intptr_t lParam;
        uint32_t time;
        int32_t pt_x;
        int32_t pt_y;
    };
    using FnPeekMessageW = int32_t(__stdcall*)(MinMsg*, void*, uint32_t, uint32_t, uint32_t);
    using FnTranslateMessage = int32_t(__stdcall*)(const MinMsg*);
    using FnDispatchMessageW = intptr_t(__stdcall*)(const MinMsg*);

    void* u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    auto p_peek = reinterpret_cast<FnPeekMessageW>(GetProcAddress(u32, "PeekMessageW"));
    auto p_trans = reinterpret_cast<FnTranslateMessage>(GetProcAddress(u32, "TranslateMessage"));
    auto p_disp = reinterpret_cast<FnDispatchMessageW>(GetProcAddress(u32, "DispatchMessageW"));
    if (!p_peek || !p_trans || !p_disp) return;

    MinMsg msg{};
    constexpr uint32_t kPmRemove = 0x0001u;
    uint32_t budget = 64u;
    while (budget-- > 0u && p_peek(&msg, nullptr, 0u, 0u, kPmRemove) != 0) {
        p_trans(&msg);
        p_disp(&msg);
    }
#endif
}

int32_t SCARLETTX_WINRT_CALL Cd_QI(SX_ICoreDispatcher* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<CoreDispatcherObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidICoreDispatcher)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Cd_AddRef(SX_ICoreDispatcher* iface) {
    return reinterpret_cast<CoreDispatcherObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Cd_Release(SX_ICoreDispatcher* iface) {
    auto* self = reinterpret_cast<CoreDispatcherObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Cd_GetIids(SX_ICoreDispatcher*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {kIidICoreDispatcher};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Cd_GetRuntimeClassName(SX_ICoreDispatcher*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.UI.Core.CoreDispatcher", name);
}
int32_t SCARLETTX_WINRT_CALL Cd_GetTrustLevel(SX_ICoreDispatcher*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cd_GetHasThreadAccess(SX_ICoreDispatcher*, uint8_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = 1u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cd_ProcessEvents(SX_ICoreDispatcher*, uint32_t /*options*/) {
    PumpWin32MessagesOnce();
    GetLifecycleState().process_events_count.fetch_add(1, std::memory_order_relaxed);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cd_RunAsync(SX_ICoreDispatcher*,
                                         int32_t /*priority*/,
                                         SX_IDispatchedHandler* callback,
                                         SX_IInspectable** asyncAction) {
    if (callback) {
        callback->lpVtbl->Invoke(callback);
    }
    if (asyncAction) {
        auto* obj = new (std::nothrow) GenericInspectableObj();
        if (!obj) {
            *asyncAction = nullptr;
            return SX_WINRT_E_OUTOFMEMORY;
        }
        obj->lpVtbl = &kGenericInspectableVtbl;
        obj->class_name = u"Windows.Foundation.IAsyncAction";
        *asyncAction = reinterpret_cast<SX_IInspectable*>(obj);
    }
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cd_RunIdleAsync(SX_ICoreDispatcher* self,
                                             SX_IDispatchedHandler* callback,
                                             SX_IInspectable** asyncAction) {
    return Cd_RunAsync(self, 0, callback, asyncAction);
}
const SX_ICoreDispatcherVtbl kCoreDispatcherVtbl = {
    &Cd_QI, &Cd_AddRef, &Cd_Release,
    &Cd_GetIids, &Cd_GetRuntimeClassName, &Cd_GetTrustLevel,
    &Cd_GetHasThreadAccess, &Cd_ProcessEvents, &Cd_RunAsync, &Cd_RunIdleAsync,
};

CoreDispatcherObj* CreateCoreDispatcher() {
    auto* obj = new (std::nothrow) CoreDispatcherObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kCoreDispatcherVtbl;
    return obj;
}

struct SX_ICoreWindow;
struct SX_ICoreWindowVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreWindow*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreWindow*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreWindow*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreWindow*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreWindow*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreWindow*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_AutomationHostProvider)(SX_ICoreWindow*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* get_Bounds)(SX_ICoreWindow*, SX_Rect*);
    int32_t(SCARLETTX_WINRT_CALL* get_CustomProperties)(SX_ICoreWindow*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* get_Dispatcher)(SX_ICoreWindow*, SX_ICoreDispatcher**);
    int32_t(SCARLETTX_WINRT_CALL* get_FlowDirection)(SX_ICoreWindow*, int32_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_FlowDirection)(SX_ICoreWindow*, int32_t);
    int32_t(SCARLETTX_WINRT_CALL* get_IsInputEnabled)(SX_ICoreWindow*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_IsInputEnabled)(SX_ICoreWindow*, uint8_t);
    int32_t(SCARLETTX_WINRT_CALL* get_PointerCursor)(SX_ICoreWindow*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* put_PointerCursor)(SX_ICoreWindow*, SX_IInspectable*);
    int32_t(SCARLETTX_WINRT_CALL* get_PointerPosition)(SX_ICoreWindow*, SX_Point*);
    int32_t(SCARLETTX_WINRT_CALL* get_Visible)(SX_ICoreWindow*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* Activate)(SX_ICoreWindow*);
    int32_t(SCARLETTX_WINRT_CALL* Close)(SX_ICoreWindow*);
    int32_t(SCARLETTX_WINRT_CALL* GetAsyncKeyState)(SX_ICoreWindow*, int32_t, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* GetKeyState)(SX_ICoreWindow*, int32_t, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* ReleasePointerCapture)(SX_ICoreWindow*);
    int32_t(SCARLETTX_WINRT_CALL* SetPointerCapture)(SX_ICoreWindow*);
    int32_t(SCARLETTX_WINRT_CALL* add_Activated)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Activated)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_AutomationProviderRequested)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_AutomationProviderRequested)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_CharacterReceived)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_CharacterReceived)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_Closed)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Closed)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_InputEnabled)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_InputEnabled)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_KeyDown)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_KeyDown)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_KeyUp)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_KeyUp)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_PointerCaptureLost)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_PointerCaptureLost)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_PointerEntered)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_PointerEntered)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_PointerExited)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_PointerExited)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_PointerMoved)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_PointerMoved)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_PointerPressed)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_PointerPressed)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_PointerReleased)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_PointerReleased)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_TouchHitTesting)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_TouchHitTesting)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_PointerWheelChanged)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_PointerWheelChanged)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_SizeChanged)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_SizeChanged)(SX_ICoreWindow*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_VisibilityChanged)(SX_ICoreWindow*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_VisibilityChanged)(SX_ICoreWindow*, SX_EventRegistrationToken);
};
struct SX_ICoreWindow {
    const SX_ICoreWindowVtbl* lpVtbl;
};

struct CoreWindowObj {
    const SX_ICoreWindowVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    int32_t flow_dir = 0;
    uint8_t input_enabled = 1;
};

int32_t SCARLETTX_WINRT_CALL Cw_QI(SX_ICoreWindow* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<CoreWindowObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidICoreWindow)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Cw_AddRef(SX_ICoreWindow* iface) {
    return reinterpret_cast<CoreWindowObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Cw_Release(SX_ICoreWindow* iface) {
    auto* self = reinterpret_cast<CoreWindowObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetIids(SX_ICoreWindow*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {kIidICoreWindow};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Cw_GetRuntimeClassName(SX_ICoreWindow*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.UI.Core.CoreWindow", name);
}
int32_t SCARLETTX_WINRT_CALL Cw_GetTrustLevel(SX_ICoreWindow*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetNullInspectable(SX_ICoreWindow*, SX_IInspectable** out) {
    if (!out) return SX_WINRT_E_POINTER;
    *out = nullptr;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetBounds(SX_ICoreWindow*, SX_Rect* value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    *value = ls.window_bounds;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetDispatcher(SX_ICoreWindow*, SX_ICoreDispatcher** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* d = CreateCoreDispatcher();
    if (!d) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_ICoreDispatcher*>(d);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetFlowDirection(SX_ICoreWindow* iface, int32_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = reinterpret_cast<CoreWindowObj*>(iface)->flow_dir;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_PutFlowDirection(SX_ICoreWindow* iface, int32_t value) {
    reinterpret_cast<CoreWindowObj*>(iface)->flow_dir = value;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetIsInputEnabled(SX_ICoreWindow* iface, uint8_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = reinterpret_cast<CoreWindowObj*>(iface)->input_enabled;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_PutIsInputEnabled(SX_ICoreWindow* iface, uint8_t value) {
    reinterpret_cast<CoreWindowObj*>(iface)->input_enabled = value ? 1u : 0u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_PutPointerCursor(SX_ICoreWindow*, SX_IInspectable*) {
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetPointerPosition(SX_ICoreWindow*, SX_Point* value) {
    if (!value) return SX_WINRT_E_POINTER;
    value->X = 0.0f;
    value->Y = 0.0f;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetVisible(SX_ICoreWindow*, uint8_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = GetLifecycleState().window_visible.load(std::memory_order_acquire);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_Activate(SX_ICoreWindow*) {
    GetLifecycleState().window_visible.store(1u, std::memory_order_release);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_Close(SX_ICoreWindow*) {
    GetLifecycleState().window_visible.store(0u, std::memory_order_release);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetKeyState(SX_ICoreWindow*, int32_t, uint32_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = 0u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_NoOp(SX_ICoreWindow*) {
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_AddActivated(SX_ICoreWindow*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().window_activated_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Cw_RemoveActivated(SX_ICoreWindow*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().window_activated_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Cw_AddSizeChanged(SX_ICoreWindow*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().window_size_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Cw_RemoveSizeChanged(SX_ICoreWindow*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().window_size_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Cw_AddVisibilityChanged(SX_ICoreWindow*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().window_visibility_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Cw_RemoveVisibilityChanged(SX_ICoreWindow*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().window_visibility_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Cw_AddDummyEvent(SX_ICoreWindow*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    if (!h || !tok) return SX_WINRT_E_INVALIDARG;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    tok->value = ls.next_token++;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_RemoveDummyEvent(SX_ICoreWindow*, SX_EventRegistrationToken) {
    return SX_WINRT_S_OK;
}

const SX_ICoreWindowVtbl kCoreWindowVtbl = {
    &Cw_QI, &Cw_AddRef, &Cw_Release,
    &Cw_GetIids, &Cw_GetRuntimeClassName, &Cw_GetTrustLevel,
    &Cw_GetNullInspectable, &Cw_GetBounds, &Cw_GetNullInspectable, &Cw_GetDispatcher,
    &Cw_GetFlowDirection, &Cw_PutFlowDirection,
    &Cw_GetIsInputEnabled, &Cw_PutIsInputEnabled,
    &Cw_GetNullInspectable, &Cw_PutPointerCursor,
    &Cw_GetPointerPosition, &Cw_GetVisible,
    &Cw_Activate, &Cw_Close,
    &Cw_GetKeyState, &Cw_GetKeyState,
    &Cw_NoOp, &Cw_NoOp,
    &Cw_AddActivated, &Cw_RemoveActivated,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddDummyEvent, &Cw_RemoveDummyEvent,
    &Cw_AddSizeChanged, &Cw_RemoveSizeChanged,
    &Cw_AddVisibilityChanged, &Cw_RemoveVisibilityChanged,
};

CoreWindowObj* CreateCoreWindow() {
    auto* obj = new (std::nothrow) CoreWindowObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kCoreWindowVtbl;
    return obj;
}

struct SX_ICoreWindowStatic;
struct SX_ICoreWindowStaticVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreWindowStatic*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreWindowStatic*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreWindowStatic*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreWindowStatic*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreWindowStatic*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreWindowStatic*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetForCurrentThread)(SX_ICoreWindowStatic*, SX_ICoreWindow**);
};
struct SX_ICoreWindowStatic {
    const SX_ICoreWindowStaticVtbl* lpVtbl;
};

struct CoreWindowFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_ICoreWindowStaticVtbl* statics_vtbl;
    std::atomic<uint32_t> ref_count{1};
};
int32_t CwFactory_QI_Common(CoreWindowFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidICoreWindowStatic)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t CwFactory_AddRef_Common(CoreWindowFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t CwFactory_Release_Common(CoreWindowFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Cwf_QI(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return CwFactory_QI_Common(reinterpret_cast<CoreWindowFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Cwf_AddRef(SX_IActivationFactory* iface) {
    return CwFactory_AddRef_Common(reinterpret_cast<CoreWindowFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Cwf_Release(SX_IActivationFactory* iface) {
    return CwFactory_Release_Common(reinterpret_cast<CoreWindowFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Cwf_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, kIidICoreWindowStatic};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Cwf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.UI.Core.CoreWindow", name);
}
int32_t SCARLETTX_WINRT_CALL Cwf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cwf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* w = CreateCoreWindow();
    if (!w) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(w);
    return SX_WINRT_S_OK;
}
CoreWindowFactoryObj* FromCwStatics(SX_ICoreWindowStatic* iface) {
    return reinterpret_cast<CoreWindowFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(CoreWindowFactoryObj, statics_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Cws_QI(SX_ICoreWindowStatic* iface, const SX_GUID* riid, void** ppv) {
    return CwFactory_QI_Common(FromCwStatics(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Cws_AddRef(SX_ICoreWindowStatic* iface) {
    return CwFactory_AddRef_Common(FromCwStatics(iface));
}
uint32_t SCARLETTX_WINRT_CALL Cws_Release(SX_ICoreWindowStatic* iface) {
    return CwFactory_Release_Common(FromCwStatics(iface));
}
int32_t SCARLETTX_WINRT_CALL Cws_GetIids(SX_ICoreWindowStatic*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, kIidICoreWindowStatic};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Cws_GetRuntimeClassName(SX_ICoreWindowStatic*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.UI.Core.CoreWindow", name);
}
int32_t SCARLETTX_WINRT_CALL Cws_GetTrustLevel(SX_ICoreWindowStatic*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cws_GetForCurrentThread(SX_ICoreWindowStatic*, SX_ICoreWindow** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    auto* w = CreateCoreWindow();
    if (!w) {
        *ppv = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *ppv = reinterpret_cast<SX_ICoreWindow*>(w);
    return SX_WINRT_S_OK;
}
const SX_IActivationFactoryVtbl kCoreWindowFactoryVtbl = {
    &Cwf_QI, &Cwf_AddRef, &Cwf_Release,
    &Cwf_GetIids, &Cwf_GetRuntimeClassName, &Cwf_GetTrustLevel,
    &Cwf_ActivateInstance,
};
const SX_ICoreWindowStaticVtbl kCoreWindowStaticsVtbl = {
    &Cws_QI, &Cws_AddRef, &Cws_Release,
    &Cws_GetIids, &Cws_GetRuntimeClassName, &Cws_GetTrustLevel,
    &Cws_GetForCurrentThread,
};

// --- Built-in Class 6: Windows.ApplicationModel.Core.CoreApplication & View ---
struct SX_ICoreApplicationView;
struct SX_ICoreApplicationViewVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreApplicationView*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreApplicationView*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreApplicationView*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreApplicationView*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreApplicationView*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreApplicationView*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_CoreWindow)(SX_ICoreApplicationView*, SX_ICoreWindow**);
    int32_t(SCARLETTX_WINRT_CALL* add_Activated)(SX_ICoreApplicationView*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Activated)(SX_ICoreApplicationView*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* get_IsMain)(SX_ICoreApplicationView*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_IsHosted)(SX_ICoreApplicationView*, uint8_t*);
};
struct SX_ICoreApplicationView {
    const SX_ICoreApplicationViewVtbl* lpVtbl;
};
struct CoreAppViewObj {
    const SX_ICoreApplicationViewVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};
int32_t SCARLETTX_WINRT_CALL Cav_QI(SX_ICoreApplicationView* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    auto* self = reinterpret_cast<CoreAppViewObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidICoreApplicationView)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Cav_AddRef(SX_ICoreApplicationView* iface) {
    return reinterpret_cast<CoreAppViewObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Cav_Release(SX_ICoreApplicationView* iface) {
    auto* self = reinterpret_cast<CoreAppViewObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Cav_GetIids(SX_ICoreApplicationView*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {kIidICoreApplicationView};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Cav_GetRuntimeClassName(SX_ICoreApplicationView*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.ApplicationModel.Core.CoreApplicationView", name);
}
int32_t SCARLETTX_WINRT_CALL Cav_GetTrustLevel(SX_ICoreApplicationView*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cav_GetCoreWindow(SX_ICoreApplicationView*, SX_ICoreWindow** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* w = CreateCoreWindow();
    if (!w) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_ICoreWindow*>(w);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cav_AddActivated(SX_ICoreApplicationView*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().view_activated_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Cav_RemoveActivated(SX_ICoreApplicationView*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().view_activated_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Cav_GetIsMain(SX_ICoreApplicationView*, uint8_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = 1u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cav_GetIsHosted(SX_ICoreApplicationView*, uint8_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = 0u;
    return SX_WINRT_S_OK;
}
const SX_ICoreApplicationViewVtbl kCoreAppViewVtbl = {
    &Cav_QI, &Cav_AddRef, &Cav_Release,
    &Cav_GetIids, &Cav_GetRuntimeClassName, &Cav_GetTrustLevel,
    &Cav_GetCoreWindow, &Cav_AddActivated, &Cav_RemoveActivated,
    &Cav_GetIsMain, &Cav_GetIsHosted,
};
CoreAppViewObj* CreateCoreAppView() {
    auto* obj = new (std::nothrow) CoreAppViewObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kCoreAppViewVtbl;
    return obj;
}

struct SX_ICoreApplication;
struct SX_ICoreApplicationVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreApplication*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreApplication*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreApplication*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreApplication*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreApplication*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreApplication*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_Id)(SX_ICoreApplication*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* add_Suspending)(SX_ICoreApplication*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Suspending)(SX_ICoreApplication*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_Resuming)(SX_ICoreApplication*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Resuming)(SX_ICoreApplication*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* get_Properties)(SX_ICoreApplication*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* GetCurrentView)(SX_ICoreApplication*, SX_ICoreApplicationView**);
    int32_t(SCARLETTX_WINRT_CALL* Run)(SX_ICoreApplication*, void*);
    int32_t(SCARLETTX_WINRT_CALL* RunWithActivationFactories)(SX_ICoreApplication*, void*);
};
struct SX_ICoreApplication {
    const SX_ICoreApplicationVtbl* lpVtbl;
};

struct SX_ICoreApplication2;
struct SX_ICoreApplication2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreApplication2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreApplication2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreApplication2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreApplication2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreApplication2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreApplication2*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* add_BackgroundActivated)(SX_ICoreApplication2*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_BackgroundActivated)(SX_ICoreApplication2*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_LeavingBackground)(SX_ICoreApplication2*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_LeavingBackground)(SX_ICoreApplication2*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* add_EnteredBackground)(SX_ICoreApplication2*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_EnteredBackground)(SX_ICoreApplication2*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* EnablePrelaunch)(SX_ICoreApplication2*, uint8_t);
};
struct SX_ICoreApplication2 {
    const SX_ICoreApplication2Vtbl* lpVtbl;
};

struct SX_ICoreApplicationExit;
struct SX_ICoreApplicationExitVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreApplicationExit*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreApplicationExit*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreApplicationExit*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreApplicationExit*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreApplicationExit*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreApplicationExit*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* Exit)(SX_ICoreApplicationExit*);
    int32_t(SCARLETTX_WINRT_CALL* add_Exiting)(SX_ICoreApplicationExit*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Exiting)(SX_ICoreApplicationExit*, SX_EventRegistrationToken);
};
struct SX_ICoreApplicationExit {
    const SX_ICoreApplicationExitVtbl* lpVtbl;
};

struct SX_ICoreApplicationUseCount;
struct SX_ICoreApplicationUseCountVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ICoreApplicationUseCount*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ICoreApplicationUseCount*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ICoreApplicationUseCount*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ICoreApplicationUseCount*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ICoreApplicationUseCount*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ICoreApplicationUseCount*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* IncrementApplicationUseCount)(SX_ICoreApplicationUseCount*);
    int32_t(SCARLETTX_WINRT_CALL* DecrementApplicationUseCount)(SX_ICoreApplicationUseCount*);
};
struct SX_ICoreApplicationUseCount {
    const SX_ICoreApplicationUseCountVtbl* lpVtbl;
};

struct CoreApplicationFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_ICoreApplicationVtbl* app_vtbl;
    const SX_ICoreApplication2Vtbl* app2_vtbl;
    const SX_ICoreApplicationExitVtbl* exit_vtbl;
    const SX_ICoreApplicationUseCountVtbl* use_count_vtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t CoreApp_QI_Common(CoreApplicationFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidICoreApplication)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->app_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidICoreApplication2)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->app2_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidICoreApplicationExit)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->exit_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidICoreApplicationUseCount)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->use_count_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t CoreApp_AddRef_Common(CoreApplicationFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t CoreApp_Release_Common(CoreApplicationFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t CoreApp_GetIids_Common(uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {
        SX_IID_IActivationFactory, kIidICoreApplication, kIidICoreApplication2,
        kIidICoreApplicationExit, kIidICoreApplicationUseCount};
    return CopyIids(kIids, 5, count, iids);
}
int32_t CoreApp_GetClassName_Common(SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.ApplicationModel.Core.CoreApplication", name);
}
int32_t CoreApp_GetTrust_Common(SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}

int32_t SCARLETTX_WINRT_CALL Caf_QI(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return CoreApp_QI_Common(reinterpret_cast<CoreApplicationFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Caf_AddRef(SX_IActivationFactory* iface) {
    return CoreApp_AddRef_Common(reinterpret_cast<CoreApplicationFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Caf_Release(SX_IActivationFactory* iface) {
    return CoreApp_Release_Common(reinterpret_cast<CoreApplicationFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Caf_GetIids(SX_IActivationFactory*, uint32_t* c, SX_GUID** i) { return CoreApp_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Caf_GetClassName(SX_IActivationFactory*, SX_HSTRING* n) { return CoreApp_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Caf_GetTrust(SX_IActivationFactory*, SX_TrustLevel* l) { return CoreApp_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Caf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* v = CreateCoreAppView();
    if (!v) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(v);
    return SX_WINRT_S_OK;
}

CoreApplicationFactoryObj* FromCoreApp1(SX_ICoreApplication* iface) {
    return reinterpret_cast<CoreApplicationFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(CoreApplicationFactoryObj, app_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Ca1_QI(SX_ICoreApplication* i, const SX_GUID* r, void** p) { return CoreApp_QI_Common(FromCoreApp1(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Ca1_AddRef(SX_ICoreApplication* i) { return CoreApp_AddRef_Common(FromCoreApp1(i)); }
uint32_t SCARLETTX_WINRT_CALL Ca1_Release(SX_ICoreApplication* i) { return CoreApp_Release_Common(FromCoreApp1(i)); }
int32_t SCARLETTX_WINRT_CALL Ca1_GetIids(SX_ICoreApplication*, uint32_t* c, SX_GUID** i) { return CoreApp_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Ca1_GetClassName(SX_ICoreApplication*, SX_HSTRING* n) { return CoreApp_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Ca1_GetTrust(SX_ICoreApplication*, SX_TrustLevel* l) { return CoreApp_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Ca1_GetId(SX_ICoreApplication*, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"OpenScarlett.App", value);
}
int32_t SCARLETTX_WINRT_CALL Ca1_AddSuspending(SX_ICoreApplication*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().suspending_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca1_RemoveSuspending(SX_ICoreApplication*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().suspending_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca1_AddResuming(SX_ICoreApplication*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().resuming_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca1_RemoveResuming(SX_ICoreApplication*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().resuming_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca1_GetProperties(SX_ICoreApplication*, SX_IInspectable** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* obj = new (std::nothrow) GenericInspectableObj();
    if (!obj) {
        *out = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    obj->lpVtbl = &kGenericInspectableVtbl;
    obj->class_name = u"Windows.Foundation.Collections.PropertySet";
    *out = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ca1_GetCurrentView(SX_ICoreApplication*, SX_ICoreApplicationView** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* v = CreateCoreAppView();
    if (!v) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_ICoreApplicationView*>(v);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ca1_Run(SX_ICoreApplication*, void*) { return SX_WINRT_S_OK; }

CoreApplicationFactoryObj* FromCoreApp2(SX_ICoreApplication2* iface) {
    return reinterpret_cast<CoreApplicationFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(CoreApplicationFactoryObj, app2_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Ca2_QI(SX_ICoreApplication2* i, const SX_GUID* r, void** p) { return CoreApp_QI_Common(FromCoreApp2(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Ca2_AddRef(SX_ICoreApplication2* i) { return CoreApp_AddRef_Common(FromCoreApp2(i)); }
uint32_t SCARLETTX_WINRT_CALL Ca2_Release(SX_ICoreApplication2* i) { return CoreApp_Release_Common(FromCoreApp2(i)); }
int32_t SCARLETTX_WINRT_CALL Ca2_GetIids(SX_ICoreApplication2*, uint32_t* c, SX_GUID** i) { return CoreApp_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Ca2_GetClassName(SX_ICoreApplication2*, SX_HSTRING* n) { return CoreApp_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Ca2_GetTrust(SX_ICoreApplication2*, SX_TrustLevel* l) { return CoreApp_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Ca2_AddBgActivated(SX_ICoreApplication2*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    if (!h || !tok) return SX_WINRT_E_INVALIDARG;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    tok->value = ls.next_token++;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ca2_RemoveBgActivated(SX_ICoreApplication2*, SX_EventRegistrationToken) { return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Ca2_AddLeavingBg(SX_ICoreApplication2*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().leaving_bg_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca2_RemoveLeavingBg(SX_ICoreApplication2*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().leaving_bg_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca2_AddEnteredBg(SX_ICoreApplication2*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().entered_bg_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca2_RemoveEnteredBg(SX_ICoreApplication2*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().entered_bg_handlers, tok);
}
int32_t SCARLETTX_WINRT_CALL Ca2_EnablePrelaunch(SX_ICoreApplication2*, uint8_t) { return SX_WINRT_S_OK; }

CoreApplicationFactoryObj* FromCoreAppExit(SX_ICoreApplicationExit* iface) {
    return reinterpret_cast<CoreApplicationFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(CoreApplicationFactoryObj, exit_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Cae_QI(SX_ICoreApplicationExit* i, const SX_GUID* r, void** p) { return CoreApp_QI_Common(FromCoreAppExit(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Cae_AddRef(SX_ICoreApplicationExit* i) { return CoreApp_AddRef_Common(FromCoreAppExit(i)); }
uint32_t SCARLETTX_WINRT_CALL Cae_Release(SX_ICoreApplicationExit* i) { return CoreApp_Release_Common(FromCoreAppExit(i)); }
int32_t SCARLETTX_WINRT_CALL Cae_GetIids(SX_ICoreApplicationExit*, uint32_t* c, SX_GUID** i) { return CoreApp_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Cae_GetClassName(SX_ICoreApplicationExit*, SX_HSTRING* n) { return CoreApp_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Cae_GetTrust(SX_ICoreApplicationExit*, SX_TrustLevel* l) { return CoreApp_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Cae_Exit(SX_ICoreApplicationExit*) {
    DispatchEventList(GetLifecycleState().exiting_handlers, nullptr, nullptr);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cae_AddExiting(SX_ICoreApplicationExit*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    return AddEventHandler(GetLifecycleState().exiting_handlers, h, tok);
}
int32_t SCARLETTX_WINRT_CALL Cae_RemoveExiting(SX_ICoreApplicationExit*, SX_EventRegistrationToken tok) {
    return RemoveEventHandler(GetLifecycleState().exiting_handlers, tok);
}

CoreApplicationFactoryObj* FromCoreAppUse(SX_ICoreApplicationUseCount* iface) {
    return reinterpret_cast<CoreApplicationFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(CoreApplicationFactoryObj, use_count_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Cau_QI(SX_ICoreApplicationUseCount* i, const SX_GUID* r, void** p) { return CoreApp_QI_Common(FromCoreAppUse(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Cau_AddRef(SX_ICoreApplicationUseCount* i) { return CoreApp_AddRef_Common(FromCoreAppUse(i)); }
uint32_t SCARLETTX_WINRT_CALL Cau_Release(SX_ICoreApplicationUseCount* i) { return CoreApp_Release_Common(FromCoreAppUse(i)); }
int32_t SCARLETTX_WINRT_CALL Cau_GetIids(SX_ICoreApplicationUseCount*, uint32_t* c, SX_GUID** i) { return CoreApp_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Cau_GetClassName(SX_ICoreApplicationUseCount*, SX_HSTRING* n) { return CoreApp_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Cau_GetTrust(SX_ICoreApplicationUseCount*, SX_TrustLevel* l) { return CoreApp_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Cau_Inc(SX_ICoreApplicationUseCount*) {
    GetLifecycleState().use_count.fetch_add(1, std::memory_order_relaxed);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cau_Dec(SX_ICoreApplicationUseCount*) {
    GetLifecycleState().use_count.fetch_sub(1, std::memory_order_relaxed);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kCoreAppFactoryVtbl = {
    &Caf_QI, &Caf_AddRef, &Caf_Release,
    &Caf_GetIids, &Caf_GetClassName, &Caf_GetTrust,
    &Caf_ActivateInstance,
};
const SX_ICoreApplicationVtbl kCoreApp1Vtbl = {
    &Ca1_QI, &Ca1_AddRef, &Ca1_Release,
    &Ca1_GetIids, &Ca1_GetClassName, &Ca1_GetTrust,
    &Ca1_GetId, &Ca1_AddSuspending, &Ca1_RemoveSuspending,
    &Ca1_AddResuming, &Ca1_RemoveResuming,
    &Ca1_GetProperties, &Ca1_GetCurrentView, &Ca1_Run, &Ca1_Run,
};
const SX_ICoreApplication2Vtbl kCoreApp2Vtbl = {
    &Ca2_QI, &Ca2_AddRef, &Ca2_Release,
    &Ca2_GetIids, &Ca2_GetClassName, &Ca2_GetTrust,
    &Ca2_AddBgActivated, &Ca2_RemoveBgActivated,
    &Ca2_AddLeavingBg, &Ca2_RemoveLeavingBg,
    &Ca2_AddEnteredBg, &Ca2_RemoveEnteredBg,
    &Ca2_EnablePrelaunch,
};
const SX_ICoreApplicationExitVtbl kCoreAppExitVtbl = {
    &Cae_QI, &Cae_AddRef, &Cae_Release,
    &Cae_GetIids, &Cae_GetClassName, &Cae_GetTrust,
    &Cae_Exit, &Cae_AddExiting, &Cae_RemoveExiting,
};
const SX_ICoreApplicationUseCountVtbl kCoreAppUseCountVtbl = {
    &Cau_QI, &Cau_AddRef, &Cau_Release,
    &Cau_GetIids, &Cau_GetClassName, &Cau_GetTrust,
    &Cau_Inc, &Cau_Dec,
};

// --- Built-in Class 7: Windows.UI.ViewManagement.ApplicationView ---
struct SX_IApplicationView;
struct SX_IApplicationViewVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IApplicationView*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IApplicationView*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IApplicationView*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IApplicationView*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IApplicationView*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IApplicationView*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_Orientation)(SX_IApplicationView*, int32_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_AdjacentToLeftDisplayEdge)(SX_IApplicationView*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_AdjacentToRightDisplayEdge)(SX_IApplicationView*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_IsFullScreen)(SX_IApplicationView*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_IsOnLockScreen)(SX_IApplicationView*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_IsScreenCaptureEnabled)(SX_IApplicationView*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_IsScreenCaptureEnabled)(SX_IApplicationView*, uint8_t);
    int32_t(SCARLETTX_WINRT_CALL* put_Title)(SX_IApplicationView*, SX_HSTRING);
    int32_t(SCARLETTX_WINRT_CALL* get_Title)(SX_IApplicationView*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* get_Id)(SX_IApplicationView*, int32_t*);
    int32_t(SCARLETTX_WINRT_CALL* add_Consolidated)(SX_IApplicationView*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Consolidated)(SX_IApplicationView*, SX_EventRegistrationToken);
};
struct SX_IApplicationView {
    const SX_IApplicationViewVtbl* lpVtbl;
};

struct SX_IApplicationView2;
struct SX_IApplicationView2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IApplicationView2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IApplicationView2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IApplicationView2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IApplicationView2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IApplicationView2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IApplicationView2*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_SuppressSystemOverlays)(SX_IApplicationView2*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_SuppressSystemOverlays)(SX_IApplicationView2*, uint8_t);
    int32_t(SCARLETTX_WINRT_CALL* get_VisibleBounds)(SX_IApplicationView2*, SX_Rect*);
    int32_t(SCARLETTX_WINRT_CALL* add_VisibleBoundsChanged)(SX_IApplicationView2*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_VisibleBoundsChanged)(SX_IApplicationView2*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* SetDesiredBoundsMode)(SX_IApplicationView2*, int32_t, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_DesiredBoundsMode)(SX_IApplicationView2*, int32_t*);
};
struct SX_IApplicationView2 {
    const SX_IApplicationView2Vtbl* lpVtbl;
};

struct SX_IApplicationView3;
struct SX_IApplicationView3Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IApplicationView3*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IApplicationView3*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IApplicationView3*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IApplicationView3*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IApplicationView3*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IApplicationView3*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_TitleBar)(SX_IApplicationView3*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* get_FullScreenSystemOverlayMode)(SX_IApplicationView3*, int32_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_FullScreenSystemOverlayMode)(SX_IApplicationView3*, int32_t);
    int32_t(SCARLETTX_WINRT_CALL* get_IsFullScreenMode)(SX_IApplicationView3*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* TryEnterFullScreenMode)(SX_IApplicationView3*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* ExitFullScreenMode)(SX_IApplicationView3*);
    int32_t(SCARLETTX_WINRT_CALL* ShowStandardSystemOverlays)(SX_IApplicationView3*);
    int32_t(SCARLETTX_WINRT_CALL* TryResizeView)(SX_IApplicationView3*, SX_Size, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* SetPreferredMinSize)(SX_IApplicationView3*, SX_Size);
};
struct SX_IApplicationView3 {
    const SX_IApplicationView3Vtbl* lpVtbl;
};

struct ApplicationViewObj {
    const SX_IApplicationViewVtbl* av1_vtbl;
    const SX_IApplicationView2Vtbl* av2_vtbl;
    const SX_IApplicationView3Vtbl* av3_vtbl;
    std::atomic<uint32_t> ref_count{1};
    uint8_t screen_capture_enabled = 1;
    uint8_t suppress_overlays = 0;
    int32_t bounds_mode = 0;
    int32_t overlay_mode = 0;
};

int32_t AppView_QI_Common(ApplicationViewObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidIApplicationView)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->av1_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidIApplicationView2)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->av2_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidIApplicationView3)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->av3_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t AppView_AddRef_Common(ApplicationViewObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t AppView_Release_Common(ApplicationViewObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t AppView_GetIids_Common(uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {kIidIApplicationView, kIidIApplicationView2, kIidIApplicationView3};
    return CopyIids(kIids, 3, count, iids);
}
int32_t AppView_GetClassName_Common(SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.UI.ViewManagement.ApplicationView", name);
}
int32_t AppView_GetTrust_Common(SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}

int32_t SCARLETTX_WINRT_CALL Av1_QI(SX_IApplicationView* i, const SX_GUID* r, void** p) {
    return AppView_QI_Common(reinterpret_cast<ApplicationViewObj*>(i), r, p);
}
uint32_t SCARLETTX_WINRT_CALL Av1_AddRef(SX_IApplicationView* i) { return AppView_AddRef_Common(reinterpret_cast<ApplicationViewObj*>(i)); }
uint32_t SCARLETTX_WINRT_CALL Av1_Release(SX_IApplicationView* i) { return AppView_Release_Common(reinterpret_cast<ApplicationViewObj*>(i)); }
int32_t SCARLETTX_WINRT_CALL Av1_GetIids(SX_IApplicationView*, uint32_t* c, SX_GUID** i) { return AppView_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Av1_GetClassName(SX_IApplicationView*, SX_HSTRING* n) { return AppView_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Av1_GetTrust(SX_IApplicationView*, SX_TrustLevel* l) { return AppView_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Av1_GetOrientation(SX_IApplicationView*, int32_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = 0; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av1_GetEdgeTrue(SX_IApplicationView*, uint8_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = 1u; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av1_GetIsFullScreen(SX_IApplicationView*, uint8_t* v) {
    if (!v) return SX_WINRT_E_POINTER;
    *v = GetLifecycleState().appview_fullscreen.load(std::memory_order_acquire);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av1_GetIsOnLockScreen(SX_IApplicationView*, uint8_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = 0u; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av1_GetScreenCap(SX_IApplicationView* i, uint8_t* v) {
    if (!v) return SX_WINRT_E_POINTER;
    *v = reinterpret_cast<ApplicationViewObj*>(i)->screen_capture_enabled;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av1_PutScreenCap(SX_IApplicationView* i, uint8_t v) {
    reinterpret_cast<ApplicationViewObj*>(i)->screen_capture_enabled = v ? 1u : 0u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av1_PutTitle(SX_IApplicationView*, SX_HSTRING title) {
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    ls.appview_title = HStringToU16(title);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av1_GetTitle(SX_IApplicationView*, SX_HSTRING* title) {
    if (!title) return SX_WINRT_E_POINTER;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    return CreateHStringFromU16(ls.appview_title, title);
}
int32_t SCARLETTX_WINRT_CALL Av1_GetId(SX_IApplicationView*, int32_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = 1; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av1_AddConsol(SX_IApplicationView*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    if (!h || !tok) return SX_WINRT_E_INVALIDARG;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    tok->value = ls.next_token++;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av1_RemConsol(SX_IApplicationView*, SX_EventRegistrationToken) { return SX_WINRT_S_OK; }

ApplicationViewObj* FromAv2(SX_IApplicationView2* iface) {
    return reinterpret_cast<ApplicationViewObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(ApplicationViewObj, av2_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Av2_QI(SX_IApplicationView2* i, const SX_GUID* r, void** p) { return AppView_QI_Common(FromAv2(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Av2_AddRef(SX_IApplicationView2* i) { return AppView_AddRef_Common(FromAv2(i)); }
uint32_t SCARLETTX_WINRT_CALL Av2_Release(SX_IApplicationView2* i) { return AppView_Release_Common(FromAv2(i)); }
int32_t SCARLETTX_WINRT_CALL Av2_GetIids(SX_IApplicationView2*, uint32_t* c, SX_GUID** i) { return AppView_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Av2_GetClassName(SX_IApplicationView2*, SX_HSTRING* n) { return AppView_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Av2_GetTrust(SX_IApplicationView2*, SX_TrustLevel* l) { return AppView_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Av2_GetSuppress(SX_IApplicationView2* i, uint8_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = FromAv2(i)->suppress_overlays; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av2_PutSuppress(SX_IApplicationView2* i, uint8_t v) { FromAv2(i)->suppress_overlays = v ? 1u : 0u; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av2_GetVisibleBounds(SX_IApplicationView2*, SX_Rect* value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    *value = ls.window_bounds;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av2_AddVbc(SX_IApplicationView2*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    if (!h || !tok) return SX_WINRT_E_INVALIDARG;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    tok->value = ls.next_token++;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av2_RemVbc(SX_IApplicationView2*, SX_EventRegistrationToken) { return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av2_SetBoundsMode(SX_IApplicationView2* i, int32_t mode, uint8_t* ok) {
    FromAv2(i)->bounds_mode = mode;
    if (ok) *ok = 1u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av2_GetBoundsMode(SX_IApplicationView2* i, int32_t* mode) {
    if (!mode) return SX_WINRT_E_POINTER;
    *mode = FromAv2(i)->bounds_mode;
    return SX_WINRT_S_OK;
}

ApplicationViewObj* FromAv3(SX_IApplicationView3* iface) {
    return reinterpret_cast<ApplicationViewObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(ApplicationViewObj, av3_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Av3_QI(SX_IApplicationView3* i, const SX_GUID* r, void** p) { return AppView_QI_Common(FromAv3(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Av3_AddRef(SX_IApplicationView3* i) { return AppView_AddRef_Common(FromAv3(i)); }
uint32_t SCARLETTX_WINRT_CALL Av3_Release(SX_IApplicationView3* i) { return AppView_Release_Common(FromAv3(i)); }
int32_t SCARLETTX_WINRT_CALL Av3_GetIids(SX_IApplicationView3*, uint32_t* c, SX_GUID** i) { return AppView_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Av3_GetClassName(SX_IApplicationView3*, SX_HSTRING* n) { return AppView_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Av3_GetTrust(SX_IApplicationView3*, SX_TrustLevel* l) { return AppView_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Av3_GetTitleBar(SX_IApplicationView3*, SX_IInspectable** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* obj = new (std::nothrow) GenericInspectableObj();
    if (!obj) { *out = nullptr; return SX_WINRT_E_OUTOFMEMORY; }
    obj->lpVtbl = &kGenericInspectableVtbl;
    obj->class_name = u"Windows.UI.ViewManagement.ApplicationViewTitleBar";
    *out = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av3_GetOverlayMode(SX_IApplicationView3* i, int32_t* m) { if (!m) return SX_WINRT_E_POINTER; *m = FromAv3(i)->overlay_mode; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av3_PutOverlayMode(SX_IApplicationView3* i, int32_t m) { FromAv3(i)->overlay_mode = m; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av3_GetIsFullScreenMode(SX_IApplicationView3*, uint8_t* v) {
    if (!v) return SX_WINRT_E_POINTER;
    *v = GetLifecycleState().appview_fullscreen.load(std::memory_order_acquire);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av3_TryEnterFullScreenMode(SX_IApplicationView3*, uint8_t* success) {
    GetLifecycleState().appview_fullscreen.store(1u, std::memory_order_release);
    if (success) *success = 1u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av3_ExitFullScreenMode(SX_IApplicationView3*) {
    GetLifecycleState().appview_fullscreen.store(0u, std::memory_order_release);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av3_ShowOverlays(SX_IApplicationView3*) { return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Av3_TryResizeView(SX_IApplicationView3*, SX_Size sz, uint8_t* ok) {
    if (sz.Width > 0.0f && sz.Height > 0.0f) {
        auto& ls = GetLifecycleState();
        std::lock_guard<std::mutex> lk(ls.mutex);
        ls.window_bounds.Width = sz.Width;
        ls.window_bounds.Height = sz.Height;
    }
    if (ok) *ok = 1u;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Av3_SetPreferredMinSize(SX_IApplicationView3*, SX_Size) { return SX_WINRT_S_OK; }

const SX_IApplicationViewVtbl kAppView1Vtbl = {
    &Av1_QI, &Av1_AddRef, &Av1_Release,
    &Av1_GetIids, &Av1_GetClassName, &Av1_GetTrust,
    &Av1_GetOrientation, &Av1_GetEdgeTrue, &Av1_GetEdgeTrue,
    &Av1_GetIsFullScreen, &Av1_GetIsOnLockScreen,
    &Av1_GetScreenCap, &Av1_PutScreenCap,
    &Av1_PutTitle, &Av1_GetTitle, &Av1_GetId,
    &Av1_AddConsol, &Av1_RemConsol,
};
const SX_IApplicationView2Vtbl kAppView2Vtbl = {
    &Av2_QI, &Av2_AddRef, &Av2_Release,
    &Av2_GetIids, &Av2_GetClassName, &Av2_GetTrust,
    &Av2_GetSuppress, &Av2_PutSuppress, &Av2_GetVisibleBounds,
    &Av2_AddVbc, &Av2_RemVbc, &Av2_SetBoundsMode, &Av2_GetBoundsMode,
};
const SX_IApplicationView3Vtbl kAppView3Vtbl = {
    &Av3_QI, &Av3_AddRef, &Av3_Release,
    &Av3_GetIids, &Av3_GetClassName, &Av3_GetTrust,
    &Av3_GetTitleBar, &Av3_GetOverlayMode, &Av3_PutOverlayMode,
    &Av3_GetIsFullScreenMode, &Av3_TryEnterFullScreenMode, &Av3_ExitFullScreenMode,
    &Av3_ShowOverlays, &Av3_TryResizeView, &Av3_SetPreferredMinSize,
};

ApplicationViewObj* CreateApplicationView() {
    auto* obj = new (std::nothrow) ApplicationViewObj();
    if (!obj) return nullptr;
    obj->av1_vtbl = &kAppView1Vtbl;
    obj->av2_vtbl = &kAppView2Vtbl;
    obj->av3_vtbl = &kAppView3Vtbl;
    return obj;
}

struct SX_IApplicationViewStatics2;
struct SX_IApplicationViewStatics2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IApplicationViewStatics2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IApplicationViewStatics2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IApplicationViewStatics2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IApplicationViewStatics2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IApplicationViewStatics2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IApplicationViewStatics2*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetForCurrentView)(SX_IApplicationViewStatics2*, SX_IApplicationView**);
    int32_t(SCARLETTX_WINRT_CALL* get_TerminateAppOnFinalViewClose)(SX_IApplicationViewStatics2*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_TerminateAppOnFinalViewClose)(SX_IApplicationViewStatics2*, uint8_t);
};
struct SX_IApplicationViewStatics2 {
    const SX_IApplicationViewStatics2Vtbl* lpVtbl;
};

struct ApplicationViewFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_IApplicationViewStatics2Vtbl* statics2_vtbl;
    std::atomic<uint32_t> ref_count{1};
    uint8_t terminate_on_close = 1;
};
int32_t Avf_QI_Common(ApplicationViewFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidIApplicationViewStatics2)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics2_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t Avf_AddRef_Common(ApplicationViewFactoryObj* s) { return s->ref_count.fetch_add(1, std::memory_order_relaxed) + 1; }
uint32_t Avf_Release_Common(ApplicationViewFactoryObj* s) {
    const uint32_t rem = s->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete s;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Avf_QI(SX_IActivationFactory* i, const SX_GUID* r, void** p) { return Avf_QI_Common(reinterpret_cast<ApplicationViewFactoryObj*>(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Avf_AddRef(SX_IActivationFactory* i) { return Avf_AddRef_Common(reinterpret_cast<ApplicationViewFactoryObj*>(i)); }
uint32_t SCARLETTX_WINRT_CALL Avf_Release(SX_IActivationFactory* i) { return Avf_Release_Common(reinterpret_cast<ApplicationViewFactoryObj*>(i)); }
int32_t SCARLETTX_WINRT_CALL Avf_GetIids(SX_IActivationFactory*, uint32_t* c, SX_GUID** i) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, kIidIApplicationViewStatics2};
    return CopyIids(kIids, 2, c, i);
}
int32_t SCARLETTX_WINRT_CALL Avf_GetClassName(SX_IActivationFactory*, SX_HSTRING* n) { return AppView_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Avf_GetTrust(SX_IActivationFactory*, SX_TrustLevel* l) { return AppView_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Avf_Activate(SX_IActivationFactory*, SX_IInspectable** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* v = CreateApplicationView();
    if (!v) { *out = nullptr; return SX_WINRT_E_OUTOFMEMORY; }
    *out = reinterpret_cast<SX_IInspectable*>(v);
    return SX_WINRT_S_OK;
}
ApplicationViewFactoryObj* FromAvs2(SX_IApplicationViewStatics2* i) {
    return reinterpret_cast<ApplicationViewFactoryObj*>(
        reinterpret_cast<uint8_t*>(i) - offsetof(ApplicationViewFactoryObj, statics2_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Avs2_QI(SX_IApplicationViewStatics2* i, const SX_GUID* r, void** p) { return Avf_QI_Common(FromAvs2(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Avs2_AddRef(SX_IApplicationViewStatics2* i) { return Avf_AddRef_Common(FromAvs2(i)); }
uint32_t SCARLETTX_WINRT_CALL Avs2_Release(SX_IApplicationViewStatics2* i) { return Avf_Release_Common(FromAvs2(i)); }
int32_t SCARLETTX_WINRT_CALL Avs2_GetIids(SX_IApplicationViewStatics2*, uint32_t* c, SX_GUID** i) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, kIidIApplicationViewStatics2};
    return CopyIids(kIids, 2, c, i);
}
int32_t SCARLETTX_WINRT_CALL Avs2_GetClassName(SX_IApplicationViewStatics2*, SX_HSTRING* n) { return AppView_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Avs2_GetTrust(SX_IApplicationViewStatics2*, SX_TrustLevel* l) { return AppView_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Avs2_GetForCurrentView(SX_IApplicationViewStatics2*, SX_IApplicationView** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* v = CreateApplicationView();
    if (!v) { *out = nullptr; return SX_WINRT_E_OUTOFMEMORY; }
    *out = reinterpret_cast<SX_IApplicationView*>(v);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Avs2_GetTerm(SX_IApplicationViewStatics2* i, uint8_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = FromAvs2(i)->terminate_on_close; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Avs2_PutTerm(SX_IApplicationViewStatics2* i, uint8_t v) { FromAvs2(i)->terminate_on_close = v ? 1u : 0u; return SX_WINRT_S_OK; }

const SX_IActivationFactoryVtbl kAppViewFactoryVtbl = {
    &Avf_QI, &Avf_AddRef, &Avf_Release,
    &Avf_GetIids, &Avf_GetClassName, &Avf_GetTrust,
    &Avf_Activate,
};
const SX_IApplicationViewStatics2Vtbl kAppViewStatics2Vtbl = {
    &Avs2_QI, &Avs2_AddRef, &Avs2_Release,
    &Avs2_GetIids, &Avs2_GetClassName, &Avs2_GetTrust,
    &Avs2_GetForCurrentView, &Avs2_GetTerm, &Avs2_PutTerm,
};

// --- Built-in Class 8: Windows.Graphics.Display.DisplayInformation ---
struct SX_IDisplayInformation;
struct SX_IDisplayInformationVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IDisplayInformation*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IDisplayInformation*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IDisplayInformation*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IDisplayInformation*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IDisplayInformation*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IDisplayInformation*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_CurrentOrientation)(SX_IDisplayInformation*, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_NativeOrientation)(SX_IDisplayInformation*, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* add_OrientationChanged)(SX_IDisplayInformation*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_OrientationChanged)(SX_IDisplayInformation*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* get_ResolutionScale)(SX_IDisplayInformation*, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_LogicalDpi)(SX_IDisplayInformation*, float*);
    int32_t(SCARLETTX_WINRT_CALL* get_RawDpiX)(SX_IDisplayInformation*, float*);
    int32_t(SCARLETTX_WINRT_CALL* get_RawDpiY)(SX_IDisplayInformation*, float*);
    int32_t(SCARLETTX_WINRT_CALL* add_DpiChanged)(SX_IDisplayInformation*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_DpiChanged)(SX_IDisplayInformation*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* get_StereoEnabled)(SX_IDisplayInformation*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* add_StereoEnabledChanged)(SX_IDisplayInformation*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_StereoEnabledChanged)(SX_IDisplayInformation*, SX_EventRegistrationToken);
    int32_t(SCARLETTX_WINRT_CALL* GetColorProfileAsync)(SX_IDisplayInformation*, void**);
    int32_t(SCARLETTX_WINRT_CALL* add_ColorProfileChanged)(SX_IDisplayInformation*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_ColorProfileChanged)(SX_IDisplayInformation*, SX_EventRegistrationToken);
};
struct SX_IDisplayInformation {
    const SX_IDisplayInformationVtbl* lpVtbl;
};

struct SX_IDisplayInformation2;
struct SX_IDisplayInformation2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IDisplayInformation2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IDisplayInformation2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IDisplayInformation2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IDisplayInformation2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IDisplayInformation2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IDisplayInformation2*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_RawPixelsPerViewPixel)(SX_IDisplayInformation2*, double*);
};
struct SX_IDisplayInformation2 {
    const SX_IDisplayInformation2Vtbl* lpVtbl;
};

struct DisplayInfoObj {
    const SX_IDisplayInformationVtbl* di1_vtbl;
    const SX_IDisplayInformation2Vtbl* di2_vtbl;
    std::atomic<uint32_t> ref_count{1};
};
int32_t Di_QI_Common(DisplayInfoObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, kIidIDisplayInformation)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->di1_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidIDisplayInformation2)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->di2_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t Di_AddRef_Common(DisplayInfoObj* s) { return s->ref_count.fetch_add(1, std::memory_order_relaxed) + 1; }
uint32_t Di_Release_Common(DisplayInfoObj* s) {
    const uint32_t rem = s->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete s;
    return rem;
}
int32_t Di_GetIids_Common(uint32_t* c, SX_GUID** i) {
    static const SX_GUID kIids[] = {kIidIDisplayInformation, kIidIDisplayInformation2};
    return CopyIids(kIids, 2, c, i);
}
int32_t Di_GetClassName_Common(SX_HSTRING* n) {
    if (!n) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Graphics.Display.DisplayInformation", n);
}
int32_t Di_GetTrust_Common(SX_TrustLevel* l) {
    if (!l) return SX_WINRT_E_POINTER;
    *l = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Di1_QI(SX_IDisplayInformation* i, const SX_GUID* r, void** p) { return Di_QI_Common(reinterpret_cast<DisplayInfoObj*>(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Di1_AddRef(SX_IDisplayInformation* i) { return Di_AddRef_Common(reinterpret_cast<DisplayInfoObj*>(i)); }
uint32_t SCARLETTX_WINRT_CALL Di1_Release(SX_IDisplayInformation* i) { return Di_Release_Common(reinterpret_cast<DisplayInfoObj*>(i)); }
int32_t SCARLETTX_WINRT_CALL Di1_GetIids(SX_IDisplayInformation*, uint32_t* c, SX_GUID** i) { return Di_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Di1_GetClassName(SX_IDisplayInformation*, SX_HSTRING* n) { return Di_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Di1_GetTrust(SX_IDisplayInformation*, SX_TrustLevel* l) { return Di_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Di1_GetOrientation(SX_IDisplayInformation*, uint32_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = 1u; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Di1_GetResScale(SX_IDisplayInformation*, uint32_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = 100u; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Di1_GetDpi(SX_IDisplayInformation*, float* v) { if (!v) return SX_WINRT_E_POINTER; *v = 96.0f; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Di1_GetStereo(SX_IDisplayInformation*, uint8_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = 0u; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Di1_AddEvt(SX_IDisplayInformation*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    if (!h || !tok) return SX_WINRT_E_INVALIDARG;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    tok->value = ls.next_token++;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Di1_RemEvt(SX_IDisplayInformation*, SX_EventRegistrationToken) { return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Di1_GetColorProf(SX_IDisplayInformation*, void**) { return SX_WINRT_E_NOTIMPL; }

DisplayInfoObj* FromDi2(SX_IDisplayInformation2* i) {
    return reinterpret_cast<DisplayInfoObj*>(
        reinterpret_cast<uint8_t*>(i) - offsetof(DisplayInfoObj, di2_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Di2_QI(SX_IDisplayInformation2* i, const SX_GUID* r, void** p) { return Di_QI_Common(FromDi2(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Di2_AddRef(SX_IDisplayInformation2* i) { return Di_AddRef_Common(FromDi2(i)); }
uint32_t SCARLETTX_WINRT_CALL Di2_Release(SX_IDisplayInformation2* i) { return Di_Release_Common(FromDi2(i)); }
int32_t SCARLETTX_WINRT_CALL Di2_GetIids(SX_IDisplayInformation2*, uint32_t* c, SX_GUID** i) { return Di_GetIids_Common(c, i); }
int32_t SCARLETTX_WINRT_CALL Di2_GetClassName(SX_IDisplayInformation2*, SX_HSTRING* n) { return Di_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Di2_GetTrust(SX_IDisplayInformation2*, SX_TrustLevel* l) { return Di_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Di2_GetRawPixelsPerViewPixel(SX_IDisplayInformation2*, double* v) {
    if (!v) return SX_WINRT_E_POINTER;
    *v = 1.0;
    return SX_WINRT_S_OK;
}

const SX_IDisplayInformationVtbl kDisplayInfo1Vtbl = {
    &Di1_QI, &Di1_AddRef, &Di1_Release,
    &Di1_GetIids, &Di1_GetClassName, &Di1_GetTrust,
    &Di1_GetOrientation, &Di1_GetOrientation,
    &Di1_AddEvt, &Di1_RemEvt,
    &Di1_GetResScale, &Di1_GetDpi, &Di1_GetDpi, &Di1_GetDpi,
    &Di1_AddEvt, &Di1_RemEvt,
    &Di1_GetStereo, &Di1_AddEvt, &Di1_RemEvt,
    &Di1_GetColorProf, &Di1_AddEvt, &Di1_RemEvt,
};
const SX_IDisplayInformation2Vtbl kDisplayInfo2Vtbl = {
    &Di2_QI, &Di2_AddRef, &Di2_Release,
    &Di2_GetIids, &Di2_GetClassName, &Di2_GetTrust,
    &Di2_GetRawPixelsPerViewPixel,
};

DisplayInfoObj* CreateDisplayInfo() {
    auto* obj = new (std::nothrow) DisplayInfoObj();
    if (!obj) return nullptr;
    obj->di1_vtbl = &kDisplayInfo1Vtbl;
    obj->di2_vtbl = &kDisplayInfo2Vtbl;
    return obj;
}

struct SX_IDisplayInformationStatics;
struct SX_IDisplayInformationStaticsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IDisplayInformationStatics*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IDisplayInformationStatics*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IDisplayInformationStatics*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IDisplayInformationStatics*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IDisplayInformationStatics*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IDisplayInformationStatics*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetForCurrentView)(SX_IDisplayInformationStatics*, SX_IDisplayInformation**);
    int32_t(SCARLETTX_WINRT_CALL* get_AutoRotationPreferences)(SX_IDisplayInformationStatics*, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_AutoRotationPreferences)(SX_IDisplayInformationStatics*, uint32_t);
    int32_t(SCARLETTX_WINRT_CALL* add_DisplayContentsInvalidated)(SX_IDisplayInformationStatics*, SX_IEventHandler*, SX_EventRegistrationToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_DisplayContentsInvalidated)(SX_IDisplayInformationStatics*, SX_EventRegistrationToken);
};
struct SX_IDisplayInformationStatics {
    const SX_IDisplayInformationStaticsVtbl* lpVtbl;
};

struct DisplayInfoFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_IDisplayInformationStaticsVtbl* statics_vtbl;
    std::atomic<uint32_t> ref_count{1};
    uint32_t auto_rot = 1u;
};
int32_t Dif_QI_Common(DisplayInfoFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, kIidIDisplayInformationStatics)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t Dif_AddRef_Common(DisplayInfoFactoryObj* s) { return s->ref_count.fetch_add(1, std::memory_order_relaxed) + 1; }
uint32_t Dif_Release_Common(DisplayInfoFactoryObj* s) {
    const uint32_t rem = s->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete s;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Dif_QI(SX_IActivationFactory* i, const SX_GUID* r, void** p) { return Dif_QI_Common(reinterpret_cast<DisplayInfoFactoryObj*>(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Dif_AddRef(SX_IActivationFactory* i) { return Dif_AddRef_Common(reinterpret_cast<DisplayInfoFactoryObj*>(i)); }
uint32_t SCARLETTX_WINRT_CALL Dif_Release(SX_IActivationFactory* i) { return Dif_Release_Common(reinterpret_cast<DisplayInfoFactoryObj*>(i)); }
int32_t SCARLETTX_WINRT_CALL Dif_GetIids(SX_IActivationFactory*, uint32_t* c, SX_GUID** i) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, kIidIDisplayInformationStatics};
    return CopyIids(kIids, 2, c, i);
}
int32_t SCARLETTX_WINRT_CALL Dif_GetClassName(SX_IActivationFactory*, SX_HSTRING* n) { return Di_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Dif_GetTrust(SX_IActivationFactory*, SX_TrustLevel* l) { return Di_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Dif_Activate(SX_IActivationFactory*, SX_IInspectable** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* d = CreateDisplayInfo();
    if (!d) { *out = nullptr; return SX_WINRT_E_OUTOFMEMORY; }
    *out = reinterpret_cast<SX_IInspectable*>(d);
    return SX_WINRT_S_OK;
}
DisplayInfoFactoryObj* FromDis(SX_IDisplayInformationStatics* i) {
    return reinterpret_cast<DisplayInfoFactoryObj*>(
        reinterpret_cast<uint8_t*>(i) - offsetof(DisplayInfoFactoryObj, statics_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Dis_QI(SX_IDisplayInformationStatics* i, const SX_GUID* r, void** p) { return Dif_QI_Common(FromDis(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Dis_AddRef(SX_IDisplayInformationStatics* i) { return Dif_AddRef_Common(FromDis(i)); }
uint32_t SCARLETTX_WINRT_CALL Dis_Release(SX_IDisplayInformationStatics* i) { return Dif_Release_Common(FromDis(i)); }
int32_t SCARLETTX_WINRT_CALL Dis_GetIids(SX_IDisplayInformationStatics*, uint32_t* c, SX_GUID** i) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, kIidIDisplayInformationStatics};
    return CopyIids(kIids, 2, c, i);
}
int32_t SCARLETTX_WINRT_CALL Dis_GetClassName(SX_IDisplayInformationStatics*, SX_HSTRING* n) { return Di_GetClassName_Common(n); }
int32_t SCARLETTX_WINRT_CALL Dis_GetTrust(SX_IDisplayInformationStatics*, SX_TrustLevel* l) { return Di_GetTrust_Common(l); }
int32_t SCARLETTX_WINRT_CALL Dis_GetForCurrentView(SX_IDisplayInformationStatics*, SX_IDisplayInformation** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* d = CreateDisplayInfo();
    if (!d) { *out = nullptr; return SX_WINRT_E_OUTOFMEMORY; }
    *out = reinterpret_cast<SX_IDisplayInformation*>(d);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Dis_GetAutoRot(SX_IDisplayInformationStatics* i, uint32_t* v) { if (!v) return SX_WINRT_E_POINTER; *v = FromDis(i)->auto_rot; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Dis_PutAutoRot(SX_IDisplayInformationStatics* i, uint32_t v) { FromDis(i)->auto_rot = v; return SX_WINRT_S_OK; }
int32_t SCARLETTX_WINRT_CALL Dis_AddInv(SX_IDisplayInformationStatics*, SX_IEventHandler* h, SX_EventRegistrationToken* tok) {
    if (!h || !tok) return SX_WINRT_E_INVALIDARG;
    auto& ls = GetLifecycleState();
    std::lock_guard<std::mutex> lk(ls.mutex);
    tok->value = ls.next_token++;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Dis_RemInv(SX_IDisplayInformationStatics*, SX_EventRegistrationToken) { return SX_WINRT_S_OK; }

const SX_IActivationFactoryVtbl kDisplayInfoFactoryVtbl = {
    &Dif_QI, &Dif_AddRef, &Dif_Release,
    &Dif_GetIids, &Dif_GetClassName, &Dif_GetTrust,
    &Dif_Activate,
};
const SX_IDisplayInformationStaticsVtbl kDisplayInfoStaticsVtbl = {
    &Dis_QI, &Dis_AddRef, &Dis_Release,
    &Dis_GetIids, &Dis_GetClassName, &Dis_GetTrust,
    &Dis_GetForCurrentView, &Dis_GetAutoRot, &Dis_PutAutoRot,
    &Dis_AddInv, &Dis_RemInv,
};

// --- Built-in Class 9 & 10: GamingDeviceInformation & GamesEnumeration ---
SX_GamingDeviceModelInfo ResolveGamingDeviceModelInfo() {
    SX_GamingDeviceModelInfo info{};
    info.vendorId = 0xC2EC03D3u;  // GAMING_DEVICE_VENDOR_ID_MICROSOFT
    info.deviceId = 0x2F7A3DFFu;  // GAMING_DEVICE_DEVICE_ID_XBOX_SCARLETT_ANACONDA
#if defined(_WIN32)
    char buf[64]{};
    const uint32_t len = GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_TYPE", buf, static_cast<uint32_t>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        std::string v(buf, len);
        for (char& c : v) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (v == "lockhart" || v == "series_s" || v == "3") {
            info.deviceId = 0x1D27FABBu;  // GAMING_DEVICE_DEVICE_ID_XBOX_SCARLETT_LOCKHART
        }
    }
#endif
    return info;
}

struct SX_IGamingDeviceInformationStatics;
struct SX_IGamingDeviceInformationStaticsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IGamingDeviceInformationStatics*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IGamingDeviceInformationStatics*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IGamingDeviceInformationStatics*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IGamingDeviceInformationStatics*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IGamingDeviceInformationStatics*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IGamingDeviceInformationStatics*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_DeviceModelInformation)(SX_IGamingDeviceInformationStatics*, SX_GamingDeviceModelInfo*);
    int32_t(SCARLETTX_WINRT_CALL* get_DeviceFamily)(SX_IGamingDeviceInformationStatics*, SX_HSTRING*);
};
struct SX_IGamingDeviceInformationStatics {
    const SX_IGamingDeviceInformationStaticsVtbl* lpVtbl;
};

struct GamingDeviceFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_IGamingDeviceInformationStaticsVtbl* statics_vtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string class_name;
    bool is_game_list = false;
};

int32_t Gdi_QI_Common(GamingDeviceFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (IsRefusedComInterface(riid)) return SX_WINRT_E_NOINTERFACE;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (!self->is_game_list && GuidEquals(riid, kIidIGamingDeviceInformationStatics)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    if (self->is_game_list && GuidEquals(riid, kIidIGameListStatics)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t Gdi_AddRef_Common(GamingDeviceFactoryObj* s) { return s->ref_count.fetch_add(1, std::memory_order_relaxed) + 1; }
uint32_t Gdi_Release_Common(GamingDeviceFactoryObj* s) {
    const uint32_t rem = s->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete s;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Gdif_QI(SX_IActivationFactory* i, const SX_GUID* r, void** p) { return Gdi_QI_Common(reinterpret_cast<GamingDeviceFactoryObj*>(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Gdif_AddRef(SX_IActivationFactory* i) { return Gdi_AddRef_Common(reinterpret_cast<GamingDeviceFactoryObj*>(i)); }
uint32_t SCARLETTX_WINRT_CALL Gdif_Release(SX_IActivationFactory* i) { return Gdi_Release_Common(reinterpret_cast<GamingDeviceFactoryObj*>(i)); }
int32_t SCARLETTX_WINRT_CALL Gdif_GetIids(SX_IActivationFactory* i, uint32_t* c, SX_GUID** iids) {
    auto* self = reinterpret_cast<GamingDeviceFactoryObj*>(i);
    const SX_GUID list[] = {
        SX_IID_IActivationFactory,
        self->is_game_list ? kIidIGameListStatics : kIidIGamingDeviceInformationStatics};
    return CopyIids(list, 2, c, iids);
}
int32_t SCARLETTX_WINRT_CALL Gdif_GetClassName(SX_IActivationFactory* i, SX_HSTRING* n) {
    if (!n) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<GamingDeviceFactoryObj*>(i)->class_name, n);
}
int32_t SCARLETTX_WINRT_CALL Gdif_GetTrust(SX_IActivationFactory*, SX_TrustLevel* l) {
    if (!l) return SX_WINRT_E_POINTER;
    *l = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gdif_Activate(SX_IActivationFactory* i, SX_IInspectable** out) {
    if (!out) return SX_WINRT_E_POINTER;
    auto* obj = new (std::nothrow) GenericInspectableObj();
    if (!obj) { *out = nullptr; return SX_WINRT_E_OUTOFMEMORY; }
    obj->lpVtbl = &kGenericInspectableVtbl;
    obj->class_name = reinterpret_cast<GamingDeviceFactoryObj*>(i)->class_name;
    *out = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}
GamingDeviceFactoryObj* FromGdis(SX_IGamingDeviceInformationStatics* i) {
    return reinterpret_cast<GamingDeviceFactoryObj*>(
        reinterpret_cast<uint8_t*>(i) - offsetof(GamingDeviceFactoryObj, statics_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Gdis_QI(SX_IGamingDeviceInformationStatics* i, const SX_GUID* r, void** p) { return Gdi_QI_Common(FromGdis(i), r, p); }
uint32_t SCARLETTX_WINRT_CALL Gdis_AddRef(SX_IGamingDeviceInformationStatics* i) { return Gdi_AddRef_Common(FromGdis(i)); }
uint32_t SCARLETTX_WINRT_CALL Gdis_Release(SX_IGamingDeviceInformationStatics* i) { return Gdi_Release_Common(FromGdis(i)); }
int32_t SCARLETTX_WINRT_CALL Gdis_GetIids(SX_IGamingDeviceInformationStatics* i, uint32_t* c, SX_GUID** iids) {
    const SX_GUID list[] = {
        SX_IID_IActivationFactory,
        FromGdis(i)->is_game_list ? kIidIGameListStatics : kIidIGamingDeviceInformationStatics};
    return CopyIids(list, 2, c, iids);
}
int32_t SCARLETTX_WINRT_CALL Gdis_GetClassName(SX_IGamingDeviceInformationStatics* i, SX_HSTRING* n) {
    if (!n) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(FromGdis(i)->class_name, n);
}
int32_t SCARLETTX_WINRT_CALL Gdis_GetTrust(SX_IGamingDeviceInformationStatics*, SX_TrustLevel* l) {
    if (!l) return SX_WINRT_E_POINTER;
    *l = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gdis_GetModelInfo(SX_IGamingDeviceInformationStatics*, SX_GamingDeviceModelInfo* out) {
    if (!out) return SX_WINRT_E_POINTER;
    *out = ResolveGamingDeviceModelInfo();
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gdis_GetDeviceFamily(SX_IGamingDeviceInformationStatics*, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Xbox", value);
}

const SX_IActivationFactoryVtbl kGamingDeviceFactoryVtbl = {
    &Gdif_QI, &Gdif_AddRef, &Gdif_Release,
    &Gdif_GetIids, &Gdif_GetClassName, &Gdif_GetTrust,
    &Gdif_Activate,
};
const SX_IGamingDeviceInformationStaticsVtbl kGamingDeviceStaticsVtbl = {
    &Gdis_QI, &Gdis_AddRef, &Gdis_Release,
    &Gdis_GetIids, &Gdis_GetClassName, &Gdis_GetTrust,
    &Gdis_GetModelInfo, &Gdis_GetDeviceFamily,
};

// Built-in factory resolver
int32_t CreateBuiltInFactory(const std::u16string& class_id, const SX_GUID* iid, void** out_factory) {
    if (class_id == u"Windows.System.Profile.AnalyticsInfo") {
        auto* f = new (std::nothrow) AnalyticsInfoFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kAnalyticsFactoryVtbl;
        f->statics_vtbl = &kAnalyticsStaticsVtbl;
        int32_t hr = AnalyticsFactory_QI_Common(f, iid, out_factory);
        AnalyticsFactory_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.Storage.ApplicationData") {
        auto* f = new (std::nothrow) ApplicationDataFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kAppDataFactoryVtbl;
        f->statics_vtbl = &kAppDataStaticsVtbl;
        int32_t hr = AdFactory_QI_Common(f, iid, out_factory);
        AdFactory_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.Gaming.Input.Gamepad") {
        auto* f = new (std::nothrow) GamepadFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->lpVtbl = &kGamepadFactoryVtbl;
        int32_t hr = Gpf_QueryInterface(reinterpret_cast<SX_IActivationFactory*>(f), iid, out_factory);
        Gpf_Release(reinterpret_cast<SX_IActivationFactory*>(f));
        return hr;
    }
    if (class_id == u"Windows.Globalization.Language") {
        auto* f = new (std::nothrow) LanguageFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kLanguageFactoryVtbl;
        f->lang_factory_vtbl = &kLanguageCustomFactoryVtbl;
        int32_t hr = LangFactory_QI_Common(f, iid, out_factory);
        LangFactory_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.ApplicationModel.Core.CoreApplication") {
        auto* f = new (std::nothrow) CoreApplicationFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kCoreAppFactoryVtbl;
        f->app_vtbl = &kCoreApp1Vtbl;
        f->app2_vtbl = &kCoreApp2Vtbl;
        f->exit_vtbl = &kCoreAppExitVtbl;
        f->use_count_vtbl = &kCoreAppUseCountVtbl;
        int32_t hr = CoreApp_QI_Common(f, iid, out_factory);
        CoreApp_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.UI.Core.CoreWindow") {
        auto* f = new (std::nothrow) CoreWindowFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kCoreWindowFactoryVtbl;
        f->statics_vtbl = &kCoreWindowStaticsVtbl;
        int32_t hr = CwFactory_QI_Common(f, iid, out_factory);
        CwFactory_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.UI.ViewManagement.ApplicationView") {
        auto* f = new (std::nothrow) ApplicationViewFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kAppViewFactoryVtbl;
        f->statics2_vtbl = &kAppViewStatics2Vtbl;
        int32_t hr = Avf_QI_Common(f, iid, out_factory);
        Avf_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.Graphics.Display.DisplayInformation") {
        auto* f = new (std::nothrow) DisplayInfoFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kDisplayInfoFactoryVtbl;
        f->statics_vtbl = &kDisplayInfoStaticsVtbl;
        int32_t hr = Dif_QI_Common(f, iid, out_factory);
        Dif_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.System.Profile.GamingDeviceInformation" ||
        class_id == u"Windows.Gaming.Preview.GamesEnumeration.GameList" ||
        class_id == u"Windows.Gaming.Preview.GamesEnumeration") {
        auto* f = new (std::nothrow) GamingDeviceFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kGamingDeviceFactoryVtbl;
        f->statics_vtbl = &kGamingDeviceStaticsVtbl;
        f->class_name = class_id;
        f->is_game_list = (class_id != u"Windows.System.Profile.GamingDeviceInformation");
        int32_t hr = Gdi_QI_Common(f, iid, out_factory);
        Gdi_Release_Common(f);
        return hr;
    }
    return SX_WINRT_REGDB_E_CLASSNOTREG;
}

}  // namespace

extern "C" {

// ===========================================================================
// WinRT Activation Runtime API (Ro*)
// ===========================================================================
int32_t RoInitialize(uint32_t initType) {
    if (initType != SX_RO_INIT_SINGLETHREADED && initType != SX_RO_INIT_MULTITHREADED) {
        return RecordWinRtHr("RoInitialize", SX_WINRT_E_INVALIDARG);
    }
    if (g_tls_ro_state.init_count == 0) {
        g_tls_ro_state.init_count = 1;
        g_tls_ro_state.init_type = initType;
        return RecordWinRtHr("RoInitialize", SX_WINRT_S_OK);
    }
    if (g_tls_ro_state.init_type != initType) {
        return RecordWinRtHr("RoInitialize", SX_WINRT_RPC_E_CHANGED_MODE);
    }
    ++g_tls_ro_state.init_count;
    return RecordWinRtHr("RoInitialize", SX_WINRT_S_FALSE);
}

void RoUninitialize(void) {
    RecordWinRtOk("RoUninitialize");
    if (g_tls_ro_state.init_count > 0) {
        --g_tls_ro_state.init_count;
    }
}

int32_t RoGetActivationFactory(
    SX_HSTRING activatableClassId,
    const SX_GUID* iid,
    void** factory) {
    if (!factory) {
        return RecordWinRtHr("RoGetActivationFactory", SX_WINRT_E_POINTER);
    }
    *factory = nullptr;
    if (g_tls_ro_state.init_count == 0) {
        return RecordWinRtHr("RoGetActivationFactory", SX_WINRT_CO_E_NOTINITIALIZED);
    }
    if (!iid || WindowsIsStringEmpty(activatableClassId)) {
        return RecordWinRtHr("RoGetActivationFactory", SX_WINRT_E_INVALIDARG);
    }

    const std::u16string class_id = HStringToU16(activatableClassId);

    // 1. Check custom runtime-registered activation factories (LIFO precedence).
    SX_PFNGETACTIVATIONFACTORY custom_cb = nullptr;
    {
        WinRtRegistry& reg = GetRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        for (auto it = reg.custom_factories.rbegin(); it != reg.custom_factories.rend(); ++it) {
            if (it->class_id == class_id) {
                custom_cb = it->callback;
                break;
            }
        }
    }

    if (custom_cb) {
        SX_IActivationFactory* raw_factory = nullptr;
        const int32_t hr = custom_cb(activatableClassId, &raw_factory);
        if (hr < 0) {
            return RecordWinRtHr("RoGetActivationFactory", hr);
        }
        if (!raw_factory) {
            return RecordWinRtHr("RoGetActivationFactory", SX_WINRT_E_FAIL);
        }
        const int32_t qi_hr = raw_factory->lpVtbl->QueryInterface(raw_factory, iid, factory);
        raw_factory->lpVtbl->Release(raw_factory);
        return RecordWinRtHr("RoGetActivationFactory", qi_hr);
    }

    // 2. Check built-in Xbox/UWP classes.
    return RecordWinRtHr("RoGetActivationFactory", CreateBuiltInFactory(class_id, iid, factory));
}

int32_t RoActivateInstance(SX_HSTRING activatableClassId, void** instance) {
    if (!instance) {
        return RecordWinRtHr("RoActivateInstance", SX_WINRT_E_POINTER);
    }
    *instance = nullptr;
    if (g_tls_ro_state.init_count == 0) {
        return RecordWinRtHr("RoActivateInstance", SX_WINRT_CO_E_NOTINITIALIZED);
    }
    if (WindowsIsStringEmpty(activatableClassId)) {
        return RecordWinRtHr("RoActivateInstance", SX_WINRT_E_INVALIDARG);
    }

    SX_IActivationFactory* factory = nullptr;
    int32_t hr = RoGetActivationFactory(
        activatableClassId, &SX_IID_IActivationFactory, reinterpret_cast<void**>(&factory));
    if (hr < 0 || !factory) {
        return RecordWinRtHr("RoActivateInstance", hr < 0 ? hr : SX_WINRT_E_FAIL);
    }

    SX_IInspectable* insp = nullptr;
    hr = factory->lpVtbl->ActivateInstance(factory, &insp);
    factory->lpVtbl->Release(factory);
    if (hr < 0) {
        return RecordWinRtHr("RoActivateInstance", hr);
    }
    *instance = insp;
    return RecordWinRtHr("RoActivateInstance", SX_WINRT_S_OK);
}

int32_t RoRegisterActivationFactories(
    SX_HSTRING* activatableClassIds,
    SX_PFNGETACTIVATIONFACTORY* activationFactoryCallbacks,
    uint32_t count,
    uint64_t* cookie) {
    if (!cookie) {
        return RecordWinRtHr("RoRegisterActivationFactories", SX_WINRT_E_POINTER);
    }
    *cookie = 0;
    if (g_tls_ro_state.init_count == 0) {
        return RecordWinRtHr("RoRegisterActivationFactories", SX_WINRT_CO_E_NOTINITIALIZED);
    }
    if (!activatableClassIds || !activationFactoryCallbacks || count == 0) {
        return RecordWinRtHr("RoRegisterActivationFactories", SX_WINRT_E_INVALIDARG);
    }

    std::vector<std::pair<std::u16string, SX_PFNGETACTIVATIONFACTORY>> validated;
    validated.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (WindowsIsStringEmpty(activatableClassIds[i]) || !activationFactoryCallbacks[i]) {
            return RecordWinRtHr("RoRegisterActivationFactories", SX_WINRT_E_INVALIDARG);
        }
        validated.emplace_back(HStringToU16(activatableClassIds[i]), activationFactoryCallbacks[i]);
    }

    WinRtRegistry& reg = GetRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    const uint64_t assigned = reg.next_cookie++;
    for (auto& [cid, cb] : validated) {
        reg.custom_factories.push_back(RegisteredFactoryEntry{assigned, std::move(cid), cb});
    }
    *cookie = assigned;
    return RecordWinRtHr("RoRegisterActivationFactories", SX_WINRT_S_OK);
}

void RoRevokeActivationFactories(uint64_t cookie) {
    RecordWinRtOk("RoRevokeActivationFactories");
    if (cookie == 0) {
        return;
    }
    WinRtRegistry& reg = GetRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    reg.custom_factories.erase(
        std::remove_if(reg.custom_factories.begin(), reg.custom_factories.end(),
                       [cookie](const RegisteredFactoryEntry& e) { return e.cookie == cookie; }),
        reg.custom_factories.end());
}

// ===========================================================================
// WinRT HSTRING API (Windows*)
// ===========================================================================
int32_t WindowsCreateString(
    const uint16_t* sourceString,
    uint32_t length,
    SX_HSTRING* string) {
    if (!string) {
        return RecordWinRtHr("WindowsCreateString", SX_WINRT_E_INVALIDARG);
    }
    *string = nullptr;
    if (length == 0) {
        // Per WinRT specification, empty string is represented by NULL HSTRING.
        return RecordWinRtHr("WindowsCreateString", SX_WINRT_S_OK);
    }
    if (!sourceString) {
        return RecordWinRtHr("WindowsCreateString", SX_WINRT_E_POINTER);
    }

    const size_t chars_bytes = (static_cast<size_t>(length) + 1) * sizeof(uint16_t);
    const size_t total_bytes = sizeof(SxHStringImpl) + chars_bytes;
    void* mem = ::operator new(total_bytes, std::nothrow);
    if (!mem) {
        return RecordWinRtHr("WindowsCreateString", SX_WINRT_E_OUTOFMEMORY);
    }
    auto* buf = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(mem) + sizeof(SxHStringImpl));
    std::memcpy(buf, sourceString, static_cast<size_t>(length) * sizeof(uint16_t));
    buf[length] = 0;

    auto* impl = new (mem) SxHStringImpl{
        kHStringMagic,
        length,
        1,
        0,
        buf,
    };
    *string = reinterpret_cast<SX_HSTRING>(impl);
    return RecordWinRtHr("WindowsCreateString", SX_WINRT_S_OK);
}

int32_t WindowsCreateStringReference(
    const uint16_t* sourceString,
    uint32_t length,
    SX_HSTRING_HEADER* hstringHeader,
    SX_HSTRING* string) {
    if (!hstringHeader || !string) {
        return RecordWinRtHr("WindowsCreateStringReference", SX_WINRT_E_INVALIDARG);
    }
    *string = nullptr;
    if (!sourceString) {
        if (length != 0) {
            return RecordWinRtHr("WindowsCreateStringReference", SX_WINRT_E_POINTER);
        }
        return RecordWinRtHr("WindowsCreateStringReference", SX_WINRT_S_OK);
    }
    // Fast-pass HSTRING requires null-termination at sourceString[length].
    if (sourceString[length] != 0) {
        return RecordWinRtHr("WindowsCreateStringReference", SX_WINRT_E_INVALIDARG);
    }
    if (length == 0) {
        return RecordWinRtHr("WindowsCreateStringReference", SX_WINRT_S_OK);
    }

    auto* impl = new (hstringHeader->Reserved.Reserved2) SxHStringImpl{
        kHStringMagic | kHStringFlagRef,
        length,
        0,
        0,
        sourceString,
    };
    *string = reinterpret_cast<SX_HSTRING>(impl);
    return RecordWinRtHr("WindowsCreateStringReference", SX_WINRT_S_OK);
}

int32_t WindowsDeleteString(SX_HSTRING string) {
    if (!string) {
        return RecordWinRtHr("WindowsDeleteString", SX_WINRT_S_OK);
    }
    SxHStringImpl* impl = AsImplMut(string);
    if ((impl->flags & kHStringMagicMask) != kHStringMagic) {
        return RecordWinRtHr("WindowsDeleteString", SX_WINRT_E_INVALIDARG);
    }
    if ((impl->flags & kHStringFlagRef) != 0) {
        // Per WinRT spec, deleting a fast-pass string reference is a safe no-op.
        return RecordWinRtHr("WindowsDeleteString", SX_WINRT_S_OK);
    }
    if (impl->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        impl->~SxHStringImpl();
        ::operator delete(impl);
    }
    return RecordWinRtHr("WindowsDeleteString", SX_WINRT_S_OK);
}

const uint16_t* WindowsGetStringRawBuffer(SX_HSTRING string, uint32_t* length) {
    RecordWinRtOk("WindowsGetStringRawBuffer");
    if (!string) {
        if (length) *length = 0;
        return kEmptyUtf16;
    }
    const SxHStringImpl* impl = AsImpl(string);
    if (length) *length = impl->length;
    return impl->buffer ? impl->buffer : kEmptyUtf16;
}

uint32_t WindowsGetStringLen(SX_HSTRING string) {
    RecordWinRtOk("WindowsGetStringLen");
    if (!string) {
        return 0;
    }
    return AsImpl(string)->length;
}

int32_t WindowsDuplicateString(SX_HSTRING string, SX_HSTRING* newString) {
    if (!newString) {
        return RecordWinRtHr("WindowsDuplicateString", SX_WINRT_E_INVALIDARG);
    }
    *newString = nullptr;
    if (!string) {
        return RecordWinRtHr("WindowsDuplicateString", SX_WINRT_S_OK);
    }
    SxHStringImpl* impl = AsImplMut(string);
    if ((impl->flags & kHStringMagicMask) != kHStringMagic) {
        return RecordWinRtHr("WindowsDuplicateString", SX_WINRT_E_INVALIDARG);
    }
    if ((impl->flags & kHStringFlagRef) != 0) {
        // Fast-pass references must be promoted to heap-allocated copies on duplicate.
        return RecordWinRtHr("WindowsDuplicateString",
                             WindowsCreateString(impl->buffer, impl->length, newString));
    }
    impl->ref_count.fetch_add(1, std::memory_order_relaxed);
    *newString = string;
    return RecordWinRtHr("WindowsDuplicateString", SX_WINRT_S_OK);
}

int32_t WindowsCompareStringOrdinal(
    SX_HSTRING string1,
    SX_HSTRING string2,
    int32_t* result) {
    if (!result) {
        return RecordWinRtHr("WindowsCompareStringOrdinal", SX_WINRT_E_INVALIDARG);
    }
    uint32_t len1 = 0;
    uint32_t len2 = 0;
    const uint16_t* buf1 = WindowsGetStringRawBuffer(string1, &len1);
    const uint16_t* buf2 = WindowsGetStringRawBuffer(string2, &len2);

    const uint32_t common = std::min(len1, len2);
    for (uint32_t i = 0; i < common; ++i) {
        if (buf1[i] < buf2[i]) {
            *result = -1;
            return RecordWinRtHr("WindowsCompareStringOrdinal", SX_WINRT_S_OK);
        }
        if (buf1[i] > buf2[i]) {
            *result = 1;
            return RecordWinRtHr("WindowsCompareStringOrdinal", SX_WINRT_S_OK);
        }
    }
    if (len1 < len2) {
        *result = -1;
    } else if (len1 > len2) {
        *result = 1;
    } else {
        *result = 0;
    }
    return RecordWinRtHr("WindowsCompareStringOrdinal", SX_WINRT_S_OK);
}

int32_t WindowsIsStringEmpty(SX_HSTRING string) {
    RecordWinRtOk("WindowsIsStringEmpty");
    return (string == nullptr || AsImpl(string)->length == 0) ? 1 : 0;
}

int32_t WindowsStringHasEmbeddedNull(SX_HSTRING string, int32_t* hasEmbedNull) {
    if (!hasEmbedNull) {
        return RecordWinRtHr("WindowsStringHasEmbeddedNull", SX_WINRT_E_INVALIDARG);
    }
    *hasEmbedNull = 0;
    if (!string) {
        return RecordWinRtHr("WindowsStringHasEmbeddedNull", SX_WINRT_S_OK);
    }
    uint32_t len = 0;
    const uint16_t* buf = WindowsGetStringRawBuffer(string, &len);
    for (uint32_t i = 0; i < len; ++i) {
        if (buf[i] == 0) {
            *hasEmbedNull = 1;
            break;
        }
    }
    return RecordWinRtHr("WindowsStringHasEmbeddedNull", SX_WINRT_S_OK);
}

int32_t WindowsConcatString(
    SX_HSTRING string1,
    SX_HSTRING string2,
    SX_HSTRING* newString) {
    if (!newString) {
        return RecordWinRtHr("WindowsConcatString", SX_WINRT_E_INVALIDARG);
    }
    *newString = nullptr;
    uint32_t len1 = 0;
    uint32_t len2 = 0;
    const uint16_t* buf1 = WindowsGetStringRawBuffer(string1, &len1);
    const uint16_t* buf2 = WindowsGetStringRawBuffer(string2, &len2);
    if (len1 == 0 && len2 == 0) {
        return RecordWinRtHr("WindowsConcatString", SX_WINRT_S_OK);
    }
    std::vector<uint16_t> combined(static_cast<size_t>(len1) + len2);
    if (len1 > 0) std::memcpy(combined.data(), buf1, static_cast<size_t>(len1) * sizeof(uint16_t));
    if (len2 > 0) std::memcpy(combined.data() + len1, buf2, static_cast<size_t>(len2) * sizeof(uint16_t));
    return RecordWinRtHr("WindowsConcatString",
                         WindowsCreateString(combined.data(), static_cast<uint32_t>(combined.size()), newString));
}

int32_t WindowsSubstring(
    SX_HSTRING string,
    uint32_t startIndex,
    SX_HSTRING* newString) {
    if (!newString) {
        return RecordWinRtHr("WindowsSubstring", SX_WINRT_E_INVALIDARG);
    }
    *newString = nullptr;
    uint32_t len = 0;
    const uint16_t* buf = WindowsGetStringRawBuffer(string, &len);
    if (startIndex > len) {
        return RecordWinRtHr("WindowsSubstring", SX_WINRT_E_BOUNDS);
    }
    return RecordWinRtHr("WindowsSubstring",
                         WindowsCreateString(buf + startIndex, len - startIndex, newString));
}

int32_t WindowsSubstringWithSpecifiedLength(
    SX_HSTRING string,
    uint32_t startIndex,
    uint32_t length,
    SX_HSTRING* newString) {
    if (!newString) {
        return RecordWinRtHr("WindowsSubstringWithSpecifiedLength", SX_WINRT_E_INVALIDARG);
    }
    *newString = nullptr;
    uint32_t total = 0;
    const uint16_t* buf = WindowsGetStringRawBuffer(string, &total);
    if (startIndex > total || length > total - startIndex) {
        return RecordWinRtHr("WindowsSubstringWithSpecifiedLength", SX_WINRT_E_BOUNDS);
    }
    return RecordWinRtHr("WindowsSubstringWithSpecifiedLength",
                         WindowsCreateString(buf + startIndex, length, newString));
}

// ===========================================================================
// OpenScarlett Lifecycle Event Delivery & GamingDeviceModelInformation Exports
// ===========================================================================
// eventKind:
//   0 = Suspending
//   1 = Resuming
//   2 = EnteredBackground
//   3 = LeavingBackground
//   4 = ConstrainedModeEntered (sets constrained=true, visible=false, notifies EnteredBackground + VisibilityChanged)
//   5 = ConstrainedModeExited  (sets constrained=false, visible=true, notifies LeavingBackground + Resuming + VisibilityChanged)
OPENSCARLETT_WINRT_API int32_t OpenScarlettTriggerWinRtLifecycleEvent(uint32_t eventKind) {
    auto& ls = GetLifecycleState();
    int32_t total = 0;
    switch (eventKind) {
        case 0u: {  // Suspending
            auto* args = new (std::nothrow) SuspendingArgsObj();
            if (!args) return RecordWinRtHr("OpenScarlettTriggerWinRtLifecycleEvent", SX_WINRT_E_OUTOFMEMORY);
            args->lpVtbl = &kSuspendingArgsVtbl;
            total = DispatchEventList(ls.suspending_handlers, nullptr, args);
            Sargs_Release(reinterpret_cast<SX_ISuspendingEventArgs*>(args));
            RecordWinRtOk("OpenScarlettTriggerWinRtLifecycleEvent");
            return total;
        }
        case 1u: {  // Resuming
            total = DispatchEventList(ls.resuming_handlers, nullptr, nullptr);
            RecordWinRtOk("OpenScarlettTriggerWinRtLifecycleEvent");
            return total;
        }
        case 2u: {  // EnteredBackground
            auto* args = new (std::nothrow) BackgroundArgsObj();
            if (!args) return RecordWinRtHr("OpenScarlettTriggerWinRtLifecycleEvent", SX_WINRT_E_OUTOFMEMORY);
            args->lpVtbl = &kBgArgsVtbl;
            args->is_entering = true;
            total = DispatchEventList(ls.entered_bg_handlers, nullptr, args);
            BgArgs_Release(reinterpret_cast<SX_IBackgroundEventArgs*>(args));
            RecordWinRtOk("OpenScarlettTriggerWinRtLifecycleEvent");
            return total;
        }
        case 3u: {  // LeavingBackground
            auto* args = new (std::nothrow) BackgroundArgsObj();
            if (!args) return RecordWinRtHr("OpenScarlettTriggerWinRtLifecycleEvent", SX_WINRT_E_OUTOFMEMORY);
            args->lpVtbl = &kBgArgsVtbl;
            args->is_entering = false;
            total = DispatchEventList(ls.leaving_bg_handlers, nullptr, args);
            BgArgs_Release(reinterpret_cast<SX_IBackgroundEventArgs*>(args));
            RecordWinRtOk("OpenScarlettTriggerWinRtLifecycleEvent");
            return total;
        }
        case 4u: {  // ConstrainedModeEntered
            ls.constrained_mode.store(true, std::memory_order_release);
            ls.window_visible.store(0u, std::memory_order_release);
            auto* args = new (std::nothrow) BackgroundArgsObj();
            if (args) {
                args->lpVtbl = &kBgArgsVtbl;
                args->is_entering = true;
                total += DispatchEventList(ls.entered_bg_handlers, nullptr, args);
                BgArgs_Release(reinterpret_cast<SX_IBackgroundEventArgs*>(args));
            }
            total += DispatchEventList(ls.window_visibility_handlers, nullptr, nullptr);
            RecordWinRtOk("OpenScarlettTriggerWinRtLifecycleEvent");
            return total;
        }
        case 5u: {  // ConstrainedModeExited
            ls.constrained_mode.store(false, std::memory_order_release);
            ls.window_visible.store(1u, std::memory_order_release);
            auto* args = new (std::nothrow) BackgroundArgsObj();
            if (args) {
                args->lpVtbl = &kBgArgsVtbl;
                args->is_entering = false;
                total += DispatchEventList(ls.leaving_bg_handlers, nullptr, args);
                BgArgs_Release(reinterpret_cast<SX_IBackgroundEventArgs*>(args));
            }
            total += DispatchEventList(ls.resuming_handlers, nullptr, nullptr);
            total += DispatchEventList(ls.window_visibility_handlers, nullptr, nullptr);
            RecordWinRtOk("OpenScarlettTriggerWinRtLifecycleEvent");
            return total;
        }
        default:
            return RecordWinRtHr("OpenScarlettTriggerWinRtLifecycleEvent", SX_WINRT_E_INVALIDARG);
    }
}

OPENSCARLETT_WINRT_API int32_t OpenScarlettGetWinRtConstrainedMode(void) {
    RecordWinRtOk("OpenScarlettGetWinRtConstrainedMode");
    return GetLifecycleState().constrained_mode.load(std::memory_order_acquire) ? 1 : 0;
}

OPENSCARLETT_WINRT_API int32_t SCARLETTX_WINRT_CALL GetGamingDeviceModelInformation(
    void* information) {
    if (!information) {
        return RecordWinRtHr("GetGamingDeviceModelInformation", SX_WINRT_E_INVALIDARG);
    }
    const SX_GamingDeviceModelInfo info = ResolveGamingDeviceModelInfo();
    std::memcpy(information, &info, sizeof(info));
    return RecordWinRtHr("GetGamingDeviceModelInformation", SX_WINRT_S_OK);
}

}  // extern "C"
