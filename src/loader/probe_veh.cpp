#include "openscarlett/loader/probe_veh.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/loader/pe_inspector.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace openscarlett {

namespace {

// ---------------------------------------------------------------------------
// String, Hex, JSON & Path Sanitization Helpers
// ---------------------------------------------------------------------------
std::string ToLowerAscii(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

std::string TrimAscii(std::string_view input) {
    size_t begin = 0;
    while (begin < input.size() &&
           std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return std::string(input.substr(begin, end - begin));
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string StripTrailingSeparators(std::string path_str) {
    while (path_str.size() > 3 &&
           (path_str.back() == '\\' || path_str.back() == '/')) {
        path_str.pop_back();
    }
    return path_str;
}

std::string ReplaceAllIgnoreCaseAscii(const std::string& text,
                                      const std::string& needle,
                                      const std::string& replacement) {
    if (needle.empty() || text.size() < needle.size()) {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        if (pos + needle.size() <= text.size() &&
            EqualsIgnoreCaseAscii(std::string_view(text.data() + pos, needle.size()), needle)) {
            out.append(replacement);
            pos += needle.size();
        } else {
            out.push_back(text[pos]);
            ++pos;
        }
    }
    return out;
}

std::string ExtractBasename(std::string_view path) {
    const size_t sep = path.find_last_of("\\/");
    if (sep == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(sep + 1));
}

std::string FormatHex64(uint64_t val) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(val));
    return std::string(buf);
}

std::string FormatHex32(uint32_t val) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(val));
    return std::string(buf);
}

