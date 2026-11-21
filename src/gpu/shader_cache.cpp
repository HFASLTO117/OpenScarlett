#include "openscarlett/gpu/shader_cache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "openscarlett/core/telemetry.h"

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <windows.h>
#endif

namespace openscarlett {

namespace {

constexpr const char* kCacheTelemetryModule = "shader_cache";
constexpr char kCacheFileMagic[4] = {'O', 'S', 'S', 'C'};

struct OnDiskCacheHeader {
    char magic[4];
    uint32_t format_version;
    uint64_t key_lo;
    uint64_t key_hi;
    uint32_t stage;
    uint32_t target_len;
    uint32_t dxil_size;
    uint32_t reserved;
    uint64_t payload_checksum;
};
static_assert(sizeof(OnDiskCacheHeader) == 48u, "Unexpected OnDiskCacheHeader size");

uint64_t Mix64(uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

uint64_t HashPayloadChecksum(const std::string& target, const std::vector<uint8_t>& dxil) noexcept {
    uint64_t h = 0xCBF29CE484222325ull;
    for (char c : target) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001B3ull;
    }
    h ^= 0xFFull;
    h *= 0x100000001B3ull;
    for (uint8_t b : dxil) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    return Mix64(h ^ static_cast<uint64_t>(dxil.size()));
}

void UpdateHash128(uint64_t& h0, uint64_t& h1, const uint8_t* data, size_t size) noexcept {
    for (size_t i = 0; i < size; ++i) {
        const uint64_t b = data[i];
        h0 ^= b;
        h0 *= 0x100000001B3ull;
        h1 ^= (b + 0x9E3779B97F4A7C15ull + (h0 >> 17));
        h1 *= 0x94D049BB133111EBull;
    }
}

void UpdateHash128U32(uint64_t& h0, uint64_t& h1, uint32_t v) noexcept {
    uint8_t buf[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu),
    };
    UpdateHash128(h0, h1, buf, sizeof(buf));
}

bool EndsWith50(const std::string& target) noexcept {
    return target.size() >= 4 && target.compare(target.size() - 4, 4, "_5_0") == 0;
}

#if defined(_WIN32)
Status CompileHlslWithFxc50(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_bytecode,
    std::string* out_diagnostics) {
    out_bytecode.clear();
    ID3DBlob* code_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    const HRESULT hr = D3DCompile(
        hlsl.data(),
        hlsl.size(),
        "openscarlett_shader.hlsl",
        nullptr,
        nullptr,
        entry_point.c_str(),
        target.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL1,
        0,
        &code_blob,
        &err_blob);
    if (err_blob) {
        if (out_diagnostics && err_blob->GetBufferPointer() && err_blob->GetBufferSize() > 0) {
            out_diagnostics->assign(
                static_cast<const char*>(err_blob->GetBufferPointer()),
                err_blob->GetBufferSize());
        }
        err_blob->Release();
    }
    if (FAILED(hr) || !code_blob) {
        if (code_blob) code_blob->Release();
        return Status::Error(StatusCode::ShaderError, "D3DCompile failed for target " + target);
    }
    const auto* ptr = static_cast<const uint8_t*>(code_blob->GetBufferPointer());
    out_bytecode.assign(ptr, ptr + code_blob->GetBufferSize());
    code_blob->Release();
    return Status::Ok();
}
#endif

std::string BuildStandInHlsl(ShaderStage stage, bool mesh_has_payload) {
    switch (stage) {
        case ShaderStage::Compute:
            return "[numthreads(1, 1, 1)] void main() {}\n";
        case ShaderStage::Vertex:
            return "struct VSOut { float4 pos : SV_POSITION; float4 param0 : TEXCOORD0; };\n"
                   "VSOut main() { VSOut o; o.pos = float4(0.0, 0.0, 0.0, 1.0); "
                   "o.param0 = float4(0.0, 0.0, 0.0, 1.0); return o; }\n";
        case ShaderStage::Pixel:
            return "float4 main() : SV_TARGET0 { return float4(0.0, 0.0, 0.0, 1.0); }\n";
        case ShaderStage::Mesh:
            if (mesh_has_payload) {
                return "struct ASPayload { uint _data[4]; };\n"
                       "struct MeshVert { float4 pos : SV_POSITION; float4 param0 : TEXCOORD0; };\n"
                       "[outputtopology(\"triangle\")] [numthreads(1, 1, 1)]\n"
                       "void main(in payload ASPayload _p, out vertices MeshVert v[3], out indices uint3 pr[1]) {\n"
                       "    SetMeshOutputCounts(0, 0);\n"
                       "}\n";
            }
            return "struct MeshVert { float4 pos : SV_POSITION; float4 param0 : TEXCOORD0; };\n"
                   "[outputtopology(\"triangle\")] [numthreads(1, 1, 1)]\n"
                   "void main(out vertices MeshVert v[3], out indices uint3 pr[1]) {\n"
                   "    SetMeshOutputCounts(0, 0);\n"
                   "}\n";
        case ShaderStage::Amplification:
            return "struct ASPayload { uint _data[4]; };\n"
                   "groupshared ASPayload _p;\n"
                   "[numthreads(1, 1, 1)] void main() {\n"
                   "    _p._data[0] = 0u; _p._data[1] = 0u; _p._data[2] = 0u; _p._data[3] = 0u;\n"
                   "    DispatchMesh(0, 0, 0, _p);\n"
                   "}\n";
        default:
            return "[numthreads(1, 1, 1)] void main() {}\n";
    }
}

std::string StageStandInFxcTarget(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return "vs_5_0";
        case ShaderStage::Pixel:  return "ps_5_0";
        case ShaderStage::Compute:
        default:                  return "cs_5_0";
    }
}

}  // namespace

