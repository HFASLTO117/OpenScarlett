#define OPENSCARLETT_BUILDING_XCRDAPI 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

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

inline int32_t RecordHr(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
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

}  // extern "C"
