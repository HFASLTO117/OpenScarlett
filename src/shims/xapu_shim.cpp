#define OPENSCARLETT_BUILDING_XAPU 1
#include "openscarlett/audio/xapu_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "xapu.dll";
constexpr uint32_t kMaxQueuedCommandsPerConnection = 4096u;
constexpr uint32_t kMaxCommandBytes = 16u * 1024u * 1024u;
constexpr int32_t kXboxXApuQueueEmpty = static_cast<int32_t>(0x8AC81001u);

struct XboxXApuConnectParams {
    uint32_t codecType;          // +0x00 (1 = Opus/Vorbis, 2 = XMA/etc.)
    uint32_t streamCount;        // +0x04
    uint32_t maxChannels;        // +0x08
    uint32_t sharedMemoryBytes;  // +0x0C
    uint32_t extraParam;         // +0x10
    uint32_t flags;              // +0x14
};

struct XboxXApuSharedMem {
    void* baseAddress;  // +0x00
    uint64_t sizeBytes; // +0x08
};

struct XboxXApuResult {
    uint8_t data[0x50];  // 80 bytes (type at +0x00, streamId at +0x04, status at +0x2C, outBytes at +0x44)
};

bool IsWritablePointer(void* ptr, size_t bytes) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000ull) {
        return false;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
    if (!writable) {
        return false;
    }
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(ptr) + bytes <= region_end;
#else
    (void)bytes;
    return true;
#endif
}

