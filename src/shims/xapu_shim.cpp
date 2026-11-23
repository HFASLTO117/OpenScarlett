#define OPENSCARLETT_BUILDING_XAPU 1
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "openscarlett/audio/xapu_abi.h"
#include "openscarlett/audio/audio_sync.h"
#include "openscarlett/audio/audio_trace.h"
#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <malloc.h>
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "xapu.dll";
constexpr uint32_t kMaxQueuedCommandsPerConnection = 4096u;
constexpr uint32_t kMaxCommandBytes = 16u * 1024u * 1024u;
constexpr uint32_t kMaxXApuConnections = 1024u;
constexpr uint32_t kMaxXApuStreamCount = 4096u;
constexpr uint32_t kMaxXApuUtilizationStreams = 500u;
constexpr size_t kXApuSharedMemAlignment = 0x10000u;  // 64 KiB
constexpr size_t kMinXApuSharedMemoryBytes = 0x10000u;
constexpr size_t kMaxXApuSharedMemoryBytes = 128u * 1024u * 1024u;
constexpr size_t kMaxTotalXApuSharedMemoryBytes = 256u * 1024u * 1024u;
constexpr int32_t kXboxXApuQueueEmpty = XAPU_E_QUEUE_EMPTY;

bool IsReadablePointer(const void* ptr, size_t bytes) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000ull || bytes == 0u) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    if (start + bytes < start) {
        return false;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                           prot == PAGE_EXECUTE_WRITECOPY);
    if (!readable) {
        return false;
    }
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + bytes <= region_end;
#else
    return true;
#endif
}

