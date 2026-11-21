#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#  include <dxgi1_6.h>
#endif

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rt_bvh.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/middleware/bink2_decoder.h"
#include "openscarlett/middleware/ffx_gpu_fsr.h"
#include "openscarlett/shims/secondary_shims_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

fs::path GetExeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        return fs::path(buf).parent_path();
    }
#endif
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path GetScratchDir() {
#if defined(_WIN32)
    char buf[1024]{};
    const DWORD len = GetEnvironmentVariableA("TEMP", buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        return fs::path(buf) / "test_secondary_shims";
    }
#endif
    return fs::path(OPENSCARLETT_BINARY_DIR) / "scratch_secondary_shims";
}

bool ProbeHardwareD3D12Gpu(std::string& out_desc) {
#if defined(_WIN32)
    openscarlett::D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    ID3D12Device* dev = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    openscarlett::D3D12AdapterInfo info{};
    const openscarlett::Status st =
        openscarlett::SelectD3D12AdapterAndCreateDevice(opts, &dev, &adapter, &info);
    if (dev) dev->Release();
    if (adapter) adapter->Release();
    if (st.ok() && !info.is_warp) {
        out_desc = info.description;
        return true;
    }
#endif
    out_desc = "none (WARP-only runner)";
    return false;
}

// ---------------------------------------------------------------------------
// 1. xgameruntime.dll — InitializeApiImpl / QueryApiImpl + OPENSCARLETT_TITLE_ID
// ---------------------------------------------------------------------------
const char kVtableAsyncId[] = "VtableCustomAsync";

struct VtableAsyncCtx {
    uint32_t in_val = 0;
    uint32_t out_val = 0;
    const IXgrAsyncApiVtable* vt = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL VtableAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<VtableAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            ctx->out_val = ctx->in_val * 5u + 11u;
            ctx->vt->XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(uint32_t)) return XGR_E_NOT_SUFFICIENT_BUFFER;
            std::memcpy(data->buffer, &ctx->out_val, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

void OPENSCARLETT_GDK_CALL SimpleQueueCb(void* ctx, bool canceled) {
    if (!canceled) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
}

bool OPENSCARLETT_GDK_CALL PackageChunkCb(
    void* ctx,
    const XPackageChunkSelector* /*sel*/,
    XPackageChunkAvailability avail) {
    if (avail == XPackageChunkAvailability_Ready) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

template <typename VtableT>
const VtableT* QueryVtableRequired(const XgrGuid& guid) {
    void* raw = nullptr;
    OS_REQUIRE(QueryApiImpl(&guid, &raw) == XGR_S_OK);
    OS_REQUIRE(raw != nullptr);
    return static_cast<const VtableT*>(raw);
}

void TestXGameRuntimeQueryApi() {
    std::printf("[1/10] xgameruntime.dll InitializeApiImpl / QueryApiImpl (8 GUIDs) + TitleId...\n");

    OS_REQUIRE(InitializeApiImpl() == XGR_S_OK);

    // Null argument checks
    void* raw = nullptr;
    OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_SYSTEM, nullptr) == XGR_E_POINTER);
    OS_REQUIRE(QueryApiImpl(nullptr, &raw) == XGR_E_INVALIDARG);
    OS_REQUIRE(raw == nullptr);

    // 1a. TaskQueue vtable
    const auto* tq_vt = QueryVtableRequired<IXgrTaskQueueApiVtable>(XGR_API_GUID_TASK_QUEUE);
    OS_REQUIRE(tq_vt->AddRef(tq_vt) >= 2u);
    void* qi_unknown = nullptr;
    OS_REQUIRE(tq_vt->QueryInterface(tq_vt, &XGR_IID_IUNKNOWN, &qi_unknown) == XGR_S_OK);
    OS_REQUIRE(qi_unknown == tq_vt);
    tq_vt->Release(tq_vt);
    tq_vt->Release(tq_vt);

    XTaskQueueHandle queue = nullptr;
    OS_REQUIRE(tq_vt->XTaskQueueCreate(
        XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_Manual, &queue) == XGR_S_OK);
    std::atomic<int> cb_ran{0};
    OS_REQUIRE(tq_vt->XTaskQueueSubmitCallback(
        queue, XTaskQueuePort_Work, &cb_ran, &SimpleQueueCb) == XGR_S_OK);
    OS_REQUIRE(tq_vt->XTaskQueueDispatch(queue, XTaskQueuePort_Work, 100u));
    OS_REQUIRE(cb_ran.load() == 1);
    OS_REQUIRE(tq_vt->XTaskQueueTerminate(queue, true, nullptr, nullptr) == XGR_S_OK);
    tq_vt->XTaskQueueCloseHandle(queue);

    // 1b. Async vtable
    const auto* async_vt = QueryVtableRequired<IXgrAsyncApiVtable>(XGR_API_GUID_ASYNC);
    {
        XTaskQueueHandle aq = nullptr;
        OS_REQUIRE(tq_vt->XTaskQueueCreate(
            XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &aq) == XGR_S_OK);
        XAsyncBlock ab{};
        ab.queue = aq;
        auto* ctx = new VtableAsyncCtx{10u, 0u, async_vt};
        OS_REQUIRE(async_vt->XAsyncBegin(&ab, ctx, kVtableAsyncId, kVtableAsyncId, &VtableAsyncProvider) == XGR_S_OK);
        OS_REQUIRE(async_vt->XAsyncSchedule(&ab, 0u) == XGR_S_OK);
        OS_REQUIRE(async_vt->XAsyncGetStatus(&ab, true) == XGR_S_OK);
        size_t res_sz = 0;
        OS_REQUIRE(async_vt->XAsyncGetResultSize(&ab, &res_sz) == XGR_S_OK);
        OS_REQUIRE(res_sz == sizeof(uint32_t));
        uint32_t out_val = 0;
        size_t used = 0;
        OS_REQUIRE(async_vt->XAsyncGetResult(&ab, kVtableAsyncId, sizeof(out_val), &out_val, &used) == XGR_S_OK);
        OS_REQUIRE(out_val == 61u && used == sizeof(uint32_t));
        tq_vt->XTaskQueueTerminate(aq, true, nullptr, nullptr);
        tq_vt->XTaskQueueCloseHandle(aq);
    }

    // 1c. User vtable
    const auto* user_vt = QueryVtableRequired<IXgrUserApiVtable>(XGR_API_GUID_USER);
    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        OS_REQUIRE(user_vt->XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        OS_REQUIRE(user_vt->XUserAddResult(&ab, &user) == XGR_S_OK);
        OS_REQUIRE(user != nullptr);
        uint64_t xuid = 0;
        OS_REQUIRE(user_vt->XUserGetId(user, &xuid) == XGR_S_OK && xuid != 0ull);
        char gt[64]{};
        size_t gt_used = 0;
        OS_REQUIRE(user_vt->XUserGetGamertag(
            user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) == XGR_S_OK);
        OS_REQUIRE(gt_used > 1u && gt[0] != '\0');
        XUserState state = XUserState_SignedOut;
        OS_REQUIRE(user_vt->XUserGetState(user, &state) == XGR_S_OK && state == XUserState_SignedIn);
        bool has_priv = false;
        XUserPrivilegeDenyReason reason = XUserPrivilegeDenyReason_Unknown;
        OS_REQUIRE(user_vt->XUserCheckPrivilege(
            user, XUserPrivilegeOptions_None, XUserPrivilege_Multiplayer, &has_priv, &reason) == XGR_S_OK);
        OS_REQUIRE(has_priv && reason == XUserPrivilegeDenyReason_None);
    }

    // 1d. GameSave vtable
    const auto* gs_vt = QueryVtableRequired<IXgrGameSaveApiVtable>(XGR_API_GUID_GAME_SAVE);
    {
        XGameSaveProviderHandle prov = nullptr;
        OS_REQUIRE(gs_vt->XGameSaveInitializeProvider(
            user, "00000000-0000-0000-0000-00007E570001", false, &prov) == XGR_S_OK);
        XGameSaveContainerHandle cont = nullptr;
        OS_REQUIRE(gs_vt->XGameSaveCreateContainer(prov, "vtable_slot0", &cont) == XGR_S_OK);
        XGameSaveUpdateHandle upd = nullptr;
        OS_REQUIRE(gs_vt->XGameSaveCreateUpdate(cont, "Slot 0", &upd) == XGR_S_OK);
        const uint8_t payload[] = {0x53, 0x43, 0x41, 0x52, 0x4C, 0x45, 0x54, 0x54};
        OS_REQUIRE(gs_vt->XGameSaveSubmitBlobWrite(upd, "header", payload, sizeof(payload)) == XGR_S_OK);
        OS_REQUIRE(gs_vt->XGameSaveSubmitUpdate(upd) == XGR_S_OK);
        gs_vt->XGameSaveCloseUpdate(upd);

        uint8_t read_buf[512]{};
        const char* names[] = {"header"};
        uint32_t count = 1;
        auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf);
        OS_REQUIRE(gs_vt->XGameSaveReadBlobData(cont, names, &count, sizeof(read_buf), blobs) == XGR_S_OK);
        OS_REQUIRE(count == 1u && blobs[0].info.size == sizeof(payload));
        OS_REQUIRE(std::memcmp(blobs[0].data, payload, sizeof(payload)) == 0);
        gs_vt->XGameSaveCloseContainer(cont);
        OS_REQUIRE(gs_vt->XGameSaveDeleteContainer(prov, "vtable_slot0") == XGR_S_OK);
        gs_vt->XGameSaveCloseProvider(prov);
    }
    user_vt->XUserCloseHandle(user);

    // 1e. System vtable + OPENSCARLETT_TITLE_ID override
    const auto* sys_vt = QueryVtableRequired<IXgrSystemApiVtable>(XGR_API_GUID_SYSTEM);
    OS_REQUIRE(sys_vt->XSystemGetDeviceType() == XSystemDeviceType_XboxScarlettAnaconda);
    char cid[64]{};
    size_t cid_used = 0;
    OS_REQUIRE(sys_vt->XSystemGetConsoleId(sizeof(cid), cid, &cid_used) == XGR_S_OK && cid_used > 1u);
    const XSystemAnalyticsInfo ainfo = sys_vt->XSystemGetAnalyticsInfo();
    OS_REQUIRE(std::strcmp(ainfo.family, "Windows.Xbox") == 0);
    const XSystemRuntimeInfo rinfo = sys_vt->XSystemGetRuntimeInfo();
    OS_REQUIRE(rinfo.runtimeVersion.major == 10u);

