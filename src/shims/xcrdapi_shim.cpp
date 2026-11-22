#define OPENSCARLETT_BUILDING_XCRDAPI 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/gdeflate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kModName = "xcrdapi.dll";
constexpr uint32_t kMaxBreadcrumbSlots = 256u;

struct BreadcrumbEntry {
    std::string tag;
    uint64_t value = 0;
};

struct ErrorReportEntry {
    uint32_t error_code = 0;
    std::string component;
    std::string message;
};

struct CrdState {
    std::mutex mutex;
    uint32_t init_count = 0;
    uint32_t flags = 0;
    std::map<uint32_t, BreadcrumbEntry> breadcrumbs;
    std::vector<ErrorReportEntry> errors;
};

CrdState& GetCrd() {
    static CrdState* s = new CrdState();
    return *s;
}

constexpr int32_t kCrdHrNotImpl = static_cast<int32_t>(0x80004001u);

inline int32_t RecordHr(const char* sym, int32_t hr) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == kCrdHrNotImpl) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (hr < 0) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        outcome,
        hr,
        -1);
    return hr;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

std::string EscapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CALL XCrdInitialize(uint32_t flags) {
    auto& st = GetCrd();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    st.flags = flags;
    return RecordHr("XCrdInitialize", OS_SHIM_S_OK);
}

void OS_SHIM_CALL XCrdUninitialize(void) {
    auto& st = GetCrd();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    if (st.init_count == 0u) {
        st.flags = 0u;
    }
    RecordOk("XCrdUninitialize");
}

