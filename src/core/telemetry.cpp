#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace openscarlett {

namespace {

#if defined(_WIN32)
using PfnHookRecordCall = void(__cdecl*)(const char* module_name,
                                         const char* symbol_name,
                                         uint8_t outcome,
                                         int32_t hr,
                                         int32_t vtable_slot);

void ForwardToHookDllIfPresent(const std::string& module_name,
                               const std::string& symbol_name,
                               CallOutcome outcome,
                               int32_t hr,
                               int32_t vtable_slot) {
    static std::atomic<int> init_state{0};  // 0 = unchecked, 1 = forward, 2 = self/disabled
    static std::atomic<PfnHookRecordCall> cached_fn{nullptr};

    int state = init_state.load(std::memory_order_acquire);
    if (state == 2) return;

    PfnHookRecordCall fn = cached_fn.load(std::memory_order_acquire);
    if (!fn) {
        HMODULE hook_mod = GetModuleHandleA("openscarlett_hook.dll");
        if (!hook_mod) return;
        HMODULE self_mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ForwardToHookDllIfPresent),
                           &self_mod);
        if (self_mod == hook_mod) {
            init_state.store(2, std::memory_order_release);
            return;
        }
        fn = reinterpret_cast<PfnHookRecordCall>(
            GetProcAddress(hook_mod, "OpenScarlettHook_RecordCall"));
        if (fn) {
            cached_fn.store(fn, std::memory_order_release);
            init_state.store(1, std::memory_order_release);
        }
    }
    if (fn) {
        fn(module_name.c_str(), symbol_name.c_str(), static_cast<uint8_t>(outcome), hr, vtable_slot);
    }
}
#endif

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

}  // namespace

CallTelemetry& CallTelemetry::Global() {
    static CallTelemetry instance;
    return instance;
}

void CallTelemetry::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

void CallTelemetry::Record(const std::string& module_name,
                           const std::string& symbol_name,
                           CallOutcome outcome,
                           int32_t hr,
                           int32_t vtable_slot) {
    std::string key = module_name + "!" + symbol_name + "#" + std::to_string(vtable_slot);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& e = entries_[key];
    if (e.total_calls == 0) {
        e.module_name = module_name;
        e.symbol_name = symbol_name;
        e.vtable_slot = vtable_slot;
    }
    ++e.total_calls;
    e.last_hr = hr;
    switch (outcome) {
        case CallOutcome::Ok: ++e.ok_calls; break;
        case CallOutcome::StubOk: ++e.stub_calls; break;
        case CallOutcome::NotImplemented: ++e.not_impl_calls; break;
        case CallOutcome::Error: ++e.error_calls; break;
    }
#if defined(_WIN32)
    ForwardToHookDllIfPresent(module_name, symbol_name, outcome, hr, vtable_slot);
#endif
}

std::vector<CallTraceEntry> CallTelemetry::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CallTraceEntry> out;
    out.reserve(entries_.size());
    for (const auto& [_, e] : entries_) {
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const CallTraceEntry& a, const CallTraceEntry& b) {
        if (a.not_impl_calls != b.not_impl_calls) return a.not_impl_calls > b.not_impl_calls;
        if (a.total_calls != b.total_calls) return a.total_calls > b.total_calls;
        if (a.module_name != b.module_name) return a.module_name < b.module_name;
        return a.symbol_name < b.symbol_name;
    });
    return out;
}

Status CallTelemetry::WriteJsonReport(const std::string& file_path) const {
    auto snap = Snapshot();
    std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::Error(StatusCode::IoError, "Failed to open telemetry report path: " + file_path);
    }
    ofs << "{\n  \"entries\": [\n";
    for (size_t i = 0; i < snap.size(); ++i) {
        const auto& e = snap[i];
        ofs << "    {"
            << "\"module\": \"" << EscapeJson(e.module_name) << "\", "
            << "\"symbol\": \"" << EscapeJson(e.symbol_name) << "\", "
            << "\"slot\": " << e.vtable_slot << ", "
            << "\"total\": " << e.total_calls << ", "
            << "\"ok\": " << e.ok_calls << ", "
            << "\"stub\": " << e.stub_calls << ", "
            << "\"not_impl\": " << e.not_impl_calls << ", "
            << "\"errors\": " << e.error_calls << ", "
            << "\"last_hr\": " << e.last_hr
            << "}" << (i + 1 < snap.size() ? "," : "") << "\n";
    }
    ofs << "  ]\n}\n";
    return Status::Ok();
}

}  // namespace openscarlett
