#define OPENSCARLETT_BUILDING_XGAMERUNTIME 1
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <malloc.h>
#endif

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace {

constexpr const char* kModName = "xgameruntime.dll";

inline XGameResult RecordHr(const char* sym, XGameResult hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

std::string GetEnvString(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) {
        return {};
    }
    std::string out(buf);
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

fs::path ResolveSaveRoot() {
    std::string env_root = GetEnvString("OPENSCARLETT_SAVE_ROOT");
    if (!env_root.empty()) {
        return fs::path(env_root);
    }
    std::string env_temp = GetEnvString("TEMP");
    if (!env_temp.empty()) {
        return fs::path(env_temp) / "saves";
    }
    return fs::path("E:\\OpenScarlett\\.tmp\\saves");
}

std::string EncodePathToken(const std::string& raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
            out.push_back(kHex[(c >> 4) & 0xFu]);
            out.push_back(kHex[c & 0xFu]);
        }
    }
    return out.empty() ? "default" : out;
}

std::string FormatXuidHex(uint64_t xuid) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(xuid));
    return std::string(buf);
}

std::string EscapeLine(const std::string& s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '%' || c == '\r' || c == '\n' || c == '\t' || c < 0x20u) {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xFu]);
            out.push_back(kHex[c & 0xFu]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string UnescapeLine(const std::string& s) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool WriteBinaryFileAtomic(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        if (size > 0 && data) {
            ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        ofs.flush();
        if (!ofs.good()) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            if (!ofs) return false;
            if (size > 0 && data) {
                ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            }
            ofs.flush();
            return ofs.good();
        }
    }
    return true;
}

bool ReadBinaryFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const std::streamoff sz = ifs.tellg();
    if (sz < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(out.data()), sz);
        if (!ifs) return false;
    }
    return true;
}

XGameResult CopyStringResult(
    const std::string& src,
    size_t buffer_size,
    char* buffer,
    size_t* used) {
    const size_t needed = src.size() + 1;
    if (used) {
        *used = needed;
    }
    if (!buffer) {
        return XGR_E_INVALIDARG;
    }
    if (buffer_size < needed) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(buffer, src.c_str(), needed);
    return XGR_S_OK;
}

// ===========================================================================
// XTaskQueue Implementation
// ===========================================================================
struct ScheduledCallback {
    SteadyClock::time_point ready_time;
    uint64_t seq = 0;
    XTaskQueueCallback callback = nullptr;
    void* context = nullptr;
};

struct MonitorEntry {
    uint64_t token = 0;
    XTaskQueueMonitorCallback callback = nullptr;
    void* context = nullptr;
};

struct PortState {
    XTaskQueueDispatchMode mode = XTaskQueueDispatchMode_ThreadPool;
    XTaskQueuePort port_id = XTaskQueuePort_Work;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ScheduledCallback> items;
    uint64_t next_seq = 1;
    bool terminating = false;
    bool stop_workers = false;
    uint32_t active_callbacks = 0;
    std::vector<std::thread> workers;
};

}  // namespace

struct XTaskQueueObject {
    std::atomic<int32_t> ref_count{1};
    PortState ports[2];

    std::mutex monitor_mutex;
    uint64_t next_monitor_token = 1;
    std::vector<MonitorEntry> monitors;

    std::mutex terminate_mutex;
    bool terminated = false;
};

namespace {

size_t FindEarliestItemIndexLocked(const PortState& port) {
    size_t best = 0;
    for (size_t i = 1; i < port.items.size(); ++i) {
        if (port.items[i].ready_time < port.items[best].ready_time ||
            (port.items[i].ready_time == port.items[best].ready_time &&
             port.items[i].seq < port.items[best].seq)) {
            best = i;
        }
    }
    return best;
}

void NotifyMonitors(XTaskQueueObject* queue, XTaskQueuePort port) {
    std::vector<MonitorEntry> snapshot;
    {
        std::lock_guard<std::mutex> lk(queue->monitor_mutex);
        snapshot = queue->monitors;
    }
    for (const auto& m : snapshot) {
        if (m.callback) {
            m.callback(m.context, queue, port);
        }
    }
}

void PortWorkerLoop(XTaskQueueObject* queue, uint32_t port_idx) {
    PortState& port = queue->ports[port_idx];
    for (;;) {
        ScheduledCallback item{};
        bool canceled = false;
        {
            std::unique_lock<std::mutex> lk(port.mutex);
            for (;;) {
                if (port.terminating) {
                    if (port.items.empty()) {
                        return;
                    }
                    const size_t idx = FindEarliestItemIndexLocked(port);
                    item = port.items[idx];
                    port.items.erase(port.items.begin() + static_cast<std::ptrdiff_t>(idx));
                    canceled = true;
                    ++port.active_callbacks;
                    break;
                }
                if (port.stop_workers) {
                    return;
                }
                if (port.items.empty()) {
                    port.cv.wait(lk);
                    continue;
                }
                const size_t idx = FindEarliestItemIndexLocked(port);
                const auto now = SteadyClock::now();
                if (port.items[idx].ready_time <= now) {
                    item = port.items[idx];
                    port.items.erase(port.items.begin() + static_cast<std::ptrdiff_t>(idx));
                    canceled = false;
                    ++port.active_callbacks;
                    break;
                }
                port.cv.wait_until(lk, port.items[idx].ready_time);
            }
        }

        if (item.callback) {
            item.callback(item.context, canceled);
        }

        {
            std::lock_guard<std::mutex> lk(port.mutex);
            if (port.active_callbacks > 0) {
                --port.active_callbacks;
            }
            port.cv.notify_all();
        }
    }
}

void StartPortWorkersIfNeeded(XTaskQueueObject* queue, uint32_t port_idx) {
    PortState& port = queue->ports[port_idx];
    uint32_t worker_count = 0;
    if (port.mode == XTaskQueueDispatchMode_ThreadPool) {
        worker_count = 2;
    } else if (port.mode == XTaskQueueDispatchMode_SerializedThreadPool) {
        worker_count = 1;
    }
    for (uint32_t i = 0; i < worker_count; ++i) {
        port.workers.emplace_back(&PortWorkerLoop, queue, port_idx);
    }
}

void ShutdownQueueInternal(XTaskQueueObject* queue, bool wait_for_completion) {
    if (!queue) return;

    // Drain manual/immediate pending callbacks as canceled and wake worker ports.
    std::vector<ScheduledCallback> manual_canceled;
    for (uint32_t p = 0; p < 2; ++p) {
        PortState& port = queue->ports[p];
        std::lock_guard<std::mutex> lk(port.mutex);
        port.terminating = true;
        if (port.workers.empty()) {
            while (!port.items.empty()) {
                const size_t idx = FindEarliestItemIndexLocked(port);
                manual_canceled.push_back(port.items[idx]);
                port.items.erase(port.items.begin() + static_cast<std::ptrdiff_t>(idx));
            }
        }
        port.cv.notify_all();
    }

    for (const auto& cb : manual_canceled) {
        if (cb.callback) {
            cb.callback(cb.context, true);
        }
    }

    if (wait_for_completion) {
        for (uint32_t p = 0; p < 2; ++p) {
            PortState& port = queue->ports[p];
            std::vector<std::thread> to_join;
            {
                std::lock_guard<std::mutex> lk(port.mutex);
                port.stop_workers = true;
                port.cv.notify_all();
                to_join.swap(port.workers);
            }
            for (auto& t : to_join) {
                if (t.joinable()) {
                    if (t.get_id() == std::this_thread::get_id()) {
                        t.detach();
                    } else {
                        t.join();
                    }
                }
            }
        }
    }
}

// ===========================================================================
// Global Runtime State (Queues, XAsync, XUser, XGameSave, XMem)
// ===========================================================================
struct AsyncRecord {
    uint64_t id = 0;
    XAsyncBlock* async_block = nullptr;
    XTaskQueueHandle queue = nullptr;
    void* provider_context = nullptr;
    const void* identity = nullptr;
    std::string identity_name;
    XAsyncProvider provider = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    XGameResult result = XGR_E_PENDING;
    size_t required_buffer_size = 0;

