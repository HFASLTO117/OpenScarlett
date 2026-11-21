#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/loader/apc_injector.h"
#include "openscarlett/loader/pe_inspector.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

int RunProbeGpu() {
    std::printf("=== OpenScarlett Host Hardware & Toolchain Probe ===\n");
#if defined(_WIN32)
    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    std::printf("Platform     : Windows x64\n");

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        for (UINT idx = 0;; ++idx) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            bool is_sw = ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) ||
                         (desc.VendorId == 0x1414u &&
                          (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0));

            Microsoft::WRL::ComPtr<ID3D12Device> dev;
            HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
            if (FAILED(hr)) continue;

            D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
            dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1));
            dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
            dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7));
            dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));

            std::printf("D3D12 [%u]    : %s (%s) | VRAM=%llu MB | Wave=%u..%u | SM=0x%02X | DXR=%u | MeshTier=%u\n",
                        idx,
                        name,
                        is_sw ? "WARP" : "Hardware",
                        static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024ull * 1024ull)),
                        opt1.WaveLaneCountMin,
                        opt1.WaveLaneCountMax,
                        static_cast<unsigned>(sm.HighestShaderModel),
                        static_cast<unsigned>(opt5.RaytracingTier),
                        static_cast<unsigned>(opt7.MeshShaderTier));
        }
    }
#endif

    openscarlett::VulkanBackend vk;
    auto vk_status = vk.Initialize(true);
    if (vk_status.ok()) {
        const auto& info = vk.GetDeviceInfo();
        std::printf("Vulkan       : %s | API %u.%u.%u | %s | compute_q=%u\n",
                    info.device_name.c_str(),
                    (info.api_version >> 22) & 0x3FFu,
                    (info.api_version >> 12) & 0x3FFu,
                    info.api_version & 0xFFFu,
                    info.is_hardware ? "Hardware ICD" : "Software Fallback",
                    info.compute_queue_family);
        vk.Shutdown();
    } else {
        std::printf("Vulkan       : unavailable (%s)\n", vk_status.message.c_str());
    }

    std::filesystem::path detected_dxc;
    std::error_code dxc_ec;
#if defined(_WIN32)
    {
        char self_buf[MAX_PATH]{};
        const DWORD self_len = GetModuleFileNameA(nullptr, self_buf, static_cast<DWORD>(sizeof(self_buf)));
        if (self_len > 0 && self_len < sizeof(self_buf)) {
            const auto beside = std::filesystem::path(self_buf).parent_path() / "dxc.exe";
            if (std::filesystem::exists(beside, dxc_ec)) {
                detected_dxc = beside;
            }
        }
        if (detected_dxc.empty()) {
            char env_dxc[1024]{};
            const DWORD env_len = GetEnvironmentVariableA("OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(sizeof(env_dxc)));
            if (env_len > 0 && env_len < sizeof(env_dxc) && std::filesystem::exists(env_dxc, dxc_ec)) {
                detected_dxc = env_dxc;
            }
        }
        if (detected_dxc.empty()) {
            char path_buf[MAX_PATH]{};
            const DWORD path_len = SearchPathA(nullptr, "dxc.exe", nullptr, static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
            if (path_len > 0 && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, dxc_ec)) {
                detected_dxc = path_buf;
            }
        }
    }
#endif
    if (detected_dxc.empty()) {
        const char* dxc_candidates[] = {
            R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe)",
            R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe)",
        };
        for (const char* p : dxc_candidates) {
            if (std::filesystem::exists(p, dxc_ec)) {
                detected_dxc = p;
                break;
            }
        }
    }
    if (detected_dxc.empty()) {
        const std::filesystem::path kits_bin(R"(C:\Program Files (x86)\Windows Kits\10\bin)");
        if (std::filesystem::exists(kits_bin, dxc_ec)) {
            std::vector<std::filesystem::path> found;
            for (const auto& entry : std::filesystem::directory_iterator(kits_bin, dxc_ec)) {
                if (!entry.is_directory()) continue;
                const auto cand = entry.path() / "x64" / "dxc.exe";
                if (std::filesystem::exists(cand, dxc_ec)) found.push_back(cand);
            }
            std::sort(found.begin(), found.end());
            if (!found.empty()) detected_dxc = found.back();
        }
    }
    if (!detected_dxc.empty()) {
        std::printf("DXC (DXIL)   : %s\n", detected_dxc.string().c_str());
    } else {
        std::printf("DXC (DXIL)   : not detected (bundle dxc.exe or install Windows SDK)\n");
    }
    return 0;
}

