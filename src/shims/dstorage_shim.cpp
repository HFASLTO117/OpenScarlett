#define OPENSCARLETT_BUILDING_DSTORAGE 1
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/gdeflate.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "dstorage_xs.dll";

#if defined(_WIN32)
using PfnQueryTitlePhysicalPages = int32_t(__stdcall*)(
    const void*, uint64_t*, uint64_t*, uint32_t*);
using PfnXMemSize = size_t(__stdcall*)(void*, uint64_t);

bool IsWin32ProtectWritable(DWORD prot) {
    if ((prot & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD base = prot & 0xFFu;
    return base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}
#endif

struct DestinationClassification {
    bool is_physical_page = false;
    bool is_xmem = false;
    uint64_t physical_id = 0;
    uint64_t physical_mapping_size = 0;
};

int32_t ValidateAndClassifyDestination(
    void* dst_ptr,
    size_t dst_cap,
    DestinationClassification& out_class) {
    out_class = DestinationClassification{};
    if (!dst_ptr || dst_cap == 0u) {
        return OS_DSTORAGE_E_INVALIDARG;
    }

#if defined(_WIN32)
    // Verify the entire destination range is committed and writable (supports multi-region
    // physical-page mappings created via MapViewOfFile3).
    uintptr_t cursor = reinterpret_cast<uintptr_t>(dst_ptr);
    if (cursor + dst_cap < cursor) {
        return OS_DSTORAGE_E_INVALIDARG;
    }
    size_t remaining = dst_cap;
    while (remaining > 0u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0u) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (mbi.State != MEM_COMMIT || !IsWin32ProtectWritable(mbi.Protect)) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const size_t avail = static_cast<size_t>(region_end - cursor);
        const size_t step = std::min(remaining, avail);
        cursor += step;
        remaining -= step;
    }

    // Dynamically inspect kernelx.dll if loaded in-process to identify physical-page
    // and XMem allocations without adding a static link cycle.
    HMODULE h_kx = GetModuleHandleW(L"kernelx.dll");
    if (h_kx) {
        auto fn_query = reinterpret_cast<PfnQueryTitlePhysicalPages>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "QueryTitlePhysicalPages")));
        if (fn_query) {
            uint64_t phys_id = 0u;
            uint64_t map_size = 0u;
            uint32_t flags = 0u;
            if (fn_query(dst_ptr, &phys_id, &map_size, &flags) != 0) {
                out_class.is_physical_page = true;
                out_class.physical_id = phys_id;
                out_class.physical_mapping_size = map_size;
                if (map_size > 0u && dst_cap > map_size) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
            }
        }
        auto fn_xmem_size = reinterpret_cast<PfnXMemSize>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "XMemSize")));
        if (fn_xmem_size) {
            const size_t xsz = fn_xmem_size(dst_ptr, 0u);
            if (xsz > 0u) {
                out_class.is_xmem = true;
            }
        }
    }
#else
    (void)dst_ptr;
    (void)dst_cap;
#endif
    return OS_DSTORAGE_S_OK;
}

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

bool GuidEquals(const DStorageGuid* a, const DStorageGuid& b) {
    return a && std::memcmp(a, &b, sizeof(DStorageGuid)) == 0;
}

// ===========================================================================
// IDStorageFileX Implementation (real Win32 file I/O)
// ===========================================================================
class DStorageFileImpl final : public IDStorageFileX {
public:
    explicit DStorageFileImpl(
#if defined(_WIN32)
        HANDLE hfile
#else
        void* hfile
#endif
        )
        : file_handle_(hfile) {}

    ~DStorageFileImpl() {
        CloseInternal();
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) || GuidEquals(riid, IID_IDStorageFileX)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFileX*>(this);
            return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageFileX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageFileX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        RecordOk("IDStorageFileX::Close", 3);
        CloseInternal();
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL GetFileInformation(DSTORAGE_FILE_INFO* info) override {
        if (!info) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        std::lock_guard<std::mutex> lk(io_mutex_);
#if defined(_WIN32)
        if (file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
        }
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file_handle_, &sz)) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
        }
        info->FileSize = static_cast<uint64_t>(sz.QuadPart);
        return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_S_OK, 4);
#else
        info->FileSize = 0;
        return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
