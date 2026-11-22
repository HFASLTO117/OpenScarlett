// OpenScarlett Sprint 9 + 10 — First-Boot Probe Mode, Win32 VEH Crash Dumper,
// Privacy-Sanitized Bring-Up Telemetry JSON & Per-Draw Frame/Shader Capture Test Suite.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/frame_capture.h"
#include "openscarlett/loader/probe_veh.h"
#include "os_check.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

using namespace openscarlett;

std::string ReadFileText(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(ifs),
                                std::istreambuf_iterator<char>());
}

uint32_t ReadBe32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24u) |
           (static_cast<uint32_t>(p[1]) << 16u) |
           (static_cast<uint32_t>(p[2]) << 8u) |
           static_cast<uint32_t>(p[3]);
}

uint32_t ComputeCrc32Verify(const uint8_t* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void VerifyValidPng(const std::vector<uint8_t>& png,
                    uint32_t expected_w,
                    uint32_t expected_h,
                    uint8_t expected_color_type) {
    static constexpr uint8_t kSig[8] = {0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    OS_REQUIRE(png.size() >= 8u + 12u + 13u + 12u);
    OS_REQUIRE(std::memcmp(png.data(), kSig, 8) == 0);

    bool saw_ihdr = false;
    bool saw_idat = false;
    bool saw_iend = false;

    size_t pos = 8u;
    while (pos + 12u <= png.size()) {
        const uint32_t chunk_len = ReadBe32(png.data() + pos);
        const uint8_t* type_ptr = png.data() + pos + 4u;
        const size_t data_offset = pos + 8u;
        const size_t crc_offset = data_offset + chunk_len;
        OS_REQUIRE(crc_offset + 4u <= png.size());

        const uint32_t stored_crc = ReadBe32(png.data() + crc_offset);
        const uint32_t computed_crc = ComputeCrc32Verify(type_ptr, 4u + chunk_len);
        OS_REQUIRE(stored_crc == computed_crc);

        if (std::memcmp(type_ptr, "IHDR", 4) == 0) {
            OS_REQUIRE(chunk_len == 13u);
            OS_REQUIRE(ReadBe32(png.data() + data_offset) == expected_w);
            OS_REQUIRE(ReadBe32(png.data() + data_offset + 4u) == expected_h);
            OS_REQUIRE(png[data_offset + 8u] == 8u);
            OS_REQUIRE(png[data_offset + 9u] == expected_color_type);
            saw_ihdr = true;
        } else if (std::memcmp(type_ptr, "IDAT", 4) == 0) {
            OS_REQUIRE(chunk_len >= 6u);
            // Check valid RFC 1950 zlib header checksum: (CMF*256 + FLG) % 31 == 0
            const uint32_t cmf = png[data_offset];
            const uint32_t flg = png[data_offset + 1u];
            OS_REQUIRE(((cmf * 256u + flg) % 31u) == 0u);
            saw_idat = true;
        } else if (std::memcmp(type_ptr, "IEND", 4) == 0) {
            OS_REQUIRE(chunk_len == 0u);
            saw_iend = true;
        }

        pos = crc_offset + 4u;
    }

    OS_REQUIRE(pos == png.size());
    OS_REQUIRE(saw_ihdr && saw_idat && saw_iend);
}

#if defined(_WIN32)
// Builds a synthetic mapped PE32+ image in memory containing an import table with
// an unknown Xbox DLL ("missing_xbox_ext.dll") importing "XboxUnknownMagicFn" and ordinal #77.
struct SyntheticPeImage {
    std::vector<uint8_t> image;
    uint32_t iat_rva = 0;
};

SyntheticPeImage BuildSyntheticMappedPeWithMissingImports() {
    SyntheticPeImage out{};
    out.image.assign(0x3000u, 0u);
    uint8_t* base = out.image.data();

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(out.image.size());
    nt->OptionalHeader.SizeOfHeaders = 0x200;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    // Place import structures at RVA 0x1000
    constexpr uint32_t kImportDescRva = 0x1000u;
    constexpr uint32_t kIntRva        = 0x1100u;
    constexpr uint32_t kIatRva        = 0x1200u;
    constexpr uint32_t kDllNameRva    = 0x1300u;
    constexpr uint32_t kIbnRva        = 0x1340u;

    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = kImportDescRva;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
        static_cast<DWORD>(sizeof(IMAGE_IMPORT_DESCRIPTOR) * 2u);

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + kImportDescRva);
    desc[0].OriginalFirstThunk = kIntRva;
    desc[0].FirstThunk = kIatRva;
    desc[0].Name = kDllNameRva;

    const char* dll_name = "missing_xbox_ext.dll";
    std::memcpy(base + kDllNameRva, dll_name, std::strlen(dll_name) + 1u);

    auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + kIbnRva);
    ibn->Hint = 0;
    const char* fn_name = "XboxUnknownMagicFn";
    std::memcpy(ibn->Name, fn_name, std::strlen(fn_name) + 1u);

    auto* int_tbl = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + kIntRva);
    int_tbl[0].u1.AddressOfData = kIbnRva;
    int_tbl[1].u1.Ordinal = IMAGE_ORDINAL_FLAG64 | 77ull;
    int_tbl[2].u1.Ordinal = 0;

    auto* iat_tbl = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + kIatRva);
    iat_tbl[0].u1.AddressOfData = kIbnRva;
    iat_tbl[1].u1.Ordinal = IMAGE_ORDINAL_FLAG64 | 77ull;
    iat_tbl[2].u1.Ordinal = 0;

    out.iat_rva = kIatRva;
    return out;
}
#endif

