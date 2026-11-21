#define OPENSCARLETT_BUILDING_LIBHTTPCLIENT 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct HCCallObject {
    std::atomic<int32_t> ref_count{1};
    std::mutex mutex;

    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::vector<uint8_t> request_body;

    bool completed = false;
    uint32_t response_status = 0;
    std::vector<std::pair<std::string, std::string>> response_headers;
    std::vector<uint8_t> response_body;
    std::string response_string;
};

namespace {

constexpr const char* kModName = "libHttpClient.GDK.dll";
constexpr const char* kLibVersion = "1.0.0-OpenScarlett";

struct MockRule {
    std::string method;       // empty = match any method
    std::string url_prefix;   // exact or prefix match
    uint32_t status_code = 200;
    std::string content_type = "application/json";
    std::vector<uint8_t> body;
};

struct HcState {
    std::mutex mutex;
    uint32_t init_count = 0;
    bool trace_to_debugger = false;
    HCTraceLevel trace_level = HCTraceLevel_Off;
    std::unordered_map<HCCallObject*, std::shared_ptr<HCCallObject>> live_calls;
    std::vector<MockRule> mocks;
};

HcState& GetHc() {
    static HcState* s = new HcState();
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

bool EqualsAsciiCaseInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::shared_ptr<HCCallObject> LookupCall(HCCallHandle handle) {
    if (!handle) return nullptr;
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.live_calls.find(handle);
    return it == st.live_calls.end() ? nullptr : it->second;
}

const char kIdHcPerform[] = "HCHttpCallPerformAsync";
const char kIdHcCleanup[] = "HCCleanupAsync";

struct HcPerformAsyncCtx {
    std::shared_ptr<HCCallObject> call;
};

XGameResult OPENSCARLETT_GDK_CALL HcPerformProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<HcPerformAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            if (!ctx || !ctx->call) {
                XAsyncComplete(data->async, XGR_E_INVALIDARG, 0);
                return XGR_E_PENDING;
            }
            std::string method;
            std::string url;
            size_t req_body_size = 0;
            {
                std::lock_guard<std::mutex> lk(ctx->call->mutex);
                method = ctx->call->method;
                url = ctx->call->url;
                req_body_size = ctx->call->request_body.size();
            }

            // Look up most recently registered matching mock rule.
            bool matched_mock = false;
            MockRule chosen{};
            {
                auto& st = GetHc();
                std::lock_guard<std::mutex> lk(st.mutex);
                for (size_t i = st.mocks.size(); i > 0; --i) {
                    const auto& rule = st.mocks[i - 1u];
                    const bool method_ok =
                        rule.method.empty() || EqualsAsciiCaseInsensitive(rule.method, method);
                    const bool url_ok =
                        rule.url_prefix.empty() || url.rfind(rule.url_prefix, 0) == 0;
                    if (method_ok && url_ok) {
                        chosen = rule;
                        matched_mock = true;
                        break;
                    }
                }
            }

            uint32_t resp_status = 200u;
            std::string content_type = "application/json";
            std::vector<uint8_t> resp_body;

            if (matched_mock) {
                resp_status = chosen.status_code;
                content_type = chosen.content_type.empty()
                                   ? "application/octet-stream"
                                   : chosen.content_type;
                resp_body = std::move(chosen.body);
            } else {
                std::ostringstream oss;
                oss << "{\"status\":\"ok\",\"offline\":true,\"method\":\""
                    << EscapeJsonString(method) << "\",\"url\":\""
                    << EscapeJsonString(url) << "\",\"requestBytes\":"
                    << req_body_size << "}";
                const std::string json = oss.str();
                resp_body.assign(json.begin(), json.end());
            }

            {
                std::lock_guard<std::mutex> lk(ctx->call->mutex);
                ctx->call->response_status = resp_status;
                ctx->call->response_body = std::move(resp_body);
                ctx->call->response_string.assign(
                    reinterpret_cast<const char*>(ctx->call->response_body.data()),
                    ctx->call->response_body.size());
                ctx->call->response_headers.clear();
                ctx->call->response_headers.emplace_back("Content-Type", content_type);
                ctx->call->response_headers.emplace_back(
                    "Content-Length", std::to_string(ctx->call->response_body.size()));
                ctx->call->response_headers.emplace_back("Server", "OpenScarlett-Offline-HTTP/1.0");
                ctx->call->response_headers.emplace_back(
                    "X-OpenScarlett-Mock", matched_mock ? "registered" : "loopback");
                ctx->call->completed = true;
            }

            XAsyncComplete(data->async, XGR_S_OK, 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

XGameResult OPENSCARLETT_GDK_CALL HcCleanupProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            auto& st = GetHc();
            std::lock_guard<std::mutex> lk(st.mutex);
            if (st.init_count > 0u) {
                --st.init_count;
            }
            if (st.init_count == 0u) {
                st.mocks.clear();
            }
            XAsyncComplete(data->async, XGR_S_OK, 0);
            return XGR_E_PENDING;
        }
        default:
            return XGR_S_OK;
    }
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CALL HCInitialize(const HCInitArgs* /*args*/) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    return RecordHr("HCInitialize", OS_SHIM_S_OK);
}