std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const unsigned char b = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (b < 0x20u) {
                    char ubuf[8]{};
                    std::snprintf(ubuf, sizeof(ubuf), "\\u%04X", static_cast<unsigned>(b));
                    out.append(ubuf);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

#if defined(_WIN32)
std::string GetEnvVarA(const char* name) {
    const DWORD needed = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string buf(needed, '\0');
    const DWORD written = ::GetEnvironmentVariableA(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return buf;
}

std::string GetCurrentUsernameA() {
    std::string env_user = GetEnvVarA("USERNAME");
    if (!env_user.empty()) {
        return env_user;
    }
    // Optional lookup via advapi32 without requiring a hard link dependency.
    HMODULE adv = ::GetModuleHandleA("advapi32.dll");
    if (adv) {
        using PfnGetUserNameA = BOOL(WINAPI*)(LPSTR, LPDWORD);
        auto fn = reinterpret_cast<PfnGetUserNameA>(::GetProcAddress(adv, "GetUserNameA"));
        if (fn) {
            char buf[256]{};
            DWORD len = static_cast<DWORD>(sizeof(buf));
            if (fn(buf, &len) && len > 1) {
                return std::string(buf, len - 1);
            }
        }
    }
    return {};
}

uint64_t QueryTimestampQpc() noexcept {
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

uint32_t QueryCurrentThreadId() noexcept {
    return static_cast<uint32_t>(::GetCurrentThreadId());
}

void ResolveAddressModuleAndRva(const void* addr,
                                std::string& out_module_basename,
                                uint64_t& out_rva) {
    out_module_basename = "<unknown>";
    out_rva = 0;
    if (!addr) {
        return;
    }
    HMODULE mod = nullptr;
    const BOOL ok = ::GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(addr),
        &mod);
    if (!ok || !mod) {
        return;
    }
    const uintptr_t base_addr = reinterpret_cast<uintptr_t>(mod);
    const uintptr_t target_addr = reinterpret_cast<uintptr_t>(addr);
    if (target_addr >= base_addr) {
        out_rva = static_cast<uint64_t>(target_addr - base_addr);
    }
    char path_buf[MAX_PATH * 2]{};
    const DWORD len = ::GetModuleFileNameA(
        mod, path_buf, static_cast<DWORD>(sizeof(path_buf)));
    if (len > 0 && len < sizeof(path_buf)) {
        out_module_basename = ExtractBasename(std::string_view(path_buf, len));
    }
}
#else
uint64_t QueryTimestampQpc() noexcept { return 0; }
uint32_t QueryCurrentThreadId() noexcept { return 0; }
#endif

// ---------------------------------------------------------------------------
// Thread-Safe 50-Entry API Call Ring Buffer
// ---------------------------------------------------------------------------
struct GlobalProbeRing {
#if defined(_WIN32)
    SRWLOCK lock = SRWLOCK_INIT;
#else
    std::mutex mutex;
#endif
    std::array<ProbeApiCallRecord, kProbeApiRingCapacity> slots{};
    uint64_t next_seq = 0;
    size_t count = 0;
    size_t head = 0;  // Next write index
};

GlobalProbeRing& GetGlobalRing() {
    static GlobalProbeRing ring;
    return ring;
}

std::vector<ProbeApiCallRecord> SnapshotRingInternal(bool non_blocking) {
    GlobalProbeRing& ring = GetGlobalRing();
    std::vector<ProbeApiCallRecord> out;

#if defined(_WIN32)
    if (non_blocking) {
        if (!::TryAcquireSRWLockShared(&ring.lock)) {
            return out;
        }
    } else {
        ::AcquireSRWLockShared(&ring.lock);
    }
#else
    (void)non_blocking;
    std::lock_guard<std::mutex> lk(ring.mutex);
#endif

    out.reserve(ring.count);
    const size_t start = (ring.count == kProbeApiRingCapacity) ? ring.head : 0u;
    for (size_t i = 0; i < ring.count; ++i) {
        const size_t idx = (start + i) % kProbeApiRingCapacity;
        out.push_back(ring.slots[idx]);
    }

#if defined(_WIN32)
    ::ReleaseSRWLockShared(&ring.lock);
#endif
    return out;
}

// ---------------------------------------------------------------------------
// Dynamic Missing-Import Trampoline Runtime Entry & Dispatcher
// ---------------------------------------------------------------------------
struct TrampolineEntry {
    std::string dll_name;
    std::string symbol_name;
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    void* stub_ptr = nullptr;
    std::atomic<uint64_t> call_count{0};
};

// Called directly by generated x64 trampoline machine code:
//   RCX = TrampolineEntry* entry
//   RDX = uint64_t caller_arg0 (original RCX passed by caller)
// Returns E_NOTIMPL (0x80004001) in RAX/EAX.
uint64_t TrampolineDispatch(TrampolineEntry* entry, uint64_t caller_arg0) {
    if (entry != nullptr) {
        entry->call_count.fetch_add(1u, std::memory_order_relaxed);
        const std::string sym_label =
            entry->by_ordinal && entry->symbol_name.empty()
                ? ("#" + std::to_string(entry->ordinal))
                : entry->symbol_name;
        RecordProbeApiCall(entry->dll_name,
                           sym_label,
                           caller_arg0,
                           static_cast<int32_t>(kProbeNotImplHResult));
        CallTelemetry::Global().Record(entry->dll_name,
                                       sym_label,
                                       CallOutcome::NotImplemented,
                                       static_cast<int32_t>(kProbeNotImplHResult),
                                       -1);
    }
    return static_cast<uint64_t>(kProbeNotImplHResult);
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
constexpr size_t kStubSlotBytes = 64u;
constexpr size_t kTrampolinePageBytes = 4096u;
constexpr size_t kStubsPerPage = kTrampolinePageBytes / kStubSlotBytes;

void EmitX64TrampolineStub(uint8_t* dst, TrampolineEntry* entry) {
    // Fill slot with INT3 (0xCC) guard bytes first.
    std::memset(dst, 0xCC, kStubSlotBytes);

    const uint64_t entry_addr = reinterpret_cast<uint64_t>(entry);
    const uint64_t dispatch_addr = reinterpret_cast<uint64_t>(&TrampolineDispatch);

    size_t p = 0;
    // sub rsp, 0x28  (32B shadow space + 8B alignment so RSP % 16 == 0 before CALL)
    dst[p++] = 0x48;
    dst[p++] = 0x83;
    dst[p++] = 0xEC;
    dst[p++] = 0x28;

    // mov rdx, rcx   (forward caller's arg0 as 2nd argument to TrampolineDispatch)
    dst[p++] = 0x48;
    dst[p++] = 0x89;
    dst[p++] = 0xCA;

    // movabs rcx, entry_addr
    dst[p++] = 0x48;
    dst[p++] = 0xB9;
    std::memcpy(dst + p, &entry_addr, sizeof(entry_addr));
    p += sizeof(entry_addr);

    // movabs rax, dispatch_addr
    dst[p++] = 0x48;
    dst[p++] = 0xB8;
    std::memcpy(dst + p, &dispatch_addr, sizeof(dispatch_addr));
    p += sizeof(dispatch_addr);

    // call rax
    dst[p++] = 0xFF;
    dst[p++] = 0xD0;

    // add rsp, 0x28
    dst[p++] = 0x48;
    dst[p++] = 0x83;
    dst[p++] = 0xC4;
    dst[p++] = 0x28;

    // ret
    dst[p++] = 0xC3;
}
#endif

// ---------------------------------------------------------------------------
// Active VEH Monitor Global Slot
// ---------------------------------------------------------------------------
#if defined(_WIN32)
bool ShouldCaptureExceptionCode(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_GUARD_PAGE:
        case kOpenScarlettTestVehException:
            return true;
        default:
            return false;
    }
}
#endif

}  // namespace

// ===========================================================================
// Environment Helper & Ring Buffer Public API
// ===========================================================================
bool IsProbeUnknownImportsEnabledByEnv() {
#if defined(_WIN32)
    const std::string val = ToLowerAscii(TrimAscii(GetEnvVarA("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS")));
#else
    const char* raw = std::getenv("OPENSCARLETT_PROBE_UNKNOWN_IMPORTS");
    const std::string val = raw ? ToLowerAscii(TrimAscii(raw)) : std::string{};
#endif
    return val == "1" || val == "true" || val == "yes" || val == "on";
}

void RecordProbeApiCall(std::string_view module_name,
                        std::string_view symbol_name,
                        uint64_t arg0,
                        int32_t hr) {
    GlobalProbeRing& ring = GetGlobalRing();
    ProbeApiCallRecord rec{};
    rec.timestamp_qpc = QueryTimestampQpc();
    rec.thread_id = QueryCurrentThreadId();
    rec.module_name = std::string(module_name);
    rec.symbol_name = std::string(symbol_name);
    rec.arg0 = arg0;
    rec.hr = hr;

#if defined(_WIN32)
    ::AcquireSRWLockExclusive(&ring.lock);
#else
    std::lock_guard<std::mutex> lk(ring.mutex);
#endif
    rec.sequence = ++ring.next_seq;
    ring.slots[ring.head] = std::move(rec);
    ring.head = (ring.head + 1u) % kProbeApiRingCapacity;
    if (ring.count < kProbeApiRingCapacity) {
        ++ring.count;
    }
#if defined(_WIN32)
    ::ReleaseSRWLockExclusive(&ring.lock);
#endif
}

std::vector<ProbeApiCallRecord> SnapshotProbeApiRing() {
    return SnapshotRingInternal(false);
}

void ResetProbeApiRing() {
    GlobalProbeRing& ring = GetGlobalRing();
#if defined(_WIN32)
    ::AcquireSRWLockExclusive(&ring.lock);
#else
    std::lock_guard<std::mutex> lk(ring.mutex);
#endif
    for (auto& slot : ring.slots) {
        slot = ProbeApiCallRecord{};
    }
    ring.next_seq = 0;
    ring.count = 0;
    ring.head = 0;
#if defined(_WIN32)
    ::ReleaseSRWLockExclusive(&ring.lock);
#endif
}

// ===========================================================================
// MissingImportTrampolinePool Implementation
// ===========================================================================
struct MissingImportTrampolinePool::Impl {
    mutable std::mutex mutex;
    struct Page {
        void* base = nullptr;
        size_t size = 0;
        size_t used_slots = 0;
    };
    std::vector<Page> pages;
    std::vector<std::unique_ptr<TrampolineEntry>> entries;
    std::unordered_map<std::string, size_t> lookup;

    ~Impl() { ReleasePages(); }

    void ReleasePages() {
#if defined(_WIN32)
        for (const auto& pg : pages) {
            if (pg.base) {
                ::VirtualFree(pg.base, 0, MEM_RELEASE);
            }
        }
#endif
        pages.clear();
        entries.clear();
        lookup.clear();
    }
};

MissingImportTrampolinePool::MissingImportTrampolinePool()
    : impl_(std::make_unique<Impl>()) {}

MissingImportTrampolinePool::~MissingImportTrampolinePool() = default;

void* MissingImportTrampolinePool::GetOrCreateTrampoline(std::string_view dll_name,
                                                         std::string_view symbol_name,
                                                         uint16_t ordinal,
                                                         bool by_ordinal) {
    const std::string dll_clean = ExtractBasename(dll_name);
    const std::string sym_clean =
        (by_ordinal && symbol_name.empty())
            ? ("#" + std::to_string(ordinal))
            : std::string(symbol_name);
    const std::string key = ToLowerAscii(dll_clean) + "!" +
                            (by_ordinal ? ("#" + std::to_string(ordinal)) : sym_clean);

    std::lock_guard<std::mutex> lk(impl_->mutex);
    const auto it = impl_->lookup.find(key);
    if (it != impl_->lookup.end()) {
        return impl_->entries[it->second]->stub_ptr;
    }

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    Impl::Page* page = nullptr;
    if (!impl_->pages.empty() && impl_->pages.back().used_slots < kStubsPerPage) {
        page = &impl_->pages.back();
    } else {
        void* mem = ::VirtualAlloc(
            nullptr, kTrampolinePageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!mem) {
            return nullptr;
        }
        impl_->pages.push_back(Impl::Page{mem, kTrampolinePageBytes, 0});
        page = &impl_->pages.back();
    }

    auto entry = std::make_unique<TrampolineEntry>();
    entry->dll_name = dll_clean;
    entry->symbol_name = sym_clean;
    entry->ordinal = ordinal;
    entry->by_ordinal = by_ordinal;

    uint8_t* stub_addr = static_cast<uint8_t*>(page->base) + page->used_slots * kStubSlotBytes;
    entry->stub_ptr = stub_addr;

    DWORD old_prot = 0;
    const BOOL made_rw = ::VirtualProtect(page->base, page->size, PAGE_EXECUTE_READWRITE, &old_prot);
    EmitX64TrampolineStub(stub_addr, entry.get());
    if (made_rw) {
        DWORD tmp_prot = 0;
        (void)::VirtualProtect(page->base, page->size, PAGE_EXECUTE_READ, &tmp_prot);
    }
    ::FlushInstructionCache(::GetCurrentProcess(), stub_addr, kStubSlotBytes);

    ++page->used_slots;
    const size_t idx = impl_->entries.size();
    void* result = entry->stub_ptr;
    impl_->entries.push_back(std::move(entry));
    impl_->lookup.emplace(key, idx);
    return result;
#else
    return nullptr;
#endif
}

std::vector<ProbeMissingImportRecord> MissingImportTrampolinePool::SnapshotMissingImports() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    std::vector<ProbeMissingImportRecord> out;
    out.reserve(impl_->entries.size());
    for (const auto& e : impl_->entries) {
        ProbeMissingImportRecord rec{};
        rec.dll_name = e->dll_name;
        rec.symbol_name = e->symbol_name;
        rec.ordinal = e->ordinal;
        rec.by_ordinal = e->by_ordinal;
        rec.stub_address = reinterpret_cast<uint64_t>(e->stub_ptr);
        rec.call_count = e->call_count.load(std::memory_order_relaxed);
        out.push_back(std::move(rec));
    }
    std::sort(out.begin(), out.end(),
              [](const ProbeMissingImportRecord& a, const ProbeMissingImportRecord& b) {
                  if (a.dll_name != b.dll_name) return a.dll_name < b.dll_name;
                  if (a.symbol_name != b.symbol_name) return a.symbol_name < b.symbol_name;
                  return a.ordinal < b.ordinal;
              });
    return out;
}

