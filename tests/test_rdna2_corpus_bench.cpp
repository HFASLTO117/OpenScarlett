// OpenScarlett - MIT License
// Automated verification for GFX10.3 779-opcode ISA coverage auditor & shader corpus benchmark.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/gpu/rdna2_coverage_tool.h"
#include "os_check.h"

namespace {

using namespace openscarlett;

std::string ResolveTempJsonPath(const char* filename) {
    const std::filesystem::path dir("E:\\OpenScarlett\\.tmp\\s8-rdna2-corpus-bench");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / filename).string();
}

void Test1_CatalogAndCoreIsaAudit() {
    OS_REQUIRE(GetGfx103ReferenceCatalogSize() == 779u);

    Rdna2CoverageReport rep{};
    Status st = AuditRdna2IsaCoverage(rep);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] AuditRdna2IsaCoverage: %s\n", st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(rep.schema_version == 1u);
    OS_REQUIRE(rep.opcodes_total == 779u);
    OS_REQUIRE(rep.opcodes_decoded >= 224u);
    OS_REQUIRE(rep.opcodes_decoded <= rep.opcodes_total);
    OS_REQUIRE(rep.opcodes_lifted_spirv >= 224u);
    OS_REQUIRE(rep.opcodes_lifted_dxil >= 224u);
    OS_REQUIRE(rep.opcodes_cpu_ref >= 224u);
    OS_REQUIRE(rep.families.size() == 20u);

    uint32_t sum_total = 0u;
    uint32_t sum_decoded = 0u;
    for (const auto& f : rep.families) {
        OS_REQUIRE(!f.family.empty());
        OS_REQUIRE(f.total > 0u);
        OS_REQUIRE(f.decoded <= f.total);
        OS_REQUIRE(f.lifted_spirv <= f.total);
        OS_REQUIRE(f.lifted_dxil <= f.total);
        OS_REQUIRE(f.cpu_ref <= f.total);
        sum_total += f.total;
        sum_decoded += f.decoded;
    }
    OS_REQUIRE(sum_total == 779u);
    OS_REQUIRE(sum_decoded == rep.opcodes_decoded);

    std::printf("[PASS] Test 1: GFX10.3 779-opcode catalog & round-trip audit (%u/779 decoded, 20 families, sv_ext=%s, mem_ext=%s)\n",
                rep.opcodes_decoded,
                rep.has_sv_isa_extension ? "yes" : "no",
                rep.has_mem_isa_extension ? "yes" : "no");
}

