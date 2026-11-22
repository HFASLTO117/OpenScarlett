#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/shims/secondary_shims_abi.h"

#include <atomic>
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
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define OS_REQUIRE(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);                 \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#if defined(_WIN32)
class ScopedEnvVarA {
public:
    explicit ScopedEnvVarA(const char* name) : name_(name) {
        DWORD needed = ::GetEnvironmentVariableA(name, nullptr, 0);
        if (needed > 0) {
            had_prev_ = true;
            prev_.resize(needed);
            DWORD got = ::GetEnvironmentVariableA(name, prev_.data(), needed);
            if (got < needed) {
                prev_.resize(got);
            }
        }
    }
    ~ScopedEnvVarA() {
        if (had_prev_) {
            ::SetEnvironmentVariableA(name_.c_str(), prev_.c_str());
        } else {
            ::SetEnvironmentVariableA(name_.c_str(), nullptr);
        }
    }

private:
    std::string name_;
    std::string prev_;
    bool had_prev_ = false;
};

fs::path GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
    return fs::current_path();
}
#endif

fs::path MakeTempRoot(const char* subdir) {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec || base.empty()) {
        base = fs::current_path();
    }
    fs::path root = base / "openscarlett_s14_p0_tests" / subdir;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void WriteBytesFile(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// ===========================================================================
// 1. P0 XAudio2_9.dll Standard Ordinal Pinning (@1..@4)
// ===========================================================================
void TestXAudio2OrdinalPinning() {
    std::printf("[1/8] XAudio2_9.dll standard ordinal pinning (@1..@4)...\n");
#if defined(_WIN32)
    const fs::path dll_path = GetExeDir() / "XAudio2_9.dll";
    HMODULE h = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(h != nullptr);
    if (!h) return;

    FARPROC ord1 = ::GetProcAddress(h, MAKEINTRESOURCEA(1));
    FARPROC ord2 = ::GetProcAddress(h, MAKEINTRESOURCEA(2));
    FARPROC ord3 = ::GetProcAddress(h, MAKEINTRESOURCEA(3));
    FARPROC ord4 = ::GetProcAddress(h, MAKEINTRESOURCEA(4));

    FARPROC fn_create = ::GetProcAddress(h, "XAudio2Create");
    FARPROC fn_vol = ::GetProcAddress(h, "CreateAudioVolumeMeter");
    FARPROC fn_rev = ::GetProcAddress(h, "CreateAudioReverb");
    FARPROC fn_ver = ::GetProcAddress(h, "XAudio2CreateWithVersionInfo");
    FARPROC fn_spatial = ::GetProcAddress(h, "OpenScarlettCreateSpatialAudioClient");

    OS_REQUIRE(ord1 != nullptr && ord1 == fn_create);
    OS_REQUIRE(ord2 != nullptr && ord2 == fn_vol);
    OS_REQUIRE(ord3 != nullptr && ord3 == fn_rev);
    OS_REQUIRE(ord4 != nullptr && ord4 == fn_ver);
    OS_REQUIRE(fn_spatial != nullptr);

    // Invoke CreateAudioVolumeMeter (@2) & CreateAudioReverb (@3) live
    using PfnCreateApo = int32_t(__stdcall*)(void**);
    void* vol_apo = nullptr;
    void* rev_apo = nullptr;
    OS_REQUIRE(reinterpret_cast<PfnCreateApo>(ord2)(&vol_apo) == 0 && vol_apo != nullptr);
    OS_REQUIRE(reinterpret_cast<PfnCreateApo>(ord3)(&rev_apo) == 0 && rev_apo != nullptr);
    struct MinUnk {
        virtual int32_t __stdcall QueryInterface(const void*, void**) = 0;
        virtual uint32_t __stdcall AddRef() = 0;
        virtual uint32_t __stdcall Release() = 0;
    };
    static_cast<MinUnk*>(vol_apo)->Release();
    static_cast<MinUnk*>(rev_apo)->Release();

    ::FreeLibrary(h);
    std::printf("  PASS: XAudio2_9.dll @1=XAudio2Create @2=CreateAudioVolumeMeter "
                "@3=CreateAudioReverb @4=XAudio2CreateWithVersionInfo verified.\n");
#endif
}

// ===========================================================================
// 2. P0 XMemGetWorkingSetStatistics (kernelx.dll + xmem.dll)
// ===========================================================================
struct TestXMemWorkingSetStats {
    uint64_t Size;
    uint64_t TotalPhysicalBytes;
    uint64_t WorkingSetBytes;
    uint64_t PeakWorkingSetBytes;
    uint64_t PrivateUsageBytes;
    uint64_t AvailablePhysicalBytes;
    uint64_t PageFaultCount;
    uint64_t TrackedTitleAllocBytes;
};

void TestXMemGetWorkingSetStatistics() {
    std::printf("[2/8] XMemGetWorkingSetStatistics (kernelx.dll & xmem.dll)...\n");
#if defined(_WIN32)
    const fs::path kx_path = GetExeDir() / "kernelx.dll";
    const fs::path xm_path = GetExeDir() / "xmem.dll";
    HMODULE h_kx = ::LoadLibraryW(kx_path.c_str());
    HMODULE h_xm = ::LoadLibraryW(xm_path.c_str());
    OS_REQUIRE(h_kx != nullptr);
    OS_REQUIRE(h_xm != nullptr);
    if (!h_kx || !h_xm) return;

    using PfnStats = int32_t(__stdcall*)(void*);
    auto p_kx_stats = reinterpret_cast<PfnStats>(::GetProcAddress(h_kx, "XMemGetWorkingSetStatistics"));
    auto p_xm_stats = reinterpret_cast<PfnStats>(::GetProcAddress(h_xm, "XMemGetWorkingSetStatistics"));
    OS_REQUIRE(p_kx_stats != nullptr);
    OS_REQUIRE(p_xm_stats != nullptr);

    // Null buffer safety
    OS_REQUIRE(p_kx_stats(nullptr) == 0);
    OS_REQUIRE(p_xm_stats(nullptr) == 0);

    TestXMemWorkingSetStats st_kx{};
    st_kx.Size = sizeof(st_kx);
    OS_REQUIRE(p_kx_stats(&st_kx) != 0);
    OS_REQUIRE(st_kx.Size == sizeof(st_kx));
    OS_REQUIRE(st_kx.TotalPhysicalBytes > 0ull);
    OS_REQUIRE(st_kx.WorkingSetBytes > 0ull);
    OS_REQUIRE(st_kx.PeakWorkingSetBytes >= st_kx.WorkingSetBytes);

    TestXMemWorkingSetStats st_xm{};
    st_xm.Size = sizeof(st_xm);
    OS_REQUIRE(p_xm_stats(&st_xm) != 0);
    OS_REQUIRE(st_xm.TotalPhysicalBytes == st_kx.TotalPhysicalBytes);
    OS_REQUIRE(st_xm.WorkingSetBytes > 0ull);

    ::FreeLibrary(h_xm);
    ::FreeLibrary(h_kx);
    std::printf("  PASS: XMemGetWorkingSetStatistics TotalPhys=%llu MiB WorkingSet=%llu KiB.\n",
                static_cast<unsigned long long>(st_kx.TotalPhysicalBytes / (1024ull * 1024ull)),
                static_cast<unsigned long long>(st_kx.WorkingSetBytes / 1024ull));
#endif
}

// ===========================================================================
// 3. P0 Real AMD FidelityFX API 2.0 ABI (5/5 exports on BOTH DLLs + Live D3D12)
// ===========================================================================
struct Ffx2Dimensions2D {
    uint32_t width;
    uint32_t height;
};

struct Ffx2FloatCoords2D {
    float x;
    float y;
};

struct Ffx2ResourceDescription {
    uint32_t type;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct Ffx2Resource {
    void* resource;
    Ffx2ResourceDescription description;
    uint32_t state;
};

struct Ffx2DispatchDescUpscale {
    FfxApiHeader header;
    void* commandList;
    Ffx2Resource color;
    Ffx2Resource depth;
    Ffx2Resource motionVectors;
    Ffx2Resource exposure;
    Ffx2Resource reactive;
    Ffx2Resource transparencyAndComposition;
    Ffx2Resource output;
    Ffx2FloatCoords2D jitterOffset;
    Ffx2FloatCoords2D motionVectorScale;
    Ffx2Dimensions2D renderSize;
    Ffx2Dimensions2D upscaleSize;
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    uint32_t flags;
};

struct Ffx2QueryGetRatio {
    FfxApiHeader header;
    uint32_t qualityMode;
    uint32_t _pad;
    float* pOutUpscaleRatio;
};

struct Ffx2QueryGetJitterPhaseCount {
    FfxApiHeader header;
    int32_t renderWidth;
    int32_t displayWidth;
    int32_t* pOutPhaseCount;
};

struct Ffx2QueryGetJitterOffset {
    FfxApiHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

void TestFfxApi2OnDll(const wchar_t* dll_name, bool run_live_d3d12) {
#if defined(_WIN32)
    const fs::path dll_path = GetExeDir() / dll_name;
    HMODULE h = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(h != nullptr);
    if (!h) return;

    using PfnCreate = int32_t(__stdcall*)(FfxContext*, FfxCreateContextDescHeader*, const FfxAllocationCallbacks*);
    using PfnDestroy = int32_t(__stdcall*)(FfxContext*, const FfxAllocationCallbacks*);
    using PfnConfigure = int32_t(__stdcall*)(FfxContext*, const FfxConfigureDescHeader*);
    using PfnQuery = int32_t(__stdcall*)(FfxContext*, FfxQueryDescHeader*);
    using PfnDispatch = int32_t(__stdcall*)(FfxContext*, const FfxDispatchDescHeader*);

    auto p_create = reinterpret_cast<PfnCreate>(::GetProcAddress(h, "ffxCreateContext"));
    auto p_destroy = reinterpret_cast<PfnDestroy>(::GetProcAddress(h, "ffxDestroyContext"));
    auto p_configure = reinterpret_cast<PfnConfigure>(::GetProcAddress(h, "ffxConfigure"));
    auto p_query = reinterpret_cast<PfnQuery>(::GetProcAddress(h, "ffxQuery"));
    auto p_dispatch = reinterpret_cast<PfnDispatch>(::GetProcAddress(h, "ffxDispatch"));

    OS_REQUIRE(p_create != nullptr);
    OS_REQUIRE(p_destroy != nullptr);
    OS_REQUIRE(p_configure != nullptr);
    OS_REQUIRE(p_query != nullptr);
    OS_REQUIRE(p_dispatch != nullptr);
    if (!p_create || !p_destroy || !p_configure || !p_query || !p_dispatch) {
        ::FreeLibrary(h);
        return;
    }

    // 1. Real AMD FFX 2.0 GET_VERSIONS (0x00000004)
    uint64_t ver_count = 1ull;
    uint64_t ver_ids[1]{};
    const char* ver_names[1]{};
    FfxQueryDescGetVersions vq{};
    vq.header.type = 0x00000004ull;
    vq.createDescType = 0x00010000ull;
    vq.outputCount = &ver_count;
    vq.versionIds = ver_ids;
    vq.versionNames = ver_names;
    OS_REQUIRE(p_query(nullptr, &vq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(ver_count == 1ull && ver_ids[0] != 0ull && ver_names[0] != nullptr);

    // 2. GETUPSCALERATIOFROMQUALITYMODE (0x00010002)
    float ratio = 0.0f;
    Ffx2QueryGetRatio rq{};
    rq.header.type = 0x00010002ull;
    rq.qualityMode = FFX_UPSCALER_QUALITY_QUALITY;
    rq.pOutUpscaleRatio = &ratio;
    OS_REQUIRE(p_query(nullptr, &rq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(std::fabs(ratio - 1.5f) < 1e-4f);

    // 3. GETRENDERRESOLUTIONFROMQUALITYMODE (0x00010003)
    uint32_t rw = 0, rh = 0;
    FfxQueryDescUpscaleGetRenderResolutionFromQualityMode rrq{};
    rrq.header.type = 0x00010003ull;
    rrq.displayWidth = 1920u;
    rrq.displayHeight = 1080u;
    rrq.qualityMode = FFX_UPSCALER_QUALITY_QUALITY;
    rrq.pOutRenderWidth = &rw;
    rrq.pOutRenderHeight = &rh;
    OS_REQUIRE(p_query(nullptr, &rrq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(rw == 1280u && rh == 720u);

    // 4. GETJITTERPHASECOUNT (0x00010004) -> ceil(8 * (1920/1280)^2) = ceil(18.0) = 18
    int32_t phases = 0;
    Ffx2QueryGetJitterPhaseCount jq{};
    jq.header.type = 0x00010004ull;
    jq.renderWidth = 1280;
    jq.displayWidth = 1920;
    jq.pOutPhaseCount = &phases;
    OS_REQUIRE(p_query(nullptr, &jq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(phases == 18);

    // 5. GETJITTEROFFSET (0x00010005) -> Halton(2,3) in [-0.5, 0.5]
    for (int32_t idx = 0; idx < phases; ++idx) {
        float jx = 99.0f, jy = 99.0f;
        Ffx2QueryGetJitterOffset jo{};
        jo.header.type = 0x00010005ull;
        jo.index = idx;
        jo.phaseCount = phases;
        jo.pOutX = &jx;
        jo.pOutY = &jy;
        OS_REQUIRE(p_query(nullptr, &jo.header) == FFX_API_RETURN_OK);
        OS_REQUIRE(jx >= -0.5f && jx <= 0.5f);
        OS_REQUIRE(jy >= -0.5f && jy <= 0.5f);
    }

    if (run_live_d3d12) {
        // 6. Live D3D12 WARP/GPU ffxCreateContext + ffxConfigure (0x00010007) + ffxDispatch writing real pixels!
        ID3D12Device* dev = nullptr;
        IDXGIAdapter1* adapter = nullptr;
        openscarlett::D3D12AdapterSelectionOptions opts{};
        opts.prefer_warp = true;
        opts.allow_warp_fallback = true;
        openscarlett::D3D12AdapterInfo ainfo{};
        OS_REQUIRE(openscarlett::SelectD3D12AdapterAndCreateDevice(opts, &dev, &adapter, &ainfo).ok());
        OS_REQUIRE(dev != nullptr);
        if (dev) {
            constexpr uint32_t kSrcW = 16u, kSrcH = 16u, kDstW = 32u, kDstH = 32u;

            FfxCreateBackendDX12Desc dx12_desc{};
            dx12_desc.header.type = 0x00000002ull;
            dx12_desc.device = dev;

            FfxCreateContextDescUpscale up_desc{};
            up_desc.header.type = 0x00010000ull;
            up_desc.header.pNext = &dx12_desc.header;
            up_desc.maxRenderWidth = kSrcW;
            up_desc.maxRenderHeight = kSrcH;
            up_desc.maxUpscaleWidth = kDstW;
            up_desc.maxUpscaleHeight = kDstH;

            FfxContext ctx = nullptr;
            OS_REQUIRE(p_create(&ctx, &up_desc.header, nullptr) == FFX_API_RETURN_OK);
            OS_REQUIRE(ctx != nullptr);

            // Real AMD FFX 2.0 configure key-value (0x00010007)
            float sharp = 0.75f;
            FfxConfigureDescUpscaleKeyValue kv{};
            kv.header.type = 0x00010007ull;
            kv.key = FFX_UPSCALE_CONFIGURE_KEY_SHARPNESS;
            kv.ptr = &sharp;
            OS_REQUIRE(p_configure(&ctx, &kv.header) == FFX_API_RETURN_OK);

            // Create D3D12 command queue, allocator, list, fence, color texture, output UAV texture, upload & readback buffers
            ID3D12CommandQueue* queue = nullptr;
            ID3D12CommandAllocator* alloc = nullptr;
            ID3D12GraphicsCommandList* cmd = nullptr;
            ID3D12Fence* fence = nullptr;
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
            OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
            OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))));
            OS_REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

            D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
            D3D12_HEAP_PROPERTIES hp_up{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
            D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

            D3D12_RESOURCE_DESC src_rd{};
            src_rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            src_rd.Width = kSrcW;
            src_rd.Height = kSrcH;
            src_rd.DepthOrArraySize = 1;
            src_rd.MipLevels = 1;
            src_rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            src_rd.SampleDesc.Count = 1;

            D3D12_RESOURCE_DESC dst_rd = src_rd;
            dst_rd.Width = kDstW;
            dst_rd.Height = kDstH;
            dst_rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            ID3D12Resource* src_tex = nullptr;
            ID3D12Resource* dst_tex = nullptr;
            ID3D12Resource* up_buf = nullptr;
            ID3D12Resource* rb_buf = nullptr;
            OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
                &hp_def, D3D12_HEAP_FLAG_NONE, &src_rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&src_tex))));
            OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
                &hp_def, D3D12_HEAP_FLAG_NONE, &dst_rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dst_tex))));

            const UINT src_pitch = (kSrcW * 4u + 255u) & ~255u;
            const UINT dst_pitch = (kDstW * 4u + 255u) & ~255u;
            D3D12_RESOURCE_DESC up_rd{};
            up_rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            up_rd.Width = static_cast<UINT64>(src_pitch) * kSrcH;
            up_rd.Height = 1;
            up_rd.DepthOrArraySize = 1;
            up_rd.MipLevels = 1;
            up_rd.SampleDesc.Count = 1;
            up_rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
                &hp_up, D3D12_HEAP_FLAG_NONE, &up_rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up_buf))));

            D3D12_RESOURCE_DESC rb_rd = up_rd;
            rb_rd.Width = static_cast<UINT64>(dst_pitch) * kDstH;
            OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
                &hp_rb, D3D12_HEAP_FLAG_NONE, &rb_rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb_buf))));

            // Fill input with vivid non-zero color (R=200, G=120, B=60, A=255)
            void* mapped = nullptr;
            D3D12_RANGE rnone{0, 0};
            OS_REQUIRE(SUCCEEDED(up_buf->Map(0, &rnone, &mapped)));
            for (uint32_t y = 0; y < kSrcH; ++y) {
                auto* row = static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * src_pitch;
                for (uint32_t x = 0; x < kSrcW; ++x) {
                    row[x * 4u + 0u] = 200u;
                    row[x * 4u + 1u] = 120u;
                    row[x * 4u + 2u] = 60u;
                    row[x * 4u + 3u] = 255u;
                }
            }
            up_buf->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION ul_src{};
            ul_src.pResource = up_buf;
            ul_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ul_src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            ul_src.PlacedFootprint.Footprint.Width = kSrcW;
            ul_src.PlacedFootprint.Footprint.Height = kSrcH;
            ul_src.PlacedFootprint.Footprint.Depth = 1;
            ul_src.PlacedFootprint.Footprint.RowPitch = src_pitch;

            D3D12_TEXTURE_COPY_LOCATION ul_dst{};
            ul_dst.pResource = src_tex;
            ul_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&ul_dst, 0, 0, 0, &ul_src, nullptr);

            D3D12_RESOURCE_BARRIER b_src{};
            b_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b_src.Transition.pResource = src_tex;
            b_src.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b_src.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &b_src);

            // Real AMD FFX 2.0 Dispatch!
            Ffx2DispatchDescUpscale d2{};
            d2.header.type = 0x00010001ull;
            d2.commandList = cmd;
            d2.color.resource = src_tex;
            d2.color.description.type = 3u;
            d2.color.description.width = kSrcW;
            d2.color.description.height = kSrcH;
            d2.color.state = 0x0002u;  // FFX_API_RESOURCE_STATE_COMPUTE_READ
            d2.output.resource = dst_tex;
            d2.output.description.type = 3u;
            d2.output.description.width = kDstW;
            d2.output.description.height = kDstH;
            d2.output.state = 0x0001u;  // FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
            d2.renderSize = {kSrcW, kSrcH};
            d2.upscaleSize = {kDstW, kDstH};
            d2.enableSharpening = true;
            d2.sharpness = 0.5f;
            d2.frameTimeDelta = 16.6f;
            d2.preExposure = 1.0f;
            d2.reset = true;

            OS_REQUIRE(p_dispatch(&ctx, &d2.header) == FFX_API_RETURN_OK);

            D3D12_RESOURCE_BARRIER b_out{};
            b_out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b_out.Transition.pResource = dst_tex;
            b_out.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b_out.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b_out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &b_out);

            D3D12_TEXTURE_COPY_LOCATION rb_src{};
            rb_src.pResource = dst_tex;
            rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION rb_dst{};
            rb_dst.pResource = rb_buf;
            rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rb_dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rb_dst.PlacedFootprint.Footprint.Width = kDstW;
            rb_dst.PlacedFootprint.Footprint.Height = kDstH;
            rb_dst.PlacedFootprint.Footprint.Depth = 1;
            rb_dst.PlacedFootprint.Footprint.RowPitch = dst_pitch;
            cmd->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);

            OS_REQUIRE(SUCCEEDED(cmd->Close()));
            ID3D12CommandList* lists[] = {cmd};
            queue->ExecuteCommandLists(1, lists);

            HANDLE ev = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
            OS_REQUIRE(SUCCEEDED(queue->Signal(fence, 1)));
            if (fence->GetCompletedValue() < 1) {
                OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, ev)));
                OS_REQUIRE(::WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0);
            }
            ::CloseHandle(ev);

            void* rb_ptr = nullptr;
            D3D12_RANGE rb_range{0, static_cast<SIZE_T>(rb_rd.Width)};
            OS_REQUIRE(SUCCEEDED(rb_buf->Map(0, &rb_range, &rb_ptr)));
            const auto* center = static_cast<const uint8_t*>(rb_ptr) +
                                 static_cast<size_t>(kDstH / 2u) * dst_pitch +
                                 static_cast<size_t>(kDstW / 2u) * 4u;
            OS_REQUIRE(center[0] >= 185u && center[0] <= 215u);
            OS_REQUIRE(center[1] >= 105u && center[1] <= 135u);
            OS_REQUIRE(center[2] >= 45u && center[2] <= 75u);
            OS_REQUIRE(center[3] == 255u);
            rb_buf->Unmap(0, &rnone);

            OS_REQUIRE(p_destroy(&ctx, nullptr) == FFX_API_RETURN_OK);
            OS_REQUIRE(ctx == nullptr);

            rb_buf->Release();
            up_buf->Release();
            dst_tex->Release();
            src_tex->Release();
            fence->Release();
            cmd->Release();
            alloc->Release();
            queue->Release();
            dev->Release();
        }
        if (adapter) adapter->Release();
    }

    ::FreeLibrary(h);