std::string ShaderCacheKey::ToHex() const {
    char buf[33]{};
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf);
}

ShaderCacheKey ComputeShaderCacheKey(
    const void* input_blob,
    size_t input_size,
    const std::string& target_profile,
    uint32_t shader_model_flags,
    uint32_t cache_format_version) noexcept {
    uint64_t h0 = 0xCBF29CE484222325ull;
    uint64_t h1 = 0x84222325CBF29CE4ull ^ 0x9E3779B97F4A7C15ull;

    UpdateHash128U32(h0, h1, cache_format_version);
    UpdateHash128U32(h0, h1, shader_model_flags);
    UpdateHash128U32(h0, h1, static_cast<uint32_t>(target_profile.size()));
    if (!target_profile.empty()) {
        UpdateHash128(h0, h1,
                      reinterpret_cast<const uint8_t*>(target_profile.data()),
                      target_profile.size());
    }
    UpdateHash128U32(h0, h1, static_cast<uint32_t>(input_size));
    if (input_blob && input_size > 0) {
        UpdateHash128(h0, h1, static_cast<const uint8_t*>(input_blob), input_size);
    }

    ShaderCacheKey k{};
    k.lo = Mix64(h0 ^ (h1 >> 13));
    k.hi = Mix64(h1 ^ (h0 << 7));
    return k;
}

std::string SelectTargetProfileForStage(ShaderStage stage, uint32_t highest_shader_model) {
    if (stage == ShaderStage::Mesh) return "ms_6_5";
    if (stage == ShaderStage::Amplification) return "as_6_5";

    const char* prefix = "cs";
    if (stage == ShaderStage::Vertex) prefix = "vs";
    else if (stage == ShaderStage::Pixel) prefix = "ps";

    if (highest_shader_model >= 0x65u) return std::string(prefix) + "_6_5";
    if (highest_shader_model >= 0x62u) return std::string(prefix) + "_6_2";
    if (highest_shader_model >= 0x60u) return std::string(prefix) + "_6_0";
    return std::string(prefix) + "_5_0";
}

