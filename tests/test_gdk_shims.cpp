#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/xsapi/xsapi_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Helpers for XMem hook test
// ---------------------------------------------------------------------------
std::atomic<int> g_custom_alloc_calls{0};
std::atomic<int> g_custom_free_calls{0};

void* OPENSCARLETT_GDK_CALL CustomXMemAlloc(size_t size, uint64_t attributes) {
    g_custom_alloc_calls.fetch_add(1, std::memory_order_relaxed);
    return XMemAllocDefault(size, attributes);
}

void OPENSCARLETT_GDK_CALL CustomXMemFree(void* ptr, uint64_t attributes) {
    g_custom_free_calls.fetch_add(1, std::memory_order_relaxed);
    XMemFreeDefault(ptr, attributes);
}

// ---------------------------------------------------------------------------
// Helpers for XTaskQueue tests
// ---------------------------------------------------------------------------
struct CounterCtx {
    std::atomic<int> count{0};
    std::atomic<int> canceled_count{0};
};

void OPENSCARLETT_GDK_CALL IncrementCb(void* ctx, bool canceled) {
    auto* c = static_cast<CounterCtx*>(ctx);
    if (canceled) {
        c->canceled_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        c->count.fetch_add(1, std::memory_order_relaxed);
    }
}

struct OrderCtx {
    std::mutex mutex;
    std::vector<int> values;
};

struct OrderItem {
    OrderCtx* owner;
    int value;
};

void OPENSCARLETT_GDK_CALL RecordOrderCb(void* ctx, bool canceled) {
    auto* item = static_cast<OrderItem*>(ctx);
    if (!canceled) {
        std::lock_guard<std::mutex> lk(item->owner->mutex);
        item->owner->values.push_back(item->value);
    }
}