#if defined(_WIN32)
    {
        char orig_tid[128]{};
        const DWORD orig_len = GetEnvironmentVariableA(
            "OPENSCARLETT_TITLE_ID", orig_tid, static_cast<DWORD>(sizeof(orig_tid)));
        const bool had_orig = (orig_len > 0 && orig_len < sizeof(orig_tid));

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", nullptr);
        uint32_t tid = 0;
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 0x5CA00001u);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "0x1234ABCD");
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 0x1234ABCDu);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "987654321");
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 987654321u);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", "invalid_tid_xyz");
        OS_REQUIRE(sys_vt->XGameGetXboxTitleId(&tid) == XGR_S_OK && tid == 0x5CA00001u);

        SetEnvironmentVariableA("OPENSCARLETT_TITLE_ID", had_orig ? orig_tid : nullptr);
    }
#endif

    // 1f. Mem vtable
    const auto* mem_vt = QueryVtableRequired<IXgrMemApiVtable>(XGR_API_GUID_MEM);
    void* p = mem_vt->XMemAlloc(256u, 0ull);
    OS_REQUIRE(p != nullptr);
    std::memset(p, 0x5A, 256u);
    mem_vt->XMemFree(p, 0ull);

    // 1g. Package vtable
    const auto* pkg_vt = QueryVtableRequired<IXgrPackageApiVtable>(XGR_API_GUID_PACKAGE);
    OS_REQUIRE(pkg_vt->XPackageIsPackagedProcess());
    char pkg_id[128]{};
    OS_REQUIRE(pkg_vt->XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id) == XGR_S_OK);
    std::atomic<int> ready_chunks{0};
    OS_REQUIRE(pkg_vt->XPackageEnumerateChunkAvailability(
        pkg_id, 0u, nullptr, &ready_chunks, &PackageChunkCb) == XGR_S_OK);
    OS_REQUIRE(ready_chunks.load() == 1);

    // 1h. Display vtable
    const auto* disp_vt = QueryVtableRequired<IXgrDisplayApiVtable>(XGR_API_GUID_DISPLAY);
    XDisplayHdrModeInfo hdr_info{};
    (void)disp_vt->XDisplayTryEnableHdrMode(1u, &hdr_info);
    OS_REQUIRE(hdr_info.maxToneMapLuminance > 0.0f);
    XDisplayTimeoutDeferralHandle deferral = nullptr;
    OS_REQUIRE(disp_vt->XDisplayTryAcquireTimeoutDeferral(&deferral) == XGR_S_OK && deferral != nullptr);
    disp_vt->XDisplayCloseTimeoutDeferralHandle(deferral);

    // Unknown GUID -> E_NOINTERFACE + NotImplemented telemetry with formatted GUID string
    constexpr XgrGuid kUnknownGuid = {
        0xDEADBEEFu, 0x1234u, 0x5678u, {0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44}};
    void* bad_vt = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1));
    OS_REQUIRE(QueryApiImpl(&kUnknownGuid, &bad_vt) == XGR_E_NOINTERFACE);
    OS_REQUIRE(bad_vt == nullptr);

    uint64_t unk_total = 0;
    uint64_t unk_not_impl = 0;
    OS_REQUIRE(XgrGetTelemetryCallCountForTest(
        "QueryApiImpl({DEADBEEF-1234-5678-9ABC-DEF011223344})", &unk_total, &unk_not_impl) == 1);
    OS_REQUIRE(unk_total >= 1ull && unk_not_impl >= 1ull);

    UninitializeApiImpl();
    std::printf("  PASS: 8/8 API GUID vtables dispatched, unknown GUID logged E_NOINTERFACE.\n");
}

// ---------------------------------------------------------------------------
// 2. pixevt.dll
// ---------------------------------------------------------------------------
void TestPixEvt() {
    std::printf("[2/10] pixevt.dll CPU/CommandList/CommandQueue events & counters...\n");
    const uint64_t before = PIXGetEventCountForTest();

    PIXBeginEventOnCpu(0xFF00FF00ull, "FrameUpdate");
    PIXSetMarkerOnCpu(0xFFFF0000ull, "PhysicsTick");
    PIXEndEventOnCpu();

    int dummy_cmdlist = 0;
    PIXBeginEventOnCommandList(&dummy_cmdlist, 0xFF0000FFull, "GBufferPass");
    PIXSetMarkerOnCommandList(&dummy_cmdlist, 0xFF00FFFFull, "DrawMesh");
    PIXEndEventOnCommandList(&dummy_cmdlist);

    int dummy_queue = 0;
    PIXBeginEventOnCommandQueue(&dummy_queue, 0xFFFFFF00ull, "PresentQueue");
    PIXSetMarkerOnCommandQueue(&dummy_queue, 0xFFFFFFFFull, "Flip");
    PIXEndEventOnCommandQueue(&dummy_queue);

    PIXNotifyWakeFromFenceSignal(&dummy_queue);
    PIXReportCounter(L"GpuFrameTimeMs", 4.25f);
    OS_REQUIRE(PIXGpuCaptureNextFrames(L"test_capture.wpix", 1u) == OS_SHIM_S_OK);
    OS_REQUIRE(PIXGpuCaptureNextFrames(L"bad.wpix", 0u) == OS_SHIM_E_INVALIDARG);

    const uint64_t after = PIXGetEventCountForTest();
    OS_REQUIRE(after - before == 12ull);
    std::printf("  PASS: pixevt recorded %llu events cleanly.\n",
                static_cast<unsigned long long>(after - before));
}

