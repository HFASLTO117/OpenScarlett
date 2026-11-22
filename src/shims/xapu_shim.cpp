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

namespace {

constexpr const char* kModName = "xapu.dll";
constexpr uint32_t kMaxQueuedCommandsPerConnection = 4096u;
constexpr uint32_t kMaxCommandBytes = 16u * 1024u * 1024u;

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
    std::mutex queue_mutex;
    uint64_t next_seq = 1;
    std::deque<XApuQueuedCommand> command_queue;
    std::deque<XApuResultHeader> result_queue;
};

class XApuRegistry {
public:
    static XApuRegistry& Instance() {
        static XApuRegistry reg;
        return reg;
    }

    int32_t Connect(XAPU_CONNECTION_HANDLE* out_connection) {
        if (!out_connection) {
            return XAPU_E_POINTER;
        }
        *out_connection = nullptr;

        auto state = std::make_shared<XApuConnectionState>();
        const uint64_t handle_val = next_handle_id_.fetch_add(1u, std::memory_order_relaxed);
        state->id = handle_val;
        auto* raw_handle = reinterpret_cast<XAPU_CONNECTION_HANDLE>(
            static_cast<uintptr_t>(0x5841505500000000ull | (handle_val & 0xFFFFFFFFull)));

        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            connections_[raw_handle] = std::move(state);
        }

        *out_connection = raw_handle;
        return XAPU_S_OK;
    }

    int32_t EnqueueCommand(
        XAPU_CONNECTION_HANDLE connection,
        const void* command,
        uint32_t command_size) {
        if (!connection) {
            return XAPU_E_HANDLE;
        }
        if (!command) {
            return XAPU_E_POINTER;
        }
        if (command_size == 0u || command_size > kMaxCommandBytes) {
            return XAPU_E_INVALIDARG;
        }

        std::shared_ptr<XApuConnectionState> state = Lookup(connection);
        if (!state) {
            return XAPU_E_HANDLE;
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
        if (out_result_size) {
            *out_result_size = 0u;
        }
        if (!connection) {
            return XAPU_E_HANDLE;
        }
        if (!result_buffer) {
            return XAPU_E_POINTER;
        }
        if (result_buffer_size == 0u) {
            return XAPU_E_INVALIDARG;
        }

        std::shared_ptr<XApuConnectionState> state = Lookup(connection);
        if (!state) {
            return XAPU_E_HANDLE;
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
    XAPU_CONNECTION_HANDLE* outConnection) {
    return RecordHr("XApuConnect", XApuRegistry::Instance().Connect(outConnection));
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuEnqueueCommand(
    XAPU_CONNECTION_HANDLE connection,
    const void* command,
    uint32_t commandSize) {
    return RecordHr(
        "XApuEnqueueCommand",
        XApuRegistry::Instance().EnqueueCommand(connection, command, commandSize));
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