void MissingImportTrampolinePool::Reset() {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->ReleasePages();
}

size_t MissingImportTrampolinePool::Size() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->entries.size();
}

// ===========================================================================
// PE Import Patcher & Scanner
// ===========================================================================
Status PatchPeImportsWithProbeTrampolines(void* module_base,
                                          MissingImportTrampolinePool& pool,
                                          ProbePePatchSummary* out_summary,
                                          const ProbeImportResolver& resolver) {
    if (out_summary) {
        *out_summary = ProbePePatchSummary{};
    }
    if (!module_base) {
        return Status::Error(StatusCode::InvalidArgument, "module_base is null");
    }

#if defined(_WIN32)
    auto* base = static_cast<uint8_t*>(module_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return Status::Error(StatusCode::ParseError, "Invalid DOS header signature");
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return Status::Error(StatusCode::UnsupportedFormat, "Not a PE32+ (x64) image");
    }

    const uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.VirtualAddress == 0 || imp_dir.Size == 0) {
        return Status::Ok();
    }
    if (imp_dir.VirtualAddress >= image_size) {
        return Status::Error(StatusCode::ParseError, "Import directory RVA out of bounds");
    }

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
    const size_t max_descs = (image_size - imp_dir.VirtualAddress) / sizeof(IMAGE_IMPORT_DESCRIPTOR);

    for (size_t d_idx = 0; d_idx < max_descs; ++d_idx) {
        const auto& d = desc[d_idx];
        if (d.Name == 0 && d.FirstThunk == 0 && d.OriginalFirstThunk == 0) {
            break;
        }
        if (d.Name == 0 || d.Name >= image_size || d.FirstThunk == 0 || d.FirstThunk >= image_size) {
            continue;
        }

        const char* dll_name_cstr = reinterpret_cast<const char*>(base + d.Name);
        const std::string dll_name(dll_name_cstr);
        const uint32_t int_rva = (d.OriginalFirstThunk != 0 && d.OriginalFirstThunk < image_size)
                                     ? d.OriginalFirstThunk
                                     : d.FirstThunk;

        HMODULE loaded_mod = nullptr;
        if (!resolver) {
            loaded_mod = ::GetModuleHandleA(dll_name.c_str());
            if (!loaded_mod) {
                loaded_mod = ::LoadLibraryA(dll_name.c_str());
            }
            if (!loaded_mod && out_summary) {
                ++out_summary->unresolved_dlls;
            }
        }

        const auto* int_table = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + int_rva);
        auto* iat_table = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d.FirstThunk);
        const size_t max_thunks =
            std::min<size_t>((image_size - int_rva) / sizeof(IMAGE_THUNK_DATA64),
                             (image_size - d.FirstThunk) / sizeof(IMAGE_THUNK_DATA64));

        bool dll_marked_unresolved = false;
        for (size_t t = 0; t < max_thunks; ++t) {
            const ULONGLONG orig_val = int_table[t].u1.Ordinal;
            if (orig_val == 0) {
                break;
            }
            if (out_summary) {
                ++out_summary->total_imports_scanned;
            }

            bool by_ordinal = false;
            uint16_t ordinal = 0;
            std::string sym_name;
            if ((orig_val & IMAGE_ORDINAL_FLAG64) != 0) {
                by_ordinal = true;
                ordinal = static_cast<uint16_t>(orig_val & 0xFFFFu);
            } else {
                const uint64_t ibn_rva = orig_val;
                if (ibn_rva + sizeof(uint16_t) + 1u >= image_size) {
                    continue;
                }
                const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + ibn_rva);
                sym_name = reinterpret_cast<const char*>(ibn->Name);
            }

            void* resolved_fn = nullptr;
            if (resolver) {
                resolved_fn = resolver(dll_name, sym_name, ordinal, by_ordinal);
                if (!resolved_fn && !dll_marked_unresolved) {
                    dll_marked_unresolved = true;
                    if (out_summary) {
                        ++out_summary->unresolved_dlls;
                    }
                }
            } else if (loaded_mod != nullptr) {
                LPCSTR proc_key = by_ordinal
                                      ? reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ordinal))
                                      : sym_name.c_str();
                resolved_fn = reinterpret_cast<void*>(::GetProcAddress(loaded_mod, proc_key));
            }

            void* target_fn = resolved_fn;
            bool used_trampoline = false;
            if (!target_fn) {
                target_fn = pool.GetOrCreateTrampoline(dll_name, sym_name, ordinal, by_ordinal);
                if (!target_fn) {
                    return Status::Error(StatusCode::OutOfMemory,
                                         "Failed to allocate missing-import trampoline");
                }
                used_trampoline = true;
            }

            DWORD old_prot = 0;
            const BOOL changed =
                ::VirtualProtect(&iat_table[t], sizeof(IMAGE_THUNK_DATA64), PAGE_READWRITE, &old_prot);
            iat_table[t].u1.Function = reinterpret_cast<ULONGLONG>(target_fn);
            if (changed) {
                DWORD tmp = 0;
                (void)::VirtualProtect(&iat_table[t], sizeof(IMAGE_THUNK_DATA64), old_prot, &tmp);
            }

            if (used_trampoline && out_summary) {
                ++out_summary->patched_slots;
                ProbeMissingImportRecord rec{};
                rec.dll_name = dll_name;
                rec.symbol_name = sym_name.empty() ? ("#" + std::to_string(ordinal)) : sym_name;
                rec.ordinal = ordinal;
                rec.by_ordinal = by_ordinal;
                rec.stub_address = reinterpret_cast<uint64_t>(target_fn);
                rec.call_count = 0;
                out_summary->missing_imports.push_back(std::move(rec));
            }
        }
    }
    return Status::Ok();