ShaderCache& ShaderCache::Global() {
    static ShaderCache g_cache;
    return g_cache;
}

ShaderCache::~ShaderCache() {
    WaitForPendingCompiles();
}

void ShaderCache::SetCacheDirectory(const std::string& dir_path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    explicit_cache_dir_ = dir_path;
}

std::string ShaderCache::ResolveCacheDirectory() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (!explicit_cache_dir_.empty()) {
            return explicit_cache_dir_;
        }
    }
#if defined(_WIN32)
    char env_buf[1024]{};
    DWORD len = GetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (len > 0 && len < sizeof(env_buf)) {
        return std::string(env_buf, len);
    }
    len = GetEnvironmentVariableA("TEMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (len == 0 || len >= sizeof(env_buf)) {
        len = GetEnvironmentVariableA("TMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    }
    if (len > 0 && len < sizeof(env_buf)) {
        return (std::filesystem::path(std::string(env_buf, len)) / "openscarlett_shader_cache").string();
    }
#endif
    std::error_code ec;
    return (std::filesystem::temp_directory_path(ec) / "openscarlett_shader_cache").string();
}

std::string ShaderCache::CacheFilePathForKey(const ShaderCacheKey& key) const {
    return (std::filesystem::path(ResolveCacheDirectory()) / (key.ToHex() + ".ossc")).string();
}

void ShaderCache::EnableAsyncPsoCompilation(bool enable) {
    async_override_.store(enable ? 1 : 0, std::memory_order_release);
}

void ShaderCache::ResetAsyncPsoCompilationOverride() {
    async_override_.store(-1, std::memory_order_release);
}

bool ShaderCache::IsAsyncPsoCompilationEnabled() const {
    const int ov = async_override_.load(std::memory_order_acquire);
    if (ov >= 0) return ov != 0;
#if defined(_WIN32)
    char buf[64]{};
    const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        return std::strcmp(buf, "1") == 0 ||
               std::strcmp(buf, "true") == 0 ||
               std::strcmp(buf, "TRUE") == 0;
    }
#endif
    return false;
}

void ShaderCache::ClearMemoryCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    memory_cache_.clear();
}

void ShaderCache::ResetStats() {
    mem_hits_.store(0, std::memory_order_relaxed);
    disk_hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    corrupt_entries_.store(0, std::memory_order_relaxed);
    async_queued_.store(0, std::memory_order_relaxed);
    async_completed_.store(0, std::memory_order_relaxed);
    async_failed_.store(0, std::memory_order_relaxed);
}

ShaderCacheStats ShaderCache::Stats() const noexcept {
    ShaderCacheStats s{};
    s.memory_hits = mem_hits_.load(std::memory_order_relaxed);
    s.disk_hits = disk_hits_.load(std::memory_order_relaxed);
    s.cache_misses = misses_.load(std::memory_order_relaxed);
    s.corrupt_entries = corrupt_entries_.load(std::memory_order_relaxed);
    s.async_compiles_queued = async_queued_.load(std::memory_order_relaxed);
    s.async_compiles_completed = async_completed_.load(std::memory_order_relaxed);
    s.async_compiles_failed = async_failed_.load(std::memory_order_relaxed);
    return s;
}