#else
    (void)dll_name;
    (void)run_live_d3d12;
#endif
}

void TestFidelityFxApi2BothDlls() {
    std::printf("[3/8] Real AMD FidelityFX API 2.0 (5/5 exports on BOTH DLLs + live D3D12)...\n");
    TestFfxApi2OnDll(L"amd_fidelityfx_upscaler_xbox.dll", true);
    TestFfxApi2OnDll(L"amd_fidelityfx_loader_xbox.dll", true);
    std::printf("  PASS: Both amd_fidelityfx_upscaler_xbox.dll & amd_fidelityfx_loader_xbox.dll "
                "verified with live D3D12 GPU/WARP upscale!\n");
}

// ===========================================================================
// 4. Smart Middleware Routing (Bink2Scarlett + OPENSCARLETT_NET_ROUTING)
// ===========================================================================
void TestSmartMiddlewareRouting() {
    std::printf("[4/8] Smart Bink2Scarlett & OPENSCARLETT_NET_ROUTING routing...\n");
    using namespace openscarlett;

    const fs::path root = MakeTempRoot("routing");
    const fs::path shim_dir = root / "shims";
    const fs::path guest_dir = root / "guest";
    const fs::path empty_guest = root / "empty_guest";
    std::error_code ec;
    fs::create_directories(empty_guest, ec);

    WriteBytesFile(shim_dir / "Bink2Scarlett.dll", "SHIM_BINK2");
    WriteBytesFile(shim_dir / "XCurl.dll", "SHIM_XCURL");
    WriteBytesFile(shim_dir / "libHttpClient.GDK.dll", "SHIM_LHC");
    WriteBytesFile(guest_dir / "Bink2Scarlett.dll", "GUEST_BINK2");
    WriteBytesFile(guest_dir / "XCurl.dll", "GUEST_XCURL");
    WriteBytesFile(guest_dir / "libHttpClient.GDK.dll", "GUEST_LHC");

    // Bink2Scarlett auto-prefers NativeGuestDll when present in guest_dir, shim when absent
    DllRoutingTable table;
    auto b_guest = table.ResolveRoute("Bink2Scarlett.dll", shim_dir.wstring(), guest_dir.wstring());
    OS_REQUIRE(b_guest.policy == DllRoutePolicy::NativeGuestDll);
    OS_REQUIRE(b_guest.resolved_path == (guest_dir / L"Bink2Scarlett.dll").wstring());

    auto b_fallback = table.ResolveRoute("Bink2Scarlett.dll", shim_dir.wstring(), empty_guest.wstring());
    OS_REQUIRE(b_fallback.policy == DllRoutePolicy::Shim);
    OS_REQUIRE(b_fallback.resolved_path == (shim_dir / L"Bink2Scarlett.dll").wstring());

    // Default net routing preserves offline shims
    auto x_def = table.ResolveRoute("XCurl.dll", shim_dir.wstring(), guest_dir.wstring());
    auto l_def = table.ResolveRoute("libHttpClient.GDK.dll", shim_dir.wstring(), guest_dir.wstring());
    OS_REQUIRE(x_def.policy == DllRoutePolicy::Shim);
    OS_REQUIRE(l_def.policy == DllRoutePolicy::Shim);

#if defined(_WIN32)
    {
        ScopedEnvVarA env_net("OPENSCARLETT_NET_ROUTING");
        ScopedEnvVarA env_dll("OPENSCARLETT_DLL_ROUTING");
        ::SetEnvironmentVariableA("OPENSCARLETT_DLL_ROUTING", nullptr);

        ::SetEnvironmentVariableA("OPENSCARLETT_NET_ROUTING", "guest");
        DllRoutingTable net_guest = DllRoutingTable::CreateWithEnvOverrides();
        auto xg = net_guest.ResolveRoute("XCurl.dll", shim_dir.wstring(), guest_dir.wstring());
        auto lg = net_guest.ResolveRoute("libHttpClient.GDK.dll", shim_dir.wstring(), guest_dir.wstring());
        OS_REQUIRE(xg.policy == DllRoutePolicy::NativeGuestDll);
        OS_REQUIRE(xg.resolved_path == (guest_dir / L"XCurl.dll").wstring());
        OS_REQUIRE(lg.policy == DllRoutePolicy::NativeGuestDll);
        OS_REQUIRE(lg.resolved_path == (guest_dir / L"libHttpClient.GDK.dll").wstring());

        ::SetEnvironmentVariableA("OPENSCARLETT_NET_ROUTING", "shim");
        DllRoutingTable net_shim = DllRoutingTable::CreateWithEnvOverrides();
        auto xs = net_shim.ResolveRoute("XCurl.dll", shim_dir.wstring(), guest_dir.wstring());
        auto ls = net_shim.ResolveRoute("libHttpClient.GDK.dll", shim_dir.wstring(), guest_dir.wstring());
        OS_REQUIRE(xs.policy == DllRoutePolicy::Shim);
        OS_REQUIRE(ls.policy == DllRoutePolicy::Shim);
    }
#endif
    std::printf("  PASS: Bink2Scarlett NativeGuestDll auto-preference & OPENSCARLETT_NET_ROUTING verified.\n");
}

