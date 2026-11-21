#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

// ---------------------------------------------------------------------------
// Topological DLL Dependency Initialization Ordering (Kahn's Algorithm)
// ---------------------------------------------------------------------------
struct DllDependencyNode {
    std::string name;                  // Display/canonical DLL name (e.g. "test_dep_top.dll")
    std::string file_path;             // Optional resolved path on disk
    int load_priority = 10;            // Tie-breaker priority (lower loads earlier)
    std::vector<std::string> imports;  // DLL names imported by this node
};

struct DllInitOrderResult {
    std::vector<std::string> ordered_names;       // Dependency-first order (dependencies before importers)
    std::vector<size_t> ordered_indices;          // Indices into input nodes vector
    std::vector<std::string> cycle_broken_names;  // Nodes where a cycle edge was deterministically broken
    bool had_cycles = false;
};

// Deterministic Kahn's topological sort over a DLL import dependency graph.
// Dependencies always precede their importers; ties are broken by (load_priority, original_index).
// Cycles are broken deterministically without hanging while preserving all DAG constraints.
DllInitOrderResult ComputeDllInitOrder(const std::vector<DllDependencyNode>& nodes);

// Inspects PE import tables of the given DLL files on disk and computes their
// topological dependency-first initialization order.
Status ComputeDllInitOrderFromFiles(const std::vector<std::string>& dll_paths,
                                    DllInitOrderResult& out_order);

// Loads the given DLLs in strict topological dependency-first order (dependencies before importers),
// recording telemetry and optionally returning the resolved order and loaded HMODULE handles.
Status LoadDllsInTopologicalOrder(const std::vector<std::string>& dll_paths,
                                  std::vector<std::string>* out_loaded_order = nullptr,
                                  std::vector<void*>* out_handles = nullptr);

struct LaunchOptions {
    std::string exe_path;               // Package dir, MicrosoftGame.Config/.mgc, or direct .exe
    std::string arguments;              // Optional guest arguments (excluding argv[0])
    std::string working_dir;            // Defaults to target exe's parent directory
    std::string shim_dir;               // Defaults to directory of current openscarlett binary
    std::string hook_dll_path;          // Defaults to <shim_dir>\openscarlett_hook.dll
    std::string vfs_g;                  // Defaults to package root / target exe's parent directory
    std::string vfs_t;                  // Defaults to <work_dir>\vfs\T (never C:\ when work dir is writable)
    std::string vfs_s;                  // Defaults to <OPENSCARLETT_SAVE_ROOT> or <work_dir>\vfs\S[\<TitleId>]
    std::string telemetry_output_path;  // Optional JSON path for unified CallTelemetry output
    uint32_t timeout_ms = 0;            // 0 = wait indefinitely (INFINITE)
    bool inherit_console = true;
    std::string vfs_g_overlay;          // Defaults to <work_dir>\vfs\G_overlay[\<TitleId>]
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

struct StoreBootstrapDiagnostic {
    bool suspected_store_bootstrapper = false;
    bool in_windows_apps = false;
    bool in_xbox_games = false;
    bool has_game_config = false;
    bool is_gamelaunchhelper = false;
    std::string reason;
    std::string message;
};

// Pure read-only diagnostic helper detecting Microsoft Store / Xbox Play Anywhere bootstrapper
// executables that delegate launch to GamingServices and exit cleanly (code 0) with 0 tracked
// telemetry symbols.
StoreBootstrapDiagnostic DiagnoseStoreBootstrapper(const LaunchResult& result,
                                                   size_t tracked_telemetry_symbols,
                                                   bool telemetry_file_written);

// Launches an unmodified x64 Windows PE executable (or GDK package directory / MicrosoftGame.Config)
// in a suspended state, prepends shim_dir to PATH, configures OpenScarlett VFS CoW overlay, TitleId,
// and telemetry environment variables, applies data-driven static import routing, queues a user-mode
// APC invoking LoadLibraryW(openscarlett_hook.dll) before main() executes, resumes, and waits.
Status LaunchWithApcHook(const LaunchOptions& options, LaunchResult& out_result);

}  // namespace openscarlett
