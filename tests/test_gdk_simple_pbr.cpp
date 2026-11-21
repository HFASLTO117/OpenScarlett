// OpenScarlett Wave 2 — Agent F: SimplePBR Live Multi-Frame 3D MSAA/Depth Test Harness.
//
// 100% Portable for CI hidden-build smoke tests:
//   - Resolves openscarlett.exe / openscarlett_hook.dll via GetModuleFileNameW first.
//   - Resolves dxc.exe portably (exe dir -> OPENSCARLETT_DXC_PATH -> PATH -> Windows Kits).
//   - Stages a zero-DLL Xbox package folder under %TEMP% with MicrosoftGame.Config,
//     compiled SM 6.5 + SM 6.2 Cook-Torrance PBR DXIL shaders, and gdk_simple_pbr.exe.
//   - Launches via `openscarlett.exe run <package_dir>` (package directory auto-resolver)
//     on BOTH real hardware GPU (when present) and D3D12 WARP (`-- --warp`).
//   - Verifies zero-DLL byte-identical package integrity, valid 256x256 P6 PPM output,
//     3 distinct animated frame hashes, depth occlusion, HW-vs-WARP MAE tolerance, and
//     CallTelemetry (not_impl == 0, all 5 modules, exact COM vtable slots).

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kPbrVsHlsl = R"HLSL(
#define PBR_ROOT_SIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL)," \
    "StaticSampler(s0," \
        "filter=FILTER_MIN_MAG_MIP_LINEAR," \
        "addressU=TEXTURE_ADDRESS_WRAP," \
        "addressV=TEXTURE_ADDRESS_WRAP," \
        "addressW=TEXTURE_ADDRESS_WRAP," \
        "visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer PbrConstants : register(b0) {
    row_major float4x4 g_World;
    row_major float4x4 g_ViewProj;
    float4 g_CameraPos;
    float4 g_LightDir;
    float4 g_LightColor;
    float4 g_AlbedoTint;
    float4 g_MaterialParams;
};

struct VsIn {
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float4 Tangent  : TANGENT;
    float2 TexCoord : TEXCOORD0;
};

struct VsOut {
    float4 PositionCS : SV_Position;
    float3 WorldPos   : POSITION0;
    float3 WorldNorm  : NORMAL0;
    float4 WorldTang  : TANGENT0;
    float2 TexCoord   : TEXCOORD0;
};

[RootSignature(PBR_ROOT_SIG)]
VsOut main(VsIn input) {
    VsOut o;
    float4 worldPos = mul(float4(input.Position, 1.0f), g_World);
    o.PositionCS = mul(worldPos, g_ViewProj);
    o.WorldPos = worldPos.xyz;
    o.WorldNorm = normalize(mul(float4(input.Normal, 0.0f), g_World).xyz);
    o.WorldTang = float4(normalize(mul(float4(input.Tangent.xyz, 0.0f), g_World).xyz), input.Tangent.w);
    o.TexCoord = input.TexCoord * g_MaterialParams.z;
    return o;
}
)HLSL";

constexpr const char* kPbrPsHlsl = R"HLSL(
#define PBR_ROOT_SIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL)," \
    "StaticSampler(s0," \
        "filter=FILTER_MIN_MAG_MIP_LINEAR," \
        "addressU=TEXTURE_ADDRESS_WRAP," \
        "addressV=TEXTURE_ADDRESS_WRAP," \
        "addressW=TEXTURE_ADDRESS_WRAP," \
        "visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer PbrConstants : register(b0) {
    row_major float4x4 g_World;
    row_major float4x4 g_ViewProj;
    float4 g_CameraPos;
    float4 g_LightDir;
    float4 g_LightColor;
    float4 g_AlbedoTint;
    float4 g_MaterialParams;
};

Texture2D<float4> g_AlbedoTex   : register(t0);
Texture2D<float4> g_NormalRmTex : register(t1);
SamplerState      g_LinearWrap  : register(s0);