// ===========================================================================
// 5. XCurl 15 Form/MIME Exports & libHttpClient WebSocket/HCHttpCall Exports
// ===========================================================================
void TestXCurlMimeAndLibHttpClientWebSocket() {
    std::printf("[5/8] XCurl 15 curl_form*/curl_mime_* & libHttpClient WebSocket/HCHttpCall exports...\n");
#if defined(_WIN32)
    // 5a. Verify all 15 XCurl form/mime exports + functional round-trip
    HMODULE h_curl = ::LoadLibraryW((GetExeDir() / "XCurl.dll").c_str());
    OS_REQUIRE(h_curl != nullptr);
    if (h_curl) {
        static constexpr const char* kCurl15[15] = {
            "curl_formadd", "curl_formfree", "curl_formget",
            "curl_mime_init", "curl_mime_free", "curl_mime_addpart",
            "curl_mime_name", "curl_mime_filename", "curl_mime_type",
            "curl_mime_encoder", "curl_mime_data", "curl_mime_filedata",
            "curl_mime_data_cb", "curl_mime_subparts", "curl_mime_headers",
        };
        for (const char* sym : kCurl15) {
            OS_REQUIRE(::GetProcAddress(h_curl, sym) != nullptr);
        }

        using PfnFormAdd = int32_t(__cdecl*)(void**, void**, ...);
        using PfnFormFree = void(__cdecl*)(void*);
        using PfnFormGetCb = size_t(__cdecl*)(void*, const char*, size_t);
        using PfnFormGet = size_t(__cdecl*)(void*, void*, PfnFormGetCb);
        using PfnMimeInit = void*(__cdecl*)(XCurlEasyHandle);
        using PfnMimeFree = void(__cdecl*)(void*);
        using PfnMimeAddPart = void*(__cdecl*)(void*);
        using PfnMimeStr = int32_t(__cdecl*)(void*, const char*);
        using PfnMimeData = int32_t(__cdecl*)(void*, const char*, size_t);
        using PfnMimeSub = int32_t(__cdecl*)(void*, void*);
        using PfnMimeHdrs = int32_t(__cdecl*)(void*, XCurlSlist*, int32_t);

        auto p_formadd = reinterpret_cast<PfnFormAdd>(::GetProcAddress(h_curl, "curl_formadd"));
        auto p_formfree = reinterpret_cast<PfnFormFree>(::GetProcAddress(h_curl, "curl_formfree"));
        auto p_formget = reinterpret_cast<PfnFormGet>(::GetProcAddress(h_curl, "curl_formget"));
        auto p_mime_init = reinterpret_cast<PfnMimeInit>(::GetProcAddress(h_curl, "curl_mime_init"));
        auto p_mime_free = reinterpret_cast<PfnMimeFree>(::GetProcAddress(h_curl, "curl_mime_free"));
        auto p_mime_add = reinterpret_cast<PfnMimeAddPart>(::GetProcAddress(h_curl, "curl_mime_addpart"));
        auto p_mime_name = reinterpret_cast<PfnMimeStr>(::GetProcAddress(h_curl, "curl_mime_name"));
        auto p_mime_file = reinterpret_cast<PfnMimeStr>(::GetProcAddress(h_curl, "curl_mime_filename"));
        auto p_mime_type = reinterpret_cast<PfnMimeStr>(::GetProcAddress(h_curl, "curl_mime_type"));
        auto p_mime_enc = reinterpret_cast<PfnMimeStr>(::GetProcAddress(h_curl, "curl_mime_encoder"));
        auto p_mime_data = reinterpret_cast<PfnMimeData>(::GetProcAddress(h_curl, "curl_mime_data"));
        auto p_mime_sub = reinterpret_cast<PfnMimeSub>(::GetProcAddress(h_curl, "curl_mime_subparts"));
        auto p_mime_hdrs = reinterpret_cast<PfnMimeHdrs>(::GetProcAddress(h_curl, "curl_mime_headers"));

        void* form_first = nullptr;
        void* form_last = nullptr;
        OS_REQUIRE(p_formadd(&form_first, &form_last, 1, "field1", 4, "hello_form", 17) == 0);
        OS_REQUIRE(form_first != nullptr);
        std::string captured_form;
        auto form_cb = [](void* arg, const char* buf, size_t len) -> size_t {
            static_cast<std::string*>(arg)->append(buf, len);
            return len;
        };
        OS_REQUIRE(p_formget(form_first, &captured_form, +form_cb) == 0u);
        OS_REQUIRE(captured_form.find("hello_form") != std::string::npos);
        p_formfree(form_first);

        XCurlEasyHandle easy = curl_easy_init();
        OS_REQUIRE(easy != nullptr);
        void* mime = p_mime_init(easy);
        OS_REQUIRE(mime != nullptr);
        void* part = p_mime_add(mime);
        OS_REQUIRE(part != nullptr);
        OS_REQUIRE(p_mime_name(part, "payload") == XCURLE_OK);
        OS_REQUIRE(p_mime_file(part, "save.bin") == XCURLE_OK);
        OS_REQUIRE(p_mime_type(part, "application/octet-stream") == XCURLE_OK);
        OS_REQUIRE(p_mime_enc(part, "binary") == XCURLE_OK);
        OS_REQUIRE(p_mime_data(part, "ABCD1234", 8u) == XCURLE_OK);
        XCurlSlist* ph = curl_slist_append(nullptr, "X-Part: 1");
        OS_REQUIRE(p_mime_hdrs(part, ph, 1) == XCURLE_OK);

        void* sub = p_mime_init(easy);
        void* sub_part = p_mime_add(sub);
        OS_REQUIRE(p_mime_name(sub_part, "nested") == XCURLE_OK);
        OS_REQUIRE(p_mime_data(sub_part, "sub_ok", static_cast<size_t>(-1)) == XCURLE_OK);
        void* p2 = p_mime_add(mime);
        OS_REQUIRE(p_mime_sub(p2, sub) == XCURLE_OK);

        // Attach MIME via CURLOPT_MIMEPOST (10269)
        OS_REQUIRE(curl_easy_setopt(easy, 10269, mime) == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_URL, "https://api.openscarlett.local/upload") == XCURLE_OK);
        OS_REQUIRE(curl_easy_perform(easy) == XCURLE_OK);

        p_mime_free(mime);
        curl_easy_cleanup(easy);
        ::FreeLibrary(h_curl);
    }

    // 5b. Verify libHttpClient WebSocket & HCHttpCall exports + live round-trip
    HMODULE h_hc = ::LoadLibraryW((GetExeDir() / "libHttpClient.GDK.dll").c_str());
    OS_REQUIRE(h_hc != nullptr);
    if (h_hc) {
        static constexpr const char* kHcExports[] = {
            "HCWebSocketCreate", "HCWebSocketConnectAsync", "HCWebSocketSendMessageAsync",
            "HCWebSocketSendBinaryMessageAsync", "HCWebSocketDisconnect",
            "HCWebSocketCloseHandle", "HCWebSocketDuplicateHandle",
            "HCWebSocketSetProxyUri", "HCWebSocketSetHeader",
            "HCWebSocketSetFunctions", "HCWebSocketGetEventFunctions",
            "HCWebSocketSetMaxReceiveBufferSize",
            "HCHttpCallRequestSetTimeout", "HCHttpCallRequestSetRetryAllowed",
            "HCHttpCallRequestSetRetryDelay", "HCHttpCallRequestSetTimeoutWindow",
            "HCHttpCallResponseGetNetworkErrorCode",
            "HCHttpCallResponseGetPlatformNetworkErrorMessage",
            "HCHttpCallResponseGetNumHeaders", "HCHttpCallResponseGetHeaderAtIndex",
        };
        for (const char* sym : kHcExports) {
            OS_REQUIRE(::GetProcAddress(h_hc, sym) != nullptr);
        }

        OS_REQUIRE(HCInitialize(nullptr) == OS_SHIM_S_OK);
        HCCallHandle call = nullptr;
        OS_REQUIRE(HCHttpCallCreate(&call) == OS_SHIM_S_OK && call != nullptr);

        using PfnSetU32 = int32_t(__stdcall*)(HCCallHandle, uint32_t);
        using PfnGetU32 = int32_t(__stdcall*)(HCCallHandle, uint32_t*);
        using PfnSetBool = int32_t(__stdcall*)(HCCallHandle, bool);
        using PfnGetNetErr = int32_t(__stdcall*)(HCCallHandle, int32_t*, uint32_t*);
        using PfnGetStr = int32_t(__stdcall*)(HCCallHandle, const char**);
        using PfnGetHdrIdx = int32_t(__stdcall*)(HCCallHandle, uint32_t, const char**, const char**);

        OS_REQUIRE(reinterpret_cast<PfnSetU32>(::GetProcAddress(h_hc, "HCHttpCallRequestSetTimeout"))(call, 45u) == 0);
        OS_REQUIRE(reinterpret_cast<PfnSetBool>(::GetProcAddress(h_hc, "HCHttpCallRequestSetRetryAllowed"))(call, false) == 0);
        OS_REQUIRE(reinterpret_cast<PfnSetU32>(::GetProcAddress(h_hc, "HCHttpCallRequestSetRetryDelay"))(call, 5u) == 0);
        OS_REQUIRE(reinterpret_cast<PfnSetU32>(::GetProcAddress(h_hc, "HCHttpCallRequestSetTimeoutWindow"))(call, 60u) == 0);
        OS_REQUIRE(HCHttpCallRequestSetUrl(call, "GET", "https://api.openscarlett.local/hc") == 0);

        XAsyncBlock ab{};
        OS_REQUIRE(HCHttpCallPerformAsync(call, &ab) == 0);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XGR_S_OK);

        int32_t net_err = -1;
        uint32_t plat_err = 99u;
        OS_REQUIRE(reinterpret_cast<PfnGetNetErr>(::GetProcAddress(h_hc, "HCHttpCallResponseGetNetworkErrorCode"))(call, &net_err, &plat_err) == 0);
        OS_REQUIRE(net_err == 0 && plat_err == 0u);

        const char* plat_msg = nullptr;
        OS_REQUIRE(reinterpret_cast<PfnGetStr>(::GetProcAddress(h_hc, "HCHttpCallResponseGetPlatformNetworkErrorMessage"))(call, &plat_msg) == 0);
        OS_REQUIRE(plat_msg != nullptr);

        uint32_t num_hdrs = 0u;
        OS_REQUIRE(reinterpret_cast<PfnGetU32>(::GetProcAddress(h_hc, "HCHttpCallResponseGetNumHeaders"))(call, &num_hdrs) == 0);
        OS_REQUIRE(num_hdrs >= 1u);
        const char* hn = nullptr;
        const char* hv = nullptr;
        OS_REQUIRE(reinterpret_cast<PfnGetHdrIdx>(::GetProcAddress(h_hc, "HCHttpCallResponseGetHeaderAtIndex"))(call, 0u, &hn, &hv) == 0);
        OS_REQUIRE(hn != nullptr && hv != nullptr);
        OS_REQUIRE(HCHttpCallCloseHandle(call) == 0);

        // WebSocket lifecycle
        using PfnWsCreate = int32_t(__stdcall*)(void**, void*, void*, void*, void*);
        using PfnWsDup = void*(__stdcall*)(void*);
        using PfnWsClose = int32_t(__stdcall*)(void*);
        using PfnWsConnect = int32_t(__stdcall*)(const char*, const char*, void*, XAsyncBlock*);
        using PfnWsSendText = int32_t(__stdcall*)(void*, const char*, XAsyncBlock*);
        using PfnWsSendBin = int32_t(__stdcall*)(void*, const uint8_t*, uint32_t, XAsyncBlock*);
        using PfnWsDisconnect = int32_t(__stdcall*)(void*);
        using PfnWsMaxBuf = int32_t(__stdcall*)(void*, size_t);

        void* ws = nullptr;
        OS_REQUIRE(reinterpret_cast<PfnWsCreate>(::GetProcAddress(h_hc, "HCWebSocketCreate"))(&ws, nullptr, nullptr, nullptr, nullptr) == 0 && ws != nullptr);
        OS_REQUIRE(reinterpret_cast<PfnWsMaxBuf>(::GetProcAddress(h_hc, "HCWebSocketSetMaxReceiveBufferSize"))(ws, 131072u) == 0);
        void* ws_dup = reinterpret_cast<PfnWsDup>(::GetProcAddress(h_hc, "HCWebSocketDuplicateHandle"))(ws);
        OS_REQUIRE(ws_dup == ws);

        XAsyncBlock ws_ab1{};
        OS_REQUIRE(reinterpret_cast<PfnWsConnect>(::GetProcAddress(h_hc, "HCWebSocketConnectAsync"))("wss://ws.openscarlett.local/live", "json", ws, &ws_ab1) == 0);
        OS_REQUIRE(XAsyncGetStatus(&ws_ab1, true) == XGR_S_OK);

        XAsyncBlock ws_ab2{};
        OS_REQUIRE(reinterpret_cast<PfnWsSendText>(::GetProcAddress(h_hc, "HCWebSocketSendMessageAsync"))(ws, "{\"ping\":1}", &ws_ab2) == 0);
        OS_REQUIRE(XAsyncGetStatus(&ws_ab2, true) == XGR_S_OK);

        const uint8_t bin_data[4] = {1u, 2u, 3u, 4u};
        XAsyncBlock ws_ab3{};
        OS_REQUIRE(reinterpret_cast<PfnWsSendBin>(::GetProcAddress(h_hc, "HCWebSocketSendBinaryMessageAsync"))(ws, bin_data, 4u, &ws_ab3) == 0);
        OS_REQUIRE(XAsyncGetStatus(&ws_ab3, true) == XGR_S_OK);

        OS_REQUIRE(reinterpret_cast<PfnWsDisconnect>(::GetProcAddress(h_hc, "HCWebSocketDisconnect"))(ws) == 0);
        OS_REQUIRE(reinterpret_cast<PfnWsClose>(::GetProcAddress(h_hc, "HCWebSocketCloseHandle"))(ws_dup) == 0);
        OS_REQUIRE(reinterpret_cast<PfnWsClose>(::GetProcAddress(h_hc, "HCWebSocketCloseHandle"))(ws) == 0);

        HCCleanup();
        ::FreeLibrary(h_hc);
    }
    std::printf("  PASS: 15 XCurl form/mime & 20 libHttpClient WebSocket/HCHttpCall exports verified.\n");