void OPENSCARLETT_GDK_CALL MonitorCb(void* ctx, XTaskQueueHandle /*q*/, XTaskQueuePort /*p*/) {
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

void OPENSCARLETT_GDK_CALL TerminatedCb(void* ctx) {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Custom XAsync provider test
// ---------------------------------------------------------------------------
const char kCustomAsyncId[] = "TestCustomAsync";

struct CustomAsyncData {
    uint32_t input = 0;
    uint32_t output = 0;
};

XGameResult OPENSCARLETT_GDK_CALL CustomAsyncProvider(
    XAsyncOp op, const XAsyncProviderData* data) {
    auto* st = static_cast<CustomAsyncData*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            st->output = st->input * 3u + 7u;
            XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(uint32_t)) return XGR_E_NOT_SUFFICIENT_BUFFER;
            std::memcpy(data->buffer, &st->output, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            delete st;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

void OPENSCARLETT_GDK_CALL CustomAsyncDoneCb(XAsyncBlock* block) {
    static_cast<std::atomic<bool>*>(block->context)->store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// XGameSave enumeration helpers
// ---------------------------------------------------------------------------
struct EnumContainersCtx {
    std::vector<std::string> names;
    uint32_t last_blob_count = 0;
};

bool OPENSCARLETT_GDK_CALL EnumContainerCb(const XGameSaveContainerInfo* info, void* ctx) {
    auto* c = static_cast<EnumContainersCtx*>(ctx);
    c->names.emplace_back(info->name);
    c->last_blob_count = info->blobCount;
    return true;
}

bool OPENSCARLETT_GDK_CALL EnumBlobCb(const XGameSaveBlobInfo* info, void* ctx) {
    auto* names = static_cast<std::vector<std::string>*>(ctx);
    names->emplace_back(info->name);
    return true;
}

bool OPENSCARLETT_GDK_CALL EnumChunkCb(
    void* ctx, const XPackageChunkSelector* /*sel*/, XPackageChunkAvailability avail) {
    if (avail == XPackageChunkAvailability_Ready) {
        static_cast<int*>(ctx)[0] += 1;
    }
    return true;
}

// ===========================================================================
// 1. xgameruntime.dll tests
// ===========================================================================
void TestXGameRuntime() {
    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    // XSystem checks
    OS_REQUIRE(XSystemGetDeviceType() == XSystemDeviceType_XboxScarlettAnaconda);
    char cid[64]{};
    size_t cid_used = 0;
    OS_REQUIRE(XSystemGetConsoleId(sizeof(cid), cid, &cid_used) == XGR_S_OK);
    OS_REQUIRE(std::strlen(cid) > 0 && cid_used == std::strlen(cid) + 1);

    char sandbox[32]{};
    size_t sb_used = 0;
    OS_REQUIRE(XSystemGetXboxLiveSandboxId(sizeof(sandbox), sandbox, &sb_used) == XGR_S_OK);
    OS_REQUIRE(std::strcmp(sandbox, "RETAIL") == 0);

    const XSystemAnalyticsInfo ainfo = XSystemGetAnalyticsInfo();
    OS_REQUIRE(ainfo.osVersion.major == 10 && ainfo.osVersion.build >= 22000);
    OS_REQUIRE(std::strcmp(ainfo.family, "Windows.Xbox") == 0);

    const XSystemRuntimeInfo rinfo = XSystemGetRuntimeInfo();
    OS_REQUIRE(rinfo.runtimeVersion.major == 10);
    OS_REQUIRE(!XSystemIsHandleValid(nullptr));
    OS_REQUIRE(XSystemIsHandleValid(cid));

    // XMem + custom hooks
    OS_REQUIRE(XMemSetAllocationHooks(&CustomXMemAlloc, &CustomXMemFree) == XGR_S_OK);
    void* mem = XMemAlloc(256, 0);
    OS_REQUIRE(mem != nullptr);
    std::memset(mem, 0x5A, 256);
    XMemFree(mem, 0);
    OS_REQUIRE(g_custom_alloc_calls.load() == 1);
    OS_REQUIRE(g_custom_free_calls.load() == 1);
    OS_REQUIRE(XMemSetAllocationHooks(nullptr, nullptr) == XGR_S_OK);

    // XPackage & XDisplay
    OS_REQUIRE(XPackageIsPackagedProcess());
    char pkg[128]{};
    OS_REQUIRE(XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg), pkg) == XGR_S_OK);
    OS_REQUIRE(std::strstr(pkg, "OpenScarlett") != nullptr);
    int ready_chunks = 0;
    OS_REQUIRE(XPackageEnumerateChunkAvailability(pkg, 0, nullptr, &ready_chunks, &EnumChunkCb) == XGR_S_OK);
    OS_REQUIRE(ready_chunks >= 1);

    XDisplayHdrModeInfo hdr_info{};
    (void)XDisplayTryEnableHdrMode(0, &hdr_info);
    OS_REQUIRE(hdr_info.maxToneMapLuminance > 0.0f);
    XDisplayTimeoutDeferralHandle deferral = nullptr;
    OS_REQUIRE(XDisplayTryAcquireTimeoutDeferral(&deferral) == XGR_S_OK && deferral != nullptr);
    XDisplayCloseTimeoutDeferralHandle(deferral);

    // XTaskQueue: ThreadPool + delayed callback
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_ThreadPool,
                       XTaskQueueDispatchMode_ThreadPool, &q) == XGR_S_OK);
        CounterCtx counter{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Work, &counter, &IncrementCb) == XGR_S_OK);
        const auto t0 = std::chrono::steady_clock::now();
        OS_REQUIRE(XTaskQueueSubmitDelayedCallback(q, XTaskQueuePort_Work, 30, &counter, &IncrementCb) == XGR_S_OK);

        const auto deadline = t0 + std::chrono::seconds(3);
        while (counter.count.load() < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
        OS_REQUIRE(counter.count.load() == 2);
        OS_REQUIRE(elapsed_ms >= 20);

        std::atomic<bool> term_called{false};
        OS_REQUIRE(XTaskQueueTerminate(q, true, &term_called, &TerminatedCb) == XGR_S_OK);
        OS_REQUIRE(term_called.load());
        XTaskQueueCloseHandle(q);
    }

    // XTaskQueue: SerializedThreadPool preserves FIFO ordering
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_SerializedThreadPool,
                       XTaskQueueDispatchMode_Manual, &q) == XGR_S_OK);
        OrderCtx octx{};
        OrderItem items[4] = {{&octx, 1}, {&octx, 2}, {&octx, 3}, {&octx, 4}};
        for (int i = 0; i < 4; ++i) {
            OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Work, &items[i], &RecordOrderCb) == XGR_S_OK);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard<std::mutex> lk(octx.mutex);
            if (octx.values.size() == 4) break;
        }
        OS_REQUIRE(octx.values.size() == 4);
        OS_REQUIRE(octx.values[0] == 1 && octx.values[1] == 2 &&
                   octx.values[2] == 3 && octx.values[3] == 4);
        XTaskQueueTerminate(q, true, nullptr, nullptr);
        XTaskQueueCloseHandle(q);
    }

    // XTaskQueue: Manual mode + monitor + Immediate mode
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Immediate, &q) == XGR_S_OK);
        std::atomic<int> monitor_hits{0};
        XTaskQueueRegistrationToken tok{};
        OS_REQUIRE(XTaskQueueRegisterMonitor(q, &monitor_hits, &MonitorCb, &tok) == XGR_S_OK);

        CounterCtx manual_c{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Work, &manual_c, &IncrementCb) == XGR_S_OK);
        OS_REQUIRE(monitor_hits.load() == 1);
        OS_REQUIRE(manual_c.count.load() == 0);
        OS_REQUIRE(XTaskQueueDispatch(q, XTaskQueuePort_Work, 100));
        OS_REQUIRE(manual_c.count.load() == 1);

        // Immediate completion port runs inline synchronously
        CounterCtx imm_c{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q, XTaskQueuePort_Completion, &imm_c, &IncrementCb) == XGR_S_OK);
        OS_REQUIRE(imm_c.count.load() == 1);

        XTaskQueueUnregisterMonitor(q, tok);
        XTaskQueueCloseHandle(q);
    }

    // XAsync custom provider roundtrip
    {
        std::atomic<bool> done{false};
        XAsyncBlock async{};
        async.context = &done;
        async.callback = &CustomAsyncDoneCb;
        auto* ctx = new CustomAsyncData{14u, 0u};
        OS_REQUIRE(XAsyncBegin(&async, ctx, kCustomAsyncId, kCustomAsyncId, &CustomAsyncProvider) == XGR_S_OK);
        OS_REQUIRE(XAsyncSchedule(&async, 0) == XGR_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&async, true) == XGR_S_OK);
        OS_REQUIRE(done.load());
        size_t sz = 0;
        OS_REQUIRE(XAsyncGetResultSize(&async, &sz) == XGR_S_OK && sz == sizeof(uint32_t));
        uint32_t val = 0;
        size_t used = 0;
        OS_REQUIRE(XAsyncGetResult(&async, kCustomAsyncId, sizeof(val), &val, &used) == XGR_S_OK);
        OS_REQUIRE(used == sizeof(uint32_t) && val == 49u);
    }

    // XUser + XGameSave full disk persistence roundtrip on E: drive.
    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        OS_REQUIRE(XUserAddResult(&ab, &user) == XGR_S_OK && user != nullptr);

        uint64_t xuid = 0;
        OS_REQUIRE(XUserGetId(user, &xuid) == XGR_S_OK && xuid != 0);
        XUserLocalId lid{};
        OS_REQUIRE(XUserGetLocalId(user, &lid) == XGR_S_OK && lid.value != 0);

        char gt[64]{};
        size_t gt_used = 0;
        OS_REQUIRE(XUserGetGamertag(user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) == XGR_S_OK);
        OS_REQUIRE(std::strlen(gt) > 0);

        XUserState st = XUserState_SignedOut;
        OS_REQUIRE(XUserGetState(user, &st) == XGR_S_OK && st == XUserState_SignedIn);
        XUserAgeGroup age = XUserAgeGroup_Unknown;
        OS_REQUIRE(XUserGetAgeGroup(user, &age) == XGR_S_OK && age == XUserAgeGroup_Adult);
        bool has_priv = false;
        XUserPrivilegeDenyReason deny = XUserPrivilegeDenyReason_Unknown;
        OS_REQUIRE(XUserCheckPrivilege(user, XUserPrivilegeOptions_None, XUserPrivilege_CloudSavedGames, &has_priv, &deny) == XGR_S_OK);
        OS_REQUIRE(has_priv && deny == XUserPrivilegeDenyReason_None);
        uint32_t max_u = 0;
        OS_REQUIRE(XUserGetMaxUsers(&max_u) == XGR_S_OK && max_u >= 4);

        XUserHandle dup = nullptr;
        OS_REQUIRE(XUserDuplicateHandle(user, &dup) == XGR_S_OK);
        OS_REQUIRE(XUserCompare(user, dup) == 0);
        XUserCloseHandle(dup);
    }

    // XGameSave write -> close -> reopen -> read back -> delete
    {
        const char* kScid = "00000000-0000-0000-0000-00005ca0cafe";
        const char* kContainer = "slot_01";
        const std::vector<uint8_t> payload_a = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44};
        const std::vector<uint8_t> payload_b = {'O', 'p', 'e', 'n', 'S', 'c', 'a', 'r', 'l', 'e', 't', 't'};

        // Async init provider
        XAsyncBlock save_init_ab{};
        OS_REQUIRE(XGameSaveInitializeProviderAsync(user, kScid, false, &save_init_ab) == XGR_S_OK);
        XGameSaveProviderHandle prov = nullptr;
        OS_REQUIRE(XGameSaveInitializeProviderResult(&save_init_ab, &prov) == XGR_S_OK && prov != nullptr);

        // Clean any leftover container from prior runs
        (void)XGameSaveDeleteContainer(prov, kContainer);

        XGameSaveContainerHandle cont = nullptr;
        OS_REQUIRE(XGameSaveCreateContainer(prov, kContainer, &cont) == XGR_S_OK);
        XGameSaveUpdateHandle upd = nullptr;
        OS_REQUIRE(XGameSaveCreateUpdate(cont, "Main Save Slot", &upd) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitBlobWrite(upd, "header.bin", payload_a.data(), payload_a.size()) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitBlobWrite(upd, "world.dat", payload_b.data(), payload_b.size()) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitUpdate(upd) == XGR_S_OK);
        XGameSaveCloseUpdate(upd);
        XGameSaveCloseContainer(cont);
        XGameSaveCloseProvider(prov);

        // Reopen fresh provider and read back from disk on E: drive!
        XGameSaveProviderHandle prov2 = nullptr;
        OS_REQUIRE(XGameSaveInitializeProvider(user, kScid, false, &prov2) == XGR_S_OK && prov2 != nullptr);

        EnumContainersCtx ectx{};
        OS_REQUIRE(XGameSaveEnumerateContainerInfo(prov2, &ectx, &EnumContainerCb) == XGR_S_OK);
        OS_REQUIRE(!ectx.names.empty() && ectx.names[0] == kContainer && ectx.last_blob_count == 2);

        XGameSaveContainerHandle cont2 = nullptr;
        OS_REQUIRE(XGameSaveCreateContainer(prov2, kContainer, &cont2) == XGR_S_OK);
        std::vector<std::string> bnames;
        OS_REQUIRE(XGameSaveEnumerateBlobInfo(cont2, &bnames, &EnumBlobCb) == XGR_S_OK);
        OS_REQUIRE(bnames.size() == 2);

        const char* req_names[2] = {"header.bin", "world.dat"};
        uint32_t req_count = 2;
        std::vector<uint8_t> read_buf(1024, 0);
        auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf.data());
        OS_REQUIRE(XGameSaveReadBlobData(cont2, req_names, &req_count, read_buf.size(), blobs) == XGR_S_OK);
        OS_REQUIRE(req_count == 2);
        OS_REQUIRE(std::strcmp(blobs[0].info.name, "header.bin") == 0);
        OS_REQUIRE(blobs[0].info.size == payload_a.size());
        OS_REQUIRE(std::memcmp(blobs[0].data, payload_a.data(), payload_a.size()) == 0);
        OS_REQUIRE(std::strcmp(blobs[1].info.name, "world.dat") == 0);
        OS_REQUIRE(blobs[1].info.size == payload_b.size());
        OS_REQUIRE(std::memcmp(blobs[1].data, payload_b.data(), payload_b.size()) == 0);

        // Delete one blob and verify XGR_E_GS_BLOB_NOT_FOUND
        XGameSaveUpdateHandle del_upd = nullptr;
        OS_REQUIRE(XGameSaveCreateUpdate(cont2, "Main Save Slot", &del_upd) == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitBlobDelete(del_upd, "world.dat") == XGR_S_OK);
        OS_REQUIRE(XGameSaveSubmitUpdate(del_upd) == XGR_S_OK);
        XGameSaveCloseUpdate(del_upd);

        const char* missing[1] = {"world.dat"};
        uint32_t mcount = 1;
        OS_REQUIRE(XGameSaveReadBlobData(cont2, missing, &mcount, read_buf.size(), blobs) == XGR_E_GS_BLOB_NOT_FOUND);

        int64_t quota = 0;
        OS_REQUIRE(XGameSaveGetRemainingQuota(prov2, &quota) == XGR_S_OK && quota > 0);

        XGameSaveCloseContainer(cont2);
        OS_REQUIRE(XGameSaveDeleteContainer(prov2, kContainer) == XGR_S_OK);

        EnumContainersCtx after_del{};
        OS_REQUIRE(XGameSaveEnumerateContainerInfo(prov2, &after_del, &EnumContainerCb) == XGR_S_OK);
        OS_REQUIRE(after_del.names.empty());

        XGameSaveCloseProvider(prov2);
    }

    XUserCloseHandle(user);
    std::printf("[PASS] xgameruntime.dll (XTaskQueue, XAsync, XUser, XGameSave disk roundtrip, XSystem, XMem, XPackage, XDisplay)\n");
}