#endif
    }

    int32_t ReadAt(
        uint64_t offset,
        uint32_t size,
        std::vector<uint8_t>& out,
        const std::atomic<bool>* cancel_flag = nullptr) {
        std::lock_guard<std::mutex> lk(io_mutex_);
#if defined(_WIN32)
        if (file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }
        if (offset + static_cast<uint64_t>(size) < offset) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        LARGE_INTEGER file_sz{};
        if (!GetFileSizeEx(file_handle_, &file_sz) ||
            file_sz.QuadPart < 0 ||
            offset + static_cast<uint64_t>(size) > static_cast<uint64_t>(file_sz.QuadPart)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_handle_, pos, nullptr, FILE_BEGIN)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        out.resize(size);
        constexpr uint32_t kReadChunkBytes = 65536u;
        uint32_t total_read = 0;
        while (total_read < size) {
            if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
                out.clear();
                return OS_DSTORAGE_E_CANCELLED;
            }
            const DWORD to_read = static_cast<DWORD>(std::min<uint32_t>(size - total_read, kReadChunkBytes));
            DWORD chunk = 0;
            if (!ReadFile(file_handle_, out.data() + total_read, to_read, &chunk, nullptr)) {
                return OS_DSTORAGE_E_IO_ERROR;
            }
            if (chunk == 0) {
                return OS_DSTORAGE_E_IO_ERROR;
            }
            total_read += chunk;
        }
        return OS_DSTORAGE_S_OK;
#else
        (void)offset;
        (void)size;
        (void)out;
        (void)cancel_flag;
        return OS_DSTORAGE_E_IO_ERROR;
#endif
    }

private:
    void CloseInternal() {
        std::lock_guard<std::mutex> lk(io_mutex_);
#if defined(_WIN32)
        if (file_handle_ != INVALID_HANDLE_VALUE && file_handle_ != nullptr) {
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
        }
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    std::mutex io_mutex_;
#if defined(_WIN32)
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
#else
    void* file_handle_ = nullptr;
#endif
};

// ===========================================================================
// IDStorageStatusArrayX Implementation
// ===========================================================================
class DStorageStatusArrayImpl final : public IDStorageStatusArrayX {
public:
    explicit DStorageStatusArrayImpl(uint32_t capacity)
        : complete_(capacity, true), results_(capacity, OS_DSTORAGE_S_OK) {}

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) {
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        }
        *ppvObject = nullptr;
        if (!riid) {
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        }
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageStatusArrayX)) {
            AddRef();
            *ppvObject = static_cast<IDStorageStatusArrayX*>(this);
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageStatusArrayX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageStatusArrayX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    bool OPENSCARLETT_DSTORAGE_CALL IsComplete(uint32_t index) override {
        RecordOk("IDStorageStatusArrayX::IsComplete", 3);
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= complete_.size()) return false;
        return complete_[index];
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL GetHResult(uint32_t index) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= results_.size()) {
            return RecordHr("IDStorageStatusArrayX::GetHResult", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        const int32_t hr = complete_[index] ? results_[index] : OS_DSTORAGE_E_PENDING;
        return RecordHr("IDStorageStatusArrayX::GetHResult", hr, 4);
    }

    void MarkPending(uint32_t index) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            complete_[index] = false;
            results_[index] = OS_DSTORAGE_E_PENDING;
        }
    }

    void MarkComplete(uint32_t index, int32_t hr) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            results_[index] = hr;
            complete_[index] = true;
        }
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::mutex mutex_;
    std::vector<bool> complete_;
    std::vector<int32_t> results_;
};

// ===========================================================================
// IDStorageQueueX Implementation (Real Async Worker + GDeflate & BCPack)
// ===========================================================================
class DStorageQueueImpl final : public IDStorageQueueX {
public:
    explicit DStorageQueueImpl(
        const DSTORAGE_QUEUE_DESC& desc,
        std::shared_ptr<std::atomic<uint32_t>> staging_size_ref = nullptr)
        : desc_(desc),
          staging_size_ref_(staging_size_ref
                                ? std::move(staging_size_ref)
                                : std::make_shared<std::atomic<uint32_t>>(
                                      DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE)) {
        if (desc_.Capacity < DSTORAGE_MIN_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        } else if (desc_.Capacity > DSTORAGE_MAX_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        }
        auto_submit_threshold_ =
            std::max<uint16_t>(1u, static_cast<uint16_t>(desc_.Capacity / 2u));
        if (desc_.Name) {
            name_storage_ = desc_.Name;
            desc_.Name = name_storage_.c_str();
        }
#if defined(_WIN32)
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
        worker_ = std::thread(&DStorageQueueImpl::WorkerLoop, this);
    }