    std::atomic<bool> complete_called{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> cleaned_up{false};
};

constexpr uint64_t kAsyncSlotMagic = 0x4F534153594E4331ULL;  // "OSASYNC1"

struct AsyncSlot {
    uint64_t magic;
    uint64_t id;
};
static_assert(sizeof(AsyncSlot) <= sizeof(XAsyncBlock::internal),
              "AsyncSlot must fit in XAsyncBlock::internal");

}  // namespace

struct XUser {
    std::atomic<int32_t> ref_count{1};
    uint64_t xuid = 0;
    uint64_t local_id = 1;
    std::string gamertag_classic;
    std::string gamertag_modern;
    std::string gamertag_suffix;
    std::string gamertag_unique;
};

struct XGameSaveProvider {
    uint64_t xuid = 0;
    std::string scid;
    fs::path root_dir;
};

struct XGameSaveContainer {
    std::string name;
    fs::path container_dir;
};

struct XGameSaveUpdate {
    struct PendingOp {
        bool is_delete = false;
        std::string blob_name;
        std::vector<uint8_t> data;
    };

    std::string container_name;
    std::string display_name;
    fs::path container_dir;
    std::vector<PendingOp> ops;
};

struct XDisplayTimeoutDeferral {
    std::atomic<uint32_t> active{1};
};

namespace {

struct RuntimeState {
    std::mutex mutex;
    std::atomic<int32_t> init_count{0};

    // Task queues
    std::unordered_map<XTaskQueueObject*, std::shared_ptr<XTaskQueueObject>> live_queues;
    XTaskQueueHandle process_queue = nullptr;

    // XAsync records
    std::mutex async_mutex;
    uint64_t next_async_id = 1;
    std::unordered_map<uint64_t, std::shared_ptr<AsyncRecord>> async_records;

    // XUser handles
    std::mutex user_mutex;
    std::unordered_map<XUser*, std::shared_ptr<XUser>> live_users;

    // XGameSave handles
    std::mutex save_mutex;
    std::unordered_map<XGameSaveProvider*, std::shared_ptr<XGameSaveProvider>> live_providers;
    std::unordered_map<XGameSaveContainer*, std::shared_ptr<XGameSaveContainer>> live_containers;
    std::unordered_map<XGameSaveUpdate*, std::shared_ptr<XGameSaveUpdate>> live_updates;

    // XMem hooks
    std::mutex mem_mutex;
    XMemAllocFunction alloc_hook = nullptr;
    XMemFreeFunction free_hook = nullptr;
};

RuntimeState& GetRuntime() {
    static RuntimeState* s = new RuntimeState();
    return *s;
}

std::shared_ptr<XTaskQueueObject> LookupQueue(XTaskQueueHandle handle) {
    if (!handle) return nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.mutex);
    auto it = rt.live_queues.find(handle);
    return it == rt.live_queues.end() ? nullptr : it->second;
}

std::shared_ptr<AsyncRecord> LookupAsync(const XAsyncBlock* block) {
    if (!block) return nullptr;
    AsyncSlot slot{};
    std::memcpy(&slot, block->internal, sizeof(slot));
    if (slot.magic != kAsyncSlotMagic || slot.id == 0) {
        return nullptr;
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.async_mutex);
    auto it = rt.async_records.find(slot.id);
    return it == rt.async_records.end() ? nullptr : it->second;
}

void CleanupAsyncRecordOnce(const std::shared_ptr<AsyncRecord>& rec) {
    if (!rec) return;
    if (!rec->cleaned_up.exchange(true, std::memory_order_acq_rel)) {
        if (rec->provider) {
            XAsyncProviderData data{};
            data.async = rec->async_block;
            data.context = rec->provider_context;
            rec->provider(XAsyncOp_Cleanup, &data);
        }
        if (rec->queue) {
            XTaskQueueCloseHandle(rec->queue);
            rec->queue = nullptr;
        }
    }
}

struct AsyncTrampolineCtx {
    std::shared_ptr<AsyncRecord> rec;
};

void OPENSCARLETT_GDK_CALL AsyncCompletionTrampoline(void* context, bool /*canceled*/) {
    std::unique_ptr<AsyncTrampolineCtx> holder(static_cast<AsyncTrampolineCtx*>(context));
    auto rec = holder->rec;

    XAsyncCompletionRoutine cb = nullptr;
    XAsyncBlock* block = nullptr;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        block = rec->async_block;
        cb = block ? block->callback : nullptr;
    }

    if (cb && block) {
        cb(block);
    }

    bool should_auto_cleanup = false;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        rec->completed = true;
        should_auto_cleanup = (rec->required_buffer_size == 0 || rec->result < 0);
    }
    rec->cv.notify_all();

    if (should_auto_cleanup) {
        CleanupAsyncRecordOnce(rec);
    }
}

void OPENSCARLETT_GDK_CALL AsyncWorkTrampoline(void* context, bool canceled) {
    std::unique_ptr<AsyncTrampolineCtx> holder(static_cast<AsyncTrampolineCtx*>(context));
    auto rec = holder->rec;

    if (canceled || rec->canceled.load(std::memory_order_acquire)) {
        XAsyncComplete(rec->async_block, XGR_E_ABORT, 0);
        return;
    }

    XAsyncProviderData data{};
    data.async = rec->async_block;
    data.context = rec->provider_context;
    const XGameResult hr = rec->provider ? rec->provider(XAsyncOp_DoWork, &data) : XGR_E_FAIL;
    if (hr != XGR_E_PENDING && !rec->complete_called.load(std::memory_order_acquire)) {
        XAsyncComplete(rec->async_block, hr, 0);
    }
}

// ===========================================================================
// XGameSave Metadata Helpers
// ===========================================================================
struct ContainerMeta {
    std::string name;
    std::string display_name;
    int64_t last_modified = 0;
    // original blob name -> filename on disk (relative to container_dir)
    std::map<std::string, std::string> blobs;
};

bool LoadContainerMeta(const fs::path& container_dir, ContainerMeta& out) {
    std::ifstream ifs(container_dir / "_meta.txt", std::ios::binary);
    if (!ifs) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("name=", 0) == 0) {
            out.name = UnescapeLine(line.substr(5));
        } else if (line.rfind("display=", 0) == 0) {
            out.display_name = UnescapeLine(line.substr(8));
        } else if (line.rfind("mtime=", 0) == 0) {
            try {
                out.last_modified = std::stoll(line.substr(6));
            } catch (...) {
                out.last_modified = 0;
            }
        } else if (line.rfind("blob=", 0) == 0) {
            std::string rest = line.substr(5);
            const size_t tab = rest.find('\t');
            if (tab != std::string::npos) {
                std::string file_name = rest.substr(0, tab);
                std::string blob_name = UnescapeLine(rest.substr(tab + 1));
                out.blobs[blob_name] = file_name;
            }
        }
    }
    return !out.name.empty();
}

