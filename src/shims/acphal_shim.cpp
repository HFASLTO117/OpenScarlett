#define OPENSCARLETT_BUILDING_ACPHAL 1
#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <malloc.h>
#endif

namespace {

constexpr const char* kModName = "AcpHal.dll";
constexpr uint32_t kMaxAcpContexts = 256u;

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const AcpHalGuid* a, const AcpHalGuid& b) {
    return a && std::memcmp(a, &b, sizeof(AcpHalGuid)) == 0;
}

openscarlett::Xma2Config ToXma2Config(const AcpHalXmaContextDesc& desc) {
    openscarlett::Xma2Config cfg{};
    cfg.sample_rate = desc.sampleRate != 0 ? desc.sampleRate : 48000u;
    cfg.channels = desc.channels != 0 ? desc.channels : 2u;
    cfg.block_size = desc.blockSize != 0 ? desc.blockSize : openscarlett::kXma2DefaultBlockSize;
    cfg.subframes_per_frame = desc.subframesPerFrame != 0 ? desc.subframesPerFrame : 4u;
    cfg.quant_bits = desc.quantBits != 0 ? desc.quantBits : 12u;
    return cfg;
}

struct AcpShapeAllocation {
    uint32_t num_xma = 0;
    uint32_t num_src = 0;
    uint32_t num_eq = 0;
    uint32_t num_dma = 0;
};