// ===========================================================================
// 2. GameInput.dll tests
// ===========================================================================
void TestGameInput() {
    IGameInput* gi = nullptr;
    OS_REQUIRE(GameInputCreate(&gi) == GAMEINPUT_S_OK && gi != nullptr);

    const uint64_t t1 = gi->GetCurrentTimestamp();
    OS_REQUIRE(t1 > 0);

    IGameInputReading* r1 = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindGamepad, nullptr, &r1) == GAMEINPUT_S_OK && r1 != nullptr);
    GameInputGamepadState gp{};
    OS_REQUIRE(r1->GetGamepadState(&gp));
    OS_REQUIRE(r1->GetTimestamp() >= t1);

    IGameInputDevice* dev = nullptr;
    r1->GetDevice(&dev);
    OS_REQUIRE(dev != nullptr);
    const GameInputDeviceInfo* info = dev->GetDeviceInfo();
    OS_REQUIRE(info != nullptr && info->vendorId == 0x045Eu && info->productId == 0x0B12u);
    OS_REQUIRE((dev->GetDeviceStatus() & GameInputDeviceConnected) != 0);

    // Rumble test (set then reset to zero)
    GameInputRumbleParams rumble{0.25f, 0.15f, 0.0f, 0.0f};
    dev->SetRumbleState(&rumble);
    GameInputRumbleParams stop_rumble{0.0f, 0.0f, 0.0f, 0.0f};
    dev->SetRumbleState(&stop_rumble);

    // Second reading + history navigation
    IGameInputReading* r2 = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindGamepad, dev, &r2) == GAMEINPUT_S_OK && r2 != nullptr);
    OS_REQUIRE(r2->GetSequenceNumber(GameInputKindGamepad) > r1->GetSequenceNumber(GameInputKindGamepad));

    IGameInputReading* prev = nullptr;
    OS_REQUIRE(gi->GetPreviousReading(r2, GameInputKindGamepad, dev, &prev) == GAMEINPUT_S_OK && prev != nullptr);
    OS_REQUIRE(prev->GetSequenceNumber(GameInputKindGamepad) == r1->GetSequenceNumber(GameInputKindGamepad));

    IGameInputReading* next = nullptr;
    OS_REQUIRE(gi->GetNextReading(r1, GameInputKindGamepad, dev, &next) == GAMEINPUT_S_OK && next != nullptr);
    OS_REQUIRE(next->GetSequenceNumber(GameInputKindGamepad) == r2->GetSequenceNumber(GameInputKindGamepad));

    // Keyboard state query
    GameInputKeyState keys[8]{};
    const uint32_t kcount = r2->GetKeyState(8, keys);
    OS_REQUIRE(kcount <= 8);

    next->Release();
    prev->Release();
    r2->Release();
    dev->Release();
    r1->Release();
    gi->Release();

    std::printf("[PASS] GameInput.dll (XInput + keyboard fallback, rumble, reading history)\n");
}

