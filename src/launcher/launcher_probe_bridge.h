#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include "openscarlett/launcher/hwtest_runner.h"

namespace openscarlett::launcher {

// Launches a target via openscarlett.exe run with optional First-Boot Probe Mode
// (--probe-unknown-imports + OPENSCARLETT_PROBE_UNKNOWN_IMPORTS=1).
LauncherRunResult LaunchTargetWithProbeOption(
    const std::filesystem::path& bundle_dir,
    const LauncherRunRequest& req,
    bool probe_unknown_imports,
    const std::function<void(const std::string&)>& on_log_line = {},
    const std::atomic<bool>* cancel_flag = nullptr);

// Loads the latest telemetry / bring-up JSON (checking preferred_telemetry_path,
// %TEMP%\openscarlett_launcher_telemetry, and bundle_dir), runs SanitizeReportText
// to strip usernames/local paths, and returns the sanitized JSON ready for the clipboard.
// Falls back to a sanitized JSON wrapper around `fallback_launch_log` when no telemetry
// file exists yet.
bool BuildSanitizedLatestTelemetryForClipboard(
    const std::filesystem::path& bundle_dir,
    const std::filesystem::path& preferred_telemetry_path,
    const std::string& fallback_launch_log,
    std::string& out_sanitized_text,
    std::string& out_source_label);

}  // namespace openscarlett::launcher
