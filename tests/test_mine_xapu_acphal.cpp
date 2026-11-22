#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/xapu_abi.h"
#include "os_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
fs::path GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
    return fs::current_path();
}

void TestXApuDll() {
    std::printf("[1/2] Testing xapu.dll dynamic load, exports, command/result lifecycle...\n");
    const fs::path dll_path = GetExeDir() / L"xapu.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnConnect = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE*);
    using PfnEnqueue = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE, const void*, uint32_t);
    using PfnDequeue = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE, void*, uint32_t, uint32_t*);
    using PfnDisconnect = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE);

    auto p_connect = reinterpret_cast<PfnConnect>(::GetProcAddress(mod, "XApuConnect"));
    auto p_enqueue = reinterpret_cast<PfnEnqueue>(::GetProcAddress(mod, "XApuEnqueueCommand"));
    auto p_dequeue = reinterpret_cast<PfnDequeue>(::GetProcAddress(mod, "XApuDequeueResult"));
    auto p_disconnect = reinterpret_cast<PfnDisconnect>(::GetProcAddress(mod, "XApuDisconnect"));

    OS_REQUIRE(p_connect != nullptr);
    OS_REQUIRE(p_enqueue != nullptr);
    OS_REQUIRE(p_dequeue != nullptr);
    OS_REQUIRE(p_disconnect != nullptr);

    // Null/invalid argument checks
    OS_REQUIRE(p_connect(nullptr) == XAPU_E_POINTER);
    OS_REQUIRE(p_disconnect(nullptr) == XAPU_E_HANDLE);
    OS_REQUIRE(p_disconnect(reinterpret_cast<XAPU_CONNECTION_HANDLE>(0x1234)) == XAPU_E_HANDLE);

    XAPU_CONNECTION_HANDLE conn = nullptr;
    OS_REQUIRE(p_connect(&conn) == XAPU_S_OK);
    OS_REQUIRE(conn != nullptr);

    // Empty queue returns XAPU_S_EMPTY (non-blocking)
    XApuResultHeader res{};
    uint32_t out_size = 99u;
    OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), &out_size) == XAPU_S_EMPTY);
    OS_REQUIRE(out_size == 0u);

    // Invalid enqueue/dequeue inputs
    OS_REQUIRE(p_enqueue(nullptr, &res, sizeof(res)) == XAPU_E_HANDLE);
    OS_REQUIRE(p_enqueue(conn, nullptr, 16u) == XAPU_E_POINTER);
    OS_REQUIRE(p_enqueue(conn, &res, 0u) == XAPU_E_INVALIDARG);
    OS_REQUIRE(p_dequeue(nullptr, &res, sizeof(res), &out_size) == XAPU_E_HANDLE);
    OS_REQUIRE(p_dequeue(conn, nullptr, sizeof(res), &out_size) == XAPU_E_POINTER);
    OS_REQUIRE(p_dequeue(conn, &res, 0u, &out_size) == XAPU_E_INVALIDARG);

    // Enqueue two commands and verify FIFO completion dequeue with matching tags
    XApuCommandHeader cmd1{};
    cmd1.commandType = XApuCommandType_DecodeOpus;
    cmd1.commandSize = sizeof(cmd1);
    cmd1.commandTag = 0xCAFE0001ull;

    XApuCommandHeader cmd2{};
    cmd2.commandType = XApuCommandType_ProcessDsp;
    cmd2.commandSize = sizeof(cmd2);
    cmd2.commandTag = 0xCAFE0002ull;

    OS_REQUIRE(p_enqueue(conn, &cmd1, sizeof(cmd1)) == XAPU_S_OK);
    OS_REQUIRE(p_enqueue(conn, &cmd2, sizeof(cmd2)) == XAPU_S_OK);

    std::memset(&res, 0, sizeof(res));
    out_size = 0u;
    OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), &out_size) == XAPU_S_OK);
    OS_REQUIRE(out_size == sizeof(XApuResultHeader));
    OS_REQUIRE(res.resultType == XApuCommandType_DecodeOpus);
    OS_REQUIRE(res.commandTag == 0xCAFE0001ull);
    OS_REQUIRE(res.status == XAPU_S_OK);
    OS_REQUIRE(res.bytesWritten == sizeof(cmd1));

    std::memset(&res, 0, sizeof(res));
    out_size = 0u;
    OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), &out_size) == XAPU_S_OK);
    OS_REQUIRE(out_size == sizeof(XApuResultHeader));
    OS_REQUIRE(res.resultType == XApuCommandType_ProcessDsp);
    OS_REQUIRE(res.commandTag == 0xCAFE0002ull);
    OS_REQUIRE(res.status == XAPU_S_OK);

    // Queue empty again
    OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), &out_size) == XAPU_S_EMPTY);
    OS_REQUIRE(out_size == 0u);

    // Concurrent multi-connection thread-safety check
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            XAPU_CONNECTION_HANDLE h = nullptr;
            OS_REQUIRE(p_connect(&h) == XAPU_S_OK && h != nullptr);
            for (uint32_t i = 0; i < 32u; ++i) {
                XApuCommandHeader c{};
                c.commandType = XApuCommandType_DecodeOpus;
                c.commandSize = sizeof(c);
                c.commandTag = (static_cast<uint64_t>(t) << 32u) | i;
                OS_REQUIRE(p_enqueue(h, &c, sizeof(c)) == XAPU_S_OK);
                XApuResultHeader r{};
                uint32_t sz = 0u;
                OS_REQUIRE(p_dequeue(h, &r, sizeof(r), &sz) == XAPU_S_OK);
                OS_REQUIRE(r.commandTag == c.commandTag && r.status == XAPU_S_OK);
            }
            OS_REQUIRE(p_disconnect(h) == XAPU_S_OK);
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    // Disconnect and verify handle is invalidated
    OS_REQUIRE(p_disconnect(conn) == XAPU_S_OK);
    OS_REQUIRE(p_disconnect(conn) == XAPU_E_HANDLE);
    OS_REQUIRE(p_enqueue(conn, &cmd1, sizeof(cmd1)) == XAPU_E_HANDLE);
    OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), &out_size) == XAPU_E_HANDLE);

    ::FreeLibrary(mod);
    std::printf("  PASS: xapu.dll (4/4 exports, FIFO command/result queue, multi-thread safe).\n");
}

