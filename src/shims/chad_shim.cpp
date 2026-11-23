#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define OPENSCARLETT_CHAD_API __declspec(dllexport)
#  define OPENSCARLETT_CHAD_CALL WINAPI
#else
#  define OPENSCARLETT_CHAD_API __attribute__((visibility("default")))
#  define OPENSCARLETT_CHAD_CALL
using HRESULT = int32_t;
using HANDLE = void*;
using UINT32 = uint32_t;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_POINTER = static_cast<HRESULT>(0x80004003);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000E);
#endif

namespace {

constexpr const char* kModName = "chad.dll";
constexpr uint32_t kMaxDspSessions = 1024u;

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

inline HRESULT RecordHr(const char* sym, HRESULT hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        static_cast<int32_t>(hr),
        -1);
    return hr;
}

struct XDspSessionState {
    uint64_t id = 0;
    void* memory = nullptr;
    uint32_t memory_size = 0;
    uint32_t max_activations = 0;
    uint32_t max_stream_limit = 0;
    uint32_t priority = 0;
    std::mutex state_mutex;
    bool active = false;
    uint64_t submitted_commands = 0;
};

class XDspRegistry {
public:
    static XDspRegistry& Instance() {
        static XDspRegistry* reg = new XDspRegistry();
        return *reg;
    }

    HRESULT ConnectWithMaximumStreamLimit(
        void* p_memory,
        uint32_t memory_size,
        uint32_t max_activations,
        uint32_t max_stream_limit,
        uint32_t priority,
        void** pp_out_handle) {
        if (!pp_out_handle) {
            return E_POINTER;
        }
        if (!IsWritablePointer(pp_out_handle, sizeof(void*))) {
            return E_POINTER;
        }
        *pp_out_handle = nullptr;

        if (max_stream_limit > 0x100u || priority > 2u) {
            return E_INVALIDARG;
        }

        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            if (sessions_.size() >= kMaxDspSessions) {
                return E_OUTOFMEMORY;
            }
        }

        std::shared_ptr<XDspSessionState> state;
        try {
            state = std::make_shared<XDspSessionState>();
        } catch (...) {
            return E_OUTOFMEMORY;
        }

        const uint64_t handle_val = next_handle_id_.fetch_add(1u, std::memory_order_relaxed);
        state->id = handle_val;
        state->memory = p_memory;
        state->memory_size = memory_size;
        state->max_activations = max_activations;
        state->max_stream_limit = max_stream_limit;
        state->priority = priority;
        auto* raw_handle = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(0x4348414400000000ull | (handle_val & 0xFFFFFFFFull)));

        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            if (sessions_.size() >= kMaxDspSessions) {
                return E_OUTOFMEMORY;
            }
            sessions_[raw_handle] = std::move(state);
        }

        *pp_out_handle = raw_handle;
        return S_OK;
    }

    HRESULT Disconnect(HANDLE handle) {
        if (!handle) {
            return E_INVALIDARG;
        }

        std::shared_ptr<XDspSessionState> removed;
        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            auto it = sessions_.find(handle);
            if (it == sessions_.end()) {
                return E_INVALIDARG;
            }
            removed = std::move(it->second);
            sessions_.erase(it);
        }

        if (removed) {
            std::lock_guard<std::mutex> lk(removed->state_mutex);
            removed->active = false;
        }
        return S_OK;
    }

    HRESULT Activate(HANDLE handle, void* params) {
        (void)params;
        std::shared_ptr<XDspSessionState> state = Lookup(handle);
        if (!state) {
            return E_INVALIDARG;
        }

        std::lock_guard<std::mutex> lk(state->state_mutex);
        state->active = true;
        return S_OK;
    }

    HRESULT Deactivate(HANDLE handle) {
        std::shared_ptr<XDspSessionState> state = Lookup(handle);
        if (!state) {
            return E_INVALIDARG;
        }

        std::lock_guard<std::mutex> lk(state->state_mutex);
        state->active = false;
        return S_OK;
    }

    HRESULT SubmitCommand(
        HANDLE handle,
        const void* command,
        uint32_t command_size,
        void* out_result) {
        // Callers may invoke with 2, 3, or 4 args under x64 ABI; never dereference
        // optional/unused RDX/R8/R9 register values.
        (void)command;
        (void)command_size;
        (void)out_result;

        std::shared_ptr<XDspSessionState> state = Lookup(handle);
        if (!state) {
            return E_INVALIDARG;
        }

        std::lock_guard<std::mutex> lk(state->state_mutex);
        ++state->submitted_commands;
        return S_OK;
    }

private:
    std::shared_ptr<XDspSessionState> Lookup(HANDLE handle) {
        if (!handle) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lk(table_mutex_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end()) {
            return nullptr;
        }
        return it->second;
    }

    std::atomic<uint64_t> next_handle_id_{1u};
    std::mutex table_mutex_;
    std::unordered_map<HANDLE, std::shared_ptr<XDspSessionState>> sessions_;
};

}  // namespace

extern "C" {

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspConnectWithMaximumStreamLimit(
    void* pMemory,
    uint32_t memorySize,
    uint32_t maxActivations,
    uint32_t maxStreamLimit,
    uint32_t priority,
    void** ppOutHandle) {
    return RecordHr(
        "XDspConnectWithMaximumStreamLimit",
        XDspRegistry::Instance().ConnectWithMaximumStreamLimit(
            pMemory, memorySize, maxActivations, maxStreamLimit, priority, ppOutHandle));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspDisconnect(HANDLE handle) {
    return RecordHr("XDspDisconnect", XDspRegistry::Instance().Disconnect(handle));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspActivate(HANDLE handle, void* params) {
    return RecordHr("XDspActivate", XDspRegistry::Instance().Activate(handle, params));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspDeactivate(HANDLE handle) {
    return RecordHr("XDspDeactivate", XDspRegistry::Instance().Deactivate(handle));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspSubmitCommand(
    HANDLE handle,
    const void* command,
    uint32_t commandSize,
    void* outResult) {
    return RecordHr(
        "XDspSubmitCommand",
        XDspRegistry::Instance().SubmitCommand(handle, command, commandSize, outResult));
}

}  // extern "C"

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    (void)fdwReason;
    (void)lpvReserved;
    return TRUE;
}
#endif
