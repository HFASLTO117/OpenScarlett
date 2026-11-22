#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

// Standard HRESULT returned by dynamic missing-import trampolines in First-Boot Probe Mode.
inline constexpr uint32_t kProbeNotImplHResult = 0x80004001u;  // E_NOTIMPL

// Controlled probe exception code ("OS\x01" with customer + error bits) used for safe VEH testing.
inline constexpr uint32_t kOpenScarlettTestVehException = 0xE04F5301u;

// Fixed capacity of the thread-safe recent API/D3D12 call ring buffer.
inline constexpr size_t kProbeApiRingCapacity = 50u;

struct ProbeApiCallRecord {
    uint64_t sequence = 0;
    uint64_t timestamp_qpc = 0;
    uint32_t thread_id = 0;
    std::string module_name;
    std::string symbol_name;
    uint64_t arg0 = 0;
    int32_t hr = 0;
};

struct ProbeMissingImportRecord {
    std::string dll_name;
    std::string symbol_name;
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    uint64_t stub_address = 0;
    uint64_t call_count = 0;
};

struct ProbeX64Registers {
    uint64_t rip = 0;
    uint64_t rsp = 0;
    uint64_t rbp = 0;
    uint64_t rax = 0;
    uint64_t rbx = 0;
    uint64_t rcx = 0;
    uint64_t rdx = 0;
    uint64_t rsi = 0;
    uint64_t rdi = 0;
    uint64_t r8 = 0;
    uint64_t r9 = 0;
    uint64_t r10 = 0;
    uint64_t r11 = 0;
    uint64_t r12 = 0;
    uint64_t r13 = 0;
    uint64_t r14 = 0;
    uint64_t r15 = 0;
    uint32_t eflags = 0;
};

struct ProbeStackFrame {
    uint64_t address = 0;
    std::string module_name;  // Basename only (no full host paths)
    uint64_t module_rva = 0;
};

struct ProbeExceptionReport {
    bool captured = false;
    uint32_t code = 0;
    uint32_t flags = 0;
    uint64_t fault_address = 0;
    std::string fault_module;  // Basename only
    uint64_t fault_rva = 0;
    uint32_t thread_id = 0;
    ProbeX64Registers registers{};
    std::vector<ProbeStackFrame> stack_frames;
    std::vector<ProbeApiCallRecord> api_ring_last_50;
};

struct ProbePePatchSummary {
    uint32_t total_imports_scanned = 0;
    uint32_t unresolved_dlls = 0;
    uint32_t patched_slots = 0;
    std::vector<ProbeMissingImportRecord> missing_imports;
};

struct BringUpTelemetrySnapshot {
    bool probe_mode = false;
    std::string host_os;
    std::vector<ProbeMissingImportRecord> missing_imports;
    std::vector<ProbeApiCallRecord> api_ring_last_50;
    ProbeExceptionReport exception_report;
};

// Returns true when OPENSCARLETT_PROBE_UNKNOWN_IMPORTS is set to 1 / true / yes.
bool IsProbeUnknownImportsEnabledByEnv();

// Thread-safe 50-entry API / D3D12 call ring buffer.
void RecordProbeApiCall(std::string_view module_name,
                        std::string_view symbol_name,
                        uint64_t arg0 = 0,
                        int32_t hr = 0);
std::vector<ProbeApiCallRecord> SnapshotProbeApiRing();
void ResetProbeApiRing();

// Dynamic executable x64 trampoline pool for unresolved PE imports in First-Boot Probe Mode.
// Each generated stub logs (dll_name, symbol_name) into the probe ring buffer & CallTelemetry
// (not_impl++), preserves the Windows x64 ABI cleanly, and returns E_NOTIMPL (0x80004001).
class MissingImportTrampolinePool {
public:
    MissingImportTrampolinePool();
    ~MissingImportTrampolinePool();

    MissingImportTrampolinePool(const MissingImportTrampolinePool&) = delete;
    MissingImportTrampolinePool& operator=(const MissingImportTrampolinePool&) = delete;

    // Allocates (or returns an existing) executable x64 stub for (dll_name, symbol_name, ordinal).
    void* GetOrCreateTrampoline(std::string_view dll_name,
                                std::string_view symbol_name,
                                uint16_t ordinal = 0,
                                bool by_ordinal = false);