// ============================================================================
// Test 1: Synthetic Missing-Import PE Patching & Live x64 Trampoline Execution
// ============================================================================
void TestMissingImportTrampolines() {
    ResetProbeApiRing();
    CallTelemetry::Global().Reset();

    MissingImportTrampolinePool pool;

#if defined(_WIN32)
    SyntheticPeImage syn = BuildSyntheticMappedPeWithMissingImports();
    ProbePePatchSummary summary{};
    const Status patch_st = PatchPeImportsWithProbeTrampolines(
        syn.image.data(),
        pool,
        &summary,
        [](const std::string&, const std::string&, uint16_t, bool) -> void* {
            return nullptr;  // Force unresolved so trampolines are synthesized
        });
    OS_REQUIRE(patch_st.ok());
    OS_REQUIRE(summary.total_imports_scanned == 2u);
    OS_REQUIRE(summary.patched_slots == 2u);
    OS_REQUIRE(summary.missing_imports.size() == 2u);
    OS_REQUIRE(pool.Size() == 2u);

    const auto* iat = reinterpret_cast<const IMAGE_THUNK_DATA64*>(syn.image.data() + syn.iat_rva);
    OS_REQUIRE(iat[0].u1.Function != 0);
    OS_REQUIRE(iat[1].u1.Function != 0);

    // Invoke the generated x64 machine-code trampolines directly from C++!
    using FnFourArgs = int32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
    auto fn_named = reinterpret_cast<FnFourArgs>(static_cast<uintptr_t>(iat[0].u1.Function));
    auto fn_ord = reinterpret_cast<FnFourArgs>(static_cast<uintptr_t>(iat[1].u1.Function));

    const int32_t hr1 = fn_named(0x1122334455667788ull, 2, 3, 4);
    const int32_t hr2 = fn_named(0xAABBCCDD00112233ull, 5, 6, 7);
    const int32_t hr3 = fn_ord(0x99887766ull, 0, 0, 0);

    OS_REQUIRE(static_cast<uint32_t>(hr1) == kProbeNotImplHResult);
    OS_REQUIRE(static_cast<uint32_t>(hr2) == kProbeNotImplHResult);
    OS_REQUIRE(static_cast<uint32_t>(hr3) == kProbeNotImplHResult);

    // Verify trampoline call counts
    const auto missing = pool.SnapshotMissingImports();
    OS_REQUIRE(missing.size() == 2u);
    bool found_magic = false;
    bool found_ord77 = false;
    for (const auto& m : missing) {
        if (m.dll_name == "missing_xbox_ext.dll" && m.symbol_name == "XboxUnknownMagicFn") {
            OS_REQUIRE(m.call_count == 2u);
            found_magic = true;
        } else if (m.dll_name == "missing_xbox_ext.dll" && m.by_ordinal && m.ordinal == 77u) {
            OS_REQUIRE(m.call_count == 1u);
            found_ord77 = true;
        }
    }
    OS_REQUIRE(found_magic && found_ord77);

    // Verify API call ring buffer recorded all 3 calls with arg0 & E_NOTIMPL
    const auto ring = SnapshotProbeApiRing();
    OS_REQUIRE(ring.size() == 3u);
    OS_REQUIRE(ring[0].module_name == "missing_xbox_ext.dll");
    OS_REQUIRE(ring[0].symbol_name == "XboxUnknownMagicFn");
    OS_REQUIRE(ring[0].arg0 == 0x1122334455667788ull);
    OS_REQUIRE(static_cast<uint32_t>(ring[0].hr) == kProbeNotImplHResult);
    OS_REQUIRE(ring[2].symbol_name == "#77");

    // Verify CallTelemetry recorded not_impl++
    const auto tel = CallTelemetry::Global().Snapshot();
    bool tel_found = false;
    for (const auto& entry : tel) {
        if (entry.module_name == "missing_xbox_ext.dll" &&
            entry.symbol_name == "XboxUnknownMagicFn") {
            OS_REQUIRE(entry.not_impl_calls == 2u);
            OS_REQUIRE(entry.total_calls == 2u);
            tel_found = true;
        }
    }
    OS_REQUIRE(tel_found);
#endif

    std::printf("[PASS] Test 1: MissingImportTrampolinePool x64 execution, E_NOTIMPL & telemetry\n");
}

