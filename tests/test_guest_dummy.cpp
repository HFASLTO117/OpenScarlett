#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using SxHString = void*;
using PfnRoInitialize = int32_t(__cdecl*)(uint32_t);
using PfnRoUninitialize = void(__cdecl*)();
using PfnWindowsCreateString = int32_t(__cdecl*)(const uint16_t*, uint32_t, SxHString*);
using PfnWindowsGetStringLen = uint32_t(__cdecl*)(SxHString);
using PfnWindowsGetStringRawBuffer = const uint16_t*(__cdecl*)(SxHString, uint32_t*);
using PfnWindowsDeleteString = int32_t(__cdecl*)(SxHString);

int Fail(const char* msg, DWORD err = 0) {
    std::fprintf(stderr, "[test_guest_dummy] FAIL: %s (err=%lu)\n", msg, static_cast<unsigned long>(err));
    return 1;
}

bool ReadWholeFileW(const wchar_t* path, std::string& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    char buf[512]{};
    DWORD read = 0;
    BOOL ok = ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    ::CloseHandle(h);
    if (!ok) {
        return false;
    }
    out.assign(buf, read);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool use_exit_process = false;
    // Optional guest argument verification when launched via `openscarlett run ... -- --guest-arg-check`
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--guest-arg-check") == 0) {
            std::printf("[test_guest_dummy] Received guest arg: %s\n", argv[i]);
        } else if (std::strcmp(argv[i], "--use-exit-process") == 0) {
            use_exit_process = true;
        }
    }

    // 1. Dynamically load shimmed Xbox WinRT DLL via hooked LoadLibraryA (no DLLs exist in our exe dir!)
    HMODULE h_winrt = ::LoadLibraryA("winrt_xs.dll");
    if (!h_winrt) {
        return Fail("LoadLibraryA(\"winrt_xs.dll\") returned null", ::GetLastError());
    }

    // Verify case-insensitive extensionless LoadLibraryExW redirection & GetModuleHandle* hooks
    HMODULE h_winrt_ex = ::LoadLibraryExW(L"WINRT_XS", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h_winrt_ex != h_winrt) {
        return Fail("LoadLibraryExW(L\"WINRT_XS\") did not match LoadLibraryA handle", ::GetLastError());
    }
    ::FreeLibrary(h_winrt_ex);

    if (::GetModuleHandleA("winrt_xs.dll") != h_winrt) {
        return Fail("GetModuleHandleA(\"winrt_xs.dll\") did not match LoadLibraryA handle");
    }
    if (::GetModuleHandleW(L"WINRT_XS") != h_winrt) {
        return Fail("GetModuleHandleW(L\"WINRT_XS\") did not match LoadLibraryA handle");
    }
    HMODULE h_mod_ex = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"winrt_xs.dll", &h_mod_ex) ||
        h_mod_ex != h_winrt) {
        return Fail("GetModuleHandleExW(winrt_xs.dll) failed");
    }
    // Verify FROM_ADDRESS flag is forwarded safely without treating pointer as string
    HMODULE h_self = nullptr;
    if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(&Fail),
                              &h_self) ||
        h_self != ::GetModuleHandleA(nullptr)) {
        return Fail("GetModuleHandleExA(FROM_ADDRESS) failed");
    }

    auto p_ro_init = reinterpret_cast<PfnRoInitialize>(::GetProcAddress(h_winrt, "RoInitialize"));
    auto p_ro_uninit = reinterpret_cast<PfnRoUninitialize>(::GetProcAddress(h_winrt, "RoUninitialize"));
    auto p_create_str = reinterpret_cast<PfnWindowsCreateString>(
        ::GetProcAddress(h_winrt, "WindowsCreateString"));
    auto p_get_len = reinterpret_cast<PfnWindowsGetStringLen>(
        ::GetProcAddress(h_winrt, "WindowsGetStringLen"));
    auto p_get_raw = reinterpret_cast<PfnWindowsGetStringRawBuffer>(
        ::GetProcAddress(h_winrt, "WindowsGetStringRawBuffer"));
    auto p_del_str = reinterpret_cast<PfnWindowsDeleteString>(
        ::GetProcAddress(h_winrt, "WindowsDeleteString"));

    if (!p_ro_init || !p_ro_uninit || !p_create_str || !p_get_len || !p_get_raw || !p_del_str) {
        return Fail("Failed to resolve WinRT exports from redirected winrt_xs.dll");
    }

    if (p_ro_init(1 /* RO_INIT_MULTITHREADED */) != 0) {
        return Fail("RoInitialize failed");
    }

    const uint16_t text[] = {
        'O', 'p', 'e', 'n', 'S', 'c', 'a', 'r', 'l', 'e', 't', 't', 'A', 'P', 'C', 0};
    SxHString hstr = nullptr;
    if (p_create_str(text, 15, &hstr) != 0 || hstr == nullptr) {
        return Fail("WindowsCreateString failed");
    }
    if (p_get_len(hstr) != 15) {
        return Fail("WindowsGetStringLen mismatch");
    }
    uint32_t raw_len = 0;
    const uint16_t* raw = p_get_raw(hstr, &raw_len);
    if (!raw || raw_len != 15 || std::memcmp(raw, text, 15 * sizeof(uint16_t)) != 0) {
        return Fail("WindowsGetStringRawBuffer payload mismatch");
    }
    p_del_str(hstr);
    p_ro_uninit();

    // 2. Probe a missing export to verify NotImplemented blocker telemetry recording
    FARPROC missing = ::GetProcAddress(h_winrt, "NonExistentXboxExport_TelemetryProbe");
    if (missing != nullptr) {
        return Fail("Unexpectedly resolved NonExistentXboxExport_TelemetryProbe");
    }

    // 3. Read game package fixture from virtual G:\ drive
    DWORD g_attrs = ::GetFileAttributesW(L"G:\\game_asset.txt");
    if (g_attrs == INVALID_FILE_ATTRIBUTES) {
        return Fail("GetFileAttributesW(G:\\game_asset.txt) failed", ::GetLastError());
    }
    DWORD g_attrs_a = ::GetFileAttributesA("g:/game_asset.txt");
    if (g_attrs_a == INVALID_FILE_ATTRIBUTES) {
        return Fail("GetFileAttributesA(g:/game_asset.txt) failed", ::GetLastError());
    }
    WIN32_FIND_DATAW find_data{};
    HANDLE h_find = ::FindFirstFileW(L"G:\\*.txt", &find_data);
    if (h_find == INVALID_HANDLE_VALUE) {
        return Fail("FindFirstFileW(G:\\*.txt) failed", ::GetLastError());
    }
    ::FindClose(h_find);

    std::string g_content;
    if (!ReadWholeFileW(L"G:\\game_asset.txt", g_content) ||
        g_content != "OPENSCARLETT_GAME_ASSET_V1") {
        return Fail("Read G:\\game_asset.txt failed or content mismatch");
    }

    // 4. Write & verify scratch file on virtual T:\ drive (via CreateFileA + CreateDirectoryW + GetFileAttributesExW)
    const char* scratch_payload = "SCRATCH_OK_42";
    HANDLE h_t = ::CreateFileA("T:\\scratch_check.txt",
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (h_t == INVALID_HANDLE_VALUE) {
        return Fail("CreateFileA(T:\\scratch_check.txt) failed", ::GetLastError());
    }
    DWORD written = 0;
    if (!::WriteFile(h_t, scratch_payload, static_cast<DWORD>(std::strlen(scratch_payload)), &written, nullptr) ||
        written != std::strlen(scratch_payload)) {
        ::CloseHandle(h_t);
        return Fail("WriteFile(T:\\scratch_check.txt) failed", ::GetLastError());
    }
    ::CloseHandle(h_t);

    WIN32_FILE_ATTRIBUTE_DATA t_attr_ex{};
    if (!::GetFileAttributesExW(L"T:\\scratch_check.txt", GetFileExInfoStandard, &t_attr_ex)) {
        return Fail("GetFileAttributesExW(T:\\scratch_check.txt) failed", ::GetLastError());
    }

    if (!::CreateDirectoryW(L"T:\\cache_dir", nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        return Fail("CreateDirectoryW(T:\\cache_dir) failed", ::GetLastError());
    }

    std::string t_verify;
    if (!ReadWholeFileW(L"T:\\scratch_check.txt", t_verify) || t_verify != scratch_payload) {
        return Fail("Verify T:\\scratch_check.txt content mismatch");
    }

    // 5. Write & verify persistent save file on virtual S:\ drive
    const char* save_payload = "SAVE_SLOT0_PAYLOAD";
    HANDLE h_s = ::CreateFileW(L"S:\\slot0.sav",
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (h_s == INVALID_HANDLE_VALUE) {
        return Fail("CreateFileW(S:\\slot0.sav) failed", ::GetLastError());
    }
    written = 0;
    if (!::WriteFile(h_s, save_payload, static_cast<DWORD>(std::strlen(save_payload)), &written, nullptr) ||
        written != std::strlen(save_payload)) {
        ::CloseHandle(h_s);
        return Fail("WriteFile(S:\\slot0.sav) failed", ::GetLastError());
    }
    ::CloseHandle(h_s);

    std::string s_verify;
    if (!ReadWholeFileW(L"S:\\slot0.sav", s_verify) || s_verify != save_payload) {
        return Fail("Verify S:\\slot0.sav content mismatch");
    }

    // 6. Verify path traversal escape above virtual drive root is blocked with ERROR_ACCESS_DENIED
    HANDLE h_escape = ::CreateFileW(L"G:\\..\\escape_attempt.txt",
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (h_escape != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_ACCESS_DENIED) {
        if (h_escape != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h_escape);
        }
        return Fail("VFS path traversal escape was not blocked with ERROR_ACCESS_DENIED");
    }

    ::FreeLibrary(h_winrt);
    std::printf("[test_guest_dummy] PASS: winrt_xs redirected + G:/T:/S: VFS + traversal guard verified\n");
    std::fflush(stdout);
    if (use_exit_process) {
        ::ExitProcess(0);
    }
    return 0;
}