struct VsOut {
    float4 PositionCS : SV_Position;
    float3 WorldPos   : POSITION0;
    float3 WorldNorm  : NORMAL0;
    float4 WorldTang  : TANGENT0;
    float2 TexCoord   : TEXCOORD0;
};

static const float kPi = 3.14159265f;

float DistributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * denom * denom, 1e-5f);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-5f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
    float t = saturate(1.0f - cosTheta);
    float t2 = t * t;
    return F0 + (1.0f - F0) * (t2 * t2 * t);
}

[RootSignature(PBR_ROOT_SIG)]
float4 main(VsOut input) : SV_TARGET0 {
    float3 texAlbedo = g_AlbedoTex.Sample(g_LinearWrap, input.TexCoord).rgb;
    float4 nrmSample = g_NormalRmTex.Sample(g_LinearWrap, input.TexCoord);

    float3 albedo = saturate(texAlbedo * g_AlbedoTint.rgb);

    // Orthonormal tangent-space normal perturbation
    float3 N0 = normalize(input.WorldNorm);
    float3 T = normalize(input.WorldTang.xyz - N0 * dot(input.WorldTang.xyz, N0));
    float3 B = cross(N0, T) * input.WorldTang.w;
    float2 nXY = (nrmSample.rg * 2.0f - 1.0f) * g_AlbedoTint.a;
    float nZ = sqrt(saturate(1.0f - dot(nXY, nXY)));
    float3 N = normalize(T * nXY.x + B * nXY.y + N0 * nZ);

    float metallic = saturate(g_MaterialParams.x * (0.75f + 0.5f * nrmSample.a));
    float roughness = clamp(g_MaterialParams.y * (0.65f + 0.7f * nrmSample.b), 0.08f, 0.96f);

    float3 V = normalize(g_CameraPos.xyz - input.WorldPos);
    float3 L = normalize(g_LightDir.xyz);
    float3 VL = V + L;
    float vlLen = length(VL);
    float3 H = (vlLen > 1e-4f) ? (VL / vlLen) : N;

    float NdotV = max(dot(N, V), 0.001f);
    float NdotL = max(dot(N, L), 0.0f);
    float NdotH = max(dot(N, H), 0.0f);
    float HdotV = max(dot(H, V), 0.0f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // Cook-Torrance microfacet specular + Lambertian energy-conserving diffuse
    float  D = DistributionGGX(NdotH, roughness);
    float  G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 0.001f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo / kPi;

    float3 radiance = g_LightColor.rgb * g_LightDir.w;
    float3 Lo = (diffuse + specular) * radiance * NdotL;

    // Subtle cool grazing rim light
    float3 rimL = normalize(float3(-L.x, 0.35f, -L.z * 0.4f));
    float rimNdotL = max(dot(N, rimL), 0.0f);
    float3 rimColor = float3(0.08f, 0.11f, 0.16f) * (albedo * 0.4f + F0 * 0.6f) * rimNdotL * 0.35f;

    // Low hemisphere ambient so back/bottom of spheres stay genuinely dark
    float hemi = 0.5f + 0.5f * N.y;
    float3 ambient = albedo * g_LightColor.a * lerp(0.45f, 1.0f, hemi);

    float3 color = ambient + Lo + rimColor;

    // Reinhard tone mapping + sRGB gamma encoding
    color = color / (color + float3(1.0f, 1.0f, 1.0f));
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    return float4(saturate(color), 1.0f);
}
)HLSL";

constexpr const char* kMicrosoftGameConfigXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Game configVersion=\"1\">\n"
    "  <Identity Name=\"OpenScarlett.SimplePBR\"\n"
    "            Publisher=\"CN=OpenScarlett, O=MIT\"\n"
    "            Version=\"1.0.0.0\" />\n"
    "  <TitleId>0x5CA01001</TitleId>\n"
    "  <MSAAppId>000000004C261001</MSAAppId>\n"
    "  <ShellVisuals DefaultDisplayName=\"OpenScarlett SimplePBR\" />\n"
    "  <ExecutableList>\n"
    "    <Executable Name=\"gdk_simple_pbr.exe\"\n"
    "                Id=\"Game\"\n"
    "                TargetDeviceFamily=\"Scarlett\" />\n"
    "  </ExecutableList>\n"
    "</Game>\n";