bool IsWritablePointer(void* ptr, size_t bytes) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000ull || bytes == 0u) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    if (start + bytes < start) {
        return false;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
    if (!writable) {
        return false;
    }
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + bytes <= region_end;
#else
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

inline bool ShouldTraceRetailCall(std::atomic<uint64_t>& counter) noexcept {
    if (!openscarlett::audio::IsRetailTraceEnabled()) {
        return false;
    }
    const uint64_t n = counter.fetch_add(1u, std::memory_order_relaxed) + 1u;
    return n <= 16u || (n % 256u) == 0u;
}

inline uint16_t ClampU16(uint32_t val) noexcept {
    return static_cast<uint16_t>((std::min)(val, static_cast<uint32_t>(UINT16_MAX)));
}

void* AllocAligned64k(size_t bytes) noexcept {
    if (bytes == 0u) {
        return nullptr;
    }
#if defined(_WIN32)
    void* p = ::_aligned_malloc(bytes, kXApuSharedMemAlignment);
    if (p) {
        std::memset(p, 0, bytes);
    }
    return p;
#else
    const size_t total = bytes + kXApuSharedMemAlignment + sizeof(void*);
    void* raw = std::malloc(total);
    if (!raw) {
        return nullptr;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(raw) + sizeof(void*);
    const uintptr_t aligned = (addr + (kXApuSharedMemAlignment - 1u)) &
                              ~static_cast<uintptr_t>(kXApuSharedMemAlignment - 1u);
    void* user = reinterpret_cast<void*>(aligned);
    std::memcpy(static_cast<uint8_t*>(user) - sizeof(void*), &raw, sizeof(void*));
    std::memset(user, 0, bytes);
    return user;
#endif
}

void FreeAligned64k(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
#if defined(_WIN32)
    ::_aligned_free(ptr);
#else
    void* raw = nullptr;
    std::memcpy(&raw, static_cast<const uint8_t*>(ptr) - sizeof(void*), sizeof(void*));
    std::free(raw);
#endif
}

inline uint64_t QueryExecUs(uint64_t start_ticks) noexcept {
#if defined(_WIN32)
    LARGE_INTEGER now{}, freq{};
    if (::QueryPerformanceCounter(&now) != FALSE &&
        ::QueryPerformanceFrequency(&freq) != FALSE &&
        freq.QuadPart > 0 &&
        static_cast<uint64_t>(now.QuadPart) >= start_ticks) {
        const uint64_t delta = static_cast<uint64_t>(now.QuadPart) - start_ticks;
        return (delta * 1000000ull) / static_cast<uint64_t>(freq.QuadPart);
    }
#else
    (void)start_ticks;
#endif
    return 0ull;
}

inline uint64_t QueryQpcNow() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER now{};
    if (::QueryPerformanceCounter(&now) != FALSE && now.QuadPart >= 0) {
        return static_cast<uint64_t>(now.QuadPart);
    }
#endif
    return 0ull;
}

struct XApuQueuedCommand {
    uint64_t sequence = 0;
    std::vector<uint8_t> payload;
};

struct XApuStreamState {
    bool active = false;
    bool stop_pending = false;
    uint32_t channels = 2u;
    float current_pitch = 1.0f;
    double frac_pos = 0.0;
    std::unique_ptr<openscarlett::OpusMftDecoder> decoder;
    std::vector<float> pending_pcm;
};

struct XApuConnectionState {
    uint64_t id = 0;
    bool xbox_retail_mode = false;
    uint32_t process_type = 0u;
    uint32_t stream_count = 0u;
    uint32_t command_queue_depth = 0u;
    uint32_t max_frame_count_per_output = 0u;
    uint32_t options = 0u;
    size_t reserved_shared_bytes = 0u;
    void* shared_mem_base = nullptr;
#if defined(_WIN32)
    HANDLE ready_event = nullptr;
#endif
    openscarlett::audio::SrwLock queue_lock;
    uint64_t next_seq = 1u;
    uint32_t consecutive_empty_polls = 0u;
    std::deque<XApuQueuedCommand> command_queue;
    std::deque<XApuResultHeader> result_queue;
    std::deque<XApuRetailResult> xbox_result_queue;
    std::unordered_map<uint32_t, XApuStreamState> streams;

    ~XApuConnectionState() {
        FreeAligned64k(shared_mem_base);
        shared_mem_base = nullptr;
#if defined(_WIN32)
        if (ready_event) {
            ::CloseHandle(ready_event);
            ready_event = nullptr;
        }
#endif
    }
};

bool EnsureOpusDecoderInitialized(XApuStreamState& stream, bool reset_existing) {
    if (stream.decoder && stream.decoder->IsInitialized() &&
        stream.decoder->Config().channels == static_cast<uint16_t>(stream.channels)) {
        if (reset_existing) {
            stream.decoder->Reset();
        }
        return true;
    }
    auto dec = std::make_unique<openscarlett::OpusMftDecoder>();
    openscarlett::OpusDecoderConfig cfg{};
    cfg.sample_rate = 48000u;
    cfg.channels = static_cast<uint16_t>(stream.channels);
    cfg.allow_fallback = true;
    if (!dec->Initialize(cfg).ok()) {
        return false;
    }
    stream.decoder = std::move(dec);
    return true;
}

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

        const bool arg1_writable_handle = IsWritablePointer(arg1, sizeof(XAPU_CONNECTION_HANDLE));
        const bool arg1_readable_params = IsReadablePointer(arg1, sizeof(XApuConnectInput));
        const bool arg2_writable = IsWritablePointer(arg2, sizeof(XApuConnectOutput));
        const bool arg3_writable = IsWritablePointer(arg3, sizeof(XAPU_CONNECTION_HANDLE));

        // If arg1 is read-only (e.g. const XApuConnectInput in .rdata), this is strictly a 3-arg call.
        if (!arg1_writable_handle) {
            if (!arg1_readable_params || !arg2_writable || !arg3_writable) {
                return XAPU_E_POINTER;
            }
        }

        // Detect real retail Xbox 3-arg XApuConnect(pInput, pOutput, ppHandle)
        // vs synthetic 1-arg XApuConnect(XAPU_CONNECTION_HANDLE* outHandle).
        bool is_xbox_retail = false;
        XApuConnectInput params{};
        if (arg2_writable && arg3_writable && arg1_readable_params &&
            arg2 != arg1 && arg3 != arg1 && arg2 != arg3) {
            std::memcpy(&params, arg1, sizeof(params));
            const bool clearly_3arg =
                !arg1_writable_handle ||
                (params.processType >= 1u && params.processType <= 3u) ||
                (params.maxFrameCountPerOutput == 0u && params.options == 0u &&
                 (params.processType != 0u || params.streamCount != 0u ||
                  params.commandQueueDepth != 0u || params.sharedMemoryBytes != 0u));
            if (clearly_3arg) {
                is_xbox_retail = true;
            }
        }

        if (is_xbox_retail) {
            auto* out_shm = static_cast<XApuConnectOutput*>(arg2);
            auto* out_handle = static_cast<XAPU_CONNECTION_HANDLE*>(arg3);
            out_shm->baseAddress = nullptr;
            out_shm->sizeOrReadyEvent = 0ull;
            *out_handle = nullptr;

            // Validate processType 1..3 (DecodeOpus, DecodeConvertOpus, ConvertPCM)
            if (params.processType != XApuProcessType_DecodeOpus &&
                params.processType != XApuProcessType_DecodeConvertOpus &&
                params.processType != XApuProcessType_ConvertPCM) {
                return XAPU_E_INVALIDARG;
            }
            if (params.streamCount == 0u || params.streamCount > kMaxXApuStreamCount) {
                return XAPU_E_INVALIDARG;
            }
            if (params.sharedMemoryBytes > kMaxXApuSharedMemoryBytes) {
                std::fprintf(stderr, "[OpenScarlett][xapu] XApuConnect FAIL: shm too big 0x%X\n",
                             params.sharedMemoryBytes);
                return XAPU_E_OUTOFMEMORY;
            }

            // Honor caller's requested sharedMemoryBytes: max(round_up_4k(sharedMemoryBytes), 0x10000).
            const uint64_t rounded_4k =
                (static_cast<uint64_t>(params.sharedMemoryBytes) + 0xFFFull) & ~0xFFFull;
            const uint64_t alloc_bytes64 =
                (std::max)(rounded_4k, static_cast<uint64_t>(kMinXApuSharedMemoryBytes));
            if (alloc_bytes64 > kMaxXApuSharedMemoryBytes) {
                std::fprintf(stderr, "[OpenScarlett][xapu] XApuConnect FAIL: alloc64 too big\n");
                return XAPU_E_OUTOFMEMORY;
            }
            const size_t alloc_bytes = static_cast<size_t>(alloc_bytes64);

            {
                openscarlett::audio::SrwLockGuard lk(table_lock_);
                if (connections_.size() >= kMaxXApuConnections) {
                    std::fprintf(stderr, "[OpenScarlett][xapu] XApuConnect FAIL: max connections\n");
                    return XAPU_E_OUTOFMEMORY;
                }
                if (total_shared_bytes_ + alloc_bytes < total_shared_bytes_ ||
                    total_shared_bytes_ + alloc_bytes > kMaxTotalXApuSharedMemoryBytes) {
                    std::fprintf(stderr, "[OpenScarlett][xapu] XApuConnect FAIL: total shm budget\n");
                    return XAPU_E_OUTOFMEMORY;
                }
                total_shared_bytes_ += alloc_bytes;
            }

            auto release_budget = [&]() {
                openscarlett::audio::SrwLockGuard lk(table_lock_);
                if (total_shared_bytes_ >= alloc_bytes) {
                    total_shared_bytes_ -= alloc_bytes;
                } else {
                    total_shared_bytes_ = 0u;
                }
            };

            std::shared_ptr<XApuConnectionState> state;
            try {
                state = std::make_shared<XApuConnectionState>();
            } catch (...) {
                release_budget();
                return XAPU_E_OUTOFMEMORY;
            }

            void* aligned_mem = AllocAligned64k(alloc_bytes);
            if (!aligned_mem) {
                std::fprintf(stderr, "[OpenScarlett][xapu] XApuConnect FAIL: aligned_alloc %zu bytes\n",
                             alloc_bytes);
                release_budget();
                return XAPU_E_OUTOFMEMORY;
            }
            state->shared_mem_base = aligned_mem;

#if defined(_WIN32)
            HANDLE ready_ev = nullptr;
            if ((params.options & XApuConnectOption_EnableResultReadyEvent) != 0u) {
                ready_ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!ready_ev) {
                    release_budget();
                    return XAPU_E_OUTOFMEMORY;
                }
            }
            state->ready_event = ready_ev;
#endif

            const uint64_t handle_val = next_handle_id_.fetch_add(1u, std::memory_order_relaxed);
            state->id = handle_val;
            state->xbox_retail_mode = true;
            state->process_type = params.processType;
            state->stream_count = params.streamCount;
            state->command_queue_depth = params.commandQueueDepth;
            state->max_frame_count_per_output = params.maxFrameCountPerOutput;
            state->options = params.options;
            state->reserved_shared_bytes = alloc_bytes;

            auto* raw_handle = reinterpret_cast<XAPU_CONNECTION_HANDLE>(
                static_cast<uintptr_t>(0x5841505500000000ull | (handle_val & 0xFFFFFFFFull)));

            out_shm->baseAddress = state->shared_mem_base;
#if defined(_WIN32)
            if ((params.options & XApuConnectOption_EnableResultReadyEvent) != 0u) {
                out_shm->sizeOrReadyEvent =
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(state->ready_event));
            } else {
                out_shm->sizeOrReadyEvent = alloc_bytes;
            }
