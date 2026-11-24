#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#if defined(_MSC_VER)
#define OS_UUIDOF(T) __uuidof(T)
#else
#define OS_UUIDOF(T) IID_##T
#endif
#endif

#include "openscarlett/gpu/descriptor_sgpr_bridge.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/kernel/title_va_mirror.h"
#include "os_check.h"

namespace {

#if defined(_WIN32)

template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { Reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

struct D3D12TestContext {
    ComRef<ID3D12Device> device;
    ComRef<ID3D12CommandQueue> queue;
    ComRef<ID3D12CommandAllocator> alloc;
    ComRef<ID3D12GraphicsCommandList> cmd;
    ComRef<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0;
    std::string adapter_name;
    bool is_warp = false;

    ~D3D12TestContext() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    bool SubmitAndWait(uint64_t& out_signaled_fence) {
        if (FAILED(cmd->Close())) return false;
        ID3D12CommandList* lists[1] = {cmd.Get()};
        queue->ExecuteCommandLists(1u, lists);
        ++fence_val;
        out_signaled_fence = fence_val;
        if (FAILED(queue->Signal(fence.Get(), fence_val))) return false;
        if (fence->GetCompletedValue() < fence_val) {
            if (FAILED(fence->SetEventOnCompletion(fence_val, fence_event))) return false;
            WaitForSingleObject(fence_event, 15000u);
        }
        return fence->GetCompletedValue() >= fence_val;
    }

    bool ResetCmdList() {
        if (FAILED(alloc->Reset())) return false;
        if (FAILED(cmd->Reset(alloc.Get(), nullptr))) return false;
        return true;
    }
};

std::string WideToUtf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool CreateD3D12Context(bool use_warp, D3D12TestContext& ctx) {
    ComRef<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(OS_UUIDOF(IDXGIFactory4),
                                  reinterpret_cast<void**>(factory.Put())))) {
        return false;
    }

    ComRef<IDXGIAdapter1> adapter;
    if (use_warp) {
        if (FAILED(factory->EnumWarpAdapter(OS_UUIDOF(IDXGIAdapter1),
                                            reinterpret_cast<void**>(adapter.Put())))) {
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            ctx.adapter_name = WideToUtf8(desc.Description);
        } else {
            ctx.adapter_name = "Microsoft Basic Render Driver (WARP)";
        }
        ctx.is_warp = true;
    } else {
        // Strictly reject software / WARP adapters in hardware D3D12 initialization,
        // and prefer the discrete GPU with the largest DedicatedVideoMemory (e.g. GTX 1650 Ti).
        UINT64 best_vram = 0ull;
        bool found_any = false;
        for (UINT idx = 0;; ++idx) {
            ComRef<IDXGIAdapter1> cand;
            if (factory->EnumAdapters1(idx, cand.Put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(cand->GetDesc1(&desc))) continue;
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                (desc.VendorId == 0x1414u &&
                 (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0))) {
                continue;
            }
            if (found_any && desc.DedicatedVideoMemory <= best_vram) {
                continue;
            }
            ComRef<ID3D12Device> probe_dev;
            if (SUCCEEDED(D3D12CreateDevice(cand.Get(),
                                            D3D_FEATURE_LEVEL_11_0,
                                            OS_UUIDOF(ID3D12Device),
                                            reinterpret_cast<void**>(probe_dev.Put())))) {
                adapter.Reset(cand.Get());
                adapter->AddRef();
                ctx.device.Reset(probe_dev.Get());
                ctx.device->AddRef();
                ctx.adapter_name = WideToUtf8(desc.Description);
                ctx.is_warp = false;
                best_vram = desc.DedicatedVideoMemory;
                found_any = true;
            }
        }
        if (!ctx.device) {
            return false;
        }
    }

    if (!ctx.device) {
        if (FAILED(D3D12CreateDevice(adapter.Get(),
                                     D3D_FEATURE_LEVEL_11_0,
                                     OS_UUIDOF(ID3D12Device),
                                     reinterpret_cast<void**>(ctx.device.Put())))) {
            return false;
        }
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qdesc.NodeMask = 0u;
    if (FAILED(ctx.device->CreateCommandQueue(
            &qdesc, OS_UUIDOF(ID3D12CommandQueue), reinterpret_cast<void**>(ctx.queue.Put())))) {
        return false;
    }

    if (FAILED(ctx.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            OS_UUIDOF(ID3D12CommandAllocator),
            reinterpret_cast<void**>(ctx.alloc.Put())))) {
        return false;
    }

    if (FAILED(ctx.device->CreateCommandList(
            0u,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            ctx.alloc.Get(),
            nullptr,
            OS_UUIDOF(ID3D12GraphicsCommandList),
            reinterpret_cast<void**>(ctx.cmd.Put())))) {
        return false;
    }

    if (FAILED(ctx.device->CreateFence(
            0ull,
            D3D12_FENCE_FLAG_NONE,
            OS_UUIDOF(ID3D12Fence),
            reinterpret_cast<void**>(ctx.fence.Put())))) {
        return false;
    }

    ctx.fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    return ctx.fence_event != nullptr;
}

std::string FindDxcExecutable() {
    char found[MAX_PATH] = {};
    if (SearchPathA(nullptr, "dxc.exe", nullptr, MAX_PATH, found, nullptr) > 0u) {
        return std::string(found);
    }
    const char* kKnownPaths[] = {
        "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\dxc.exe",
        "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.22621.0\\x64\\dxc.exe",
        "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.22000.0\\x64\\dxc.exe",
    };
    for (const char* p : kKnownPaths) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            return std::string(p);
        }
    }
    return {};
}

bool CompileHlslWithDxc(const std::string& dxc_exe,
                        const std::string& base_name,
                        const char* hlsl_source,
                        const char* profile,
                        const char* entry_point,
                        std::vector<uint8_t>& out_dxil) {
    if (dxc_exe.empty()) return false;
    std::error_code ec;
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path(ec) / "openscarlett_bindless_test";
    if (ec) return false;
    std::filesystem::create_directories(scratch, ec);

    const std::filesystem::path hlsl_path = scratch / (base_name + ".hlsl");
    const std::filesystem::path dxil_path = scratch / (base_name + ".dxil");
    {
        std::ofstream ofs(hlsl_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(hlsl_source, static_cast<std::streamsize>(std::strlen(hlsl_source)));
    }

    std::string cmd = "\"" + dxc_exe + "\" -T " + profile + " -E " + entry_point +
                      " -Fo \"" + dxil_path.string() + "\" \"" + hlsl_path.string() + "\"";
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000u);
    DWORD exit_code = 1u;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0u) return false;

    std::ifstream ifs(dxil_path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const std::streamsize sz = ifs.tellg();
    if (sz <= 0) return false;
    out_dxil.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(out_dxil.data()), sz);

    std::filesystem::remove(hlsl_path, ec);
    std::filesystem::remove(dxil_path, ec);
    return ifs.good() || ifs.eof();
}

bool CompileHlslSm51(const char* hlsl_source,
                     const char* entry_point,
                     std::vector<uint8_t>& out_bytecode) {
    ComRef<ID3DBlob> code;
    ComRef<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        hlsl_source,
        std::strlen(hlsl_source),
        "unified_mem_bindless.hlsl",
        nullptr,
        nullptr,
        entry_point,
        "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL2,
        0u,
        code.Put(),
        err.Put());
    if (FAILED(hr) || !code) {
        if (err) {
            std::fprintf(stderr, "D3DCompile error:\n%s\n",
                         static_cast<const char*>(err->GetBufferPointer()));
        }
        return false;
    }
    out_bytecode.resize(code->GetBufferSize());
    std::memcpy(out_bytecode.data(), code->GetBufferPointer(), code->GetBufferSize());
    return true;
}