int32_t OS_SHIM_CALL XCrdGetDiagnosticInfo(XCrdDiagnosticInfo* outInfo) {
    if (!outInfo) {
        return RecordHr("XCrdGetDiagnosticInfo", OS_SHIM_E_POINTER);
    }
    if (outInfo->structSize != 0u && outInfo->structSize < sizeof(XCrdDiagnosticInfo)) {
        return RecordHr("XCrdGetDiagnosticInfo", OS_SHIM_E_INVALIDARG);
    }

    auto& st = GetCrd();
    std::lock_guard<std::mutex> lk(st.mutex);

    std::memset(outInfo, 0, sizeof(XCrdDiagnosticInfo));
    outInfo->structSize = static_cast<uint32_t>(sizeof(XCrdDiagnosticInfo));
    outInfo->initialized = st.init_count > 0u ? 1u : 0u;
    outInfo->breadcrumbCount = static_cast<uint32_t>(st.breadcrumbs.size());
    outInfo->errorReportCount = static_cast<uint32_t>(st.errors.size());
    outInfo->flags = st.flags;
    std::snprintf(outInfo->consoleFamily, sizeof(outInfo->consoleFamily), "%s",
                  "Xbox Series X (OpenScarlett)");
    if (!st.errors.empty()) {
        const auto& last = st.errors.back();
        outInfo->lastErrorCode = last.error_code;
        std::snprintf(outInfo->lastErrorComponent, sizeof(outInfo->lastErrorComponent), "%s",
                      last.component.c_str());
        std::snprintf(outInfo->lastErrorMessage, sizeof(outInfo->lastErrorMessage), "%s",
                      last.message.c_str());
    }
    return RecordHr("XCrdGetDiagnosticInfo", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XCrdSetBreadcrumb(uint32_t slot, const char* tag, uint64_t value) {
    if (slot >= kMaxBreadcrumbSlots || !tag || !*tag) {
        return RecordHr("XCrdSetBreadcrumb", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetCrd();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.breadcrumbs[slot] = BreadcrumbEntry{std::string(tag), value};
    return RecordHr("XCrdSetBreadcrumb", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XCrdGetBreadcrumb(
    uint32_t slot,
    char* outTag,
    size_t tagBufferSize,
    uint64_t* outValue) {
    if (slot >= kMaxBreadcrumbSlots) {
        return RecordHr("XCrdGetBreadcrumb", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetCrd();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.breadcrumbs.find(slot);
    if (it == st.breadcrumbs.end()) {
        return RecordHr("XCrdGetBreadcrumb", OS_SHIM_E_NOT_FOUND);
    }
    if (outValue) {
        *outValue = it->second.value;
    }
    if (outTag) {
        const size_t needed = it->second.tag.size() + 1u;
        if (tagBufferSize < needed) {
            return RecordHr("XCrdGetBreadcrumb", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        }
        std::memcpy(outTag, it->second.tag.c_str(), needed);
    }
    return RecordHr("XCrdGetBreadcrumb", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XCrdReportError(
    uint32_t errorCode,
    const char* component,
    const char* message) {
    if (!component || !message) {
        return RecordHr("XCrdReportError", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetCrd();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.errors.push_back(ErrorReportEntry{errorCode, std::string(component), std::string(message)});
    return RecordHr("XCrdReportError", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XCrdFlushDiagnostics(const char* optionalOutputPath) {
    auto& st = GetCrd();
    std::ostringstream oss;
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        oss << "{\n";
        oss << "  \"module\": \"xcrdapi.dll\",\n";
        oss << "  \"initialized\": " << (st.init_count > 0u ? "true" : "false") << ",\n";
        oss << "  \"breadcrumbs\": [\n";
        size_t bidx = 0;
        for (const auto& [slot, bc] : st.breadcrumbs) {
            oss << "    {\"slot\": " << slot
                << ", \"tag\": \"" << EscapeJson(bc.tag)
                << "\", \"value\": " << bc.value << "}";
            if (++bidx < st.breadcrumbs.size()) oss << ",";
            oss << "\n";
        }
        oss << "  ],\n";
        oss << "  \"errors\": [\n";
        for (size_t i = 0; i < st.errors.size(); ++i) {
            oss << "    {\"code\": " << st.errors[i].error_code
                << ", \"component\": \"" << EscapeJson(st.errors[i].component)
                << "\", \"message\": \"" << EscapeJson(st.errors[i].message) << "\"}";
            if (i + 1u < st.errors.size()) oss << ",";
            oss << "\n";
        }
        oss << "  ]\n";
        oss << "}\n";
    }

    if (optionalOutputPath && *optionalOutputPath) {
        fs::path out_path(optionalOutputPath);
        std::error_code ec;
        if (out_path.has_parent_path()) {
            fs::create_directories(out_path.parent_path(), ec);
        }
        std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return RecordHr("XCrdFlushDiagnostics", OS_SHIM_E_FAIL);
        }
        const std::string text = oss.str();
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
        ofs.flush();
        if (!ofs.good()) {
            return RecordHr("XCrdFlushDiagnostics", OS_SHIM_E_FAIL);
        }
    }

    return RecordHr("XCrdFlushDiagnostics", OS_SHIM_S_OK);
}

// Fail-closed Xbox hardware crash-dump / cloud upload APIs (never fake S_OK).
OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdStartMiniDumpCapture(
    uint32_t dumpFlags,
    const char* outputPath) {
    (void)dumpFlags;
    (void)outputPath;
    return RecordHr("XCrdStartMiniDumpCapture", kCrdHrNotImpl);
}

OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdUploadCrashDump(
    const char* dumpPath,
    uint32_t uploadFlags) {
    (void)dumpPath;
    (void)uploadFlags;
    return RecordHr("XCrdUploadCrashDump", kCrdHrNotImpl);
}

OS_XCRDAPI_API void OS_SHIM_CALL OpenScarlettXCrdResetTelemetry(void) {
    openscarlett::CallTelemetry::Global().Reset();
}

OS_XCRDAPI_API int OS_SHIM_CALL OpenScarlettXCrdGetTelemetryDetail(
    const char* symbol_name,
    uint64_t* out_total_calls,
    uint64_t* out_not_impl_calls,
    int32_t* out_last_hr) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_total_calls) *out_total_calls = entry.total_calls;
            if (out_not_impl_calls) *out_not_impl_calls = entry.not_impl_calls;
            if (out_last_hr) *out_last_hr = entry.last_hr;
            return 1;
        }
    }
    if (out_total_calls) *out_total_calls = 0;
    if (out_not_impl_calls) *out_not_impl_calls = 0;
    if (out_last_hr) *out_last_hr = 0;
    return 0;
}

OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdValidateIntermediateBuffer(
    size_t requiredSize,
    size_t intermediateSize,
    size_t maxAllowedSize) {
    constexpr size_t kDefaultMaxIntermediate = 256u * 1024u * 1024u;
    const size_t limit = (maxAllowedSize != 0u) ? maxAllowedSize : kDefaultMaxIntermediate;
    if (requiredSize == 0u || intermediateSize == 0u || intermediateSize > limit) {
        return RecordHr("XCrdValidateIntermediateBuffer", OS_SHIM_E_INVALIDARG);
    }
    if (intermediateSize < requiredSize) {
        return RecordHr("XCrdValidateIntermediateBuffer", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
    }
    return RecordHr("XCrdValidateIntermediateBuffer", OS_SHIM_S_OK);
}

OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdDecompressToBuffer(
    uint32_t compressionFormat,
    const void* src,
    size_t srcSize,
    void* dst,
    size_t dstSize,
    size_t intermediateSize,
    size_t* outDecompressedSize) {
    if (outDecompressedSize) {
        *outDecompressedSize = 0u;
    }
    if (!src || !dst || srcSize == 0u || dstSize == 0u) {
        return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_INVALIDARG);
    }
    constexpr size_t kMaxIntermediate = 256u * 1024u * 1024u;
    if (intermediateSize == 0u || intermediateSize > kMaxIntermediate) {
        return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_INVALIDARG);
    }

#if defined(_WIN32)
    // Verify destination is committed & writable (supports kernelx/xmem physical pages).
    uintptr_t cursor = reinterpret_cast<uintptr_t>(dst);
    if (cursor + dstSize < cursor) {
        return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_INVALIDARG);
    }
    size_t rem = dstSize;
    while (rem > 0u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0u ||
            mbi.State != MEM_COMMIT) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_INVALIDARG);
        }
        const DWORD base_prot = mbi.Protect & 0xFFu;
        const bool writable =
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0u &&
            (base_prot == PAGE_READWRITE || base_prot == PAGE_WRITECOPY ||
             base_prot == PAGE_EXECUTE_READWRITE || base_prot == PAGE_EXECUTE_WRITECOPY);
        if (!writable) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_INVALIDARG);
        }
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= cursor) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_INVALIDARG);
        }
        const size_t step = std::min(rem, static_cast<size_t>(end - cursor));
        cursor += step;
        rem -= step;
    }