struct TelemetryRow {
    std::string module;
    std::string symbol;
    int32_t slot = -1;
    uint64_t total = 0;
    uint64_t ok = 0;
    uint64_t stub = 0;
    uint64_t not_impl = 0;
    uint64_t errors = 0;
};

struct TelemetrySummary {
    std::vector<TelemetryRow> rows;
    std::set<std::string> modules;
    uint64_t total_calls = 0;
    uint64_t total_not_impl = 0;
    uint64_t total_errors = 0;

    bool HasSymbol(const std::string& sym, int32_t expected_slot = -999, uint64_t min_ok = 1) const {
        for (const auto& r : rows) {
            if (r.symbol == sym) {
                if (expected_slot == -999 || r.slot == expected_slot) {
                    return r.total >= min_ok && r.ok >= min_ok;
                }
            }
        }
        return false;
    }

    bool HasModule(const std::string& mod) const {
        return modules.find(mod) != modules.end();
    }
};

struct DirFileDigest {
    std::string rel_name;
    uint64_t byte_size = 0;
    uint64_t fnv1a_64 = 0;

    bool operator==(const DirFileDigest& other) const noexcept {
        return rel_name == other.rel_name &&
               byte_size == other.byte_size &&
               fnv1a_64 == other.fnv1a_64;
    }
};

struct RunReport {
    std::string mode;
    std::map<std::string, std::string> metrics;
    TelemetrySummary telemetry;
    std::vector<uint8_t> ppm_rgb;
    double wall_ms = 0.0;
};

uint64_t Fnv1a64(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::vector<uint8_t> ReadBinaryFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    ifs.seekg(0, std::ios::end);
    const std::streamoff sz = ifs.tellg();
    OS_REQUIRE(sz >= 0);
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
        OS_REQUIRE(static_cast<bool>(ifs));
    }
    return buf;
}

void WriteBinaryFile(const fs::path& p, const void* data, size_t size) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(static_cast<bool>(ofs));
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    OS_REQUIRE(static_cast<bool>(ofs));
}

std::map<std::string, std::string> ParseKeyValueFile(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::istringstream iss(ReadTextFile(p));
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return kv;
}

std::vector<DirFileDigest> SnapshotDirectoryBytes(const fs::path& dir) {
    std::vector<DirFileDigest> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        OS_REQUIRE(entry.is_regular_file());
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(ext != ".dll");

        const auto bytes = ReadBinaryFile(entry.path());
        DirFileDigest d{};
        d.rel_name = fs::relative(entry.path(), dir).generic_string();
        d.byte_size = bytes.size();
        d.fnv1a_64 = Fnv1a64(bytes.data(), bytes.size());
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const DirFileDigest& a, const DirFileDigest& b) {
        return a.rel_name < b.rel_name;
    });
    return out;
}

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"openscarlett.exe", ec) &&
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec)) {
            return exe_dir;
        }
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path ResolveScratchDir(const wchar_t* subdir_name) {
    std::error_code ec;
    wchar_t temp_buf[1024]{};
    const DWORD tlen = ::GetEnvironmentVariableW(L"TEMP", temp_buf, static_cast<DWORD>(1024));
    if (tlen > 0 && tlen < 1024) {
        return fs::path(temp_buf) / subdir_name;
    }
    const fs::path sys_tmp = fs::temp_directory_path(ec);
    if (!ec && !sys_tmp.empty()) {
        return sys_tmp / subdir_name;
    }
    return ResolveRuntimeBinDir() / subdir_name;
}

