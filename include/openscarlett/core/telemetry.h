#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

enum class CallOutcome : uint8_t {
    Ok = 0,
    StubOk = 1,
    NotImplemented = 2,
    Error = 3,
};

struct CallTraceEntry {
    std::string module_name;
    std::string symbol_name;
    int32_t vtable_slot = -1;  // >= 0 for COM interface vtable slots
    uint64_t total_calls = 0;
    uint64_t ok_calls = 0;
    uint64_t stub_calls = 0;
    uint64_t not_impl_calls = 0;
    uint64_t error_calls = 0;
    int32_t last_hr = 0;
};

// Thread-safe live call & COM vtable telemetry collector used during real-binary bring-up.
class CallTelemetry {
public:
    static CallTelemetry& Global();

    void Reset();
    void Record(const std::string& module_name,
                const std::string& symbol_name,
                CallOutcome outcome,
                int32_t hr = 0,
                int32_t vtable_slot = -1);

    std::vector<CallTraceEntry> Snapshot() const;
    Status WriteJsonReport(const std::string& file_path) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CallTraceEntry> entries_;
};

}  // namespace openscarlett