bool ShaderCache::TryLoadFromDisk(const ShaderCacheKey& key, MemoryEntry& out_entry) {
    const std::string path = CacheFilePathForKey(key);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }

    auto mark_corrupt = [&]() {
        corrupt_entries_.fetch_add(1, std::memory_order_relaxed);
        std::filesystem::remove(path, ec);
    };

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return false;
    }
    const std::streamsize file_size = ifs.tellg();
    if (file_size < static_cast<std::streamsize>(sizeof(OnDiskCacheHeader))) {
        ifs.close();
        mark_corrupt();
        return false;
    }
    ifs.seekg(0, std::ios::beg);

    OnDiskCacheHeader hdr{};
    ifs.read(reinterpret_cast<char*>(&hdr), static_cast<std::streamsize>(sizeof(hdr)));
    if (!ifs ||
        std::memcmp(hdr.magic, kCacheFileMagic, 4) != 0 ||
        hdr.format_version != kShaderCacheFormatVersion ||
        hdr.key_lo != key.lo ||
        hdr.key_hi != key.hi ||
        hdr.target_len == 0u || hdr.target_len > 64u ||
        hdr.dxil_size < 32u || hdr.dxil_size > (64u * 1024u * 1024u)) {
        ifs.close();
        mark_corrupt();
        return false;
    }

    const uint64_t expected_size = sizeof(OnDiskCacheHeader) +
                                   static_cast<uint64_t>(hdr.target_len) +
                                   static_cast<uint64_t>(hdr.dxil_size);
    if (static_cast<uint64_t>(file_size) != expected_size) {
        ifs.close();
        mark_corrupt();
        return false;
    }

    std::string target(hdr.target_len, '\0');
    std::vector<uint8_t> dxil(hdr.dxil_size);
    ifs.read(target.data(), static_cast<std::streamsize>(hdr.target_len));
    ifs.read(reinterpret_cast<char*>(dxil.data()), static_cast<std::streamsize>(hdr.dxil_size));
    ifs.close();

    if (std::memcmp(dxil.data(), "DXBC", 4) != 0) {
        mark_corrupt();
        return false;
    }
    if (HashPayloadChecksum(target, dxil) != hdr.payload_checksum) {
        mark_corrupt();
        return false;
    }

    out_entry.stage = static_cast<ShaderStage>(hdr.stage);
    out_entry.target_profile = std::move(target);
    out_entry.dxil = std::move(dxil);
    return true;
}

Status ShaderCache::SaveToDiskAtomic(const ShaderCacheKey& key, const MemoryEntry& entry) {
    static std::atomic<uint64_t> g_tmp_seq{1};
    const std::string dir = ResolveCacheDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Status::Error(StatusCode::IoError, "Failed to create shader cache directory: " + dir);
    }

    const std::filesystem::path final_path = std::filesystem::path(dir) / (key.ToHex() + ".ossc");
    uint32_t pid = 0;
#if defined(_WIN32)
    pid = static_cast<uint32_t>(GetCurrentProcessId());
#endif
    const uint64_t seq = g_tmp_seq.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream tmp_name;
    tmp_name << key.ToHex() << "." << pid << "." << seq << ".tmp";
    const std::filesystem::path tmp_path = std::filesystem::path(dir) / tmp_name.str();

    OnDiskCacheHeader hdr{};
    std::memcpy(hdr.magic, kCacheFileMagic, 4);
    hdr.format_version = kShaderCacheFormatVersion;
    hdr.key_lo = key.lo;
    hdr.key_hi = key.hi;
    hdr.stage = static_cast<uint32_t>(entry.stage);
    hdr.target_len = static_cast<uint32_t>(entry.target_profile.size());
    hdr.dxil_size = static_cast<uint32_t>(entry.dxil.size());
    hdr.reserved = 0u;
    hdr.payload_checksum = HashPayloadChecksum(entry.target_profile, entry.dxil);

    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return Status::Error(StatusCode::IoError, "Failed to open shader cache temp file");
        }
        ofs.write(reinterpret_cast<const char*>(&hdr), static_cast<std::streamsize>(sizeof(hdr)));
        ofs.write(entry.target_profile.data(), static_cast<std::streamsize>(entry.target_profile.size()));
        ofs.write(reinterpret_cast<const char*>(entry.dxil.data()), static_cast<std::streamsize>(entry.dxil.size()));
        ofs.flush();
        if (!ofs) {
            ofs.close();
            std::filesystem::remove(tmp_path, ec);
            return Status::Error(StatusCode::IoError, "Failed to write shader cache temp file");
        }
    }