// ===========================================================================
// 3. XAudio2_9.dll + AcpHal.dll tests (PCM16 + real XMA2 0x0166 decoding)
// ===========================================================================
void TestXAudio2AndAcpHal() {
    // Generate real XMA2 2048-byte packets from a 440 Hz mono sine wave.
    openscarlett::Xma2Config xma_cfg{};
    xma_cfg.sample_rate = 48000u;
    xma_cfg.channels = 1u;
    constexpr size_t kXmaFrames = 4096u;
    std::vector<float> sine_mono(kXmaFrames);
    for (size_t i = 0; i < kXmaFrames; ++i) {
        sine_mono[i] = 0.6f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }
    std::vector<uint8_t> xma_packets;
    OS_REQUIRE(openscarlett::Xma2EncodePcmToPackets(sine_mono.data(), kXmaFrames, xma_cfg, xma_packets).ok());
    OS_REQUIRE(!xma_packets.empty() && (xma_packets.size() % openscarlett::kXma2PacketSize) == 0);

    // --- Test XAudio2_9.dll ---
    IXAudio2* xa = nullptr;
    OS_REQUIRE(XAudio2Create(&xa, 0, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);

    IXAudio2MasteringVoice* master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&master, 2, 48000) == OS_XAUDIO2_S_OK && master != nullptr);

    IXAudio2SubmixVoice* submix = nullptr;
    OS_REQUIRE(xa->CreateSubmixVoice(&submix, 2, 48000) == OS_XAUDIO2_S_OK && submix != nullptr);
    OS_REQUIRE(submix->SetVolume(0.8f) == OS_XAUDIO2_S_OK);

    // A) Pure XMA2 (wFormatTag == 0x0166) source voice decode & mix verification
    OS_XMA2WAVEFORMATEX xma_fmt{};
    xma_fmt.wfx.wFormatTag = OS_WAVE_FORMAT_XMA2;
    xma_fmt.wfx.nChannels = 1;
    xma_fmt.wfx.nSamplesPerSec = 48000;
    xma_fmt.wfx.wBitsPerSample = 16;
    xma_fmt.wfx.cbSize = sizeof(OS_XMA2WAVEFORMATEX) - sizeof(OS_WAVEFORMATEX);
    xma_fmt.NumStreams = 1;
    xma_fmt.SamplesEncoded = static_cast<uint32_t>(kXmaFrames);
    xma_fmt.BytesPerBlock = openscarlett::kXma2PacketSize;
    xma_fmt.BlockCount = static_cast<uint16_t>(xma_packets.size() / openscarlett::kXma2PacketSize);

    OS_XAUDIO2_SEND_DESCRIPTOR send_desc{0, submix};
    OS_XAUDIO2_VOICE_SENDS sends{1, &send_desc};
    IXAudio2SourceVoice* xma_voice = nullptr;
    OS_REQUIRE(xa->CreateSourceVoice(&xma_voice, &xma_fmt.wfx, 0, 2.0f, nullptr, &sends) == OS_XAUDIO2_S_OK);

    OS_XAUDIO2_BUFFER xma_buf{};
    xma_buf.Flags = OS_XAUDIO2_END_OF_STREAM;
    xma_buf.AudioBytes = static_cast<uint32_t>(xma_packets.size());
    xma_buf.pAudioData = xma_packets.data();
    OS_REQUIRE(xma_voice->SubmitSourceBuffer(&xma_buf) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(xma_voice->Start() == OS_XAUDIO2_S_OK);

    // Render 2048 stereo frames from XMA2 voice alone and verify real waveform energy.
    constexpr uint32_t kMixFrames = 2048u;
    std::vector<float> xma_mix(kMixFrames * 2u, 0.0f);
    OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, xma_mix.data(), kMixFrames) == OS_XAUDIO2_S_OK);
    double xma_energy = 0.0;
    float xma_peak = 0.0f;
    for (float s : xma_mix) {
        xma_energy += static_cast<double>(s) * static_cast<double>(s);
        xma_peak = std::max(xma_peak, std::fabs(s));
    }
    OS_REQUIRE(xma_energy > 1.0 && xma_peak > 0.05f && xma_peak <= 1.0f);

    // B) PCM16 stereo voice + XMA2 voice mixed together
    OS_WAVEFORMATEX pcm_fmt{};
    pcm_fmt.wFormatTag = OS_WAVE_FORMAT_PCM;
    pcm_fmt.nChannels = 2;
    pcm_fmt.nSamplesPerSec = 48000;
    pcm_fmt.wBitsPerSample = 16;
    pcm_fmt.nBlockAlign = 4;
    pcm_fmt.nAvgBytesPerSec = 48000 * 4;

    IXAudio2SourceVoice* pcm_voice = nullptr;
    OS_REQUIRE(xa->CreateSourceVoice(&pcm_voice, &pcm_fmt) == OS_XAUDIO2_S_OK);
    std::vector<int16_t> pcm16(kMixFrames * 2u);
    for (uint32_t i = 0; i < kMixFrames; ++i) {
        const float v = 0.4f * std::sin(2.0f * 3.14159265f * 880.0f * static_cast<float>(i) / 48000.0f);
        const int16_t s = static_cast<int16_t>(v * 32767.0f);
        pcm16[i * 2 + 0] = s;
        pcm16[i * 2 + 1] = s;
    }
    OS_XAUDIO2_BUFFER pcm_buf{};
    pcm_buf.Flags = OS_XAUDIO2_END_OF_STREAM;
    pcm_buf.AudioBytes = static_cast<uint32_t>(pcm16.size() * sizeof(int16_t));
    pcm_buf.pAudioData = reinterpret_cast<const uint8_t*>(pcm16.data());
    OS_REQUIRE(pcm_voice->SubmitSourceBuffer(&pcm_buf) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(pcm_voice->SetVolume(0.5f) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(pcm_voice->SetFrequencyRatio(1.0f) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(pcm_voice->Start() == OS_XAUDIO2_S_OK);

    std::vector<float> combined_mix(kMixFrames * 2u, 0.0f);
    OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, combined_mix.data(), kMixFrames) == OS_XAUDIO2_S_OK);
    double comb_energy = 0.0;
    for (float s : combined_mix) {
        comb_energy += static_cast<double>(s) * static_cast<double>(s);
    }
    OS_REQUIRE(comb_energy > 1.0);

    OS_XAUDIO2_VOICE_STATE vstate{};
    pcm_voice->GetState(&vstate);
    OS_REQUIRE(vstate.SamplesPlayed == kMixFrames);

    OS_XAUDIO2_PERFORMANCE_DATA perf{};
    xa->GetPerformanceData(&perf);
    OS_REQUIRE(perf.TotalSourceVoiceCount == 2);

    pcm_voice->DestroyVoice();
    xma_voice->DestroyVoice();
    submix->DestroyVoice();
    master->DestroyVoice();
    xa->Release();

    // --- Test AcpHal.dll ---
    AcpHalShapeContexts req_ctx{16, 8, 4, 4, nullptr};
    AcpHalShapeContexts alloc_ctx{};
    OS_REQUIRE(AcpHalAllocateShapeContexts(&req_ctx, &alloc_ctx) == ACPHAL_S_OK);
    OS_REQUIRE(alloc_ctx.numXmaContexts == 16 && alloc_ctx.internalAllocation != nullptr);

    IAcpHal* acp = nullptr;
    OS_REQUIRE(AcpHalCreate(&acp) == ACPHAL_S_OK && acp != nullptr);

    AcpHalXmaContextDesc xdesc{};
    xdesc.sampleRate = 48000;
    xdesc.channels = 1;
    xdesc.blockSize = openscarlett::kXma2DefaultBlockSize;
    xdesc.subframesPerFrame = 4;
    xdesc.quantBits = 12;
    OS_REQUIRE(acp->ConfigureXmaContext(0, &xdesc) == ACPHAL_S_OK);

    std::vector<float> acp_pcm(8192, 0.0f);
    uint32_t written = 0;
    AcpHalCommand cmd{};
    cmd.type = AcpHalCommand_XmaDecode;
    cmd.contextIndex = 0;
    cmd.inputData = xma_packets.data();
    cmd.inputBytes = static_cast<uint32_t>(xma_packets.size());
    cmd.outputFloatPcm = acp_pcm.data();
    cmd.outputCapacitySamples = static_cast<uint32_t>(acp_pcm.size());
    cmd.outSamplesWritten = &written;
    cmd.xmaConfig = xdesc;

    OS_REQUIRE(acp->SubmitCommand(&cmd) == ACPHAL_S_OK);
    OS_REQUIRE(acp->WaitIdle() == ACPHAL_S_OK);
    OS_REQUIRE(written >= kXmaFrames);

    double acp_energy = 0.0;
    for (uint32_t i = 0; i < written; ++i) {
        acp_energy += static_cast<double>(acp_pcm[i]) * static_cast<double>(acp_pcm[i]);
    }
    OS_REQUIRE(acp_energy > 1.0);

    uint64_t pkts_decoded = 0;
    OS_REQUIRE(acp->GetDecodedPacketCount(0, &pkts_decoded) == ACPHAL_S_OK && pkts_decoded >= 1);

    acp->Release();
    OS_REQUIRE(AcpHalReleaseShapeContexts(&alloc_ctx) == ACPHAL_S_OK);

    std::printf("[PASS] XAudio2_9.dll + AcpHal.dll (PCM16 + real XMA2 0x0166 decode & float32 mix verified)\n");
}

