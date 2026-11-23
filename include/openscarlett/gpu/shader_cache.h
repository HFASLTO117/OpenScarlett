#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"

namespace openscarlett {

constexpr uint32_t kShaderCacheFormatVersion = 1u;

struct ShaderCacheKey {
    uint64_t lo = 0;
    uint64_t hi = 0;

    bool operator==(const ShaderCacheKey& o) const noexcept {
        return lo == o.lo && hi == o.hi;
    }
    bool operator!=(const ShaderCacheKey& o) const noexcept {
        return !(*this == o);
    }
    std::string ToHex() const;
};

struct ShaderCacheKeyHash {
    size_t operator()(const ShaderCacheKey& k) const noexcept {
        return static_cast<size_t>(k.lo ^ (k.hi * 0x9E3779B97F4A7C15ull));
    }
};

struct ShaderCacheStats {
    uint64_t memory_hits = 0;
    uint64_t disk_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t corrupt_entries = 0;
    uint64_t async_compiles_queued = 0;
    uint64_t async_compiles_completed = 0;
    uint64_t async_compiles_failed = 0;
    uint64_t sync_compiles_succeeded = 0;
    uint64_t sync_compiles_failed = 0;
    uint64_t total_compile_failures = 0;
    uint64_t standins_served = 0;
    // Extended Sprint 16 async PSO + persistent disk cache telemetry (appended for ABI compatibility)
    uint64_t total_compile_time_us = 0;
    uint64_t avg_compile_time_us = 0;
    uint64_t disk_bytes_read = 0;
    uint64_t disk_bytes_written = 0;
    uint64_t active_worker_threads = 0;
    uint64_t peak_queue_depth = 0;
    uint64_t hot_swaps_ready = 0;
    uint64_t disk_entries_pruned = 0;
    uint64_t async_dedup_hits = 0;
    uint64_t async_priority_promotions = 0;
};

enum class ShaderCachePriority : uint8_t {
    Prewarm = 0,
    Normal = 1,
    Urgent = 2,
};

struct ShaderCacheDiskEntryInfo {
    std::string key_hex;
    std::string path;
    ShaderStage stage = ShaderStage::Unknown;
    std::string target_profile;
    uint64_t size_bytes = 0;
    uint64_t last_write_epoch_sec = 0;
    bool valid_crc32 = false;
};

struct CachedShaderResult {
    ShaderCacheKey key{};
    ShaderStage stage = ShaderStage::Unknown;
    std::string target_profile;
    std::vector<uint8_t> dxil;
    bool from_memory_cache = false;
    bool from_disk_cache = false;
    bool is_standin_async = false;
    std::string diagnostics;
};

using AsyncCompileCompletionCallback = std::function<void(
    const ShaderCacheKey& key,
    const CachedShaderResult& result,
    const Status& status)>;

struct ShaderCacheLiftRequest {
    uint32_t highest_shader_model = 0x65u;  // D3D_SHADER_MODEL (e.g. 0x65=SM6.5, 0x62=SM6.2)
    bool mesh_has_payload = true;           // Relevant for ShaderStage::Mesh (true iff AS is bound)
    bool prefer_dxil_standin = false;       // True for multi-stage graphics/mesh PSOs to match DXIL
    std::string dxc_path;                   // Optional explicit dxc.exe path (empty = portable lookup)
    ShaderCachePriority priority = ShaderCachePriority::Normal;
};

// Deterministic 128-bit content hash over (input_blob, target_profile, shader_model_flags, format_version).
ShaderCacheKey ComputeShaderCacheKey(
    const void* input_blob,
    size_t input_size,
    const std::string& target_profile,
    uint32_t shader_model_flags,
    uint32_t cache_format_version = kShaderCacheFormatVersion) noexcept;

// Selects the optimal host HLSL target profile for a given stage and adapter highest_shader_model.
std::string SelectTargetProfileForStage(ShaderStage stage, uint32_t highest_shader_model);

// Thread-safe content-hashed in-memory + on-disk shader cache with optional async background dxc compilation.
class ShaderCache {
public:
    ShaderCache() = default;
    ~ShaderCache();

    ShaderCache(const ShaderCache&) = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;

    static ShaderCache& Global();

    // Cache directory resolution: explicit SetCacheDirectory -> OPENSCARLETT_SHADER_CACHE -> %TEMP%\openscarlett_shader_cache.
    void SetCacheDirectory(const std::string& dir_path);
    std::string ResolveCacheDirectory() const;
    std::string CacheFilePathForKey(const ShaderCacheKey& key) const;

    // Controls async background dxc PSO compilation mode (defaults to OPENSCARLETT_ASYNC_PSO env var).
    void EnableAsyncPsoCompilation(bool enable);
    void ResetAsyncPsoCompilationOverride();
    bool IsAsyncPsoCompilationEnabled() const;

    // Configures background worker pool size (0 = auto: min(4, max(2, hw_threads / 2)); clamped to [1, 8]).
    void SetAsyncWorkerCount(uint32_t worker_count);
    uint32_t GetAsyncWorkerCount() const noexcept;

    // Registers an optional completion callback invoked outside internal locks when an async
    // compilation finishes, allowing callers to hot-swap stand-in PSOs with zero hitch.
    void SetAsyncCompletionCallback(AsyncCompileCompletionCallback callback);

    // Enqueues an asynchronous background prewarm compile at the requested priority without
    // compiling a stand-in or blocking the caller. Returns Ok immediately on cache hit or enqueue.
    Status PrewarmXboxShaderAsync(
        const void* blob,
        size_t blob_size,
        const ShaderCacheLiftRequest& request,
        ShaderCacheKey* out_key = nullptr,
        ShaderCachePriority priority = ShaderCachePriority::Prewarm);