#endif
}

// ===========================================================================
// 6. PE Inspection Hardening (Unwritable TLS AddressOfIndex + Raw > EOF)
// ===========================================================================
std::vector<uint8_t> BuildSyntheticPe64(
    uint32_t section_chars,
    uint32_t section_raw_size_override,
    bool with_tls,
    uint64_t tls_addr_of_index_va) {
    constexpr uint32_t kPeOff = 0x80u;
    constexpr uint32_t kOptOff = kPeOff + 24u;
    constexpr uint32_t kOptSize = 240u;
    constexpr uint32_t kSecOff = kOptOff + kOptSize;
    constexpr uint64_t kImgBase = 0x140000000ull;

    std::vector<uint8_t> img(0x600u, 0u);
    auto write_u16 = [&](size_t off, uint16_t v) { std::memcpy(img.data() + off, &v, 2); };
    auto write_u32 = [&](size_t off, uint32_t v) { std::memcpy(img.data() + off, &v, 4); };
    auto write_u64 = [&](size_t off, uint64_t v) { std::memcpy(img.data() + off, &v, 8); };

    write_u16(0, 0x5A4Du);
    write_u32(0x3C, kPeOff);
    write_u32(kPeOff, 0x00004550u);
    write_u16(kPeOff + 4, 0x8664u);
    write_u16(kPeOff + 6, 1u);       // NumberOfSections = 1
    write_u16(kPeOff + 20, kOptSize);
    write_u16(kPeOff + 22, 0x0022u);

    write_u16(kOptOff, 0x020Bu);     // PE32+
    write_u32(kOptOff + 16, 0x1000u);
    write_u64(kOptOff + 24, kImgBase);
    write_u32(kOptOff + 56, 0x3000u);  // SizeOfImage
    write_u16(kOptOff + 68, 3u);
    write_u32(kOptOff + 108, 16u);     // NumberOfRvaAndSizes

    if (with_tls) {
        write_u32(kOptOff + 112 + 9u * 8u, 0x1000u);      // TLS dir RVA
        write_u32(kOptOff + 112 + 9u * 8u + 4u, 40u);     // sizeof(IMAGE_TLS_DIRECTORY64)
        // Put TLS directory at file offset 0x400 (RVA 0x1000)
        write_u64(0x400u + 16u, tls_addr_of_index_va);
    }

    // Section 0 at RVA 0x1000, file offset 0x400, size 0x200
    std::memcpy(img.data() + kSecOff, ".tlssec", 7);
    write_u32(kSecOff + 8, 0x200u);                        // VirtualSize
    write_u32(kSecOff + 12, 0x1000u);                      // VirtualAddress
    write_u32(kSecOff + 16, section_raw_size_override);    // SizeOfRawData
    write_u32(kSecOff + 20, 0x400u);                       // PointerToRawData
    write_u32(kSecOff + 36, section_chars);                // Characteristics

    return img;
}