#endif

    const auto* src_bytes = static_cast<const uint8_t*>(src);
    auto* dst_bytes = static_cast<uint8_t*>(dst);

    if (compressionFormat == 0u) {
        if (intermediateSize < srcSize || dstSize < srcSize) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        }
        std::memcpy(dst_bytes, src_bytes, srcSize);
        if (outDecompressedSize) *outDecompressedSize = srcSize;
        return RecordHr("XCrdDecompressToBuffer", OS_SHIM_S_OK);
    }

    if (compressionFormat == 1u) {  // GDeflate
        size_t needed = 0u;
        if (!openscarlett::GDeflateIsValidStream(src_bytes, srcSize) ||
            !openscarlett::GDeflateGetUncompressedSize(src_bytes, srcSize, &needed) ||
            needed == 0u) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_FAIL);
        }
        if (intermediateSize < needed || dstSize < needed) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        }
        size_t written = 0u;
        if (openscarlett::GDeflateDecompress(src_bytes, srcSize, dst_bytes, dstSize, &written) !=
            openscarlett::GDeflateResult::Ok) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_FAIL);
        }
        if (outDecompressedSize) *outDecompressedSize = written;
        return RecordHr("XCrdDecompressToBuffer", OS_SHIM_S_OK);
    }

    if (compressionFormat == 2u || compressionFormat == 3u) {  // BCPack
        openscarlett::BcPackHeader hdr{};
        if (!openscarlett::BcPackParseHeader(src_bytes, srcSize, hdr).ok() ||
            hdr.original_size == 0u) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_FAIL);
        }
        if (intermediateSize < hdr.original_size || dstSize < hdr.original_size) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        }
        std::vector<uint8_t> unpacked;
        if (!openscarlett::BcPackDecompress(src_bytes, srcSize, unpacked).ok()) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_FAIL);
        }
        if (unpacked.size() > dstSize) {
            return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
        }
        std::memcpy(dst_bytes, unpacked.data(), unpacked.size());
        if (outDecompressedSize) *outDecompressedSize = unpacked.size();
        return RecordHr("XCrdDecompressToBuffer", OS_SHIM_S_OK);
    }

    return RecordHr("XCrdDecompressToBuffer", OS_SHIM_E_INVALIDARG);
}

}  // extern "C"