    ~DStorageQueueImpl() {
        CloseInternal();
#if defined(_WIN32)
        if (error_event_) {
            CloseHandle(error_event_);
            error_event_ = nullptr;
        }
#endif
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageQueueX) ||
            GuidEquals(riid, IID_IDStorageQueueX1)) {
            AddRef();
            *ppvObject = static_cast<IDStorageQueueX*>(this);
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageQueueX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageQueueX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        RecordOk("IDStorageQueueX::EnqueueRequest", 3);
        if (!request) return;
        QueueItem item{};
        item.kind = QueueItem::Kind::Request;
        item.request = *request;
        if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
            item.request.Source.File.Source) {
            item.request.Source.File.Source->AddRef();
        }
        bool release_file = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
                ++staged_request_count_;
                if (staged_request_count_ >= auto_submit_threshold_) {
                    FlushStagedLocked();
                }
            } else if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                       item.request.Source.File.Source) {
                release_file = true;
            }
        }
        if (release_file) {
            item.request.Source.File.Source->Release();
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) override {
        RecordOk("IDStorageQueueX::EnqueueStatus", 4);
        if (!statusArray) return;
        auto* impl = static_cast<DStorageStatusArrayImpl*>(statusArray);
        impl->MarkPending(index);
        impl->AddRef();

        QueueItem item{};
        item.kind = QueueItem::Kind::Status;
        item.status_array = impl;
        item.status_index = index;
        bool fail_closed = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
            } else {
                fail_closed = true;
            }
        }
        if (fail_closed) {
            impl->MarkComplete(index, OS_DSTORAGE_E_FAIL);
            impl->Release();
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueSetEvent(void* handle) override {
        RecordOk("IDStorageQueueX::EnqueueSetEvent", 5);
        if (!handle) return;
        QueueItem item{};
        item.kind = QueueItem::Kind::Event;
        item.event_handle = handle;
        std::unique_lock<std::mutex> lk(mutex_);
        WaitForSlotLocked(lk);
        if (!closed_) {
            staged_.push_back(item);
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL Submit() override {
        RecordOk("IDStorageQueueX::Submit", 6);
        std::lock_guard<std::mutex> lk(mutex_);
        if (closed_ || staged_.empty()) return;
        FlushStagedLocked();
    }

    void OPENSCARLETT_DSTORAGE_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) override {
        RecordOk("IDStorageQueueX::CancelRequestsWithTag", 7);
        std::lock_guard<std::mutex> lk(mutex_);
        auto cancel_in = [&](std::deque<QueueItem>& q, uint64_t& counter) {
            for (auto& item : q) {
                if (item.kind == QueueItem::Kind::Request &&
                    !item.canceled &&
                    (item.request.CancellationTag & mask) == value) {
                    item.canceled = true;
                    ++counter;
                }
            }
        };
        cancel_in(staged_, staged_canceled_count_);
        cancel_in(active_, submitted_canceled_count_);
        if (executing_request_ && !executing_canceled_ &&
            (executing_tag_ & mask) == value) {
            executing_canceled_ = true;
            current_cancel_requested_.store(true, std::memory_order_release);
            ++inflight_canceled_count_;
        }
    }

    void GetQueueStats(DSTORAGE_QUEUE_STATS& out) {
        std::lock_guard<std::mutex> lk(mutex_);
        out.StagedCanceled = staged_canceled_count_;
        out.SubmittedCanceled = submitted_canceled_count_;
        out.InFlightCanceled = inflight_canceled_count_;
        out.CompletedOk = completed_ok_count_.load(std::memory_order_acquire);
        out.CompletedFailed = completed_failed_count_.load(std::memory_order_acquire);
        out.CompletedCanceled = completed_canceled_count_.load(std::memory_order_acquire);
        out.PhysicalPageWrites = physical_page_writes_.load(std::memory_order_acquire);
        out.XMemDestinationWrites = xmem_destination_writes_.load(std::memory_order_acquire);
        out.StagingBufferSize = staging_size_ref_->load(std::memory_order_acquire);
        out.LastIntermediateSize = last_intermediate_size_.load(std::memory_order_acquire);
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        RecordOk("IDStorageQueueX::Close", 8);
        CloseInternal();
    }

    void* OPENSCARLETT_DSTORAGE_CALL GetErrorEvent() override {
        RecordOk("IDStorageQueueX::GetErrorEvent", 9);
#if defined(_WIN32)
        return error_event_;
#else
        return nullptr;
#endif
    }

    void OPENSCARLETT_DSTORAGE_CALL RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        RecordOk("IDStorageQueueX::RetrieveErrorRecord", 10);
        if (!record) return;
        std::lock_guard<std::mutex> lk(error_mutex_);
        *record = error_record_;
        std::memset(&error_record_, 0, sizeof(error_record_));
#if defined(_WIN32)
        if (error_event_) {
            ResetEvent(error_event_);
        }
#endif
    }

    void OPENSCARLETT_DSTORAGE_CALL Query(DSTORAGE_QUEUE_INFO* info) override {
        RecordOk("IDStorageQueueX::Query", 11);
        if (!info) return;
        std::lock_guard<std::mutex> lk(mutex_);
        info->Desc = desc_;
        const size_t used = staged_.size() + active_.size();
        info->EmptySlotCount = (used < desc_.Capacity)
                                   ? static_cast<uint16_t>(desc_.Capacity - used)
                                   : 0u;
        info->RequestCountUntilAutoSubmit =
            (staged_request_count_ < auto_submit_threshold_)
                ? static_cast<uint16_t>(auto_submit_threshold_ - staged_request_count_)
                : 0u;
    }

private:
    void FlushStagedLocked() {
        if (staged_.empty()) {
            staged_request_count_ = 0u;
            return;
        }
        for (auto& it : staged_) {
            active_.push_back(it);
        }
        staged_.clear();
        staged_request_count_ = 0u;
        cv_.notify_one();
    }

    void WaitForSlotLocked(std::unique_lock<std::mutex>& lk) {
        while (!closed_ && (staged_.size() + active_.size()) >= desc_.Capacity) {
            if (!staged_.empty()) {
                FlushStagedLocked();
            }
            space_cv_.wait(lk, [&]() {
                return closed_ || (staged_.size() + active_.size()) < desc_.Capacity;
            });
        }
    }
    struct QueueItem {
        enum class Kind { Request, Status, Event };
        Kind kind = Kind::Request;
        bool canceled = false;
        DSTORAGE_REQUEST request{};
        DStorageStatusArrayImpl* status_array = nullptr;
        uint32_t status_index = 0;
        void* event_handle = nullptr;
    };

    int32_t ExecuteRequest(const DSTORAGE_REQUEST& req) {
        auto is_canceled = [&]() -> bool {
            return current_cancel_requested_.load(std::memory_order_acquire);
        };
        if (is_canceled()) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        if (req.Options.DestinationType != DSTORAGE_REQUEST_DESTINATION_MEMORY ||
            !req.Destination.Memory.Buffer || req.Destination.Memory.Size == 0) {
            return OS_DSTORAGE_E_INVALIDARG;
        }

        auto* dst_ptr = static_cast<uint8_t*>(req.Destination.Memory.Buffer);
        const size_t dst_cap = req.Destination.Memory.Size;
        DestinationClassification dst_class{};
        const int32_t dst_hr = ValidateAndClassifyDestination(dst_ptr, dst_cap, dst_class);
        if (dst_hr < 0) {
            return dst_hr;
        }

        const size_t staging_limit =
            static_cast<size_t>(staging_size_ref_->load(std::memory_order_acquire));

        // 1. Acquire raw source bytes (file or memory).
        std::vector<uint8_t> file_buf;
        const uint8_t* src_ptr = nullptr;
        size_t src_size = 0;

        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!req.Source.File.Source || req.Source.File.Size == 0) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            if (req.Options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE &&
                req.Source.File.Size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            auto* file_impl = static_cast<DStorageFileImpl*>(req.Source.File.Source);
            const int32_t rd_hr = file_impl->ReadAt(
                req.Source.File.Offset, req.Source.File.Size, file_buf, &current_cancel_requested_);
            if (rd_hr < 0) {
                return rd_hr;
            }
            src_ptr = file_buf.data();
            src_size = file_buf.size();
        } else if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY) {
            if (!req.Source.Memory.Source || req.Source.Memory.Size == 0) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            if (req.Options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE &&
                req.Source.Memory.Size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            src_ptr = static_cast<const uint8_t*>(req.Source.Memory.Source);
            src_size = req.Source.Memory.Size;
        } else {
            return OS_DSTORAGE_E_INVALIDARG;
        }

        if (is_canceled()) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        auto record_destination_write = [&]() {
            if (dst_class.is_physical_page) {
                physical_page_writes_.fetch_add(1u, std::memory_order_relaxed);
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            if (dst_class.is_xmem) {
                xmem_destination_writes_.fetch_add(1u, std::memory_order_relaxed);
            }
        };

        // 2. Decompress or copy according to CompressionFormat with intermediate_size bounds validation.
        switch (req.Options.CompressionFormat) {
            case DSTORAGE_COMPRESSION_FORMAT_NONE: {
                if (src_size > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && req.UncompressedSize != src_size) {
                    return OS_DSTORAGE_E_INVALIDARG;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                constexpr size_t kCopyChunk = 65536u;
                size_t copied = 0u;
                while (copied < src_size) {
                    if (is_canceled()) {
                        return OS_DSTORAGE_E_CANCELLED;
                    }
                    const size_t step = std::min(src_size - copied, kCopyChunk);
                    std::memcpy(dst_ptr + copied, src_ptr + copied, step);
                    copied += step;
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
                size_t header_uncompressed = 0u;
                if (!openscarlett::GDeflateIsValidStream(src_ptr, src_size) ||
                    !openscarlett::GDeflateGetUncompressedSize(src_ptr, src_size, &header_uncompressed) ||
                    header_uncompressed == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(
                    static_cast<uint32_t>(std::min<size_t>(header_uncompressed, UINT32_MAX)),
                    std::memory_order_release);
                if (header_uncompressed > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (header_uncompressed > dst_cap ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap)) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && header_uncompressed != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }

                size_t out_size = 0u;
                if (dst_class.is_physical_page) {
                    // Direct zero-copy decompression into kernelx/xmem physical-page mapping.
                    const auto res = openscarlett::GDeflateDecompress(
                        src_ptr, src_size, dst_ptr, dst_cap, &out_size);
                    if (res != openscarlett::GDeflateResult::Ok) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    if (req.UncompressedSize != 0u && out_size != req.UncompressedSize) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    record_destination_write();
                    return OS_DSTORAGE_S_OK;
                }

                std::vector<uint8_t> staging(header_uncompressed);
                const auto res = openscarlett::GDeflateDecompress(
                    src_ptr, src_size, staging.data(), staging.size(), &out_size);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (out_size > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && out_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (out_size > 0u) {
                    std::memcpy(dst_ptr, staging.data(), out_size);
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_ZLIB:
            case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
                openscarlett::BcPackHeader hdr{};
                if (!openscarlett::BcPackParseHeader(src_ptr, src_size, hdr).ok() ||
                    hdr.original_size == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(hdr.original_size, std::memory_order_release);
                if (static_cast<size_t>(hdr.original_size) > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (static_cast<size_t>(hdr.original_size) > dst_cap ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap)) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && hdr.original_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }

                std::vector<uint8_t> unpacked;
                const auto st = openscarlett::BcPackDecompress(src_ptr, src_size, unpacked);
                if (!st.ok()) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (unpacked.size() > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && unpacked.size() != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (!unpacked.empty()) {
                    std::memcpy(dst_ptr, unpacked.data(), unpacked.size());
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            default:
                return OS_DSTORAGE_E_INVALIDARG;
        }
    }

    void RecordFailure(int32_t hr, const DSTORAGE_REQUEST& req) {
        std::lock_guard<std::mutex> lk(error_mutex_);
        if (error_record_.FailureCount == 0) {
            error_record_.FirstFailure.HResult = hr;
            error_record_.FirstFailure.CommandType = 0;
            error_record_.FirstFailure.FailedRequest = req;
        }
        ++error_record_.FailureCount;
#if defined(_WIN32)
        if (error_event_) {
            SetEvent(error_event_);
        }
#endif
    }

    void WorkerLoop() {
        int32_t batch_hr = OS_DSTORAGE_S_OK;
        for (;;) {
            QueueItem item{};
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [&]() { return closed_ || !active_.empty(); });
                if (active_.empty()) {
                    if (closed_) return;
                    continue;
                }
                item = active_.front();
                active_.pop_front();
                if (item.kind == QueueItem::Kind::Request) {
                    executing_request_ = true;
                    executing_tag_ = item.request.CancellationTag;
                    executing_canceled_ = item.canceled;
                    current_cancel_requested_.store(item.canceled, std::memory_order_release);
                }
            }
            space_cv_.notify_all();

            switch (item.kind) {
                case QueueItem::Kind::Request: {
                    int32_t hr = OS_DSTORAGE_S_OK;
                    if (!item.canceled) {
                        hr = ExecuteRequest(item.request);
                    }
                    bool was_canceled = item.canceled || (hr == OS_DSTORAGE_E_CANCELLED);
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        was_canceled = was_canceled || executing_canceled_;
                        executing_request_ = false;
                        executing_tag_ = 0u;
                        executing_canceled_ = false;
                        current_cancel_requested_.store(false, std::memory_order_release);
                    }
                    if (was_canceled) {
                        completed_canceled_count_.fetch_add(1u, std::memory_order_relaxed);
                    } else if (hr < 0) {
                        completed_failed_count_.fetch_add(1u, std::memory_order_relaxed);
                        if (batch_hr >= 0) batch_hr = hr;
                        RecordFailure(hr, item.request);
                    } else {
                        completed_ok_count_.fetch_add(1u, std::memory_order_relaxed);
                    }
                    if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                        item.request.Source.File.Source) {
                        item.request.Source.File.Source->Release();
                    }
                    break;
                }

                case QueueItem::Kind::Status: {
                    if (item.status_array) {
                        item.status_array->MarkComplete(item.status_index, batch_hr);
                        item.status_array->Release();
                    }
                    batch_hr = OS_DSTORAGE_S_OK;
                    break;
                }

                case QueueItem::Kind::Event: {
#if defined(_WIN32)
                    if (item.event_handle) {
                        SetEvent(static_cast<HANDLE>(item.event_handle));
                    }
#endif
                    break;
                }
            }
        }
    }

    void CloseInternal() {
        std::deque<QueueItem> leftover_staged;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (closed_) return;
            closed_ = true;
            leftover_staged.swap(staged_);
            staged_request_count_ = 0u;
            cv_.notify_all();
            space_cv_.notify_all();
        }
        for (auto& item : leftover_staged) {
            if (item.kind == QueueItem::Kind::Request &&
                item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                item.request.Source.File.Source) {
                item.request.Source.File.Source->Release();
            } else if (item.kind == QueueItem::Kind::Status && item.status_array) {
                item.status_array->MarkComplete(item.status_index, OS_DSTORAGE_E_FAIL);
                item.status_array->Release();
            }
        }
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                worker_.detach();
            } else {
                worker_.join();
            }
        }
    }

    std::atomic<uint32_t> ref_count_{1};
    DSTORAGE_QUEUE_DESC desc_{};
    std::shared_ptr<std::atomic<uint32_t>> staging_size_ref_;
    uint16_t auto_submit_threshold_ = DSTORAGE_MIN_QUEUE_CAPACITY / 2u;
    std::string name_storage_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable space_cv_;
    bool closed_ = false;
    uint32_t staged_request_count_ = 0u;
    bool executing_request_ = false;
    uint64_t executing_tag_ = 0u;
    bool executing_canceled_ = false;
    std::atomic<bool> current_cancel_requested_{false};
    uint64_t staged_canceled_count_ = 0u;
    uint64_t submitted_canceled_count_ = 0u;
    uint64_t inflight_canceled_count_ = 0u;
    std::atomic<uint64_t> completed_ok_count_{0u};
    std::atomic<uint64_t> completed_failed_count_{0u};
    std::atomic<uint64_t> completed_canceled_count_{0u};
    std::atomic<uint64_t> physical_page_writes_{0u};
    std::atomic<uint64_t> xmem_destination_writes_{0u};
    std::atomic<uint32_t> last_intermediate_size_{0u};
    std::deque<QueueItem> staged_;
    std::deque<QueueItem> active_;
    std::thread worker_;

    std::mutex error_mutex_;
    DSTORAGE_ERROR_RECORD error_record_{};
#if defined(_WIN32)
    HANDLE error_event_ = nullptr;
#endif
};