// ---------------------------------------------------------------------------
// Test 1: Live Win32 TitleVAMirror Dirty-Page Tracking + GPU Compute + Readback
// ---------------------------------------------------------------------------
void RunTitleVaMirrorLiveTest(D3D12TestContext& ctx) {
    using namespace openscarlett;

    TitleVAMirror mirror;
    OS_REQUIRE(mirror.PageSizeBytes() == 4096ull);

    // 1A. Allocate a 256 KiB (64 x 4 KiB pages) write-watched Title-VA region.
    constexpr uint64_t kRegionBytes = 256ull * 1024ull;
    constexpr uint64_t kTitleVaBase = 0x0000700010000000ull;
    TitleVaRegionInfo reg{};
    OS_REQUIRE(mirror.AllocateRegion(kRegionBytes,
                                     kPageSize64KB,
                                     TitleVaResourceKind::Buffer,
                                     ctx.device.Get(),
                                     reg,
                                     kTitleVaBase,
                                     true)
                   .ok());
    OS_REQUIRE(reg.title_va == kTitleVaBase);
    OS_REQUIRE(reg.cpu_backing_ptr != nullptr);
    OS_REQUIRE(reg.write_watch_enabled);
    OS_REQUIRE(reg.gpu_shadow_resource != nullptr);
    OS_REQUIRE(reg.pc_gpu_va != 0ull);
    OS_REQUIRE(mirror.HasPersistentUploadMapping(kTitleVaBase));
    OS_REQUIRE(!mirror.HasReadbackStaging(kTitleVaBase));

    // 1B. Register an existing caller-allocated MEM_WRITE_WATCH region to test coalescing & lookup.
    constexpr uint64_t kCoalesceBytes = 64ull * 1024ull;
    constexpr uint64_t kCoalesceTitleVa = 0x0000700020000000ull;
    void* ext_mem = VirtualAlloc(
        nullptr, kCoalesceBytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    OS_REQUIRE(ext_mem != nullptr);

    TitleVaMappingDesc ext_desc{};
    ext_desc.title_va = kCoalesceTitleVa;
    ext_desc.size_bytes = kCoalesceBytes;
    ext_desc.alignment = kPageSize64KB;
    ext_desc.kind = TitleVaResourceKind::Buffer;
    ext_desc.existing_write_watch_cpu_ptr = ext_mem;
    TitleVaRegionInfo ext_reg{};
    OS_REQUIRE(mirror.RegisterRegion(ext_desc, ctx.device.Get(), ext_reg).ok());
    OS_REQUIRE(ext_reg.write_watch_enabled);
    OS_REQUIRE(!ext_reg.owns_cpu_memory);
    OS_REQUIRE(mirror.HasPersistentUploadMapping(kCoalesceTitleVa));
    OS_REQUIRE(!mirror.HasReadbackStaging(kCoalesceTitleVa));

    // Verify fast O(log N) ResolveTitleVa lookup (exact base, interior offset, and miss).
    uint64_t resolved_gpu_va = 0ull;
    uint64_t resolved_off = 0ull;
    OS_REQUIRE(mirror.ResolveTitleVa(kTitleVaBase + 17ull * 4096ull + 64ull,
                                     resolved_gpu_va,
                                     resolved_off));
    OS_REQUIRE(resolved_off == 17ull * 4096ull + 64ull);
    OS_REQUIRE(resolved_gpu_va == reg.pc_gpu_va + resolved_off);
    OS_REQUIRE(!mirror.ResolveTitleVa(0x0000700018000000ull, resolved_gpu_va, resolved_off));

    // Verify contiguous page coalescing on the external region (pages 4, 5, 6 -> 1 range of 12 KiB).
    auto* ext_u32 = static_cast<uint32_t*>(ext_mem);
    ext_u32[(4u * 4096u) / 4u] = 0xAAu;
    ext_u32[(5u * 4096u) / 4u] = 0xBBu;
    ext_u32[(6u * 4096u) / 4u] = 0xCCu;
    std::vector<DirtyPageRange> coal_ranges;
    uint64_t coal_dirty = 0ull;
    uint64_t coal_clean = 0ull;
    OS_REQUIRE(mirror.QueryDirtyPageRanges(kCoalesceTitleVa, true, coal_ranges, &coal_dirty, &coal_clean).ok());
    OS_REQUIRE(coal_dirty == 3ull);
    OS_REQUIRE(coal_clean == 13ull);
    OS_REQUIRE(coal_ranges.size() == 1u);
    OS_REQUIRE(coal_ranges[0].offset == 4ull * 4096ull);
    OS_REQUIRE(coal_ranges[0].size == 3ull * 4096ull);

    // 1C. Clean flush test on main 256 KiB region: 0 dirty pages, 64 clean pages skipped!
    FlushStats clean_flush{};
    OS_REQUIRE(mirror.FlushDirtyPagesToGpu(ctx.cmd.Get(), kTitleVaBase, &clean_flush).ok());
    OS_REQUIRE(clean_flush.dirty_pages_flushed == 0ull);
    OS_REQUIRE(clean_flush.clean_pages_skipped == 64ull);
    OS_REQUIRE(clean_flush.clean_regions_skipped == 1ull);
    OS_REQUIRE(clean_flush.bytes_uploaded == 0ull);

    // 1D. Touch ONLY page #3 and page #17 in the 256 KiB Title-VA region!
    auto* cpu_u32 = static_cast<uint32_t*>(reg.cpu_backing_ptr);
    constexpr uint32_t kPage3WordBase = (3u * 4096u) / 4u;
    constexpr uint32_t kPage17WordBase = (17u * 4096u) / 4u;
    constexpr uint32_t kPage42WordBase = (42u * 4096u) / 4u;

    for (uint32_t i = 0; i < 64u; ++i) {
        cpu_u32[kPage3WordBase + i] = 0x11110000u + i;
        cpu_u32[kPage17WordBase + i] = 0x22220000u + i;
    }

    // Non-destructive query must report EXACTLY pages #3 and #17 (2 dirty, 62 clean).
    std::vector<DirtyPageRange> pre_ranges;
    uint64_t pre_dirty = 0ull;
    uint64_t pre_clean = 0ull;
    OS_REQUIRE(mirror.QueryDirtyPageRanges(kTitleVaBase, false, pre_ranges, &pre_dirty, &pre_clean).ok());
    OS_REQUIRE(pre_dirty == 2ull);
    OS_REQUIRE(pre_clean == 62ull);
    OS_REQUIRE(pre_ranges.size() == 2u);
    OS_REQUIRE(pre_ranges[0].offset == 3ull * 4096ull && pre_ranges[0].size == 4096ull);
    OS_REQUIRE(pre_ranges[1].offset == 17ull * 4096ull && pre_ranges[1].size == 4096ull);

    // Flush ONLY dirty pages to PC GPU default-heap shadow buffer and transition to UAV.
    FlushStats fstats{};
    OS_REQUIRE(mirror.FlushDirtyPagesToGpu(
                         ctx.cmd.Get(),
                         kTitleVaBase,
                         &fstats,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                   .ok());
    OS_REQUIRE(fstats.regions_flushed == 1ull);
    OS_REQUIRE(fstats.dirty_pages_flushed == 2ull);
    OS_REQUIRE(fstats.clean_pages_skipped == 62ull);
    OS_REQUIRE(fstats.coalesced_ranges == 2ull);
    OS_REQUIRE(fstats.bytes_uploaded == 8192ull);

    // 1E. Run a live D3D12 compute shader directly on the GPU shadow buffer!
    // Reads page #3 and page #17, mutates both in-place, and writes combined result to page #42.
    static const char* kMutateCsHlsl = R"HLSL(
        RWByteAddressBuffer g_Buf : register(u0);

        [numthreads(64, 1, 1)]
        void CSMain(uint3 dtid : SV_DispatchThreadID) {
            uint i = dtid.x;
            uint off3  = 3u * 4096u + i * 4u;
            uint off17 = 17u * 4096u + i * 4u;
            uint off42 = 42u * 4096u + i * 4u;

            uint v3  = g_Buf.Load(off3);
            uint v17 = g_Buf.Load(off17);

            uint out3  = v3 ^ 0xA5A55A5Au;
            uint out17 = v17 + 0x01020304u;
            uint out42 = (v3 + v17) ^ 0xDEADBEEFu;

            g_Buf.Store(off3, out3);
            g_Buf.Store(off17, out17);
            g_Buf.Store(off42, out42);
        }
    )HLSL";

    std::vector<uint8_t> cs_blob;
    OS_REQUIRE(CompileHlslSm51(kMutateCsHlsl, "CSMain", cs_blob));

    D3D12_ROOT_PARAMETER root_param{};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_param.Descriptor.ShaderRegister = 0u;
    root_param.Descriptor.RegisterSpace = 0u;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1u;
    rs_desc.pParameters = &root_param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComRef<ID3DBlob> rs_blob;
    ComRef<ID3DBlob> rs_err;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));

    ComRef<ID3D12RootSignature> root_sig;
    OS_REQUIRE(SUCCEEDED(ctx.device->CreateRootSignature(
        0u,
        rs_blob->GetBufferPointer(),
        rs_blob->GetBufferSize(),
        OS_UUIDOF(ID3D12RootSignature),
        reinterpret_cast<void**>(root_sig.Put()))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.CS.pShaderBytecode = cs_blob.data();
    pso_desc.CS.BytecodeLength = cs_blob.size();

    ComRef<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(ctx.device->CreateComputePipelineState(
        &pso_desc,
        OS_UUIDOF(ID3D12PipelineState),
        reinterpret_cast<void**>(pso.Put()))));

    ctx.cmd->SetComputeRootSignature(root_sig.Get());
    ctx.cmd->SetPipelineState(pso.Get());
    ctx.cmd->SetComputeRootUnorderedAccessView(0u, reg.pc_gpu_va);
    ctx.cmd->Dispatch(1u, 1u, 1u);

    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = reg.gpu_shadow_resource;
    ctx.cmd->ResourceBarrier(1u, &uav_barrier);

    // 1F. Schedule GPU readback tagged with next fence value, submit, and complete readback!
    OS_REQUIRE(!mirror.HasReadbackStaging(kTitleVaBase));
    const uint64_t expected_fence = ctx.fence_val + 1ull;
    OS_REQUIRE(mirror.ScheduleGpuReadback(ctx.cmd.Get(), kTitleVaBase, expected_fence).ok());
    OS_REQUIRE(mirror.HasReadbackStaging(kTitleVaBase));
    OS_REQUIRE(!mirror.HasReadbackStaging(kCoalesceTitleVa));

    uint64_t completed_fence = 0ull;
    OS_REQUIRE(ctx.SubmitAndWait(completed_fence));
    OS_REQUIRE(completed_fence == expected_fence);

    uint64_t rb_count = 0ull;
    uint64_t rb_bytes = 0ull;
    OS_REQUIRE(mirror.CompleteReadbacksOnFence(completed_fence, &rb_count, &rb_bytes).ok());
    OS_REQUIRE(rb_count == 1ull);
    OS_REQUIRE(rb_bytes == kRegionBytes);

    // 1G. Verify CPU Title-VA backing memory holds exact GPU-computed values!
    for (uint32_t i = 0; i < 64u; ++i) {
        const uint32_t orig3 = 0x11110000u + i;
        const uint32_t orig17 = 0x22220000u + i;
        OS_REQUIRE(cpu_u32[kPage3WordBase + i] == (orig3 ^ 0xA5A55A5Au));
        OS_REQUIRE(cpu_u32[kPage17WordBase + i] == (orig17 + 0x01020304u));
        OS_REQUIRE(cpu_u32[kPage42WordBase + i] == ((orig3 + orig17) ^ 0xDEADBEEFu));
    }

    // 1H. Immediately verify dirty page count is ZERO right after readback!
    std::vector<DirtyPageRange> post_rb_ranges;
    uint64_t post_rb_dirty = 999ull;
    uint64_t post_rb_clean = 0ull;
    OS_REQUIRE(mirror.QueryDirtyPageRanges(
                         kTitleVaBase, false, post_rb_ranges, &post_rb_dirty, &post_rb_clean)
                   .ok());
    OS_REQUIRE(post_rb_dirty == 0ull);
    OS_REQUIRE(post_rb_clean == 64ull);
    OS_REQUIRE(post_rb_ranges.empty());

    const TitleVaMirrorStats stats = mirror.GetStats();
    OS_REQUIRE(stats.total_registrations == 2ull);
    OS_REQUIRE(stats.active_registrations == 2ull);
    OS_REQUIRE(stats.dirty_pages_flushed == 2ull);
    OS_REQUIRE(stats.bytes_uploaded == 8192ull);
    OS_REQUIRE(stats.bytes_readback == kRegionBytes);
    OS_REQUIRE(stats.clean_skips == 1ull);
    OS_REQUIRE(stats.pending_readbacks == 0ull);
    OS_REQUIRE(stats.completed_readbacks == 1ull);

    OS_REQUIRE(mirror.UnregisterRegion(kCoalesceTitleVa).ok());
    VirtualFree(ext_mem, 0, MEM_RELEASE);
    OS_REQUIRE(ctx.ResetCmdList());

    std::printf("[PASS] TitleVAMirror live (%s): 2/64 dirty pages flushed (8 KiB uploaded, 62 clean skipped), GPU compute + 256 KiB readback verified, post-readback dirty=0\n",
                ctx.adapter_name.c_str());
}

