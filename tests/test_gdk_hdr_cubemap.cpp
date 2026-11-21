// OpenScarlett Wave 4 — Agent G: HDRCubeMap Real-Binary GDK End-to-End Test Harness.
//
// 100% Portable for CI hidden-build smoke tests:
//   - Resolves openscarlett.exe / openscarlett_hook.dll / gdk_hdr_cubemap.exe via GetModuleFileNameW.
//   - Resolves dxc.exe portably (exe dir -> OPENSCARLETT_DXC_PATH -> PATH -> Windows Kits).
//   - Stages a zero-DLL Xbox package folder under %TEMP% with MicrosoftGame.Config,
//     compiled SM 6.5 + SM 6.2 DXIL shaders, and gdk_hdr_cubemap.exe.
//   - Launches via `openscarlett.exe run <package_dir>` on:
//       1. Live hardware GPU (e.g. NVIDIA GTX 1650 Ti) with OPENSCARLETT_FORCE_HDR=1 (HDR10 + ACES)
//       2. Live hardware GPU with OPENSCARLETT_FORCE_HDR=0 (SDR fallback + Reinhard)
//       3. D3D12 WARP (`-- --warp`) with OPENSCARLETT_FORCE_HDR=1
//   - Verifies zero-DLL package cleanliness, XDisplayTryEnableHdrMode HDR10 luminance metadata,
//     all 6 TextureCube faces + all 6 default (nullptr desc) Texture2DArray SRV slices (ScarlettX #46),
//     HDR10 ST.2084 decoded peak luminance (> 300 nits) vs dark shadows (< 50 nits),
//     automatic SDR ACES & Reinhard tone-mapped RGBA8 output, HW-vs-WARP pixel MAE parity,
//     and CallTelemetry (not_impl == 0, CreateShaderResourceView[DefaultArrayPreserved], HDR10 PresentX).

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

constexpr const char* kSkyboxVsHlsl = R"HLSL(
#define SKYBOX_ROOT_SIG \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL)," \
    "StaticSampler(s0," \
        "filter=FILTER_MIN_MAG_MIP_LINEAR," \
        "addressU=TEXTURE_ADDRESS_CLAMP," \
        "addressV=TEXTURE_ADDRESS_CLAMP," \
        "addressW=TEXTURE_ADDRESS_CLAMP," \
        "visibility=SHADER_VISIBILITY_PIXEL)"

struct VsOut {
    float4 PositionCS : SV_Position;
    float2 TexCoord   : TEXCOORD0;
};

[RootSignature(SKYBOX_ROOT_SIG)]
VsOut main(uint vid : SV_VertexID) {
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.TexCoord = uv;
    o.PositionCS = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
)HLSL";

constexpr const char* kSkyboxPsHlsl = R"HLSL(
#define SKYBOX_ROOT_SIG \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL)," \
    "StaticSampler(s0," \
        "filter=FILTER_MIN_MAG_MIP_LINEAR," \
        "addressU=TEXTURE_ADDRESS_CLAMP," \
        "addressV=TEXTURE_ADDRESS_CLAMP," \
        "addressW=TEXTURE_ADDRESS_CLAMP," \
        "visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer SceneConstants : register(b0) {
    row_major float4x4 g_World;
    row_major float4x4 g_ViewProj;
    float4 g_CameraPos;
    float4 g_CamRight;
    float4 g_CamUp;
    float4 g_CamForward;
    float4 g_ViewportParams;
};

TextureCube<float4>    g_EnvCube     : register(t0);
Texture2DArray<float4> g_EnvArray    : register(t1);
SamplerState           g_LinearClamp : register(s0);

struct VsOut {
    float4 PositionCS : SV_Position;
    float2 TexCoord   : TEXCOORD0;
};

float3 CubeFaceDir(uint face, float u, float v) {
    float s = u * 2.0f - 1.0f;
    float t = v * 2.0f - 1.0f;
    if (face == 0u) return normalize(float3( 1.0f, -t,   -s));
    if (face == 1u) return normalize(float3(-1.0f, -t,    s));
    if (face == 2u) return normalize(float3( s,     1.0f, t));
    if (face == 3u) return normalize(float3( s,    -1.0f,-t));
    if (face == 4u) return normalize(float3( s,    -t,    1.0f));
    return normalize(float3(-s, -t, -1.0f));
}