// ===========================================================================
// IDStorageFactoryX Implementation
// ===========================================================================
class DStorageFactoryImpl final : public IDStorageFactoryX {
public:
    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageFactoryX) ||
            GuidEquals(riid, IID_IDStorageFactoryX1)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFactoryX*>(this);
            return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageFactoryX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageFactoryX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL CreateQueue(
        const DSTORAGE_QUEUE_DESC* desc, const DStorageGuid* riid, void** ppv) override {
        if (!desc || !ppv) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_INVALIDARG, 3);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageQueueX) &&
            !GuidEquals(riid, IID_IDStorageQueueX1) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_NOINTERFACE, 3);
        }
        auto* q = new (std::nothrow) DStorageQueueImpl(*desc, staging_size_);
        if (!q) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_OUTOFMEMORY, 3);
        }
        *ppv = static_cast<IDStorageQueueX*>(q);
        return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_S_OK, 3);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL OpenFile(
        const wchar_t* path, const DStorageGuid* riid, void** ppv) override {
        if (!path || !*path || !ppv) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageFileX) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_NOINTERFACE, 4);
        }
#if defined(_WIN32)
        HANDLE h = CreateFileW(
            path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_FILE_NOT_FOUND, 4);
        }
        auto* f = new (std::nothrow) DStorageFileImpl(h);
        if (!f) {
            CloseHandle(h);
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_OUTOFMEMORY, 4);
        }
        *ppv = static_cast<IDStorageFileX*>(f);
        return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_S_OK, 4);