bool SaveContainerMeta(const fs::path& container_dir, const ContainerMeta& meta) {
    std::ostringstream oss;
    oss << "name=" << EscapeLine(meta.name) << "\n";
    oss << "display=" << EscapeLine(meta.display_name) << "\n";
    oss << "mtime=" << meta.last_modified << "\n";
    for (const auto& [blob_name, file_name] : meta.blobs) {
        oss << "blob=" << file_name << "\t" << EscapeLine(blob_name) << "\n";
    }
    const std::string text = oss.str();
    return WriteBinaryFileAtomic(
        container_dir / "_meta.txt",
        reinterpret_cast<const uint8_t*>(text.data()),
        text.size());
}

std::shared_ptr<XUser> LookupUser(XUserHandle handle) {
    if (!handle) return nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.user_mutex);
    auto it = rt.live_users.find(handle);
    return it == rt.live_users.end() ? nullptr : it->second;
}

uint64_t DefaultUserXuid() {
    std::string env = GetEnvString("OPENSCARLETT_XUID");
    if (!env.empty()) {
        try {
            return std::stoull(env, nullptr, 0);
        } catch (...) {
        }
    }
    return 0x0009000012345678ULL;
}

std::shared_ptr<XUser> CreateDefaultUserObj() {
    auto u = std::make_shared<XUser>();
    u->xuid = DefaultUserXuid();
    u->local_id = 1;
    std::string env_gt = GetEnvString("OPENSCARLETT_GAMERTAG");
    if (!env_gt.empty()) {
        u->gamertag_classic = env_gt;
        u->gamertag_modern = env_gt;
    } else {
        u->gamertag_classic = "OpenScarlettUser";
        u->gamertag_modern = "OpenScarlett";
    }
    u->gamertag_suffix = "1001";
    u->gamertag_unique = u->gamertag_modern + "#" + u->gamertag_suffix;
    return u;
}

// ===========================================================================
// Async Provider Helpers for XUserAddAsync & XGameSaveInitializeProviderAsync
// ===========================================================================
const char kIdUserAdd[] = "XUserAddAsync";
const char kIdGameSaveInit[] = "XGameSaveInitializeProviderAsync";

struct UserAddAsyncCtx {
    XUserHandle user = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL UserAddProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<UserAddAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            auto u = CreateDefaultUserObj();
            XUserHandle raw = u.get();
            {
                auto& rt = GetRuntime();
                std::lock_guard<std::mutex> lk(rt.user_mutex);
                rt.live_users[raw] = std::move(u);
            }
            ctx->user = raw;
            XAsyncComplete(data->async, XGR_S_OK, sizeof(XUserHandle));
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(XUserHandle)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            XUserHandle h = ctx->user;
            ctx->user = nullptr;
            std::memcpy(data->buffer, &h, sizeof(h));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            if (ctx->user) {
                XUserCloseHandle(ctx->user);
                ctx->user = nullptr;
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameSaveInitAsyncCtx {
    XUserHandle requesting_user = nullptr;
    std::string scid;
    bool sync_on_demand = false;
    XGameSaveProviderHandle provider = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL GameSaveInitProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameSaveInitAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            XGameSaveProviderHandle p = nullptr;
            XGameResult hr = XGameSaveInitializeProvider(
                ctx->requesting_user,
                ctx->scid.c_str(),
                ctx->sync_on_demand,
                &p);
            ctx->provider = p;
            XAsyncComplete(data->async, hr, hr >= 0 ? sizeof(XGameSaveProviderHandle) : 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(XGameSaveProviderHandle)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            XGameSaveProviderHandle p = ctx->provider;
            ctx->provider = nullptr;
            std::memcpy(data->buffer, &p, sizeof(p));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            if (ctx->provider) {
                XGameSaveCloseProvider(ctx->provider);
                ctx->provider = nullptr;
            }
            if (ctx->requesting_user) {
                XUserCloseHandle(ctx->requesting_user);
                ctx->requesting_user = nullptr;
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

}  // namespace

extern "C" {

// ===========================================================================
// Game Runtime Lifecycle
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XGameRuntimeInitialize(void) {
    auto& rt = GetRuntime();
    rt.init_count.fetch_add(1, std::memory_order_acq_rel);
    return RecordHr("XGameRuntimeInitialize", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XGameRuntimeUninitialize(void) {
    auto& rt = GetRuntime();
    const int32_t prev = rt.init_count.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 1) {
        rt.init_count.store(0, std::memory_order_release);
        XTaskQueueHandle proc_q = nullptr;
        {
            std::lock_guard<std::mutex> lk(rt.mutex);
            proc_q = rt.process_queue;
            rt.process_queue = nullptr;
        }
        if (proc_q) {
            XTaskQueueTerminate(proc_q, true, nullptr, nullptr);
            XTaskQueueCloseHandle(proc_q);
        }
        // Clean up any unclaimed async records.
        std::vector<std::shared_ptr<AsyncRecord>> records;
        {
            std::lock_guard<std::mutex> lk(rt.async_mutex);
            for (auto& [_, r] : rt.async_records) {
                records.push_back(r);
            }
            rt.async_records.clear();
        }
        for (auto& r : records) {
            CleanupAsyncRecordOnce(r);
        }
    }
    RecordOk("XGameRuntimeUninitialize");
}

XGameResult OPENSCARLETT_GDK_CALL XGameGetXboxTitleId(uint32_t* titleId) {
    if (!titleId) {
        return RecordHr("XGameGetXboxTitleId", XGR_E_INVALIDARG);
    }
    uint32_t tid = 0x5CA00001u;
    std::string env = GetEnvString("OPENSCARLETT_TITLE_ID");
    if (!env.empty()) {
        try {
            tid = static_cast<uint32_t>(std::stoul(env, nullptr, 0));
        } catch (...) {
        }
    }
    *titleId = tid;
    return RecordHr("XGameGetXboxTitleId", XGR_S_OK);
}

// ===========================================================================
// XTaskQueue
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue) {
    if (!queue) {
        return RecordHr("XTaskQueueCreate", XGR_E_INVALIDARG);
    }
    *queue = nullptr;

    auto obj = std::make_shared<XTaskQueueObject>();
    obj->ports[0].mode = workDispatchMode;
    obj->ports[0].port_id = XTaskQueuePort_Work;
    obj->ports[1].mode = completionDispatchMode;
    obj->ports[1].port_id = XTaskQueuePort_Completion;

    XTaskQueueHandle raw = obj.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        rt.live_queues[raw] = obj;
    }

    StartPortWorkersIfNeeded(raw, 0);
    StartPortWorkersIfNeeded(raw, 1);

    *queue = raw;
    return RecordHr("XTaskQueueCreate", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle) {
    if (!queueHandle || !duplicatedHandle) {
        return RecordHr("XTaskQueueDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto obj = LookupQueue(queueHandle);
    if (!obj) {
        return RecordHr("XTaskQueueDuplicateHandle", XGR_E_HANDLE);
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = queueHandle;
    return RecordHr("XTaskQueueDuplicateHandle", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    RecordOk("XTaskQueueCloseHandle");
    if (!queue) return;

    std::shared_ptr<XTaskQueueObject> dropped;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        auto it = rt.live_queues.find(queue);
        if (it == rt.live_queues.end()) {
            return;
        }
        if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dropped = std::move(it->second);
            rt.live_queues.erase(it);
        }
    }
    if (dropped) {
        ShutdownQueueInternal(dropped.get(), true);
    }
}

bool OPENSCARLETT_GDK_CALL XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue) {
    if (!queue) {
        RecordHr("XTaskQueueGetCurrentProcessTaskQueue", XGR_E_INVALIDARG);
        return false;
    }
    *queue = nullptr;

    XTaskQueueHandle existing = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        existing = rt.process_queue;
    }
    if (!existing) {
        XTaskQueueHandle created = nullptr;
        if (XTaskQueueCreate(
                XTaskQueueDispatchMode_ThreadPool,
                XTaskQueueDispatchMode_ThreadPool,
                &created) < 0) {
            RecordHr("XTaskQueueGetCurrentProcessTaskQueue", XGR_E_FAIL);
            return false;
        }
        XTaskQueueHandle discard = nullptr;
        {
            auto& rt = GetRuntime();
            std::lock_guard<std::mutex> lk(rt.mutex);
            if (!rt.process_queue) {
                rt.process_queue = created;
                existing = created;
            } else {
                existing = rt.process_queue;
                discard = created;
            }
        }
        if (discard) {
            XTaskQueueCloseHandle(discard);
        }
    }

    const XGameResult hr = XTaskQueueDuplicateHandle(existing, queue);
    RecordHr("XTaskQueueGetCurrentProcessTaskQueue", hr);
    return hr >= 0;
}

void OPENSCARLETT_GDK_CALL XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue) {
    XTaskQueueHandle dup = nullptr;
    if (queue) {
        if (XTaskQueueDuplicateHandle(queue, &dup) < 0) {
            RecordHr("XTaskQueueSetCurrentProcessTaskQueue", XGR_E_HANDLE);
            return;
        }
    }
    XTaskQueueHandle old_q = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        old_q = rt.process_queue;
        rt.process_queue = dup;
    }
    if (old_q) {
        XTaskQueueCloseHandle(old_q);
    }
    RecordOk("XTaskQueueSetCurrentProcessTaskQueue");
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback callback) {
    if (!queue || !callback || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) {
        return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_INVALIDARG);
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_HANDLE);
    }

    PortState& p = obj->ports[static_cast<uint32_t>(port)];

    if (p.mode == XTaskQueueDispatchMode_Immediate) {
        {
            std::lock_guard<std::mutex> lk(p.mutex);
            if (p.terminating) {
                return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_ABORT);
            }
        }
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        callback(callbackContext, false);
        return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_S_OK);
    }

    {
        std::lock_guard<std::mutex> lk(p.mutex);
        if (p.terminating) {
            return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_ABORT);
        }
        ScheduledCallback item{};
        item.ready_time = SteadyClock::now() + std::chrono::milliseconds(delayMs);
        item.seq = p.next_seq++;
        item.callback = callback;
        item.context = callbackContext;
        p.items.push_back(item);
        p.cv.notify_one();
    }

    NotifyMonitors(obj.get(), port);
    return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback callback) {
    const XGameResult hr = XTaskQueueSubmitDelayedCallback(queue, port, 0, callbackContext, callback);
    return RecordHr("XTaskQueueSubmitCallback", hr);
}

