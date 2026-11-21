#include "openscarlett/loader/apc_injector.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "openscarlett/loader/vfs_mounts.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace openscarlett {

namespace {

#if defined(_WIN32)

std::string FormatWin32Error(const char* action, DWORD err) {
    return std::string(action) + " failed (Win32 error " + std::to_string(err) + ")";
}

std::wstring GetCurrentExecutableDirW() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::filesystem::path p(buf);
    return p.parent_path().wstring();
}

std::wstring ResolveAbsolutePathW(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    auto abs_p = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec) {
        return path;
    }
    return abs_p.lexically_normal().wstring();
}

bool FileExistsW(const std::wstring& path) {
    DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void UpsertEnvEntry(std::vector<std::pair<std::wstring, std::wstring>>& entries,
                    const std::wstring& key,
                    const std::wstring& value) {
    for (auto& kv : entries) {
        if (::_wcsicmp(kv.first.c_str(), key.c_str()) == 0) {
            kv.second = value;
            return;
        }
    }
    entries.emplace_back(key, value);
}

void RemoveEnvEntry(std::vector<std::pair<std::wstring, std::wstring>>& entries,
                    const std::wstring& key) {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const std::pair<std::wstring, std::wstring>& kv) {
                           return ::_wcsicmp(kv.first.c_str(), key.c_str()) == 0;
                       }),
        entries.end());
}

