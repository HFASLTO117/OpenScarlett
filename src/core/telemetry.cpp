#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
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
using PfnHookIsTelemetryActive = int(__cdecl*)();

struct HookForwardCache {
    std::atomic<int> init_state{0};  // 0 = unchecked, 1 = forward, 2 = self/disabled
    std::atomic<PfnHookRecordCall> record_fn{nullptr};
    std::atomic<PfnHookIsTelemetryActive> active_fn{nullptr};
};

HookForwardCache& GetHookForwardCache() {
    static HookForwardCache cache;
    return cache;
}

void EnsureHookForwardResolved(HookForwardCache& cache) noexcept {
    int state = cache.init_state.load(std::memory_order_acquire);
    if (state != 0) return;

    HMODULE hook_mod = GetModuleHandleA("openscarlett_hook.dll");
    if (!hook_mod) return;

    HMODULE self_mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&EnsureHookForwardResolved),
                       &self_mod);
    if (self_mod == hook_mod) {
        cache.init_state.store(2, std::memory_order_release);
        return;
    }

    auto rec_fn = reinterpret_cast<PfnHookRecordCall>(
        GetProcAddress(hook_mod, "OpenScarlettHook_RecordCall"));
    auto act_fn = reinterpret_cast<PfnHookIsTelemetryActive>(
        GetProcAddress(hook_mod, "OpenScarlettHook_IsTelemetryActive"));
    if (act_fn) {
        cache.active_fn.store(act_fn, std::memory_order_release);
    }
    if (rec_fn) {
        cache.record_fn.store(rec_fn, std::memory_order_release);
        cache.init_state.store(1, std::memory_order_release);
    }
}

void ForwardToHookDllIfPresent(const char* module_name,
                               const char* symbol_name,
                               CallOutcome outcome,
                               int32_t hr,
                               int32_t vtable_slot) {
    auto& cache = GetHookForwardCache();
    int state = cache.init_state.load(std::memory_order_acquire);
    if (state == 2) return;
    if (state == 0) {
        EnsureHookForwardResolved(cache);
        state = cache.init_state.load(std::memory_order_acquire);
        if (state != 1) return;
    }
    if (auto act_fn = cache.active_fn.load(std::memory_order_acquire)) {
        if (act_fn() == 0) return;
    }
    if (auto fn = cache.record_fn.load(std::memory_order_acquire)) {
        fn(module_name, symbol_name, static_cast<uint8_t>(outcome), hr, vtable_slot);
    }
}
#endif

inline uint64_t HashKey(const char* mod, const char* sym, int32_t slot) noexcept {
    uint64_t h = 14695981039346656037ull;
    if (mod) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(mod); *p; ++p) {
            h ^= *p;
            h *= 1099511628211ull;
        }
    }
    h ^= 0x21ull;
    h *= 1099511628211ull;
    if (sym) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(sym); *p; ++p) {
            h ^= *p;
            h *= 1099511628211ull;
        }
    }
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(slot));
    h *= 1099511628211ull;
    return h;
}

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

bool CallTelemetry::IsHookTelemetryActive() noexcept {
#if defined(_WIN32)
    auto& cache = GetHookForwardCache();
    int state = cache.init_state.load(std::memory_order_acquire);
    if (state == 0) {
        EnsureHookForwardResolved(cache);
        state = cache.init_state.load(std::memory_order_acquire);
    }
    if (state == 1) {
        if (auto act_fn = cache.active_fn.load(std::memory_order_acquire)) {
            return act_fn() != 0;
        }
        return true;
    }
#endif
    return false;
}

void CallTelemetry::Reset() {
    generation_.fetch_add(1ull, std::memory_order_acq_rel);
    std::vector<std::unique_ptr<AtomicEntry>> moved;
    for (auto& shard : shards_) {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        for (auto& [_, vec] : shard.buckets) {
            for (auto& ptr : vec) {
                moved.push_back(std::move(ptr));
            }
        }
        shard.buckets.clear();
    }
    if (!moved.empty()) {
        std::unique_lock<std::shared_mutex> rlock(retired_mutex_);
        retired_entries_.insert(
            retired_entries_.end(),
            std::make_move_iterator(moved.begin()),
            std::make_move_iterator(moved.end()));
    }
}