fs::path FindDxcExe() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const auto beside = fs::path(self_buf).parent_path() / L"dxc.exe";
        if (fs::exists(beside, ec)) return beside;
    }
    wchar_t env_dxc[1024]{};
    const DWORD env_len = ::GetEnvironmentVariableW(
        L"OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(1024));
    if (env_len > 0 && env_len < 1024 && fs::exists(env_dxc, ec)) {
        return fs::path(env_dxc);
    }
    wchar_t path_buf[MAX_PATH]{};
    const DWORD path_len = ::SearchPathW(
        nullptr, L"dxc.exe", nullptr, static_cast<DWORD>(MAX_PATH), path_buf, nullptr);
    if (path_len > 0 && path_len < MAX_PATH && fs::exists(path_buf, ec)) {
        return fs::path(path_buf);
    }
    const wchar_t* candidates[] = {
        LR"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe)",
        LR"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe)",
    };
    for (const wchar_t* p : candidates) {
        if (fs::exists(p, ec)) return fs::path(p);
    }
    const fs::path kits_bin(LR"(C:\Program Files (x86)\Windows Kits\10\bin)");
    if (fs::exists(kits_bin, ec)) {
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(kits_bin, ec)) {
            if (!entry.is_directory()) continue;
            const auto cand = entry.path() / L"x64" / L"dxc.exe";
            if (fs::exists(cand, ec)) found.push_back(cand);
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.back();
    }
    return {};
}

DWORD RunProcessW(const fs::path& exe_path, const std::wstring& cmd_line, DWORD timeout_ms = 45000) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    const BOOL ok = ::CreateProcessW(
        exe_path.c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi);
    OS_REQUIRE(ok != 0);
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    OS_REQUIRE(wr == WAIT_OBJECT_0);
    DWORD exit_code = 999;
    OS_REQUIRE(::GetExitCodeProcess(pi.hProcess, &exit_code) != 0);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return exit_code;
}

void CompileShaderWithDxc(const fs::path& dxc_exe,
                          const fs::path& hlsl_path,
                          const fs::path& cso_out_path,
                          const std::wstring& profile,
                          const std::wstring& entry_point) {
    const std::wstring cmd =
        L"\"" + dxc_exe.wstring() + L"\" -T " + profile +
        L" -E " + entry_point + L" -O3 -WX" +
        L" -Fo \"" + cso_out_path.wstring() + L"\" \"" + hlsl_path.wstring() + L"\"";
    const DWORD rc = RunProcessW(dxc_exe, cmd, 30000);
    OS_REQUIRE(rc == 0);
    OS_REQUIRE(fs::exists(cso_out_path));
    OS_REQUIRE(fs::file_size(cso_out_path) > 64u);
}

bool ExtractJsonString(const std::string& line, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    std::string val;
    while (pos < line.size()) {
        const char c = line[pos++];
        if (c == '\\' && pos < line.size()) {
            val.push_back(line[pos++]);
        } else if (c == '"') {
            out = std::move(val);
            return true;
        } else {
            val.push_back(c);
        }
    }
    return false;
}

bool ExtractJsonInt(const std::string& line, const std::string& key, int64_t& out) {
    const std::string pat = "\"" + key + "\": ";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    char* end_ptr = nullptr;
    const long long v = std::strtoll(line.c_str() + pos, &end_ptr, 10);
    if (end_ptr == line.c_str() + pos) return false;
    out = static_cast<int64_t>(v);
    return true;
}