    // Returns a deterministic snapshot of all allocated missing-import stubs and call counts.
    std::vector<ProbeMissingImportRecord> SnapshotMissingImports() const;

    // Releases all executable trampoline pages and clears records.
    void Reset();

    size_t Size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Custom resolver callback for PE import scanning/patching.
// Returning nullptr indicates the import is unresolved and should receive a probe trampoline.
using ProbeImportResolver = std::function<void*(const std::string& dll_name,
                                                const std::string& symbol_name,
                                                uint16_t ordinal,
                                                bool by_ordinal)>;

// Walks an in-memory mapped PE32+ image's IMAGE_DIRECTORY_ENTRY_IMPORT and replaces any
// unresolved IAT slots with dynamic E_NOTIMPL trampolines from `pool`.
Status PatchPeImportsWithProbeTrampolines(void* module_base,
                                          MissingImportTrampolinePool& pool,
                                          ProbePePatchSummary* out_summary = nullptr,
                                          const ProbeImportResolver& resolver = {});

// Scans a PE32+ file buffer (on-disk layout) for imports that fail resolution via `resolver`
// (or live host modules when `resolver` is empty) without modifying memory.
Status ScanPeBytesForMissingImports(const std::vector<uint8_t>& pe_bytes,
                                    const std::string& label,
                                    std::vector<ProbeMissingImportRecord>& out_missing,
                                    const ProbeImportResolver& resolver = {});

// Win32 Vectored Exception Handler (VEH) crash & bring-up monitor.
// Portable across MSVC, Clang-CL, and MinGW-w64 (no __try/__except or dbghelp.dll dependency).
class VehCrashMonitor {
public:
    // Optional hook invoked after capturing an exception. Return true to signal
    // EXCEPTION_CONTINUE_EXECUTION (e.g., controlled test recovery); false continues search.
    // `exception_pointers` is a Win32 PEXCEPTION_POINTERS (opaque void* for header hygiene).
    using RecoveryHook = std::function<bool(const ProbeExceptionReport& report,
                                            void* exception_pointers)>;

    VehCrashMonitor();
    ~VehCrashMonitor();

    VehCrashMonitor(const VehCrashMonitor&) = delete;
    VehCrashMonitor& operator=(const VehCrashMonitor&) = delete;

    // Installs the Win32 VEH via AddVectoredExceptionHandler(first_handler, ...).
    Status Install(uint32_t first_handler = 1);

    // Uninstalls the Win32 VEH via RemoveVectoredExceptionHandler.
    void Uninstall();

    bool IsInstalled() const noexcept;

    // Optionally links a trampoline pool so auto-exported JSON includes missing imports.
    void SetTrampolinePool(const MissingImportTrampolinePool* pool);

    // Optionally sets a file path where sanitized bring-up JSON is auto-written upon capture.
    void SetAutoExportPath(std::string json_path, std::vector<std::string> extra_local_roots = {});

    // Installs a recovery hook for controlled exception tests.
    void SetRecoveryHook(RecoveryHook hook);

    // Returns the most recently captured exception report.
    ProbeExceptionReport GetLastReport() const;

    // Clears any previously captured report.
    void ClearLastReport();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Strips Windows usernames (<Drive>:\Users\<USER>), %USERPROFILE%, %LOCALAPPDATA%, %APPDATA%,
// %TEMP%, %TMP%, and any caller-supplied local repo/bundle root paths so zero PII leaks.
std::string SanitizeProbeText(std::string_view text,
                              const std::vector<std::string>& extra_local_roots = {});

// Returns a concise host OS string (e.g. "Windows 10 x64 (Build 19045)") with zero PII.
std::string GetProbeHostOsSummary();

// Exports a deterministic, privacy-sanitized Bring-Up Telemetry JSON string.
std::string ExportSanitizedBringUpTelemetryJson(
    const BringUpTelemetrySnapshot& snapshot,
    const std::vector<std::string>& extra_local_roots = {});

// Writes the sanitized Bring-Up Telemetry JSON report to disk.
Status WriteSanitizedBringUpTelemetryJson(
    const std::string& file_path,
    const BringUpTelemetrySnapshot& snapshot,
    const std::vector<std::string>& extra_local_roots = {});

}  // namespace openscarlett
