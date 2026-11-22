#ifndef OPENSCARLETT_BUILDING_D3D12_XS
#define OPENSCARLETT_BUILDING_D3D12_XS 1
#endif
#include "openscarlett/gpu/d3d12_xs_abi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>

#include "gpu/d3d12_xs/command_list_x.h"
#include "gpu/d3d12_xs/device_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"

// DirectX 12 Agility SDK magic exports (public Microsoft D3D12 Agility SDK specification).
// Exported from d3d12_xs.dll so titles / D3D12 loader detect Agility SDK version & path.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 614u;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";

// Standard D3D12 COM interface IID data symbols imported statically by Xbox Series X
// GDKX titles (e.g., Minecraft Preview & UE5 The Matrix Awakens) from d3d12_xs.dll.
// Defined under OS_IID_* internal names and aliased to IID_ID3D12* DATA exports via
// linker directives (and d3d12_xs_mingw.def) to avoid collisions with <d3d12.h> / uuid.lib.
extern const GUID OS_IID_ID3D12Device = {
    0x189819f1u, 0x1db6u, 0x4b57u, {0xbeu, 0x54u, 0x18u, 0x21u, 0x33u, 0x9bu, 0x85u, 0xf7u}};
extern const GUID OS_IID_ID3D12Device1 = {
    0x77acce80u, 0x638eu, 0x4e65u, {0x88u, 0x95u, 0xc1u, 0xf2u, 0x33u, 0x86u, 0x86u, 0x3eu}};
extern const GUID OS_IID_ID3D12Device2 = {
    0x30baa41eu, 0xb15bu, 0x475cu, {0xa0u, 0xbbu, 0x1au, 0xf5u, 0xc5u, 0xb6u, 0x43u, 0x28u}};
extern const GUID OS_IID_ID3D12Device3 = {
    0x81dadc15u, 0x2badu, 0x4392u, {0x93u, 0xc5u, 0x10u, 0x13u, 0x45u, 0xc4u, 0xaau, 0x98u}};
extern const GUID OS_IID_ID3D12Device4 = {
    0xe865df17u, 0xa9eeu, 0x46f9u, {0xa4u, 0x63u, 0x30u, 0x98u, 0x31u, 0x5au, 0xa2u, 0xe5u}};
extern const GUID OS_IID_ID3D12Device5 = {
    0x8b4f173bu, 0x2feau, 0x4b80u, {0x8fu, 0x58u, 0x43u, 0x07u, 0x19u, 0x1au, 0xb9u, 0x5du}};
extern const GUID OS_IID_ID3D12Device6 = {
    0xc70b221bu, 0x40e4u, 0x4a17u, {0x89u, 0xafu, 0x02u, 0x5au, 0x07u, 0x27u, 0xa6u, 0xdcu}};
extern const GUID OS_IID_ID3D12Device7 = {
    0x5c014b53u, 0x68a1u, 0x4b9bu, {0x8bu, 0xd1u, 0xddu, 0x60u, 0x46u, 0xb9u, 0x35u, 0x8bu}};
extern const GUID OS_IID_ID3D12Device8 = {
    0x9218e6bbu, 0xf944u, 0x4f7eu, {0xa7u, 0x5cu, 0xb1u, 0xb2u, 0xc7u, 0xb7u, 0x01u, 0xf3u}};
extern const GUID OS_IID_ID3D12CommandQueue = {
    0x0ec870a6u, 0x5d7eu, 0x4c22u, {0x8cu, 0xfcu, 0x5bu, 0xaau, 0xe0u, 0x76u, 0x16u, 0xedu}};