[RootSignature(SKYBOX_ROOT_SIG)]
float4 main(VsOut input) : SV_TARGET0 {
    float px = input.PositionCS.x;
    float py = input.PositionCS.y;
    float width = g_ViewportParams.x;
    float sceneH = g_ViewportParams.z;

    if (py < sceneH) {
        float ndcX = (px / width) * 2.0f - 1.0f;
        float ndcY = 1.0f - (py / sceneH) * 2.0f;
        float tanHalfFov = g_ViewportParams.w;
        float aspect = g_CamRight.w;
        float3 dir = normalize(
            g_CamForward.xyz +
            g_CamRight.xyz * (ndcX * aspect * tanHalfFov) +
            g_CamUp.xyz * (ndcY * tanHalfFov));
        return float4(g_EnvCube.SampleLevel(g_LinearClamp, dir, 0.0f).rgb, 1.0f);
    }

    float panelW = width / 6.0f;
    uint faceIdx = min(5u, (uint)(px / panelW));
    float localU = saturate((px - (float)faceIdx * panelW) / panelW);
    float u = lerp(0.08f, 0.92f, localU);

    if (py < sceneH + 32.0f) {
        // Row A: Explicit TextureCube SRV (t0) across all 6 faces
        float localV = saturate((py - sceneH) / 32.0f);
        float v = lerp(0.08f, 0.92f, localV);
        float3 dir = CubeFaceDir(faceIdx, u, v);
        return float4(g_EnvCube.SampleLevel(g_LinearClamp, dir, 0.0f).rgb, 1.0f);
    }

    // Row B: Default (nullptr desc) Texture2DArray SRV (t1) across all 6 slices
    float localV = saturate((py - (sceneH + 32.0f)) / 32.0f);
    float v = lerp(0.08f, 0.92f, localV);
    return float4(g_EnvArray.SampleLevel(g_LinearClamp, float3(u, v, (float)faceIdx), 0.0f).rgb, 1.0f);
}
)HLSL";

constexpr const char* kSphereVsHlsl = R"HLSL(
#define SPHERE_ROOT_SIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL)," \
    "StaticSampler(s0," \
        "filter=FILTER_MIN_MAG_MIP_LINEAR," \
        "addressU=TEXTURE_ADDRESS_CLAMP," \
        "addressV=TEXTURE_ADDRESS_CLAMP," \
        "addressW=TEXTURE_ADDRESS_CLAMP," \
        "visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer SceneConstants : register(b0) {
    row_major float4x4 g_World;
    row_major float4x4 g_ViewProj;
    float4 g_CameraPos;
    float4 g_CamRight;
    float4 g_CamUp;
    float4 g_CamForward;
    float4 g_ViewportParams;
};

struct VsIn {
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
};

struct VsOut {
    float4 PositionCS : SV_Position;
    float3 WorldPos   : POSITION0;
    float3 WorldNorm  : NORMAL0;
};

[RootSignature(SPHERE_ROOT_SIG)]
VsOut main(VsIn input) {
    VsOut o;
    float4 worldPos = mul(float4(input.Position, 1.0f), g_World);
    o.PositionCS = mul(worldPos, g_ViewProj);
    o.WorldPos = worldPos.xyz;
    o.WorldNorm = normalize(mul(float4(input.Normal, 0.0f), g_World).xyz);
    return o;
}
)HLSL";

constexpr const char* kSpherePsHlsl = R"HLSL(
#define SPHERE_ROOT_SIG \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL)," \
    "StaticSampler(s0," \
        "filter=FILTER_MIN_MAG_MIP_LINEAR," \
        "addressU=TEXTURE_ADDRESS_CLAMP," \
        "addressV=TEXTURE_ADDRESS_CLAMP," \
        "addressW=TEXTURE_ADDRESS_CLAMP," \
        "visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer SceneConstants : register(b0) {
    row_major float4x4 g_World;
    row_major float4x4 g_ViewProj;
    float4 g_CameraPos;
    float4 g_CamRight;
    float4 g_CamUp;
    float4 g_CamForward;
    float4 g_ViewportParams;
};

TextureCube<float4>    g_EnvCube     : register(t0);
Texture2DArray<float4> g_EnvArray    : register(t1);
SamplerState           g_LinearClamp : register(s0);