#else
            out_shm->sizeOrReadyEvent = alloc_bytes;
#endif
            *out_handle = raw_handle;

            {
                openscarlett::audio::SrwLockGuard lk(table_lock_);
                connections_[raw_handle] = std::move(state);
            }
            return XAPU_S_OK;
        }

        // 1-arg synthetic path: XApuConnect(XAPU_CONNECTION_HANDLE* outHandle)
        if (!arg1_writable_handle) {
            return XAPU_E_POINTER;
        }
        *static_cast<XAPU_CONNECTION_HANDLE*>(arg1) = nullptr;

        {
            openscarlett::audio::SrwLockGuard lk(table_lock_);
            if (connections_.size() >= kMaxXApuConnections) {
                return XAPU_E_OUTOFMEMORY;
            }
        }

        std::shared_ptr<XApuConnectionState> state;
        try {
            state = std::make_shared<XApuConnectionState>();
        } catch (...) {
            return XAPU_E_OUTOFMEMORY;
        }
        const uint64_t handle_val = next_handle_id_.fetch_add(1u, std::memory_order_relaxed);
        state->id = handle_val;
        state->xbox_retail_mode = false;
        auto* raw_handle = reinterpret_cast<XAPU_CONNECTION_HANDLE>(
            static_cast<uintptr_t>(0x5841505500000000ull | (handle_val & 0xFFFFFFFFull)));

        {
            openscarlett::audio::SrwLockGuard lk(table_lock_);
            if (connections_.size() >= kMaxXApuConnections) {
                return XAPU_E_OUTOFMEMORY;
            }
            connections_[raw_handle] = std::move(state);
        }
        *static_cast<XAPU_CONNECTION_HANDLE*>(arg1) = raw_handle;
        return XAPU_S_OK;
    }

    int32_t EnqueueCommand(
        XAPU_CONNECTION_HANDLE connection,
        const void* command,
        uint32_t command_size_or_flags,
        void* caller_addr) {
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
            return EnqueueRetailCommand(*state, command, caller_addr);
        }

        const uint32_t command_size = command_size_or_flags;
        if (command_size == 0u || command_size > kMaxCommandBytes) {
            return XAPU_E_INVALIDARG;
        }

        const auto* bytes = static_cast<const uint8_t*>(command);

        openscarlett::audio::SrwLockGuard lk(state->queue_lock);
        if (state->result_queue.size() >= kMaxQueuedCommandsPerConnection) {
            return XAPU_E_OUTOFMEMORY;
        }

        const uint64_t seq = state->next_seq++;
        XApuQueuedCommand queued{};
        queued.sequence = seq;
        queued.payload.assign(bytes, bytes + command_size);
        state->command_queue.push_back(std::move(queued));

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
            if (out_result_size && IsWritablePointer(out_result_size, sizeof(uint32_t))) {
                *out_result_size = 0u;
            }
            return XAPU_E_HANDLE;
        }
        if (!result_buffer) {
            if (out_result_size && IsWritablePointer(out_result_size, sizeof(uint32_t))) {
                *out_result_size = 0u;
            }
            return XAPU_E_POINTER;
        }

        std::shared_ptr<XApuConnectionState> state = Lookup(connection);
        if (!state) {
            if (out_result_size && IsWritablePointer(out_result_size, sizeof(uint32_t))) {
                *out_result_size = 0u;
            }
            return XAPU_E_HANDLE;
        }

        if (state->xbox_retail_mode) {
            const bool full_writable = IsWritablePointer(result_buffer, sizeof(XApuRetailResult));
            const bool partial_writable =
                !full_writable &&
                result_buffer_size > 0u &&
                result_buffer_size < sizeof(XApuRetailResult) &&
                IsWritablePointer(result_buffer, result_buffer_size);

            uint32_t empty_polls = 0u;
            XApuRetailResult completed{};
            {
                openscarlett::audio::SrwLockGuard lk(state->queue_lock);
                if (state->xbox_result_queue.empty()) {
                    empty_polls = ++state->consecutive_empty_polls;
                } else {
                    if (!full_writable && !partial_writable) {
                        return XAPU_E_POINTER;
                    }
                    state->consecutive_empty_polls = 0u;
                    completed = state->xbox_result_queue.front();
                    state->xbox_result_queue.pop_front();

                    if (completed.header.type == XApuRetailCommand_Stop) {
                        auto it = state->streams.find(completed.header.streamIndex);
                        if (it != state->streams.end() && it->second.stop_pending) {
                            it->second.active = false;
                            it->second.stop_pending = false;
                            it->second.pending_pcm.clear();
                            it->second.frac_pos = 0.0;
                            it->second.decoder.reset();
                        }
                    }
                }
            }

            if (empty_polls > 0u) {
#if defined(_WIN32)
                if (empty_polls >= 4u) {
                    ::Sleep(1u);
                }
#endif
                return kXboxXApuQueueEmpty;
            }

            const size_t copy_bytes =
                full_writable ? sizeof(XApuRetailResult) : static_cast<size_t>(result_buffer_size);
            std::memcpy(result_buffer, &completed, copy_bytes);
            return XAPU_S_OK;
        }

        if (out_result_size) {
            *out_result_size = 0u;
        }
        if (result_buffer_size == 0u) {
            return XAPU_E_INVALIDARG;
        }

        openscarlett::audio::SrwLockGuard lk(state->queue_lock);
        if (state->result_queue.empty()) {
            return XAPU_S_EMPTY;
        }

        const XApuResultHeader completed = state->result_queue.front();
        state->result_queue.pop_front();

        std::memset(result_buffer, 0, result_buffer_size);
        const uint32_t copy_bytes = (std::min)(
            result_buffer_size, static_cast<uint32_t>(sizeof(XApuResultHeader)));
        std::memcpy(result_buffer, &completed, copy_bytes);

        if (out_result_size) {
            *out_result_size = copy_bytes;
        }
        return XAPU_S_OK;
    }

    int32_t GetResultCount(
        XAPU_CONNECTION_HANDLE connection,
        uint32_t* out_result_count) {
        if (!out_result_count || !IsWritablePointer(out_result_count, sizeof(uint32_t))) {
            return XAPU_E_INVALIDARG;
        }
        *out_result_count = 0u;

        if (!connection) {
            return XAPU_E_INVALIDARG;
        }

        std::shared_ptr<XApuConnectionState> state = Lookup(connection);
        if (!state) {
            return XAPU_E_INVALIDARG;
        }

        openscarlett::audio::SrwLockGuard lk(state->queue_lock);
        const size_t count = state->xbox_retail_mode
                                 ? state->xbox_result_queue.size()
                                 : state->result_queue.size();
        const size_t max_u32 = static_cast<size_t>((std::numeric_limits<uint32_t>::max)());
        *out_result_count = static_cast<uint32_t>((std::min)(count, max_u32));
        return XAPU_S_OK;
    }

    int32_t GetUtilization(
        uint32_t codec_type,
        uint32_t kind,
        uint32_t* out_counts) {
        if (!out_counts || !IsWritablePointer(out_counts, 2u * sizeof(uint32_t))) {
            return XAPU_E_POINTER;
        }
        if (codec_type < 1u || codec_type > 3u || kind < 1u || kind > 2u) {
            return XAPU_E_INVALIDARG;
        }

        uint32_t active_streams = 0u;
        {
            openscarlett::audio::SrwLockGuard tbl_lk(table_lock_);
            for (const auto& kv : connections_) {
                const auto& conn = kv.second;
                if (!conn || !conn->xbox_retail_mode || conn->process_type != codec_type) {
                    continue;
                }
                openscarlett::audio::SrwLockGuard q_lk(conn->queue_lock);
                for (const auto& skv : conn->streams) {
                    if (skv.second.active) {
                        ++active_streams;
                    }
                }
            }
        }

        const uint32_t free_streams =
            (active_streams < kMaxXApuUtilizationStreams)
                ? (kMaxXApuUtilizationStreams - active_streams)
                : 0u;
        out_counts[0] = free_streams;
        out_counts[1] = kMaxXApuUtilizationStreams;
        return XAPU_S_OK;
    }

    int32_t Disconnect(XAPU_CONNECTION_HANDLE connection) {
        if (!connection) {
            return XAPU_E_HANDLE;
        }

        std::shared_ptr<XApuConnectionState> removed;
        {
            openscarlett::audio::SrwLockGuard lk(table_lock_);
            auto it = connections_.find(connection);
            if (it == connections_.end()) {
                return XAPU_E_HANDLE;
            }
            removed = std::move(it->second);
            connections_.erase(it);
            if (removed && removed->reserved_shared_bytes > 0u) {
                if (total_shared_bytes_ >= removed->reserved_shared_bytes) {
                    total_shared_bytes_ -= removed->reserved_shared_bytes;
                } else {
                    total_shared_bytes_ = 0u;
                }
                removed->reserved_shared_bytes = 0u;
            }
        }

        if (removed) {
#if defined(_WIN32)
            HANDLE ev = nullptr;
#endif
            void* shm_to_free = nullptr;
            {
                openscarlett::audio::SrwLockGuard lk(removed->queue_lock);
                removed->command_queue.clear();
                removed->result_queue.clear();
                removed->xbox_result_queue.clear();
                removed->streams.clear();
                shm_to_free = removed->shared_mem_base;
                removed->shared_mem_base = nullptr;
#if defined(_WIN32)
                ev = removed->ready_event;
                removed->ready_event = nullptr;
#endif
            }
            FreeAligned64k(shm_to_free);
#if defined(_WIN32)
            if (ev) {
                ::CloseHandle(ev);
            }
#endif
        }
        return XAPU_S_OK;
    }