void TestPeHardening() {
    std::printf("[6/8] PE hardening (TLS unwritable AddressOfIndex & section raw > EOF)...\n");
    using namespace openscarlett;

    constexpr uint32_t kReadOnly = 0x40000040u;   // INITIALIZED_DATA | MEM_READ (NO MEM_WRITE)
    constexpr uint32_t kReadWrite = 0xC0000040u;  // INITIALIZED_DATA | MEM_READ | MEM_WRITE
    constexpr uint64_t kImgBase = 0x140000000ull;

    // 1. Valid PE with writable TLS AddressOfIndex -> PASS
    {
        auto pe = BuildSyntheticPe64(kReadWrite, 0x200u, true, kImgBase + 0x1040ull);
        PeInspectionReport rep{};
        OS_REQUIRE(InspectPeBytes(pe, "valid_tls.exe", rep).ok());
        OS_REQUIRE(rep.has_tls);
    }

    // 2. Unwritable TLS AddressOfIndex (in read-only section) -> REJECT (Xbox2Windows #543)
    {
        auto pe = BuildSyntheticPe64(kReadOnly, 0x200u, true, kImgBase + 0x1040ull);
        PeInspectionReport rep{};
        OS_REQUIRE(!InspectPeBytes(pe, "ro_tls.exe", rep).ok());
    }

    // 3. Out-of-bounds TLS AddressOfIndex -> REJECT
    {
        auto pe = BuildSyntheticPe64(kReadWrite, 0x200u, true, kImgBase + 0x9000ull);
        PeInspectionReport rep{};
        OS_REQUIRE(!InspectPeBytes(pe, "oob_tls.exe", rep).ok());
    }

    // 4. Section PointerToRawData + SizeOfRawData > EOF -> REJECT (Xbox2Windows #143)
    {
        auto pe = BuildSyntheticPe64(kReadWrite, 0x1000u, false, 0ull);
        PeInspectionReport rep{};
        OS_REQUIRE(!InspectPeBytes(pe, "trunc_sec.exe", rep).ok());
    }

    std::printf("  PASS: PE unwritable TLS AddressOfIndex & truncated section raw-data rejections verified.\n");
}