std::vector<wchar_t> BuildChildEnvironmentBlock(const std::wstring& shim_dir,
                                                const std::wstring& vfs_g,
                                                const std::wstring& vfs_t,
                                                const std::wstring& vfs_s,
                                                const std::wstring& telemetry_path) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;

    LPWCH raw_env = ::GetEnvironmentStringsW();
    if (raw_env != nullptr) {
        const wchar_t* cur = raw_env;
        while (*cur != L'\0') {
            std::wstring entry(cur);
            cur += entry.size() + 1;
            // Drive-current-directory entries begin with '=' (e.g. "=E:=E:\dir").
            size_t eq = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
            if (eq == std::wstring::npos) {
                continue;
            }
            entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
        ::FreeEnvironmentStringsW(raw_env);
    }

    // Prepend shim_dir to PATH so static PE imports of Xbox-exclusive DLLs resolve cleanly
    // without touching the guest executable's directory.
    bool updated_path = false;
    for (auto& kv : entries) {
        if (::_wcsicmp(kv.first.c_str(), L"PATH") == 0) {
            if (kv.second.empty()) {
                kv.second = shim_dir;
            } else {
                kv.second = shim_dir + L";" + kv.second;
            }
            updated_path = true;
            break;
        }
    }
    if (!updated_path) {
        entries.emplace_back(L"PATH", shim_dir);
    }

    UpsertEnvEntry(entries, L"OPENSCARLETT_SHIM_DIR", shim_dir);
    UpsertEnvEntry(entries, L"OPENSCARLETT_VFS_G", vfs_g);
    UpsertEnvEntry(entries, L"OPENSCARLETT_VFS_T", vfs_t);
    UpsertEnvEntry(entries, L"OPENSCARLETT_VFS_S", vfs_s);
    UpsertEnvEntry(entries, L"OPENSCARLETT_SAVE_ROOT", vfs_s);

    if (!telemetry_path.empty()) {
        UpsertEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_PATH", telemetry_path);
    } else {
        RemoveEnvEntry(entries, L"OPENSCARLETT_TELEMETRY_PATH");
    }

    std::sort(entries.begin(), entries.end(),
              [](const std::pair<std::wstring, std::wstring>& a,
                 const std::pair<std::wstring, std::wstring>& b) {
                  return ::_wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
              });

    std::vector<wchar_t> block;
    block.reserve(4096);
    for (const auto& kv : entries) {
        block.insert(block.end(), kv.first.begin(), kv.first.end());
        block.push_back(L'=');
        block.insert(block.end(), kv.second.begin(), kv.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

#endif  // _WIN32

}  // namespace

Status LaunchWithApcHook(const LaunchOptions& options, LaunchResult& out_result) {
    out_result = {};

#if !defined(_WIN32)
    (void)options;
    return Status::Error(StatusCode::NotImplemented, "APC hook loader is Windows-only");
#else
    if (options.exe_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Target executable path is empty");
    }

    const std::wstring exe_w = ResolveAbsolutePathW(Utf8ToWide(options.exe_path));
    if (!FileExistsW(exe_w)) {
        return Status::Error(StatusCode::NotFound, "Target executable not found: " + WideToUtf8(exe_w));
    }

    const std::wstring exe_dir_w = std::filesystem::path(exe_w).parent_path().wstring();

    std::wstring shim_dir_w = options.shim_dir.empty()
                                  ? GetCurrentExecutableDirW()
                                  : ResolveAbsolutePathW(Utf8ToWide(options.shim_dir));
    if (shim_dir_w.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Unable to determine OpenScarlett shim directory");
    }

    std::wstring hook_dll_w = options.hook_dll_path.empty()
                                  ? (std::filesystem::path(shim_dir_w) / L"openscarlett_hook.dll").wstring()
                                  : ResolveAbsolutePathW(Utf8ToWide(options.hook_dll_path));
    if (!FileExistsW(hook_dll_w)) {
        return Status::Error(StatusCode::NotFound,
                             "openscarlett_hook.dll not found: " + WideToUtf8(hook_dll_w));
    }

    // Build default VFS mounts without touching C:\ or modifying the guest directory.
    VfsMountConfig defaults = VfsMounts::BuildDefaultConfig(exe_dir_w);
    std::wstring vfs_g_w = options.vfs_g.empty() ? defaults.g_root : ResolveAbsolutePathW(Utf8ToWide(options.vfs_g));
    std::wstring vfs_t_w = options.vfs_t.empty() ? defaults.t_root : ResolveAbsolutePathW(Utf8ToWide(options.vfs_t));
    std::wstring vfs_s_w = options.vfs_s.empty() ? defaults.s_root : ResolveAbsolutePathW(Utf8ToWide(options.vfs_s));

    std::wstring work_dir_w = options.working_dir.empty()
                                  ? exe_dir_w
                                  : ResolveAbsolutePathW(Utf8ToWide(options.working_dir));

    std::wstring telemetry_w = options.telemetry_output_path.empty()
                                   ? std::wstring{}
                                   : ResolveAbsolutePathW(Utf8ToWide(options.telemetry_output_path));

    // Pre-create writable backing directories on E:\ (never touches vfs_g / target exe directory).
    if (!vfs_t_w.empty()) {
        EnsureDirectoryTreeW(vfs_t_w);
    }
    if (!vfs_s_w.empty()) {
        EnsureDirectoryTreeW(vfs_s_w);
    }
    if (!telemetry_w.empty()) {
        std::wstring tel_parent = std::filesystem::path(telemetry_w).parent_path().wstring();
        if (!tel_parent.empty()) {
            EnsureDirectoryTreeW(tel_parent);
        }
    }

    out_result.resolved_shim_dir = WideToUtf8(shim_dir_w);
    out_result.resolved_hook_dll = WideToUtf8(hook_dll_w);
    out_result.resolved_vfs_g = WideToUtf8(vfs_g_w);
    out_result.resolved_vfs_t = WideToUtf8(vfs_t_w);
    out_result.resolved_vfs_s = WideToUtf8(vfs_s_w);
    out_result.resolved_telemetry_path = WideToUtf8(telemetry_w);

    // Build mutable command line: "<exe>" [arguments]
    std::wstring cmd_line = L"\"" + exe_w + L"\"";
    if (!options.arguments.empty()) {
        cmd_line.push_back(L' ');
        cmd_line.append(Utf8ToWide(options.arguments));
    }

    std::vector<wchar_t> env_block =
        BuildChildEnvironmentBlock(shim_dir_w, vfs_g_w, vfs_t_w, vfs_s_w, telemetry_w);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD creation_flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (!options.inherit_console) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    if (!::CreateProcessW(exe_w.c_str(),
                          cmd_line.data(),
                          nullptr,
                          nullptr,
                          options.inherit_console ? TRUE : FALSE,
                          creation_flags,
                          env_block.data(),
                          work_dir_w.empty() ? nullptr : work_dir_w.c_str(),
                          &si,
                          &pi)) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("CreateProcessW", ::GetLastError()));
    }

    out_result.process_id = static_cast<uint32_t>(pi.dwProcessId);

    auto abort_child = [&](StatusCode code, const char* step, DWORD err) -> Status {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return Status::Error(code, FormatWin32Error(step, err));
    };

    HMODULE h_kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    FARPROC p_load_library_w = h_kernel32 ? ::GetProcAddress(h_kernel32, "LoadLibraryW") : nullptr;
    if (!p_load_library_w) {
        return abort_child(StatusCode::DeviceError, "GetProcAddress(LoadLibraryW)", ::GetLastError());
    }

    const size_t path_bytes = (hook_dll_w.size() + 1) * sizeof(wchar_t);
    LPVOID remote_path = ::VirtualAllocEx(pi.hProcess,
                                          nullptr,
                                          path_bytes,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (!remote_path) {
        return abort_child(StatusCode::OutOfMemory, "VirtualAllocEx", ::GetLastError());
    }

    SIZE_T written = 0;
    if (!::WriteProcessMemory(pi.hProcess, remote_path, hook_dll_w.c_str(), path_bytes, &written) ||
        written != path_bytes) {
        DWORD err = ::GetLastError();
        ::VirtualFreeEx(pi.hProcess, remote_path, 0, MEM_RELEASE);
        return abort_child(StatusCode::IoError, "WriteProcessMemory", err);
    }

    if (::QueueUserAPC(reinterpret_cast<PAPCFUNC>(p_load_library_w),
                       pi.hThread,
                       reinterpret_cast<ULONG_PTR>(remote_path)) == 0) {
        DWORD err = ::GetLastError();
        ::VirtualFreeEx(pi.hProcess, remote_path, 0, MEM_RELEASE);
        return abort_child(StatusCode::DeviceError, "QueueUserAPC", err);
    }

    if (::ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        return abort_child(StatusCode::DeviceError, "ResumeThread", ::GetLastError());
    }

    const DWORD wait_ms = (options.timeout_ms == 0) ? INFINITE : static_cast<DWORD>(options.timeout_ms);
    const DWORD wait_rc = ::WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        out_result.timed_out = true;
        out_result.exit_code = 124;
        ::TerminateProcess(pi.hProcess, 124);
        ::WaitForSingleObject(pi.hProcess, 3000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return Status::Error(StatusCode::DeviceError,
                             "Guest process timed out after " + std::to_string(options.timeout_ms) + " ms");
    }
    if (wait_rc != WAIT_OBJECT_0) {
        DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("WaitForSingleObject", err));
    }

    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(pi.hProcess, &exit_code)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return Status::Error(StatusCode::DeviceError, FormatWin32Error("GetExitCodeProcess", err));
    }

    out_result.exit_code = static_cast<uint32_t>(exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return Status::Ok();
#endif
}

}  // namespace openscarlett