void Test2_StratifiedCorpusAndLiveGpuParity(bool run_full_3120, const std::string& json_override) {
    Rdna2BenchOptions opts{};
    opts.full_corpus = run_full_3120;
    opts.corpus_count = run_full_3120 ? 3120u : 240u;
    opts.external_val_sample = run_full_3120 ? 60u : 24u;
    opts.run_live_gpu = true;
    opts.run_external_validators = true;
    opts.json_output_path = json_override.empty()
                                ? ResolveTempJsonPath(run_full_3120 ? "rdna2_coverage_full.json"
                                                                    : "rdna2_coverage_ctest.json")
                                : json_override;

    Rdna2CoverageReport rep{};
    Status st = RunRdna2CoverageBenchmark(opts, rep);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] RunRdna2CoverageBenchmark: %s\n", st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(rep.opcodes_total == 779u);
    OS_REQUIRE(rep.opcodes_decoded >= 224u);
    OS_REQUIRE(rep.corpus_shaders_total == opts.corpus_count);
    OS_REQUIRE(rep.corpus_shaders_passed == opts.corpus_count);
    OS_REQUIRE(rep.corpus_unique_hashes == opts.corpus_count);
    OS_REQUIRE(rep.corpus_cpu_ref_executed >= 48u);

    // All 5 stages (CS, VS, PS, MS, AS) must have non-zero shaders and 100% pass rate
    OS_REQUIRE(rep.stages.size() == 5u);
    uint32_t stage_total_sum = 0u;
    uint32_t stage_passed_sum = 0u;
    for (const auto& s : rep.stages) {
        OS_REQUIRE(s.total > 0u);
        OS_REQUIRE(s.passed == s.total);
        OS_REQUIRE(s.disasm_passed == s.total);
        if (s.stage != "AS") {
            OS_REQUIRE(s.spirv_passed == s.total);
        }
        if (rep.has_dxc_tool) {
            OS_REQUIRE(s.dxil_passed > 0u);
        }
        OS_REQUIRE(s.cpu_ref_passed > 0u);
        stage_total_sum += s.total;
        stage_passed_sum += s.passed;
    }
    OS_REQUIRE(stage_total_sum == rep.corpus_shaders_total);
    OS_REQUIRE(stage_passed_sum == rep.corpus_shaders_passed);

    // All 12 AAA workloads must have equal counts and 100% pass rate
    OS_REQUIRE(rep.workloads.size() == 12u);
    const uint32_t expected_per_wl = opts.corpus_count / 12u;
    for (const auto& w : rep.workloads) {
        OS_REQUIRE(w.total == expected_per_wl);
        OS_REQUIRE(w.passed == w.total);
        if (rep.has_dxc_tool) {
            OS_REQUIRE(w.dxil_passed > 0u);
        }
        OS_REQUIRE(w.cpu_ref_passed > 0u);
    }

    if (rep.has_spirv_val_tool) {
        OS_REQUIRE(rep.spirv_val_external_validated >= 12u);
    }
    if (rep.has_dxc_tool) {
        OS_REQUIRE(rep.corpus_dxil_compiled >= opts.external_val_sample);
        OS_REQUIRE(rep.corpus_dxil_resigned_verified == rep.corpus_dxil_compiled);
    }
    if (rep.has_d3d12_hw || rep.has_d3d12_warp || rep.has_vulkan_hw) {
        OS_REQUIRE(rep.corpus_gpu_executed >= 7u);
        OS_REQUIRE(rep.opcodes_gpu_verified >= 25u);
    }

    // Verify deterministic JSON serialization and on-disk output
    const std::string json1 = SerializeRdna2CoverageJson(rep);
    const std::string json2 = SerializeRdna2CoverageJson(rep);
    OS_REQUIRE(json1 == json2);
    OS_REQUIRE(json1.find("\"schema_version\": 1") != std::string::npos);
    OS_REQUIRE(json1.find("\"opcodes_total\": 779") != std::string::npos);
    OS_REQUIRE(json1.find("\"opcodes_decoded\": ") != std::string::npos);
    OS_REQUIRE(json1.find("\"corpus_shaders_total\": ") != std::string::npos);
    OS_REQUIRE(json1.find("\"corpus_shaders_passed\": ") != std::string::npos);
    OS_REQUIRE(json1.find("\"pbr_ibl\"") != std::string::npos);
    OS_REQUIRE(json1.find("\"meshlet_culling\"") != std::string::npos);
    OS_REQUIRE(json1.find("\"bvh4_ray_traversal\"") != std::string::npos);
    OS_REQUIRE(json1.find("\"scratch_register_spilling\"") != std::string::npos);

    std::ifstream ifs(opts.json_output_path, std::ios::binary);
    OS_REQUIRE(ifs.good());
    std::ostringstream ss;
    ss << ifs.rdbuf();
    OS_REQUIRE(ss.str() == json1);

    std::printf(
        "[PASS] Test 2: %u/%u corpus shaders passed (%u unique hashes, 5 stages, 12 AAA workloads) | "
        "spirv-val=%u | DXIL=%u (resigned=%u) | CPU=%u | GPU=%u (HW=%u, WARP=%u, VK=%u, ops_gpu=%u) | %.2fs\n",
        rep.corpus_shaders_passed,
        rep.corpus_shaders_total,
        rep.corpus_unique_hashes,
        rep.spirv_val_external_validated,
        rep.corpus_dxil_compiled,
        rep.corpus_dxil_resigned_verified,
        rep.corpus_cpu_ref_executed,
        rep.corpus_gpu_executed,
        rep.corpus_gpu_hw_executed,
        rep.corpus_gpu_warp_executed,
        rep.corpus_vulkan_executed,
        rep.opcodes_gpu_verified,
        rep.elapsed_seconds);
}

void Test3_CliSelfTest() {
    {
        char arg0[] = "openscarlett";
        char arg1[] = "rdna2-bench";
        char arg2[] = "--help";
        char* argv[] = {arg0, arg1, arg2};
        OS_REQUIRE(RunRdna2BenchCli(3, argv) == 0);
    }
    {
        const std::string cli_json = ResolveTempJsonPath("rdna2_cli_smoke.json");
        std::vector<char> path_buf(cli_json.begin(), cli_json.end());
        path_buf.push_back('\0');
        char arg0[] = "openscarlett";
        char arg1[] = "rdna2-bench";
        char arg2[] = "--fast";
        char arg3[] = "--corpus-count";
        char arg4[] = "24";
        char arg5[] = "--no-gpu";
        char arg6[] = "--no-external";
        char arg7[] = "--quiet";
        char arg8[] = "--json";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, path_buf.data()};
        OS_REQUIRE(RunRdna2BenchCli(10, argv) == 0);
        OS_REQUIRE(std::filesystem::exists(cli_json));
    }
    std::printf("[PASS] Test 3: RunRdna2BenchCli (--help & --fast smoke JSON output) verified\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool run_full = false;
    std::string json_override;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--full-bench") == 0) {
            run_full = true;
        } else if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_override = argv[++i];
        }
    }

    Test1_CatalogAndCoreIsaAudit();
    Test2_StratifiedCorpusAndLiveGpuParity(run_full, json_override);
    Test3_CliSelfTest();

    std::printf("=== ALL RDNA2 CORPUS BENCHMARK TESTS PASSED ===\n");
    return 0;
}