int RunInspect(const std::string& path) {
    openscarlett::PeInspectionReport rep;
    auto st = openscarlett::InspectPeFile(path, rep);
    if (!st.ok()) {
        std::fprintf(stderr, "inspect failed: %s\n", st.message.c_str());
        return 1;
    }
    std::printf("File         : %s\n", rep.file_path.c_str());
    std::printf("Format       : PE32+ x64 (%s) | ImageBase=0x%llX | Size=%u B | Sections=%u\n",
                rep.is_dll ? "DLL" : "EXE",
                static_cast<unsigned long long>(rep.image_base),
                rep.image_size,
                rep.section_count);
    std::printf("Flags        : Relocs=%s | TLS=%s\n",
                rep.has_relocations ? "yes" : "stripped",
                rep.has_tls ? "yes" : "no");
    std::printf("Imports      : %u total (%u console-only, %u GDK, %u delay-loaded) across %zu DLLs\n",
                rep.total_imports,
                rep.console_imports,
                rep.gdk_imports,
                rep.delay_load_imports,
                rep.imports.size());
    for (const auto& mod : rep.imports) {
        std::printf("  - %-36s [%-16s] %zu imports\n",
                    mod.dll_name.c_str(),
                    openscarlett::DllCategoryName(mod.category),
                    mod.symbols.size());
    }
    return 0;
}

std::string QuoteWin32Arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

bool ExtractJsonStringField(const std::string& line, const std::string& key, std::string& out_val) {
    std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos += pat.size();
    std::string val;
    while (pos < line.size()) {
        char c = line[pos++];
        if (c == '\\' && pos < line.size()) {
            val.push_back(line[pos++]);
        } else if (c == '"') {
            out_val = std::move(val);
            return true;
        } else {
            val.push_back(c);
        }
    }
    return false;
}

bool ExtractJsonUintField(const std::string& line, const std::string& key, uint64_t& out_val) {
    std::string pat = "\"" + key + "\": ";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos += pat.size();
    char* end_ptr = nullptr;
    unsigned long long parsed = std::strtoull(line.c_str() + pos, &end_ptr, 10);
    if (end_ptr == line.c_str() + pos) {
        return false;
    }
    out_val = static_cast<uint64_t>(parsed);
    return true;
}

void PrintTelemetrySummary(const std::string& json_path) {
    if (json_path.empty() || !std::filesystem::exists(json_path)) {
        std::printf("Telemetry    : (no report written)\n");
        return;
    }
    std::ifstream ifs(json_path);
    if (!ifs) {
        std::printf("Telemetry    : %s (unreadable)\n", json_path.c_str());
        return;
    }

    struct BlockerRow {
        std::string module;
        std::string symbol;
        uint64_t not_impl = 0;
        uint64_t total = 0;
    };
    size_t total_entries = 0;
    std::vector<BlockerRow> blockers;

    std::string line;
    while (std::getline(ifs, line)) {
        std::string mod;
        std::string sym;
        uint64_t not_impl = 0;
        uint64_t total = 0;
        if (!ExtractJsonStringField(line, "module", mod) ||
            !ExtractJsonStringField(line, "symbol", sym)) {
            continue;
        }
        ++total_entries;
        ExtractJsonUintField(line, "not_impl", not_impl);
        ExtractJsonUintField(line, "total", total);
        if (not_impl > 0) {
            blockers.push_back(BlockerRow{std::move(mod), std::move(sym), not_impl, total});
        }
    }

    std::printf("Telemetry    : %s (%zu symbols tracked, %zu missing blockers)\n",
                json_path.c_str(),
                total_entries,
                blockers.size());
    const size_t show = std::min<size_t>(blockers.size(), 10);
    for (size_t i = 0; i < show; ++i) {
        std::printf("  [BLOCKER]  %s!%s (not_impl=%llu, total=%llu)\n",
                    blockers[i].module.c_str(),
                    blockers[i].symbol.c_str(),
                    static_cast<unsigned long long>(blockers[i].not_impl),
                    static_cast<unsigned long long>(blockers[i].total));
    }
}

