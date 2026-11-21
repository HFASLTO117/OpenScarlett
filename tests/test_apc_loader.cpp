#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/loader/apc_injector.h"
#include "openscarlett/loader/vfs_mounts.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

void WriteTextFile(const fs::path& p, const std::string& data) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(static_cast<bool>(ofs));
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    OS_REQUIRE(static_cast<bool>(ofs));
}

std::vector<std::string> ListDirFilenames(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void VerifyTelemetryJson(const fs::path& json_path) {
    OS_REQUIRE(fs::exists(json_path));
    std::string content = ReadTextFile(json_path);

    // Redirected Xbox DLL load entry
    OS_REQUIRE(content.find("\"module\": \"winrt_xs.dll\"") != std::string::npos);
    OS_REQUIRE(content.find("\"symbol\": \"LoadLibraryA\"") != std::string::npos);

    // Resolved WinRT shim symbols queried via GetProcAddress
    OS_REQUIRE(content.find("\"symbol\": \"RoInitialize\"") != std::string::npos);
    OS_REQUIRE(content.find("\"symbol\": \"WindowsCreateString\"") != std::string::npos);

    // Missing export blocker recorded with not_impl >= 1
    size_t probe_pos = content.find("\"symbol\": \"NonExistentXboxExport_TelemetryProbe\"");
    OS_REQUIRE(probe_pos != std::string::npos);
    size_t line_end = content.find('\n', probe_pos);
    std::string probe_line = content.substr(probe_pos, line_end - probe_pos);
    OS_REQUIRE(probe_line.find("\"not_impl\": 1") != std::string::npos);

    // VFS I/O redirection telemetry entries
    OS_REQUIRE(content.find("\"module\": \"vfs\"") != std::string::npos);
    OS_REQUIRE(content.find("CreateFileW:traversal_blocked") != std::string::npos);
}

void TestVfsMountsUnit() {
    using namespace openscarlett;

    const fs::path unit_root = fs::path(R"(E:\OpenScarlett\.tmp\sprint1-loader\vfs_unit)");
    std::error_code ec;
    fs::remove_all(unit_root, ec);

    const fs::path g_dir = unit_root / "game_pkg";
    const fs::path t_dir = unit_root / "scratch_T";
    const fs::path s_dir = unit_root / "saves_S";
    fs::create_directories(g_dir, ec);
    OS_REQUIRE(fs::exists(g_dir));
    OS_REQUIRE(!fs::exists(t_dir));
    OS_REQUIRE(!fs::exists(s_dir));

    VfsMounts vfs;
    vfs.Configure(VfsMountConfig{g_dir.wstring(), t_dir.wstring(), s_dir.wstring()});

    // 1. Case-insensitivity & slash normalization
    std::wstring out;
    OS_REQUIRE(vfs.TranslateW(L"G:\\assets\\hero.dds", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == (g_dir / L"assets\\hero.dds").wstring());

    OS_REQUIRE(vfs.TranslateW(L"g:/assets//./textures/hero.dds", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == (g_dir / L"assets\\textures\\hero.dds").wstring());

    // 2. Extended-length \\?\ prefix support
    OS_REQUIRE(vfs.TranslateW(L"\\\\?\\G:\\dir\\sub\\..\\file.bin", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == L"\\\\?\\" + (g_dir / L"dir\\file.bin").wstring());

    // 3. Auto-creation of T:\ and S:\ backing directories on demand
    OS_REQUIRE(vfs.TranslateW(L"t:/shader_cache/pso.bin", out, true) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == (t_dir / L"shader_cache\\pso.bin").wstring());
    OS_REQUIRE(fs::exists(t_dir));

    OS_REQUIRE(vfs.TranslateW(L"S:\\profile\\slot0.sav", out, true) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == (s_dir / L"profile\\slot0.sav").wstring());
    OS_REQUIRE(fs::exists(s_dir));

    // 4. Drive root access (T:\, G:, S:/)
    OS_REQUIRE(vfs.TranslateW(L"T:\\", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == t_dir.wstring());
    OS_REQUIRE(vfs.TranslateW(L"G:", out) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out == g_dir.wstring());

    // 5. Path traversal escape rejection
    OS_REQUIRE(vfs.TranslateW(L"G:\\..\\secret.txt", out) == VfsTranslateStatus::RejectedTraversal);
    OS_REQUIRE(vfs.TranslateW(L"T:\\sub\\..\\..\\escape.txt", out) == VfsTranslateStatus::RejectedTraversal);
    OS_REQUIRE(vfs.TranslateW(L"\\\\?\\S:\\..\\escape.sav", out) == VfsTranslateStatus::RejectedTraversal);
    OS_REQUIRE(vfs.TranslateW(L"g:../escape.txt", out) == VfsTranslateStatus::RejectedTraversal);
    OS_REQUIRE(vfs.TranslateW(L"G:\\C:\\Windows\\win.ini", out) == VfsTranslateStatus::RejectedTraversal);

    // 6. Non-virtual paths pass through untouched
    OS_REQUIRE(vfs.TranslateW(L"E:\\OpenScarlett\\README.md", out) == VfsTranslateStatus::NotVirtual);
    OS_REQUIRE(vfs.TranslateW(L"C:\\Windows\\System32\\kernel32.dll", out) == VfsTranslateStatus::NotVirtual);
    OS_REQUIRE(vfs.TranslateW(L"relative\\local.txt", out) == VfsTranslateStatus::NotVirtual);

    // 7. UTF-8 helper
    std::string out_u8;
    OS_REQUIRE(vfs.TranslateUtf8("s:/slot1.sav", out_u8) == VfsTranslateStatus::Translated);
    OS_REQUIRE(out_u8 == (s_dir / "slot1.sav").string());

    std::printf("[PASS] VfsMounts unit suite (case/slash/extended/auto-create/traversal)\n");
}

void TestLiveApcInjectionAndCli() {
    using namespace openscarlett;

    const fs::path build_dir = fs::path(OPENSCARLETT_BINARY_DIR);
    const fs::path src_guest_exe = build_dir / "test_guest_dummy.exe";
    const fs::path cli_exe = build_dir / "openscarlett.exe";
    OS_REQUIRE(fs::exists(src_guest_exe));
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(build_dir / "openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(build_dir / "winrt_xs.dll"));

    // Prepare completely isolated guest folder on E:\ with ZERO shim DLLs inside it.
    const fs::path sandbox_root = fs::path(R"(E:\OpenScarlett\.tmp\sprint1-loader\apc_e2e)");
    std::error_code ec;
    fs::remove_all(sandbox_root, ec);

    const fs::path isolated_guest_dir = sandbox_root / "isolated_guest";
    fs::create_directories(isolated_guest_dir, ec);
    OS_REQUIRE(fs::exists(isolated_guest_dir));

    const fs::path isolated_guest_exe = isolated_guest_dir / "test_guest_dummy.exe";
    fs::copy_file(src_guest_exe, isolated_guest_exe, fs::copy_options::overwrite_existing);
    WriteTextFile(isolated_guest_dir / "game_asset.txt", "OPENSCARLETT_GAME_ASSET_V1");

    const std::vector<std::string> expected_guest_dir_files = {"game_asset.txt", "test_guest_dummy.exe"};
    OS_REQUIRE(ListDirFilenames(isolated_guest_dir) == expected_guest_dir_files);

    // --- Part A: Direct LaunchWithApcHook(...) ---
    const fs::path vfs_t_api = sandbox_root / "vfs_api_T";
    const fs::path vfs_s_api = sandbox_root / "vfs_api_S";
    const fs::path tel_api = sandbox_root / "telemetry_api.json";

    LaunchOptions opts{};
    opts.exe_path = isolated_guest_exe.string();
    opts.shim_dir = build_dir.string();
    opts.vfs_g = isolated_guest_dir.string();
    opts.vfs_t = vfs_t_api.string();
    opts.vfs_s = vfs_s_api.string();
    opts.telemetry_output_path = tel_api.string();
    opts.timeout_ms = 30000;
    opts.inherit_console = true;

    LaunchResult res{};
    Status st = LaunchWithApcHook(opts, res);
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.process_id != 0);
    OS_REQUIRE(!res.timed_out);
    OS_REQUIRE(res.exit_code == 0);

    // Prove zero files were copied or modified in the isolated guest directory!
    OS_REQUIRE(ListDirFilenames(isolated_guest_dir) == expected_guest_dir_files);

    // Verify virtual T:\ and S:\ writes landed in their E:\ backing directories
    OS_REQUIRE(fs::exists(vfs_t_api / "scratch_check.txt"));
    OS_REQUIRE(ReadTextFile(vfs_t_api / "scratch_check.txt") == "SCRATCH_OK_42");
    OS_REQUIRE(fs::is_directory(vfs_t_api / "cache_dir"));
    OS_REQUIRE(fs::exists(vfs_s_api / "slot0.sav"));
    OS_REQUIRE(ReadTextFile(vfs_s_api / "slot0.sav") == "SAVE_SLOT0_PAYLOAD");

    // Verify unified CallTelemetry JSON report
    VerifyTelemetryJson(tel_api);
    std::printf("[PASS] LaunchWithApcHook live suspended-process injection + VFS + telemetry\n");

    // --- Part B: End-to-End CLI `openscarlett.exe run ...` ---
    const fs::path vfs_t_cli = sandbox_root / "vfs_cli_T";
    const fs::path vfs_s_cli = sandbox_root / "vfs_cli_S";
    const fs::path tel_cli = sandbox_root / "telemetry_cli.json";

    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + isolated_guest_exe.wstring() +
        L"\" --shim-dir \"" + build_dir.wstring() +
        L"\" --vfs-g \"" + isolated_guest_dir.wstring() +
        L"\" --vfs-t \"" + vfs_t_cli.wstring() +
        L"\" --vfs-s \"" + vfs_s_cli.wstring() +
        L"\" --telemetry \"" + tel_cli.wstring() +
        L"\" --timeout-ms 30000 -- --guest-arg-check --use-exit-process";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    OS_REQUIRE(::CreateProcessW(cli_exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                0, nullptr, nullptr, &si, &pi) != 0);
    OS_REQUIRE(::WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0);
    DWORD cli_exit = 999;
    OS_REQUIRE(::GetExitCodeProcess(pi.hProcess, &cli_exit) != 0);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    OS_REQUIRE(cli_exit == 0);

    OS_REQUIRE(ListDirFilenames(isolated_guest_dir) == expected_guest_dir_files);
    OS_REQUIRE(ReadTextFile(vfs_t_cli / "scratch_check.txt") == "SCRATCH_OK_42");
    OS_REQUIRE(ReadTextFile(vfs_s_cli / "slot0.sav") == "SAVE_SLOT0_PAYLOAD");
    VerifyTelemetryJson(tel_cli);
    std::printf("[PASS] openscarlett.exe run CLI end-to-end verification\n");
}

void TestCrossDllTelemetryForwarding() {
    using namespace openscarlett;

    const fs::path build_dir = fs::path(OPENSCARLETT_BINARY_DIR);
    const fs::path hook_dll = build_dir / "openscarlett_hook.dll";
    const fs::path forward_json = fs::path(R"(E:\OpenScarlett\.tmp\sprint1-loader\apc_e2e\telemetry_forward.json)");
    std::error_code ec;
    fs::remove(forward_json, ec);

    OS_REQUIRE(::SetEnvironmentVariableW(L"OPENSCARLETT_TELEMETRY_PATH", forward_json.c_str()) != 0);

    HMODULE h_hook = ::LoadLibraryW(hook_dll.c_str());
    OS_REQUIRE(h_hook != nullptr);

    using PfnFlush = int(__cdecl*)();
    auto p_flush = reinterpret_cast<PfnFlush>(::GetProcAddress(h_hook, "OpenScarlettHook_FlushTelemetry"));
    OS_REQUIRE(p_flush != nullptr);

    // CallTelemetry::Global().Record() in openscarlett_core automatically forwards to OpenScarlettHook_RecordCall!
    CallTelemetry::Global().Record("winrt_xs.dll", "CrossDllForwarded_RoActivate", CallOutcome::StubOk, 0, 7);
    OS_REQUIRE(p_flush() == 1);

    OS_REQUIRE(fs::exists(forward_json));
    std::string content = ReadTextFile(forward_json);
    OS_REQUIRE(content.find("\"module\": \"winrt_xs.dll\"") != std::string::npos);
    OS_REQUIRE(content.find("\"symbol\": \"CrossDllForwarded_RoActivate\"") != std::string::npos);
    OS_REQUIRE(content.find("\"slot\": 7") != std::string::npos);
    OS_REQUIRE(content.find("\"stub\": 1") != std::string::npos);

    std::printf("[PASS] Cross-DLL CallTelemetry forwarding -> OpenScarlettHook_RecordCall verified\n");
}

}  // namespace

int main() {
    TestVfsMountsUnit();
    TestLiveApcInjectionAndCli();
    TestCrossDllTelemetryForwarding();
    std::printf("[PASS] test_apc_loader (all VFS, APC loader, CLI, and telemetry checks succeeded)\n");
    return 0;
}