// ---------------------------------------------------------------------------
// 3. xmem.dll (forwarded to kernelx.dll)
// ---------------------------------------------------------------------------
void TestXMem() {
    std::printf("[3/10] xmem.dll aligned XMemAlloc/XMemSize/XMemFree -> kernelx.dll...\n");
#if defined(_WIN32)
    const fs::path xmem_path = GetExeDir() / "xmem.dll";
    HMODULE mod = LoadLibraryA(xmem_path.string().c_str());
    OS_REQUIRE(mod != nullptr);

    auto fn_alloc = reinterpret_cast<PFN_XMemAlloc>(GetProcAddress(mod, "XMemAlloc"));
    auto fn_alloc_def = reinterpret_cast<PFN_XMemAllocDefault>(GetProcAddress(mod, "XMemAllocDefault"));
    auto fn_free = reinterpret_cast<PFN_XMemFree>(GetProcAddress(mod, "XMemFree"));
    auto fn_free_def = reinterpret_cast<PFN_XMemFreeDefault>(GetProcAddress(mod, "XMemFreeDefault"));
    auto fn_size = reinterpret_cast<PFN_XMemSize>(GetProcAddress(mod, "XMemSize"));
    auto fn_size_def = reinterpret_cast<PFN_XMemSizeDefault>(GetProcAddress(mod, "XMemSizeDefault"));
    auto fn_get_aux = reinterpret_cast<PFN_XMemGetAuxiliaryTitleMemory>(
        GetProcAddress(mod, "XMemGetAuxiliaryTitleMemory"));
    auto fn_rel_aux = reinterpret_cast<PFN_XMemReleaseAuxiliaryTitleMemory>(
        GetProcAddress(mod, "XMemReleaseAuxiliaryTitleMemory"));
    auto fn_map = reinterpret_cast<PFN_XMemMapPhysicalPages>(
        GetProcAddress(mod, "XMemMapPhysicalPages"));
    auto fn_unmap = reinterpret_cast<PFN_XMemUnmapPhysicalPages>(
        GetProcAddress(mod, "XMemUnmapPhysicalPages"));
    auto fn_tel = reinterpret_cast<PFN_XMemGetTelemetryCallCountForTest>(
        GetProcAddress(mod, "XMemGetTelemetryCallCountForTest"));

    OS_REQUIRE(fn_alloc && fn_alloc_def && fn_free && fn_free_def);
    OS_REQUIRE(fn_size && fn_size_def && fn_get_aux && fn_rel_aux);
    OS_REQUIRE(fn_map && fn_unmap && fn_tel);

    // 4 KiB alignment (code 0xB in bits 24..27) + zero-init (bit 34)
    const uint64_t attrs_4k_zero = (0xBull << 24) | (1ull << 34);
    constexpr size_t kAllocSize = 8192u;
    void* ptr = fn_alloc(kAllocSize, attrs_4k_zero);
    OS_REQUIRE(ptr != nullptr);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(ptr) & 4095u) == 0u);

    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < kAllocSize; ++i) {
        OS_REQUIRE(bytes[i] == 0u);
    }
    const size_t reported = fn_size(ptr, attrs_4k_zero);
    OS_REQUIRE(reported >= kAllocSize);
    std::memset(bytes, 0xA5, kAllocSize);
    fn_free(ptr, attrs_4k_zero);

    void* def_ptr = fn_alloc_def(512u, 0ull);
    OS_REQUIRE(def_ptr != nullptr);
    OS_REQUIRE(fn_size_def(def_ptr, 0ull) >= 512u);
    fn_free_def(def_ptr, 0ull);

    size_t aux_actual = 0;
    void* aux = fn_get_aux(65536u, &aux_actual);
    OS_REQUIRE(aux != nullptr && aux_actual >= 65536u);
    OS_REQUIRE(fn_rel_aux(aux) != 0);

    uint64_t tot = 0, ok = 0;
    OS_REQUIRE(fn_tel("XMemAlloc", &tot, &ok) == 1 && tot >= 1ull && ok >= 1ull);
    OS_REQUIRE(fn_tel("XMemSize", &tot, &ok) == 1 && tot >= 1ull && ok >= 1ull);
    OS_REQUIRE(fn_tel("XMemFree", &tot, &ok) == 1 && tot >= 1ull && ok >= 1ull);

    FreeLibrary(mod);
    std::printf("  PASS: xmem.dll 4KiB zero-init alloc (%zu B) + aux mem forwarded to kernelx.dll.\n",
                reported);
#endif
}