#else
    (void)pool;
    (void)resolver;
    return Status::Error(StatusCode::UnsupportedFormat, "PE IAT patching requires Windows x64");
#endif
}

Status ScanPeBytesForMissingImports(const std::vector<uint8_t>& pe_bytes,
                                    const std::string& label,
                                    std::vector<ProbeMissingImportRecord>& out_missing,
                                    const ProbeImportResolver& resolver) {
    out_missing.clear();
    PeInspectionReport rep{};
    const Status st = InspectPeBytes(pe_bytes, label, rep);
    if (!st.ok()) {
        return st;
    }

    for (const auto& mod : rep.imports) {
#if defined(_WIN32)
        HMODULE loaded = nullptr;
        if (!resolver) {
            loaded = ::GetModuleHandleA(mod.dll_name.c_str());
        }
#endif
        for (const auto& sym : mod.symbols) {
            void* resolved = nullptr;
            if (resolver) {
                resolved = resolver(mod.dll_name, sym.name, sym.ordinal, sym.by_ordinal);
            } else {
#if defined(_WIN32)
                if (loaded) {
                    LPCSTR proc_key = sym.by_ordinal
                                          ? reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(sym.ordinal))
                                          : sym.name.c_str();
                    resolved = reinterpret_cast<void*>(::GetProcAddress(loaded, proc_key));
                }
#endif
            }
            if (!resolved) {
                ProbeMissingImportRecord rec{};
                rec.dll_name = mod.dll_name;
                rec.symbol_name =
                    (sym.by_ordinal && sym.name.empty())
                        ? ("#" + std::to_string(sym.ordinal))
                        : sym.name;
                rec.ordinal = sym.ordinal;
                rec.by_ordinal = sym.by_ordinal;
                out_missing.push_back(std::move(rec));
            }
        }
    }
    return Status::Ok();
}