private:
    int32_t EnqueueRetailCommand(
        XApuConnectionState& state,
        const void* command,
        void* caller_addr) {
        if (!IsReadablePointer(command, 8u)) {
            return XAPU_E_POINTER;
        }

        const uint64_t qpc_start = QueryQpcNow();
        const auto* cmd_bytes = static_cast<const uint8_t*>(command);

        XApuRetailCommandHeader hdr{};
        std::memcpy(&hdr.type, cmd_bytes + 0x00, sizeof(uint32_t));
        std::memcpy(&hdr.streamIndex, cmd_bytes + 0x04, sizeof(uint32_t));
        if (IsReadablePointer(command, 0x18u)) {
            std::memcpy(&hdr.sequence, cmd_bytes + 0x08, sizeof(uint32_t));
            std::memcpy(&hdr.options, cmd_bytes + 0x0C, sizeof(uint32_t));
            std::memcpy(&hdr.context, cmd_bytes + 0x10, sizeof(uint64_t));
        } else if (IsReadablePointer(command, 0x10u)) {
            std::memcpy(&hdr.sequence, cmd_bytes + 0x08, sizeof(uint32_t));
            std::memcpy(&hdr.options, cmd_bytes + 0x0C, sizeof(uint32_t));
        }

        XApuRetailResult res{};
        res.header = hdr;
        res.hresult = XAPU_S_OK;
        res.currentPitch = 1.0f;

#if defined(_WIN32)
        HANDLE event_to_signal = nullptr;
#endif
        {
            openscarlett::audio::SrwLockGuard lk(state.queue_lock);
            if (state.xbox_result_queue.size() >= kMaxQueuedCommandsPerConnection) {
                return XAPU_E_OUTOFMEMORY;
            }

            if (hdr.type == XApuRetailCommand_Start) {
                XApuStreamState& stream = state.streams[hdr.streamIndex];
                uint32_t channels = 2u;
                if (IsReadablePointer(command, 0x1Cu)) {
                    uint32_t raw_ch = 0u;
                    std::memcpy(&raw_ch, cmd_bytes + 0x18, sizeof(uint32_t));
                    channels = (raw_ch == 0u) ? 2u : std::clamp<uint32_t>(raw_ch, 1u, 2u);
                }
                float start_pitch = 1.0f;
                if (IsReadablePointer(command, 0x2Cu)) {
                    float raw_pitch = 1.0f;
                    std::memcpy(&raw_pitch, cmd_bytes + 0x28, sizeof(float));
                    start_pitch = std::isfinite(raw_pitch)
                                      ? std::clamp(raw_pitch, 0.125f, 8.0f)
                                      : 1.0f;
                }

                stream.channels = channels;
                stream.current_pitch = start_pitch;
                stream.frac_pos = 0.0;
                stream.pending_pcm.clear();
                stream.stop_pending = false;
                stream.active = true;

                if (state.process_type == XApuProcessType_DecodeOpus ||
                    state.process_type == XApuProcessType_DecodeConvertOpus) {
                    (void)EnsureOpusDecoderInitialized(stream, true);
                }

                res.currentPitch = stream.current_pitch;
                res.state = 2u;
            } else if (hdr.type == XApuRetailCommand_Stop) {
                auto it = state.streams.find(hdr.streamIndex);
                if (it != state.streams.end()) {
                    it->second.stop_pending = true;
                    res.currentPitch = it->second.current_pitch;
                    const uint32_t pending_frames =
                        static_cast<uint32_t>(it->second.pending_pcm.size() / it->second.channels);
                    res.state = (pending_frames == 0u) ? 2u : 4u;
                    res.stateData[3] = ClampU16(pending_frames);
                } else {
                    res.state = 2u;
                }
            } else if (hdr.type == XApuRetailCommand_Reset) {
                auto it = state.streams.find(hdr.streamIndex);
                if (it != state.streams.end()) {
                    it->second.pending_pcm.clear();
                    it->second.frac_pos = 0.0;
                    if ((hdr.options & 4u) != 0u && it->second.decoder) {
                        it->second.decoder->Reset();
                    }
                    res.currentPitch = it->second.current_pitch;
                }
                res.state = 2u;
            } else if (hdr.type == XApuRetailCommand_Data) {
                ProcessRetailDataCommand(state, hdr, cmd_bytes, command, res);
            } else {
                openscarlett::audio::LogUnimplAudio(
                    "xapu",
                    caller_addr,
                    "unknown retail command type=%u stream=%u seq=%u",
                    hdr.type,
                    hdr.streamIndex,
                    hdr.sequence);
                res.hresult = XAPU_S_OK;
                res.state = 2u;
            }

            res.execTimeUs = QueryExecUs(qpc_start);
            state.xbox_result_queue.push_back(res);
#if defined(_WIN32)
            event_to_signal = state.ready_event;
#endif
        }

#if defined(_WIN32)
        if (event_to_signal) {
            ::SetEvent(event_to_signal);
        }
#endif
        return XAPU_S_OK;
    }

    void ProcessRetailDataCommand(
        XApuConnectionState& state,
        const XApuRetailCommandHeader& hdr,
        const uint8_t* cmd_bytes,
        const void* command,
        XApuRetailResult& res) {
        XApuStreamState& stream = state.streams[hdr.streamIndex];
        stream.active = true;
        const uint32_t ch = std::clamp<uint32_t>(stream.channels, 1u, 2u);
        stream.channels = ch;

        float pitch_ramp = 0.0f;
        float target_pitch = stream.current_pitch;
        uint32_t first_frame = 0u;
        uint32_t frame_count = 0xFFFFFFFFu;
        uint32_t input_data_len = 0u;
        uint64_t input_data_ptr_u64 = 0ull;
        uint32_t max_output_len = 0u;
        uint64_t output_data_ptr_u64 = 0ull;

        if (IsReadablePointer(command, sizeof(XApuRetailDataCommand))) {
            std::memcpy(&pitch_ramp, cmd_bytes + 0x18, sizeof(float));
            std::memcpy(&target_pitch, cmd_bytes + 0x1C, sizeof(float));
            std::memcpy(&first_frame, cmd_bytes + 0x20, sizeof(uint32_t));
            std::memcpy(&frame_count, cmd_bytes + 0x24, sizeof(uint32_t));
            std::memcpy(&input_data_len, cmd_bytes + 0x28, sizeof(uint32_t));
            std::memcpy(&input_data_ptr_u64, cmd_bytes + 0x2C, sizeof(uint64_t));
            std::memcpy(&max_output_len, cmd_bytes + 0x34, sizeof(uint32_t));
            std::memcpy(&output_data_ptr_u64, cmd_bytes + 0x38, sizeof(uint64_t));
        }

        const void* in_ptr =
            reinterpret_cast<const void*>(static_cast<uintptr_t>(input_data_ptr_u64));
        void* out_ptr =
            reinterpret_cast<void*>(static_cast<uintptr_t>(output_data_ptr_u64));

        res.inputDataPtr = input_data_ptr_u64;
        res.outputDataPtr = output_data_ptr_u64;

        uint32_t input_bytes_consumed = 0u;
        const bool flush_end = (hdr.options & 2u) != 0u;

        // 1. Decode / ingest input audio
        if (state.process_type == XApuProcessType_DecodeOpus ||
            state.process_type == XApuProcessType_DecodeConvertOpus) {
            if (!EnsureOpusDecoderInitialized(stream, false)) {
                res.hresult = XAPU_E_FAIL;
            } else {
                std::vector<float> decoded;
                if (input_data_len > 0u && IsReadablePointer(in_ptr, input_data_len)) {
                    const auto* in_u8 = static_cast<const uint8_t*>(in_ptr);
                    openscarlett::Status st = stream.decoder->DecodeStream(
                        in_u8, input_data_len, decoded, openscarlett::OpusPacketFraming::Auto);
                    if (!st.ok()) {
                        decoded.clear();
                        st = stream.decoder->DecodePacket(in_u8, input_data_len, decoded);
                    }
                    if (!st.ok()) {
                        res.hresult = XAPU_E_BAD_OPUS_PACKET;
                    } else {
                        input_bytes_consumed = input_data_len;
                    }
                }
                if (flush_end && res.hresult == XAPU_S_OK) {
                    (void)stream.decoder->Drain(decoded);
                }

                const uint32_t decoded_frames = static_cast<uint32_t>(decoded.size() / ch);
                const uint32_t skip = (std::min)(first_frame, decoded_frames);
                const uint32_t avail = decoded_frames - skip;
                const uint32_t keep =
                    (frame_count == 0xFFFFFFFFu) ? avail : (std::min)(frame_count, avail);
                if (keep > 0u) {
                    const size_t start_sample = static_cast<size_t>(skip) * ch;
                    const size_t copy_samples = static_cast<size_t>(keep) * ch;
                    stream.pending_pcm.insert(
                        stream.pending_pcm.end(),
                        decoded.begin() + static_cast<std::ptrdiff_t>(start_sample),
                        decoded.begin() + static_cast<std::ptrdiff_t>(start_sample + copy_samples));
                }
            }
        } else if (state.process_type == XApuProcessType_ConvertPCM) {
            if (input_data_len > 0u && IsReadablePointer(in_ptr, input_data_len)) {
                const uint32_t total_in_frames =
                    input_data_len / (ch * static_cast<uint32_t>(sizeof(float)));
                const uint32_t skip = (std::min)(first_frame, total_in_frames);
                const uint32_t avail = total_in_frames - skip;
                const uint32_t keep =
                    (frame_count == 0xFFFFFFFFu) ? avail : (std::min)(frame_count, avail);
                const bool deinterleaved_in =
                    (state.options & XApuConnectOption_DeinterleavedInput) != 0u;
                const size_t base_samples = stream.pending_pcm.size();
                stream.pending_pcm.resize(base_samples + static_cast<size_t>(keep) * ch);
                if (keep > 0u) {
                    if (deinterleaved_in && ch == 2u) {
                        const auto* in_u8 = static_cast<const uint8_t*>(in_ptr);
                        for (uint32_t f = 0u; f < keep; ++f) {
                            const size_t src_idx = static_cast<size_t>(skip + f);
                            float s0 = 0.0f;
                            float s1 = 0.0f;
                            std::memcpy(&s0, in_u8 + src_idx * sizeof(float), sizeof(float));
                            std::memcpy(
                                &s1,
                                in_u8 + (static_cast<size_t>(total_in_frames) + src_idx) * sizeof(float),
                                sizeof(float));
                            stream.pending_pcm[base_samples + static_cast<size_t>(f) * 2u + 0u] = s0;
                            stream.pending_pcm[base_samples + static_cast<size_t>(f) * 2u + 1u] = s1;
                        }
                    } else {
                        const auto* in_u8 = static_cast<const uint8_t*>(in_ptr);
                        std::memcpy(
                            stream.pending_pcm.data() + base_samples,
                            in_u8 + static_cast<size_t>(skip) * ch * sizeof(float),
                            static_cast<size_t>(keep) * ch * sizeof(float));
                    }
                }
                input_bytes_consumed = total_in_frames * ch * static_cast<uint32_t>(sizeof(float));
            }
        }

        // 2. Output emission (direct copy for DecodeOpus, pitch-ramped linear SRC for Type 2/3)
        uint32_t out_frames = 0u;
        uint32_t input_frames_consumed = 0u;
        bool saturated = false;
        std::vector<float> out_interleaved;

        if (max_output_len > 0u && IsWritablePointer(out_ptr, max_output_len)) {
            const uint32_t max_out_frames =
                max_output_len / (ch * static_cast<uint32_t>(sizeof(float)));
            if (max_out_frames == 0u) {
                if (res.hresult == XAPU_S_OK) {
                    res.hresult = XAPU_E_BUFFER_TOO_SMALL_APU;
                }
            } else if (state.process_type == XApuProcessType_DecodeOpus) {
                const uint32_t pending_frames =
                    static_cast<uint32_t>(stream.pending_pcm.size() / ch);
                out_frames = (std::min)(pending_frames, max_out_frames);
                if (out_frames > 0u) {
                    const size_t sample_cnt = static_cast<size_t>(out_frames) * ch;
                    out_interleaved.assign(
                        stream.pending_pcm.begin(),
                        stream.pending_pcm.begin() + static_cast<std::ptrdiff_t>(sample_cnt));
                    for (float s : out_interleaved) {
                        if (std::fabs(s) > 1.0f) {
                            saturated = true;
                        }
                    }
                    stream.pending_pcm.erase(
                        stream.pending_pcm.begin(),
                        stream.pending_pcm.begin() + static_cast<std::ptrdiff_t>(sample_cnt));
                    input_frames_consumed = out_frames;
                }
            } else {
                const uint32_t pending_frames =
                    static_cast<uint32_t>(stream.pending_pcm.size() / ch);
                const float clamped_target =
                    (std::isfinite(target_pitch) && target_pitch > 0.0f)
                        ? std::clamp(target_pitch, 0.125f, 8.0f)
                        : stream.current_pitch;
                const float abs_ramp =
                    std::isfinite(pitch_ramp) ? std::fabs(pitch_ramp) : 0.0f;

                if (pending_frames > 0u) {
                    out_interleaved.reserve(static_cast<size_t>(max_out_frames) * ch);
                    for (uint32_t f = 0u; f < max_out_frames; ++f) {
                        if (stream.frac_pos < 0.0) {
                            stream.frac_pos = 0.0;
                        }
                        const double floor_pos = std::floor(stream.frac_pos);
                        const uint32_t idx0 = static_cast<uint32_t>(floor_pos);
                        if (idx0 >= pending_frames) {
                            break;
                        }
                        const float alpha = static_cast<float>(stream.frac_pos - floor_pos);
                        if (idx0 + 1u >= pending_frames && !flush_end && alpha > 0.0f) {
                            break;
                        }
                        const uint32_t idx1 =
                            (idx0 + 1u < pending_frames) ? (idx0 + 1u) : idx0;

                        for (uint32_t c = 0u; c < ch; ++c) {
                            const float s0 =
                                stream.pending_pcm[static_cast<size_t>(idx0) * ch + c];
                            const float s1 =
                                stream.pending_pcm[static_cast<size_t>(idx1) * ch + c];
                            const float s = s0 + (s1 - s0) * alpha;
                            if (std::fabs(s) > 1.0f) {
                                saturated = true;
                            }
                            out_interleaved.push_back(s);
                        }
                        ++out_frames;

                        stream.frac_pos += static_cast<double>(stream.current_pitch);
                        if (abs_ramp > 0.0f) {
                            if (stream.current_pitch < clamped_target) {
                                stream.current_pitch =
                                    (std::min)(clamped_target, stream.current_pitch + abs_ramp);
                            } else if (stream.current_pitch > clamped_target) {
                                stream.current_pitch =
                                    (std::max)(clamped_target, stream.current_pitch - abs_ramp);
                            }
                        }
                    }

                    const uint32_t consumed = (std::min)(
                        pending_frames,
                        static_cast<uint32_t>(std::floor(stream.frac_pos)));
                    if (consumed > 0u) {
                        stream.pending_pcm.erase(
                            stream.pending_pcm.begin(),
                            stream.pending_pcm.begin() +
                                static_cast<std::ptrdiff_t>(static_cast<size_t>(consumed) * ch));
                        stream.frac_pos -= static_cast<double>(consumed);
                        if (stream.pending_pcm.empty()) {
                            stream.frac_pos = 0.0;
                        }
                    }
                    input_frames_consumed = consumed;
                }
            }

            if (out_frames > 0u) {
                const bool deinterleaved_out =
                    (state.options & XApuConnectOption_DeinterleavedOutput) != 0u;
                auto* out_u8 = static_cast<uint8_t*>(out_ptr);
                if (deinterleaved_out && ch == 2u) {
                    const uint32_t max_out_floats =
                        max_output_len / static_cast<uint32_t>(sizeof(float));
                    uint32_t stride =
                        (state.process_type == XApuProcessType_DecodeOpus)
                            ? out_frames
                            : (std::max)(out_frames, state.max_frame_count_per_output);
                    const uint32_t max_stride =
                        (max_out_floats >= out_frames) ? (max_out_floats - out_frames) : 0u;
                    if (stride > max_stride) {
                        stride = max_stride;
                    }
                    for (uint32_t f = 0u; f < out_frames; ++f) {
                        const float s0 = out_interleaved[static_cast<size_t>(f) * 2u + 0u];
                        const float s1 = out_interleaved[static_cast<size_t>(f) * 2u + 1u];
                        std::memcpy(out_u8 + static_cast<size_t>(f) * sizeof(float), &s0, sizeof(float));
                        std::memcpy(
                            out_u8 + (static_cast<size_t>(stride) + f) * sizeof(float),
                            &s1,
                            sizeof(float));
                    }
                } else {
                    std::memcpy(
                        out_u8,
                        out_interleaved.data(),
                        static_cast<size_t>(out_frames) * ch * sizeof(float));
                }
            }
        }

        const uint32_t remaining_pending_frames =
            static_cast<uint32_t>(stream.pending_pcm.size() / ch);
        res.state = (out_frames > 0u ? 1u : 0u) |
                    (remaining_pending_frames == 0u ? 2u : 0u) |
                    (remaining_pending_frames > 0u ? 4u : 0u) |
                    (saturated ? 8u : 0u);
        res.stateData[0] = ClampU16(out_frames);
        res.stateData[1] = ClampU16(input_frames_consumed);
        res.stateData[2] = 0u;
        res.stateData[3] = ClampU16(remaining_pending_frames);
        res.outputFrameCount = out_frames;
        res.outputDataLength = out_frames * ch * static_cast<uint32_t>(sizeof(float));
        res.currentPitch = stream.current_pitch;
        res.inputDataLength = input_bytes_consumed;
    }

    std::shared_ptr<XApuConnectionState> Lookup(XAPU_CONNECTION_HANDLE connection) {
        openscarlett::audio::SrwLockGuard lk(table_lock_);
        auto it = connections_.find(connection);
        if (it == connections_.end()) {
            return nullptr;
        }
        return it->second;
    }

    std::atomic<uint64_t> next_handle_id_{1u};
    openscarlett::audio::SrwLock table_lock_;
    size_t total_shared_bytes_ = 0u;
    std::unordered_map<XAPU_CONNECTION_HANDLE, std::shared_ptr<XApuConnectionState>> connections_;
};