#else
        return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_FAIL, 4);
#endif
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL CreateStatusArray(
        uint32_t capacity, const char* /*name*/, const DStorageGuid* riid, void** ppv) override {
        if (capacity == 0 || !ppv) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_INVALIDARG, 5);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageStatusArrayX) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_NOINTERFACE, 5);
        }
        auto* arr = new (std::nothrow) DStorageStatusArrayImpl(capacity);
        if (!arr) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_OUTOFMEMORY, 5);
        }
        *ppv = static_cast<IDStorageStatusArrayX*>(arr);
        return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_S_OK, 5);
    }

    void OPENSCARLETT_DSTORAGE_CALL SetDebugFlags(uint32_t flags) override {
        RecordOk("IDStorageFactoryX::SetDebugFlags", 6);
        debug_flags_.store(flags, std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL SetStagingBufferSize(uint32_t size) override {
        if (size < DSTORAGE_MIN_STAGING_BUFFER_SIZE || size > DSTORAGE_MAX_STAGING_BUFFER_SIZE) {
            return RecordHr("IDStorageFactoryX::SetStagingBufferSize",
                            OS_DSTORAGE_E_STAGING_BUFFER_INVALID, 7);
        }
        staging_size_->store(size, std::memory_order_release);
        return RecordHr("IDStorageFactoryX::SetStagingBufferSize", OS_DSTORAGE_S_OK, 7);
    }

    uint32_t GetStagingBufferSizeValue() const {
        return staging_size_->load(std::memory_order_acquire);
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> debug_flags_{0};
    std::shared_ptr<std::atomic<uint32_t>> staging_size_{
        std::make_shared<std::atomic<uint32_t>>(DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE)};
};

struct GlobalDStorageConfig {
    std::mutex mutex;
    DSTORAGE_CONFIGURATION1 config{};
};

GlobalDStorageConfig& GetConfig() {
    static GlobalDStorageConfig cfg;
    return cfg;
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageGetFactory(const DStorageGuid* riid, void** ppv) {
    if (!ppv) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_POINTER);
    }
    *ppv = nullptr;
    if (riid && !GuidEquals(riid, IID_IDStorageFactoryX) &&
        !GuidEquals(riid, IID_IDStorageFactoryX1) &&
        !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_NOINTERFACE);
    }
    auto* f = new (std::nothrow) DStorageFactoryImpl();
    if (!f) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_OUTOFMEMORY);
    }
    *ppv = static_cast<IDStorageFactoryX*>(f);
    return RecordHr("DStorageGetFactory", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration(
    const DSTORAGE_CONFIGURATION* configuration) {
    if (!configuration) {
        return RecordHr("DStorageSetConfiguration", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& cfg = GetConfig();
    std::lock_guard<std::mutex> lk(cfg.mutex);
    cfg.config.NumSubmitThreads = configuration->NumSubmitThreads;
    cfg.config.NumBuiltInCpuDecompressionThreads = configuration->NumBuiltInCpuDecompressionThreads;
    cfg.config.ForceMappingLayer = configuration->ForceMappingLayer;
    cfg.config.DisableBypassIO = configuration->DisableBypassIO;
    cfg.config.DisableTelemetry = configuration->DisableTelemetry;
    cfg.config.DisableGpuDecompressionMetacommand = configuration->DisableGpuDecompressionMetacommand;
    cfg.config.DisableGpuDecompression = configuration->DisableGpuDecompression;
    return RecordHr("DStorageSetConfiguration", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration1(
    const DSTORAGE_CONFIGURATION1* configuration) {
    if (!configuration) {
        return RecordHr("DStorageSetConfiguration1", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& cfg = GetConfig();
    std::lock_guard<std::mutex> lk(cfg.mutex);
    cfg.config = *configuration;
    return RecordHr("DStorageSetConfiguration1", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetQueueStats(
    IDStorageQueueX* queue, DSTORAGE_QUEUE_STATS* outStats) {
    if (!queue || !outStats) {
        return RecordHr("OpenScarlettDStorageGetQueueStats", OS_DSTORAGE_E_INVALIDARG);
    }
    std::memset(outStats, 0, sizeof(*outStats));
    auto* impl = static_cast<DStorageQueueImpl*>(queue);
    impl->GetQueueStats(*outStats);
    return RecordHr("OpenScarlettDStorageGetQueueStats", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetStagingBufferSize(
    IDStorageFactoryX* factory, uint32_t* outSize) {
    if (!factory || !outSize) {
        return RecordHr("OpenScarlettDStorageGetStagingBufferSize", OS_DSTORAGE_E_INVALIDARG);
    }
    auto* impl = static_cast<DStorageFactoryImpl*>(factory);
    *outSize = impl->GetStagingBufferSizeValue();
    return RecordHr("OpenScarlettDStorageGetStagingBufferSize", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageDecompressWithIntermediate(
    DSTORAGE_COMPRESSION_FORMAT format,
    const void* src,
    size_t src_size,
    void* dst,
    size_t dst_size,
    size_t intermediate_size,
    size_t* out_decompressed_size) {
    if (out_decompressed_size) {
        *out_decompressed_size = 0u;
    }
    if (!src || !dst || src_size == 0u || dst_size == 0u) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_E_INVALIDARG);
    }
    if (intermediate_size == 0u || intermediate_size > DSTORAGE_MAX_STAGING_BUFFER_SIZE) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                        OS_DSTORAGE_E_STAGING_BUFFER_INVALID);
    }

    DestinationClassification dst_class{};
    const int32_t dst_hr = ValidateAndClassifyDestination(dst, dst_size, dst_class);
    if (dst_hr < 0) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", dst_hr);
    }

    const auto* src_bytes = static_cast<const uint8_t*>(src);
    auto* dst_bytes = static_cast<uint8_t*>(dst);

    switch (format) {
        case DSTORAGE_COMPRESSION_FORMAT_NONE: {
            if (intermediate_size < src_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < src_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            std::memcpy(dst_bytes, src_bytes, src_size);
            if (out_decompressed_size) {
                *out_decompressed_size = src_size;
            }
            if (dst_class.is_physical_page) {
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
            size_t required = 0u;
            if (!openscarlett::GDeflateIsValidStream(src_bytes, src_size) ||
                !openscarlett::GDeflateGetUncompressedSize(src_bytes, src_size, &required) ||
                required == 0u) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (intermediate_size < required) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < required) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            size_t out_sz = 0u;
            if (dst_class.is_physical_page) {
                const auto res = openscarlett::GDeflateDecompress(
                    src_bytes, src_size, dst_bytes, dst_size, &out_sz);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_DECOMPRESSION_ERROR);
                }
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            } else {
                std::vector<uint8_t> staging(intermediate_size);
                const auto res = openscarlett::GDeflateDecompress(
                    src_bytes, src_size, staging.data(), staging.size(), &out_sz);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_DECOMPRESSION_ERROR);
                }
                if (out_sz > dst_size) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_REQUEST_TOO_LARGE);
                }
                std::memcpy(dst_bytes, staging.data(), out_sz);
            }
            if (out_decompressed_size) {
                *out_decompressed_size = out_sz;
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        case DSTORAGE_COMPRESSION_FORMAT_ZLIB:
        case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
            openscarlett::BcPackHeader hdr{};
            if (!openscarlett::BcPackParseHeader(src_bytes, src_size, hdr).ok() ||
                hdr.original_size == 0u) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (intermediate_size < static_cast<size_t>(hdr.original_size)) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < static_cast<size_t>(hdr.original_size)) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            std::vector<uint8_t> unpacked;
            if (!openscarlett::BcPackDecompress(src_bytes, src_size, unpacked).ok()) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (unpacked.size() > dst_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            if (!unpacked.empty()) {
                std::memcpy(dst_bytes, unpacked.data(), unpacked.size());
            }
            if (out_decompressed_size) {
                *out_decompressed_size = unpacked.size();
            }
            if (dst_class.is_physical_page) {
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        default:
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                            OS_DSTORAGE_E_INVALIDARG);
    }
}

}  // extern "C"