// ===========================================================================
// VehCrashMonitor Implementation
// ===========================================================================
struct VehCrashMonitor::Impl {
    mutable std::mutex mutex;
    void* veh_handle = nullptr;
    const MissingImportTrampolinePool* trampoline_pool = nullptr;
    std::string auto_export_path;
    std::vector<std::string> extra_local_roots;
    RecoveryHook recovery_hook;
    ProbeExceptionReport last_report{};

#if defined(_WIN32)
    static std::atomic<Impl*>& ActiveInstance() {
        static std::atomic<Impl*> active{nullptr};
        return active;
    }

    static LONG CALLBACK VehHandlerProc(PEXCEPTION_POINTERS ep) {
        if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (!ShouldCaptureExceptionCode(code)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        static thread_local bool tl_in_veh = false;
        if (tl_in_veh) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        tl_in_veh = true;
        struct ReentrancyGuard {
            ~ReentrancyGuard() { tl_in_veh = false; }
        } guard;

        Impl* self = ActiveInstance().load(std::memory_order_acquire);
        if (!self) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        ProbeExceptionReport report{};
        report.captured = true;
        report.code = static_cast<uint32_t>(code);
        report.flags = static_cast<uint32_t>(ep->ExceptionRecord->ExceptionFlags);
        report.fault_address = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
        report.thread_id = QueryCurrentThreadId();
        ResolveAddressModuleAndRva(ep->ExceptionRecord->ExceptionAddress,
                                   report.fault_module,
                                   report.fault_rva);

#if defined(_M_X64) || defined(__x86_64__)
        const CONTEXT* ctx = ep->ContextRecord;
        report.registers.rip = ctx->Rip;
        report.registers.rsp = ctx->Rsp;
        report.registers.rbp = ctx->Rbp;
        report.registers.rax = ctx->Rax;
        report.registers.rbx = ctx->Rbx;
        report.registers.rcx = ctx->Rcx;
        report.registers.rdx = ctx->Rdx;
        report.registers.rsi = ctx->Rsi;
        report.registers.rdi = ctx->Rdi;
        report.registers.r8 = ctx->R8;
        report.registers.r9 = ctx->R9;
        report.registers.r10 = ctx->R10;
        report.registers.r11 = ctx->R11;
        report.registers.r12 = ctx->R12;
        report.registers.r13 = ctx->R13;
        report.registers.r14 = ctx->R14;
        report.registers.r15 = ctx->R15;
        report.registers.eflags = ctx->EFlags;
#endif

        // Capture stack trace via kernel32!RtlCaptureStackBackTrace (zero dbghelp.dll dependency).
        using PfnCaptureStackBackTrace = USHORT(WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
        HMODULE k32 = ::GetModuleHandleA("kernel32.dll");
        auto capture_bt =
            k32 ? reinterpret_cast<PfnCaptureStackBackTrace>(
                      ::GetProcAddress(k32, "RtlCaptureStackBackTrace"))
                : nullptr;
        if (capture_bt) {
            void* raw_frames[64]{};
            const USHORT captured_count = capture_bt(0u, 64u, raw_frames, nullptr);
            report.stack_frames.reserve(captured_count);
            for (USHORT i = 0; i < captured_count; ++i) {
                ProbeStackFrame frame{};
                frame.address = reinterpret_cast<uint64_t>(raw_frames[i]);
                ResolveAddressModuleAndRva(raw_frames[i], frame.module_name, frame.module_rva);
                report.stack_frames.push_back(std::move(frame));
            }
        }
        if (report.stack_frames.empty() && report.fault_address != 0) {
            ProbeStackFrame fallback{};
            fallback.address = report.fault_address;
            fallback.module_name = report.fault_module;
            fallback.module_rva = report.fault_rva;
            report.stack_frames.push_back(std::move(fallback));
        }

        // Non-blocking snapshot of the last 50 API/D3D12 calls.
        report.api_ring_last_50 = SnapshotRingInternal(true);

        RecoveryHook hook_copy;
        std::string export_path_copy;
        std::vector<std::string> roots_copy;
        const MissingImportTrampolinePool* pool_copy = nullptr;
        {
            std::lock_guard<std::mutex> lk(self->mutex);
            self->last_report = report;
            hook_copy = self->recovery_hook;
            export_path_copy = self->auto_export_path;
            roots_copy = self->extra_local_roots;
            pool_copy = self->trampoline_pool;
        }

        if (!export_path_copy.empty()) {
            BringUpTelemetrySnapshot snap{};
            snap.probe_mode = true;
            snap.host_os = GetProbeHostOsSummary();
            if (pool_copy) {
                snap.missing_imports = pool_copy->SnapshotMissingImports();
            }
            snap.api_ring_last_50 = report.api_ring_last_50;
            snap.exception_report = report;
            (void)WriteSanitizedBringUpTelemetryJson(export_path_copy, snap, roots_copy);
        }

        if (hook_copy) {
            if (hook_copy(report, static_cast<void*>(ep))) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        } else if (code == kOpenScarlettTestVehException) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif
};

VehCrashMonitor::VehCrashMonitor() : impl_(std::make_unique<Impl>()) {}

VehCrashMonitor::~VehCrashMonitor() {
    Uninstall();
}

Status VehCrashMonitor::Install(uint32_t first_handler) {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (impl_->veh_handle != nullptr) {
        return Status::Ok();
    }
    Impl::ActiveInstance().store(impl_.get(), std::memory_order_release);
    PVOID handle = ::AddVectoredExceptionHandler(first_handler, &Impl::VehHandlerProc);
    if (!handle) {
        Impl::ActiveInstance().store(nullptr, std::memory_order_release);
        return Status::Error(StatusCode::DeviceError, "AddVectoredExceptionHandler failed");
    }
    impl_->veh_handle = handle;
    return Status::Ok();
#else
    (void)first_handler;
    return Status::Error(StatusCode::UnsupportedFormat, "VEH is only supported on Windows");
#endif
}

void VehCrashMonitor::Uninstall() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (impl_->veh_handle != nullptr) {
        ::RemoveVectoredExceptionHandler(impl_->veh_handle);
        impl_->veh_handle = nullptr;
    }
    Impl* expected = impl_.get();
    Impl::ActiveInstance().compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
#endif
}

bool VehCrashMonitor::IsInstalled() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->veh_handle != nullptr;
}

void VehCrashMonitor::SetTrampolinePool(const MissingImportTrampolinePool* pool) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->trampoline_pool = pool;
}