#if defined(_WIN32)
    if (!MoveFileExA(tmp_path.string().c_str(), final_path.string().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp_path, ec);
        return Status::Error(StatusCode::IoError, "Atomic rename failed for shader cache entry");
    }
#else
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return Status::Error(StatusCode::IoError, "Rename failed for shader cache entry");
    }
#endif
    return Status::Ok();
}

bool ShaderCache::TryGet(const ShaderCacheKey& key, CachedShaderResult& out_result) {
    out_result = CachedShaderResult{};
    out_result.key = key;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = memory_cache_.find(key);
        if (it != memory_cache_.end()) {
            out_result.stage = it->second.stage;
            out_result.target_profile = it->second.target_profile;
            out_result.dxil = it->second.dxil;
            out_result.from_memory_cache = true;
            mem_hits_.fetch_add(1, std::memory_order_relaxed);
            CallTelemetry::Global().Record(kCacheTelemetryModule, "MemoryHit", CallOutcome::Ok, 0);
            return true;
        }
    }

    MemoryEntry disk_entry{};
    if (TryLoadFromDisk(key, disk_entry)) {
        out_result.stage = disk_entry.stage;
        out_result.target_profile = disk_entry.target_profile;
        out_result.dxil = disk_entry.dxil;
        out_result.from_disk_cache = true;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            memory_cache_[key] = std::move(disk_entry);
        }
        disk_hits_.fetch_add(1, std::memory_order_relaxed);
        CallTelemetry::Global().Record(kCacheTelemetryModule, "DiskHit", CallOutcome::Ok, 0);
        return true;
    }

    return false;
}

Status ShaderCache::Put(
    const ShaderCacheKey& key,
    ShaderStage stage,
    const std::string& target_profile,
    const std::vector<uint8_t>& dxil) {
    if (dxil.size() < 32u || std::memcmp(dxil.data(), "DXBC", 4) != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Cannot cache invalid DXBC/DXIL bytecode");
    }
    MemoryEntry entry{};
    entry.stage = stage;
    entry.target_profile = target_profile;
    entry.dxil = dxil;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        memory_cache_[key] = entry;
    }
    return SaveToDiskAtomic(key, entry);
}

Status ShaderCache::CompileStandInShader(
    ShaderStage stage,
    const std::string& target_profile,
    bool mesh_has_payload,
    bool prefer_dxil,
    const std::string& dxc_path,
    std::vector<uint8_t>& out_bytecode,
    std::string* out_diagnostics) {
    const bool needs_dxc = (stage == ShaderStage::Mesh || stage == ShaderStage::Amplification || prefer_dxil);
    const std::string standin_target = needs_dxc ? target_profile : StageStandInFxcTarget(stage);
    const std::string cache_id = std::to_string(static_cast<uint32_t>(stage)) + ":" +
                                 standin_target + ":" + (mesh_has_payload ? "1" : "0");

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = standin_cache_.find(cache_id);
        if (it != standin_cache_.end()) {
            out_bytecode = it->second;
            return Status::Ok();
        }
    }

    const std::string hlsl = BuildStandInHlsl(stage, mesh_has_payload);
    Status st = Status::Ok();
    std::string diag;
#if defined(_WIN32)
    if (!needs_dxc || EndsWith50(standin_target)) {
        st = CompileHlslWithFxc50(hlsl, standin_target, "main", out_bytecode, &diag);
    } else {
        st = CompileHlslWithDxc(hlsl, standin_target, "main", out_bytecode, diag, dxc_path);
    }
#else
    (void)dxc_path;
    st = Status::Error(StatusCode::NotImplemented, "Stand-in shader compilation requires Windows");
#endif
    if (out_diagnostics && !diag.empty()) {
        *out_diagnostics = diag;
    }
    if (!st.ok()) {
        return st;
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        standin_cache_[cache_id] = out_bytecode;
    }
    return Status::Ok();
}

