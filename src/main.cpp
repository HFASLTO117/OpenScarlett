#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "openscarlett/gpu/vulkan_backend.h"
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
            bool is_sw = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

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

    const char* dxc_candidates[] = {
        R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe)",
        R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe)",
    };
    bool found_dxc = false;
    for (const char* p : dxc_candidates) {
        if (std::filesystem::exists(p)) {
            std::printf("DXC (DXIL)   : %s\n", p);
            found_dxc = true;
            break;
        }
    }
    if (!found_dxc) {
        std::printf("DXC (DXIL)   : not detected in default Windows Kits paths\n");
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

void PrintUsage() {
    std::printf(
        "OpenScarlett — Clean-room Xbox Series X|S (Scarlett) translation layer for Windows x64\n"
        "Usage:\n"
        "  openscarlett probe-gpu               Probe host D3D12, Vulkan, and DXC capabilities\n"
        "  openscarlett inspect <exe_or_dll>    Inspect PE32+ x64 imports & console DLL dependencies\n");
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
    PrintUsage();
    return 1;
}