void VehCrashMonitor::SetAutoExportPath(std::string json_path,
                                        std::vector<std::string> extra_local_roots) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->auto_export_path = std::move(json_path);
    impl_->extra_local_roots = std::move(extra_local_roots);
}

void VehCrashMonitor::SetRecoveryHook(RecoveryHook hook) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->recovery_hook = std::move(hook);
}

ProbeExceptionReport VehCrashMonitor::GetLastReport() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->last_report;
}

void VehCrashMonitor::ClearLastReport() {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->last_report = ProbeExceptionReport{};
}

// ===========================================================================
// Privacy Sanitizer & Sanitized Bring-Up Telemetry JSON Exporter
// ===========================================================================
std::string SanitizeProbeText(std::string_view text,
                              const std::vector<std::string>& extra_local_roots) {
    if (text.empty()) {
        return {};
    }

    struct ReplaceRule {
        std::string key;
        std::string val;
    };
    std::vector<ReplaceRule> rules;

    auto add_path_rule = [&](const std::string& raw_path, const char* token) {
        const std::string clean = StripTrailingSeparators(TrimAscii(raw_path));
        if (clean.empty()) {
            return;
        }
        rules.push_back(ReplaceRule{clean, token});
        std::string fwd = clean;
        std::replace(fwd.begin(), fwd.end(), '\\', '/');
        if (fwd != clean) {
            rules.push_back(ReplaceRule{std::move(fwd), token});
        }
        std::string back = clean;
        std::replace(back.begin(), back.end(), '/', '\\');
        if (back != clean) {
            rules.push_back(ReplaceRule{std::move(back), token});
        }
    };

    for (const auto& root : extra_local_roots) {
        add_path_rule(root, "<LOCAL_ROOT>");
    }

#if defined(_WIN32)
    add_path_rule(GetEnvVarA("LOCALAPPDATA"), "<LOCALAPPDATA>");
    add_path_rule(GetEnvVarA("APPDATA"), "<APPDATA>");
    add_path_rule(GetEnvVarA("TEMP"), "<TEMP>");
    add_path_rule(GetEnvVarA("TMP"), "<TEMP>");
    add_path_rule(GetEnvVarA("USERPROFILE"), "<USER_HOME>");
#endif

    // Longest paths first so nested paths (<TEMP>, <LOCALAPPDATA>) match before <USER_HOME>.
    std::sort(rules.begin(), rules.end(), [](const ReplaceRule& a, const ReplaceRule& b) {
        if (a.key.size() != b.key.size()) {
            return a.key.size() > b.key.size();
        }
        return a.key < b.key;
    });

    std::string out(text);
    for (const auto& rule : rules) {
        out = ReplaceAllIgnoreCaseAscii(out, rule.key, rule.val);
    }

    // Scrub any <Drive>:\Users\<ProfileName> or <Drive>:/Users/<ProfileName> segment
    // (excluding system accounts like Public / Default / All) -> <Drive>:\Users\<USER>.
    {
        std::string scrubbed;
        scrubbed.reserve(out.size());
        size_t i = 0;
        while (i < out.size()) {
            const char ch = out[i];
            const bool is_drive = ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) &&
                                  (i + 2 < out.size()) && out[i + 1] == ':' &&
                                  (out[i + 2] == '\\' || out[i + 2] == '/');
            if (is_drive) {
                size_t j = i + 2;
                while (j < out.size() && (out[j] == '\\' || out[j] == '/')) {
                    ++j;
                }
                if (j + 5 < out.size() &&
                    EqualsIgnoreCaseAscii(std::string_view(out.data() + j, 5), "Users") &&
                    (out[j + 5] == '\\' || out[j + 5] == '/')) {
                    size_t name_start = j + 5;
                    while (name_start < out.size() &&
                           (out[name_start] == '\\' || out[name_start] == '/')) {
                        ++name_start;
                    }
                    size_t name_end = name_start;
                    while (name_end < out.size() &&
                           out[name_end] != '\\' && out[name_end] != '/' &&
                           out[name_end] != '\r' && out[name_end] != '\n' &&
                           out[name_end] != ' ' && out[name_end] != '\t' &&
                           out[name_end] != '"' && out[name_end] != '\'' &&
                           out[name_end] != ')' && out[name_end] != ']' &&
                           out[name_end] != ';') {
                        ++name_end;
                    }
                    if (name_end > name_start) {
                        const std::string seg_lower =
                            ToLowerAscii(std::string_view(out.data() + name_start, name_end - name_start));
                        if (seg_lower != "<user>" && seg_lower != "public" &&
                            seg_lower != "default" && seg_lower != "all") {
                            scrubbed.append(out, i, name_start - i);
                            scrubbed.append("<USER>");
                            i = name_end;
                            continue;
                        }
                    }
                }
            }
            scrubbed.push_back(ch);
            ++i;
        }
        out = std::move(scrubbed);
    }