Status ShaderCache::CompileRealXboxShaderSync(
    const std::vector<uint8_t>& blob_bytes,
    ShaderStage stage,
    const std::string& target_profile,
    const ShaderCacheLiftRequest& request,
    std::vector<uint8_t>& out_dxil,
    std::string* out_diagnostics) {
    out_dxil.clear();

    const bool use_fxc_50 = EndsWith50(target_profile);
    Rdna2LiftOptions opts{};
    opts.use_sm65 = !use_fxc_50;
    opts.emit_spirv = false;
    opts.emit_hlsl = true;
    opts.validate_spirv_external = false;
    opts.compile_dxil_with_dxc = !use_fxc_50;
    opts.mesh_has_payload = request.mesh_has_payload;
    opts.target_profile_override = target_profile;
    opts.dxc_path = request.dxc_path;

    Rdna2LiftResult lifted{};
    Status st = LiftRdna2Xs2Shader(blob_bytes, lifted, opts);
    if (!st.ok()) {
        if (out_diagnostics) {
            *out_diagnostics = lifted.dxc_diagnostics.empty() ? st.message : lifted.dxc_diagnostics;
        }
        return st;
    }

    if (use_fxc_50) {
#if defined(_WIN32)
        std::string fxc_diag;
        st = CompileHlslWithFxc50(lifted.hlsl_source, target_profile, "main", out_dxil, &fxc_diag);
        if (!st.ok()) {
            if (out_diagnostics) *out_diagnostics = fxc_diag.empty() ? st.message : fxc_diag;
            return st;
        }
#else
        (void)stage;
        return Status::Error(StatusCode::NotImplemented, "SM 5.0 fallback requires Windows");
#endif
    } else {
        out_dxil = std::move(lifted.pc_dxil);
    }

    (void)stage;
    return Status::Ok();
}

void ShaderCache::EnqueueAsyncJob(AsyncJob job) {
    {
        std::lock_guard<std::mutex> lock(async_mutex_);
        if (!inflight_keys_.insert(job.key).second) {
            return;  // Already queued or compiling
        }
        job_queue_.push_back(std::move(job));
        async_queued_.fetch_add(1, std::memory_order_relaxed);
        CallTelemetry::Global().Record(kCacheTelemetryModule, "AsyncCompileQueued", CallOutcome::Ok, 0);
        if (active_workers_ < 2u) {
            ++active_workers_;
            workers_.emplace_back(&ShaderCache::WorkerLoop, this);
        }
    }
    async_cv_.notify_one();
}

void ShaderCache::WorkerLoop() {
    for (;;) {
        AsyncJob job{};
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            if (job_queue_.empty()) {
                --active_workers_;
                async_cv_.notify_all();
                return;
            }
            job = std::move(job_queue_.front());
            job_queue_.pop_front();
            ++active_jobs_;
        }

        std::vector<uint8_t> dxil;
        std::string diag;
        Status st = CompileRealXboxShaderSync(
            job.blob_bytes, job.stage, job.target_profile, job.request, dxil, &diag);
        if (st.ok()) {
            (void)Put(job.key, job.stage, job.target_profile, dxil);
            async_completed_.fetch_add(1, std::memory_order_relaxed);
            CallTelemetry::Global().Record(kCacheTelemetryModule, "AsyncCompileCompleted", CallOutcome::Ok, 0);
        } else {
            async_failed_.fetch_add(1, std::memory_order_relaxed);
            CallTelemetry::Global().Record(kCacheTelemetryModule, "AsyncCompileCompleted", CallOutcome::Error, -1);
        }

        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            inflight_keys_.erase(job.key);
            --active_jobs_;
        }
        async_cv_.notify_all();
    }
}