// ===========================================================================
// 4. dstorage_xs.dll tests (Raw file, GDeflate file, BCPack memory)
// ===========================================================================
void TestDirectStorage() {
    DSTORAGE_CONFIGURATION1 cfg{};
    cfg.NumSubmitThreads = 1;
    cfg.NumBuiltInCpuDecompressionThreads = 1;
    OS_REQUIRE(DStorageSetConfiguration1(&cfg) == OS_DSTORAGE_S_OK);

    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(factory != nullptr);

    // Payload 1: 16 KiB raw payload
    std::vector<uint8_t> raw_payload(16384);
    for (size_t i = 0; i < raw_payload.size(); ++i) {
        raw_payload[i] = static_cast<uint8_t>((i * 29 + 7) & 0xFFu);
    }

    // Payload 2: 96 KiB GDeflate-compressed payload (multi-tile!)
    std::vector<uint8_t> gdef_src(96 * 1024);
    for (size_t i = 0; i < gdef_src.size(); ++i) {
        gdef_src[i] = static_cast<uint8_t>(((i * 5) + (i / 64) * 19) & 0xFFu);
    }
    std::vector<uint8_t> gdef_compressed;
    OS_REQUIRE(openscarlett::GDeflateCompress(gdef_src.data(), gdef_src.size(), gdef_compressed));
    OS_REQUIRE(!gdef_compressed.empty());

    // Payload 3: BC7 64x64 BCPack-compressed texture in memory
    constexpr uint32_t kTexW = 64, kTexH = 64;
    const size_t bc7_bytes = static_cast<size_t>((kTexW / 4) * (kTexH / 4) * 16);
    std::vector<uint8_t> bc7_src(bc7_bytes);
    for (size_t b = 0; b < bc7_bytes / 16; ++b) {
        bc7_src[b * 16 + 0] = 0x40;
        for (size_t k = 1; k < 16; ++k) {
            bc7_src[b * 16 + k] = static_cast<uint8_t>((b * 7 + k * 13) & 0xFFu);
        }
    }
    std::vector<uint8_t> bcpack_compressed;
    OS_REQUIRE(openscarlett::BcPackCompress(
                   openscarlett::BcFormat::BC7, kTexW, kTexH,
                   bc7_src.data(), bc7_src.size(), bcpack_compressed)
                   .ok());
    OS_REQUIRE(!bcpack_compressed.empty());

    // Write real combined data file inside the build directory (on E: locally, portable in CI)
    const fs::path test_dir = fs::path(OPENSCARLETT_BINARY_DIR) / ".tmp_dstorage_test";
    std::error_code ec;
    fs::create_directories(test_dir, ec);
    const fs::path test_file = test_dir / "pack_stream.bin";
    {
        std::ofstream ofs(test_file, std::ios::binary | std::ios::trunc);
        OS_REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(raw_payload.data()),
                  static_cast<std::streamsize>(raw_payload.size()));
        ofs.write(reinterpret_cast<const char*>(gdef_compressed.data()),
                  static_cast<std::streamsize>(gdef_compressed.size()));
        ofs.flush();
        OS_REQUIRE(ofs.good());
    }

    IDStorageFileX* ds_file = nullptr;
    const std::wstring wpath = test_file.wstring();
    OS_REQUIRE(factory->OpenFile(wpath.c_str(), &IID_IDStorageFileX,
                                 reinterpret_cast<void**>(&ds_file)) == OS_DSTORAGE_S_OK);
    DSTORAGE_FILE_INFO finfo{};
    OS_REQUIRE(ds_file->GetFileInformation(&finfo) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(finfo.FileSize == raw_payload.size() + gdef_compressed.size());

    DSTORAGE_QUEUE_DESC qdesc{};
    qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qdesc.Name = "test_dstorage_queue";

    IDStorageQueueX* queue = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX,
                                    reinterpret_cast<void**>(&queue)) == OS_DSTORAGE_S_OK);

    IDStorageStatusArrayX* status = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(4, "test_status", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&status)) == OS_DSTORAGE_S_OK);

    std::vector<uint8_t> raw_out(raw_payload.size(), 0);
    std::vector<uint8_t> gdef_out(gdef_src.size(), 0);
    std::vector<uint8_t> bc7_out(bc7_src.size(), 0);

    // Request 1: Uncompressed file read
    DSTORAGE_REQUEST r1{};
    r1.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    r1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r1.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r1.Source.File.Source = ds_file;
    r1.Source.File.Offset = 0;
    r1.Source.File.Size = static_cast<uint32_t>(raw_payload.size());
    r1.Destination.Memory.Buffer = raw_out.data();
    r1.Destination.Memory.Size = static_cast<uint32_t>(raw_out.size());
    r1.UncompressedSize = static_cast<uint32_t>(raw_payload.size());
    queue->EnqueueRequest(&r1);
    queue->EnqueueStatus(status, 0);

    // Request 2: GDeflate file decompress
    DSTORAGE_REQUEST r2{};
    r2.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    r2.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r2.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r2.Source.File.Source = ds_file;
    r2.Source.File.Offset = raw_payload.size();
    r2.Source.File.Size = static_cast<uint32_t>(gdef_compressed.size());
    r2.Destination.Memory.Buffer = gdef_out.data();
    r2.Destination.Memory.Size = static_cast<uint32_t>(gdef_out.size());
    r2.UncompressedSize = static_cast<uint32_t>(gdef_src.size());
    queue->EnqueueRequest(&r2);
    queue->EnqueueStatus(status, 1);

    // Request 3: BCPack memory decompress
    DSTORAGE_REQUEST r3{};
    r3.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_BCPACK;
    r3.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r3.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r3.Source.Memory.Source = bcpack_compressed.data();
    r3.Source.Memory.Size = static_cast<uint32_t>(bcpack_compressed.size());
    r3.Destination.Memory.Buffer = bc7_out.data();
    r3.Destination.Memory.Size = static_cast<uint32_t>(bc7_out.size());
    r3.UncompressedSize = static_cast<uint32_t>(bc7_src.size());
    queue->EnqueueRequest(&r3);
    queue->EnqueueStatus(status, 2);