struct VsOut {
    float4 PositionCS : SV_Position;
    float3 WorldPos   : POSITION0;
    float3 WorldNorm  : NORMAL0;
};

[RootSignature(SPHERE_ROOT_SIG)]
float4 main(VsOut input) : SV_TARGET0 {
    float3 N = normalize(input.WorldNorm);
    float3 V = normalize(g_CameraPos.xyz - input.WorldPos);
    float3 R = reflect(-V, N);

    float NdotV = saturate(dot(N, V));
    float fresnel = 0.72f + 0.28f * pow(1.0f - NdotV, 5.0f);
    float3 envRefl = g_EnvCube.SampleLevel(g_LinearClamp, R, 0.0f).rgb;
    float hemiShadow = lerp(0.30f, 1.0f, saturate(N.y * 0.65f + 0.55f));
    return float4(envRefl * fresnel * hemiShadow, 1.0f);
}
)HLSL";

constexpr const char* kHdr10EncodeVsHlsl = R"HLSL(
#define HDR10_ROOT_SIG \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_PIXEL)," \
    "DescriptorTable(SRV(t0, numDescriptors=1), visibility=SHADER_VISIBILITY_PIXEL)"

struct VsOut {
    float4 PositionCS : SV_Position;
};

[RootSignature(HDR10_ROOT_SIG)]
VsOut main(uint vid : SV_VertexID) {
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.PositionCS = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
)HLSL";

constexpr const char* kHdr10EncodePsHlsl = R"HLSL(
#define HDR10_ROOT_SIG \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_PIXEL)," \
    "DescriptorTable(SRV(t0, numDescriptors=1), visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer Hdr10EncodeConstants : register(b0) {
    float g_LinearToNits;
    float g_MaxDisplayNits;
    float g_Pad0;
    float g_Pad1;
};

Texture2D<float4> g_HdrScene : register(t0);

struct VsOut {
    float4 PositionCS : SV_Position;
};

float3 Rec709ToBt2020(float3 c) {
    float3 r;
    r.r = 0.6274040f * c.r + 0.3292820f * c.g + 0.0433136f * c.b;
    r.g = 0.0690970f * c.r + 0.9195400f * c.g + 0.0113612f * c.b;
    r.b = 0.0163916f * c.r + 0.0880132f * c.g + 0.8955950f * c.b;
    return max(r, 0.0f);
}

float3 LinearNitsToPqSt2084(float3 nits) {
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float3 Y = saturate(nits / 10000.0f);
    float3 Ym1 = pow(max(Y, 0.0f), m1);
    return saturate(pow((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2));
}

[RootSignature(HDR10_ROOT_SIG)]
float4 main(VsOut input) : SV_TARGET0 {
    int2 xy = int2((int)input.PositionCS.x, (int)input.PositionCS.y);
    float3 lin709 = max(g_HdrScene.Load(int3(xy, 0)).rgb, 0.0f);
    float3 lin2020 = Rec709ToBt2020(lin709);
    float3 nits = min(lin2020 * g_LinearToNits, g_MaxDisplayNits);
    float3 pq = LinearNitsToPqSt2084(nits);
    return float4(pq, 1.0f);
}
)HLSL";

constexpr const char* kMicrosoftGameConfigXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Game configVersion=\"1\">\n"
    "  <Identity Name=\"OpenScarlett.HDRCubeMap\"\n"
    "            Publisher=\"CN=OpenScarlett, O=MIT\"\n"
    "            Version=\"1.0.0.0\" />\n"
    "  <TitleId>0x5CA01003</TitleId>\n"
    "  <MSAAppId>000000004C261003</MSAAppId>\n"
    "  <ShellVisuals DefaultDisplayName=\"OpenScarlett HDRCubeMap\" />\n"
    "  <ExecutableList>\n"
    "    <Executable Name=\"gdk_hdr_cubemap.exe\"\n"
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
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec) &&
            fs::exists(exe_dir / L"gdk_hdr_cubemap.exe", ec)) {
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

void VerifyHdrCubeMapRun(const std::string& mode,
                         const fs::path& cli_exe,
                         const fs::path& pkg_dir,
                         const fs::path& build_dir,
                         const fs::path& stage_root,
                         const std::vector<DirFileDigest>& pkg_before,
                         bool force_hdr,
                         const std::wstring& guest_args,
                         RunReport& out_report) {
    ::SetEnvironmentVariableW(L"OPENSCARLETT_FORCE_HDR", force_hdr ? L"1" : L"0");

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

    // 1. Package directory remains zero-DLL and 100% byte-identical
    const auto pkg_after = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_after == pkg_before);

    // 2. Verify automatic SDR tone-mapped 256x256 P6 PPM output
    out_report.ppm_rgb = VerifyAndLoadPpm(run_t / "hdr_cubemap_sdr.ppm", 256, 256);

    // 3. Verify metrics.txt
    out_report.mode = mode;
    out_report.metrics = ParseKeyValueFile(run_t / "metrics.txt");
    const auto& m = out_report.metrics;
    OS_REQUIRE(m.at("sample") == "HDRCubeMap");
    OS_REQUIRE(m.at("title_id") == "0x5CA01003");
    OS_REQUIRE(m.at("width") == "256");
    OS_REQUIRE(m.at("height") == "256");
    OS_REQUIRE(m.at("hdr_mode_enabled") == (force_hdr ? "1" : "0"));
    if (force_hdr) {
        OS_REQUIRE(std::stod(m.at("max_tone_map_nits")) >= 1000.0);
        OS_REQUIRE(m.at("tonemap_mode") == "1");  // ACES
    } else {
        OS_REQUIRE(m.at("tonemap_fallback_active") == "1");
        OS_REQUIRE(m.at("tonemap_mode") == "2");  // Reinhard
    }
    OS_REQUIRE(m.at("frame0_fnv1a") != m.at("frame1_fnv1a"));
    OS_REQUIRE(std::stod(m.at("scene_peak_nits")) > 300.0);
    OS_REQUIRE(std::stod(m.at("scene_min_nits")) < 50.0);
    OS_REQUIRE(std::stoull(m.at("hdr_bright_pixels")) >= 100ull);
    OS_REQUIRE(std::stoull(m.at("hdr_dark_pixels")) >= 200ull);
    OS_REQUIRE(m.at("cubemap_faces_verified") == "6");
    OS_REQUIRE(m.at("default_array_slices_verified") == "6");
    OS_REQUIRE(std::stoull(m.at("sdr_unique_colors")) >= 500ull);

    // 4. Verify CallTelemetry JSON: not_impl == 0, DefaultArrayPreserved SRV, HDR10 PresentX, XDisplay
    out_report.telemetry = ParseTelemetryJson(run_tel);
    const auto& tel = out_report.telemetry;
    OS_REQUIRE(tel.total_not_impl == 0);
    OS_REQUIRE(tel.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(tel.HasModule("xgameruntime.dll"));
    OS_REQUIRE(tel.HasModule("pixevt.dll"));
    OS_REQUIRE(tel.HasModule("vfs"));

    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]", 18, 1));
    OS_REQUIRE(tel.HasSymbol("ID3D12DeviceX::CreateShaderResourceView", 18, 3));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::SetHdrMetaData", -1, 1));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::SetPresentConfig", -1, 1));
    OS_REQUIRE(tel.HasSymbol("ID3D12CommandQueueX::PresentX", 19, 3));  // 2 frames + PresentX(0)
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, 6));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 13, 2));
    OS_REQUIRE(tel.HasSymbol("ID3D12GraphicsCommandListX::DrawInstanced", 12, 4));

    OS_REQUIRE(tel.HasSymbol("XDisplayTryEnableHdrMode", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XDisplayAcquireTimeoutDeferral", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XDisplayCloseTimeoutDeferralHandle", -1, 1));
    OS_REQUIRE(tel.HasSymbol("XGameGetXboxTitleId", -1, 1));
    OS_REQUIRE(tel.HasSymbol("PIXBeginEventOnCommandList", -1, 2));
    OS_REQUIRE(tel.HasSymbol("PIXEndEventOnCommandList", -1, 2));
}

double ComputeRgbMae(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t& out_max_diff) {
    OS_REQUIRE(a.size() == b.size() && !a.empty());
    uint64_t sum = 0;
    uint32_t max_d = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const uint32_t d = static_cast<uint32_t>(std::abs(
            static_cast<int32_t>(a[i]) - static_cast<int32_t>(b[i])));
        sum += d;
        if (d > max_d) max_d = d;
    }
    out_max_diff = max_d;
    return static_cast<double>(sum) / static_cast<double>(a.size());
}

}  // namespace