class AcpHalImpl final : public IAcpHal {
public:
    int32_t OPENSCARLETT_ACPHAL_CALL QueryInterface(
        const AcpHalGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_AcpHal_IUnknown) || GuidEquals(riid, IID_IAcpHal)) {
            AddRef();
            *ppvObject = static_cast<IAcpHal*>(this);
            return RecordHr("IAcpHal::QueryInterface", ACPHAL_S_OK, 0);
        }
        return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_ACPHAL_CALL AddRef() override {
        RecordOk("IAcpHal::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_ACPHAL_CALL Release() override {
        RecordOk("IAcpHal::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_ACPHAL_CALL ConfigureXmaContext(
        uint32_t contextIndex, const AcpHalXmaContextDesc* desc) override {
        if (contextIndex >= kMaxAcpContexts || !desc) {
            return RecordHr("IAcpHal::ConfigureXmaContext", ACPHAL_E_INVALIDARG, 3);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto& dec = decoders_[contextIndex];
        if (!dec) {
            dec = std::make_unique<openscarlett::Xma2Decoder>();
        }
        const auto st = dec->Initialize(ToXma2Config(*desc));
        return RecordHr("IAcpHal::ConfigureXmaContext", st.ok() ? ACPHAL_S_OK : ACPHAL_E_INVALIDARG, 3);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommand(const AcpHalCommand* command) override {
        if (!command) {
            return RecordHr("IAcpHal::SubmitCommand", ACPHAL_E_INVALIDARG, 4);
        }
        const int32_t hr = ExecuteCommand(*command);
        return RecordHr("IAcpHal::SubmitCommand", hr, 4);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommands(
        uint32_t count, const AcpHalCommand* commands) override {
        if (count > 0 && !commands) {
            return RecordHr("IAcpHal::SubmitCommands", ACPHAL_E_INVALIDARG, 5);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const int32_t hr = ExecuteCommand(commands[i]);
            if (hr < 0) {
                return RecordHr("IAcpHal::SubmitCommands", hr, 5);
            }
        }
        return RecordHr("IAcpHal::SubmitCommands", ACPHAL_S_OK, 5);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL WaitIdle() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return RecordHr("IAcpHal::WaitIdle", ACPHAL_S_OK, 6);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL GetDecodedPacketCount(
        uint32_t contextIndex, uint64_t* packetCount) override {
        if (contextIndex >= kMaxAcpContexts || !packetCount) {
            return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_E_INVALIDARG, 7);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = decoders_.find(contextIndex);
        *packetCount = (it != decoders_.end() && it->second)
                           ? it->second->Stats().packets_parsed
                           : 0u;
        return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_S_OK, 7);
    }

private:
    int32_t ExecuteCommand(const AcpHalCommand& cmd) {
        std::lock_guard<std::mutex> lk(mutex_);
        switch (cmd.type) {
            case AcpHalCommand_Nop:
            case AcpHalCommand_Flush:
                return ACPHAL_S_OK;

            case AcpHalCommand_ResetContext: {
                if (cmd.contextIndex >= kMaxAcpContexts) {
                    return ACPHAL_E_INVALIDARG;
                }
                auto it = decoders_.find(cmd.contextIndex);
                if (it != decoders_.end() && it->second) {
                    it->second->Reset();
                }
                return ACPHAL_S_OK;
            }

            case AcpHalCommand_XmaDecode: {
                if (cmd.contextIndex >= kMaxAcpContexts ||
                    !cmd.inputData || cmd.inputBytes == 0) {
                    return ACPHAL_E_INVALIDARG;
                }
                auto& dec = decoders_[cmd.contextIndex];
                if (!dec) {
                    dec = std::make_unique<openscarlett::Xma2Decoder>();
                }
                if (!dec->IsInitialized()) {
                    const auto init_st = dec->Initialize(ToXma2Config(cmd.xmaConfig));
                    if (!init_st.ok()) {
                        return ACPHAL_E_INVALIDARG;
                    }
                }

                if (cmd.outputFloatPcm) {
                    std::vector<float> pcm;
                    const auto st = dec->DecodePackets(cmd.inputData, cmd.inputBytes, pcm);
                    if (!st.ok()) {
                        return ACPHAL_E_FAIL;
                    }
                    const uint32_t to_copy = std::min<uint32_t>(
                        cmd.outputCapacitySamples, static_cast<uint32_t>(pcm.size()));
                    if (to_copy > 0) {
                        std::memcpy(cmd.outputFloatPcm, pcm.data(), to_copy * sizeof(float));
                    }
                    if (cmd.outSamplesWritten) {
                        *cmd.outSamplesWritten = to_copy;
                    }
                    return ACPHAL_S_OK;
                }

                if (cmd.outputInt16Pcm) {
                    std::vector<int16_t> pcm;
                    const auto st = dec->DecodePacketsInt16(cmd.inputData, cmd.inputBytes, pcm);
                    if (!st.ok()) {
                        return ACPHAL_E_FAIL;
                    }
                    const uint32_t to_copy = std::min<uint32_t>(
                        cmd.outputCapacitySamples, static_cast<uint32_t>(pcm.size()));
                    if (to_copy > 0) {
                        std::memcpy(cmd.outputInt16Pcm, pcm.data(), to_copy * sizeof(int16_t));
                    }
                    if (cmd.outSamplesWritten) {
                        *cmd.outSamplesWritten = to_copy;
                    }
                    return ACPHAL_S_OK;
                }

                return ACPHAL_E_INVALIDARG;
            }

            default:
                return ACPHAL_E_INVALIDARG;
        }
    }

    std::atomic<uint32_t> ref_count_{1};
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<openscarlett::Xma2Decoder>> decoders_;
};

// ===========================================================================
// Xbox APU (Audio Processing Unit) Heap Allocator & Virtual<->APU Mapper
// ===========================================================================
constexpr uint32_t kApuInvalidAddress = 0u;
constexpr uint32_t kApuAllocNonCachedFlag = 0x00000001u;
constexpr uint32_t kApuDefaultHeapCapacity = 64u * 1024u * 1024u;
constexpr uint32_t kApuDefaultAlignment = 16u;
constexpr uint32_t kApuMinGranularity = 4u;
constexpr uint32_t kApuMaxAlignment = 64u * 1024u * 1024u;
constexpr uint32_t kApuCachedBaseDefault = 0x10000000u;
constexpr uint32_t kApuNonCachedBaseDefault = 0x90000000u;
constexpr uint32_t kApuAddressSpaceLimit = 0xFFFF0000u;

struct AcpApuHeapState {
    uint32_t bytesFree;
    uint32_t bytesAllocated;
    uint32_t bytesLost;
    uint32_t maximumBlockSizeAvailable;
    uint32_t allocationCount;
};

static_assert(sizeof(AcpApuHeapState) == 20u, "APU_HEAP_STATE must be 20 bytes");

inline bool IsPowerOfTwoU32(uint32_t v) noexcept {
    return v != 0u && (v & (v - 1u)) == 0u;
}

inline bool AlignUpU32(uint32_t value, uint32_t alignment, uint32_t& out) noexcept {
    const uint64_t mask = static_cast<uint64_t>(alignment) - 1ull;
    const uint64_t aligned = (static_cast<uint64_t>(value) + mask) & ~mask;
    if (aligned > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out = static_cast<uint32_t>(aligned);
    return true;
}

void* AllocAlignedHostMemory(size_t size, size_t alignment) noexcept {
    const size_t eff_align = std::max<size_t>(alignment, sizeof(void*));
#if defined(_WIN32)
    return _aligned_malloc(size, eff_align);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, eff_align, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void FreeAlignedHostMemory(void* ptr) noexcept {
    if (!ptr) return;
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

struct ApuFreeSpan {
    uint32_t start = 0u;
    uint32_t size = 0u;
};

struct ApuAllocation {
    void* host_ptr = nullptr;
    uint32_t apu_addr = 0u;
    uint32_t requested_size = 0u;
    uint32_t reserved_size = 0u;
    uint32_t alignment = 0u;
    bool non_cached = false;
};

struct ApuHeapPool {
    bool initialized = false;
    uint32_t base_addr = 0u;
    uint32_t capacity = 0u;
    uint32_t bytes_allocated = 0u;
    uint32_t bytes_lost = 0u;
    uint32_t allocation_count = 0u;
    std::vector<ApuFreeSpan> free_spans;

    void Reset(uint32_t base, uint32_t cap) {
        initialized = true;
        base_addr = base;
        capacity = cap;
        bytes_allocated = 0u;
        bytes_lost = 0u;
        allocation_count = 0u;
        free_spans.clear();
        if (cap > 0u) {
            free_spans.push_back(ApuFreeSpan{base, cap});
        }
    }

    void CoalesceInsert(uint32_t addr, uint32_t len) {
        if (len == 0u) return;
        auto it = std::lower_bound(
            free_spans.begin(),
            free_spans.end(),
            addr,
            [](const ApuFreeSpan& s, uint32_t key) { return s.start < key; });
        it = free_spans.insert(it, ApuFreeSpan{addr, len});

        // Merge with next span if adjacent
        auto next_it = it + 1;
        if (next_it != free_spans.end() &&
            static_cast<uint64_t>(it->start) + it->size == next_it->start) {
            it->size += next_it->size;
            free_spans.erase(next_it);
        }

        // Merge with previous span if adjacent
        if (it != free_spans.begin()) {
            auto prev_it = it - 1;
            if (static_cast<uint64_t>(prev_it->start) + prev_it->size == it->start) {
                prev_it->size += it->size;
                free_spans.erase(it);
            }
        }
    }

    bool GrowCapacity(uint32_t new_cap) {
        if (new_cap < capacity) {
            return false;
        }
        if (new_cap == capacity) {
            return true;
        }
        const uint32_t delta = new_cap - capacity;
        const uint64_t tail = static_cast<uint64_t>(base_addr) + capacity;
        if (tail + delta > kApuAddressSpaceLimit) {
            return false;
        }
        CoalesceInsert(static_cast<uint32_t>(tail), delta);
        capacity = new_cap;
        return true;
    }

    bool AllocateSpan(
        uint32_t requested_size,
        uint32_t alignment,
        uint32_t& out_apu_addr,
        uint32_t& out_reserved_size) {
        uint32_t reserved_size = 0u;
        if (!AlignUpU32(requested_size, kApuMinGranularity, reserved_size) || reserved_size == 0u) {
            return false;
        }
        for (size_t i = 0u; i < free_spans.size(); ++i) {
            const uint32_t span_start = free_spans[i].start;
            const uint32_t span_size = free_spans[i].size;
            const uint64_t span_end = static_cast<uint64_t>(span_start) + span_size;

            uint32_t aligned_start = 0u;
            if (!AlignUpU32(span_start, alignment, aligned_start)) {
                continue;
            }
            if (static_cast<uint64_t>(aligned_start) > span_end) {
                continue;
            }
            const uint64_t avail_after_align = span_end - aligned_start;
            if (avail_after_align < reserved_size) {
                continue;
            }

            const uint32_t prefix_len = aligned_start - span_start;
            const uint64_t alloc_end = static_cast<uint64_t>(aligned_start) + reserved_size;
            const uint32_t suffix_len = static_cast<uint32_t>(span_end - alloc_end);

            if (prefix_len > 0u && suffix_len > 0u) {
                free_spans[i].size = prefix_len;
                free_spans.insert(
                    free_spans.begin() + static_cast<std::ptrdiff_t>(i + 1u),
                    ApuFreeSpan{static_cast<uint32_t>(alloc_end), suffix_len});
            } else if (prefix_len > 0u) {
                free_spans[i].size = prefix_len;
            } else if (suffix_len > 0u) {
                free_spans[i].start = static_cast<uint32_t>(alloc_end);
                free_spans[i].size = suffix_len;
            } else {
                free_spans.erase(free_spans.begin() + static_cast<std::ptrdiff_t>(i));
            }

            out_apu_addr = aligned_start;
            out_reserved_size = reserved_size;
            bytes_allocated += requested_size;
            bytes_lost += (reserved_size - requested_size);
            ++allocation_count;
            return true;
        }
        return false;
    }

    void FreeSpan(uint32_t apu_addr, uint32_t requested_size, uint32_t reserved_size) {
        CoalesceInsert(apu_addr, reserved_size);
        bytes_allocated -= requested_size;
        bytes_lost -= (reserved_size - requested_size);
        --allocation_count;
    }

    AcpApuHeapState SnapshotState() const noexcept {
        AcpApuHeapState st{};
        st.bytesAllocated = bytes_allocated;
        st.bytesLost = bytes_lost;
        st.allocationCount = allocation_count;
        uint64_t total_free = 0ull;
        uint32_t max_block = 0u;
        for (const auto& s : free_spans) {
            total_free += s.size;
            if (s.size > max_block) {
                max_block = s.size;
            }
        }
        st.bytesFree = static_cast<uint32_t>(
            std::min<uint64_t>(total_free, std::numeric_limits<uint32_t>::max()));
        st.maximumBlockSizeAvailable = max_block;
        return st;
    }
};

class ApuHeapManager {
public:
    static ApuHeapManager& Global() {
        static ApuHeapManager* s_mgr = new ApuHeapManager();
        return *s_mgr;
    }

    int32_t CreateHeap(uint32_t cached_size, uint32_t non_cached_size) {
        if (cached_size == 0u && non_cached_size == 0u) {
            return ACPHAL_E_INVALIDARG;
        }
        const uint64_t cached_end = static_cast<uint64_t>(kApuCachedBaseDefault) + cached_size;
        uint64_t non_cached_base = kApuNonCachedBaseDefault;
        if (cached_end > non_cached_base) {
            non_cached_base = (cached_end + 0xFFFFull) & ~0xFFFFull;
        }
        if (non_cached_base + non_cached_size > kApuAddressSpaceLimit) {
            return ACPHAL_E_INVALIDARG;
        }

        std::lock_guard<std::mutex> lk(mutex_);
        if (cached_.allocation_count == 0u) {
            cached_.Reset(kApuCachedBaseDefault, cached_size);
        } else if (!cached_.GrowCapacity(cached_size)) {
            return ACPHAL_E_INVALIDARG;
        }

        if (non_cached_.allocation_count == 0u) {
            non_cached_.Reset(static_cast<uint32_t>(non_cached_base), non_cached_size);
        } else if (!non_cached_.GrowCapacity(non_cached_size)) {
            return ACPHAL_E_INVALIDARG;
        }
        return ACPHAL_S_OK;
    }

    int32_t Alloc(
        void** out_va,
        uint32_t* out_apu_addr,
        uint32_t size_in_bytes,
        uint32_t alignment_in_bytes,
        uint32_t flags) {
        if (!out_va) {
            return ACPHAL_E_POINTER;
        }
        *out_va = nullptr;
        if (out_apu_addr) {
            *out_apu_addr = kApuInvalidAddress;
        }
        if (size_in_bytes == 0u) {
            return ACPHAL_E_INVALIDARG;
        }
        const uint32_t alignment =
            (alignment_in_bytes == 0u) ? kApuDefaultAlignment : alignment_in_bytes;
        if (!IsPowerOfTwoU32(alignment) || alignment > kApuMaxAlignment) {
            return ACPHAL_E_INVALIDARG;
        }

        const bool non_cached = (flags & kApuAllocNonCachedFlag) != 0u;
        void* host_ptr = AllocAlignedHostMemory(size_in_bytes, alignment);
        if (!host_ptr) {
            return ACPHAL_E_OUTOFMEMORY;
        }
        std::memset(host_ptr, 0, size_in_bytes);

        uint32_t apu_addr = kApuInvalidAddress;
        uint32_t reserved_size = 0u;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            EnsureDefaultHeapsLocked();
            ApuHeapPool& pool = non_cached ? non_cached_ : cached_;
            if (!pool.AllocateSpan(size_in_bytes, alignment, apu_addr, reserved_size)) {
                FreeAlignedHostMemory(host_ptr);
                return ACPHAL_E_OUTOFMEMORY;
            }

            auto rec = std::make_unique<ApuAllocation>();
            rec->host_ptr = host_ptr;
            rec->apu_addr = apu_addr;
            rec->requested_size = size_in_bytes;
            rec->reserved_size = reserved_size;
            rec->alignment = alignment;
            rec->non_cached = non_cached;

            ApuAllocation* raw = rec.get();
            const uintptr_t va_key = reinterpret_cast<uintptr_t>(host_ptr);
            by_va_[va_key] = std::move(rec);
            va_intervals_[va_key] = raw;
            apu_intervals_[apu_addr] = raw;
        }

        *out_va = host_ptr;
        if (out_apu_addr) {
            *out_apu_addr = apu_addr;
        }
        return ACPHAL_S_OK;
    }

    int32_t Free(void* virtual_address) {
        if (!virtual_address) {
            return ACPHAL_S_OK;
        }
        void* to_free = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            const uintptr_t va_key = reinterpret_cast<uintptr_t>(virtual_address);
            auto it = by_va_.find(va_key);
            if (it == by_va_.end()) {
                return ACPHAL_E_INVALIDARG;
            }
            const ApuAllocation& rec = *it->second;
            ApuHeapPool& pool = rec.non_cached ? non_cached_ : cached_;
            pool.FreeSpan(rec.apu_addr, rec.requested_size, rec.reserved_size);
            va_intervals_.erase(va_key);
            apu_intervals_.erase(rec.apu_addr);
            to_free = rec.host_ptr;
            by_va_.erase(it);
        }
        FreeAlignedHostMemory(to_free);
        return ACPHAL_S_OK;
    }

    int32_t GetState(AcpApuHeapState* out_state, uint32_t flags) {
        if (!out_state) {
            return ACPHAL_E_POINTER;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        EnsureDefaultHeapsLocked();
        const bool non_cached = (flags & kApuAllocNonCachedFlag) != 0u;
        const ApuHeapPool& pool = non_cached ? non_cached_ : cached_;
        *out_state = pool.SnapshotState();
        return ACPHAL_S_OK;
    }

    uint32_t MapVirtualAddress(const void* virtual_address) {
        if (!virtual_address) {
            return kApuInvalidAddress;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        const uintptr_t addr = reinterpret_cast<uintptr_t>(virtual_address);
        auto it = va_intervals_.upper_bound(addr);
        if (it == va_intervals_.begin()) {
            return kApuInvalidAddress;
        }
        --it;
        const ApuAllocation* rec = it->second;
        const uintptr_t base = it->first;
        if (addr >= base && (addr - base) < rec->requested_size) {
            return rec->apu_addr + static_cast<uint32_t>(addr - base);
        }
        return kApuInvalidAddress;
    }

    void* MapApuAddress(uint32_t apu_address) {
        if (apu_address == kApuInvalidAddress) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = apu_intervals_.upper_bound(apu_address);
        if (it == apu_intervals_.begin()) {
            return nullptr;
        }
        --it;
        const ApuAllocation* rec = it->second;
        const uint32_t base = it->first;
        if (apu_address >= base && (apu_address - base) < rec->requested_size) {
            return static_cast<uint8_t*>(rec->host_ptr) + (apu_address - base);
        }
        return nullptr;
    }

    int32_t IsVirtualAddressValid(const void* virtual_address) {
        return MapVirtualAddress(virtual_address) != kApuInvalidAddress ? 1 : 0;
    }

private:
    void EnsureDefaultHeapsLocked() {
        if (!cached_.initialized && !non_cached_.initialized) {
            cached_.Reset(kApuCachedBaseDefault, kApuDefaultHeapCapacity);
            non_cached_.Reset(kApuNonCachedBaseDefault, kApuDefaultHeapCapacity);
        }
    }

    std::mutex mutex_;
    ApuHeapPool cached_;
    ApuHeapPool non_cached_;
    std::unordered_map<uintptr_t, std::unique_ptr<ApuAllocation>> by_va_;
    std::map<uintptr_t, ApuAllocation*> va_intervals_;
    std::map<uint32_t, ApuAllocation*> apu_intervals_;
};

}  // namespace

extern "C" {

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalCreate(IAcpHal** ppAcpHal) {
    if (!ppAcpHal) {
        return RecordHr("AcpHalCreate", ACPHAL_E_POINTER);
    }
    *ppAcpHal = nullptr;
    auto* obj = new (std::nothrow) AcpHalImpl();
    if (!obj) {
        return RecordHr("AcpHalCreate", ACPHAL_E_OUTOFMEMORY);
    }
    *ppAcpHal = obj;
    return RecordHr("AcpHalCreate", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalAllocateShapeContexts(
    const AcpHalShapeContexts* pRequested,
    AcpHalShapeContexts* pAllocated) {
    if (!pRequested || !pAllocated) {
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_INVALIDARG);
    }
    auto* alloc = new (std::nothrow) AcpShapeAllocation();
    if (!alloc) {
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_OUTOFMEMORY);
    }
    alloc->num_xma = std::min<uint32_t>(pRequested->numXmaContexts, kMaxAcpContexts);
    alloc->num_src = pRequested->numSrcContexts;
    alloc->num_eq = pRequested->numEqContexts;
    alloc->num_dma = pRequested->numDmaContexts;

    pAllocated->numXmaContexts = alloc->num_xma;
    pAllocated->numSrcContexts = alloc->num_src;
    pAllocated->numEqContexts = alloc->num_eq;
    pAllocated->numDmaContexts = alloc->num_dma;
    pAllocated->internalAllocation = alloc;
    return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalReleaseShapeContexts(AcpHalShapeContexts* pAllocated) {
    if (!pAllocated) {
        return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_E_INVALIDARG);
    }
    delete static_cast<AcpShapeAllocation*>(pAllocated->internalAllocation);
    std::memset(pAllocated, 0, sizeof(*pAllocated));
    return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_S_OK);
}

// ===========================================================================
// Xbox APU Heap Exports (apu.h / AcpHal.dll)
// ===========================================================================
OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL ApuCreateHeap(
    uint32_t cachedSizeInBytes,
    uint32_t nonCachedSizeInBytes) {
    return RecordHr(
        "ApuCreateHeap",
        ApuHeapManager::Global().CreateHeap(cachedSizeInBytes, nonCachedSizeInBytes));
}

OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL ApuAlloc(
    void** virtualAddress,
    uint32_t* physicalAddress,
    uint32_t sizeInBytes,
    uint32_t alignmentInBytes,
    uint32_t flags) {
    return RecordHr(
        "ApuAlloc",
        ApuHeapManager::Global().Alloc(
            virtualAddress, physicalAddress, sizeInBytes, alignmentInBytes, flags));
}

OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL ApuFree(void* virtualAddress) {
    return RecordHr("ApuFree", ApuHeapManager::Global().Free(virtualAddress));
}

OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL ApuHeapGetState(
    AcpApuHeapState* heapState,
    uint32_t flags) {
    return RecordHr("ApuHeapGetState", ApuHeapManager::Global().GetState(heapState, flags));
}

OPENSCARLETT_ACPHAL_API uint32_t OPENSCARLETT_ACPHAL_CALL ApuMapVirtualAddress(
    const void* virtualAddress) {
    const uint32_t apu_addr = ApuHeapManager::Global().MapVirtualAddress(virtualAddress);
    RecordOk("ApuMapVirtualAddress");
    return apu_addr;
}

OPENSCARLETT_ACPHAL_API void* OPENSCARLETT_ACPHAL_CALL ApuMapApuAddress(uint32_t apuAddress) {
    void* va = ApuHeapManager::Global().MapApuAddress(apuAddress);
    RecordOk("ApuMapApuAddress");
    return va;
}

OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL ApuIsVirtualAddressValid(
    const void* virtualAddress) {
    const int32_t valid = ApuHeapManager::Global().IsVirtualAddressValid(virtualAddress);
    RecordOk("ApuIsVirtualAddressValid");
    return valid;
}

}  // extern "C"