TelemetrySummary ParseTelemetryJson(const fs::path& json_path) {
    OS_REQUIRE(fs::exists(json_path));
    TelemetrySummary sum;
    std::istringstream iss(ReadTextFile(json_path));
    std::string line;
    while (std::getline(iss, line)) {
        TelemetryRow row{};
        if (!ExtractJsonString(line, "module", row.module) ||
            !ExtractJsonString(line, "symbol", row.symbol)) {
            continue;
        }
        int64_t v = 0;
        if (ExtractJsonInt(line, "slot", v)) row.slot = static_cast<int32_t>(v);
        if (ExtractJsonInt(line, "total", v)) row.total = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "ok", v)) row.ok = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "stub", v)) row.stub = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "not_impl", v)) row.not_impl = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "errors", v)) row.errors = static_cast<uint64_t>(v);

        sum.modules.insert(row.module);
        sum.total_calls += row.total;
        sum.total_not_impl += row.not_impl;
        sum.total_errors += row.errors;
        sum.rows.push_back(std::move(row));
    }
    return sum;
}

std::vector<uint8_t> VerifyAndLoadPpm(const fs::path& ppm_path, uint32_t width, uint32_t height) {
    OS_REQUIRE(fs::exists(ppm_path));
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const size_t rgb_bytes = static_cast<size_t>(width) * height * 3u;
    const auto all_bytes = ReadBinaryFile(ppm_path);
    OS_REQUIRE(all_bytes.size() == header.size() + rgb_bytes);
    OS_REQUIRE(std::memcmp(all_bytes.data(), header.data(), header.size()) == 0);
    return std::vector<uint8_t>(all_bytes.begin() + static_cast<std::ptrdiff_t>(header.size()),
                                all_bytes.end());
}

void VerifyStandaloneXboxPeImports(const fs::path& exe_path,
                                   const std::vector<std::string>& required_dlls) {
    openscarlett::PeInspectionReport rep;
    OS_REQUIRE(openscarlett::InspectPeFile(exe_path.string(), rep).ok());
    OS_REQUIRE(!rep.is_dll);
    OS_REQUIRE(rep.console_imports + rep.gdk_imports > 0u);

    std::set<std::string> imported_lower;
    for (const auto& mod : rep.imports) {
        std::string lo = mod.dll_name;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(lo != "openscarlett_hook.dll");
        OS_REQUIRE(lo != "d3d12.dll");
        imported_lower.insert(lo);
    }
    for (const auto& req : required_dlls) {
        std::string lo = req;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(imported_lower.find(lo) != imported_lower.end());
    }
}

DWORD LaunchPackageDirViaOpenScarlettCli(const fs::path& cli_exe,
                                         const fs::path& package_dir,
                                         const fs::path& shim_dir,
                                         const fs::path& vfs_g_overlay,
                                         const fs::path& vfs_t,
                                         const fs::path& vfs_s,
                                         const fs::path& telemetry_json,
                                         const std::wstring& guest_args = {}) {
    // Pass the PACKAGE DIRECTORY (not the .exe path) and omit --vfs-g so Wave 1's
    // MicrosoftGame.Config auto-resolver selects gdk_simple_pbr.exe, G:\ root, and TitleId!
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + package_dir.wstring() +
        L"\" --shim-dir \"" + shim_dir.wstring() +
        L"\" --vfs-g-overlay \"" + vfs_g_overlay.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --vfs-s \"" + vfs_s.wstring() +
        L"\" --telemetry \"" + telemetry_json.wstring() +
        L"\" --timeout-ms 45000";
    if (!guest_args.empty()) {
        cmd += L" -- " + guest_args;
    }
    return RunProcessW(cli_exe, cmd, 50000);
}