// ============================================================================
// Test 2: Controlled VEH Capture, 50-Entry Ring Buffer & PII Sanitizer JSON
// ============================================================================
void TestVehCrashMonitorAndSanitizedJson(const fs::path& scratch_dir) {
    ResetProbeApiRing();

    // Record 60 synthetic API/D3D12 calls -> ring must retain exact last 50 (calls #11..#60)
    for (uint32_t i = 1; i <= 60; ++i) {
        RecordProbeApiCall(
            "d3d12_xs.dll",
            "ID3D12GraphicsCommandListX::DrawIndexedInstanced_" + std::to_string(i),
            0x1000ull + i,
            0);
    }

    const auto ring_before = SnapshotProbeApiRing();
    OS_REQUIRE(ring_before.size() == kProbeApiRingCapacity);
    OS_REQUIRE(ring_before.front().sequence == 11u);
    OS_REQUIRE(ring_before.front().symbol_name == "ID3D12GraphicsCommandListX::DrawIndexedInstanced_11");
    OS_REQUIRE(ring_before.back().sequence == 60u);
    OS_REQUIRE(ring_before.back().symbol_name == "ID3D12GraphicsCommandListX::DrawIndexedInstanced_60");

#if defined(_WIN32)
    MissingImportTrampolinePool pool;
    (void)pool.GetOrCreateTrampoline("missing_xbox_ext.dll", "XboxProbeStubFn", 0, false);

    VehCrashMonitor monitor;
    monitor.SetTrampolinePool(&pool);
    const fs::path auto_json_path = scratch_dir / "veh_auto_bringup.json";
    monitor.SetAutoExportPath(auto_json_path.string(), {scratch_dir.string()});

    bool hook_invoked = false;
    monitor.SetRecoveryHook([&](const ProbeExceptionReport& rep, void* /*ep*/) -> bool {
        if (rep.code == kOpenScarlettTestVehException) {
            hook_invoked = true;
            return true;  // Safely resume after RaiseException
        }
        return false;
    });

    OS_REQUIRE(monitor.Install(1).ok());
    OS_REQUIRE(monitor.IsInstalled());

    // Trigger controlled continuable VEH probe exception
    ::RaiseException(kOpenScarlettTestVehException, 0, 0, nullptr);

    OS_REQUIRE(hook_invoked);
    const ProbeExceptionReport rep = monitor.GetLastReport();
    OS_REQUIRE(rep.captured);
    OS_REQUIRE(rep.code == kOpenScarlettTestVehException);
    OS_REQUIRE(rep.fault_address != 0);
    OS_REQUIRE(rep.registers.rip != 0);
    OS_REQUIRE(rep.registers.rsp != 0);
    OS_REQUIRE(!rep.stack_frames.empty());
    OS_REQUIRE(rep.api_ring_last_50.size() == kProbeApiRingCapacity);
    OS_REQUIRE(rep.api_ring_last_50.front().sequence == 11u);
    OS_REQUIRE(rep.api_ring_last_50.back().sequence == 60u);

    // Also verify auto-exported JSON file exists and is valid
    OS_REQUIRE(fs::is_regular_file(auto_json_path));
    const std::string auto_json = ReadFileText(auto_json_path);
    OS_REQUIRE(auto_json.find("\"exception_report\"") != std::string::npos);
    OS_REQUIRE(auto_json.find("0xE04F5301") != std::string::npos);
    OS_REQUIRE(auto_json.find("\"stack_frames\"") != std::string::npos);
    OS_REQUIRE(auto_json.find("\"api_ring_last_50\"") != std::string::npos);

    monitor.Uninstall();
    OS_REQUIRE(!monitor.IsInstalled());

    // Verify PII sanitization strips real Windows username, synthetic username, %USERPROFILE%, and local roots
    char user_env[256]{};
    const DWORD ulen = ::GetEnvironmentVariableA("USERNAME", user_env, static_cast<DWORD>(sizeof(user_env)));
    const std::string real_user = (ulen > 0 && ulen < sizeof(user_env)) ? std::string(user_env, ulen) : "TestUser";
    const std::string fake_root = scratch_dir.string();

    BringUpTelemetrySnapshot snap{};
    snap.probe_mode = true;
    snap.host_os = GetProbeHostOsSummary();
    snap.missing_imports = pool.SnapshotMissingImports();
    snap.api_ring_last_50 = rep.api_ring_last_50;
    snap.exception_report = rep;
    // Inject synthetic PII into module/symbol fields to prove sanitizer strips them completely!
    snap.api_ring_last_50.back().module_name =
        "C:\\Users\\" + real_user + "\\Desktop\\secret_game.dll";
    snap.api_ring_last_50.back().symbol_name =
        fake_root + "\\private_repo\\SecretCall_C:\\Users\\AliceSmith\\foo.bin";

    const std::string sanitized_json = ExportSanitizedBringUpTelemetryJson(snap, {fake_root});
    OS_REQUIRE(sanitized_json.find("\"probe_mode\": true") != std::string::npos);
    OS_REQUIRE(sanitized_json.find("\"missing_imports\"") != std::string::npos);
    OS_REQUIRE(sanitized_json.find("\"api_ring_last_50\"") != std::string::npos);
    OS_REQUIRE(sanitized_json.find("\"exception_report\"") != std::string::npos);
    OS_REQUIRE(sanitized_json.find("AliceSmith") == std::string::npos);
    OS_REQUIRE(sanitized_json.find("<USER>") != std::string::npos);
    OS_REQUIRE(sanitized_json.find("<LOCAL_ROOT>") != std::string::npos);
    if (real_user.size() >= 3u) {
        OS_REQUIRE(sanitized_json.find("Users\\\\" + real_user) == std::string::npos);
        OS_REQUIRE(sanitized_json.find("Users\\" + real_user) == std::string::npos);
    }
#endif

    std::printf("[PASS] Test 2: VehCrashMonitor capture, 50-call ring buffer & PII-sanitized JSON\n");
}