    // Enumerates on-disk `.ossc` cache entries with IEEE 802.3 CRC32 verification (non-destructive).
    std::vector<ShaderCacheDiskEntryInfo> EnumerateDiskCacheEntries() const;

    // Prunes corrupt entries first, then oldest-modified `.ossc` files until total bytes <= max_bytes
    // (when max_bytes > 0) and entry count <= max_entries (when max_entries > 0). Returns count removed.
    size_t PruneDiskCache(uint64_t max_bytes, size_t max_entries = 0);

    // Clears in-memory cache entries (preserving on-disk cache files).
    void ClearMemoryCache();

    // Resets internal hit/miss/async statistics counters and last diagnostic error.
    void ResetStats();
    ShaderCacheStats Stats() const noexcept;
    std::string LastCompileError() const;
    std::string FormatDiagnosticsSummary() const;

    // Direct cache lookup / store by key (validates header magic + checksum on disk).
    bool TryGet(const ShaderCacheKey& key, CachedShaderResult& out_result);
    Status Put(const ShaderCacheKey& key,
               ShaderStage stage,
               const std::string& target_profile,
               const std::vector<uint8_t>& dxil);

    // High-level Xbox shader auto-lift + cache lookup (handles XS2\0 and Xbox-wrapped DXBC).
    // When async mode is active and a cache miss occurs, returns a valid stand-in host shader
    // immediately while compiling the real lifted DXIL on a background worker thread.
    Status GetOrLiftXboxShader(
        const void* blob,
        size_t blob_size,
        const ShaderCacheLiftRequest& request,
        CachedShaderResult& out_result,
        std::string* out_diagnostics = nullptr);

    // Returns true once the real compiled DXIL for `key` is available in memory/disk cache.
    bool IsCompileReady(const ShaderCacheKey& key) const;

    // Blocks until all queued async background compilations have finished and joins worker threads.
    void WaitForPendingCompiles();

private:
    struct MemoryEntry {
        ShaderStage stage = ShaderStage::Unknown;
        std::string target_profile;
        std::vector<uint8_t> dxil;
    };

    struct AsyncJob {
        ShaderCacheKey key{};
        std::vector<uint8_t> blob_bytes;
        ShaderStage stage = ShaderStage::Unknown;
        std::string target_profile;
        ShaderCacheLiftRequest request{};
        ShaderCachePriority priority = ShaderCachePriority::Normal;
        uint64_t sequence = 0;
        bool had_standin = false;
    };

    bool TryLoadFromDisk(const ShaderCacheKey& key, MemoryEntry& out_entry);
    Status SaveToDiskAtomic(const ShaderCacheKey& key, const MemoryEntry& entry);
    Status CompileStandInShader(
        ShaderStage stage,
        const std::string& target_profile,
        bool mesh_has_payload,
        bool prefer_dxil,
        const std::string& dxc_path,
        std::vector<uint8_t>& out_bytecode,
        std::string* out_diagnostics);
    Status CompileRealXboxShaderSync(
        const std::vector<uint8_t>& blob_bytes,
        ShaderStage stage,
        const std::string& target_profile,
        const ShaderCacheLiftRequest& request,
        std::vector<uint8_t>& out_dxil,
        std::string* out_diagnostics);
    void EnqueueAsyncJob(AsyncJob job);
    void WorkerLoop();

    mutable std::mutex cache_mutex_;
    std::string explicit_cache_dir_;
    std::unordered_map<ShaderCacheKey, MemoryEntry, ShaderCacheKeyHash> memory_cache_;
    std::unordered_map<std::string, std::vector<uint8_t>> standin_cache_;

    std::atomic<int> async_override_{-1};  // -1 = env var, 0 = off, 1 = on
    std::atomic<uint32_t> configured_worker_count_{0};  // 0 = auto
    std::atomic<uint64_t> mem_hits_{0};
    std::atomic<uint64_t> disk_hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> corrupt_entries_{0};
    std::atomic<uint64_t> async_queued_{0};
    std::atomic<uint64_t> async_completed_{0};
    std::atomic<uint64_t> async_failed_{0};
    std::atomic<uint64_t> sync_succeeded_{0};
    std::atomic<uint64_t> sync_failed_{0};
    std::atomic<uint64_t> standins_served_{0};
    std::atomic<uint64_t> total_compile_time_us_{0};
    std::atomic<uint64_t> compile_count_{0};
    std::atomic<uint64_t> disk_bytes_read_{0};
    std::atomic<uint64_t> disk_bytes_written_{0};
    std::atomic<uint64_t> active_worker_threads_{0};
    std::atomic<uint64_t> peak_queue_depth_{0};
    std::atomic<uint64_t> hot_swaps_ready_{0};
    std::atomic<uint64_t> disk_entries_pruned_{0};
    std::atomic<uint64_t> async_dedup_hits_{0};
    std::atomic<uint64_t> async_priority_promotions_{0};
    std::string last_compile_error_;

    mutable std::mutex async_mutex_;
    std::condition_variable async_cv_;
    std::vector<AsyncJob> job_queue_;
    std::unordered_set<ShaderCacheKey, ShaderCacheKeyHash> inflight_keys_;
    std::unordered_set<ShaderCacheKey, ShaderCacheKeyHash> standin_pending_keys_;
    AsyncCompileCompletionCallback completion_callback_;
    uint64_t next_job_seq_ = 1;
    std::vector<std::thread> workers_;
    uint32_t active_workers_ = 0;
    uint32_t active_jobs_ = 0;
};

}  // namespace openscarlett