void TestAcpHalApuHeap() {
    std::printf("[2/2] Testing AcpHal.dll dynamic load, Apu* exports, alignment & address mapping...\n");
    const fs::path dll_path = GetExeDir() / L"AcpHal.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnCreateHeap = int32_t(__stdcall*)(uint32_t, uint32_t);
    using PfnAlloc = int32_t(__stdcall*)(void**, APU_ADDRESS*, uint32_t, uint32_t, uint32_t);
    using PfnFree = int32_t(__stdcall*)(void*);
    using PfnHeapGetState = int32_t(__stdcall*)(APU_HEAP_STATE*, uint32_t);
    using PfnIsValid = int32_t(__stdcall*)(const void*, uint32_t);
    using PfnMapVa = APU_ADDRESS(__stdcall*)(const void*);
    using PfnMapApu = void*(__stdcall*)(APU_ADDRESS);

    auto p_create_heap = reinterpret_cast<PfnCreateHeap>(::GetProcAddress(mod, "ApuCreateHeap"));
    auto p_alloc = reinterpret_cast<PfnAlloc>(::GetProcAddress(mod, "ApuAlloc"));
    auto p_free = reinterpret_cast<PfnFree>(::GetProcAddress(mod, "ApuFree"));
    auto p_get_state = reinterpret_cast<PfnHeapGetState>(::GetProcAddress(mod, "ApuHeapGetState"));
    auto p_is_valid = reinterpret_cast<PfnIsValid>(::GetProcAddress(mod, "ApuIsVirtualAddressValid"));
    auto p_map_va = reinterpret_cast<PfnMapVa>(::GetProcAddress(mod, "ApuMapVirtualAddress"));
    auto p_map_apu = reinterpret_cast<PfnMapApu>(::GetProcAddress(mod, "ApuMapApuAddress"));

    OS_REQUIRE(p_create_heap != nullptr);
    OS_REQUIRE(p_alloc != nullptr);
    OS_REQUIRE(p_free != nullptr);
    OS_REQUIRE(p_get_state != nullptr);
    OS_REQUIRE(p_is_valid != nullptr);
    OS_REQUIRE(p_map_va != nullptr);
    OS_REQUIRE(p_map_apu != nullptr);

    // Also verify pre-existing AcpHal.dll exports remain intact
    OS_REQUIRE(::GetProcAddress(mod, "AcpHalCreate") != nullptr);
    OS_REQUIRE(::GetProcAddress(mod, "AcpHalAllocateShapeContexts") != nullptr);
    OS_REQUIRE(::GetProcAddress(mod, "AcpHalReleaseShapeContexts") != nullptr);

    // 1. Test auto-initialization when ApuAlloc is called before ApuCreateHeap
    {
        void* auto_va = nullptr;
        APU_ADDRESS auto_apu = APU_INVALID_ADDRESS;
        OS_REQUIRE(p_alloc(&auto_va, &auto_apu, 512u, 64u, APU_ALLOC_CACHED) == ACPHAL_S_OK);
        OS_REQUIRE(auto_va != nullptr);
        OS_REQUIRE(auto_apu != APU_INVALID_ADDRESS);
        OS_REQUIRE((reinterpret_cast<uintptr_t>(auto_va) % 64u) == 0u);
        OS_REQUIRE((auto_apu % 64u) == 0u);
        OS_REQUIRE(p_free(auto_va) == ACPHAL_S_OK);
    }

    // 2. Explicit ApuCreateHeap with known capacities
    constexpr uint32_t kCachedCap = 4u * 1024u * 1024u;
    constexpr uint32_t kNonCachedCap = 1u * 1024u * 1024u;
    OS_REQUIRE(p_create_heap(kCachedCap, kNonCachedCap) == ACPHAL_S_OK);

    APU_HEAP_STATE st_cached{};
    APU_HEAP_STATE st_noncached{};
    OS_REQUIRE(p_get_state(&st_cached, APU_ALLOC_CACHED) == ACPHAL_S_OK);
    OS_REQUIRE(p_get_state(&st_noncached, APU_ALLOC_NONCACHED) == ACPHAL_S_OK);
    OS_REQUIRE(st_cached.bytesAllocated == 0u);
    OS_REQUIRE(st_cached.bytesFree == kCachedCap);
    OS_REQUIRE(st_cached.bytesLost == 0u);
    OS_REQUIRE(st_cached.maximumBlockSizeAvailable == kCachedCap);
    OS_REQUIRE(st_cached.allocationCount == 0u);
    OS_REQUIRE(st_noncached.bytesFree == kNonCachedCap);

    // 3. Invalid parameter checks
    void* bad_va = nullptr;
    OS_REQUIRE(p_alloc(nullptr, nullptr, 64u, 64u, APU_ALLOC_CACHED) == ACPHAL_E_POINTER);
    OS_REQUIRE(p_alloc(&bad_va, nullptr, 0u, 64u, APU_ALLOC_CACHED) == ACPHAL_E_INVALIDARG);
    OS_REQUIRE(p_alloc(&bad_va, nullptr, 64u, 48u, APU_ALLOC_CACHED) == ACPHAL_E_INVALIDARG);
    OS_REQUIRE(p_alloc(&bad_va, nullptr, kCachedCap + 4096u, 64u, APU_ALLOC_CACHED) == ACPHAL_E_OUTOFMEMORY);
    OS_REQUIRE(p_get_state(nullptr, APU_ALLOC_CACHED) == ACPHAL_E_POINTER);
    OS_REQUIRE(p_free(nullptr) == ACPHAL_S_OK);
    OS_REQUIRE(p_free(reinterpret_cast<void*>(0x12345678ull)) == ACPHAL_E_INVALIDARG);

    // 4. Aligned allocations (4096-byte alignment, zero-initialized, optional physicalAddress)
    void* va1 = nullptr;
    APU_ADDRESS apu1 = APU_INVALID_ADDRESS;
    constexpr uint32_t kSize1 = 4096u;
    OS_REQUIRE(p_alloc(&va1, &apu1, kSize1, 4096u, APU_ALLOC_CACHED) == ACPHAL_S_OK);
    OS_REQUIRE(va1 != nullptr && apu1 != APU_INVALID_ADDRESS);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(va1) % 4096u) == 0u);
    OS_REQUIRE((apu1 % 4096u) == 0u);

    // Verify zero-initialized
    const auto* bytes1 = static_cast<const uint8_t*>(va1);
    for (uint32_t i = 0; i < kSize1; ++i) {
        OS_REQUIRE(bytes1[i] == 0u);
    }

    // Write pattern and verify through mapped pointer
    auto* mut1 = static_cast<uint8_t*>(va1);
    mut1[0] = 0xA5u;
    mut1[1337] = 0x5Au;

    // Non-cached allocation with nullptr physicalAddress and non-multiple-of-4 size (tests bytesLost)
    void* va2 = nullptr;
    constexpr uint32_t kSize2 = 1001u;
    OS_REQUIRE(p_alloc(&va2, nullptr, kSize2, 256u, APU_ALLOC_NONCACHED) == ACPHAL_S_OK);
    OS_REQUIRE(va2 != nullptr);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(va2) % 256u) == 0u);
    const APU_ADDRESS apu2 = p_map_va(va2);
    OS_REQUIRE(apu2 != APU_INVALID_ADDRESS && (apu2 % 256u) == 0u);

    // 5. ApuIsVirtualAddressValid in-bounds & out-of-bounds checks
    OS_REQUIRE(p_is_valid(nullptr, 1u) == 0);
    OS_REQUIRE(p_is_valid(va1, kSize1) != 0);
    OS_REQUIRE(p_is_valid(mut1 + 1024u, kSize1 - 1024u) != 0);
    OS_REQUIRE(p_is_valid(mut1 + 1024u, kSize1 - 1023u) == 0);  // 1 byte past end
    OS_REQUIRE(p_is_valid(mut1 + kSize1, 1u) == 0);             // exact end
    OS_REQUIRE(p_is_valid(mut1 - 1u, 1u) == 0);                 // 1 byte before start
    OS_REQUIRE(p_is_valid(va1, 0xFFFFFFFFu) == 0);              // overflow guard

    // 6. ApuMapVirtualAddress <-> ApuMapApuAddress round-trip (base & interior offset)
    OS_REQUIRE(p_map_va(nullptr) == APU_INVALID_ADDRESS);
    OS_REQUIRE(p_map_apu(APU_INVALID_ADDRESS) == nullptr);
    OS_REQUIRE(p_map_va(va1) == apu1);
    OS_REQUIRE(p_map_apu(apu1) == va1);

    constexpr uint32_t kInterior = 1337u;
    const APU_ADDRESS interior_apu = p_map_va(mut1 + kInterior);
    OS_REQUIRE(interior_apu == apu1 + kInterior);
    void* roundtrip_va = p_map_apu(interior_apu);
    OS_REQUIRE(roundtrip_va == mut1 + kInterior);
    OS_REQUIRE(*static_cast<const uint8_t*>(roundtrip_va) == 0x5Au);

    // Out-of-bounds mapping returns invalid/null
    OS_REQUIRE(p_map_va(mut1 + kSize1) == APU_INVALID_ADDRESS);
    OS_REQUIRE(p_map_apu(apu1 + kSize1) == nullptr);

    // 7. ApuHeapGetState counters
    OS_REQUIRE(p_get_state(&st_cached, APU_ALLOC_CACHED) == ACPHAL_S_OK);
    OS_REQUIRE(st_cached.bytesAllocated == kSize1);
    OS_REQUIRE(st_cached.bytesLost == 0u);
    OS_REQUIRE(st_cached.bytesFree == kCachedCap - kSize1);
    OS_REQUIRE(st_cached.maximumBlockSizeAvailable == st_cached.bytesFree);
    OS_REQUIRE(st_cached.allocationCount == 1u);

    OS_REQUIRE(p_get_state(&st_noncached, APU_ALLOC_NONCACHED) == ACPHAL_S_OK);
    OS_REQUIRE(st_noncached.bytesAllocated == kSize2);
    OS_REQUIRE(st_noncached.bytesLost == 3u);
    OS_REQUIRE(st_noncached.bytesFree == kNonCachedCap - (kSize2 + 3u));
    OS_REQUIRE(st_noncached.allocationCount == 1u);

    // 8. ApuFree restores state and invalidates addresses
    OS_REQUIRE(p_free(va1) == ACPHAL_S_OK);
    OS_REQUIRE(p_free(va1) == ACPHAL_E_INVALIDARG);  // double-free rejected
    OS_REQUIRE(p_is_valid(va1, 16u) == 0);
    OS_REQUIRE(p_map_va(va1) == APU_INVALID_ADDRESS);
    OS_REQUIRE(p_map_apu(apu1) == nullptr);

    OS_REQUIRE(p_free(va2) == ACPHAL_S_OK);
    OS_REQUIRE(p_get_state(&st_cached, APU_ALLOC_CACHED) == ACPHAL_S_OK);
    OS_REQUIRE(st_cached.bytesAllocated == 0u && st_cached.bytesFree == kCachedCap &&
               st_cached.allocationCount == 0u);
    OS_REQUIRE(p_get_state(&st_noncached, APU_ALLOC_NONCACHED) == ACPHAL_S_OK);
    OS_REQUIRE(st_noncached.bytesAllocated == 0u && st_noncached.bytesLost == 0u &&
               st_noncached.bytesFree == kNonCachedCap && st_noncached.allocationCount == 0u);

    ::FreeLibrary(mod);
    std::printf("  PASS: AcpHal.dll (7/7 Apu* exports, alignment, bounds, VA<->APU mapping, heap stats).\n");
}
#endif

}  // namespace

int main() {
#if defined(_WIN32)
    TestXApuDll();
    TestAcpHalApuHeap();
#endif
    std::printf("ALL MINE XAPU + ACPHAL TESTS PASSED\n");
    return 0;
}