// ---------------------------------------------------------------------------
// 4. xcrdapi.dll
// ---------------------------------------------------------------------------
void TestXCrdApi() {
    std::printf("[4/10] xcrdapi.dll diagnostics, breadcrumbs, error report & JSON flush...\n");
    OS_REQUIRE(XCrdInitialize(0x1u) == OS_SHIM_S_OK);
    OS_REQUIRE(XCrdSetBreadcrumb(0u, "InitStage", 0x1001ull) == OS_SHIM_S_OK);
    OS_REQUIRE(XCrdSetBreadcrumb(7u, "ShaderCompile", 42ull) == OS_SHIM_S_OK);

    char tag_small[4]{};
    uint64_t val = 0;
    OS_REQUIRE(XCrdGetBreadcrumb(7u, tag_small, sizeof(tag_small), &val) == OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
    char tag_buf[64]{};
    OS_REQUIRE(XCrdGetBreadcrumb(7u, tag_buf, sizeof(tag_buf), &val) == OS_SHIM_S_OK);
    OS_REQUIRE(std::strcmp(tag_buf, "ShaderCompile") == 0 && val == 42ull);
    OS_REQUIRE(XCrdGetBreadcrumb(99u, tag_buf, sizeof(tag_buf), &val) == OS_SHIM_E_NOT_FOUND);

    OS_REQUIRE(XCrdReportError(0xC0001234u, "Renderer", "Synthetic diagnostic warning") == OS_SHIM_S_OK);

    XCrdDiagnosticInfo info{};
    info.structSize = sizeof(info);
    OS_REQUIRE(XCrdGetDiagnosticInfo(&info) == OS_SHIM_S_OK);
    OS_REQUIRE(info.initialized == 1u);
    OS_REQUIRE(info.breadcrumbCount >= 2u);
    OS_REQUIRE(info.errorReportCount >= 1u);
    OS_REQUIRE(info.lastErrorCode == 0xC0001234u);
    OS_REQUIRE(std::strcmp(info.lastErrorComponent, "Renderer") == 0);

    const fs::path diag_path = GetScratchDir() / "xcrd_diag.json";
    OS_REQUIRE(XCrdFlushDiagnostics(diag_path.string().c_str()) == OS_SHIM_S_OK);
    std::ifstream ifs(diag_path);
    OS_REQUIRE(ifs.good());
    std::string json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    OS_REQUIRE(json.find("ShaderCompile") != std::string::npos);
    OS_REQUIRE(json.find("Synthetic diagnostic warning") != std::string::npos);

    XCrdUninitialize();
    std::printf("  PASS: xcrdapi breadcrumbs=%u errors=%u flushed JSON (%zu B).\n",
                info.breadcrumbCount, info.errorReportCount, json.size());
}

// ---------------------------------------------------------------------------
// 5. libHttpClient.GDK.dll
// ---------------------------------------------------------------------------
void TestLibHttpClient() {
    std::printf("[5/10] libHttpClient.GDK.dll async HTTP (XAsync + offline loopback & mock)...\n");
    HCInitArgs init_args{};
    OS_REQUIRE(HCInitialize(&init_args) == OS_SHIM_S_OK);

    const char* ver = nullptr;
    OS_REQUIRE(HCGetLibVersion(&ver) == OS_SHIM_S_OK && ver && std::strlen(ver) > 0u);
    HCTraceSetTraceToDebugger(false);
    OS_REQUIRE(HCSettingsSetTraceLevel(HCTraceLevel_Warning) == OS_SHIM_S_OK);
    HCClearMockResponsesForTest();

    // 5a. Default offline loopback GET
    {
        HCCallHandle call = nullptr;
        OS_REQUIRE(HCHttpCallCreate(&call) == OS_SHIM_S_OK && call != nullptr);
        HCCallHandle dup = HCHttpCallDuplicateHandle(call);
        OS_REQUIRE(dup == call);

        OS_REQUIRE(HCHttpCallRequestSetUrl(
            call, "GET", "https://api.openscarlett.local/v1/status") == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallRequestSetHeader(call, "X-Title", "OpenScarlett", true) == OS_SHIM_S_OK);

        XAsyncBlock ab{};
        OS_REQUIRE(HCHttpCallPerformAsync(call, &ab) == OS_SHIM_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XGR_S_OK);

        uint32_t status = 0;
        OS_REQUIRE(HCHttpCallResponseGetStatusCode(call, &status) == OS_SHIM_S_OK && status == 200u);

        const char* resp_str = nullptr;
        OS_REQUIRE(HCHttpCallResponseGetResponseString(call, &resp_str) == OS_SHIM_S_OK && resp_str);
        OS_REQUIRE(std::string(resp_str).find("api.openscarlett.local/v1/status") != std::string::npos);

        const char* ct = nullptr;
        OS_REQUIRE(HCHttpCallResponseGetHeader(call, "content-type", &ct) == OS_SHIM_S_OK);
        OS_REQUIRE(ct && std::strcmp(ct, "application/json") == 0);

        OS_REQUIRE(HCHttpCallCloseHandle(dup) == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallCloseHandle(call) == OS_SHIM_S_OK);
    }

    // 5b. Registered mock POST response
    {
        const std::string mock_body = R"({"accepted":true,"token":"OS-HTTP-200"})";
        OS_REQUIRE(HCAddMockResponseForTest(
            "POST",
            "https://api.openscarlett.local/v1/session",
            200u,
            "application/json",
            reinterpret_cast<const uint8_t*>(mock_body.data()),
            mock_body.size()) == OS_SHIM_S_OK);

        HCCallHandle call = nullptr;
        OS_REQUIRE(HCHttpCallCreate(&call) == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallRequestSetUrl(
            call, "POST", "https://api.openscarlett.local/v1/session") == OS_SHIM_S_OK);
        OS_REQUIRE(HCHttpCallRequestSetRequestBodyString(call, R"({"user":"tester"})") == OS_SHIM_S_OK);

        XAsyncBlock ab{};
        OS_REQUIRE(HCHttpCallPerformAsync(call, &ab) == OS_SHIM_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XGR_S_OK);

        uint32_t status = 0;
        OS_REQUIRE(HCHttpCallResponseGetStatusCode(call, &status) == OS_SHIM_S_OK && status == 200u);
        size_t body_sz = 0;
        OS_REQUIRE(HCHttpCallResponseGetResponseBodyBytesSize(call, &body_sz) == OS_SHIM_S_OK);
        OS_REQUIRE(body_sz == mock_body.size());
        std::vector<uint8_t> buf(body_sz);
        size_t used = 0;
        OS_REQUIRE(HCHttpCallResponseGetResponseBodyBytes(call, buf.size(), buf.data(), &used) == OS_SHIM_S_OK);
        OS_REQUIRE(used == mock_body.size());
        OS_REQUIRE(std::memcmp(buf.data(), mock_body.data(), mock_body.size()) == 0);
        OS_REQUIRE(HCHttpCallCloseHandle(call) == OS_SHIM_S_OK);
    }

    // Async cleanup via XAsync
    {
        XAsyncBlock cleanup_ab{};
        OS_REQUIRE(HCCleanupAsync(&cleanup_ab) == OS_SHIM_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&cleanup_ab, true) == XGR_S_OK);
    }
    std::printf("  PASS: libHttpClient.GDK async loopback + mock HTTP 200 verified via XAsync.\n");
}

// ---------------------------------------------------------------------------
// 6. XCurl.dll
// ---------------------------------------------------------------------------
size_t OS_SHIM_CDECL CurlCaptureWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, total);
    return total;
}

void TestXCurl() {
    std::printf("[6/10] XCurl.dll easy + multi offline transfer & callback verification...\n");
    OS_REQUIRE(curl_global_init(XCURL_GLOBAL_DEFAULT) == XCURLE_OK);
    OS_REQUIRE(curl_version() && std::strstr(curl_version(), "XCurl") != nullptr);
    OS_REQUIRE(std::strcmp(curl_easy_strerror(XCURLE_OK), "No error") == 0);

    // 6a. Easy transfer with write + header callbacks
    XCurlEasyHandle easy = curl_easy_init();
    OS_REQUIRE(easy != nullptr);
    XCurlSlist* hdrs = curl_slist_append(nullptr, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, "X-Client: OpenScarlett");
    OS_REQUIRE(hdrs != nullptr);

    std::string body;
    std::string resp_headers;
    int sentinel = 1234;
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_URL, "https://title.openscarlett.local/config") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_HTTPHEADER, hdrs) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEDATA, &body) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_HEADERFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_HEADERDATA, &resp_headers) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_PRIVATE, &sentinel) == XCURLE_OK);

    OS_REQUIRE(curl_easy_perform(easy) == XCURLE_OK);

    long http_code = 0;
    double dl_bytes = 0.0;
    const char* eff_url = nullptr;
    const char* ctype = nullptr;
    void* priv_out = nullptr;
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_RESPONSE_CODE, &http_code) == XCURLE_OK && http_code == 200L);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_SIZE_DOWNLOAD, &dl_bytes) == XCURLE_OK && dl_bytes > 0.0);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_EFFECTIVE_URL, &eff_url) == XCURLE_OK);
    OS_REQUIRE(eff_url && std::strcmp(eff_url, "https://title.openscarlett.local/config") == 0);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_CONTENT_TYPE, &ctype) == XCURLE_OK);
    OS_REQUIRE(ctype && std::strcmp(ctype, "application/json") == 0);
    OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_PRIVATE, &priv_out) == XCURLE_OK && priv_out == &sentinel);
    OS_REQUIRE(body.find("openscarlett-xcurl") != std::string::npos);
    OS_REQUIRE(resp_headers.find("HTTP/1.1 200 OK") != std::string::npos);

    // 6b. Multi transfer cycle with 2 easy handles
    curl_easy_reset(easy);
    XCurlEasyHandle easy2 = curl_easy_init();
    OS_REQUIRE(easy2 != nullptr);

    std::string body1, body2;
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_URL, "https://title.openscarlett.local/m1") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEDATA, &body1) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_URL, "https://title.openscarlett.local/m2") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_POSTFIELDS, "hello=world") == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_WRITEFUNCTION, &CurlCaptureWriteCb) == XCURLE_OK);
    OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_WRITEDATA, &body2) == XCURLE_OK);

    XCurlMultiHandle multi = curl_multi_init();
    OS_REQUIRE(multi != nullptr);
    OS_REQUIRE(curl_multi_add_handle(multi, easy) == XCURLM_OK);
    OS_REQUIRE(curl_multi_add_handle(multi, easy2) == XCURLM_OK);

    int32_t running = -1;
    OS_REQUIRE(curl_multi_perform(multi, &running) == XCURLM_OK);
    OS_REQUIRE(running == 0);

    int done_count = 0;
    int32_t in_q = 0;
    while (XCurlMsg* msg = curl_multi_info_read(multi, &in_q)) {
        OS_REQUIRE(msg->msg == XCURLMSG_DONE);
        OS_REQUIRE(msg->data.result == XCURLE_OK);
        ++done_count;
    }
    OS_REQUIRE(done_count == 2);
    OS_REQUIRE(body1.find("/m1") != std::string::npos);
    OS_REQUIRE(body2.find("/m2") != std::string::npos);
    OS_REQUIRE(body2.find("\"postBytes\":11") != std::string::npos);

    OS_REQUIRE(curl_multi_remove_handle(multi, easy) == XCURLM_OK);
    OS_REQUIRE(curl_multi_remove_handle(multi, easy2) == XCURLM_OK);
    OS_REQUIRE(curl_multi_cleanup(multi) == XCURLM_OK);
    curl_easy_cleanup(easy2);
    curl_easy_cleanup(easy);
    curl_slist_free_all(hdrs);
    curl_global_cleanup();

    std::printf("  PASS: XCurl easy HTTP 200 (%zu B) + multi (%d completed).\n", body.size(), done_count);
}

