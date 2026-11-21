#pragma once

#include <cstdint>
#include <string>

#include "openscarlett/core/status.h"

namespace openscarlett {

struct LaunchOptions {
    std::string exe_path;               // Package dir, MicrosoftGame.Config/.mgc, or direct .exe
    std::string arguments;              // Optional guest arguments (excluding argv[0])
    std::string working_dir;            // Defaults to target exe's parent directory
    std::string shim_dir;               // Defaults to directory of current openscarlett binary
    std::string hook_dll_path;          // Defaults to <shim_dir>\openscarlett_hook.dll
    std::string vfs_g;                  // Defaults to package root / target exe's parent directory
    std::string vfs_t;                  // Defaults to %TEMP%\vfs\T or E:\OpenScarlett\.tmp\vfs\T
    std::string vfs_s;                  // Defaults to <OPENSCARLETT_SAVE_ROOT>[\<TitleId>]
    std::string telemetry_output_path;  // Optional JSON path for unified CallTelemetry output
    uint32_t timeout_ms = 0;            // 0 = wait indefinitely (INFINITE)
    bool inherit_console = true;
    std::string vfs_g_overlay;          // Defaults to %TEMP%\vfs\G_overlay[\<TitleId>]
    std::string title_id;               // Optional override (auto-populated from MicrosoftGame.Config)
    std::string package_id;             // Optional override (auto-populated from MicrosoftGame.Config)
};

struct LaunchResult {
    uint32_t process_id = 0;
    uint32_t exit_code = 0;
    bool timed_out = false;
    std::string resolved_shim_dir;
    std::string resolved_hook_dll;
    std::string resolved_vfs_g;
    std::string resolved_vfs_t;
    std::string resolved_vfs_s;
    std::string resolved_telemetry_path;
    std::string resolved_exe_path;
    std::string resolved_config_path;
    std::string resolved_vfs_g_overlay;
    std::string resolved_title_id;
    std::string resolved_package_id;
};

// Launches an unmodified x64 Windows PE executable (or GDK package directory / MicrosoftGame.Config)
// in a suspended state, prepends shim_dir to PATH, configures OpenScarlett VFS CoW overlay, TitleId,
// and telemetry environment variables, applies data-driven static import routing, queues a user-mode
// APC invoking LoadLibraryW(openscarlett_hook.dll) before main() executes, resumes, and waits.
Status LaunchWithApcHook(const LaunchOptions& options, LaunchResult& out_result);

}  // namespace openscarlett