std::atomic<uint64_t> g_trace_connect{0u};
std::atomic<uint64_t> g_trace_enqueue{0u};
std::atomic<uint64_t> g_trace_dequeue{0u};
std::atomic<uint64_t> g_trace_disconnect{0u};
std::atomic<uint64_t> g_trace_utilization{0u};

}  // namespace

extern "C" {

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuConnect(
    void* arg1,
    void* arg2,
    void* arg3) {
    void* caller_addr = OPENSCARLETT_CALLER_ADDR();
    const int32_t hr = RecordHr("XApuConnect", XApuRegistry::Instance().Connect(arg1, arg2, arg3));
    if (ShouldTraceRetailCall(g_trace_connect)) {
        openscarlett::audio::TraceRetailAudio(
            "xapu", caller_addr, "XApuConnect(arg1=%p, arg2=%p, arg3=%p) -> 0x%08X",
            arg1, arg2, arg3, static_cast<uint32_t>(hr));
    }
    return hr;
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuEnqueueCommand(
    XAPU_CONNECTION_HANDLE connection,
    const void* command,
    uint32_t commandSizeOrFlags) {
    void* caller_addr = OPENSCARLETT_CALLER_ADDR();
    const int32_t hr = RecordHr(
        "XApuEnqueueCommand",
        XApuRegistry::Instance().EnqueueCommand(
            connection, command, commandSizeOrFlags, caller_addr));
    if (ShouldTraceRetailCall(g_trace_enqueue)) {
        openscarlett::audio::TraceRetailAudio(
            "xapu", caller_addr, "XApuEnqueueCommand(conn=%p, cmd=%p, sizeOrFlags=0x%X) -> 0x%08X",
            connection, command, commandSizeOrFlags, static_cast<uint32_t>(hr));
    }
    return hr;
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuDequeueResult(
    XAPU_CONNECTION_HANDLE connection,
    void* resultBuffer,
    uint32_t resultBufferSize,
    uint32_t* outResultSize) {
    void* caller_addr = OPENSCARLETT_CALLER_ADDR();
    const int32_t hr = RecordHr(
        "XApuDequeueResult",
        XApuRegistry::Instance().DequeueResult(
            connection, resultBuffer, resultBufferSize, outResultSize));
    if (ShouldTraceRetailCall(g_trace_dequeue)) {
        openscarlett::audio::TraceRetailAudio(
            "xapu", caller_addr, "XApuDequeueResult(conn=%p, buf=%p, sz=0x%X) -> 0x%08X",
            connection, resultBuffer, resultBufferSize, static_cast<uint32_t>(hr));
    }
    return hr;
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuGetResultCount(
    XAPU_CONNECTION_HANDLE connection,
    uint32_t* resultCount) {
    return RecordHr(
        "XApuGetResultCount",
        XApuRegistry::Instance().GetResultCount(connection, resultCount));
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuGetUtilization(
    uint32_t codecType,
    uint32_t kind,
    uint32_t* outCounts) {
    void* caller_addr = OPENSCARLETT_CALLER_ADDR();
    const int32_t hr = RecordHr(
        "XApuGetUtilization",
        XApuRegistry::Instance().GetUtilization(codecType, kind, outCounts));
    if (ShouldTraceRetailCall(g_trace_utilization)) {
        openscarlett::audio::TraceRetailAudio(
            "xapu", caller_addr, "XApuGetUtilization(codec=%u, kind=%u, out=%p) -> 0x%08X",
            codecType, kind, static_cast<void*>(outCounts), static_cast<uint32_t>(hr));
    }
    return hr;
}

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuDisconnect(
    XAPU_CONNECTION_HANDLE connection) {
    void* caller_addr = OPENSCARLETT_CALLER_ADDR();
    const int32_t hr = RecordHr("XApuDisconnect", XApuRegistry::Instance().Disconnect(connection));
    if (ShouldTraceRetailCall(g_trace_disconnect)) {
        openscarlett::audio::TraceRetailAudio(
            "xapu", caller_addr, "XApuDisconnect(conn=%p) -> 0x%08X",
            connection, static_cast<uint32_t>(hr));
    }
    return hr;
}

}  // extern "C"