// ---------------------------------------------------------------------------
// Test 2: Bit-Exact GFX10.3 V# / T# / S# Encode/Decode & Translation Round-Trip
// ---------------------------------------------------------------------------
void RunRdna2DescriptorRoundTripTests() {
    using namespace openscarlett;

    // 2A. V# Buffer Descriptor round-trip across raw, structured, and typed formats.
    {
        Rdna2BufferDescriptor v{};
        v.base_address = 0x0000ABCD12345678ull;
        v.stride = 64u;
        v.swizzle_enable = true;
        v.cache_swizzle = true;
        v.num_records = 123456u;
        v.dst_sel_x = Rdna2DstSel::B;
        v.dst_sel_y = Rdna2DstSel::G;
        v.dst_sel_z = Rdna2DstSel::R;
        v.dst_sel_w = Rdna2DstSel::One;
        v.format = static_cast<uint8_t>(Rdna2MtbufFormat::R32G32B32A32_FLOAT);
        v.index_stride = 2u;
        v.add_tid_enable = true;
        v.resource_level = true;
        v.oob_select = Rdna2BufferOobSelect::StructuredWithIndex;
        v.type = 0u;

        uint32_t words[4] = {};
        EncodeRdna2BufferDescriptor(v, words);
        const Rdna2BufferDescriptor dec = DecodeRdna2BufferDescriptor(words);

        OS_REQUIRE(dec.base_address == v.base_address);
        OS_REQUIRE(dec.stride == v.stride);
        OS_REQUIRE(dec.swizzle_enable == v.swizzle_enable);
        OS_REQUIRE(dec.cache_swizzle == v.cache_swizzle);
        OS_REQUIRE(dec.num_records == v.num_records);
        OS_REQUIRE(dec.dst_sel_x == v.dst_sel_x);
        OS_REQUIRE(dec.dst_sel_y == v.dst_sel_y);
        OS_REQUIRE(dec.dst_sel_z == v.dst_sel_z);
        OS_REQUIRE(dec.dst_sel_w == v.dst_sel_w);
        OS_REQUIRE(dec.format == v.format);
        OS_REQUIRE(dec.index_stride == v.index_stride);
        OS_REQUIRE(dec.add_tid_enable == v.add_tid_enable);
        OS_REQUIRE(dec.resource_level == v.resource_level);
        OS_REQUIRE(dec.oob_select == v.oob_select);
        OS_REQUIRE(dec.type == v.type);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        OS_REQUIRE(DescriptorSgprBridge::SynthesizeD3D12BufferSrv(dec, 128ull, srv));
        OS_REQUIRE(srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER);
        OS_REQUIRE(srv.Format == DXGI_FORMAT_UNKNOWN);
        OS_REQUIRE(srv.Buffer.StructureByteStride == 64u);
        OS_REQUIRE(srv.Buffer.FirstElement == 2ull);
        OS_REQUIRE(srv.Buffer.NumElements == 123456u);

        const VulkanBindlessBindingEntry vk =
            DescriptorSgprBridge::TranslateVSharpToVulkan(dec, 7u, 0x900000ull, 0u);
        OS_REQUIRE(vk.vk_descriptor_type == kVkDescriptorTypeStorageBuffer);
        OS_REQUIRE(vk.array_index == 7u);
        OS_REQUIRE(vk.byte_range == 123456ull * 64ull);
        OS_REQUIRE((vk.binding_flags & kVkDescriptorBindingUpdateAfterBindBit) != 0u);
        OS_REQUIRE((vk.binding_flags & kVkDescriptorBindingPartiallyBoundBit) != 0u);
    }

    // Raw ByteAddressBuffer V# synthesis
    {
        Rdna2BufferDescriptor raw_v{};
        raw_v.base_address = 0x0000500000010000ull;
        raw_v.stride = 0u;
        raw_v.num_records = 4096u;
        raw_v.format = static_cast<uint8_t>(Rdna2MtbufFormat::R32_UINT);
        raw_v.oob_select = Rdna2BufferOobSelect::NumRecordsAddress;

        uint32_t words[4] = {};
        EncodeRdna2BufferDescriptor(raw_v, words);
        const Rdna2BufferDescriptor dec = DecodeRdna2BufferDescriptor(words);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        OS_REQUIRE(DescriptorSgprBridge::SynthesizeD3D12BufferUav(dec, 0ull, uav));
        OS_REQUIRE(uav.Format == DXGI_FORMAT_R32_TYPELESS);
        OS_REQUIRE(uav.Buffer.Flags == D3D12_BUFFER_UAV_FLAG_RAW);
        OS_REQUIRE(uav.Buffer.NumElements == 1024u);
    }

    // 2B. T# Image Descriptor round-trip across 1D/2D/3D/Cube/1DArray/2DArray/2DMsaa/2DArrayMsaa.
    const Rdna2ImageType kImageTypes[] = {
        Rdna2ImageType::Img1D,
        Rdna2ImageType::Img2D,
        Rdna2ImageType::Img3D,
        Rdna2ImageType::ImgCube,
        Rdna2ImageType::Img1DArray,
        Rdna2ImageType::Img2DArray,
        Rdna2ImageType::Img2DMsaa,
        Rdna2ImageType::Img2DArrayMsaa,
    };
    const Rdna2SwizzleMode kSwizzles[] = {
        Rdna2SwizzleMode::Linear,
        Rdna2SwizzleMode::Sw256B_S,
        Rdna2SwizzleMode::Sw64KB_S,
        Rdna2SwizzleMode::Sw64KB_D_X,
        Rdna2SwizzleMode::Sw64KB_R_X,
        Rdna2SwizzleMode::Sw64KB_Z_X,
        Rdna2SwizzleMode::Sw4KB_R,
        Rdna2SwizzleMode::Sw64KB_S_X,
    };
    const Rdna2ImageFormat kFormats[] = {
        Rdna2ImageFormat::R8G8B8A8_UNORM,
        Rdna2ImageFormat::R16G16B16A16_FLOAT,
        Rdna2ImageFormat::R32G32B32A32_FLOAT,
        Rdna2ImageFormat::BC7_UNORM,
        Rdna2ImageFormat::BC1_UNORM,
        Rdna2ImageFormat::R10G10B10A2_UNORM,
        Rdna2ImageFormat::B8G8R8A8_SRGB,
        Rdna2ImageFormat::D32_FLOAT,
    };

    for (size_t i = 0; i < 8u; ++i) {
        Rdna2ImageDescriptor t{};
        t.base_address = 0x0000123456789A00ull + (static_cast<uint64_t>(i) << 16);
        t.min_lod_raw = FloatToFixed4_8(1.25f);
        t.format = static_cast<uint16_t>(kFormats[i]);
        t.iterate_256 = (i % 2u) == 1u;
        t.width = (i == 0u) ? 16384u : static_cast<uint32_t>(128u << (i % 5u));
        t.height = (i == 0u) ? 1u : static_cast<uint32_t>(64u << (i % 5u));
        t.perf_mod = static_cast<uint8_t>(i & 7u);
        t.resource_level = true;
        t.dst_sel_x = Rdna2DstSel::R;
        t.dst_sel_y = Rdna2DstSel::G;
        t.dst_sel_z = Rdna2DstSel::B;
        t.dst_sel_w = (i == 7u) ? Rdna2DstSel::One : Rdna2DstSel::A;
        t.base_level = 1u;
        t.last_level = 6u;
        t.swizzle_mode = kSwizzles[i];
        t.bc_swizzle = static_cast<uint8_t>(i & 7u);
        t.type = kImageTypes[i];
        t.depth = (t.type == Rdna2ImageType::Img3D) ? 128u : 6u;
        t.base_array = 2u;
        t.last_array = 7u;
        t.max_mip = 9u;
        t.array_pitch = 3u;
        t.meta_pipe_aligned = true;
        t.dcc_compression_enable = (i % 2u == 0u);
        t.alpha_is_on_msb = true;
        t.color_transform = true;
        t.meta_linear = false;
        t.write_compress_enable = true;
        t.max_uncompressed_block_size = 2u;
        t.max_compressed_block_size = 1u;
        t.meta_data_address = 0x000000FEDCBA9800ull;

        uint32_t words[8] = {};
        EncodeRdna2ImageDescriptor(t, words);
        const Rdna2ImageDescriptor dec = DecodeRdna2ImageDescriptor(words);

        OS_REQUIRE(dec.base_address == t.base_address);
        OS_REQUIRE(dec.min_lod_raw == t.min_lod_raw);
        OS_REQUIRE(dec.format == t.format);
        OS_REQUIRE(dec.iterate_256 == t.iterate_256);
        OS_REQUIRE(dec.width == t.width);
        OS_REQUIRE(dec.height == t.height);
        OS_REQUIRE(dec.perf_mod == t.perf_mod);
        OS_REQUIRE(dec.resource_level == t.resource_level);
        OS_REQUIRE(dec.dst_sel_x == t.dst_sel_x);
        OS_REQUIRE(dec.dst_sel_y == t.dst_sel_y);
        OS_REQUIRE(dec.dst_sel_z == t.dst_sel_z);
        OS_REQUIRE(dec.dst_sel_w == t.dst_sel_w);
        OS_REQUIRE(dec.base_level == t.base_level);
        OS_REQUIRE(dec.last_level == t.last_level);
        OS_REQUIRE(dec.swizzle_mode == t.swizzle_mode);
        OS_REQUIRE(dec.bc_swizzle == t.bc_swizzle);
        OS_REQUIRE(dec.type == t.type);
        OS_REQUIRE(dec.depth == t.depth);
        OS_REQUIRE(dec.base_array == t.base_array);
        OS_REQUIRE(dec.last_array == t.last_array);
        OS_REQUIRE(dec.max_mip == t.max_mip);
        OS_REQUIRE(dec.array_pitch == t.array_pitch);
        OS_REQUIRE(dec.meta_pipe_aligned == t.meta_pipe_aligned);
        OS_REQUIRE(dec.dcc_compression_enable == t.dcc_compression_enable);
        OS_REQUIRE(dec.alpha_is_on_msb == t.alpha_is_on_msb);
        OS_REQUIRE(dec.color_transform == t.color_transform);
        OS_REQUIRE(dec.write_compress_enable == t.write_compress_enable);
        OS_REQUIRE(dec.max_uncompressed_block_size == t.max_uncompressed_block_size);
        OS_REQUIRE(dec.max_compressed_block_size == t.max_compressed_block_size);
        OS_REQUIRE(dec.meta_data_address == t.meta_data_address);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        OS_REQUIRE(DescriptorSgprBridge::SynthesizeD3D12TextureSrv(dec, srv));
        const VulkanBindlessBindingEntry vk =
            DescriptorSgprBridge::TranslateTSharpToVulkan(dec, static_cast<uint32_t>(i), false, 0ull, 1u);
        OS_REQUIRE(vk.vk_descriptor_type == kVkDescriptorTypeSampledImage);
        OS_REQUIRE(vk.vk_format != 0u);
        OS_REQUIRE(vk.mip_count == 6u);
    }

    // 2C. S# Sampler Descriptor round-trip including signed negative/positive LOD bias, aniso, compare.
    {
        Rdna2SamplerDescriptor s{};
        s.clamp_x = Rdna2ClampMode::Wrap;
        s.clamp_y = Rdna2ClampMode::Mirror;
        s.clamp_z = Rdna2ClampMode::ClampBorder;
        s.max_aniso_ratio = 4u;  // 16x
        s.depth_compare_func = Rdna2CompareFunc::LessEqual;
        s.force_unnormalized = false;
        s.aniso_threshold = 3u;
        s.mc_coord_trunc = true;
        s.force_degamma = false;
        s.aniso_bias = 12u;
        s.trunc_coord = true;
        s.disable_cube_wrap = true;
        s.filter_mode = 0u;
        s.compat_mode = false;
        s.min_lod_raw = FloatToFixed4_8(0.5f);
        s.max_lod_raw = FloatToFixed4_8(11.75f);
        s.perf_mip = 5u;
        s.perf_z = 2u;
        s.lod_bias_raw = FloatToFixed6_8(-2.5f);
        s.lod_bias_sec = 7u;
        s.xy_mag_filter = Rdna2XyFilter::AnisoLinear;
        s.xy_min_filter = Rdna2XyFilter::AnisoLinear;
        s.z_filter = Rdna2MipFilter::Linear;
        s.mip_filter = Rdna2MipFilter::Linear;
        s.aniso_override = true;
        s.blend_zero_prt = false;
        s.border_color_ptr = 0x123u;
        s.border_color_type = Rdna2BorderColorType::OpaqueWhite;

        uint32_t words[4] = {};
        EncodeRdna2SamplerDescriptor(s, words);
        const Rdna2SamplerDescriptor dec = DecodeRdna2SamplerDescriptor(words);

        OS_REQUIRE(dec.clamp_x == s.clamp_x);
        OS_REQUIRE(dec.clamp_y == s.clamp_y);
        OS_REQUIRE(dec.clamp_z == s.clamp_z);
        OS_REQUIRE(dec.max_aniso_ratio == s.max_aniso_ratio);
        OS_REQUIRE(dec.depth_compare_func == s.depth_compare_func);
        OS_REQUIRE(dec.aniso_threshold == s.aniso_threshold);
        OS_REQUIRE(dec.mc_coord_trunc == s.mc_coord_trunc);
        OS_REQUIRE(dec.aniso_bias == s.aniso_bias);
        OS_REQUIRE(dec.trunc_coord == s.trunc_coord);
        OS_REQUIRE(dec.disable_cube_wrap == s.disable_cube_wrap);
        OS_REQUIRE(dec.min_lod_raw == s.min_lod_raw);
        OS_REQUIRE(dec.max_lod_raw == s.max_lod_raw);
        OS_REQUIRE(dec.perf_mip == s.perf_mip);
        OS_REQUIRE(dec.perf_z == s.perf_z);
        OS_REQUIRE(dec.lod_bias_raw == s.lod_bias_raw);
        OS_REQUIRE(std::fabs(Fixed6_8ToFloat(dec.lod_bias_raw) - (-2.5f)) < 1e-4f);
        OS_REQUIRE(dec.lod_bias_sec == s.lod_bias_sec);
        OS_REQUIRE(dec.xy_mag_filter == s.xy_mag_filter);
        OS_REQUIRE(dec.xy_min_filter == s.xy_min_filter);
        OS_REQUIRE(dec.z_filter == s.z_filter);
        OS_REQUIRE(dec.mip_filter == s.mip_filter);
        OS_REQUIRE(dec.aniso_override == s.aniso_override);
        OS_REQUIRE(dec.border_color_ptr == s.border_color_ptr);
        OS_REQUIRE(dec.border_color_type == s.border_color_type);

        D3D12_SAMPLER_DESC d3d_samp{};
        OS_REQUIRE(DescriptorSgprBridge::SynthesizeD3D12Sampler(dec, d3d_samp));
        OS_REQUIRE(d3d_samp.Filter == D3D12_FILTER_COMPARISON_ANISOTROPIC);
        OS_REQUIRE(d3d_samp.AddressU == D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        OS_REQUIRE(d3d_samp.AddressV == D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
        OS_REQUIRE(d3d_samp.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        OS_REQUIRE(d3d_samp.MaxAnisotropy == 16u);
        OS_REQUIRE(d3d_samp.ComparisonFunc == D3D12_COMPARISON_FUNC_LESS_EQUAL);
        OS_REQUIRE(std::fabs(d3d_samp.MipLODBias - (-2.5f)) < 1e-4f);
        OS_REQUIRE(std::fabs(d3d_samp.MinLOD - 0.5f) < 1e-4f);
        OS_REQUIRE(std::fabs(d3d_samp.MaxLOD - 11.75f) < 1e-4f);
        OS_REQUIRE(d3d_samp.BorderColor[0] == 1.0f && d3d_samp.BorderColor[3] == 1.0f);

        const VulkanBindlessBindingEntry vk =
            DescriptorSgprBridge::TranslateSSharpToVulkan(dec, 2u, 3u);
        OS_REQUIRE(vk.vk_descriptor_type == kVkDescriptorTypeSampler);
        OS_REQUIRE(vk.max_anisotropy == 16u);
        OS_REQUIRE(vk.compare_enable);
        OS_REQUIRE(vk.vk_compare_op == static_cast<uint32_t>(Rdna2CompareFunc::LessEqual));
        OS_REQUIRE(std::fabs(vk.mip_lod_bias - (-2.5f)) < 1e-4f);
    }

    std::printf("[PASS] GFX10.3 V# (4dw) / T# (8dw, 8 image types) / S# (4dw) bit-exact round-trip + D3D12/Vulkan synthesis verified\n");
}

// ---------------------------------------------------------------------------
// Test 3: End-to-End Live GPU Bindless Sampling (T#+S#) & Buffer Read/Write (V#)
//         via DescriptorSgprBridge on GTX 1650 Ti HW AND D3D12 WARP
// ---------------------------------------------------------------------------
void RunBindlessLiveGpuTest(D3D12TestContext& ctx) {
    using namespace openscarlett;

    TitleVAMirror mirror;
    DescriptorSgprBridge bridge;
    OS_REQUIRE(bridge.InitializeD3D12(ctx.device.Get(), 256u, 32u).ok());

    // 3A. Create a 4x4 RGBA8_UNORM GPU texture with distinct known texels per pixel.
    constexpr uint32_t kTexW = 4u;
    constexpr uint32_t kTexH = 4u;
    ComRef<ID3D12Resource> texture;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kTexW;
        td.Height = kTexH;
        td.DepthOrArraySize = 1u;
        td.MipLevels = 1u;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1u;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        OS_REQUIRE(SUCCEEDED(ctx.device->CreateCommittedResource(
            &hp,
            D3D12_HEAP_FLAG_NONE,
            &td,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            OS_UUIDOF(ID3D12Resource),
            reinterpret_cast<void**>(texture.Put()))));
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_footprint{};
    UINT num_rows = 0u;
    UINT64 row_size = 0ull;
    UINT64 total_upload_bytes = 0ull;
    const D3D12_RESOURCE_DESC tex_desc = texture->GetDesc();
    ctx.device->GetCopyableFootprints(
        &tex_desc, 0u, 1u, 0ull, &tex_footprint, &num_rows, &row_size, &total_upload_bytes);

    ComRef<ID3D12Resource> tex_upload;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total_upload_bytes;
        bd.Height = 1u;
        bd.DepthOrArraySize = 1u;
        bd.MipLevels = 1u;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1u;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        OS_REQUIRE(SUCCEEDED(ctx.device->CreateCommittedResource(
            &hp,
            D3D12_HEAP_FLAG_NONE,
            &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            OS_UUIDOF(ID3D12Resource),
            reinterpret_cast<void**>(tex_upload.Put()))));

        uint8_t* dst = nullptr;
        const D3D12_RANGE empty_read{0, 0};
        OS_REQUIRE(SUCCEEDED(tex_upload->Map(0u, &empty_read, reinterpret_cast<void**>(&dst))));
        std::memset(dst, 0, static_cast<size_t>(total_upload_bytes));
        for (uint32_t y = 0; y < kTexH; ++y) {
            uint8_t* row = dst + tex_footprint.Offset + y * tex_footprint.Footprint.RowPitch;
            for (uint32_t x = 0; x < kTexW; ++x) {
                // Pixel (x, y) = (R=20 + x*50, G=40 + y*50, B=100 + (x+y)*20, A=255)
                row[x * 4u + 0u] = static_cast<uint8_t>(20u + x * 50u);
                row[x * 4u + 1u] = static_cast<uint8_t>(40u + y * 50u);
                row[x * 4u + 2u] = static_cast<uint8_t>(100u + (x + y) * 20u);
                row[x * 4u + 3u] = 255u;
            }
        }
        tex_upload->Unmap(0u, nullptr);
    }

    D3D12_TEXTURE_COPY_LOCATION copy_dst{};
    copy_dst.pResource = texture.Get();
    copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_dst.SubresourceIndex = 0u;

    D3D12_TEXTURE_COPY_LOCATION copy_src{};
    copy_src.pResource = tex_upload.Get();
    copy_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copy_src.PlacedFootprint = tex_footprint;

    ctx.cmd->CopyTextureRegion(&copy_dst, 0u, 0u, 0u, &copy_src, nullptr);

    D3D12_RESOURCE_BARRIER tex_barrier{};
    tex_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tex_barrier.Transition.pResource = texture.Get();
    tex_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tex_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    tex_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ctx.cmd->ResourceBarrier(1u, &tex_barrier);

    // 3B. Allocate Title-VA regions via TitleVAMirror:
    //   - Input buffer (V#_in): 16 samples' texel coords + scalar bias
    //   - Output buffer (V#_out): 16 uint4 results written by compute UAV
    //   - Guest descriptor table buffer: holds raw encoded T# (8dw) + S# (4dw) in guest memory!
    constexpr uint64_t kInTitleVa = 0x0000710000010000ull;
    constexpr uint64_t kOutTitleVa = 0x0000710000020000ull;
    constexpr uint64_t kTableTitleVa = 0x0000710000030000ull;
    constexpr uint64_t kTexTitleVa = 0x0000710000040000ull;

    TitleVaRegionInfo in_reg{};
    TitleVaRegionInfo out_reg{};
    TitleVaRegionInfo table_reg{};
    TitleVaRegionInfo tex_reg{};
    OS_REQUIRE(mirror.AllocateRegion(4096ull, kPageSize64KB, TitleVaResourceKind::Buffer,
                                     ctx.device.Get(), in_reg, kInTitleVa, false).ok());
    OS_REQUIRE(mirror.AllocateRegion(4096ull, kPageSize64KB, TitleVaResourceKind::Buffer,
                                     ctx.device.Get(), out_reg, kOutTitleVa, true).ok());
    OS_REQUIRE(mirror.AllocateRegion(4096ull, kPageSize64KB, TitleVaResourceKind::Buffer,
                                     nullptr, table_reg, kTableTitleVa, false).ok());

    TitleVaMappingDesc tex_map{};
    tex_map.title_va = kTexTitleVa;
    tex_map.size_bytes = 4096ull;
    tex_map.kind = TitleVaResourceKind::Texture;
    tex_map.gpu_shadow_resource = texture.Get();
    tex_map.initial_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    OS_REQUIRE(mirror.RegisterRegion(tex_map, nullptr, tex_reg).ok());

    // Fill V#_in with 16 entries of (x, y, add_bias, 0) in raw u32 words.
    auto* in_u32 = static_cast<uint32_t*>(in_reg.cpu_backing_ptr);
    for (uint32_t i = 0; i < 16u; ++i) {
        in_u32[i * 4u + 0u] = i % kTexW;       // texel x
        in_u32[i * 4u + 1u] = i / kTexW;       // texel y
        in_u32[i * 4u + 2u] = 10u + i;         // additive bias
        in_u32[i * 4u + 3u] = 0u;
    }

    // Flush ONLY dirty input buffer page to PC GPU shadow buffer in SRV state!
    FlushStats in_flush{};
    OS_REQUIRE(mirror.FlushDirtyPagesToGpu(
                         ctx.cmd.Get(),
                         kInTitleVa,
                         &in_flush,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                   .ok());
    OS_REQUIRE(in_flush.dirty_pages_flushed == 1ull);

    // Transition output buffer shadow to UAV state.
    D3D12_RESOURCE_BARRIER out_to_uav{};
    out_to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out_to_uav.Transition.pResource = out_reg.gpu_shadow_resource;
    out_to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out_to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    out_to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ctx.cmd->ResourceBarrier(1u, &out_to_uav);
    OS_REQUIRE(mirror.NotifyGpuResourceState(kOutTitleVa, D3D12_RESOURCE_STATE_UNORDERED_ACCESS).ok());

    // 3C. Author raw RDNA2 V#, T#, S# hardware descriptors and place them into:
    //     - Guest root SGPRs s0..s3 (inline V#_in) & s4..s7 (inline V#_out)
    //     - Guest descriptor table in Title-VA memory pointed to by s8..s9 (T# at +0, S# at +32)
    Rdna2BufferDescriptor v_in{};
    v_in.base_address = kInTitleVa;
    v_in.stride = 0u;
    v_in.num_records = 4096u;
    v_in.format = static_cast<uint8_t>(Rdna2MtbufFormat::R32_UINT);
    v_in.oob_select = Rdna2BufferOobSelect::NumRecordsAddress;

    Rdna2BufferDescriptor v_out{};
    v_out.base_address = kOutTitleVa;
    v_out.stride = 0u;
    v_out.num_records = 4096u;
    v_out.format = static_cast<uint8_t>(Rdna2MtbufFormat::R32_UINT);
    v_out.oob_select = Rdna2BufferOobSelect::NumRecordsAddress;

    Rdna2ImageDescriptor t_img{};
    t_img.base_address = kTexTitleVa;
    t_img.format = static_cast<uint16_t>(Rdna2ImageFormat::R8G8B8A8_UNORM);
    t_img.width = kTexW;
    t_img.height = kTexH;
    t_img.base_level = 0u;
    t_img.last_level = 0u;
    t_img.type = Rdna2ImageType::Img2D;
    t_img.swizzle_mode = Rdna2SwizzleMode::Sw64KB_S;

    Rdna2SamplerDescriptor s_point{};
    s_point.clamp_x = Rdna2ClampMode::ClampLastTexel;
    s_point.clamp_y = Rdna2ClampMode::ClampLastTexel;
    s_point.clamp_z = Rdna2ClampMode::ClampLastTexel;
    s_point.xy_mag_filter = Rdna2XyFilter::Point;
    s_point.xy_min_filter = Rdna2XyFilter::Point;
    s_point.mip_filter = Rdna2MipFilter::Point;
    s_point.min_lod_raw = 0u;
    s_point.max_lod_raw = FloatToFixed4_8(0.0f);

    // Write encoded raw T# (8 dwords) and S# (4 dwords) into guest descriptor-table Title-VA memory.
    auto* table_words = static_cast<uint32_t*>(table_reg.cpu_backing_ptr);
    EncodeRdna2ImageDescriptor(t_img, table_words + 0u);
    EncodeRdna2SamplerDescriptor(s_point, table_words + 8u);

    // Populate guest root SGPRs s0..s15!
    OS_REQUIRE(bridge.SetRootSgprInlineVSharp(0u, v_in).ok());
    OS_REQUIRE(bridge.SetRootSgprInlineVSharp(4u, v_out).ok());
    OS_REQUIRE(bridge.SetRootSgprTablePtr(8u, kTableTitleVa).ok());
    OS_REQUIRE(bridge.SetRootSgpr(10u, 16u).ok());          // element count
    OS_REQUIRE(bridge.SetRootSgpr(11u, 0x100u).ok());       // root constant salt

    // Decode descriptors directly back out of guest root SGPRs & Title-VA descriptor table!
    Rdna2BufferDescriptor dec_vin{};
    Rdna2BufferDescriptor dec_vout{};
    Rdna2ImageDescriptor dec_t{};
    Rdna2SamplerDescriptor dec_s{};
    OS_REQUIRE(bridge.DecodeInlineVSharpFromRootSgpr(0u, dec_vin).ok());
    OS_REQUIRE(bridge.DecodeInlineVSharpFromRootSgpr(4u, dec_vout).ok());
    OS_REQUIRE(bridge.ReadTableTSharpFromRootSgpr(8u, 0ull, mirror, dec_t).ok());
    OS_REQUIRE(bridge.ReadTableSSharpFromRootSgpr(8u, 32ull, mirror, dec_s).ok());

    // Bind into PC shader-visible bindless heaps (resolving resources via TitleVAMirror!).
    uint32_t idx_tex = UINT32_MAX;
    uint32_t idx_vin = UINT32_MAX;
    uint32_t idx_vout = UINT32_MAX;
    uint32_t idx_samp = UINT32_MAX;
    OS_REQUIRE(bridge.BindTSharpSrv(dec_t, &mirror, nullptr, idx_tex).ok());
    OS_REQUIRE(bridge.BindVSharpSrv(dec_vin, &mirror, nullptr, idx_vin).ok());
    OS_REQUIRE(bridge.BindVSharpUav(dec_vout, &mirror, nullptr, idx_vout).ok());
    OS_REQUIRE(bridge.BindSSharp(dec_s, idx_samp).ok());

    // Verify content-hash deduplication returns the exact same bindless indices!
    uint32_t idx_tex_dup = UINT32_MAX;
    uint32_t idx_samp_dup = UINT32_MAX;
    OS_REQUIRE(bridge.BindTSharpSrv(dec_t, &mirror, nullptr, idx_tex_dup).ok());
    OS_REQUIRE(bridge.BindSSharp(dec_s, idx_samp_dup).ok());
    OS_REQUIRE(idx_tex_dup == idx_tex);
    OS_REQUIRE(idx_samp_dup == idx_samp);

    const DescriptorBridgeStats bstats = bridge.GetStats();
    OS_REQUIRE(bstats.deduplication_hits == 2ull);
    OS_REQUIRE(bstats.allocated_resource_slots == 3u);
    OS_REQUIRE(bstats.allocated_sampler_slots == 1u);

    // 3D. Compile & execute bindless compute shader:
    //     Auto-select SM 6.6 ResourceDescriptorHeap/SamplerDescriptorHeap if supported + dxc found,
    //     otherwise SM 5.1 unbounded descriptor-table heap indexing over the EXACT same heaps!
    const std::string dxc_exe = FindDxcExecutable();
    bool use_sm66_path = bridge.SupportsSm66DirectHeapIndexing() && !dxc_exe.empty();
    std::vector<uint8_t> cs_blob;

    if (use_sm66_path) {
        static const char* kBindlessSm66Hlsl = R"HLSL(
            cbuffer RootIdx : register(b0) {
                uint g_TexIdx;
                uint g_InIdx;
                uint g_OutIdx;
                uint g_SampIdx;
                uint g_Salt;
            };

            [numthreads(16, 1, 1)]
            void CSMain(uint3 dtid : SV_DispatchThreadID) {
                uint i = dtid.x;
                Texture2D<float4> tex = ResourceDescriptorHeap[g_TexIdx];
                ByteAddressBuffer inBuf = ResourceDescriptorHeap[g_InIdx];
                RWByteAddressBuffer outBuf = ResourceDescriptorHeap[g_OutIdx];
                SamplerState samp = SamplerDescriptorHeap[g_SampIdx];

                uint4 rec = inBuf.Load4(i * 16u);
                float2 uv = float2((float(rec.x) + 0.5f) / 4.0f, (float(rec.y) + 0.5f) / 4.0f);
                float4 c = tex.SampleLevel(samp, uv, 0.0f);

                uint r = uint(round(c.r * 255.0f)) + rec.z;
                uint g = uint(round(c.g * 255.0f)) + g_Salt;
                uint b = uint(round(c.b * 255.0f));
                uint a = uint(round(c.a * 255.0f));
                outBuf.Store4(i * 16u, uint4(r, g, b, a));
            }
        )HLSL";
        if (!CompileHlslWithDxc(dxc_exe, "bindless_sm66", kBindlessSm66Hlsl, "cs_6_6", "CSMain", cs_blob)) {
            use_sm66_path = false;
        }
    }

    ComRef<ID3D12RootSignature> root_sig;
    if (use_sm66_path) {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.Constants.ShaderRegister = 0u;
        rp.Constants.RegisterSpace = 0u;
        rp.Constants.Num32BitValues = 5u;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1u;
        rsd.pParameters = &rp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                    D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

        ComRef<ID3DBlob> sig_blob;
        ComRef<ID3DBlob> err_blob;
        if (FAILED(D3D12SerializeRootSignature(
                &rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put())) ||
            FAILED(ctx.device->CreateRootSignature(
                0u,
                sig_blob->GetBufferPointer(),
                sig_blob->GetBufferSize(),
                OS_UUIDOF(ID3D12RootSignature),
                reinterpret_cast<void**>(root_sig.Put())))) {
            use_sm66_path = false;
        }
    }

    if (!use_sm66_path) {
        static const char* kBindlessTableHlsl = R"HLSL(
            cbuffer RootIdx : register(b0) {
                uint g_TexIdx;
                uint g_InIdx;
                uint g_OutIdx;
                uint g_SampIdx;
                uint g_Salt;
            };

            Texture2D<float4> g_Textures[256] : register(t0, space0);
            ByteAddressBuffer g_InBuffers[256] : register(t0, space1);
            RWByteAddressBuffer g_OutBuffers[256] : register(u0, space0);
            SamplerState g_Samplers[32] : register(s0, space0);

            [numthreads(16, 1, 1)]
            void CSMain(uint3 dtid : SV_DispatchThreadID) {
                uint i = dtid.x;
                uint4 rec = g_InBuffers[g_InIdx].Load4(i * 16u);
                float2 uv = float2((float(rec.x) + 0.5f) / 4.0f, (float(rec.y) + 0.5f) / 4.0f);
                float4 c = g_Textures[g_TexIdx].SampleLevel(g_Samplers[g_SampIdx], uv, 0.0f);

                uint r = uint(round(c.r * 255.0f)) + rec.z;
                uint g = uint(round(c.g * 255.0f)) + g_Salt;
                uint b = uint(round(c.b * 255.0f));
                uint a = uint(round(c.a * 255.0f));
                g_OutBuffers[g_OutIdx].Store4(i * 16u, uint4(r, g, b, a));
            }
        )HLSL";
        OS_REQUIRE(CompileHlslSm51(kBindlessTableHlsl, "CSMain", cs_blob));

        D3D12_DESCRIPTOR_RANGE r_tex{};
        r_tex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r_tex.NumDescriptors = 256u;
        r_tex.BaseShaderRegister = 0u;
        r_tex.RegisterSpace = 0u;
        r_tex.OffsetInDescriptorsFromTableStart = 0u;

        D3D12_DESCRIPTOR_RANGE r_in{};
        r_in.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r_in.NumDescriptors = 256u;
        r_in.BaseShaderRegister = 0u;
        r_in.RegisterSpace = 1u;
        r_in.OffsetInDescriptorsFromTableStart = 0u;

        D3D12_DESCRIPTOR_RANGE r_out{};
        r_out.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r_out.NumDescriptors = 256u;
        r_out.BaseShaderRegister = 0u;
        r_out.RegisterSpace = 0u;
        r_out.OffsetInDescriptorsFromTableStart = 0u;

        D3D12_DESCRIPTOR_RANGE r_samp{};
        r_samp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        r_samp.NumDescriptors = 32u;
        r_samp.BaseShaderRegister = 0u;
        r_samp.RegisterSpace = 0u;
        r_samp.OffsetInDescriptorsFromTableStart = 0u;

        D3D12_ROOT_PARAMETER params[5] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0u;
        params[0].Constants.RegisterSpace = 0u;
        params[0].Constants.Num32BitValues = 5u;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1u;
        params[1].DescriptorTable.pDescriptorRanges = &r_tex;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1u;
        params[2].DescriptorTable.pDescriptorRanges = &r_in;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1u;
        params[3].DescriptorTable.pDescriptorRanges = &r_out;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable.NumDescriptorRanges = 1u;
        params[4].DescriptorTable.pDescriptorRanges = &r_samp;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 5u;
        rsd.pParameters = params;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComRef<ID3DBlob> sig_blob;
        ComRef<ID3DBlob> err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put())));
        OS_REQUIRE(SUCCEEDED(ctx.device->CreateRootSignature(
            0u,
            sig_blob->GetBufferPointer(),
            sig_blob->GetBufferSize(),
            OS_UUIDOF(ID3D12RootSignature),
            reinterpret_cast<void**>(root_sig.Put()))));
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.CS.pShaderBytecode = cs_blob.data();
    pso_desc.CS.BytecodeLength = cs_blob.size();

    ComRef<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(ctx.device->CreateComputePipelineState(
        &pso_desc,
        OS_UUIDOF(ID3D12PipelineState),
        reinterpret_cast<void**>(pso.Put()))));

    bridge.BindHeapsToCommandList(ctx.cmd.Get());
    ctx.cmd->SetComputeRootSignature(root_sig.Get());
    ctx.cmd->SetPipelineState(pso.Get());

    const uint32_t root_consts[5] = {
        idx_tex,
        idx_vin,
        idx_vout,
        idx_samp,
        bridge.GetRootSgpr(11u),
    };
    ctx.cmd->SetComputeRoot32BitConstants(0u, 5u, root_consts, 0u);
    if (!use_sm66_path) {
        const D3D12_GPU_DESCRIPTOR_HANDLE res_start = bridge.GetResourceHeapGpuStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE samp_start = bridge.GetSamplerHeapGpuStart();
        ctx.cmd->SetComputeRootDescriptorTable(1u, res_start);
        ctx.cmd->SetComputeRootDescriptorTable(2u, res_start);
        ctx.cmd->SetComputeRootDescriptorTable(3u, res_start);
        ctx.cmd->SetComputeRootDescriptorTable(4u, samp_start);
    }
    ctx.cmd->Dispatch(1u, 1u, 1u);

    D3D12_RESOURCE_BARRIER uav_b{};
    uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_b.UAV.pResource = out_reg.gpu_shadow_resource;
    ctx.cmd->ResourceBarrier(1u, &uav_b);

    // Read back V#_out via TitleVAMirror and verify exact sampled + combined output!
    const uint64_t expected_fence = ctx.fence_val + 1ull;
    OS_REQUIRE(mirror.ScheduleGpuReadback(ctx.cmd.Get(), kOutTitleVa, expected_fence).ok());

    uint64_t completed = 0ull;
    OS_REQUIRE(ctx.SubmitAndWait(completed));
    OS_REQUIRE(mirror.CompleteReadbacksOnFence(completed).ok());

    const auto* out_u32 = static_cast<const uint32_t*>(out_reg.cpu_backing_ptr);
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t x = i % kTexW;
        const uint32_t y = i / kTexW;
        const uint32_t expected_r = (20u + x * 50u) + (10u + i);
        const uint32_t expected_g = (40u + y * 50u) + 0x100u;
        const uint32_t expected_b = 100u + (x + y) * 20u;
        const uint32_t expected_a = 255u;

        OS_REQUIRE(out_u32[i * 4u + 0u] == expected_r);
        OS_REQUIRE(out_u32[i * 4u + 1u] == expected_g);
        OS_REQUIRE(out_u32[i * 4u + 2u] == expected_b);
        OS_REQUIRE(out_u32[i * 4u + 3u] == expected_a);
    }

    // 3E. Verify Vulkan VK_EXT_descriptor_indexing metadata accumulated by the bridge.
    const VulkanBindlessLayoutTable vk_table = bridge.GetVulkanBindlessTable();
    OS_REQUIRE(vk_table.update_after_bind);
    OS_REQUIRE(vk_table.partially_bound);
    OS_REQUIRE(vk_table.entries.size() == 4u);
    OS_REQUIRE(vk_table.entries[0].vk_descriptor_type == kVkDescriptorTypeSampledImage);
    OS_REQUIRE(vk_table.entries[0].vk_format == 37u);  // VK_FORMAT_R8G8B8A8_UNORM
    OS_REQUIRE(vk_table.entries[0].vk_image_view_type == kVkImageViewType2D);
    OS_REQUIRE(vk_table.entries[1].vk_descriptor_type == kVkDescriptorTypeStorageBuffer);
    OS_REQUIRE(vk_table.entries[2].vk_descriptor_type == kVkDescriptorTypeStorageBuffer);
    OS_REQUIRE(vk_table.entries[3].vk_descriptor_type == kVkDescriptorTypeSampler);
    OS_REQUIRE(vk_table.entries[3].vk_mag_filter == 0u);  // NEAREST
    OS_REQUIRE(vk_table.entries[3].vk_min_filter == 0u);  // NEAREST

    OS_REQUIRE(ctx.ResetCmdList());

    std::printf("[PASS] DescriptorSgprBridge live (%s, SM 0x%X, Tier %u, mode=%s): T#+S# sampling + V# SRV/UAV verified across 16/16 lanes (dedup_hits=%llu)\n",
                ctx.adapter_name.c_str(),
                bridge.HighestShaderModel(),
                bridge.ResourceBindingTier(),
                use_sm66_path ? "SM6.6-DirectHeap" : "SM5.1-BindlessTable",
                static_cast<unsigned long long>(bstats.deduplication_hits));
}