// ===========================================================================
// 7. MicrosoftGame.Config Parsing + OPENSCARLETT_LOCALE + P:\ PLS Quota
// ===========================================================================
void TestGameConfigPlsAndLocale() {
    std::printf("[7/8] MicrosoftGame.Config (<PersistentLocalStorage>, <VirtualMachine>, <StoreId>, <Resources>) + PLS & Locale...\n");
    using namespace openscarlett;

    const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="OpenScarlett.Sprint14Test" Publisher="CN=OpenScarlett" Version="1.2.3.4" />
  <TitleId>5CA14001</TitleId>
  <StoreId>9ND00FD7RLN1</StoreId>
  <ShellVisuals DefaultDisplayName="Sprint 14 Config Test" />
  <ExecutableList>
    <Executable Name="TestGame.exe" Id="Game" TargetDeviceFamily="Scarlett" />
  </ExecutableList>
  <PersistentLocalStorage>
    <SizeMB>2048</SizeMB>
    <GrowableToMB>4096</GrowableToMB>
  </PersistentLocalStorage>
  <VirtualMachine SMTEnabled="false" XboxLockhartTitleMemory="8192" XboxAnacondaTitleMemory="13824" />
  <Resources>
    <Resource Language="fr-FR" />
    <Resource Language="en-US" />
    <Resource Language="ja-JP" />
  </Resources>
</Game>
)";

    GameConfig cfg{};
    OS_REQUIRE(ParseGameConfigXml(xml, "MicrosoftGame.Config", cfg).ok());
    OS_REQUIRE(cfg.store_id == "9ND00FD7RLN1");
    OS_REQUIRE(cfg.persistent_local_storage.has_config);
    OS_REQUIRE(cfg.persistent_local_storage.size_mb == 2048ull);
    OS_REQUIRE(cfg.persistent_local_storage.has_growable_to_mb);
    OS_REQUIRE(cfg.persistent_local_storage.growable_to_mb == 4096ull);
    OS_REQUIRE(cfg.virtual_machine.has_config);
    OS_REQUIRE(cfg.virtual_machine.has_smt_enabled && !cfg.virtual_machine.smt_enabled);
    OS_REQUIRE(cfg.virtual_machine.has_lockhart_title_memory &&
               cfg.virtual_machine.xbox_lockhart_title_memory == 8192ull);
    OS_REQUIRE(cfg.virtual_machine.has_anaconda_title_memory &&
               cfg.virtual_machine.xbox_anaconda_title_memory == 13824ull);
    OS_REQUIRE(cfg.resources.size() == 3u);
    OS_REQUIRE(cfg.resources[0] == "fr-FR" && cfg.resources[1] == "en-US" && cfg.resources[2] == "ja-JP");