// ---------------------------------------------------------------------------
// 7. Bink2Scarlett.dll (backed by openscarlett::Bink2Decoder)
// ---------------------------------------------------------------------------
std::vector<uint8_t> MakeSolidRgbaFrame(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4u);
    for (uint32_t i = 0; i < w * h; ++i) {
        pixels[i * 4u + 0u] = r;
        pixels[i * 4u + 1u] = g;
        pixels[i * 4u + 2u] = b;
        pixels[i * 4u + 3u] = 255u;
    }
    return pixels;
}

double MeanAbsRgbDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    OS_REQUIRE(a.size() == b.size() && (a.size() % 4u) == 0u);
    double sum = 0.0;
    const size_t px = a.size() / 4u;
    for (size_t i = 0; i < px; ++i) {
        sum += std::abs(static_cast<int>(a[i * 4u + 0u]) - static_cast<int>(b[i * 4u + 0u]));
        sum += std::abs(static_cast<int>(a[i * 4u + 1u]) - static_cast<int>(b[i * 4u + 1u]));
        sum += std::abs(static_cast<int>(a[i * 4u + 2u]) - static_cast<int>(b[i * 4u + 2u]));
    }
    return sum / static_cast<double>(px * 3u);
}

void TestBink2Scarlett() {
    std::printf("[7/10] Bink2Scarlett.dll KB2i decode, RGBA/BGRA copy & BinkGoto seek...\n");
    constexpr uint32_t kW = 32u;
    constexpr uint32_t kH = 32u;

    std::vector<std::vector<uint8_t>> src_frames = {
        MakeSolidRgbaFrame(kW, kH, 220u, 40u, 50u),
        MakeSolidRgbaFrame(kW, kH, 40u, 210u, 60u),
        MakeSolidRgbaFrame(kW, kH, 50u, 70u, 230u),
        MakeSolidRgbaFrame(kW, kH, 235u, 220u, 45u),
    };

    openscarlett::Bink2EncodeParams enc{};
    enc.width = kW;
    enc.height = kH;
    enc.fps_num = 60u;
    enc.fps_den = 1u;
    enc.fourcc = "KB2i";
    enc.chroma_format = openscarlett::Bink2ChromaFormat::YCbCr420;
    enc.keyframe_interval = 2u;

    std::vector<uint8_t> container;
    OS_REQUIRE(openscarlett::Bink2EncodeTestStream(src_frames, enc, container).ok());
    OS_REQUIRE(!container.empty());

    HBink bink = BinkOpenBuffer(container.data(), container.size(), 0u);
    OS_REQUIRE(bink != nullptr);
    OS_REQUIRE(bink->Width == kW && bink->Height == kH && bink->Frames == 4u && bink->FrameNum == 1u);
    OS_REQUIRE(std::strlen(BinkGetError()) == 0u);

    BinkSummary sum{};
    BinkGetSummary(bink, &sum);
    OS_REQUIRE(sum.IsBink2 == 1u && std::strcmp(sum.FourCC, "KB2i") == 0);
    OS_REQUIRE(sum.FrameRate == 60u && sum.FrameRateDiv == 1u);

    BinkSetSoundOnOff(bink, 0);
    OS_REQUIRE(bink->SoundOn == 0u);
    OS_REQUIRE(BinkPause(bink, 1) == 1 && BinkWait(bink) == 1);
    OS_REQUIRE(BinkPause(bink, 0) == 0 && BinkWait(bink) == 0);

    std::vector<uint8_t> decoded(static_cast<size_t>(kW) * kH * 4u);
    double worst_mae = 0.0;
    for (uint32_t f = 0; f < 4u; ++f) {
        OS_REQUIRE(bink->FrameNum == f + 1u);
        OS_REQUIRE(BinkDoFrame(bink) == 0);
        OS_REQUIRE(BinkCopyToBuffer(
            bink, decoded.data(), static_cast<int32_t>(kW * 4u), kH, 0u, 0u, OS_BINKSURFACE32RGBA) == 0);
        const double mae = MeanAbsRgbDiff(decoded, src_frames[f]);
        if (mae > worst_mae) worst_mae = mae;
        OS_REQUIRE(mae < 8.0);
        BinkNextFrame(bink);
    }

    // Verify BinkGoto seek to frame 3 + BGRA channel swap check
    BinkGoto(bink, 3u, 0u);
    OS_REQUIRE(bink->FrameNum == 3u);
    OS_REQUIRE(BinkCopyToBuffer(
        bink, decoded.data(), static_cast<int32_t>(kW * 4u), kH, 0u, 0u, OS_BINKSURFACE32RGBA) == 0);
    OS_REQUIRE(MeanAbsRgbDiff(decoded, src_frames[2]) < 8.0);

    std::vector<uint8_t> decoded_bgra(static_cast<size_t>(kW) * kH * 4u);
    OS_REQUIRE(BinkCopyToBuffer(
        bink, decoded_bgra.data(), static_cast<int32_t>(kW * 4u), kH, 0u, 0u, OS_BINKSURFACE32) == 0);
    OS_REQUIRE(decoded_bgra[0] == decoded[2] && decoded_bgra[2] == decoded[0]);

    BinkClose(bink);
    std::printf("  PASS: Bink2Scarlett decoded 4 KB2i frames + seek (worst MAE=%.2f LSB).\n", worst_mae);
}

// ---------------------------------------------------------------------------
// 8. amd_fidelityfx_loader_xbox.dll + amd_fidelityfx_upscaler_xbox.dll
// ---------------------------------------------------------------------------
double ComputeMeanEdgeGradient(const uint8_t* rgba8, uint32_t w, uint32_t h) {
    double sum = 0.0;
    uint64_t count = 0;
    for (uint32_t y = 1u; y + 1u < h; ++y) {
        for (uint32_t x = 1u; x + 1u < w; ++x) {
            const size_t c = (static_cast<size_t>(y) * w + x) * 4u;
            const size_t r = (static_cast<size_t>(y) * w + (x + 1u)) * 4u;
            const size_t b = (static_cast<size_t>(y + 1u) * w + x) * 4u;
            const int gx = std::abs(static_cast<int>(rgba8[r]) - static_cast<int>(rgba8[c]));
            const int gy = std::abs(static_cast<int>(rgba8[b]) - static_cast<int>(rgba8[c]));
            sum += static_cast<double>(gx + gy);
            ++count;
        }
    }
    return count > 0 ? (sum / static_cast<double>(count)) : 0.0;
}

