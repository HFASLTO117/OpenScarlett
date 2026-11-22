#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#endif

#include "os_check.h"

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
fs::path GetExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (len > 0 && len < (sizeof(buf) / sizeof(buf[0]))) {
        return fs::path(buf).parent_path();
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

void VerifyGuidExport(HMODULE mod, const char* symbol_name, const GUID& expected) {
    const void* sym = reinterpret_cast<const void*>(GetProcAddress(mod, symbol_name));
    if (!sym) {
        std::fprintf(stderr, "Missing data export from d3d12_xs.dll: %s\n", symbol_name);
    }
    OS_REQUIRE(sym != nullptr);
    GUID actual{};
    std::memcpy(&actual, sym, sizeof(GUID));
    if (std::memcmp(&actual, &expected, sizeof(GUID)) != 0) {
        std::fprintf(
            stderr,
            "GUID mismatch for %s: actual={%08lX-%04X-%04X-...} expected={%08lX-%04X-%04X-...}\n",
            symbol_name,
            static_cast<unsigned long>(actual.Data1),
            static_cast<unsigned int>(actual.Data2),
            static_cast<unsigned int>(actual.Data3),
            static_cast<unsigned long>(expected.Data1),
            static_cast<unsigned int>(expected.Data2),
            static_cast<unsigned int>(expected.Data3));
    }
    OS_REQUIRE(std::memcmp(&actual, &expected, sizeof(GUID)) == 0);
}

void TestD3D12XsGuidExports() {
    std::printf("[1/2] d3d12_xs.dll exported GUID data symbols (31 IID_ID3D12* exports)...\n");

    const fs::path dll_path = GetExeDir() / L"d3d12_xs.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    // Explicit byte-exact verification for IID_ID3D12Device: {189819f1-1db6-4b57-be54-1821339b85f7}
    constexpr GUID kExpectedDeviceGuid = {
        0x189819f1u, 0x1db6u, 0x4b57u, {0xbeu, 0x54u, 0x18u, 0x21u, 0x33u, 0x9bu, 0x85u, 0xf7u}};
    OS_REQUIRE(std::memcmp(&kExpectedDeviceGuid, &__uuidof(ID3D12Device), sizeof(GUID)) == 0);
    VerifyGuidExport(mod, "IID_ID3D12Device", kExpectedDeviceGuid);

    // Core sibling D3D12 COM interface GUID data exports
    VerifyGuidExport(mod, "IID_ID3D12Device1", __uuidof(ID3D12Device1));
    VerifyGuidExport(mod, "IID_ID3D12Device2", __uuidof(ID3D12Device2));
    VerifyGuidExport(mod, "IID_ID3D12Device3", __uuidof(ID3D12Device3));
    VerifyGuidExport(mod, "IID_ID3D12Device4", __uuidof(ID3D12Device4));
    VerifyGuidExport(mod, "IID_ID3D12Device5", __uuidof(ID3D12Device5));
    VerifyGuidExport(mod, "IID_ID3D12Device6", __uuidof(ID3D12Device6));
    VerifyGuidExport(mod, "IID_ID3D12Device7", __uuidof(ID3D12Device7));
    VerifyGuidExport(mod, "IID_ID3D12Device8", __uuidof(ID3D12Device8));
    VerifyGuidExport(mod, "IID_ID3D12CommandQueue", __uuidof(ID3D12CommandQueue));
    VerifyGuidExport(mod, "IID_ID3D12CommandAllocator", __uuidof(ID3D12CommandAllocator));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList", __uuidof(ID3D12GraphicsCommandList));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList1", __uuidof(ID3D12GraphicsCommandList1));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList2", __uuidof(ID3D12GraphicsCommandList2));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList3", __uuidof(ID3D12GraphicsCommandList3));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList4", __uuidof(ID3D12GraphicsCommandList4));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList5", __uuidof(ID3D12GraphicsCommandList5));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList6", __uuidof(ID3D12GraphicsCommandList6));
    VerifyGuidExport(mod, "IID_ID3D12Resource", __uuidof(ID3D12Resource));
    VerifyGuidExport(mod, "IID_ID3D12Resource1", __uuidof(ID3D12Resource1));
    VerifyGuidExport(mod, "IID_ID3D12Resource2", __uuidof(ID3D12Resource2));
    VerifyGuidExport(mod, "IID_ID3D12Heap", __uuidof(ID3D12Heap));
    VerifyGuidExport(mod, "IID_ID3D12Heap1", __uuidof(ID3D12Heap1));
    VerifyGuidExport(mod, "IID_ID3D12Fence", __uuidof(ID3D12Fence));
    VerifyGuidExport(mod, "IID_ID3D12Fence1", __uuidof(ID3D12Fence1));
    VerifyGuidExport(mod, "IID_ID3D12PipelineState", __uuidof(ID3D12PipelineState));
    VerifyGuidExport(mod, "IID_ID3D12RootSignature", __uuidof(ID3D12RootSignature));
    VerifyGuidExport(mod, "IID_ID3D12DescriptorHeap", __uuidof(ID3D12DescriptorHeap));
    VerifyGuidExport(mod, "IID_ID3D12QueryHeap", __uuidof(ID3D12QueryHeap));
    VerifyGuidExport(mod, "IID_ID3D12CommandSignature", __uuidof(ID3D12CommandSignature));
    VerifyGuidExport(mod, "IID_ID3D12StateObject", __uuidof(ID3D12StateObject));

    OS_REQUIRE(FreeLibrary(mod) != FALSE);
}

