#define OPENSCARLETT_BUILDING_PIXEVT 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

constexpr const char* kModName = "pixevt.dll";

struct PixState {
    std::atomic<uint64_t> event_count{0};
    std::atomic<uint64_t> cpu_depth{0};
    std::atomic<uint64_t> cmdlist_depth{0};
    std::atomic<uint64_t> cmdqueue_depth{0};
    std::atomic<uint64_t> fence_wake_count{0};
    std::atomic<uint64_t> gpu_capture_requests{0};

    std::mutex mutex;
    std::string last_label;
    std::wstring last_capture_file;
    std::unordered_map<std::wstring, float> counters;
};

PixState& GetPix() {
    static PixState* s = new PixState();
    return *s;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
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

void DecrementDepthClamped(std::atomic<uint64_t>& depth) {
    uint64_t cur = depth.load(std::memory_order_acquire);
    while (cur > 0u) {
        if (depth.compare_exchange_weak(cur, cur - 1u, std::memory_order_acq_rel)) {
            return;
        }
    }
}

void RecordLabel(const char* label) {
    if (!label) return;
    auto& st = GetPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.last_label = label;
}

}  // namespace

extern "C" {

void OS_SHIM_CALL PIXBeginEventOnCpu(uint64_t /*color*/, const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    st.cpu_depth.fetch_add(1u, std::memory_order_acq_rel);
    RecordLabel(formatString);
    RecordOk("PIXBeginEventOnCpu");
}

void OS_SHIM_CALL PIXEndEventOnCpu(void) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    DecrementDepthClamped(st.cpu_depth);
    RecordOk("PIXEndEventOnCpu");
}

void OS_SHIM_CALL PIXSetMarkerOnCpu(uint64_t /*color*/, const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordLabel(formatString);
    RecordOk("PIXSetMarkerOnCpu");
}

void OS_SHIM_CALL PIXBeginEventOnCommandList(
    void* /*commandList*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    st.cmdlist_depth.fetch_add(1u, std::memory_order_acq_rel);
    RecordLabel(formatString);
    RecordOk("PIXBeginEventOnCommandList");
}

void OS_SHIM_CALL PIXEndEventOnCommandList(void* /*commandList*/) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    DecrementDepthClamped(st.cmdlist_depth);
    RecordOk("PIXEndEventOnCommandList");
}

void OS_SHIM_CALL PIXSetMarkerOnCommandList(
    void* /*commandList*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordLabel(formatString);
    RecordOk("PIXSetMarkerOnCommandList");
}

void OS_SHIM_CALL PIXBeginEventOnCommandQueue(
    void* /*commandQueue*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    st.cmdqueue_depth.fetch_add(1u, std::memory_order_acq_rel);
    RecordLabel(formatString);
    RecordOk("PIXBeginEventOnCommandQueue");
}

void OS_SHIM_CALL PIXEndEventOnCommandQueue(void* /*commandQueue*/) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    DecrementDepthClamped(st.cmdqueue_depth);
    RecordOk("PIXEndEventOnCommandQueue");
}

void OS_SHIM_CALL PIXSetMarkerOnCommandQueue(
    void* /*commandQueue*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordLabel(formatString);
    RecordOk("PIXSetMarkerOnCommandQueue");
}

void OS_SHIM_CALL PIXNotifyWakeFromFenceSignal(void* /*eventHandle*/) {
    auto& st = GetPix();
    st.fence_wake_count.fetch_add(1u, std::memory_order_relaxed);
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordOk("PIXNotifyWakeFromFenceSignal");
}

void OS_SHIM_CALL PIXReportCounter(const wchar_t* name, float value) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    if (name && *name) {
        std::lock_guard<std::mutex> lk(st.mutex);
        st.counters[std::wstring(name)] = value;
    }
    RecordOk("PIXReportCounter");
}

int32_t OS_SHIM_CALL PIXGpuCaptureNextFrames(const wchar_t* fileName, uint32_t numFrames) {
    if (numFrames == 0u) {
        return RecordHr("PIXGpuCaptureNextFrames", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetPix();
    st.gpu_capture_requests.fetch_add(1u, std::memory_order_relaxed);
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        st.last_capture_file = (fileName && *fileName) ? std::wstring(fileName) : L"capture.wpix";
    }
    return RecordHr("PIXGpuCaptureNextFrames", OS_SHIM_S_OK);
}

uint64_t OS_SHIM_CALL PIXGetEventCountForTest(void) {
    RecordOk("PIXGetEventCountForTest");
    return GetPix().event_count.load(std::memory_order_acquire);
}

}  // extern "C"