void RunFfxLoaderSession(
    bool prefer_warp,
    const std::vector<uint8_t>& input,
    uint32_t in_w,
    uint32_t in_h,
    uint32_t out_w,
    uint32_t out_h,
    std::vector<uint8_t>& out_pixels,
    FfxUpscalerDispatchResult& out_res) {
    FfxCreateContextDescUpscale create_desc{};
    create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create_desc.flags = prefer_warp
                            ? (FFX_UPSCALER_FLAG_PREFER_WARP | FFX_UPSCALER_FLAG_ENABLE_RCAS)
                            : FFX_UPSCALER_FLAG_ENABLE_RCAS;
    create_desc.maxRenderWidth = in_w;
    create_desc.maxRenderHeight = in_h;
    create_desc.maxUpscaleWidth = out_w;
    create_desc.maxUpscaleHeight = out_h;
    create_desc.defaultSharpness = 0.8f;

    FfxContext ctx = nullptr;
    OS_REQUIRE(ffxCreateContext(&ctx, &create_desc.header, nullptr) == FFX_API_RETURN_OK);
    OS_REQUIRE(ctx != nullptr);

    // Configure sharpness via FidelityFX SDK 2.0 key-value descriptor
    float sharpness = 0.85f;
    FfxConfigureDescUpscaleKeyValue cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
    cfg.key = FFX_UPSCALE_CONFIGURE_KEY_SHARPNESS;
    cfg.ptr = &sharpness;
    OS_REQUIRE(ffxConfigure(&ctx, &cfg.header) == FFX_API_RETURN_OK);

    // Query adapter info
    FfxQueryDescUpscaleGetAdapterInfo adapter_q{};
    adapter_q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETADAPTERINFO;
    OS_REQUIRE(ffxQuery(&ctx, &adapter_q.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(adapter_q.adapterDescription[0] != '\0');
    if (prefer_warp) {
        OS_REQUIRE(adapter_q.outIsWarp == 1u);
    }

    out_pixels.assign(static_cast<size_t>(out_w) * out_h * 4u, 0u);
    std::memset(&out_res, 0, sizeof(out_res));

    FfxDispatchDescUpscale disp{};
    disp.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    disp.inputRgba8 = input.data();
    disp.inputWidth = in_w;
    disp.inputHeight = in_h;
    disp.outputRgba8 = out_pixels.data();
    disp.outputBufferBytes = out_pixels.size();
    disp.outputWidth = out_w;
    disp.outputHeight = out_h;
    disp.sharpness = -1.0f;  // use configured sharpness
    disp.enableRcas = 0xFFFFFFFFu;
    disp.outResult = &out_res;

    OS_REQUIRE(ffxDispatch(&ctx, &disp.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(out_res.usedGpuCompute == 1u);
    OS_REQUIRE(out_res.outputFnv1aHash != 0ull);
    OS_REQUIRE(out_res.easuGroupsX > 0u && out_res.easuGroupsY > 0u);
    OS_REQUIRE(out_res.rcasGroupsX > 0u && out_res.rcasGroupsY > 0u);

    OS_REQUIRE(ffxDestroyContext(&ctx, nullptr) == FFX_API_RETURN_OK);
    OS_REQUIRE(ctx == nullptr);
}

void TestFidelityFxShims(bool has_hw_gpu) {
    std::printf("[8/10] amd_fidelityfx_loader_xbox.dll + upscaler_xbox.dll live GPU FSR1...\n");

    // Provider version & render resolution queries (context-free)
    uint64_t ver_count = 1ull;
    uint64_t ver_ids[1]{};
    const char* ver_names[1]{};
    FfxQueryDescGetVersions vq{};
    vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    vq.outputCount = &ver_count;
    vq.versionIds = ver_ids;
    vq.versionNames = ver_names;
    OS_REQUIRE(ffxQuery(nullptr, &vq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(ver_count == 1ull && ver_ids[0] == ffxUpscalerGetVersion() && ver_names[0] != nullptr);

    uint32_t ren_w = 0, ren_h = 0;
    FfxQueryDescUpscaleGetRenderResolutionFromQualityMode rq{};
    rq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    rq.displayWidth = 1920u;
    rq.displayHeight = 1080u;
    rq.qualityMode = FFX_UPSCALER_QUALITY_QUALITY;
    rq.pOutRenderWidth = &ren_w;
    rq.pOutRenderHeight = &ren_h;
    OS_REQUIRE(ffxQuery(nullptr, &rq.header) == FFX_API_RETURN_OK);
    OS_REQUIRE(ren_w == 1280u && ren_h == 720u);

    constexpr uint32_t kInW = 64u, kInH = 64u, kOutW = 128u, kOutH = 128u;
    std::vector<uint8_t> input(static_cast<size_t>(kInW) * kInH * 4u);
    for (uint32_t y = 0; y < kInH; ++y) {
        for (uint32_t x = 0; x < kInW; ++x) {
            uint8_t* p = &input[(static_cast<size_t>(y) * kInW + x) * 4u];
            const bool checker = (((x / 8u) + (y / 8u)) & 1u) != 0u;
            const bool diag = (x + y) >= kInW;
            p[0] = diag ? (checker ? 240u : 20u) : (checker ? 230u : 15u);
            p[1] = diag ? (checker ? 240u : 30u) : (checker ? 60u : 220u);
            p[2] = diag ? (checker ? 240u : 40u) : (checker ? 30u : 230u);
            p[3] = 255u;
        }
    }

    std::vector<uint8_t> warp_out;
    FfxUpscalerDispatchResult warp_res{};
    RunFfxLoaderSession(true, input, kInW, kInH, kOutW, kOutH, warp_out, warp_res);
    OS_REQUIRE(warp_res.isWarp == 1u);

    openscarlett::FrameBufferRgba8 bil_fb{};
    OS_REQUIRE(openscarlett::FfxGpuFsrPipeline::UpscaleBilinearCpu(
        input.data(), kInW, kInH, kOutW, kOutH, bil_fb).ok());
    const double bil_grad = ComputeMeanEdgeGradient(bil_fb.pixels.data(), kOutW, kOutH);
    const double warp_grad = ComputeMeanEdgeGradient(warp_out.data(), kOutW, kOutH);
    OS_REQUIRE(warp_grad > bil_grad);

    if (has_hw_gpu) {
        std::vector<uint8_t> hw_out;
        FfxUpscalerDispatchResult hw_res{};
        RunFfxLoaderSession(false, input, kInW, kInH, kOutW, kOutH, hw_out, hw_res);
        OS_REQUIRE(hw_res.isWarp == 0u);
        int max_diff = 0;
        for (size_t i = 0; i < hw_out.size(); ++i) {
            const int d = std::abs(static_cast<int>(hw_out[i]) - static_cast<int>(warp_out[i]));
            if (d > max_diff) max_diff = d;
        }
        OS_REQUIRE(max_diff <= 2);
        std::printf("  PASS: FFX HW='%s' FNV1a=0x%016llX | WARP='%s' FNV1a=0x%016llX | EdgeGrad=%.2f>%.2f | MaxDiff=%d LSB.\n",
                    hw_res.adapterDescription,
                    static_cast<unsigned long long>(hw_res.outputFnv1aHash),
                    warp_res.adapterDescription,
                    static_cast<unsigned long long>(warp_res.outputFnv1aHash),
                    warp_grad, bil_grad, max_diff);
    } else {
        std::printf("  PASS: FFX WARP='%s' FNV1a=0x%016llX | EdgeGrad=%.2f>%.2f (WARP-only CI).\n",
                    warp_res.adapterDescription,
                    static_cast<unsigned long long>(warp_res.outputFnv1aHash),
                    warp_grad, bil_grad);
    }
}

// ---------------------------------------------------------------------------
// 9. XboxBvhBuilder.dll (backed by DxrHardwareEngine + BvhAccelerationStructure)
// ---------------------------------------------------------------------------
std::vector<XboxBvhTriangle> ConvertRtTriangles(const std::vector<openscarlett::RtTriangle>& src) {
    std::vector<XboxBvhTriangle> out;
    out.reserve(src.size());
    for (const auto& t : src) {
        XboxBvhTriangle b{};
        b.v0[0] = t.v0.x; b.v0[1] = t.v0.y; b.v0[2] = t.v0.z;
        b.v1[0] = t.v1.x; b.v1[1] = t.v1.y; b.v1[2] = t.v1.z;
        b.v2[0] = t.v2.x; b.v2[1] = t.v2.y; b.v2[2] = t.v2.z;
        b.color[0] = t.color.x; b.color[1] = t.color.y; b.color[2] = t.color.z;
        b.reflectivity = t.reflectivity;
        out.push_back(b);
    }
    return out;
}

struct BvhSessionResult {
    XboxBvhCapabilities caps{};
    XboxBvhDispatchStats initial_stats{};
    XboxBvhDispatchStats updated_stats{};
    std::vector<uint8_t> initial_pixels;
};

BvhSessionResult RunXboxBvhSession(bool prefer_warp) {
    BvhSessionResult res{};

    XboxBvhCreateDesc cdesc{};
    cdesc.preferWarp = prefer_warp ? 1u : 0u;
    XboxBvhBuilderHandle builder = nullptr;
    OS_REQUIRE(XboxBvhCreateBuilder(&cdesc, &builder) == OS_SHIM_S_OK && builder != nullptr);

    OS_REQUIRE(XboxBvhGetCapabilities(builder, &res.caps) == OS_SHIM_S_OK);
    OS_REQUIRE(res.caps.adapterDescription[0] != '\0');
    if (prefer_warp) {
        OS_REQUIRE(res.caps.isWarp == 1u);
    }

    const auto room_src = openscarlett::BuildDefaultRtTestScene();
    const auto room_tris = ConvertRtTriangles(room_src);
    auto cube_src = openscarlett::BuildUnitCubeMesh({0.15f, 0.78f, 0.95f}, 0.35f);
    auto cube_tris = ConvertRtTriangles(cube_src);

    XboxBvhPrebuildInputs pre_in{};
    pre_in.asType = XBOX_BVH_AS_TYPE_BOTTOM_LEVEL;
    pre_in.buildFlags = XBOX_BVH_BUILD_ALLOW_UPDATE | XBOX_BVH_BUILD_PREFER_FAST_TRACE;
    pre_in.primitiveCount = static_cast<uint32_t>(room_tris.size());
    pre_in.geometryCount = 1u;
    pre_in.vertexStride = 12u;
    pre_in.isOpaque = 1u;
    XboxBvhPrebuildInfo pre_info{};
    OS_REQUIRE(XboxBvhGetPrebuildInfo(builder, &pre_in, &pre_info) == OS_SHIM_S_OK);
    OS_REQUIRE(pre_info.resultDataMaxSizeBytes > 0ull && (pre_info.resultDataMaxSizeBytes % 256ull) == 0ull);
    OS_REQUIRE(pre_info.scratchDataSizeBytes > 0ull && (pre_info.scratchDataSizeBytes % 256ull) == 0ull);

    uint64_t room_blas = 0, cube_blas = 0;
    OS_REQUIRE(XboxBvhBuildBlas(
        builder, room_tris.data(), static_cast<uint32_t>(room_tris.size()),
        XBOX_BVH_BUILD_PREFER_FAST_TRACE, &room_blas) == OS_SHIM_S_OK && room_blas != 0ull);
    OS_REQUIRE(XboxBvhBuildBlas(
        builder, cube_tris.data(), static_cast<uint32_t>(cube_tris.size()),
        XBOX_BVH_BUILD_ALLOW_UPDATE | XBOX_BVH_BUILD_PREFER_FAST_TRACE, &cube_blas) == OS_SHIM_S_OK &&
        cube_blas != 0ull);

    XboxBvhInstanceDesc insts[2]{};
    insts[0].blasId = room_blas;
    openscarlett::MakeIdentity3x4(insts[0].transform);
    insts[0].instanceId = 0u;
    insts[0].instanceMask = 0xFFu;

    insts[1].blasId = cube_blas;
    openscarlett::MakeScaleTranslation3x4(0.7f, 0.7f, 0.7f, -0.9f, 0.5f, 0.3f, insts[1].transform);
    insts[1].instanceId = 1u;
    insts[1].instanceMask = 0xFFu;

    uint64_t tlas_id = 0;
    OS_REQUIRE(XboxBvhBuildTlas(
        builder, insts, 2u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas_id) == OS_SHIM_S_OK && tlas_id != 0ull);

    // Single-ray trace directly on TLAS
    XboxBvhRay ray{};
    ray.origin[0] = 0.0f; ray.origin[1] = 1.0f; ray.origin[2] = -3.5f;
    ray.direction[0] = 0.0f; ray.direction[1] = -0.1f; ray.direction[2] = 0.994987f;
    ray.tMin = 0.001f;
    ray.tMax = 100.0f;
    XboxBvhHit hit{};
    OS_REQUIRE(XboxBvhTraceRay(builder, tlas_id, &ray, &hit) == OS_SHIM_S_OK);
    OS_REQUIRE(hit.hit == 1u && hit.t > 0.0f);

    // Full raytraced frame dispatch
    constexpr uint32_t kW = 80u, kH = 60u;
    XboxBvhDispatchDesc ddesc{};
    ddesc.width = kW;
    ddesc.height = kH;
    ddesc.maxBounces = 2u;
    ddesc.cameraPos[0] = 0.0f; ddesc.cameraPos[1] = 1.0f; ddesc.cameraPos[2] = -3.5f;
    ddesc.cameraLookAt[0] = 0.0f; ddesc.cameraLookAt[1] = 0.5f; ddesc.cameraLookAt[2] = 0.0f;
    ddesc.lightPos[0] = 1.5f; ddesc.lightPos[1] = 3.0f; ddesc.lightPos[2] = -1.5f;

    res.initial_pixels.assign(static_cast<size_t>(kW) * kH * 4u, 0u);
    OS_REQUIRE(XboxBvhDispatchRays(
        builder, tlas_id, &ddesc, res.initial_pixels.data(), res.initial_pixels.size(), &res.initial_stats) ==
        OS_SHIM_S_OK);
    OS_REQUIRE(res.initial_stats.primaryRays == static_cast<uint64_t>(kW) * kH);
    OS_REQUIRE(res.initial_stats.primaryHits > res.initial_stats.primaryRays / 2u);
    OS_REQUIRE(res.initial_stats.frameHash != 0ull);

    // Update cube BLAS color & vertex offset, rebuild TLAS, verify frame hash changes!
    for (auto& t : cube_tris) {
        t.v0[1] += 0.35f; t.v1[1] += 0.35f; t.v2[1] += 0.35f;
        t.color[0] = 0.95f; t.color[1] = 0.20f; t.color[2] = 0.15f;
    }
    OS_REQUIRE(XboxBvhUpdateBlas(
        builder, cube_blas, cube_tris.data(), static_cast<uint32_t>(cube_tris.size())) == OS_SHIM_S_OK);
    uint64_t tlas_updated = 0;
    OS_REQUIRE(XboxBvhBuildTlas(
        builder, insts, 2u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas_updated) == OS_SHIM_S_OK);
    std::vector<uint8_t> updated_pixels(static_cast<size_t>(kW) * kH * 4u, 0u);
    OS_REQUIRE(XboxBvhDispatchRays(
        builder, tlas_updated, &ddesc, updated_pixels.data(), updated_pixels.size(), &res.updated_stats) ==
        OS_SHIM_S_OK);
    OS_REQUIRE(res.updated_stats.frameHash != 0ull);
    OS_REQUIRE(res.updated_stats.frameHash != res.initial_stats.frameHash);

    XboxBvhDestroyBuilder(builder);
    return res;
}

void TestXboxBvhBuilder(bool has_hw_gpu) {
    std::printf("[9/10] XboxBvhBuilder.dll BLAS/TLAS build, update, TraceRay & DispatchRays...\n");
    const BvhSessionResult warp = RunXboxBvhSession(true);
    const double hit_pct =
        100.0 * static_cast<double>(warp.initial_stats.primaryHits) /
        static_cast<double>(warp.initial_stats.primaryRays);

    if (has_hw_gpu) {
        const BvhSessionResult hw = RunXboxBvhSession(false);
        OS_REQUIRE(hw.caps.isWarp == 0u);
        OS_REQUIRE(hw.initial_stats.frameHash == warp.initial_stats.frameHash);
        OS_REQUIRE(hw.updated_stats.frameHash == warp.updated_stats.frameHash);
        OS_REQUIRE(hw.initial_pixels == warp.initial_pixels);
        std::printf("  PASS: BVH HW='%s' (tier=%u) == WARP bit-exact hash=0x%016llX (hit=%.1f%%, updated=0x%016llX).\n",
                    hw.caps.adapterDescription,
                    hw.caps.hardwareTier,
                    static_cast<unsigned long long>(hw.initial_stats.frameHash),
                    hit_pct,
                    static_cast<unsigned long long>(hw.updated_stats.frameHash));
    } else {
        std::printf("  PASS: BVH WARP='%s' hash=0x%016llX (hit=%.1f%%, updated=0x%016llX, WARP-only CI).\n",
                    warp.caps.adapterDescription,
                    static_cast<unsigned long long>(warp.initial_stats.frameHash),
                    hit_pct,
                    static_cast<unsigned long long>(warp.updated_stats.frameHash));
    }
}

// ---------------------------------------------------------------------------
// 10. PE Export Audit Across xgameruntime.dll + All 9 Secondary Shim DLLs
// ---------------------------------------------------------------------------
void TestPeExportAudit() {
    std::printf("[10/10] PE64 export table audit across xgameruntime.dll + 9 new shim DLLs...\n");
    struct DllExpectation {
        const char* dll_name;
        std::vector<const char*> required_exports;
    };
    const std::vector<DllExpectation> specs = {
        {"xgameruntime.dll", {"InitializeApiImpl", "UninitializeApiImpl", "QueryApiImpl", "XGameGetXboxTitleId"}},
        {"pixevt.dll", {"PIXBeginEventOnCpu", "PIXEndEventOnCpu", "PIXSetMarkerOnCpu", "PIXBeginEventOnCommandList", "PIXEndEventOnCommandList", "PIXSetMarkerOnCommandList", "PIXBeginEventOnCommandQueue", "PIXEndEventOnCommandQueue", "PIXSetMarkerOnCommandQueue", "PIXNotifyWakeFromFenceSignal", "PIXReportCounter", "PIXGpuCaptureNextFrames", "PIXGetEventCountForTest"}},
        {"xmem.dll", {"XMemAlloc", "XMemAllocDefault", "XMemFree", "XMemFreeDefault", "XMemSize", "XMemSizeDefault", "XMemGetAuxiliaryTitleMemory", "XMemReleaseAuxiliaryTitleMemory", "XMemMapPhysicalPages", "XMemUnmapPhysicalPages"}},
        {"xcrdapi.dll", {"XCrdInitialize", "XCrdUninitialize", "XCrdGetDiagnosticInfo", "XCrdSetBreadcrumb", "XCrdGetBreadcrumb", "XCrdReportError", "XCrdFlushDiagnostics"}},
        {"libHttpClient.GDK.dll", {"HCInitialize", "HCCleanup", "HCCleanupAsync", "HCGetLibVersion", "HCTraceSetTraceToDebugger", "HCSettingsSetTraceLevel", "HCHttpCallCreate", "HCHttpCallDuplicateHandle", "HCHttpCallCloseHandle", "HCHttpCallRequestSetUrl", "HCHttpCallRequestSetHeader", "HCHttpCallRequestSetRequestBodyBytes", "HCHttpCallRequestSetRequestBodyString", "HCHttpCallPerformAsync", "HCHttpCallResponseGetStatusCode", "HCHttpCallResponseGetResponseString", "HCHttpCallResponseGetResponseBodyBytes", "HCHttpCallResponseGetResponseBodyBytesSize", "HCHttpCallResponseGetHeader", "HCAddMockResponseForTest"}},
        {"XCurl.dll", {"curl_global_init", "curl_global_cleanup", "curl_version", "curl_easy_init", "curl_easy_cleanup", "curl_easy_reset", "curl_easy_setopt", "curl_easy_perform", "curl_easy_getinfo", "curl_easy_strerror", "curl_slist_append", "curl_slist_free_all", "curl_multi_init", "curl_multi_cleanup", "curl_multi_add_handle", "curl_multi_remove_handle", "curl_multi_perform", "curl_multi_info_read"}},
        {"Bink2Scarlett.dll", {"BinkOpen", "BinkOpenBuffer", "BinkClose", "BinkDoFrame", "BinkNextFrame", "BinkWait", "BinkCopyToBuffer", "BinkGoto", "BinkGetSummary", "BinkGetError", "BinkSetSoundOnOff", "BinkPause"}},
        {"amd_fidelityfx_upscaler_xbox.dll", {"ffxUpscalerGetVersion", "ffxUpscalerCreateContext", "ffxUpscalerDestroyContext", "ffxUpscalerDispatchRgba8", "ffxUpscalerConfigure", "ffxUpscalerQuery"}},
        {"amd_fidelityfx_loader_xbox.dll", {"ffxQuery", "ffxCreateContext", "ffxDestroyContext", "ffxConfigure", "ffxDispatch"}},
        {"XboxBvhBuilder.dll", {"XboxBvhCreateBuilder", "XboxBvhDestroyBuilder", "XboxBvhGetCapabilities", "XboxBvhGetPrebuildInfo", "XboxBvhBuildBlas", "XboxBvhUpdateBlas", "XboxBvhBuildTlas", "XboxBvhTraceRay", "XboxBvhDispatchRays"}},
    };

    const fs::path exe_dir = GetExeDir();
    size_t total_exports = 0;
    for (const auto& spec : specs) {
        const fs::path dll_path = exe_dir / spec.dll_name;
        openscarlett::PeInspectionReport rep{};
        OS_REQUIRE(openscarlett::InspectPeFile(dll_path.string(), rep).ok());
        OS_REQUIRE(rep.is_pe64 && rep.is_dll);

#if defined(_WIN32)
        HMODULE mod = LoadLibraryA(dll_path.string().c_str());
        OS_REQUIRE(mod != nullptr);

        const auto* base = reinterpret_cast<const uint8_t*>(mod);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        OS_REQUIRE(dos->e_magic == IMAGE_DOS_SIGNATURE);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        OS_REQUIRE(nt->Signature == IMAGE_NT_SIGNATURE);
        const auto& exp_dir_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        OS_REQUIRE(exp_dir_entry.VirtualAddress != 0u);
        const auto* exp_dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exp_dir_entry.VirtualAddress);
        const auto* name_rvas = reinterpret_cast<const uint32_t*>(base + exp_dir->AddressOfNames);

        std::unordered_set<std::string> exported;
        for (uint32_t i = 0; i < exp_dir->NumberOfNames; ++i) {
            exported.insert(reinterpret_cast<const char*>(base + name_rvas[i]));
        }

        for (const char* req : spec.required_exports) {
            OS_REQUIRE(exported.find(req) != exported.end());
            OS_REQUIRE(GetProcAddress(mod, req) != nullptr);
        }
        total_exports += exported.size();
        std::printf("  %-34s : %2zu exports verified\n", spec.dll_name, exported.size());
        FreeLibrary(mod);
#endif
    }
    std::printf("  PASS: All 10 DLL PE export tables audited (%zu total exports).\n", total_exports);
}

}  // namespace

int main() {
    std::string hw_desc;
    const bool has_hw_gpu = ProbeHardwareD3D12Gpu(hw_desc);
    std::printf("=== OpenScarlett Secondary Shims & QueryApiImpl Suite (HW GPU: %s) ===\n",
                hw_desc.c_str());

    TestXGameRuntimeQueryApi();
    TestPixEvt();
    TestXMem();
    TestXCrdApi();
    TestLibHttpClient();
    TestXCurl();
    TestBink2Scarlett();
    TestFidelityFxShims(has_hw_gpu);
    TestXboxBvhBuilder(has_hw_gpu);
    TestPeExportAudit();

    std::printf("ALL 10 SECONDARY SHIMS & QUERYAPI TESTS PASSED!\n");
    return 0;
}
