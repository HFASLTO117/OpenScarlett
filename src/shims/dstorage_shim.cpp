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

    int32_t ReadAt(uint64_t offset, uint32_t size, std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lk(io_mutex_);
#if defined(_WIN32)
        if (file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_handle_, pos, nullptr, FILE_BEGIN)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        out.resize(size);
        uint32_t total_read = 0;
        while (total_read < size) {
            DWORD chunk = 0;
            if (!ReadFile(file_handle_, out.data() + total_read, size - total_read, &chunk, nullptr)) {
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
    explicit DStorageQueueImpl(const DSTORAGE_QUEUE_DESC& desc) : desc_(desc) {
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
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) || GuidEquals(riid, IID_IDStorageQueueX)) {
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
        auto cancel_in = [&](std::deque<QueueItem>& q) {
            for (auto& item : q) {
                if (item.kind == QueueItem::Kind::Request &&
                    (item.request.CancellationTag & mask) == value) {
                    item.canceled = true;
                }
            }
        };
        cancel_in(staged_);
        cancel_in(active_);
        if (executing_request_ && (executing_tag_ & mask) == value) {
            executing_canceled_ = true;
        }
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
        if (req.Options.DestinationType != DSTORAGE_REQUEST_DESTINATION_MEMORY ||
            !req.Destination.Memory.Buffer || req.Destination.Memory.Size == 0) {
            return OS_DSTORAGE_E_INVALIDARG;
        }

        // 1. Acquire raw source bytes (file or memory).
        std::vector<uint8_t> file_buf;
        const uint8_t* src_ptr = nullptr;
        size_t src_size = 0;

        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!req.Source.File.Source || req.Source.File.Size == 0) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            auto* file_impl = static_cast<DStorageFileImpl*>(req.Source.File.Source);
            const int32_t rd_hr = file_impl->ReadAt(
                req.Source.File.Offset, req.Source.File.Size, file_buf);
            if (rd_hr < 0) {
                return rd_hr;
            }
            src_ptr = file_buf.data();
            src_size = file_buf.size();
        } else if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY) {
            if (!req.Source.Memory.Source || req.Source.Memory.Size == 0) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            src_ptr = static_cast<const uint8_t*>(req.Source.Memory.Source);
            src_size = req.Source.Memory.Size;
        } else {
            return OS_DSTORAGE_E_INVALIDARG;
        }

        auto* dst_ptr = static_cast<uint8_t*>(req.Destination.Memory.Buffer);
        const size_t dst_cap = req.Destination.Memory.Size;

        // 2. Decompress or copy according to CompressionFormat.
        switch (req.Options.CompressionFormat) {
            case DSTORAGE_COMPRESSION_FORMAT_NONE: {
                if (src_size > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                std::memcpy(dst_ptr, src_ptr, src_size);
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
                size_t out_size = 0;
                const auto res = openscarlett::GDeflateDecompress(
                    src_ptr, src_size, dst_ptr, dst_cap, &out_size);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (req.UncompressedSize != 0 && out_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_ZLIB:
            case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
                std::vector<uint8_t> unpacked;
                const auto st = openscarlett::BcPackDecompress(src_ptr, src_size, unpacked);
                if (!st.ok()) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (unpacked.size() > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0 && unpacked.size() != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (!unpacked.empty()) {
                    std::memcpy(dst_ptr, unpacked.data(), unpacked.size());
                }
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
                }
            }
            space_cv_.notify_all();

            switch (item.kind) {
                case QueueItem::Kind::Request: {
                    int32_t hr = OS_DSTORAGE_S_OK;
                    if (!item.canceled) {
                        hr = ExecuteRequest(item.request);
                    }
                    bool was_canceled = item.canceled;
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        was_canceled = was_canceled || executing_canceled_;
                        executing_request_ = false;
                        executing_tag_ = 0u;
                        executing_canceled_ = false;
                    }
                    if (!was_canceled && hr < 0) {
                        if (batch_hr >= 0) batch_hr = hr;
                        RecordFailure(hr, item.request);
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
            GuidEquals(riid, IID_IDStorageFactoryX)) {
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
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_NOINTERFACE, 3);
        }
        auto* q = new (std::nothrow) DStorageQueueImpl(*desc);
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
        staging_size_.store(size, std::memory_order_relaxed);
        return RecordHr("IDStorageFactoryX::SetStagingBufferSize", OS_DSTORAGE_S_OK, 7);
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> debug_flags_{0};
    std::atomic<uint32_t> staging_size_{32u * 1024u * 1024u};
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

}  // extern "C"