#if defined(_WIN32)
    const fs::path root = MakeTempRoot("config_pls");
    const fs::path cfg_path = root / "MicrosoftGame.Config";
    const fs::path save_root = root / "saves";
    WriteBytesFile(cfg_path, xml);

    ScopedEnvVarA env_cfg("OPENSCARLETT_GAME_CONFIG");
    ScopedEnvVarA env_save("OPENSCARLETT_SAVE_ROOT");
    ScopedEnvVarA env_loc("OPENSCARLETT_LOCALE");
    ScopedEnvVarA env_p("OPENSCARLETT_PLS_DRIVE_P");

    ::SetEnvironmentVariableA("OPENSCARLETT_GAME_CONFIG", cfg_path.string().c_str());
    ::SetEnvironmentVariableA("OPENSCARLETT_SAVE_ROOT", save_root.string().c_str());
    ::SetEnvironmentVariableA("OPENSCARLETT_LOCALE", nullptr);
    ::SetEnvironmentVariableA("OPENSCARLETT_PLS_DRIVE_P", nullptr);

    // Locale falls back to first <Resources><Resource Language="..."/> when env unset
    OS_REQUIRE(ResolvePreferredUserLocale(&cfg) == "fr-FR");
    char loc_buf[64]{};
    OS_REQUIRE(XPackageGetUserLocale(sizeof(loc_buf), loc_buf) == XGR_S_OK);
    OS_REQUIRE(std::strcmp(loc_buf, "fr-FR") == 0);

    // OPENSCARLETT_LOCALE overrides config
    ::SetEnvironmentVariableA("OPENSCARLETT_LOCALE", "de-DE");
    OS_REQUIRE(ResolvePreferredUserLocale(&cfg) == "de-DE");
    OS_REQUIRE(XPackageGetUserLocale(sizeof(loc_buf), loc_buf) == XGR_S_OK);
    OS_REQUIRE(std::strcmp(loc_buf, "de-DE") == 0);

    // PLS quota wired from <PersistentLocalStorage><SizeMB>2048</SizeMB>
    XPersistentLocalStorageSpaceInfo pls_info{};
    OS_REQUIRE(XPersistentLocalStorageGetSpaceInfo(&pls_info) == XGR_S_OK);
    OS_REQUIRE(pls_info.totalBytes == 2048ull * 1024ull * 1024ull);
    OS_REQUIRE(pls_info.totalFreeBytes <= pls_info.totalBytes);
    OS_REQUIRE(pls_info.availableFreeBytes <= pls_info.totalFreeBytes);

    // P:\ PLS drive reporting mode
    ::SetEnvironmentVariableA("OPENSCARLETT_PLS_DRIVE_P", "1");
    size_t pls_sz = 0;
    OS_REQUIRE(XPersistentLocalStorageGetPathSize(&pls_sz) == XGR_S_OK && pls_sz == 4u);
    char pls_path[16]{};
    size_t pls_used = 0;
    OS_REQUIRE(XPersistentLocalStorageGetPath(sizeof(pls_path), pls_path, &pls_used) == XGR_S_OK);
    OS_REQUIRE(std::strcmp(pls_path, "P:\\") == 0);