void TestXMemTransferMemory() {
    std::printf("[2/2] xmem.dll XMemTransferMemory (normal, overlapping, zero-size, null checks, telemetry)...\n");

    const fs::path dll_path = GetExeDir() / L"xmem.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnXMemTransferMemory = HRESULT(WINAPI*)(void*, const void*, SIZE_T, UINT64);
    using PfnGetTelemetry = int32_t(WINAPI*)(const char*, uint64_t*, uint64_t*);

    auto transfer_fn = reinterpret_cast<PfnXMemTransferMemory>(
        GetProcAddress(mod, "XMemTransferMemory"));
    auto telemetry_fn = reinterpret_cast<PfnGetTelemetry>(
        GetProcAddress(mod, "XMemGetTelemetryCallCountForTest"));
    OS_REQUIRE(transfer_fn != nullptr);
    OS_REQUIRE(telemetry_fn != nullptr);

    // 1. Normal non-overlapping copy with guard sentinels
    {
        constexpr size_t kGuard = 16u;
        constexpr size_t kSize = 4096u;
        std::vector<uint8_t> src(kSize);
        for (size_t i = 0; i < kSize; ++i) {
            src[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
        }
        std::vector<uint8_t> dst(kSize + 2u * kGuard, 0xCCu);
        const HRESULT hr = transfer_fn(dst.data() + kGuard, src.data(), kSize, 0ull);
        OS_REQUIRE(hr == S_OK);
        OS_REQUIRE(std::memcmp(dst.data() + kGuard, src.data(), kSize) == 0);
        for (size_t i = 0; i < kGuard; ++i) {
            OS_REQUIRE(dst[i] == 0xCCu);
            OS_REQUIRE(dst[kGuard + kSize + i] == 0xCCu);
        }
    }

    // 2. Overlapping copy (forward: dst > src) with non-zero Scarlett transfer flags
    {
        constexpr size_t kBufSize = 512u;
        constexpr size_t kShift = 37u;
        constexpr size_t kCopyLen = 300u;
        std::vector<uint8_t> buf(kBufSize);
        for (size_t i = 0; i < kBufSize; ++i) {
            buf[i] = static_cast<uint8_t>((i * 17u + 5u) & 0xFFu);
        }
        std::vector<uint8_t> expected = buf;
        std::memmove(expected.data() + kShift, expected.data(), kCopyLen);

        const HRESULT hr = transfer_fn(buf.data() + kShift, buf.data(), kCopyLen, 0x1ull);
        OS_REQUIRE(hr == S_OK);
        OS_REQUIRE(buf == expected);
    }

    // 3. Overlapping copy (backward: dst < src)
    {
        constexpr size_t kBufSize = 512u;
        constexpr size_t kShift = 41u;
        constexpr size_t kCopyLen = 300u;
        std::vector<uint8_t> buf(kBufSize);
        for (size_t i = 0; i < kBufSize; ++i) {
            buf[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
        }
        std::vector<uint8_t> expected = buf;
        std::memmove(expected.data(), expected.data() + kShift, kCopyLen);

        const HRESULT hr = transfer_fn(buf.data(), buf.data() + kShift, kCopyLen, 0ull);
        OS_REQUIRE(hr == S_OK);
        OS_REQUIRE(buf == expected);
    }

    // 4. Self-copy (dst == src)
    {
        uint8_t buf[64]{};
        for (size_t i = 0; i < sizeof(buf); ++i) {
            buf[i] = static_cast<uint8_t>(i ^ 0xA5u);
        }
        uint8_t expected[64]{};
        std::memcpy(expected, buf, sizeof(buf));
        OS_REQUIRE(transfer_fn(buf, buf, sizeof(buf), 0ull) == S_OK);
        OS_REQUIRE(std::memcmp(buf, expected, sizeof(buf)) == 0);
    }

    // 5. Zero-size copy with valid pointers leaves destination untouched
    {
        uint8_t src[16]{};
        uint8_t dst[16]{};
        std::memset(src, 0x11, sizeof(src));
        std::memset(dst, 0xEE, sizeof(dst));
        OS_REQUIRE(transfer_fn(dst, src, 0u, 0ull) == S_OK);
        for (uint8_t b : dst) {
            OS_REQUIRE(b == 0xEEu);
        }
    }

    // 6. Null-pointer validation
    {
        uint8_t buf[32]{};
        const HRESULT hr_null_dst = transfer_fn(nullptr, buf, sizeof(buf), 0ull);
        OS_REQUIRE(FAILED(hr_null_dst) && (hr_null_dst == E_POINTER || hr_null_dst == E_INVALIDARG));

        const HRESULT hr_null_src = transfer_fn(buf, nullptr, sizeof(buf), 0ull);
        OS_REQUIRE(FAILED(hr_null_src) && (hr_null_src == E_POINTER || hr_null_src == E_INVALIDARG));

        const HRESULT hr_both_null = transfer_fn(nullptr, nullptr, sizeof(buf), 0ull);
        OS_REQUIRE(FAILED(hr_both_null) && (hr_both_null == E_POINTER || hr_both_null == E_INVALIDARG));

        const HRESULT hr_null_zero = transfer_fn(nullptr, buf, 0u, 0ull);
        OS_REQUIRE(FAILED(hr_null_zero) && (hr_null_zero == E_POINTER || hr_null_zero == E_INVALIDARG));
    }

    // 7. CallTelemetry recorded all 9 calls (5 ok + 4 errors)
    {
        uint64_t total_calls = 0;
        uint64_t ok_calls = 0;
        OS_REQUIRE(telemetry_fn("XMemTransferMemory", &total_calls, &ok_calls) == 1);
        OS_REQUIRE(total_calls == 9ull);
        OS_REQUIRE(ok_calls == 5ull);
    }

    OS_REQUIRE(FreeLibrary(mod) != FALSE);
}
#endif

}  // namespace

int main() {
#if defined(_WIN32)
    TestD3D12XsGuidExports();
    TestXMemTransferMemory();
    std::printf("ALL test_mine_d3d12_xmem checks PASSED.\n");
    return 0;
#else
    std::printf("test_mine_d3d12_xmem skipped on non-Windows host.\n");
    return 0;
#endif
}