// ---------------------------------------------------------------------------
// Test 4: Guarded Vulkan ICD + VK_EXT_descriptor_indexing Verification
// ---------------------------------------------------------------------------
void RunVulkanBindlessGuardTest() {
    using namespace openscarlett;

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());
    const VulkanDeviceInfo& info = vk.GetDeviceInfo();

    // Validate headless Vulkan bindless metadata translation (works with or without ICD).
    DescriptorSgprBridge vk_bridge;
    Rdna2BufferDescriptor v{};
    v.base_address = 0x0000800000100000ull;
    v.stride = 16u;
    v.num_records = 256u;

    Rdna2ImageDescriptor t{};
    t.base_address = 0x0000800000200000ull;
    t.format = static_cast<uint16_t>(Rdna2ImageFormat::R16G16B16A16_FLOAT);
    t.width = 512u;
    t.height = 512u;
    t.type = Rdna2ImageType::Img2DArray;
    t.base_array = 0u;
    t.last_array = 3u;

    Rdna2SamplerDescriptor s{};
    s.clamp_x = Rdna2ClampMode::Wrap;
    s.clamp_y = Rdna2ClampMode::Wrap;
    s.clamp_z = Rdna2ClampMode::Wrap;
    s.xy_mag_filter = Rdna2XyFilter::Bilinear;
    s.xy_min_filter = Rdna2XyFilter::Bilinear;
    s.mip_filter = Rdna2MipFilter::Linear;
    s.max_aniso_ratio = 3u;  // 8x

    uint32_t v_slot = 0u, t_slot = 0u, s_slot = 0u;
    OS_REQUIRE(vk_bridge.BindVSharpUav(v, nullptr, nullptr, v_slot).ok());
    OS_REQUIRE(vk_bridge.BindTSharpSrv(t, nullptr, nullptr, t_slot).ok());
    OS_REQUIRE(vk_bridge.BindSSharp(s, s_slot).ok());

    const VulkanBindlessLayoutTable table = vk_bridge.GetVulkanBindlessTable();
    OS_REQUIRE(table.entries.size() == 3u);
    OS_REQUIRE(table.entries[0].vk_descriptor_type == kVkDescriptorTypeStorageBuffer);
    OS_REQUIRE(table.entries[0].byte_range == 4096ull);
    OS_REQUIRE(table.entries[1].vk_descriptor_type == kVkDescriptorTypeSampledImage);
    OS_REQUIRE(table.entries[1].vk_format == 97u);  // VK_FORMAT_R16G16B16A16_SFLOAT
    OS_REQUIRE(table.entries[1].vk_image_view_type == kVkImageViewType2DArray);
    OS_REQUIRE(table.entries[1].layer_count == 4u);
    OS_REQUIRE(table.entries[2].vk_descriptor_type == kVkDescriptorTypeSampler);
    OS_REQUIRE(table.entries[2].max_anisotropy == 8u);

    // If a live Vulkan ICD is present, execute a quick command-list submission on it!
    VulkanCommandListDesc cl_desc{};
    cl_desc.submit_empty_fence_sync = true;
    cl_desc.commands.push_back(VulkanCommandItem{VulkanCommandOp::BindDescriptorSets, 0u, 0u, 0u, 0ull});
    cl_desc.commands.push_back(VulkanCommandItem{VulkanCommandOp::PipelineBarrier, 0u, 0u, 0u, 0ull});
    VulkanCommandListResult cl_res{};
    OS_REQUIRE(vk.RecordCommandList(cl_desc, cl_res).ok());

    std::printf("[PASS] Vulkan VK_EXT_descriptor_indexing table verified (device=%s, icd_exec=%s, entries=%zu)\n",
                info.device_name.c_str(),
                cl_res.executed_on_vulkan_icd ? "true" : "fallback",
                table.entries.size());
}