inline int32_t RecordHr(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

struct XApuQueuedCommand {
    uint64_t sequence = 0;
    std::vector<uint8_t> payload;
};

struct XApuConnectionState {
    uint64_t id = 0;
    bool xbox_retail_mode = false;
    std::vector<uint8_t> shared_memory;
    std::mutex queue_mutex;
    uint64_t next_seq = 1;
    std::deque<XApuQueuedCommand> command_queue;
    std::deque<XApuResultHeader> result_queue;
    std::deque<XboxXApuResult> xbox_result_queue;
};

class XApuRegistry {
public:
    static XApuRegistry& Instance() {
        static XApuRegistry reg;
        return reg;
    }

    int32_t Connect(void* arg1, void* arg2, void* arg3) {
        if (!arg1) {
            return XAPU_E_POINTER;
        }

        // Detect real retail Xbox 3-arg XApuConnect(pParams, pOutSharedMem, pOutHandle)
        // vs synthetic 1-arg XApuConnect(XAPU_CONNECTION_HANDLE* outHandle).
        bool is_xbox_retail = false;
        XboxXApuConnectParams params{};
        if (IsWritablePointer(arg2, sizeof(XboxXApuSharedMem)) &&
            IsWritablePointer(arg3, sizeof(XAPU_CONNECTION_HANDLE))) {
            std::memcpy(&params, arg1, sizeof(params));
            if ((params.codecType == 1u || params.codecType == 2u) &&
                params.streamCount > 0u && params.streamCount <= 4096u &&
                params.sharedMemoryBytes >= 4096u &&
                params.sharedMemoryBytes <= 256u * 1024u * 1024u) {
                is_xbox_retail = true;
            }
        }

        auto state = std::make_shared<XApuConnectionState>();
        const uint64_t handle_val = next_handle_id_.fetch_add(1u, std::memory_order_relaxed);
        state->id = handle_val;
        state->xbox_retail_mode = is_xbox_retail;
        auto* raw_handle = reinterpret_cast<XAPU_CONNECTION_HANDLE>(
            static_cast<uintptr_t>(0x5841505500000000ull | (handle_val & 0xFFFFFFFFull)));

        if (is_xbox_retail) {
            auto* out_shm = static_cast<XboxXApuSharedMem*>(arg2);
            auto* out_handle = static_cast<XAPU_CONNECTION_HANDLE*>(arg3);
            out_shm->baseAddress = nullptr;
            out_shm->sizeBytes = 0ull;
            *out_handle = nullptr;

            // Allocate generous shared memory backing (fmod.dll indexes up to streamCount * 0xE000),
            // using checked 64-bit arithmetic and OOM rollback so outputs stay zeroed on failure.
            constexpr uint64_t kMaxRetailShmBytes = 512ull * 1024ull * 1024ull;
            const uint64_t min_bytes = static_cast<uint64_t>(params.streamCount) * 0x10000ull;
            const uint64_t alloc_bytes = std::max<uint64_t>(params.sharedMemoryBytes, min_bytes);
            if (alloc_bytes == 0ull || alloc_bytes > kMaxRetailShmBytes) {
                return XAPU_E_OUTOFMEMORY;
            }
            try {
                state->shared_memory.assign(static_cast<size_t>(alloc_bytes), 0u);
            } catch (const std::bad_alloc&) {
                state->shared_memory.clear();
                return XAPU_E_OUTOFMEMORY;
            }
            out_shm->baseAddress = state->shared_memory.data();
            out_shm->sizeBytes = alloc_bytes;
            *out_handle = raw_handle;
        } else {
            *static_cast<XAPU_CONNECTION_HANDLE*>(arg1) = raw_handle;
        }

        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            connections_[raw_handle] = std::move(state);
        }

        return XAPU_S_OK;
    }

    int32_t EnqueueCommand(
        XAPU_CONNECTION_HANDLE connection,
        const void* command,
        uint32_t command_size_or_flags) {
        if (!connection) {
            return XAPU_E_HANDLE;
        }
        if (!command) {
            return XAPU_E_POINTER;
        }

        std::shared_ptr<XApuConnectionState> state = Lookup(connection);
        if (!state) {
            return XAPU_E_HANDLE;
        }

        if (state->xbox_retail_mode) {
            uint32_t cmd_type = 0u;
            uint32_t stream_id = 0u;
            std::memcpy(&cmd_type, command, sizeof(uint32_t));
            std::memcpy(&stream_id, static_cast<const uint8_t*>(command) + 4, sizeof(uint32_t));

            std::lock_guard<std::mutex> lk(state->queue_mutex);
            if (state->xbox_result_queue.size() >= kMaxQueuedCommandsPerConnection) {
                return XAPU_E_OUTOFMEMORY;
            }
            XboxXApuResult res{};
            std::memcpy(res.data + 0x00, &cmd_type, sizeof(uint32_t));
            std::memcpy(res.data + 0x04, &stream_id, sizeof(uint32_t));
            // status at +0x2C = 0 (S_OK), bytesProduced at +0x44 = 0
            state->xbox_result_queue.push_back(res);
            return XAPU_S_OK;
        }

        const uint32_t command_size = command_size_or_flags;
        if (command_size == 0u || command_size > kMaxCommandBytes) {
            return XAPU_E_INVALIDARG;
        }

        const auto* bytes = static_cast<const uint8_t*>(command);

        std::lock_guard<std::mutex> lk(state->queue_mutex);
        if (state->result_queue.size() >= kMaxQueuedCommandsPerConnection) {
            return XAPU_E_OUTOFMEMORY;
        }

        const uint64_t seq = state->next_seq++;

        XApuQueuedCommand queued{};
        queued.sequence = seq;
        queued.payload.assign(bytes, bytes + command_size);
        state->command_queue.push_back(std::move(queued));

        // Process the command immediately onto the completion queue so callers
        // performing non-blocking XApuDequeueResult polls observe clean completion.
        XApuResultHeader result{};
        result.resultSize = static_cast<uint32_t>(sizeof(XApuResultHeader));
        result.status = XAPU_S_OK;
        result.bytesWritten = command_size;

        if (command_size >= sizeof(uint32_t)) {
            std::memcpy(&result.resultType, bytes, sizeof(uint32_t));
        } else {
            result.resultType = XApuCommandType_Nop;
        }

        if (command_size >= sizeof(uint32_t) * 2u + sizeof(uint64_t)) {
            std::memcpy(&result.commandTag, bytes + sizeof(uint32_t) * 2u, sizeof(uint64_t));
        } else {
            result.commandTag = seq;
        }

        if (result.resultType == XApuCommandType_Nop ||
            result.resultType == XApuCommandType_Flush) {
            result.bytesWritten = 0u;
        }

        state->command_queue.pop_front();
        state->result_queue.push_back(result);
        return XAPU_S_OK;
    }

    int32_t DequeueResult(
        XAPU_CONNECTION_HANDLE connection,
        void* result_buffer,
        uint32_t result_buffer_size,
        uint32_t* out_result_size) {
        if (!connection) {
            if (out_result_size) *out_result_size = 0u;
            return XAPU_E_HANDLE;
        }
        if (!result_buffer) {
            if (out_result_size) *out_result_size = 0u;
            return XAPU_E_POINTER;
        }

        std::shared_ptr<XApuConnectionState> state = Lookup(connection);
        if (!state) {
            if (out_result_size) *out_result_size = 0u;
            return XAPU_E_HANDLE;
        }

        if (state->xbox_retail_mode) {
            std::lock_guard<std::mutex> lk(state->queue_mutex);
            if (state->xbox_result_queue.empty()) {
                return kXboxXApuQueueEmpty;
            }
            const XboxXApuResult completed = state->xbox_result_queue.front();
            state->xbox_result_queue.pop_front();
            std::memcpy(result_buffer, completed.data, sizeof(completed.data));
            return XAPU_S_OK;
        }

        if (out_result_size) {
            *out_result_size = 0u;
        }
        if (result_buffer_size == 0u) {
            return XAPU_E_INVALIDARG;
        }

        std::lock_guard<std::mutex> lk(state->queue_mutex);
        if (state->result_queue.empty()) {
            return XAPU_S_EMPTY;
        }

        const XApuResultHeader completed = state->result_queue.front();
        state->result_queue.pop_front();

        std::memset(result_buffer, 0, result_buffer_size);
        const uint32_t copy_bytes = std::min<uint32_t>(
            result_buffer_size, static_cast<uint32_t>(sizeof(XApuResultHeader)));
        std::memcpy(result_buffer, &completed, copy_bytes);

        if (out_result_size) {
            *out_result_size = copy_bytes;
        }
        return XAPU_S_OK;
    }

    int32_t Disconnect(XAPU_CONNECTION_HANDLE connection) {
        if (!connection) {
            return XAPU_E_HANDLE;
        }

        std::shared_ptr<XApuConnectionState> removed;
        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            auto it = connections_.find(connection);
            if (it == connections_.end()) {
                return XAPU_E_HANDLE;
            }
            removed = std::move(it->second);
            connections_.erase(it);
        }

        if (removed) {
            std::lock_guard<std::mutex> lk(removed->queue_mutex);
            removed->command_queue.clear();
            removed->result_queue.clear();
            removed->xbox_result_queue.clear();
            removed->shared_memory.clear();
        }
        return XAPU_S_OK;
    }

