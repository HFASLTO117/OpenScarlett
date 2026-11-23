#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "openscarlett/loader/pe_forwarder.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

fs::path ResolveScratchDir() {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec || base.empty()) {
        base = fs::current_path(ec);
    }
    fs::path root = base / "openscarlett_test_mine_routing";
    fs::create_directories(root, ec);
    return root.make_preferred();
}

void WriteStubFile(const fs::path& path, const char* content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(ofs.good());
    ofs << content;
}

void TestXapuShimRouting() {
    using namespace openscarlett;

    OS_REQUIRE(ClassifyImportDll("xapu.dll") == DllCategory::ConsoleExclusive);
    OS_REQUIRE(ClassifyImportDll("XAPU.DLL") == DllCategory::ConsoleExclusive);

    DllRoutingTable table;
    const DllRouteRule* rule = table.FindRule("xapu.dll");
    OS_REQUIRE(rule != nullptr);
    OS_REQUIRE(rule->canonical_name == "xapu.dll");
    OS_REQUIRE(rule->policy == DllRoutePolicy::Shim);
    OS_REQUIRE(rule->load_priority == 1);

    const DllRouteRule* rule_upper = table.FindRule("XAPU");
    OS_REQUIRE(rule_upper != nullptr);
    OS_REQUIRE(rule_upper->canonical_name == "xapu.dll");
    OS_REQUIRE(rule_upper->policy == DllRoutePolicy::Shim);

    auto has_xapu = [](const std::vector<DllRouteRule>& rules) {
        for (const auto& r : rules) {
            if (r.canonical_name == "xapu.dll" && r.policy == DllRoutePolicy::Shim) {
                return true;
            }
        }
        return false;
    };
    OS_REQUIRE(has_xapu(table.ListRules()));
    OS_REQUIRE(has_xapu(table.ListAllRules()));
    OS_REQUIRE(has_xapu(table.ListCompleteRules()));

    const fs::path root = ResolveScratchDir();
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path shim_dir = root / "shims";
    const fs::path guest_dir = root / "guest";
    fs::create_directories(guest_dir, ec);
    WriteStubFile(shim_dir / "xapu.dll", "OPENSCARLETT_XAPU_SHIM");

    const auto decision = table.ResolveRoute("xapu.dll", shim_dir.wstring(), guest_dir.wstring());
    OS_REQUIRE(decision.policy == DllRoutePolicy::Shim);
    OS_REQUIRE(decision.canonical_name == "xapu.dll");
    OS_REQUIRE(decision.load_priority == 1);
    OS_REQUIRE(!decision.aliased);
    OS_REQUIRE(!decision.from_override);
    OS_REQUIRE(decision.resolved_path == (shim_dir / L"xapu.dll").wstring());

    const auto decision_upper = table.ResolveRoute("XAPU.DLL", shim_dir.wstring(), guest_dir.wstring());
    OS_REQUIRE(decision_upper.policy == DllRoutePolicy::Shim);
    OS_REQUIRE(decision_upper.canonical_name == "xapu.dll");
    OS_REQUIRE(decision_upper.resolved_path == (shim_dir / L"xapu.dll").wstring());

    fs::remove_all(root, ec);
    std::printf("[PASS] xapu.dll registered in DllRoutingTable and resolves to DllRoutePolicy::Shim\n");
}

void TestRtcoreNtuserAndGdiApiSets() {
    using namespace openscarlett;

    PeForwarderResolver resolver;

    struct ExpectedApiSet {
        const char* dll;
        const char* symbol;
        const char* expected_target;
    };

    static constexpr ExpectedApiSet kCases[] = {
        {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow", "user32.dll"},
        {"ext-ms-win-rtcore-ntuser-message-l1-1-0.dll", "PeekMessageW", "user32.dll"},
        {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA", "user32.dll"},
        {"ext-ms-win-rtcore-ntuser-cursor-l1-1-0.dll", "ShowCursor", "user32.dll"},
        {"api-ms-win-rtcore-ntuser-window-l1-1-0.dll", "DestroyWindow", "user32.dll"},
        {"ext-ms-win-rtcore-gdi-object-l1-1-0.dll", "DeleteObject", "gdi32.dll"},
        {"api-ms-win-rtcore-gdi-rgn-l1-1-0.dll", "CreateRectRgn", "gdi32.dll"},
        {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification", "kernelbase.dll"},
    };

    for (const auto& tc : kCases) {
        const auto res = resolver.ResolveImportSymbol(tc.dll, tc.symbol);
        OS_REQUIRE(res.ok);
        OS_REQUIRE(res.kind == ForwarderResolutionKind::ApiSetRedirected);
        OS_REQUIRE(res.api_set_resolved);
        OS_REQUIRE(res.routed_to_windows_system);
        OS_REQUIRE(res.resolved_dll == tc.expected_target);
        OS_REQUIRE(res.resolved_symbol == tc.symbol);
    }

    // Verify xapu.dll resolves to OpenScarlettShim through PeForwarderResolver as well
    const auto xapu_res = resolver.ResolveImportSymbol("xapu.dll", "XApuCreate");
    OS_REQUIRE(xapu_res.ok);
    OS_REQUIRE(xapu_res.kind == ForwarderResolutionKind::OpenScarlettShim);
    OS_REQUIRE(xapu_res.routed_to_openscarlett_shim);
    OS_REQUIRE(xapu_res.resolved_dll == "xapu.dll");
    OS_REQUIRE(xapu_res.resolved_symbol == "XApuCreate");

    // Verify full preflight report marks all Minecraft Preview rtcore + xapu imports ready to launch
    const auto preflight = resolver.BuildPreflightReportFromForwarders(
        "minecraft_preview_rtcore_xapu",
        {
            "ext-ms-win-rtcore-ntuser-window-l1-1-0.ShowWindow",
            "ext-ms-win-rtcore-ntuser-message-l1-1-0.PeekMessageW",
            "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.CreateWindowExA",
            "ext-ms-win-rtcore-ntuser-cursor-l1-1-0.ShowCursor",
            "ext-ms-win-rtcore-gdi-object-l1-1-0.DeleteObject",
            "api-ms-win-rtcore-gdi-rgn-l1-1-0.CreateRectRgn",
            "xapu.XApuCreate",
        });
    OS_REQUIRE(preflight.ready_to_launch);
    OS_REQUIRE(preflight.total_entries == 7u);
    OS_REQUIRE(preflight.openscarlett_shim_count == 1u);
    OS_REQUIRE(preflight.apiset_redirected_count == 6u);
    OS_REQUIRE(preflight.unresolved_count == 0u);

    std::printf("[PASS] ext-ms-win-rtcore-ntuser-*, ext-ms-win-rtcore-gdi-*, and xapu.dll resolved cleanly\n");
}

}  // namespace

int main() {
    TestXapuShimRouting();
    TestRtcoreNtuserAndGdiApiSets();
    return 0;
}