#endif  // defined(_WIN32)

}  // namespace

int main() {
#if !defined(_WIN32)
    std::printf("[SKIP] Windows-only test_unified_mem_bindless\n");
    return 0;
#else
    RunRdna2DescriptorRoundTripTests();

    int d3d12_passes = 0;

    // 1. Hardware D3D12 pass (rejects software/WARP adapters; gracefully skipped if headless CI).
    {
        D3D12TestContext hw_ctx;
        if (CreateD3D12Context(false, hw_ctx)) {
            RunTitleVaMirrorLiveTest(hw_ctx);
            RunBindlessLiveGpuTest(hw_ctx);
            ++d3d12_passes;
        } else {
            std::printf("[INFO] No discrete/HW D3D12 adapter available; skipping HW D3D12 pass (CI-safe)\n");
        }
    }

    // 2. D3D12 WARP pass (always executed for deterministic CI + reference parity).
    {
        D3D12TestContext warp_ctx;
        OS_REQUIRE(CreateD3D12Context(true, warp_ctx));
        RunTitleVaMirrorLiveTest(warp_ctx);
        RunBindlessLiveGpuTest(warp_ctx);
        ++d3d12_passes;
    }

    OS_REQUIRE(d3d12_passes >= 1);

    // 3. Guarded Vulkan VK_EXT_descriptor_indexing & ICD verification.
    RunVulkanBindlessGuardTest();

    std::printf("[ALL PASS] test_unified_mem_bindless (%d D3D12 adapter pass(es) + GFX10.3 V#/T#/S# + Vulkan indexing)\n",
                d3d12_passes);
    return 0;
#endif
}