#if defined(_WIN32)
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OS_REQUIRE(ev != nullptr);
    queue->EnqueueSetEvent(ev);
    queue->Submit();
    OS_REQUIRE(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    CloseHandle(ev);
#else
    queue->Submit();
    while (!status->IsComplete(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif

    OS_REQUIRE(status->IsComplete(0) && status->GetHResult(0) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(status->IsComplete(1) && status->GetHResult(1) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(status->IsComplete(2) && status->GetHResult(2) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(raw_out == raw_payload);
    OS_REQUIRE(gdef_out == gdef_src);
    OS_REQUIRE(bc7_out == bc7_src);

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    OS_REQUIRE(err.FailureCount == 0);

    queue->Close();
    queue->Release();
    status->Release();
    ds_file->Close();
    ds_file->Release();
    factory->Release();

    fs::remove(test_file, ec);
    std::printf("[PASS] dstorage_xs.dll (async raw file + GDeflate file + BCPack memory verified bit-exact)\n");
}

// ===========================================================================
// 5. Microsoft.Xbox.Services.GDK.C.dll (XSAPI) tests
// ===========================================================================
void TestXsapi() {
    OS_REQUIRE(XblMemSetFunctions(nullptr, nullptr) == XBL_S_OK);

    XblInitArgs init_args{};
    init_args.queue = nullptr;
    init_args.scid = "00000000-0000-0000-0000-00005ca0beef";
    OS_REQUIRE(XblInitialize(&init_args) == XBL_S_OK);

    XAsyncBlock user_ab{};
    OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_ab) == XGR_S_OK);
    XUserHandle user = nullptr;
    OS_REQUIRE(XUserAddResult(&user_ab, &user) == XGR_S_OK && user != nullptr);

    XblContextHandle ctx = nullptr;
    OS_REQUIRE(XblContextCreateHandle(user, &ctx) == XBL_S_OK && ctx != nullptr);

    uint64_t xuid = 0;
    OS_REQUIRE(XblContextGetXboxUserId(ctx, &xuid) == XBL_S_OK && xuid != 0);

    // Update achievement to 100%
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblAchievementsUpdateAchievementAsync(ctx, xuid, "ach_first_blood", 100u, &ab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, true) == XBL_S_OK);
    }

    // Query achievements and verify unlocked state
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       ctx, xuid, 0, XblAchievementType_All, true,
                       XblAchievementOrderBy_DefaultOrder, 0, 32, &ab) == XBL_S_OK);
        XblAchievementsResultHandle res = nullptr;
        OS_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res) == XBL_S_OK && res != nullptr);
        const XblAchievement* items = nullptr;
        size_t count = 0;
        OS_REQUIRE(XblAchievementsResultGetAchievements(res, &items, &count) == XBL_S_OK);
        OS_REQUIRE(count >= 1);
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(items[i].id, "ach_first_blood") == 0) {
                OS_REQUIRE(items[i].percentComplete == 100u);
                OS_REQUIRE(items[i].progressState == XblAchievementProgressState_Achieved);
                found = true;
            }
        }
        OS_REQUIRE(found);
        XblAchievementsResultCloseHandle(res);
    }

    // Profile query
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblProfileGetUserProfileAsync(ctx, xuid, &ab) == XBL_S_OK);
        XblUserProfile profile{};
        OS_REQUIRE(XblProfileGetUserProfileResult(&ab, &profile) == XBL_S_OK);
        OS_REQUIRE(profile.xboxUserId == xuid);
        OS_REQUIRE(std::strlen(profile.gamertag) > 0);
        OS_REQUIRE(std::atoi(profile.gamerscore) >= 25);
    }

    // Presence set + get
    {
        XblPresenceRichPresenceIds rp{};
        std::snprintf(rp.scid, sizeof(rp.scid), "%s", init_args.scid);
        rp.presenceId = "PlayingCampaign";
        XAsyncBlock set_ab{};
        OS_REQUIRE(XblPresenceSetPresenceAsync(ctx, true, &rp, &set_ab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&set_ab, true) == XBL_S_OK);

        XAsyncBlock get_ab{};
        OS_REQUIRE(XblPresenceGetPresenceAsync(ctx, xuid, &get_ab) == XBL_S_OK);
        XblPresenceRecordHandle prec = nullptr;
        OS_REQUIRE(XblPresenceGetPresenceResult(&get_ab, &prec) == XBL_S_OK && prec != nullptr);
        XblPresenceUserState ustate = XblPresenceUserState_Unknown;
        OS_REQUIRE(XblPresenceRecordGetUserState(prec, &ustate) == XBL_S_OK &&
                   ustate == XblPresenceUserState_Online);
        const XblPresenceDeviceRecord* devs = nullptr;
        size_t dcount = 0;
        OS_REQUIRE(XblPresenceRecordGetDeviceRecords(prec, &devs, &dcount) == XBL_S_OK && dcount == 1);
        OS_REQUIRE(devs[0].deviceType == XblPresenceDeviceType_Scarlett);
        XblPresenceRecordCloseHandle(prec);
    }

    // Social relationships (offline-safe)
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XblSocialGetSocialRelationshipsAsync(
                       ctx, xuid, XblSocialRelationshipFilter_All, 0, 16, &ab) == XBL_S_OK);
        XblSocialRelationshipResultHandle sres = nullptr;
        OS_REQUIRE(XblSocialGetSocialRelationshipsResult(&ab, &sres) == XBL_S_OK && sres != nullptr);
        const XblSocialRelationship* rels = nullptr;
        size_t rcount = 99;
        OS_REQUIRE(XblSocialRelationshipResultGetRelationships(sres, &rels, &rcount) == XBL_S_OK);
        OS_REQUIRE(rcount == 0);
        bool has_next = true;
        OS_REQUIRE(XblSocialRelationshipResultHasNext(sres, &has_next) == XBL_S_OK && !has_next);
        XblSocialRelationshipResultCloseHandle(sres);
    }

    // Title-managed stats write + read back
    {
        XblTitleManagedStatistic st{};
        st.statisticName = "HighScore";
        st.statisticType = XblTitleManagedStatType_Number;
        st.numberValue = 98765.0;
        XAsyncBlock wab{};
        OS_REQUIRE(XblTitleManagedStatsWriteAsync(ctx, xuid, &st, 1, &wab) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&wab, true) == XBL_S_OK);

        XAsyncBlock rab{};
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticAsync(
                       ctx, xuid, init_args.scid, "HighScore", &rab) == XBL_S_OK);
        size_t rsize = 0;
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticResultSize(&rab, &rsize) == XBL_S_OK && rsize > 0);
        std::vector<uint8_t> buf(rsize);
        XblUserStatisticsResult* res = nullptr;
        size_t used = 0;
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticResult(&rab, buf.size(), buf.data(), &res, &used) == XBL_S_OK);
        OS_REQUIRE(res != nullptr && res->serviceConfigStatisticsCount == 1);
        OS_REQUIRE(res->serviceConfigStatistics[0].statisticsCount == 1);
        OS_REQUIRE(std::strcmp(res->serviceConfigStatistics[0].statistics[0].value, "98765") == 0);
    }

    XblContextCloseHandle(ctx);
    XUserCloseHandle(user);

    XAsyncBlock cleanup_ab{};
    OS_REQUIRE(XblCleanupAsync(&cleanup_ab) == XBL_S_OK);
    OS_REQUIRE(XAsyncGetStatus(&cleanup_ab, true) == XBL_S_OK);

    std::printf("[PASS] Microsoft.Xbox.Services.GDK.C.dll (XSAPI profile, achievements, presence, social, stats)\n");
}

}  // namespace

int main() {
#if defined(_MSC_VER)
    _putenv_s("OPENSCARLETT_AUDIO_NULL", "1");
#else
    setenv("OPENSCARLETT_AUDIO_NULL", "1", 1);
#endif

    TestXGameRuntime();
    TestGameInput();
    TestXAudio2AndAcpHal();
    TestDirectStorage();
    TestXsapi();

    XGameRuntimeUninitialize();

    std::printf("[PASS] test_gdk_shims: all 6 GDK runtime shims verified end-to-end!\n");
    return 0;
}