extern const GUID OS_IID_ID3D12CommandAllocator = {
    0x6102dee4u, 0xaf59u, 0x4b09u, {0xb9u, 0x99u, 0xb4u, 0x4du, 0x73u, 0xf0u, 0x9bu, 0x24u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList = {
    0x5b160d0fu, 0xac1bu, 0x4185u, {0x8bu, 0xa8u, 0xb3u, 0xaeu, 0x42u, 0xa5u, 0xa4u, 0x55u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList1 = {
    0x553103fbu, 0x1fe7u, 0x4557u, {0xbbu, 0x38u, 0x94u, 0x6du, 0x7du, 0x0eu, 0x7cu, 0xa7u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList2 = {
    0x38c3e585u, 0xff17u, 0x412cu, {0x91u, 0x50u, 0x4fu, 0xc6u, 0xf9u, 0xd7u, 0x2au, 0x28u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList3 = {
    0x6fda83a7u, 0xb84cu, 0x4e38u, {0x9au, 0xc8u, 0xc7u, 0xbdu, 0x22u, 0x01u, 0x6bu, 0x3du}};
extern const GUID OS_IID_ID3D12GraphicsCommandList4 = {
    0x8754318eu, 0xd3a9u, 0x4541u, {0x98u, 0xcfu, 0x64u, 0x5bu, 0x50u, 0xdcu, 0x48u, 0x74u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList5 = {
    0x55050859u, 0x4024u, 0x474cu, {0x87u, 0xf5u, 0x64u, 0x72u, 0xeau, 0xeeu, 0x44u, 0xeau}};
extern const GUID OS_IID_ID3D12GraphicsCommandList6 = {
    0xc3827890u, 0xe548u, 0x4cfau, {0x96u, 0xcfu, 0x56u, 0x89u, 0xa9u, 0x37u, 0x0fu, 0x80u}};
extern const GUID OS_IID_ID3D12Resource = {
    0x696442beu, 0xa72eu, 0x4059u, {0xbcu, 0x79u, 0x5bu, 0x5cu, 0x98u, 0x04u, 0x0fu, 0xadu}};
extern const GUID OS_IID_ID3D12Resource1 = {
    0x9d5e227au, 0x4430u, 0x4161u, {0x88u, 0xb3u, 0x3eu, 0xcau, 0x6bu, 0xb1u, 0x6eu, 0x19u}};
extern const GUID OS_IID_ID3D12Resource2 = {
    0xbe36ec3bu, 0xea85u, 0x4aebu, {0xa4u, 0x5au, 0xe9u, 0xd7u, 0x64u, 0x04u, 0xa4u, 0x95u}};
extern const GUID OS_IID_ID3D12Heap = {
    0x6b3b2502u, 0x6e51u, 0x45b3u, {0x90u, 0xeeu, 0x98u, 0x84u, 0x26u, 0x5eu, 0x8du, 0xf3u}};
extern const GUID OS_IID_ID3D12Heap1 = {
    0x572f7389u, 0x2168u, 0x49e3u, {0x96u, 0x93u, 0xd6u, 0xdfu, 0x58u, 0x71u, 0xbfu, 0x6du}};
extern const GUID OS_IID_ID3D12Fence = {
    0x0a753dcfu, 0xc4d8u, 0x4b91u, {0xadu, 0xf6u, 0xbeu, 0x5au, 0x60u, 0xd9u, 0x5au, 0x76u}};
extern const GUID OS_IID_ID3D12Fence1 = {
    0x433685feu, 0xe22bu, 0x4ca0u, {0xa8u, 0xdbu, 0xb5u, 0xb4u, 0xf4u, 0xddu, 0x0eu, 0x4au}};
extern const GUID OS_IID_ID3D12PipelineState = {
    0x765a30f3u, 0xf624u, 0x4c6fu, {0xa8u, 0x28u, 0xacu, 0xe9u, 0x48u, 0x62u, 0x24u, 0x45u}};
extern const GUID OS_IID_ID3D12RootSignature = {
    0xc54a6b66u, 0x72dfu, 0x4ee8u, {0x8bu, 0xe5u, 0xa9u, 0x46u, 0xa1u, 0x42u, 0x92u, 0x14u}};
extern const GUID OS_IID_ID3D12DescriptorHeap = {
    0x8efb471du, 0x616cu, 0x4f49u, {0x90u, 0xf7u, 0x12u, 0x7bu, 0xb7u, 0x63u, 0xfau, 0x51u}};
extern const GUID OS_IID_ID3D12QueryHeap = {
    0x0d9658aeu, 0xed45u, 0x469eu, {0xa6u, 0x1du, 0x97u, 0x0eu, 0xc5u, 0x83u, 0xcau, 0xb4u}};
extern const GUID OS_IID_ID3D12CommandSignature = {
    0xc36a797cu, 0xec80u, 0x4f0au, {0x89u, 0x85u, 0xa7u, 0xb2u, 0x47u, 0x50u, 0x82u, 0xd1u}};
extern const GUID OS_IID_ID3D12StateObject = {
    0x47016943u, 0xfca8u, 0x4594u, {0x93u, 0xeau, 0xafu, 0x25u, 0x8bu, 0x55u, 0x34u, 0x6du}};
}

#if defined(_MSC_VER)
#pragma comment(linker, "/EXPORT:D3D12CreateDevice=OS_D3D12CreateDevice")
#pragma comment(linker, "/EXPORT:D3D12SerializeRootSignature=OS_D3D12SerializeRootSignature")
#pragma comment(linker, "/EXPORT:D3D12SerializeVersionedRootSignature=OS_D3D12SerializeVersionedRootSignature")
#pragma comment(linker, "/EXPORT:D3D12CreateRootSignatureDeserializer=OS_D3D12CreateRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12CreateVersionedRootSignatureDeserializer=OS_D3D12CreateVersionedRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12GetDebugInterface=OS_D3D12GetDebugInterface")
#pragma comment(linker, "/EXPORT:D3D12EnableExperimentalFeatures=OS_D3D12EnableExperimentalFeatures")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device=OS_IID_ID3D12Device,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device1=OS_IID_ID3D12Device1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device2=OS_IID_ID3D12Device2,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device3=OS_IID_ID3D12Device3,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device4=OS_IID_ID3D12Device4,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device5=OS_IID_ID3D12Device5,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device6=OS_IID_ID3D12Device6,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device7=OS_IID_ID3D12Device7,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device8=OS_IID_ID3D12Device8,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12CommandQueue=OS_IID_ID3D12CommandQueue,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12CommandAllocator=OS_IID_ID3D12CommandAllocator,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList=OS_IID_ID3D12GraphicsCommandList,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList1=OS_IID_ID3D12GraphicsCommandList1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList2=OS_IID_ID3D12GraphicsCommandList2,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList3=OS_IID_ID3D12GraphicsCommandList3,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList4=OS_IID_ID3D12GraphicsCommandList4,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList5=OS_IID_ID3D12GraphicsCommandList5,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList6=OS_IID_ID3D12GraphicsCommandList6,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Resource=OS_IID_ID3D12Resource,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Resource1=OS_IID_ID3D12Resource1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Resource2=OS_IID_ID3D12Resource2,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Heap=OS_IID_ID3D12Heap,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Heap1=OS_IID_ID3D12Heap1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Fence=OS_IID_ID3D12Fence,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Fence1=OS_IID_ID3D12Fence1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12PipelineState=OS_IID_ID3D12PipelineState,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12RootSignature=OS_IID_ID3D12RootSignature,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12DescriptorHeap=OS_IID_ID3D12DescriptorHeap,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12QueryHeap=OS_IID_ID3D12QueryHeap,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12CommandSignature=OS_IID_ID3D12CommandSignature,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12StateObject=OS_IID_ID3D12StateObject,DATA")
#endif

namespace {

constexpr const char* kModule = "d3d12_xs.dll";
std::atomic<int> g_prefer_warp{0};

inline void RecordExport(const char* sym, HRESULT hr) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (FAILED(hr)) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        sym,
        outcome,
        static_cast<int32_t>(hr),
        -1);
}

constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = (2523.0 / 4096.0) * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = (2413.0 / 4096.0) * 32.0;
constexpr double kPqC3 = (2392.0 / 4096.0) * 32.0;
constexpr double kPqMaxNits = 10000.0;

inline float SanitizeNonNegative(float v) noexcept {
    if (!std::isfinite(v) || v <= 0.0f) return 0.0f;
    return v;
}

inline float ToneMapAcesChannel(float x) noexcept {
    x = SanitizeNonNegative(x);
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    const float num = x * (a * x + b);
    const float den = x * (c * x + d) + e;
    if (den <= 0.0f) return 0.0f;
    return std::clamp(num / den, 0.0f, 1.0f);
}

inline float ToneMapReinhardChannel(float x) noexcept {
    x = SanitizeNonNegative(x);
    return std::clamp(x / (1.0f + x), 0.0f, 1.0f);
}

inline float LinearToSrgbChannel(float x) noexcept {
    x = std::clamp(SanitizeNonNegative(x), 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

inline uint8_t FloatToU8(float v) noexcept {
    if (!std::isfinite(v)) return 0u;
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

using PfnD3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnD3D12SerializeRootSignature = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using PfnD3D12SerializeVersionedRootSignature = HRESULT(WINAPI*)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using PfnD3D12CreateRootSignatureDeserializer = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, REFIID, void**);
using PfnD3D12CreateVersionedRootSignatureDeserializer = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, REFIID, void**);
using PfnD3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
using PfnD3D12EnableExperimentalFeatures = HRESULT(WINAPI*)(
    UINT, const IID*, void*, UINT*);
using PfnD3D12GetInterface = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);

// Local clean-room definitions of public Microsoft Agility SDK CLSID/IID/COM interface so
// MSVC, Clang-CL, AND MinGW-w64 compile cleanly regardless of installed SDK header version.
constexpr GUID kClsidD3D12SDKConfiguration = {
    0x7cda6acau, 0xa03eu, 0x49c8u, {0x94u, 0x58u, 0x03u, 0x34u, 0xd2u, 0x0eu, 0x07u, 0xceu}};
constexpr GUID kIidID3D12SDKConfiguration = {
    0xe9eb5314u, 0x33aau, 0x42b2u, {0xa7u, 0x18u, 0xd7u, 0x7fu, 0x58u, 0xb1u, 0xf1u, 0xc7u}};

struct OS_ID3D12SDKConfiguration : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT SDKVersion, LPCSTR SDKPath) = 0;
};

struct AgilitySdkRuntimeState {
    std::once_flag init_once;
    std::atomic<bool> configured{false};
    std::atomic<bool> core_dll_found{false};
    std::atomic<bool> using_inbox_fallback{true};
    std::atomic<uint32_t> sdk_version{614u};
    std::atomic<HRESULT> last_hr{S_FALSE};
    std::mutex path_mutex;
    std::string discovered_dir;
};

AgilitySdkRuntimeState& GetAgilityState() {
    static AgilitySdkRuntimeState st;
    return st;
}

std::string GetModuleDirectoryA(HMODULE mod) {
    char buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameA(mod, buf, static_cast<DWORD>(sizeof(buf) - 1u));
    if (n == 0u || n >= sizeof(buf) - 1u) return {};
    std::error_code ec;
    const auto p = std::filesystem::path(buf).parent_path();
    return ec ? std::string() : p.string();
}

void TryConfigureAgilitySdkOnce() noexcept {
    auto& st = GetAgilityState();
    std::call_once(st.init_once, [&st]() {
        uint32_t desired_ver = D3D12SDKVersion;
        {
            char ver_buf[32]{};
            const DWORD vlen = GetEnvironmentVariableA(
                "OPENSCARLETT_AGILITY_SDK_VERSION", ver_buf, static_cast<DWORD>(sizeof(ver_buf)));
            if (vlen > 0u && vlen < sizeof(ver_buf)) {
                char* end = nullptr;
                const unsigned long parsed = std::strtoul(ver_buf, &end, 10);
                if (end && end != ver_buf && parsed > 0ul) {
                    desired_ver = static_cast<uint32_t>(parsed);
                }
            }
        }
        st.sdk_version.store(desired_ver, std::memory_order_release);

        // Build ordered opt-in candidate directories for D3D12Core.dll.
        std::vector<std::string> candidates;
        {
            char env_dir[MAX_PATH * 2]{};
            const DWORD elen = GetEnvironmentVariableA(
                "OPENSCARLETT_AGILITY_SDK_DIR", env_dir, static_cast<DWORD>(sizeof(env_dir)));
            if (elen > 0u && elen < sizeof(env_dir)) {
                candidates.emplace_back(env_dir, elen);
            }
        }

        HMODULE self_mod = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&TryConfigureAgilitySdkOnce),
                &self_mod) != FALSE && self_mod != nullptr) {
            const std::string dll_dir = GetModuleDirectoryA(self_mod);
            if (!dll_dir.empty()) {
                candidates.push_back(dll_dir + "\\D3D12");
            }
        }

        const std::string exe_dir = GetModuleDirectoryA(nullptr);
        if (!exe_dir.empty()) {
            candidates.push_back(exe_dir + "\\D3D12");
        }
        candidates.emplace_back(".\\D3D12");

        std::string found_dir;
        for (const auto& dir : candidates) {
            if (dir.empty()) continue;
            std::error_code ec;
            const auto core_path = std::filesystem::path(dir) / "D3D12Core.dll";
            if (std::filesystem::exists(core_path, ec) && !ec) {
                found_dir = dir;
                break;
            }
        }

        if (found_dir.empty()) {
            // Graceful fallback to inbox d3d12.dll when D3D12Core.dll is absent.
            st.core_dll_found.store(false, std::memory_order_release);
            st.configured.store(false, std::memory_order_release);
            st.using_inbox_fallback.store(true, std::memory_order_release);
            st.last_hr.store(S_FALSE, std::memory_order_release);
            return;
        }

        st.core_dll_found.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(st.path_mutex);
            st.discovered_dir = found_dir;
        }

        auto get_iface = reinterpret_cast<PfnD3D12GetInterface>(
            openscarlett::ResolveHostD3D12Proc("D3D12GetInterface"));
        if (!get_iface) {
            st.using_inbox_fallback.store(true, std::memory_order_release);
            st.last_hr.store(E_NOINTERFACE, std::memory_order_release);
            return;
        }

        OS_ID3D12SDKConfiguration* sdk_cfg = nullptr;
        HRESULT hr = get_iface(
            kClsidD3D12SDKConfiguration,
            kIidID3D12SDKConfiguration,
            reinterpret_cast<void**>(&sdk_cfg));
        if (SUCCEEDED(hr) && sdk_cfg) {
            std::string sdk_path = found_dir;
            if (!sdk_path.empty() && sdk_path.back() != '\\' && sdk_path.back() != '/') {
                sdk_path.push_back('\\');
            }
            hr = sdk_cfg->SetSDKVersion(desired_ver, sdk_path.c_str());
            if (FAILED(hr)) {
                // Retry with canonical relative path ".\\D3D12\\" if absolute path wasn't accepted.
                hr = sdk_cfg->SetSDKVersion(desired_ver, D3D12SDKPath);
            }
            sdk_cfg->Release();
        }

        st.last_hr.store(hr, std::memory_order_release);
        if (SUCCEEDED(hr)) {
            st.configured.store(true, std::memory_order_release);
            st.using_inbox_fallback.store(false, std::memory_order_release);
        } else {
            st.configured.store(false, std::memory_order_release);
            st.using_inbox_fallback.store(true, std::memory_order_release);
        }
    });
}

bool EnvPrefersWarp() noexcept {
    char buf[32]{};
    const DWORD len = GetEnvironmentVariableA(
        "OPENSCARLETT_PREFER_WARP", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
}

HRESULT CreateWrappedDeviceInternal(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL min_level,
    bool prefer_warp,
    bool enable_debug_layer,
    HWND default_hwnd,
    REFIID riid,
    void** ppDevice) {
    // Opt-in D3D12 Agility SDK discovery (graceful fallback to inbox d3d12.dll when absent).
    TryConfigureAgilitySdkOnce();

    auto host_create = reinterpret_cast<PfnD3D12CreateDevice>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateDevice"));
    if (!host_create) {
        return E_FAIL;
    }

    // D3D12 probe mode (ppDevice == nullptr)
    if (!ppDevice) {
        return host_create(pAdapter, min_level, riid, nullptr);
    }
    *ppDevice = nullptr;

    ID3D12Device* host_dev = nullptr;
    IDXGIAdapter1* adapter1 = nullptr;
    openscarlett::D3D12AdapterInfo info{};

    if (pAdapter) {
        HRESULT hr = host_create(pAdapter, min_level, IID_PPV_ARGS(&host_dev));
        if (FAILED(hr) || !host_dev) {
            return hr;
        }
        pAdapter->QueryInterface(IID_PPV_ARGS(&adapter1));
        openscarlett::QueryD3D12DeviceAdapterInfo(host_dev, adapter1, info);
        if (adapter1) adapter1->Release();
    } else {
        openscarlett::D3D12AdapterSelectionOptions opts{};
        opts.prefer_warp = prefer_warp;
        opts.allow_warp_fallback = true;
        opts.enable_debug_layer = enable_debug_layer;
        opts.min_feature_level = static_cast<uint32_t>(min_level);
        openscarlett::Status st = openscarlett::SelectD3D12AdapterAndCreateDevice(
            opts, &host_dev, &adapter1, &info);
        if (adapter1) adapter1->Release();
        if (!st.ok() || !host_dev) {
            return DXGI_ERROR_UNSUPPORTED;
        }
    }

    auto* dev_x = new (std::nothrow) openscarlett::d3d12_xs::DeviceX(host_dev, info, default_hwnd);
    host_dev->Release();
    if (!dev_x) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = dev_x->QueryInterface(riid, ppDevice);
    dev_x->Release();
    return hr;
}

}  // namespace

namespace openscarlett::d3d12_xs {

void QueryAgilitySdkRuntimeSnapshot(D3D12XBOX_AGILITY_SDK_INFO* out_info) noexcept {
    if (!out_info) return;
    std::memset(out_info, 0, sizeof(*out_info));
    TryConfigureAgilitySdkOnce();
    auto& st = GetAgilityState();
    out_info->exported_sdk_version = D3D12SDKVersion;
    out_info->configured_sdk_version = st.sdk_version.load(std::memory_order_acquire);
    out_info->core_dll_found = st.core_dll_found.load(std::memory_order_acquire) ? 1u : 0u;
    out_info->agility_active = st.configured.load(std::memory_order_acquire) ? 1u : 0u;
    out_info->using_inbox_fallback = st.using_inbox_fallback.load(std::memory_order_acquire) ? 1u : 0u;
    out_info->last_config_hr = st.last_hr.load(std::memory_order_acquire);
    if (D3D12SDKPath) {
        const size_t plen = std::min<size_t>(std::strlen(D3D12SDKPath), sizeof(out_info->exported_sdk_path) - 1u);
        std::memcpy(out_info->exported_sdk_path, D3D12SDKPath, plen);
        out_info->exported_sdk_path[plen] = '\0';
    }
    std::lock_guard<std::mutex> lk(st.path_mutex);
    if (!st.discovered_dir.empty()) {
        const size_t dlen = std::min<size_t>(st.discovered_dir.size(), sizeof(out_info->discovered_core_dir) - 1u);
        std::memcpy(out_info->discovered_core_dir, st.discovered_dir.data(), dlen);
        out_info->discovered_core_dir[dlen] = '\0';
    }
}

}  // namespace openscarlett::d3d12_xs

extern "C" {

HRESULT WINAPI OS_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice) {
    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        MinimumFeatureLevel,
        (g_prefer_warp.load(std::memory_order_acquire) != 0) || EnvPrefersWarp(),
        false,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12CreateDevice", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12CreateDeviceX(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice) {
    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        MinimumFeatureLevel,
        (g_prefer_warp.load(std::memory_order_acquire) != 0) || EnvPrefersWarp(),
        false,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12CreateDeviceX", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    REFIID riid,
    void** ppDevice) {
    // Real GXDK 221000+ D3D12XBOX_CREATE_DEVICE_PARAMETERS does NOT carry OutputWindow or
    // PreferWarp fields — never misinterpret AmplificationShader* sizes as HWND/flags!
    const bool global_warp =
        (g_prefer_warp.load(std::memory_order_acquire) != 0) || EnvPrefersWarp();
    const bool debug_layer =
        (pParameters && (pParameters->ProcessDebugFlags & D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG) != 0);

    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        D3D_FEATURE_LEVEL_12_0,
        global_warp,
        debug_layer,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12XboxCreateDevice", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    auto fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    const HRESULT hr = fn ? fn(pRootSignature, Version, ppBlob, ppErrorBlob) : E_FAIL;
    RecordExport("D3D12SerializeRootSignature", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    auto fn = reinterpret_cast<PfnD3D12SerializeVersionedRootSignature>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeVersionedRootSignature"));
    const HRESULT hr = fn ? fn(pRootSignature, ppBlob, ppErrorBlob) : E_FAIL;
    RecordExport("D3D12SerializeVersionedRootSignature", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    auto fn = reinterpret_cast<PfnD3D12CreateRootSignatureDeserializer>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateRootSignatureDeserializer"));
    const HRESULT hr = fn
                           ? fn(pSrcData, SrcDataSizeInBytes,
                                pRootSignatureDeserializerInterface,
                                ppRootSignatureDeserializer)
                           : E_FAIL;
    RecordExport("D3D12CreateRootSignatureDeserializer", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    auto fn = reinterpret_cast<PfnD3D12CreateVersionedRootSignatureDeserializer>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateVersionedRootSignatureDeserializer"));
    const HRESULT hr = fn
                           ? fn(pSrcData, SrcDataSizeInBytes,
                                pRootSignatureDeserializerInterface,
                                ppRootSignatureDeserializer)
                           : E_FAIL;
    RecordExport("D3D12CreateVersionedRootSignatureDeserializer", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12GetDebugInterface(REFIID riid, void** ppvDebug) {
    auto fn = reinterpret_cast<PfnD3D12GetDebugInterface>(
        openscarlett::ResolveHostD3D12Proc("D3D12GetDebugInterface"));
    const HRESULT hr = fn ? fn(riid, ppvDebug) : E_FAIL;
    RecordExport("D3D12GetDebugInterface", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12EnableExperimentalFeatures(
    UINT NumFeatures,
    const IID* pIIDs,
    void* pConfigurationStructs,
    UINT* pConfigurationStructSizes) {
    auto fn = reinterpret_cast<PfnD3D12EnableExperimentalFeatures>(
        openscarlett::ResolveHostD3D12Proc("D3D12EnableExperimentalFeatures"));
    const HRESULT hr = fn
                           ? fn(NumFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes)
                           : E_FAIL;
    RecordExport("D3D12EnableExperimentalFeatures", hr);
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetPreferWarp(int prefer_warp) {
    g_prefer_warp.store(prefer_warp ? 1 : 0, std::memory_order_release);
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetLastPresentHash(
    ID3D12CommandQueue* queue,
    uint64_t* out_hash,
    uint32_t* out_width,
    uint32_t* out_height) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentCapture* capture = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentCapture,
                                       reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = capture->GetLastPresentStats(out_hash, out_width, out_height, nullptr);
    capture->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CopyLastPresentPixels(
    ID3D12CommandQueue* queue,
    uint8_t* out_rgba8,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentCapture* capture = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentCapture,
                                       reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = capture->CopyLastPresentPixels(out_rgba8, buffer_bytes, out_copied_bytes);
    capture->Release();
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ResetTelemetry() {
    openscarlett::CallTelemetry::Global().Reset();
}

OPENSCARLETT_D3D12_API int WINAPI OpenScarlettD3D12GetTelemetryEntry(
    const char* symbol_name,
    int32_t* out_vtable_slot,
    uint64_t* out_total_calls) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_vtable_slot) *out_vtable_slot = entry.vtable_slot;
            if (out_total_calls) *out_total_calls = entry.total_calls;
            return 1;
        }
    }
    if (out_vtable_slot) *out_vtable_slot = -1;
    if (out_total_calls) *out_total_calls = 0;
    return 0;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetPresentWindowState(
    ID3D12CommandQueue* queue,
    D3D12XBOX_PRESENT_WINDOW_STATE* out_state) {
    if (!queue) return E_INVALIDARG;
    if (!out_state) return E_POINTER;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = win->GetPresentWindowState(out_state);
    win->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12ToggleBorderlessFullscreen(
    ID3D12CommandQueue* queue) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = win->ToggleBorderlessFullscreen();
    win->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HANDLE WINAPI OpenScarlettD3D12GetFrameLatencyWaitableObject(
    ID3D12CommandQueue* queue) {
    if (!queue) return nullptr;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return nullptr;
    }
    HANDLE h = win->GetFrameLatencyWaitableObject();
    win->Release();
    return h;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetWindowCloseBehavior(
    ID3D12CommandQueue* queue,
    int post_quit_on_close) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = win->SetWindowCloseBehavior(post_quit_on_close ? TRUE : FALSE);
    win->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetPresentConfig(
    ID3D12CommandQueue* queue,
    const D3D12XBOX_PRESENTX_CONFIG* config) {
    if (!queue || !config) return E_INVALIDARG;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->SetPresentConfig(config);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetHdrMetaData(
    ID3D12CommandQueue* queue,
    const D3D12XBOX_HDR10_METADATA* metadata) {
    if (!queue || !metadata) return E_INVALIDARG;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->SetHdrMetaData(metadata);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetHdrSwapChainState(
    ID3D12CommandQueue* queue,
    D3D12XBOX_HDR_SWAPCHAIN_STATE* out_state) {
    if (!queue) return E_INVALIDARG;
    if (!out_state) return E_POINTER;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->GetHdrSwapChainState(out_state);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CopyLastPresentRawPixels(
    ID3D12CommandQueue* queue,
    uint8_t* out_raw,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes,
    DXGI_FORMAT* out_format) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->CopyLastPresentRawPixels(out_raw, buffer_bytes, out_copied_bytes, out_format);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API float WINAPI OpenScarlettD3D12LinearNitsToPqSt2084(float nits) {
    if (!std::isfinite(nits) || nits <= 0.0f) {
        return 0.0f;
    }
    const double y = std::clamp(static_cast<double>(nits) / kPqMaxNits, 0.0, 1.0);
    const double ym1 = std::pow(y, kPqM1);
    const double num = kPqC1 + kPqC2 * ym1;
    const double den = 1.0 + kPqC3 * ym1;
    const double pq = std::pow(num / den, kPqM2);
    return static_cast<float>(std::clamp(pq, 0.0, 1.0));
}

OPENSCARLETT_D3D12_API float WINAPI OpenScarlettD3D12PqSt2084ToLinearNits(float pq) {
    if (!std::isfinite(pq) || pq <= 0.0f) {
        return 0.0f;
    }
    const double n = std::clamp(static_cast<double>(pq), 0.0, 1.0);
    const double np = std::pow(n, 1.0 / kPqM2);
    const double num = std::max(np - kPqC1, 0.0);
    const double den = kPqC2 - kPqC3 * np;
    if (den <= 0.0) {
        return static_cast<float>(kPqMaxNits);
    }
    const double y = std::pow(num / den, 1.0 / kPqM1);
    return static_cast<float>(std::clamp(y * kPqMaxNits, 0.0, kPqMaxNits));
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(
    float r,
    float g,
    float b,
    float a,
    D3D12XBOX_TONEMAP_MODE mode,
    uint8_t out_rgba8[4]) {
    if (!out_rgba8) return;
    float mr = 0.0f;
    float mg = 0.0f;
    float mb = 0.0f;
    switch (mode) {
        case D3D12XBOX_TONEMAP_REINHARD:
            mr = ToneMapReinhardChannel(r);
            mg = ToneMapReinhardChannel(g);
            mb = ToneMapReinhardChannel(b);
            break;
        case D3D12XBOX_TONEMAP_NONE:
            mr = std::clamp(SanitizeNonNegative(r), 0.0f, 1.0f);
            mg = std::clamp(SanitizeNonNegative(g), 0.0f, 1.0f);
            mb = std::clamp(SanitizeNonNegative(b), 0.0f, 1.0f);
            break;
        case D3D12XBOX_TONEMAP_AUTO:
        case D3D12XBOX_TONEMAP_ACES:
        default:
            mr = ToneMapAcesChannel(r);
            mg = ToneMapAcesChannel(g);
            mb = ToneMapAcesChannel(b);
            break;
    }
    out_rgba8[0] = FloatToU8(LinearToSrgbChannel(mr));
    out_rgba8[1] = FloatToU8(LinearToSrgbChannel(mg));
    out_rgba8[2] = FloatToU8(LinearToSrgbChannel(mb));
    out_rgba8[3] = FloatToU8(std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 1.0f);
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12RegisterUnifiedTexture(
    ID3D12Device* device,
    const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc) {
    if (!device || !desc) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->RegisterUnifiedTexture(desc);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12UnregisterUnifiedTexture(
    ID3D12Device* device,
    ID3D12Resource* resource) {
    if (!device || !resource) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->UnregisterUnifiedTexture(resource);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12NotifyVirtualRemap(
    ID3D12Device* device,
    uint64_t guest_va_base,
    uint64_t byte_size,
    uint64_t new_backing_va,
    uint32_t* out_flagged_count) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->NotifyVirtualRemap(guest_va_base, byte_size, new_backing_va, out_flagged_count);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12FlushDirtyUnifiedTextures(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    uint32_t* out_flushed_count) {
    if (!device || !queue) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->FlushDirtyUnifiedTextures(queue, out_flushed_count);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetUnifiedTextureStats(
    ID3D12Device* device,
    D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats) {
    if (!device) return E_INVALIDARG;
    if (!out_stats) return E_POINTER;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->GetUnifiedTextureStats(out_stats);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API int WINAPI OpenScarlettD3D12GetTelemetryDetail(
    const char* symbol_name,
    int32_t* out_vtable_slot,
    uint64_t* out_total_calls,
    uint64_t* out_ok_calls,
    uint64_t* out_not_impl_calls,
    uint64_t* out_error_calls,
    int32_t* out_last_hr) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_vtable_slot) *out_vtable_slot = entry.vtable_slot;
            if (out_total_calls) *out_total_calls = entry.total_calls;
            if (out_ok_calls) *out_ok_calls = entry.ok_calls;
            if (out_not_impl_calls) *out_not_impl_calls = entry.not_impl_calls;
            if (out_error_calls) *out_error_calls = entry.error_calls;
            if (out_last_hr) *out_last_hr = entry.last_hr;
            return 1;
        }
    }
    if (out_vtable_slot) *out_vtable_slot = -1;
    if (out_total_calls) *out_total_calls = 0;
    if (out_ok_calls) *out_ok_calls = 0;
    if (out_not_impl_calls) *out_not_impl_calls = 0;
    if (out_error_calls) *out_error_calls = 0;
    if (out_last_hr) *out_last_hr = 0;
    return 0;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxCreateDmaCommandQueue(
    ID3D12Device* pDevice,
    const void* pDesc,
    REFIID riid,
    void** ppCommandQueue) {
    (void)pDevice;
    (void)pDesc;
    (void)riid;
    if (ppCommandQueue) {
        *ppCommandQueue = nullptr;
    }
    RecordExport("D3D12XboxCreateDmaCommandQueue", E_NOTIMPL);
    return E_NOTIMPL;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxEnableDccMetadata(
    ID3D12Device* pDevice,
    ID3D12Resource* pResource,
    uint32_t Flags) {
    (void)pDevice;
    (void)pResource;
    (void)Flags;
    RecordExport("D3D12XboxEnableDccMetadata", E_NOTIMPL);
    return E_NOTIMPL;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueKickoffX(
    ID3D12CommandQueue* queue) {
    if (!queue) return E_INVALIDARG;
    ID3D12CommandQueueX* qx = nullptr;
    HRESULT hr = queue->QueryInterface(IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx));
    if (FAILED(hr) || !qx) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = qx->KickoffX();
    qx->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueWait32BitValueX(
    ID3D12CommandQueue* queue,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    uint32_t value,
    uint32_t mask,
    D3D12XBOX_WAIT_VALUE_OP operation) {
    if (!queue) return E_INVALIDARG;
    ID3D12CommandQueueX* qx = nullptr;
    HRESULT hr = queue->QueryInterface(IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx));
    if (FAILED(hr) || !qx) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = qx->Wait32BitValueX(address, value, mask, operation);
    qx->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueWait64BitValueX(
    ID3D12CommandQueue* queue,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    uint64_t value,
    uint64_t mask,
    D3D12XBOX_WAIT_VALUE_OP operation) {
    if (!queue) return E_INVALIDARG;
    ID3D12CommandQueueX* qx = nullptr;
    HRESULT hr = queue->QueryInterface(IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx));
    if (FAILED(hr) || !qx) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = qx->Wait64BitValueX(address, value, mask, operation);
    qx->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListKickoffX(
    ID3D12GraphicsCommandList* cmd_list) {
    if (!cmd_list) return E_INVALIDARG;
    ID3D12GraphicsCommandListX* clx = nullptr;
    HRESULT hr = cmd_list->QueryInterface(
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(&clx));
    if (FAILED(hr) || !clx) return FAILED(hr) ? hr : E_NOINTERFACE;
    clx->KickoffX();
    clx->Release();
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListRSSetDepthBiasX(
    ID3D12GraphicsCommandList* cmd_list,
    float depth_bias,
    float depth_bias_clamp,
    float slope_scaled_depth_bias) {
    if (!cmd_list) return E_INVALIDARG;
    ID3D12GraphicsCommandListX* clx = nullptr;
    HRESULT hr = cmd_list->QueryInterface(
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(&clx));
    if (FAILED(hr) || !clx) return FAILED(hr) ? hr : E_NOINTERFACE;
    clx->RSSetDepthBiasX(depth_bias, depth_bias_clamp, slope_scaled_depth_bias);
    clx->Release();
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListGetDepthBiasX(
    ID3D12GraphicsCommandList* cmd_list,
    float* out_depth_bias,
    float* out_depth_bias_clamp,
    float* out_slope_scaled_depth_bias) {
    if (!cmd_list) return E_INVALIDARG;
    ID3D12GraphicsCommandListX* clx = nullptr;
    HRESULT hr = cmd_list->QueryInterface(
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(&clx));
    if (FAILED(hr) || !clx) return FAILED(hr) ? hr : E_NOINTERFACE;
    auto* impl = static_cast<openscarlett::d3d12_xs::CommandListX*>(clx);
    impl->GetDepthBiasXState(out_depth_bias, out_depth_bias_clamp, out_slope_scaled_depth_bias);
    clx->Release();
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12AttachXMemAllocation(
    ID3D12Device* device,
    uint64_t guest_va,
    uint64_t byte_size,
    uint64_t* out_desktop_gpu_va) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->AttachXMemAllocation(guest_va, byte_size, out_desktop_gpu_va);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12DetachXMemAllocation(
    ID3D12Device* device,
    uint64_t guest_va) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->DetachXMemAllocation(guest_va);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12ResolveXMemGpuVa(
    ID3D12Device* device,
    uint64_t guest_va,
    uint64_t* out_desktop_gpu_va) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->ResolveXMemGpuVa(guest_va, out_desktop_gpu_va);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetForceR9G9B9E5UavFallback(
    int force_fallback) {
    openscarlett::d3d12_xs::DeviceX::SetForceR9G9B9E5UavFallback(force_fallback != 0);
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetR9G9B9E5UavFallbackActive(
    ID3D12Device* device,
    int* out_active) {
    if (!device) return E_INVALIDARG;
    if (!out_active) return E_POINTER;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->GetR9G9B9E5UavFallbackActive(out_active);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetPresentTimingStats(
    ID3D12CommandQueue* queue,
    D3D12XBOX_PRESENT_TIMING_STATS* out_stats) {
    if (!queue) return E_INVALIDARG;
    if (!out_stats) return E_POINTER;
    IOpenScarlettPresentTiming* timing = nullptr;
    HRESULT hr = queue->QueryInterface(
        IID_IOpenScarlettPresentTiming, reinterpret_cast<void**>(&timing));
    if (FAILED(hr) || !timing) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = timing->GetPresentTimingStats(out_stats);
    timing->Release();
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetBackendMode(
    uint32_t backend_mode) {
    openscarlett::d3d12_xs::DeviceX::SetGlobalBackendMode(backend_mode);
}

OPENSCARLETT_D3D12_API uint32_t WINAPI OpenScarlettD3D12GetActiveBackend(
    ID3D12Device* device) {
    if (!device) {
        return openscarlett::d3d12_xs::DeviceX::ResolveEffectiveBackendMode();
    }
    IOpenScarlettVulkanRouting* routing = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettVulkanRouting, reinterpret_cast<void**>(&routing));
    if (FAILED(hr) || !routing) {
        return D3D12XBOX_BACKEND_D3D12;
    }
    const uint32_t mode = routing->GetActiveBackendMode();
    routing->Release();
    return mode;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetVulkanRoutingStats(
    ID3D12Device* device,
    D3D12XBOX_VULKAN_ROUTING_STATS* out_stats) {
    if (!device) return E_INVALIDARG;
    if (!out_stats) return E_POINTER;
    IOpenScarlettVulkanRouting* routing = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettVulkanRouting, reinterpret_cast<void**>(&routing));
    if (FAILED(hr) || !routing) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = routing->GetVulkanRoutingStats(out_stats);
    routing->Release();
    return hr;
}

}  // extern "C"