#if defined(_WIN32)
    // Also scrub explicit \Users\<CurrentUsername> if no drive prefix was present.
    const std::string username = GetCurrentUsernameA();
    if (!username.empty()) {
        out = ReplaceAllIgnoreCaseAscii(out, "\\Users\\" + username, "\\Users\\<USER>");
        out = ReplaceAllIgnoreCaseAscii(out, "/Users/" + username, "/Users/<USER>");
    }
#endif

    return out;
}

std::string GetProbeHostOsSummary() {
#if defined(_WIN32)
    HMODULE ntdll = ::GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        using PfnRtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto rtl_ver =
            reinterpret_cast<PfnRtlGetVersion>(::GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtl_ver) {
            OSVERSIONINFOW osvi{};
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            if (rtl_ver(&osvi) == 0) {
                const char* name =
                    (osvi.dwBuildNumber >= 22000u) ? "Windows 11" : "Windows 10";
                char buf[96]{};
                std::snprintf(buf,
                              sizeof(buf),
                              "%s x64 (Build %lu)",
                              name,
                              static_cast<unsigned long>(osvi.dwBuildNumber));
                return std::string(buf);
            }
        }
    }
    return "Windows x64";
#else
    return "Non-Windows";
#endif
}

std::string ExportSanitizedBringUpTelemetryJson(
    const BringUpTelemetrySnapshot& snapshot,
    const std::vector<std::string>& extra_local_roots) {
    auto clean_str = [&](std::string_view raw) -> std::string {
        return JsonEscape(SanitizeProbeText(raw, extra_local_roots));
    };

    const std::string host_os =
        snapshot.host_os.empty() ? GetProbeHostOsSummary() : snapshot.host_os;

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"schema\": \"openscarlett.bringup_telemetry.v1\",\n";
    oss << "  \"probe_mode\": " << (snapshot.probe_mode ? "true" : "false") << ",\n";
    oss << "  \"host_os\": \"" << clean_str(host_os) << "\",\n";

    oss << "  \"missing_imports\": [\n";
    for (size_t i = 0; i < snapshot.missing_imports.size(); ++i) {
        const auto& m = snapshot.missing_imports[i];
        oss << "    {"
            << "\"dll\": \"" << clean_str(m.dll_name) << "\", "
            << "\"symbol\": \"" << clean_str(m.symbol_name) << "\", "
            << "\"ordinal\": " << m.ordinal << ", "
            << "\"by_ordinal\": " << (m.by_ordinal ? "true" : "false") << ", "
            << "\"stub_address\": \"" << FormatHex64(m.stub_address) << "\", "
            << "\"call_count\": " << m.call_count
            << "}" << (i + 1 < snapshot.missing_imports.size() ? "," : "") << "\n";
    }
    oss << "  ],\n";

    const auto& ring =
        !snapshot.api_ring_last_50.empty()
            ? snapshot.api_ring_last_50
            : snapshot.exception_report.api_ring_last_50;
    oss << "  \"api_ring_last_50\": [\n";
    for (size_t i = 0; i < ring.size(); ++i) {
        const auto& r = ring[i];
        oss << "    {"
            << "\"seq\": " << r.sequence << ", "
            << "\"thread_id\": " << r.thread_id << ", "
            << "\"module\": \"" << clean_str(r.module_name) << "\", "
            << "\"symbol\": \"" << clean_str(r.symbol_name) << "\", "
            << "\"arg0\": \"" << FormatHex64(r.arg0) << "\", "
            << "\"hr\": \"" << FormatHex32(static_cast<uint32_t>(r.hr)) << "\""
            << "}" << (i + 1 < ring.size() ? "," : "") << "\n";
    }
    oss << "  ],\n";

    const auto& ex = snapshot.exception_report;
    oss << "  \"exception_report\": {\n";
    oss << "    \"captured\": " << (ex.captured ? "true" : "false") << ",\n";
    oss << "    \"code\": \"" << FormatHex32(ex.code) << "\",\n";
    oss << "    \"flags\": \"" << FormatHex32(ex.flags) << "\",\n";
    oss << "    \"fault_module\": \"" << clean_str(ex.fault_module) << "\",\n";
    oss << "    \"fault_rva\": \"" << FormatHex64(ex.fault_rva) << "\",\n";
    oss << "    \"thread_id\": " << ex.thread_id << ",\n";
    oss << "    \"registers\": {\n";
    oss << "      \"rip\": \"" << FormatHex64(ex.registers.rip) << "\",\n";
    oss << "      \"rsp\": \"" << FormatHex64(ex.registers.rsp) << "\",\n";
    oss << "      \"rbp\": \"" << FormatHex64(ex.registers.rbp) << "\",\n";
    oss << "      \"rax\": \"" << FormatHex64(ex.registers.rax) << "\",\n";
    oss << "      \"rbx\": \"" << FormatHex64(ex.registers.rbx) << "\",\n";
    oss << "      \"rcx\": \"" << FormatHex64(ex.registers.rcx) << "\",\n";
    oss << "      \"rdx\": \"" << FormatHex64(ex.registers.rdx) << "\",\n";
    oss << "      \"rsi\": \"" << FormatHex64(ex.registers.rsi) << "\",\n";
    oss << "      \"rdi\": \"" << FormatHex64(ex.registers.rdi) << "\",\n";
    oss << "      \"r8\": \"" << FormatHex64(ex.registers.r8) << "\",\n";
    oss << "      \"r9\": \"" << FormatHex64(ex.registers.r9) << "\",\n";
    oss << "      \"r10\": \"" << FormatHex64(ex.registers.r10) << "\",\n";
    oss << "      \"r11\": \"" << FormatHex64(ex.registers.r11) << "\",\n";
    oss << "      \"r12\": \"" << FormatHex64(ex.registers.r12) << "\",\n";
    oss << "      \"r13\": \"" << FormatHex64(ex.registers.r13) << "\",\n";
    oss << "      \"r14\": \"" << FormatHex64(ex.registers.r14) << "\",\n";
    oss << "      \"r15\": \"" << FormatHex64(ex.registers.r15) << "\",\n";
    oss << "      \"eflags\": \"" << FormatHex32(ex.registers.eflags) << "\"\n";
    oss << "    },\n";
    oss << "    \"stack_frames\": [\n";
    for (size_t i = 0; i < ex.stack_frames.size(); ++i) {
        const auto& f = ex.stack_frames[i];
        oss << "      {"
            << "\"index\": " << i << ", "
            << "\"module\": \"" << clean_str(f.module_name) << "\", "
            << "\"rva\": \"" << FormatHex64(f.module_rva) << "\""
            << "}" << (i + 1 < ex.stack_frames.size() ? "," : "") << "\n";
    }
    oss << "    ]\n";
    oss << "  }\n";
    oss << "}\n";

    return oss.str();
}

Status WriteSanitizedBringUpTelemetryJson(
    const std::string& file_path,
    const BringUpTelemetrySnapshot& snapshot,
    const std::vector<std::string>& extra_local_roots) {
    if (file_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "file_path is empty");
    }
    std::error_code ec;
    const std::filesystem::path p(file_path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    const std::string json = ExportSanitizedBringUpTelemetryJson(snapshot, extra_local_roots);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::Error(StatusCode::IoError, "Failed to open telemetry JSON for writing");
    }
    ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!ofs.good()) {
        return Status::Error(StatusCode::IoError, "Failed writing telemetry JSON");
    }
    return Status::Ok();
}

}  // namespace openscarlett