int RunGuestExecutable(int argc, char** argv) {
    openscarlett::LaunchOptions opts{};
    std::vector<std::string> guest_args;
    bool in_guest_args = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (in_guest_args) {
            guest_args.push_back(arg);
            continue;
        }
        if (arg == "--") {
            in_guest_args = true;
            continue;
        }
        auto take_value = [&](const char* flag, std::string& dst) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "run: missing value for %s\n", flag);
                return false;
            }
            dst = argv[++i];
            return true;
        };

        if (arg == "--shim-dir") {
            if (!take_value("--shim-dir", opts.shim_dir)) return 1;
        } else if (arg == "--vfs-g") {
            if (!take_value("--vfs-g", opts.vfs_g)) return 1;
        } else if (arg == "--vfs-t") {
            if (!take_value("--vfs-t", opts.vfs_t)) return 1;
        } else if (arg == "--vfs-s") {
            if (!take_value("--vfs-s", opts.vfs_s)) return 1;
        } else if (arg == "--telemetry") {
            if (!take_value("--telemetry", opts.telemetry_output_path)) return 1;
        } else if (arg == "--timeout-ms") {
            std::string raw;
            if (!take_value("--timeout-ms", raw)) return 1;
            char* end_ptr = nullptr;
            unsigned long val = std::strtoul(raw.c_str(), &end_ptr, 10);
            if (!end_ptr || *end_ptr != '\0') {
                std::fprintf(stderr, "run: invalid --timeout-ms value: %s\n", raw.c_str());
                return 1;
            }
            opts.timeout_ms = static_cast<uint32_t>(val);
        } else if (opts.exe_path.empty() && (!arg.empty() && arg[0] != '-')) {
            opts.exe_path = arg;
        } else if (!opts.exe_path.empty()) {
            guest_args.push_back(arg);
        } else {
            std::fprintf(stderr, "run: unknown option: %s\n", arg.c_str());
            return 1;
        }
    }

    if (opts.exe_path.empty()) {
        std::fprintf(stderr, "run: missing target executable path\n");
        return 1;
    }

    for (size_t i = 0; i < guest_args.size(); ++i) {
        if (i > 0) opts.arguments.push_back(' ');
        opts.arguments += QuoteWin32Arg(guest_args[i]);
    }

    if (opts.telemetry_output_path.empty()) {
        std::string stem = std::filesystem::path(opts.exe_path).stem().string();
        if (stem.empty()) stem = "guest";
        std::filesystem::path tel_base;
#if defined(_WIN32)
        char temp_buf[1024]{};
        DWORD tlen = GetEnvironmentVariableA("TEMP", temp_buf, static_cast<DWORD>(sizeof(temp_buf)));
        if (tlen > 0 && tlen < sizeof(temp_buf)) {
            tel_base = std::filesystem::path(temp_buf) / "telemetry";
        } else {
            tel_base = std::filesystem::path("E:\\OpenScarlett\\.tmp\\telemetry");
        }
#else
        tel_base = std::filesystem::temp_directory_path() / "openscarlett_telemetry";
#endif
        opts.telemetry_output_path = (tel_base / (stem + "_telemetry.json")).string();
    }

    openscarlett::LaunchResult res{};
    auto st = openscarlett::LaunchWithApcHook(opts, res);

    std::printf("=== OpenScarlett Suspended-Process APC Launch ===\n");
    std::printf("Target EXE   : %s\n", opts.exe_path.c_str());
    std::printf("Shim Dir     : %s\n", res.resolved_shim_dir.c_str());
    std::printf("VFS G:\\      : %s\n", res.resolved_vfs_g.c_str());
    std::printf("VFS T:\\      : %s\n", res.resolved_vfs_t.c_str());
    std::printf("VFS S:\\      : %s\n", res.resolved_vfs_s.c_str());
    std::printf("Guest PID    : %u\n", res.process_id);
    std::printf("Exit Code    : %u%s\n", res.exit_code, res.timed_out ? " (TIMED OUT)" : "");
    PrintTelemetrySummary(res.resolved_telemetry_path);

    if (!st.ok()) {
        std::fprintf(stderr, "run failed: %s\n", st.message.c_str());
        return res.timed_out ? 124 : 1;
    }
    return static_cast<int>(res.exit_code);
}

void PrintUsage() {
    std::printf(
        "OpenScarlett — Clean-room Xbox Series X|S (Scarlett) translation layer for Windows x64\n"
        "Usage:\n"
        "  openscarlett probe-gpu               Probe host D3D12, Vulkan, and DXC capabilities\n"
        "  openscarlett inspect <exe_or_dll>    Inspect PE32+ x64 imports & console DLL dependencies\n"
        "  openscarlett run <exe> [options] [-- <guest-args...>]\n"
        "      --shim-dir <dir>                 Directory containing OpenScarlett shim DLLs\n"
        "      --vfs-g <dir>                    Backing directory for virtual G:\\ (game package)\n"
        "      --vfs-t <dir>                    Backing directory for virtual T:\\ (title scratch)\n"
        "      --vfs-s <dir>                    Backing directory for virtual S:\\ (persistent saves)\n"
        "      --telemetry <json>               Output path for unified CallTelemetry JSON report\n"
        "      --timeout-ms <ms>                Maximum runtime in milliseconds (0 = infinite)\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 0;
    }
    std::string cmd = argv[1];
    if (cmd == "probe-gpu") return RunProbeGpu();
    if (cmd == "inspect" && argc >= 3) return RunInspect(argv[2]);
    if (cmd == "run") return RunGuestExecutable(argc, argv);
    PrintUsage();
    return 1;
}