void OS_SHIM_CALL HCCleanup(void) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    if (st.init_count == 0u) {
        st.mocks.clear();
    }
    RecordOk("HCCleanup");
}

int32_t OS_SHIM_CALL HCCleanupAsync(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("HCCleanupAsync", OS_SHIM_E_INVALIDARG);
    }
    XGameResult hr = XAsyncBegin(async, nullptr, kIdHcCleanup, kIdHcCleanup, &HcCleanupProvider);
    if (hr < 0) {
        return RecordHr("HCCleanupAsync", hr);
    }
    hr = XAsyncSchedule(async, 0u);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("HCCleanupAsync", hr);
}

int32_t OS_SHIM_CALL HCGetLibVersion(const char** version) {
    if (!version) {
        return RecordHr("HCGetLibVersion", OS_SHIM_E_INVALIDARG);
    }
    *version = kLibVersion;
    return RecordHr("HCGetLibVersion", OS_SHIM_S_OK);
}

void OS_SHIM_CALL HCTraceSetTraceToDebugger(bool traceToDebugger) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.trace_to_debugger = traceToDebugger;
    RecordOk("HCTraceSetTraceToDebugger");
}

int32_t OS_SHIM_CALL HCSettingsSetTraceLevel(HCTraceLevel traceLevel) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.trace_level = traceLevel;
    return RecordHr("HCSettingsSetTraceLevel", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallCreate(HCCallHandle* call) {
    if (!call) {
        return RecordHr("HCHttpCallCreate", OS_SHIM_E_INVALIDARG);
    }
    *call = nullptr;
    auto obj = std::make_shared<HCCallObject>();
    HCCallHandle raw = obj.get();
    {
        auto& st = GetHc();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.live_calls[raw] = std::move(obj);
    }
    *call = raw;
    return RecordHr("HCHttpCallCreate", OS_SHIM_S_OK);
}

HCCallHandle OS_SHIM_CALL HCHttpCallDuplicateHandle(HCCallHandle call) {
    auto obj = LookupCall(call);
    if (!obj) {
        RecordHr("HCHttpCallDuplicateHandle", OS_SHIM_E_HANDLE);
        return nullptr;
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    RecordOk("HCHttpCallDuplicateHandle");
    return call;
}

int32_t OS_SHIM_CALL HCHttpCallCloseHandle(HCCallHandle call) {
    if (!call) {
        return RecordHr("HCHttpCallCloseHandle", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.live_calls.find(call);
    if (it == st.live_calls.end()) {
        return RecordHr("HCHttpCallCloseHandle", OS_SHIM_E_HANDLE);
    }
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        st.live_calls.erase(it);
    }
    return RecordHr("HCHttpCallCloseHandle", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetUrl(
    HCCallHandle call,
    const char* method,
    const char* url) {
    if (!call || !method || !*method || !url || !*url) {
        return RecordHr("HCHttpCallRequestSetUrl", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallRequestSetUrl", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->method = method;
    obj->url = url;
    return RecordHr("HCHttpCallRequestSetUrl", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetHeader(
    HCCallHandle call,
    const char* headerName,
    const char* headerValue,
    bool /*allowTracing*/) {
    if (!call || !headerName || !*headerName || !headerValue) {
        return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    for (auto& kv : obj->request_headers) {
        if (EqualsAsciiCaseInsensitive(kv.first, headerName)) {
            kv.second = headerValue;
            return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_S_OK);
        }
    }
    obj->request_headers.emplace_back(headerName, headerValue);
    return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* requestBodyBytes,
    uint32_t requestBodySize) {
    if (!call || (requestBodySize > 0u && !requestBodyBytes)) {
        return RecordHr("HCHttpCallRequestSetRequestBodyBytes", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallRequestSetRequestBodyBytes", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    if (requestBodySize > 0u) {
        obj->request_body.assign(requestBodyBytes, requestBodyBytes + requestBodySize);
    } else {
        obj->request_body.clear();
    }
    return RecordHr("HCHttpCallRequestSetRequestBodyBytes", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetRequestBodyString(
    HCCallHandle call,
    const char* requestBodyString) {
    if (!call || !requestBodyString) {
        return RecordHr("HCHttpCallRequestSetRequestBodyString", OS_SHIM_E_INVALIDARG);
    }
    const size_t len = std::strlen(requestBodyString);
    const int32_t hr = HCHttpCallRequestSetRequestBodyBytes(
        call,
        reinterpret_cast<const uint8_t*>(requestBodyString),
        static_cast<uint32_t>(len));
    return RecordHr("HCHttpCallRequestSetRequestBodyString", hr);
}

int32_t OS_SHIM_CALL HCHttpCallPerformAsync(HCCallHandle call, XAsyncBlock* asyncBlock) {
    if (!call || !asyncBlock) {
        return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_HANDLE);
    }
    {
        std::lock_guard<std::mutex> lk(obj->mutex);
        if (obj->url.empty()) {
            return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_INVALIDARG);
        }
    }

    auto* ctx = new (std::nothrow) HcPerformAsyncCtx{std::move(obj)};
    if (!ctx) {
        return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_OUTOFMEMORY);
    }

    XGameResult hr = XAsyncBegin(asyncBlock, ctx, kIdHcPerform, kIdHcPerform, &HcPerformProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("HCHttpCallPerformAsync", hr);
    }
    hr = XAsyncSchedule(asyncBlock, 0u);
    if (hr < 0) {
        XAsyncComplete(asyncBlock, hr, 0);
    }
    return RecordHr("HCHttpCallPerformAsync", hr);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetStatusCode(
    HCCallHandle call,
    uint32_t* statusCode) {
    if (!call || !statusCode) {
        return RecordHr("HCHttpCallResponseGetStatusCode", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetStatusCode", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    *statusCode = obj->response_status;
    return RecordHr("HCHttpCallResponseGetStatusCode", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseString(
    HCCallHandle call,
    const char** responseString) {
    if (!call || !responseString) {
        return RecordHr("HCHttpCallResponseGetResponseString", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetResponseString", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    *responseString = obj->response_string.c_str();
    return RecordHr("HCHttpCallResponseGetResponseString", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseBodyBytesSize(
    HCCallHandle call,
    size_t* bufferSize) {
    if (!call || !bufferSize) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytesSize", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytesSize", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    *bufferSize = obj->response_body.size();
    return RecordHr("HCHttpCallResponseGetResponseBodyBytesSize", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed) {
    if (!call || (!buffer && bufferSize > 0u)) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    const size_t n = obj->response_body.size();
    if (bufferUsed) {
        *bufferUsed = n;
    }
    if (bufferSize < n) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
    }
    if (n > 0u && buffer) {
        std::memcpy(buffer, obj->response_body.data(), n);
    }
    return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetHeader(
    HCCallHandle call,
    const char* headerName,
    const char** headerValue) {
    if (!call || !headerName || !*headerName || !headerValue) {
        return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_E_INVALIDARG);
    }
    *headerValue = nullptr;
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    for (const auto& kv : obj->response_headers) {
        if (EqualsAsciiCaseInsensitive(kv.first, headerName)) {
            *headerValue = kv.second.c_str();
            return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_S_OK);
        }
    }
    return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCAddMockResponseForTest(
    const char* method,
    const char* urlPrefix,
    uint32_t statusCode,
    const char* contentType,
    const uint8_t* body,
    size_t bodySize) {
    if (bodySize > 0u && !body) {
        return RecordHr("HCAddMockResponseForTest", OS_SHIM_E_INVALIDARG);
    }
    MockRule rule{};
    rule.method = method ? method : "";
    rule.url_prefix = urlPrefix ? urlPrefix : "";
    rule.status_code = statusCode == 0u ? 200u : statusCode;
    rule.content_type = (contentType && *contentType) ? contentType : "application/json";
    if (bodySize > 0u) {
        rule.body.assign(body, body + bodySize);
    }
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.mocks.push_back(std::move(rule));
    return RecordHr("HCAddMockResponseForTest", OS_SHIM_S_OK);
}

void OS_SHIM_CALL HCClearMockResponsesForTest(void) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.mocks.clear();
    RecordOk("HCClearMockResponsesForTest");
}

}  // extern "C"