void CallTelemetry::Record(const char* module_name,
                           const char* symbol_name,
                           CallOutcome outcome,
                           int32_t hr,
                           int32_t vtable_slot) {
    const char* mod = module_name ? module_name : "";
    const char* sym = symbol_name ? symbol_name : "";

    auto bump = [&](AtomicEntry& e) noexcept {
        e.total_calls.fetch_add(1ull, std::memory_order_relaxed);
        e.last_hr.store(hr, std::memory_order_relaxed);
        switch (outcome) {
            case CallOutcome::Ok:
                e.ok_calls.fetch_add(1ull, std::memory_order_relaxed);
                break;
            case CallOutcome::StubOk:
                e.stub_calls.fetch_add(1ull, std::memory_order_relaxed);
                break;
            case CallOutcome::NotImplemented:
                e.not_impl_calls.fetch_add(1ull, std::memory_order_relaxed);
                break;
            case CallOutcome::Error:
                e.error_calls.fetch_add(1ull, std::memory_order_relaxed);
                break;
        }
    };

    // Lock-free thread-local direct-mapped cache keyed by (instance, generation, mod ptr, sym ptr, slot).
    // String-literal callers hit in ~3 ns with zero FNV-1a hashing and zero SRWLock acquisition.
    constexpr size_t kTlCacheSize = 64u;
    struct TlSlot {
        const CallTelemetry* owner = nullptr;
        uint64_t generation = 0ull;
        const char* mod_ptr = nullptr;
        const char* sym_ptr = nullptr;
        int32_t slot = -1;
        AtomicEntry* entry = nullptr;
    };
    thread_local std::array<TlSlot, kTlCacheSize> tl_cache{};

    const uint64_t cur_gen = generation_.load(std::memory_order_acquire);
    const uintptr_t ptr_mix =
        (reinterpret_cast<uintptr_t>(mod) >> 3u) ^
        (reinterpret_cast<uintptr_t>(sym) >> 2u) ^
        static_cast<uintptr_t>(static_cast<uint32_t>(vtable_slot) * 2654435761u);
    TlSlot& cache_slot = tl_cache[(ptr_mix ^ (ptr_mix >> 6u)) & (kTlCacheSize - 1u)];

    if (cache_slot.owner == this &&
        cache_slot.generation == cur_gen &&
        cache_slot.mod_ptr == mod &&
        cache_slot.sym_ptr == sym &&
        cache_slot.slot == vtable_slot &&
        cache_slot.entry != nullptr) {
        bump(*cache_slot.entry);
#if defined(_WIN32)
        ForwardToHookDllIfPresent(mod, sym, outcome, hr, vtable_slot);
#endif
        return;
    }

    const uint64_t h = HashKey(mod, sym, vtable_slot);
    auto& shard = shards_[static_cast<size_t>(h) & (kShardCount - 1u)];

    AtomicEntry* found = nullptr;
    {
        std::shared_lock<std::shared_mutex> rlock(shard.mutex);
        auto it = shard.buckets.find(h);
        if (it != shard.buckets.end()) {
            for (const auto& entry_ptr : it->second) {
                if (entry_ptr->vtable_slot == vtable_slot &&
                    entry_ptr->module_name == mod &&
                    entry_ptr->symbol_name == sym) {
                    found = entry_ptr.get();
                    bump(*found);
                    break;
                }
            }
        }
    }

    if (!found) {
        std::unique_lock<std::shared_mutex> wlock(shard.mutex);
        auto& vec = shard.buckets[h];
        for (const auto& entry_ptr : vec) {
            if (entry_ptr->vtable_slot == vtable_slot &&
                entry_ptr->module_name == mod &&
                entry_ptr->symbol_name == sym) {
                found = entry_ptr.get();
                break;
            }
        }
        if (!found) {
            auto node = std::make_unique<AtomicEntry>();
            node->module_name = mod;
            node->symbol_name = sym;
            node->vtable_slot = vtable_slot;
            found = node.get();
            vec.push_back(std::move(node));
        }
        bump(*found);
    }

    if (found && generation_.load(std::memory_order_acquire) == cur_gen) {
        cache_slot.owner = this;
        cache_slot.generation = cur_gen;
        cache_slot.mod_ptr = mod;
        cache_slot.sym_ptr = sym;
        cache_slot.slot = vtable_slot;
        cache_slot.entry = found;
    }

#if defined(_WIN32)
    ForwardToHookDllIfPresent(mod, sym, outcome, hr, vtable_slot);
#endif
}

void CallTelemetry::Record(const std::string& module_name,
                           const std::string& symbol_name,
                           CallOutcome outcome,
                           int32_t hr,
                           int32_t vtable_slot) {
    Record(module_name.c_str(), symbol_name.c_str(), outcome, hr, vtable_slot);
}

std::vector<CallTraceEntry> CallTelemetry::Snapshot() const {
    std::vector<CallTraceEntry> out;
    for (const auto& shard : shards_) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (const auto& [_, vec] : shard.buckets) {
            for (const auto& entry_ptr : vec) {
                CallTraceEntry e{};
                e.module_name = entry_ptr->module_name;
                e.symbol_name = entry_ptr->symbol_name;
                e.vtable_slot = entry_ptr->vtable_slot;
                e.total_calls = entry_ptr->total_calls.load(std::memory_order_relaxed);
                e.ok_calls = entry_ptr->ok_calls.load(std::memory_order_relaxed);
                e.stub_calls = entry_ptr->stub_calls.load(std::memory_order_relaxed);
                e.not_impl_calls = entry_ptr->not_impl_calls.load(std::memory_order_relaxed);
                e.error_calls = entry_ptr->error_calls.load(std::memory_order_relaxed);
                e.last_hr = entry_ptr->last_hr.load(std::memory_order_relaxed);
                out.push_back(std::move(e));
            }
        }
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