#endif
    std::printf("  PASS: Full MicrosoftGame.Config, OPENSCARLETT_LOCALE & P:\\ PLS quota verified.\n");
}

// ===========================================================================
// 8. XErrorReport HRESULT S_OK + XGameSaveFiles + PR #24 XSystemHandleTrack
// ===========================================================================
void TestXErrorReportGameSaveFilesAndPr24() {
    std::printf("[8/8] XErrorReport HRESULT S_OK, XGameSaveFiles & PR #24 XSystemHandleTrack...\n");
#if defined(_WIN32)
    HMODULE h_xgr = ::LoadLibraryW((GetExeDir() / "xgameruntime.dll").c_str());
    OS_REQUIRE(h_xgr != nullptr);
    if (!h_xgr) return;

    // 8a. XErrorReport / XErrorSetCallback / XErrorSetOptions return HRESULT S_OK (0)
    using PfnXErrorReportHr = XGameResult(__stdcall*)(XGameResult, const char*);
    using PfnXErrorSetCbHr = XGameResult(__stdcall*)(XErrorCallback, void*);
    using PfnXErrorSetOptHr = XGameResult(__stdcall*)(XErrorOptions, XErrorOptions);

    auto p_err_report = reinterpret_cast<PfnXErrorReportHr>(::GetProcAddress(h_xgr, "XErrorReport"));
    auto p_err_set_cb = reinterpret_cast<PfnXErrorSetCbHr>(::GetProcAddress(h_xgr, "XErrorSetCallback"));
    auto p_err_set_opt = reinterpret_cast<PfnXErrorSetOptHr>(::GetProcAddress(h_xgr, "XErrorSetOptions"));
    OS_REQUIRE(p_err_report != nullptr && p_err_set_cb != nullptr && p_err_set_opt != nullptr);

    std::atomic<int> cb_hits{0};
    auto err_cb = [](XGameResult, const char*, void* ctx) -> bool {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
        return false;
    };
    OS_REQUIRE(p_err_set_opt(XErrorOptions_None, XErrorOptions_None) == XGR_S_OK);
    OS_REQUIRE(p_err_set_cb(+err_cb, &cb_hits) == XGR_S_OK);
    OS_REQUIRE(p_err_report(XGR_E_FAIL, "sprint14_error_report_test") == XGR_S_OK);
    OS_REQUIRE(cb_hits.load() == 1);
    OS_REQUIRE(p_err_set_cb(nullptr, nullptr) == XGR_S_OK);

    // 8b. XGameSaveFilesGetFolderWithUiAsync / ResultSize / Result / RemainingQuota
    const fs::path save_root = MakeTempRoot("gamesavefiles");
    ScopedEnvVarA env_save("OPENSCARLETT_SAVE_ROOT");
    ::SetEnvironmentVariableA("OPENSCARLETT_SAVE_ROOT", save_root.string().c_str());

    using PfnGsfAsync = XGameResult(__stdcall*)(XUserHandle, const char*, XAsyncBlock*);
    using PfnGsfSize = XGameResult(__stdcall*)(XAsyncBlock*, size_t*);
    using PfnGsfResult = XGameResult(__stdcall*)(XAsyncBlock*, size_t, char*);
    using PfnGsfQuota = XGameResult(__stdcall*)(XUserHandle, const char*, int64_t*);

    auto p_gsf_async = reinterpret_cast<PfnGsfAsync>(::GetProcAddress(h_xgr, "XGameSaveFilesGetFolderWithUiAsync"));
    auto p_gsf_size = reinterpret_cast<PfnGsfSize>(::GetProcAddress(h_xgr, "XGameSaveFilesGetFolderWithUiResultSize"));
    auto p_gsf_result = reinterpret_cast<PfnGsfResult>(::GetProcAddress(h_xgr, "XGameSaveFilesGetFolderWithUiResult"));
    auto p_gsf_quota = reinterpret_cast<PfnGsfQuota>(::GetProcAddress(h_xgr, "XGameSaveFilesGetRemainingQuota"));
    OS_REQUIRE(p_gsf_async && p_gsf_size && p_gsf_result && p_gsf_quota);

    const char* kScid = "00000000-0000-0000-0000-00005ca14001";
    XAsyncBlock ab{};
    OS_REQUIRE(p_gsf_async(nullptr, kScid, &ab) == XGR_S_OK);
    OS_REQUIRE(XAsyncGetStatus(&ab, true) == XGR_S_OK);
    size_t folder_sz = 0u;
    OS_REQUIRE(p_gsf_size(&ab, &folder_sz) == XGR_S_OK && folder_sz > 1u);
    std::string folder_buf(folder_sz, '\0');
    OS_REQUIRE(p_gsf_result(&ab, folder_buf.size(), folder_buf.data()) == XGR_S_OK);
    const fs::path gsf_dir(folder_buf.c_str());
    OS_REQUIRE(fs::is_directory(gsf_dir));

    int64_t quota_before = 0;
    OS_REQUIRE(p_gsf_quota(nullptr, kScid, &quota_before) == XGR_S_OK);
    OS_REQUIRE(quota_before == 256LL * 1024LL * 1024LL);

    WriteBytesFile(gsf_dir / "slot0.sav", std::string(4096u, 'S'));
    int64_t quota_after = 0;
    OS_REQUIRE(p_gsf_quota(nullptr, kScid, &quota_after) == XGR_S_OK);
    OS_REQUIRE(quota_after == quota_before - 4096LL);

    // 8c. PR #24 sanity check: XSystemHandleTrack + XSystemIsHandleValid preserved 100%
    std::atomic<int> created_cnt{0};
    std::atomic<int> destroyed_cnt{0};
    struct TrackCounters {
        std::atomic<int>* created;
        std::atomic<int>* destroyed;
    } counters{&created_cnt, &destroyed_cnt};

    auto track_cb = [](void*, XSystemHandleType, XSystemHandleCallbackReason reason, void* ctx) {
        auto* c = static_cast<TrackCounters*>(ctx);
        if (reason == XSystemHandleCallbackReason_Created) {
            c->created->fetch_add(1);
        } else if (reason == XSystemHandleCallbackReason_Destroyed) {
            c->destroyed->fetch_add(1);
        }
    };
    OS_REQUIRE(XSystemHandleTrack(+track_cb, &counters) == XGR_S_OK);

    XTaskQueueHandle q = nullptr;
    OS_REQUIRE(XTaskQueueCreate(XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_Manual, &q) == XGR_S_OK);
    OS_REQUIRE(XSystemIsHandleValid(q));
    XTaskQueueCloseHandle(q);
    OS_REQUIRE(!XSystemIsHandleValid(q));
    OS_REQUIRE(created_cnt.load() >= 1 && destroyed_cnt.load() >= 1);
    OS_REQUIRE(XSystemHandleTrack(nullptr, nullptr) == XGR_S_OK);

    ::FreeLibrary(h_xgr);
    std::printf("  PASS: XErrorReport S_OK, XGameSaveFiles & PR #24 XSystemHandleTrack verified.\n");
#endif
}

}  // namespace

int main() {
    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    TestXAudio2OrdinalPinning();
    TestXMemGetWorkingSetStatistics();
    TestFidelityFxApi2BothDlls();
    TestSmartMiddlewareRouting();
    TestXCurlMimeAndLibHttpClientWebSocket();
    TestPeHardening();
    TestGameConfigPlsAndLocale();
    TestXErrorReportGameSaveFilesAndPr24();

    XGameRuntimeUninitialize();

    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("=== ALL Sprint 14 P0/P1 Shims, FFX 2.0, Routing & Config Tests PASSED ===\n");
    return 0;
}