private:
    std::shared_ptr<XApuConnectionState> Lookup(XAPU_CONNECTION_HANDLE connection) {
        std::lock_guard<std::mutex> lk(table_mutex_);
        auto it = connections_.find(connection);
        if (it == connections_.end()) {
            return nullptr;
        }
        return it->second;
    }

    std::atomic<uint64_t> next_handle_id_{1u};
    std::mutex table_mutex_;
    std::unordered_map<XAPU_CONNECTION_HANDLE, std::shared_ptr<XApuConnectionState>> connections_;
};

}  // namespace

extern "C" {

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuConnect(
    void* arg1,
    void* arg2,
    void* arg3) {
    return RecordHr("XApuConnect", XApuRegistry::Instance().Connect(arg1, arg2, arg3));
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuEnqueueCommand(
    XAPU_CONNECTION_HANDLE connection,
    const void* command,
    uint32_t commandSizeOrFlags) {
    return RecordHr(
        "XApuEnqueueCommand",
        XApuRegistry::Instance().EnqueueCommand(connection, command, commandSizeOrFlags));
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuDequeueResult(
    XAPU_CONNECTION_HANDLE connection,
    void* resultBuffer,
    uint32_t resultBufferSize,
    uint32_t* outResultSize) {
    return RecordHr(
        "XApuDequeueResult",
        XApuRegistry::Instance().DequeueResult(
            connection, resultBuffer, resultBufferSize, outResultSize));
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuDisconnect(
    XAPU_CONNECTION_HANDLE connection) {
    return RecordHr("XApuDisconnect", XApuRegistry::Instance().Disconnect(connection));
}

}  // extern "C"