// ============================================================================
// Test 3: Per-Draw Frame & Shader Dumper (metadata.json, shaders, valid PNGs)
// ============================================================================
void TestFrameCaptureRecorder(const fs::path& scratch_dir) {
    const fs::path cap_dir = scratch_dir / "frame_capture_out";

    FrameCaptureRecorder rec;
    // Disabled fast-path must return false immediately
    OS_REQUIRE(!rec.IsEnabled());
    OS_REQUIRE(!rec.BeginDraw(0, CapturedDrawType::DrawInstanced));

    OS_REQUIRE(rec.Initialize(cap_dir.string(), 8).ok());
    OS_REQUIRE(rec.IsEnabled());

    constexpr uint32_t kW = 16;
    constexpr uint32_t kH = 12;

    for (uint32_t draw_idx = 0; draw_idx < 3; ++draw_idx) {
        const CapturedDrawType dtype =
            (draw_idx == 0) ? CapturedDrawType::DrawIndexedInstanced
            : (draw_idx == 1) ? CapturedDrawType::DispatchMesh
                              : CapturedDrawType::DrawInstanced;
        OS_REQUIRE(rec.BeginDraw(draw_idx, dtype, "pass_" + std::to_string(draw_idx)));

        const std::vector<uint8_t> vs_blob = {
            0x44, 0x58, 0x42, 0x43, static_cast<uint8_t>(draw_idx), 0x10, 0x20, 0x30
        };
        const std::vector<uint8_t> ps_blob = {
            0x58, 0x53, 0x32, 0x00, static_cast<uint8_t>(draw_idx + 1u), 0xAA, 0xBB, 0xCC
        };
        rec.AttachShaderBlob("vs", vs_blob);
        rec.AttachShaderBlob("ps", ps_blob);
        rec.AttachRootArguments({0xDEADBEEFu, draw_idx, 0x3F800000u});

        CapturedDescriptorBinding desc{};
        desc.root_parameter_index = 1;
        desc.kind = "CBV";
        desc.gpu_va = 0x0000000140001000ull + draw_idx * 0x100ull;
        desc.descriptor_count = 1;
        desc.label = "FrameConstants";
        rec.AttachDescriptorBinding(desc);

        std::vector<uint8_t> rgba(kW * kH * 4u, 0u);
        std::vector<float> depth(kW * kH, 1.0f);
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const size_t idx = static_cast<size_t>(y) * kW + x;
                rgba[idx * 4u + 0u] = static_cast<uint8_t>((x * 16u + draw_idx * 30u) & 0xFFu);
                rgba[idx * 4u + 1u] = static_cast<uint8_t>((y * 20u) & 0xFFu);
                rgba[idx * 4u + 2u] = static_cast<uint8_t>(64u + draw_idx * 40u);
                rgba[idx * 4u + 3u] = 255u;
                depth[idx] = 0.1f + 0.8f * (static_cast<float>(x + y) / static_cast<float>(kW + kH));
            }
        }
        rec.AttachColorTargetRgba8(kW, kH, rgba);
        rec.AttachDepthTargetF32(kW, kH, depth);

        OS_REQUIRE(rec.EndDraw().ok());
    }

    OS_REQUIRE(rec.WriteCaptureIndexJson().ok());
    OS_REQUIRE(rec.GetCapturedDraws().size() == 3u);

    // Verify capture_index.json
    const fs::path index_path = cap_dir / "capture_index.json";
    OS_REQUIRE(fs::is_regular_file(index_path));
    const std::string index_text = ReadFileText(index_path);
    OS_REQUIRE(index_text.find("\"captured_draws\": 3") != std::string::npos);
    OS_REQUIRE(index_text.find("\"draw_0000\"") != std::string::npos);
    OS_REQUIRE(index_text.find("\"draw_0001\"") != std::string::npos);
    OS_REQUIRE(index_text.find("\"draw_0002\"") != std::string::npos);

    // Verify draw_0000 artifacts
    const fs::path d0 = cap_dir / "draw_0000";
    OS_REQUIRE(fs::is_regular_file(d0 / "metadata.json"));
    OS_REQUIRE(fs::is_regular_file(d0 / "shader_vs.bin"));
    OS_REQUIRE(fs::is_regular_file(d0 / "shader_ps.bin"));
    OS_REQUIRE(fs::is_regular_file(d0 / "color_rt0.png"));
    OS_REQUIRE(fs::is_regular_file(d0 / "depth_d32.png"));

    const std::string meta0 = ReadFileText(d0 / "metadata.json");
    OS_REQUIRE(meta0.find("\"draw_index\": 0") != std::string::npos);
    OS_REQUIRE(meta0.find("\"DrawIndexedInstanced\"") != std::string::npos);
    OS_REQUIRE(meta0.find("\"0xDEADBEEF\"") != std::string::npos);
    OS_REQUIRE(meta0.find("\"shader_vs.bin\"") != std::string::npos);
    OS_REQUIRE(meta0.find("\"color_rt0.png\"") != std::string::npos);
    OS_REQUIRE(meta0.find("\"depth_d32.png\"") != std::string::npos);

    const auto vs0_bytes = ReadFileBytes(d0 / "shader_vs.bin");
    OS_REQUIRE(vs0_bytes.size() == 8u);
    OS_REQUIRE(vs0_bytes[0] == 0x44 && vs0_bytes[1] == 0x58 && vs0_bytes[2] == 0x42 && vs0_bytes[3] == 0x43);

    const auto color0_png = ReadFileBytes(d0 / "color_rt0.png");
    VerifyValidPng(color0_png, kW, kH, 6u);  // RGBA8

    const auto depth0_png = ReadFileBytes(d0 / "depth_d32.png");
    VerifyValidPng(depth0_png, kW, kH, 0u);  // Gray8

    rec.Reset();
    OS_REQUIRE(!rec.IsEnabled());

    std::printf("[PASS] Test 3: FrameCaptureRecorder (3 draws, metadata.json, shaders, CRC32-verified PNGs)\n");
}

}  // namespace

int main() {
    std::error_code ec;
    const fs::path scratch_dir =
        fs::temp_directory_path(ec) / "openscarlett_test_probe_veh_capture";
    fs::remove_all(scratch_dir, ec);
    fs::create_directories(scratch_dir, ec);

    TestMissingImportTrampolines();
    TestVehCrashMonitorAndSanitizedJson(scratch_dir);
    TestFrameCaptureRecorder(scratch_dir);

    fs::remove_all(scratch_dir, ec);
    std::printf("=== ALL probe_veh + frame_capture TESTS PASSED ===\n");
    return 0;
}