bool ShaderCache::IsCompileReady(const ShaderCacheKey& key) const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (memory_cache_.find(key) != memory_cache_.end()) {
            return true;
        }
    }
    std::error_code ec;
    return std::filesystem::exists(CacheFilePathForKey(key), ec);
}

void ShaderCache::WaitForPendingCompiles() {
    std::vector<std::thread> to_join;
    {
        std::unique_lock<std::mutex> lock(async_mutex_);
        async_cv_.wait(lock, [&]() {
            return job_queue_.empty() && active_jobs_ == 0u && active_workers_ == 0u;
        });
        to_join.swap(workers_);
    }
    for (auto& t : to_join) {
        if (t.joinable()) {
            t.join();
        }
    }
}

Status ShaderCache::GetOrLiftXboxShader(
    const void* blob,
    size_t blob_size,
    const ShaderCacheLiftRequest& request,
    CachedShaderResult& out_result,
    std::string* out_diagnostics) {
    out_result = CachedShaderResult{};
    if (!blob || blob_size < 4u) {
        return Status::Error(StatusCode::InvalidArgument, "Empty Xbox shader bytecode");
    }

    XboxDxbcContainer container{};
    if (auto st = ParseXboxShaderContainer(blob, blob_size, container); !st.ok()) {
        if (out_diagnostics) *out_diagnostics = st.message;
        return st;
    }
    if (!container.has_xs2) {
        return Status::Error(StatusCode::UnsupportedFormat, "Input blob does not carry Xbox XS2 microcode");
    }

    const ShaderStage stage = container.xs2.stage;
    const std::string target_profile = SelectTargetProfileForStage(stage, request.highest_shader_model);
    const uint32_t sm_flags =
        ((stage == ShaderStage::Mesh && request.mesh_has_payload) ? 1u : 0u) |
        ((request.highest_shader_model & 0xFFu) << 8) |
        ((static_cast<uint32_t>(stage) & 0xFFu) << 16);

    const ShaderCacheKey key = ComputeShaderCacheKey(
        blob, blob_size, target_profile, sm_flags, kShaderCacheFormatVersion);

    // 1. Check memory + disk cache
    if (TryGet(key, out_result)) {
        return Status::Ok();
    }

    // 2. Cache miss
    misses_.fetch_add(1, std::memory_order_relaxed);
    CallTelemetry::Global().Record(kCacheTelemetryModule, "CacheMiss", CallOutcome::Ok, 0);

    const auto* raw_ptr = static_cast<const uint8_t*>(blob);
    std::vector<uint8_t> blob_copy(raw_ptr, raw_ptr + blob_size);

    // 3. Optional async background dxc compilation mode
    if (IsAsyncPsoCompilationEnabled()) {
        std::vector<uint8_t> standin_bc;
        Status st = CompileStandInShader(
            stage, target_profile, request.mesh_has_payload,
            request.prefer_dxil_standin, request.dxc_path, standin_bc, out_diagnostics);
        if (!st.ok()) {
            return st;
        }

        AsyncJob job{};
        job.key = key;
        job.blob_bytes = std::move(blob_copy);
        job.stage = stage;
        job.target_profile = target_profile;
        job.request = request;
        EnqueueAsyncJob(std::move(job));

        out_result.key = key;
        out_result.stage = stage;
        out_result.target_profile = target_profile;
        out_result.dxil = std::move(standin_bc);
        out_result.is_standin_async = true;
        return Status::Ok();
    }

    // 4. Synchronous lift + dxc compile + atomic disk cache store
    std::vector<uint8_t> dxil;
    Status st = CompileRealXboxShaderSync(blob_copy, stage, target_profile, request, dxil, out_diagnostics);
    if (!st.ok()) {
        return st;
    }

    (void)Put(key, stage, target_profile, dxil);

    out_result.key = key;
    out_result.stage = stage;
    out_result.target_profile = target_profile;
    out_result.dxil = std::move(dxil);
    return Status::Ok();
}

}  // namespace openscarlett