void VerifySimplePbrRun(const std::string& mode,
                        const fs::path& cli_exe,
                        const fs::path& pkg_dir,
                        const fs::path& build_dir,
                        const fs::path& stage_root,
                        const std::vector<DirFileDigest>& pkg_before,
                        const std::wstring& guest_args,
                        RunReport& out_report) {
    const fs::path run_ovl = stage_root / ("run_" + mode + "_ovl");
    const fs::path run_t = stage_root / ("run_" + mode + "_T");
    const fs::path run_s = stage_root / ("run_" + mode + "_S");
    const fs::path run_tel = stage_root / ("telemetry_" + mode + ".json");

    const auto t0 = std::chrono::steady_clock::now();
    const DWORD rc = LaunchPackageDirViaOpenScarlettCli(
        cli_exe, pkg_dir, build_dir, run_ovl, run_t, run_s, run_tel, guest_args);
    out_report.wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    OS_REQUIRE(rc == 0);

    // 1. Target package directory remains zero-DLL and 100% byte-identical!
    const auto pkg_after = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_after == pkg_before);

    // 2. Verify 256x256 P6 PPM output
    out_report.ppm_rgb = VerifyAndLoadPpm(run_t / "simple_pbr_frame2.ppm", 256, 256);

    // 3. Verify metrics.txt (TitleId, 4x MSAA, 3 distinct frame hashes, shading & depth occlusion)
    out_report.mode = mode;
    out_report.metrics = ParseKeyValueFile(run_t / "metrics.txt");
    const auto& m = out_report.metrics;
    OS_REQUIRE(m.at("sample") == "SimplePBR");
    OS_REQUIRE(m.at("title_id") == "0x5CA01001");
    OS_REQUIRE(m.at("width") == "256");
    OS_REQUIRE(m.at("height") == "256");
    OS_REQUIRE(m.at("msaa_samples") == "4");
    OS_REQUIRE(m.at("depth_occlusion_verified") == "1");
    OS_REQUIRE(std::stoull(m.at("triangle_count")) >= 500ull);
    OS_REQUIRE(m.at("frame0_fnv1a") != m.at("frame1_fnv1a"));
    OS_REQUIRE(m.at("frame1_fnv1a") != m.at("frame2_fnv1a"));
    OS_REQUIRE(m.at("frame0_fnv1a") != m.at("frame2_fnv1a"));
    OS_REQUIRE(std::stoull(m.at("specular_pixels")) >= 30ull);
    OS_REQUIRE(std::stoull(m.at("diffuse_pixels")) >= 2500ull);
    OS_REQUIRE(std::stoull(m.at("shadow_pixels")) >= 400ull);
    OS_REQUIRE(std::stoull(m.at("front_occluder_pixels")) >= 400ull);
    OS_REQUIRE(std::stoull(m.at("back_sphere_pixels")) >= 2500ull);

    // 4. Verify CallTelemetry JSON: not_impl == 0, all 5 modules, exact COM vtable slots
    out_report.telemetry = ParseTelemetryJson(run_tel);
    const auto& tel = out_report.telemetry;
    OS_REQUIRE(tel.total_not_impl == 0);
    OS_REQUIRE(tel.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(tel.HasModule("xgameruntime.dll"));
    OS_REQUIRE(tel.HasModule("GameInput.dll"));
    OS_REQUIRE(tel.HasModule("pixevt.dll"));
    OS_REQUIRE(tel.HasModule("vfs"));

    // Core D3D12X MSAA/Depth/Texture/Indexed/PresentX vtable slots
    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::CreateDepthStencilView", 21, 1));
    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::CreateShaderResourceView", 18, 2));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::ClearDepthStencilView", 47, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::ResolveSubresource", 19, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 13, 9));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, 2));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::SetDescriptorHeaps", 28, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::SetGraphicsRootDescriptorTable", 32, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::SetGraphicsRootConstantBufferView", 38, 9));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::IASetIndexBuffer", 43, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::PresentX", 19, 4));  // 3 frames + PresentX(0)

    // GDK TitleId & PIX markers
    OS_REQUIRE(tel.HasSymbol("XGameGetXboxTitleId", -1, 1));
    OS_REQUIRE(tel.HasSymbol("PIXBeginEventOnCommandList", -1, 3));
    OS_REQUIRE(tel.HasSymbol("PIXEndEventOnCommandList", -1, 3));
}

}  // namespace