int main() {
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_TITLE_ID", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_FORCE_HDR", nullptr);

    const fs::path build_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = build_dir / L"openscarlett.exe";
    const fs::path dxc_exe = FindDxcExe();
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(build_dir / L"openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(build_dir / L"gdk_hdr_cubemap.exe"));
    OS_REQUIRE(!dxc_exe.empty());

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

    const fs::path stage_root = ResolveScratchDir(L"openscarlett_wave4_hdr_cubemap");
    std::error_code ec;
    fs::remove_all(stage_root, ec);
    fs::create_directories(stage_root, ec);
    OS_REQUIRE(fs::exists(stage_root));

    const fs::path pkg_dir = stage_root / L"HDRCubeMap_Package";
    const fs::path hlsl_dir = stage_root / L"_hlsl_src";
    fs::create_directories(pkg_dir, ec);
    fs::create_directories(hlsl_dir, ec);

    const fs::path staged_exe = pkg_dir / L"gdk_hdr_cubemap.exe";
    fs::copy_file(build_dir / L"gdk_hdr_cubemap.exe", staged_exe,
                  fs::copy_options::overwrite_existing);
    VerifyStandaloneXboxPeImports(staged_exe, {
        "d3d12_xs.dll",
        "xgameruntime.dll",
        "pixevt.dll",
    });

    WriteBinaryFile(pkg_dir / L"MicrosoftGame.Config",
                    kMicrosoftGameConfigXml, std::strlen(kMicrosoftGameConfigXml));

    WriteBinaryFile(hlsl_dir / L"HdrSkyboxVS.hlsl", kSkyboxVsHlsl, std::strlen(kSkyboxVsHlsl));
    WriteBinaryFile(hlsl_dir / L"HdrSkyboxPS.hlsl", kSkyboxPsHlsl, std::strlen(kSkyboxPsHlsl));
    WriteBinaryFile(hlsl_dir / L"HdrSphereVS.hlsl", kSphereVsHlsl, std::strlen(kSphereVsHlsl));
    WriteBinaryFile(hlsl_dir / L"HdrSpherePS.hlsl", kSpherePsHlsl, std::strlen(kSpherePsHlsl));
    WriteBinaryFile(hlsl_dir / L"Hdr10EncodeVS.hlsl", kHdr10EncodeVsHlsl, std::strlen(kHdr10EncodeVsHlsl));
    WriteBinaryFile(hlsl_dir / L"Hdr10EncodePS.hlsl", kHdr10EncodePsHlsl, std::strlen(kHdr10EncodePsHlsl));

    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSkyboxVS.hlsl", pkg_dir / L"HdrSkyboxVS.cso", L"vs_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSkyboxPS.hlsl", pkg_dir / L"HdrSkyboxPS.cso", L"ps_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSphereVS.hlsl", pkg_dir / L"HdrSphereVS.cso", L"vs_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSpherePS.hlsl", pkg_dir / L"HdrSpherePS.cso", L"ps_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"Hdr10EncodeVS.hlsl", pkg_dir / L"Hdr10EncodeVS.cso", L"vs_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"Hdr10EncodePS.hlsl", pkg_dir / L"Hdr10EncodePS.cso", L"ps_6_5", L"main");

    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSkyboxVS.hlsl", pkg_dir / L"HdrSkyboxVS_sm62.cso", L"vs_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSkyboxPS.hlsl", pkg_dir / L"HdrSkyboxPS_sm62.cso", L"ps_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSphereVS.hlsl", pkg_dir / L"HdrSphereVS_sm62.cso", L"vs_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"HdrSpherePS.hlsl", pkg_dir / L"HdrSpherePS_sm62.cso", L"ps_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"Hdr10EncodeVS.hlsl", pkg_dir / L"Hdr10EncodeVS_sm62.cso", L"vs_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / L"Hdr10EncodePS.hlsl", pkg_dir / L"Hdr10EncodePS_sm62.cso", L"ps_6_2", L"main");

    const auto pkg_before = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_before.size() == 14u);  // exe + MicrosoftGame.Config + 12 CSOs

    RunReport hw_hdr_report{};
    RunReport hw_sdr_report{};
    if (has_hw_gpu) {
        VerifyHdrCubeMapRun("hw_hdr10", cli_exe, pkg_dir, build_dir, stage_root,
                            pkg_before, true, L"", hw_hdr_report);
        VerifyHdrCubeMapRun("hw_sdr", cli_exe, pkg_dir, build_dir, stage_root,
                            pkg_before, false, L"", hw_sdr_report);

        // Verify ACES (HDR10) and Reinhard (SDR fallback) tone-mapping curves are distinct!
        uint32_t tm_max_diff = 0u;
        const double tm_mae = ComputeRgbMae(hw_hdr_report.ppm_rgb, hw_sdr_report.ppm_rgb, tm_max_diff);
        OS_REQUIRE(tm_mae > 3.0 && tm_max_diff > 15u);
    }

    RunReport warp_hdr_report{};
    VerifyHdrCubeMapRun("warp_hdr10", cli_exe, pkg_dir, build_dir, stage_root,
                        pkg_before, true, L"--warp", warp_hdr_report);

    ::SetEnvironmentVariableW(L"OPENSCARLETT_FORCE_HDR", nullptr);

    double hw_warp_mae = 0.0;
    uint32_t hw_warp_max_diff = 0u;
    if (has_hw_gpu) {
        hw_warp_mae = ComputeRgbMae(hw_hdr_report.ppm_rgb, warp_hdr_report.ppm_rgb, hw_warp_max_diff);
        if (hw_warp_mae >= 2.5) {
            std::fprintf(stderr,
                         "[test_gdk_hdr_cubemap] HW vs WARP mismatch: MAE=%.4f maxDiff=%u\n",
                         hw_warp_mae, hw_warp_max_diff);
            OS_REQUIRE(false);
        }
    }

    std::printf("\n===================================================================================================================\n");
    std::printf(" OpenScarlett Wave 4 - HDRCubeMap (FP16 6-Face Cube + Default Texture2DArray SRV + HDR10 ST.2084 + SDR ToneMap)\n");
    std::printf("===================================================================================================================\n");
    auto print_report = [](const RunReport& r) {
        const auto& m = r.metrics;
        std::printf(" Mode        : %s (%s)\n", r.mode.c_str(), m.at("adapter").c_str());
        std::printf(" XDisplay    : HDR=%s | Luminance=[%s .. %s nits] (FullFrame=%s) | SwapFmt=%s SC4=%s TM=%s\n",
                    m.at("hdr_mode_enabled").c_str(),
                    m.at("min_tone_map_nits").c_str(),
                    m.at("max_tone_map_nits").c_str(),
                    m.at("max_full_frame_nits").c_str(),
                    m.at("swapchain_format").c_str(),
                    m.at("has_swapchain4").c_str(),
                    m.at("tonemap_mode").c_str());
        std::printf(" HDR10 Nits  : Scene=[%s .. %s nits] (Peak=%s) | Bright=%s Dark=%s | Cube=%s/6 DefaultArray=%s/6 (Diff=%s)\n",
                    m.at("scene_min_nits").c_str(),
                    m.at("scene_peak_nits").c_str(),
                    m.at("overall_peak_nits").c_str(),
                    m.at("hdr_bright_pixels").c_str(),
                    m.at("hdr_dark_pixels").c_str(),
                    m.at("cubemap_faces_verified").c_str(),
                    m.at("default_array_slices_verified").c_str(),
                    m.at("cube_vs_array_max_nit_diff").c_str());
        std::printf(" SDR Output  : F0=%s  F1=%s | UniqueColors=%s | MeanLuma=%s\n",
                    m.at("frame0_fnv1a").c_str(),
                    m.at("frame1_fnv1a").c_str(),
                    m.at("sdr_unique_colors").c_str(),
                    m.at("sdr_mean_luma").c_str());
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
        print_report(hw_hdr_report);
        print_report(hw_sdr_report);
    }
    print_report(warp_hdr_report);
    if (has_hw_gpu) {
        std::printf(" HW vs WARP  : RGB MAE = %.4f / 255 (max channel diff = %u) | PASS\n",
                    hw_warp_mae, hw_warp_max_diff);
        std::printf("===================================================================================================================\n");
    }

    fs::remove_all(stage_root, ec);
    return 0;
}