bool OPENSCARLETT_GDK_CALL XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs) {
    if (!queue || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) {
        RecordHr("XTaskQueueDispatch", XGR_E_INVALIDARG);
        return false;
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        RecordHr("XTaskQueueDispatch", XGR_E_HANDLE);
        return false;
    }

    PortState& p = obj->ports[static_cast<uint32_t>(port)];
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeoutInMs);

    ScheduledCallback item{};
    bool has_item = false;
    bool canceled = false;

    {
        std::unique_lock<std::mutex> lk(p.mutex);
        for (;;) {
            if (!p.items.empty()) {
                const size_t idx = FindEarliestItemIndexLocked(p);
                const auto now = SteadyClock::now();
                if (p.terminating || p.items[idx].ready_time <= now) {
                    item = p.items[idx];
                    p.items.erase(p.items.begin() + static_cast<std::ptrdiff_t>(idx));
                    canceled = p.terminating;
                    has_item = true;
                    ++p.active_callbacks;
                    break;
                }
                if (timeoutInMs == 0 || now >= deadline) {
                    break;
                }
                const auto wait_until_tp = std::min(deadline, p.items[idx].ready_time);
                p.cv.wait_until(lk, wait_until_tp);
                continue;
            }
            if (p.terminating || timeoutInMs == 0 || SteadyClock::now() >= deadline) {
                break;
            }
            p.cv.wait_until(lk, deadline);
        }
    }

    if (has_item && item.callback) {
        item.callback(item.context, canceled);
        std::lock_guard<std::mutex> lk(p.mutex);
        if (p.active_callbacks > 0) {
            --p.active_callbacks;
        }
        p.cv.notify_all();
    }

    RecordOk("XTaskQueueDispatch");
    return has_item;
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback callback) {
    if (!queue) {
        return RecordHr("XTaskQueueTerminate", XGR_E_INVALIDARG);
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueTerminate", XGR_E_HANDLE);
    }

    {
        std::lock_guard<std::mutex> lk(obj->terminate_mutex);
        obj->terminated = true;
    }

    ShutdownQueueInternal(obj.get(), wait);

    if (callback) {
        callback(callbackContext);
    }
    return RecordHr("XTaskQueueTerminate", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!queue || !callback || !token) {
        return RecordHr("XTaskQueueRegisterMonitor", XGR_E_INVALIDARG);
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueRegisterMonitor", XGR_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->monitor_mutex);
    const uint64_t tok = obj->next_monitor_token++;
    obj->monitors.push_back(MonitorEntry{tok, callback, callbackContext});
    token->token = tok;
    return RecordHr("XTaskQueueRegisterMonitor", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token) {
    RecordOk("XTaskQueueUnregisterMonitor");
    if (!queue || token.token == 0) return;
    auto obj = LookupQueue(queue);
    if (!obj) return;
    std::lock_guard<std::mutex> lk(obj->monitor_mutex);
    auto& m = obj->monitors;
    m.erase(
        std::remove_if(m.begin(), m.end(),
                       [&](const MonitorEntry& e) { return e.token == token.token; }),
        m.end());
}

// ===========================================================================
// XAsync
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider provider) {
    if (!asyncBlock || !provider) {
        return RecordHr("XAsyncBegin", XGR_E_INVALIDARG);
    }

    // Acquire queue handle for this operation.
    XTaskQueueHandle q = nullptr;
    if (asyncBlock->queue) {
        if (XTaskQueueDuplicateHandle(asyncBlock->queue, &q) < 0) {
            return RecordHr("XAsyncBegin", XGR_E_HANDLE);
        }
    } else {
        if (!XTaskQueueGetCurrentProcessTaskQueue(&q)) {
            return RecordHr("XAsyncBegin", XGR_E_FAIL);
        }
    }

    auto rec = std::make_shared<AsyncRecord>();
    rec->async_block = asyncBlock;
    rec->queue = q;
    rec->provider_context = context;
    rec->identity = identity;
    rec->identity_name = identityName ? identityName : "";
    rec->provider = provider;

    // Register record & write slot into asyncBlock->internal.
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.async_mutex);
        rec->id = rt.next_async_id++;
        rt.async_records[rec->id] = rec;
    }

    AsyncSlot slot{kAsyncSlotMagic, rec->id};
    std::memset(asyncBlock->internal, 0, sizeof(asyncBlock->internal));
    std::memcpy(asyncBlock->internal, &slot, sizeof(slot));

    XAsyncProviderData data{};
    data.async = asyncBlock;
    data.context = context;
    const XGameResult hr = provider(XAsyncOp_Begin, &data);
    if (hr < 0) {
        {
            auto& rt = GetRuntime();
            std::lock_guard<std::mutex> lk(rt.async_mutex);
            rt.async_records.erase(rec->id);
        }
        if (rec->queue) {
            XTaskQueueCloseHandle(rec->queue);
            rec->queue = nullptr;
        }
        std::memset(asyncBlock->internal, 0, sizeof(asyncBlock->internal));
        return RecordHr("XAsyncBegin", hr);
    }

    return RecordHr("XAsyncBegin", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncSchedule(
    XAsyncBlock* asyncBlock,
    uint32_t delayInMs) {
    auto rec = LookupAsync(asyncBlock);
    if (!rec || !rec->queue) {
        return RecordHr("XAsyncSchedule", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) AsyncTrampolineCtx{rec};
    if (!ctx) {
        return RecordHr("XAsyncSchedule", XGR_E_OUTOFMEMORY);
    }
    const XGameResult hr = XTaskQueueSubmitDelayedCallback(
        rec->queue, XTaskQueuePort_Work, delayInMs, ctx, &AsyncWorkTrampoline);
    if (hr < 0) {
        delete ctx;
    }
    return RecordHr("XAsyncSchedule", hr);
}

void OPENSCARLETT_GDK_CALL XAsyncComplete(
    XAsyncBlock* asyncBlock,
    XGameResult result,
    size_t requiredBufferSize) {
    RecordOk("XAsyncComplete");
    auto rec = LookupAsync(asyncBlock);
    if (!rec) return;

    if (rec->complete_called.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    XTaskQueueHandle q = nullptr;
    bool has_callback = false;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        rec->result = result;
        rec->required_buffer_size = requiredBufferSize;
        has_callback = (rec->async_block && rec->async_block->callback != nullptr);
        q = rec->queue;
    }

    if (has_callback && q) {
        auto* ctx = new (std::nothrow) AsyncTrampolineCtx{rec};
        if (ctx) {
            if (XTaskQueueSubmitCallback(
                    q, XTaskQueuePort_Completion, ctx, &AsyncCompletionTrampoline) >= 0) {
                return;
            }
            delete ctx;
        }
    }

    bool should_auto_cleanup = false;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        rec->completed = true;
        should_auto_cleanup = (rec->required_buffer_size == 0 || rec->result < 0);
    }
    rec->cv.notify_all();

    if (should_auto_cleanup) {
        CleanupAsyncRecordOnce(rec);
    }
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncGetStatus(
    XAsyncBlock* asyncBlock,
    bool wait) {
    auto rec = LookupAsync(asyncBlock);
    if (!rec) {
        return RecordHr("XAsyncGetStatus", XGR_E_INVALIDARG);
    }

    std::unique_lock<std::mutex> lk(rec->mutex);
    if (wait) {
        rec->cv.wait(lk, [&]() { return rec->completed; });
    }
    const XGameResult hr = rec->completed ? rec->result : XGR_E_PENDING;
    return RecordHr("XAsyncGetStatus", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResultSize(
    XAsyncBlock* asyncBlock,
    size_t* bufferSize) {
    if (!asyncBlock || !bufferSize) {
        return RecordHr("XAsyncGetResultSize", XGR_E_INVALIDARG);
    }
    auto rec = LookupAsync(asyncBlock);
    if (!rec) {
        return RecordHr("XAsyncGetResultSize", XGR_E_INVALIDARG);
    }
    std::lock_guard<std::mutex> lk(rec->mutex);
    if (!rec->completed) {
        return RecordHr("XAsyncGetResultSize", XGR_E_PENDING);
    }
    if (rec->result < 0) {
        return RecordHr("XAsyncGetResultSize", rec->result);
    }
    *bufferSize = rec->required_buffer_size;
    return RecordHr("XAsyncGetResultSize", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) {
    auto rec = LookupAsync(asyncBlock);
    if (!rec) {
        return RecordHr("XAsyncGetResult", XGR_E_INVALIDARG);
    }

    XGameResult status = XGR_S_OK;
    size_t req_size = 0;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        if (!rec->completed) {
            return RecordHr("XAsyncGetResult", XGR_E_PENDING);
        }
        status = rec->result;
        req_size = rec->required_buffer_size;
    }

    if (identity != nullptr && rec->identity != identity) {
        return RecordHr("XAsyncGetResult", XGR_E_INVALIDARG);
    }
    if (status < 0) {
        CleanupAsyncRecordOnce(rec);
        return RecordHr("XAsyncGetResult", status);
    }

    if (req_size == 0) {
        if (bufferUsed) *bufferUsed = 0;
        CleanupAsyncRecordOnce(rec);
        return RecordHr("XAsyncGetResult", XGR_S_OK);
    }

    if (!buffer) {
        return RecordHr("XAsyncGetResult", XGR_E_INVALIDARG);
    }
    if (bufferSize < req_size) {
        return RecordHr("XAsyncGetResult", XGR_E_NOT_SUFFICIENT_BUFFER);
    }

    XAsyncProviderData data{};
    data.async = asyncBlock;
    data.buffer = buffer;
    data.bufferSize = bufferSize;
    data.context = rec->provider_context;
    const XGameResult hr = rec->provider ? rec->provider(XAsyncOp_GetResult, &data) : XGR_E_FAIL;
    if (hr >= 0 && bufferUsed) {
        *bufferUsed = req_size;
    }
    if (hr >= 0) {
        CleanupAsyncRecordOnce(rec);
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.async_mutex);
        rt.async_records.erase(rec->id);
    }
    return RecordHr("XAsyncGetResult", hr);
}

void OPENSCARLETT_GDK_CALL XAsyncCancel(XAsyncBlock* asyncBlock) {
    RecordOk("XAsyncCancel");
    auto rec = LookupAsync(asyncBlock);
    if (!rec) return;
    rec->canceled.store(true, std::memory_order_release);
    if (rec->provider) {
        XAsyncProviderData data{};
        data.async = asyncBlock;
        data.context = rec->provider_context;
        rec->provider(XAsyncOp_Cancel, &data);
    }
}

// ===========================================================================
// XUser
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XUserAddAsync(
    XUserAddOptions /*options*/,
    XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XUserAddAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) UserAddAsyncCtx();
    if (!ctx) {
        return RecordHr("XUserAddAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(async, ctx, kIdUserAdd, kIdUserAdd, &UserAddProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XUserAddAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XUserAddAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser) {
    if (!async || !newUser) {
        return RecordHr("XUserAddResult", XGR_E_INVALIDARG);
    }
    *newUser = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserAddResult", hr);
    }
    hr = XAsyncGetResult(async, kIdUserAdd, sizeof(XUserHandle), newUser, nullptr);
    return RecordHr("XUserAddResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return RecordHr("XUserDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto u = LookupUser(handle);
    if (!u) {
        return RecordHr("XUserDuplicateHandle", XGR_E_HANDLE);
    }
    u->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return RecordHr("XUserDuplicateHandle", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XUserCloseHandle(XUserHandle user) {
    RecordOk("XUserCloseHandle");
    if (!user) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.user_mutex);
    auto it = rt.live_users.find(user);
    if (it == rt.live_users.end()) return;
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        rt.live_users.erase(it);
    }
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetId(XUserHandle user, uint64_t* userId) {
    if (!user || !userId) {
        return RecordHr("XUserGetId", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetId", XGR_E_HANDLE);
    }
    *userId = u->xuid;
    return RecordHr("XUserGetId", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId) {
    if (!user || !userLocalId) {
        return RecordHr("XUserGetLocalId", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetLocalId", XGR_E_HANDLE);
    }
    userLocalId->value = u->local_id;
    return RecordHr("XUserGetLocalId", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed) {
    if (!user) {
        return RecordHr("XUserGetGamertag", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetGamertag", XGR_E_HANDLE);
    }
    const std::string* src = &u->gamertag_classic;
    switch (gamertagComponent) {
        case XUserGamertagComponent_Classic: src = &u->gamertag_classic; break;
        case XUserGamertagComponent_Modern: src = &u->gamertag_modern; break;
        case XUserGamertagComponent_ModernSuffix: src = &u->gamertag_suffix; break;
        case XUserGamertagComponent_UniqueModern: src = &u->gamertag_unique; break;
        default: return RecordHr("XUserGetGamertag", XGR_E_INVALIDARG);
    }
    return RecordHr("XUserGetGamertag", CopyStringResult(*src, gamertagSize, gamertag, gamertagUsed));
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetState(XUserHandle user, XUserState* state) {
    if (!user || !state) {
        return RecordHr("XUserGetState", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserGetState", XGR_E_HANDLE);
    }
    *state = XUserState_SignedIn;
    return RecordHr("XUserGetState", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* ageGroup) {
    if (!user || !ageGroup) {
        return RecordHr("XUserGetAgeGroup", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserGetAgeGroup", XGR_E_HANDLE);
    }
    *ageGroup = XUserAgeGroup_Adult;
    return RecordHr("XUserGetAgeGroup", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege /*privilege*/,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason) {
    if (!user || !hasPrivilege) {
        return RecordHr("XUserCheckPrivilege", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserCheckPrivilege", XGR_E_HANDLE);
    }
    *hasPrivilege = true;
    if (reason) {
        *reason = XUserPrivilegeDenyReason_None;
    }
    return RecordHr("XUserCheckPrivilege", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetMaxUsers(uint32_t* maxUsers) {
    if (!maxUsers) {
        return RecordHr("XUserGetMaxUsers", XGR_E_INVALIDARG);
    }
    *maxUsers = 16u;
    return RecordHr("XUserGetMaxUsers", XGR_S_OK);
}

int32_t OPENSCARLETT_GDK_CALL XUserCompare(XUserHandle user1, XUserHandle user2) {
    RecordOk("XUserCompare");
    if (user1 == user2) return 0;
    auto u1 = LookupUser(user1);
    auto u2 = LookupUser(user2);
    const uint64_t id1 = u1 ? u1->xuid : 0;
    const uint64_t id2 = u2 ? u2->xuid : 0;
    if (id1 < id2) return -1;
    if (id1 > id2) return 1;
    return 0;
}

// ===========================================================================
// XGameSave (persistent disk storage under OPENSCARLETT_SAVE_ROOT or E:\)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool /*syncOnDemand*/,
    XGameSaveProviderHandle* provider) {
    if (!provider || !configurationId || !*configurationId) {
        return RecordHr("XGameSaveInitializeProvider", XGR_E_INVALIDARG);
    }
    *provider = nullptr;

    uint64_t xuid = DefaultUserXuid();
    if (requestingUser) {
        auto u = LookupUser(requestingUser);
        if (!u) {
            return RecordHr("XGameSaveInitializeProvider", XGR_E_HANDLE);
        }
        xuid = u->xuid;
    }

    auto p = std::make_shared<XGameSaveProvider>();
    p->xuid = xuid;
    p->scid = configurationId;
    p->root_dir = ResolveSaveRoot() / "xgamesave" / EncodePathToken(p->scid) / FormatXuidHex(xuid);

    std::error_code ec;
    fs::create_directories(p->root_dir, ec);
    if (ec) {
        return RecordHr("XGameSaveInitializeProvider", XGR_E_FAIL);
    }

    XGameSaveProviderHandle raw = p.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        rt.live_providers[raw] = std::move(p);
    }
    *provider = raw;
    return RecordHr("XGameSaveInitializeProvider", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async) {
    if (!async || !configurationId || !*configurationId) {
        return RecordHr("XGameSaveInitializeProviderAsync", XGR_E_INVALIDARG);
    }
    XUserHandle dup_user = nullptr;
    if (requestingUser) {
        XGameResult hr = XUserDuplicateHandle(requestingUser, &dup_user);
        if (hr < 0) {
            return RecordHr("XGameSaveInitializeProviderAsync", hr);
        }
    }
    auto* ctx = new (std::nothrow) GameSaveInitAsyncCtx();
    if (!ctx) {
        if (dup_user) XUserCloseHandle(dup_user);
        return RecordHr("XGameSaveInitializeProviderAsync", XGR_E_OUTOFMEMORY);
    }
    ctx->requesting_user = dup_user;
    ctx->scid = configurationId;
    ctx->sync_on_demand = syncOnDemand;

    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameSaveInit, kIdGameSaveInit, &GameSaveInitProvider);
    if (hr < 0) {
        if (ctx->requesting_user) XUserCloseHandle(ctx->requesting_user);
        delete ctx;
        return RecordHr("XGameSaveInitializeProviderAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameSaveInitializeProviderAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider) {
    if (!async || !provider) {
        return RecordHr("XGameSaveInitializeProviderResult", XGR_E_INVALIDARG);
    }
    *provider = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameSaveInitializeProviderResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdGameSaveInit, sizeof(XGameSaveProviderHandle), provider, nullptr);
    return RecordHr("XGameSaveInitializeProviderResult", hr);
}

void OPENSCARLETT_GDK_CALL XGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    RecordOk("XGameSaveCloseProvider");
    if (!provider) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    rt.live_providers.erase(provider);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* result) {
    if (!provider || !name || !*name || !result) {
        return RecordHr("XGameSaveCreateContainer", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    std::shared_ptr<XGameSaveProvider> prov;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveCreateContainer", XGR_E_HANDLE);
        }
        prov = it->second;
    }

    auto c = std::make_shared<XGameSaveContainer>();
    c->name = name;
    c->container_dir = prov->root_dir / EncodePathToken(c->name);

    XGameSaveContainerHandle raw = c.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        rt.live_containers[raw] = std::move(c);
    }
    *result = raw;
    return RecordHr("XGameSaveCreateContainer", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XGameSaveCloseContainer(XGameSaveContainerHandle context) {
    RecordOk("XGameSaveCloseContainer");
    if (!context) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    rt.live_containers.erase(context);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* result) {
    if (!container || !result) {
        return RecordHr("XGameSaveCreateUpdate", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    std::shared_ptr<XGameSaveContainer> c;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveCreateUpdate", XGR_E_HANDLE);
        }
        c = it->second;
    }

    auto upd = std::make_shared<XGameSaveUpdate>();
    upd->container_name = c->name;
    upd->display_name = (containerDisplayName && *containerDisplayName)
                            ? containerDisplayName
                            : c->name;
    upd->container_dir = c->container_dir;

    XGameSaveUpdateHandle raw = upd.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        rt.live_updates[raw] = std::move(upd);
    }
    *result = raw;
    return RecordHr("XGameSaveCreateUpdate", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount) {
    if (!updateContext || !blobName || !*blobName || (byteCount > 0 && !data)) {
        return RecordHr("XGameSaveSubmitBlobWrite", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    auto it = rt.live_updates.find(updateContext);
    if (it == rt.live_updates.end()) {
        return RecordHr("XGameSaveSubmitBlobWrite", XGR_E_HANDLE);
    }
    XGameSaveUpdate::PendingOp op{};
    op.is_delete = false;
    op.blob_name = blobName;
    if (byteCount > 0) {
        op.data.assign(data, data + byteCount);
    }
    it->second->ops.push_back(std::move(op));
    return RecordHr("XGameSaveSubmitBlobWrite", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName) {
    if (!updateContext || !blobName || !*blobName) {
        return RecordHr("XGameSaveSubmitBlobDelete", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    auto it = rt.live_updates.find(updateContext);
    if (it == rt.live_updates.end()) {
        return RecordHr("XGameSaveSubmitBlobDelete", XGR_E_HANDLE);
    }
    XGameSaveUpdate::PendingOp op{};
    op.is_delete = true;
    op.blob_name = blobName;
    it->second->ops.push_back(std::move(op));
    return RecordHr("XGameSaveSubmitBlobDelete", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdate(XGameSaveUpdateHandle updateContext) {
    if (!updateContext) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    auto it = rt.live_updates.find(updateContext);
    if (it == rt.live_updates.end()) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_HANDLE);
    }
    auto& upd = *it->second;

    std::error_code ec;
    fs::create_directories(upd.container_dir, ec);
    if (ec) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_FAIL);
    }

    ContainerMeta meta{};
    if (!LoadContainerMeta(upd.container_dir, meta)) {
        meta.name = upd.container_name;
    }
    meta.display_name = upd.display_name;
    meta.last_modified = NowUnixSeconds();

    for (const auto& op : upd.ops) {
        const std::string file_name = EncodePathToken(op.blob_name) + ".blob";
        const fs::path blob_path = upd.container_dir / file_name;
        if (op.is_delete) {
            fs::remove(blob_path, ec);
            meta.blobs.erase(op.blob_name);
        } else {
            if (!WriteBinaryFileAtomic(blob_path, op.data.data(), op.data.size())) {
                return RecordHr("XGameSaveSubmitUpdate", XGR_E_FAIL);
            }
            meta.blobs[op.blob_name] = file_name;
        }
    }

    if (!SaveContainerMeta(upd.container_dir, meta)) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_FAIL);
    }
    return RecordHr("XGameSaveSubmitUpdate", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XGameSaveCloseUpdate(XGameSaveUpdateHandle updateContext) {
    RecordOk("XGameSaveCloseUpdate");
    if (!updateContext) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    rt.live_updates.erase(updateContext);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData) {
    if (!container || !countOfBlobs || !blobData) {
        return RecordHr("XGameSaveReadBlobData", XGR_E_INVALIDARG);
    }
    fs::path dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveReadBlobData", XGR_E_HANDLE);
        }
        dir = it->second->container_dir;
    }

    ContainerMeta meta{};
    if (!LoadContainerMeta(dir, meta)) {
        return RecordHr("XGameSaveReadBlobData", XGR_E_GS_BLOB_NOT_FOUND);
    }

    std::vector<std::string> requested;
    if (blobNames && *countOfBlobs > 0) {
        requested.reserve(*countOfBlobs);
        for (uint32_t i = 0; i < *countOfBlobs; ++i) {
            if (!blobNames[i] || !*blobNames[i]) {
                return RecordHr("XGameSaveReadBlobData", XGR_E_INVALIDARG);
            }
            requested.emplace_back(blobNames[i]);
        }
    } else {
        for (const auto& [bname, _] : meta.blobs) {
            requested.push_back(bname);
        }
    }

    struct LoadedBlob {
        std::string name;
        std::vector<uint8_t> bytes;
    };
    std::vector<LoadedBlob> loaded;
    loaded.reserve(requested.size());

    size_t required_bytes = requested.size() * sizeof(XGameSaveBlob);
    for (const auto& req_name : requested) {
        auto mit = meta.blobs.find(req_name);
        if (mit == meta.blobs.end()) {
            return RecordHr("XGameSaveReadBlobData", XGR_E_GS_BLOB_NOT_FOUND);
        }
        LoadedBlob lb{};
        lb.name = req_name;
        if (!ReadBinaryFile(dir / mit->second, lb.bytes)) {
            return RecordHr("XGameSaveReadBlobData", XGR_E_GS_BLOB_NOT_FOUND);
        }
        required_bytes += (lb.name.size() + 1) + lb.bytes.size();
        loaded.push_back(std::move(lb));
    }

    if (blobsSize < required_bytes) {
        return RecordHr("XGameSaveReadBlobData", XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL);
    }

    auto* base = reinterpret_cast<uint8_t*>(blobData);
    size_t cursor = loaded.size() * sizeof(XGameSaveBlob);
    for (size_t i = 0; i < loaded.size(); ++i) {
        char* name_dst = reinterpret_cast<char*>(base + cursor);
        std::memcpy(name_dst, loaded[i].name.c_str(), loaded[i].name.size() + 1);
        cursor += loaded[i].name.size() + 1;

        uint8_t* data_dst = base + cursor;
        if (!loaded[i].bytes.empty()) {
            std::memcpy(data_dst, loaded[i].bytes.data(), loaded[i].bytes.size());
            cursor += loaded[i].bytes.size();
        }

        blobData[i].info.name = name_dst;
        blobData[i].info.size = static_cast<uint32_t>(loaded[i].bytes.size());
        blobData[i].data = loaded[i].bytes.empty() ? nullptr : data_dst;
    }

    *countOfBlobs = static_cast<uint32_t>(loaded.size());
    return RecordHr("XGameSaveReadBlobData", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback callback) {
    if (!provider || !callback) {
        return RecordHr("XGameSaveEnumerateContainerInfo", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveEnumerateContainerInfo", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    std::error_code ec;
    if (!fs::exists(root_dir, ec)) {
        return RecordHr("XGameSaveEnumerateContainerInfo", XGR_S_OK);
    }

    for (const auto& entry : fs::directory_iterator(root_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        ContainerMeta meta{};
        if (!LoadContainerMeta(entry.path(), meta)) continue;

        uint64_t total_size = 0;
        for (const auto& [_, file_name] : meta.blobs) {
            std::error_code fec;
            const auto sz = fs::file_size(entry.path() / file_name, fec);
            if (!fec) total_size += sz;
        }

        XGameSaveContainerInfo info{};
        info.name = meta.name.c_str();
        info.displayName = meta.display_name.c_str();
        info.blobCount = static_cast<uint32_t>(meta.blobs.size());
        info.totalSize = total_size;
        info.lastModifiedTime = meta.last_modified;
        info.needsSync = false;

        if (!callback(&info, context)) {
            break;
        }
    }
    return RecordHr("XGameSaveEnumerateContainerInfo", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback callback) {
    if (!container || !callback) {
        return RecordHr("XGameSaveEnumerateBlobInfo", XGR_E_INVALIDARG);
    }
    fs::path dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveEnumerateBlobInfo", XGR_E_HANDLE);
        }
        dir = it->second->container_dir;
    }

    ContainerMeta meta{};
    if (!LoadContainerMeta(dir, meta)) {
        return RecordHr("XGameSaveEnumerateBlobInfo", XGR_S_OK);
    }
    for (const auto& [blob_name, file_name] : meta.blobs) {
        std::error_code ec;
        const auto sz = fs::file_size(dir / file_name, ec);
        XGameSaveBlobInfo info{};
        info.name = blob_name.c_str();
        info.size = ec ? 0u : static_cast<uint32_t>(sz);
        if (!callback(&info, context)) {
            break;
        }
    }
    return RecordHr("XGameSaveEnumerateBlobInfo", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name) {
    if (!provider || !name || !*name) {
        return RecordHr("XGameSaveDeleteContainer", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveDeleteContainer", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    const fs::path cdir = root_dir / EncodePathToken(name);
    std::error_code ec;
    fs::remove_all(cdir, ec);
    return RecordHr("XGameSaveDeleteContainer", ec ? XGR_E_FAIL : XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota) {
    if (!provider || !remainingQuota) {
        return RecordHr("XGameSaveGetRemainingQuota", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveGetRemainingQuota", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    constexpr int64_t kQuotaBytes = 256LL * 1024LL * 1024LL;
    int64_t used = 0;
    std::error_code ec;
    if (fs::exists(root_dir, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(root_dir, ec)) {
            if (entry.is_regular_file(ec)) {
                used += static_cast<int64_t>(entry.file_size(ec));
            }
        }
    }
    *remainingQuota = std::max<int64_t>(0, kQuotaBytes - used);
    return RecordHr("XGameSaveGetRemainingQuota", XGR_S_OK);
}

// ===========================================================================
// XSystem
// ===========================================================================
XSystemDeviceType OPENSCARLETT_GDK_CALL XSystemGetDeviceType(void) {
    RecordOk("XSystemGetDeviceType");
    return XSystemDeviceType_XboxScarlettAnaconda;
}

XGameResult OPENSCARLETT_GDK_CALL XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed) {
    const std::string cid = "OS-SCARLETT-0001.0001.0001.0001";
    return RecordHr("XSystemGetConsoleId",
                    CopyStringResult(cid, consoleIdSize, consoleId, consoleIdUsed));
}

XGameResult OPENSCARLETT_GDK_CALL XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed) {
    std::string sb = GetEnvString("OPENSCARLETT_SANDBOX_ID");
    if (sb.empty()) sb = "RETAIL";
    return RecordHr("XSystemGetXboxLiveSandboxId",
                    CopyStringResult(sb, sandboxIdSize, sandboxId, sandboxIdUsed));
}

XSystemAnalyticsInfo OPENSCARLETT_GDK_CALL XSystemGetAnalyticsInfo(void) {
    RecordOk("XSystemGetAnalyticsInfo");
    XSystemAnalyticsInfo info{};
    info.osVersion.major = 10;
    info.osVersion.minor = 0;
    info.osVersion.build = 26100;
    info.osVersion.revision = 2499;
    info.hostingOsVersion = info.osVersion;
    std::snprintf(info.family, sizeof(info.family), "%s", "Windows.Xbox");
    std::snprintf(info.form, sizeof(info.form), "%s", "Xbox Series X");
    return info;
}

XSystemRuntimeInfo OPENSCARLETT_GDK_CALL XSystemGetRuntimeInfo(void) {
    RecordOk("XSystemGetRuntimeInfo");
    XSystemRuntimeInfo info{};
    info.runtimeVersion.major = 10;
    info.runtimeVersion.minor = 0;
    info.runtimeVersion.build = 26100;
    info.runtimeVersion.revision = 2499;
    info.availableVersion = info.runtimeVersion;
    return info;
}

bool OPENSCARLETT_GDK_CALL XSystemIsHandleValid(void* handle) {
    RecordOk("XSystemIsHandleValid");
    return handle != nullptr && handle != reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// ===========================================================================
// XMem
// ===========================================================================
void* OPENSCARLETT_GDK_CALL XMemAllocDefault(size_t size, uint64_t /*attributes*/) {
    RecordOk("XMemAllocDefault");
    const size_t alloc_size = size == 0 ? 16u : size;
#if defined(_WIN32)
    return _aligned_malloc(alloc_size, 16u);
#else
    return std::malloc(alloc_size);
#endif
}

void OPENSCARLETT_GDK_CALL XMemFreeDefault(void* pointer, uint64_t /*attributes*/) {
    RecordOk("XMemFreeDefault");
    if (!pointer) return;
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void* OPENSCARLETT_GDK_CALL XMemAlloc(size_t size, uint64_t attributes) {
    XMemAllocFunction hook = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mem_mutex);
        hook = rt.alloc_hook;
    }
    RecordOk("XMemAlloc");
    if (hook) {
        return hook(size, attributes);
    }
    return XMemAllocDefault(size, attributes);
}

void OPENSCARLETT_GDK_CALL XMemFree(void* pointer, uint64_t attributes) {
    XMemFreeFunction hook = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mem_mutex);
        hook = rt.free_hook;
    }
    RecordOk("XMemFree");
    if (hook) {
        hook(pointer, attributes);
        return;
    }
    XMemFreeDefault(pointer, attributes);
}

XGameResult OPENSCARLETT_GDK_CALL XMemSetAllocationHooks(
    XMemAllocFunction allocHook,
    XMemFreeFunction freeHook) {
    if ((allocHook != nullptr) != (freeHook != nullptr)) {
        return RecordHr("XMemSetAllocationHooks", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.mem_mutex);
    rt.alloc_hook = allocHook;
    rt.free_hook = freeHook;
    return RecordHr("XMemSetAllocationHooks", XGR_S_OK);
}

// ===========================================================================
// XPackage
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize,
    char* buffer) {
    const std::string pkg_id = "OpenScarlett.Title_1.0.0.0_x64__openscarlett0";
    return RecordHr("XPackageGetCurrentProcessPackageIdentifier",
                    CopyStringResult(pkg_id, bufferSize, buffer, nullptr));
}

bool OPENSCARLETT_GDK_CALL XPackageIsPackagedProcess(void) {
    RecordOk("XPackageIsPackagedProcess");
    return true;
}

XGameResult OPENSCARLETT_GDK_CALL XPackageEnumerateChunkAvailability(
    const char* /*packageIdentifier*/,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    void* context,
    XPackageChunkAvailabilityCallback callback) {
    if (!callback) {
        return RecordHr("XPackageEnumerateChunkAvailability", XGR_E_INVALIDARG);
    }
    if (selectors && selectorCount > 0) {
        for (uint32_t i = 0; i < selectorCount; ++i) {
            if (!callback(context, &selectors[i], XPackageChunkAvailability_Ready)) {
                break;
            }
        }
    } else {
        XPackageChunkSelector default_chunk{};
        default_chunk.type = XPackageChunkSelectorType_Chunk;
        default_chunk.chunkId = 1u;
        callback(context, &default_chunk, XPackageChunkAvailability_Ready);
    }
    return RecordHr("XPackageEnumerateChunkAvailability", XGR_S_OK);
}

// ===========================================================================
// XDisplay
// ===========================================================================
XDisplayHdrModeResult OPENSCARLETT_GDK_CALL XDisplayTryEnableHdrMode(
    uint32_t /*preferredHdrMode*/,
    XDisplayHdrModeInfo* displayHdrModeInfo) {
    RecordOk("XDisplayTryEnableHdrMode");
    if (displayHdrModeInfo) {
        displayHdrModeInfo->minToneMapLuminance = 0.05f;
        displayHdrModeInfo->maxToneMapLuminance = 300.0f;
        displayHdrModeInfo->maxFullFrameToneMapLuminance = 250.0f;
    }
    return XDisplayHdrModeResult_Disabled;
}

XGameResult OPENSCARLETT_GDK_CALL XDisplayTryAcquireTimeoutDeferral(
    XDisplayTimeoutDeferralHandle* handle) {
    if (!handle) {
        return RecordHr("XDisplayTryAcquireTimeoutDeferral", XGR_E_INVALIDARG);
    }
    auto* d = new (std::nothrow) XDisplayTimeoutDeferral();
    if (!d) {
        *handle = nullptr;
        return RecordHr("XDisplayTryAcquireTimeoutDeferral", XGR_E_OUTOFMEMORY);
    }
    *handle = d;
    return RecordHr("XDisplayTryAcquireTimeoutDeferral", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XDisplayCloseTimeoutDeferralHandle(
    XDisplayTimeoutDeferralHandle handle) {
    RecordOk("XDisplayCloseTimeoutDeferralHandle");
    delete handle;
}

}  // extern "C"