int main() {
    // Clear any inherited VFS_G / TitleId overrides so MicrosoftGame.Config auto-resolution is tested cleanly.
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_TITLE_ID", nullptr);

    const fs::path build_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = build_dir / L"openscarlett.exe";
    const fs::path dxc_exe = FindDxcExe();
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(build_dir / L"openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(build_dir / L"gdk_simple_pbr.exe"));
    OS_REQUIRE(!dxc_exe.empty());

    // Probe hardware D3D12 GPU availability so WARP-only CI runners pass gracefully
    // while hardware hosts (e.g. NVIDIA GTX 1650 Ti) strictly verify BOTH HW and WARP!
    bool has_hw_gpu = false;
    {
        std::vector<openscarlett::D3D12AdapterInfo> adapters;
        if (openscarlett::EnumerateD3D12Adapters(adapters).ok()) {
            for (const auto& a : adapters) {
                if (!a.is_warp) {
                    has_hw_gpu = true;
                    break;
                }
            }
        }
    }

    const fs::path stage_root = ResolveScratchDir(L"openscarlett_wave2_simple_pbr");
    std::error_code ec;
    fs::remove_all(stage_root, ec);
    fs::create_directories(stage_root, ec);
    OS_REQUIRE(fs::exists(stage_root));

    // Stage zero-DLL package directory with gdk_simple_pbr.exe, MicrosoftGame.Config, and compiled DXIL CSOs
    const fs::path pkg_dir = stage_root / L"SimplePBR_Package";
    const fs::path hlsl_dir = stage_root / L"_hlsl_src";
    fs::create_directories(pkg_dir, ec);
    fs::create_directories(hlsl_dir, ec);

    const fs::path staged_exe = pkg_dir / L"gdk_simple_pbr.exe";
    fs::copy_file(build_dir / L"gdk_simple_pbr.exe", staged_exe,
                  fs::copy_options::overwrite_existing);
    VerifyStandaloneXboxPeImports(staged_exe, {
        "d3d12_xs.dll",
        "xgameruntime.dll",
        "GameInput.dll",
        "pixevt.dll",
    });

    WriteBinaryFile(pkg_dir / L"MicrosoftGame.Config",
                    kMicrosoftGameConfigXml, std::strlen(kMicrosoftGameConfigXml));

    WriteBinaryFile(hlsl_dir / L"PbrVS.hlsl", kPbrVsHlsl, std::strlen(kPbrVsHlsl));
    WriteBinaryFile(hlsl_dir / L"PbrPS.hlsl", kPbrPsHlsl, std::strlen(kPbrPsHlsl));
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"PbrVS.hlsl", pkg_dir / L"PbrVS.cso", L"vs_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"PbrPS.hlsl", pkg_dir / L"PbrPS.cso", L"ps_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"PbrVS.hlsl", pkg_dir / L"PbrVS_sm62.cso", L"vs_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"PbrPS.hlsl", pkg_dir / L"PbrPS_sm62.cso", L"ps_6_2", L"main");

    // Pre-launch zero-DLL byte-exact snapshot of package directory
    const auto pkg_before = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_before.size() == 6u);  // exe + MicrosoftGame.Config + 4 CSOs

    RunReport hw_report{};
    if (has_hw_gpu) {
        VerifySimplePbrRun("hw", cli_exe, pkg_dir, build_dir, stage_root, pkg_before, L"", hw_report);
    }

    RunReport warp_report{};
    VerifySimplePbrRun("warp", cli_exe, pkg_dir, build_dir, stage_root, pkg_before, L"--warp", warp_report);

    // Compare HW vs WARP rendered Frame 2 images within tight MSAA/specular tolerance
    double hw_warp_mae = 0.0;
    uint32_t hw_warp_max_diff = 0;
    double cov_rel_diff = 0.0;
    if (has_hw_gpu) {
        OS_REQUIRE(hw_report.ppm_rgb.size() == warp_report.ppm_rgb.size());
        uint64_t sum_abs = 0;
        for (size_t i = 0; i < hw_report.ppm_rgb.size(); ++i) {
            const uint32_t d = static_cast<uint32_t>(std::abs(
                static_cast<int32_t>(hw_report.ppm_rgb[i]) -
                static_cast<int32_t>(warp_report.ppm_rgb[i])));
            sum_abs += d;
            if (d > hw_warp_max_diff) hw_warp_max_diff = d;
        }
        hw_warp_mae = static_cast<double>(sum_abs) / static_cast<double>(hw_report.ppm_rgb.size());

        const double hw_non_bg = std::stod(hw_report.metrics.at("non_bg_pixels"));
        const double wp_non_bg = std::stod(warp_report.metrics.at("non_bg_pixels"));
        cov_rel_diff = std::abs(hw_non_bg - wp_non_bg) / std::max(1.0, hw_non_bg);

        if (hw_warp_mae >= 3.0 || cov_rel_diff >= 0.02) {
            std::fprintf(stderr,
                         "[test_gdk_simple_pbr] HW vs WARP mismatch: MAE=%.4f maxDiff=%u covRelDiff=%.5f\n",
                         hw_warp_mae, hw_warp_max_diff, cov_rel_diff);
            OS_REQUIRE(false);
        }
    }

    // Crisp summary output
    std::printf("\n===================================================================================================================\n");
    std::printf(" OpenScarlett Wave 2 - SimplePBR Multi-Frame 3D Cook-Torrance PBR (4x MSAA + D32_FLOAT + ResolveSubresource)\n");
    std::printf("===================================================================================================================\n");
    auto print_report = [](const RunReport& r) {
        const auto& m = r.metrics;
        std::printf(" Mode        : %s (%s)\n", r.mode.c_str(), m.at("adapter").c_str());
        std::printf(" Geometry    : %s verts / %s indices / %s tris | %sx%s %sx MSAA\n",
                    m.at("vertex_count").c_str(), m.at("index_count").c_str(),
                    m.at("triangle_count").c_str(), m.at("width").c_str(),
                    m.at("height").c_str(), m.at("msaa_samples").c_str());
        std::printf(" Frame Hashes: F0=%s  F1=%s  F2=%s\n",
                    m.at("frame0_fnv1a").c_str(),
                    m.at("frame1_fnv1a").c_str(),
                    m.at("frame2_fnv1a").c_str());
        std::printf(" PBR Pixels  : Spec=%s | Diff=%s | Shad=%s | Front=%s | Back=%s | BG=%s | Colors=%s\n",
                    m.at("specular_pixels").c_str(), m.at("diffuse_pixels").c_str(),
                    m.at("shadow_pixels").c_str(), m.at("front_occluder_pixels").c_str(),
                    m.at("back_sphere_pixels").c_str(), m.at("bg_pixels").c_str(),
                    m.at("unique_colors").c_str());
        std::printf(" Telemetry   : %llu calls across %zu symbols / %zu modules (not_impl=%llu) | GPU=%s ms | Wall=%.1f ms\n",
                    static_cast<unsigned long long>(r.telemetry.total_calls),
                    r.telemetry.rows.size(),
                    r.telemetry.modules.size(),
                    static_cast<unsigned long long>(r.telemetry.total_not_impl),
                    m.at("gpu_total_ms").c_str(),
                    r.wall_ms);
        std::printf("-------------------------------------------------------------------------------------------------------------------\n");
    };
    if (has_hw_gpu) {
        print_report(hw_report);
    }
    print_report(warp_report);
    if (has_hw_gpu) {
        std::printf(" HW vs WARP  : Frame2 RGB MAE=%.4f / 255 | MaxChannelDiff=%u | CoverageRelDiff=%.5f\n",
                    hw_warp_mae, hw_warp_max_diff, cov_rel_diff);
        std::printf("===================================================================================================================\n");
    }
    std::printf("[PASS] test_gdk_simple_pbr: SimplePBR passed on %s with 0 blockers and 100%% byte-identical zero-DLL package!\n",
                has_hw_gpu ? "HW + WARP" : "WARP (CI)");
    return 0;
}
