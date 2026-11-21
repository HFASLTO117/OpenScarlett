#include <filesystem>
#include <string>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

int main(int argc, char** argv) {
    using namespace openscarlett;

    // 1. Classify known console-only, GDK, middleware, and native Windows DLLs
    OS_REQUIRE(ClassifyImportDll("d3d12_xs.dll") == DllCategory::ConsoleExclusive);
    OS_REQUIRE(ClassifyImportDll("xg_xs.dll") == DllCategory::ConsoleExclusive);
    OS_REQUIRE(ClassifyImportDll("kernelx.dll") == DllCategory::ConsoleExclusive);
    OS_REQUIRE(ClassifyImportDll("xgameruntime.dll") == DllCategory::GdkRuntime);
    OS_REQUIRE(ClassifyImportDll("GameInput.dll") == DllCategory::GdkRuntime);
    OS_REQUIRE(ClassifyImportDll("Bink2Scarlett.dll") == DllCategory::Middleware);
    OS_REQUIRE(ClassifyImportDll("kernel32.dll") == DllCategory::WindowsNative);
    OS_REQUIRE(ClassifyImportDll("api-ms-win-core-synch-l1-2-0.dll") == DllCategory::WindowsNative);

    // 2. Inspect our own compiled openscarlett.exe PE32+ image (works in CTest and portable bundles)
    std::filesystem::path exe_path;
    if (argc > 0 && argv[0] != nullptr) {
        auto beside_test = std::filesystem::path(argv[0]).parent_path() / "openscarlett.exe";
        if (std::filesystem::exists(beside_test)) {
            exe_path = beside_test;
        }
    }
    if (exe_path.empty()) {
        exe_path = std::filesystem::path(OPENSCARLETT_BINARY_DIR) / "openscarlett.exe";
    }
    OS_REQUIRE(std::filesystem::exists(exe_path));

    PeInspectionReport rep;
    auto st = InspectPeFile(exe_path.string(), rep);
    OS_REQUIRE(st.ok());
    OS_REQUIRE(rep.is_pe64);
    OS_REQUIRE(!rep.is_dll);
    OS_REQUIRE(rep.section_count >= 3);
    OS_REQUIRE(rep.total_imports > 10);
    OS_REQUIRE(rep.exception_entry_count > 0);

    // 3. Reject invalid/truncated PE bytes cleanly
    PeInspectionReport bad;
    OS_REQUIRE(!InspectPeBytes({0x00, 0x01, 0x02}, "bad.exe", bad).ok());

    // 4. Verify live CallTelemetry recording & JSON export
    auto& tel = CallTelemetry::Global();
    tel.Reset();
    tel.Record("d3d12_xs.dll", "ID3D12GraphicsCommandList::DispatchMesh", CallOutcome::NotImplemented, -2147467263, 67);
    tel.Record("xgameruntime.dll", "XUserGetId", CallOutcome::Ok, 0, 12);
    tel.Record("xgameruntime.dll", "XUserGetId", CallOutcome::Ok, 0, 12);
    auto snap = tel.Snapshot();
    OS_REQUIRE(snap.size() == 2);
    // Unimplemented slots sort first so blockers jump straight to the top
    OS_REQUIRE(snap[0].not_impl_calls == 1);
    OS_REQUIRE(snap[0].vtable_slot == 67);
    OS_REQUIRE(snap[1].ok_calls == 2);

    auto report_path = std::filesystem::temp_directory_path() / "openscarlett_telemetry_test.json";
    OS_REQUIRE(tel.WriteJsonReport(report_path.string()).ok());
    OS_REQUIRE(std::filesystem::exists(report_path));
    std::error_code ec;
    std::filesystem::remove(report_path, ec);

    std::printf("[PASS] test_pe_inspect (%u imports parsed, telemetry verified)\n", rep.total_imports);
    return 0;
}
