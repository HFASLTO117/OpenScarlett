#define OPENSCARLETT_BUILDING_XCURL 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kModName = "XCurl.dll";
constexpr const char* kVersionStr = "XCurl/8.0.0-OpenScarlett (offline-loopback)";

struct EasyObject {
    std::string url;
    std::string method = "GET";
    std::string user_agent = "OpenScarlett-XCurl/1.0";
    std::vector<uint8_t> post_body;
    std::vector<std::string> headers;

    XCurlWriteCallback write_cb = nullptr;
    void* write_data = nullptr;
    XCurlWriteCallback header_cb = nullptr;
    void* header_data = nullptr;
    void* private_ptr = nullptr;

    long timeout_ms = 30000L;
    long connect_timeout_ms = 10000L;
    bool follow_location = true;
    bool nobody = false;
    bool verify_peer = true;
    bool verify_host = true;
    bool verbose = false;

    // Last transfer results
    long response_code = 0L;
    long header_size = 0L;
    long request_size = 0L;
    double total_time = 0.0;
    double size_download = 0.0;
    std::string effective_url;
    std::string content_type = "application/json";

    void Reset() {
        url.clear();
        method = "GET";
        user_agent = "OpenScarlett-XCurl/1.0";
        post_body.clear();
        headers.clear();
        write_cb = nullptr;
        write_data = nullptr;
        header_cb = nullptr;
        header_data = nullptr;
        private_ptr = nullptr;
        timeout_ms = 30000L;
        connect_timeout_ms = 10000L;
        follow_location = true;
        nobody = false;
        verify_peer = true;
        verify_host = true;
        verbose = false;
        response_code = 0L;
        header_size = 0L;
        request_size = 0L;
        total_time = 0.0;
        size_download = 0.0;
        effective_url.clear();
        content_type = "application/json";
    }
};

struct MultiObject {
    std::vector<XCurlEasyHandle> attached;
    std::vector<XCurlEasyHandle> pending;
    std::deque<XCurlMsg> completed_msgs;
    XCurlMsg current_msg{};
};

struct XCurlState {
    std::mutex mutex;
    uint32_t global_init_count = 0;
    std::unordered_map<void*, std::unique_ptr<EasyObject>> easy_handles;
    std::unordered_map<void*, std::unique_ptr<MultiObject>> multi_handles;
};

XCurlState& GetXCurl() {
    static XCurlState* s = new XCurlState();
    return *s;
}

inline int32_t RecordCode(const char* sym, int32_t code) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        code == 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        code,
        -1);
    return code;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

bool HasSupportedScheme(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

int32_t PerformOfflineTransfer(EasyObject& easy) {
    if (easy.url.empty()) {
        return XCURLE_URL_MALFORMAT;
    }
    if (!HasSupportedScheme(easy.url)) {
        return XCURLE_UNSUPPORTED_PROTOCOL;
    }

    easy.effective_url = easy.url;
    easy.response_code = 200L;
    easy.content_type = "application/json";
    easy.total_time = 0.001;
    easy.request_size = static_cast<long>(easy.url.size() + easy.method.size() + easy.post_body.size());

    std::string payload;
    if (!easy.nobody) {
        std::ostringstream oss;
        oss << "{\"service\":\"openscarlett-xcurl\",\"status\":200,\"method\":\""
            << EscapeJson(easy.method) << "\",\"url\":\""
            << EscapeJson(easy.url) << "\",\"postBytes\":"
            << easy.post_body.size() << "}";
        payload = oss.str();
    }

    const std::string hdrs =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Server: OpenScarlett-XCurl/1.0\r\n"
        "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    easy.header_size = static_cast<long>(hdrs.size());

    if (easy.header_cb) {
        size_t pos = 0;
        while (pos < hdrs.size()) {
            const size_t nl = hdrs.find('\n', pos);
            const size_t end = (nl == std::string::npos) ? hdrs.size() : (nl + 1u);
            const size_t line_len = end - pos;
            std::vector<char> line_buf(hdrs.begin() + static_cast<std::ptrdiff_t>(pos),
                                       hdrs.begin() + static_cast<std::ptrdiff_t>(end));
            const size_t written = easy.header_cb(line_buf.data(), 1u, line_len, easy.header_data);
            if (written != line_len) {
                return XCURLE_WRITE_ERROR;
            }
            pos = end;
        }
    }

    easy.size_download = static_cast<double>(payload.size());
    if (!payload.empty() && easy.write_cb) {
        std::vector<char> body_buf(payload.begin(), payload.end());
        const size_t written = easy.write_cb(body_buf.data(), 1u, body_buf.size(), easy.write_data);
        if (written != body_buf.size()) {
            return XCURLE_WRITE_ERROR;
        }
    }

    return XCURLE_OK;
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CDECL curl_global_init(long /*flags*/) {
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.global_init_count;
    return RecordCode("curl_global_init", XCURLE_OK);
}

void OS_SHIM_CDECL curl_global_cleanup(void) {
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.global_init_count > 0u) {
        --st.global_init_count;
    }
    RecordOk("curl_global_cleanup");
}

const char* OS_SHIM_CDECL curl_version(void) {
    RecordOk("curl_version");
    return kVersionStr;
}

XCurlEasyHandle OS_SHIM_CDECL curl_easy_init(void) {
    auto obj = std::make_unique<EasyObject>();
    void* raw = obj.get();
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.easy_handles[raw] = std::move(obj);
    }
    RecordOk("curl_easy_init");
    return raw;
}

void OS_SHIM_CDECL curl_easy_cleanup(XCurlEasyHandle curl) {
    RecordOk("curl_easy_cleanup");
    if (!curl) return;
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.easy_handles.erase(curl);
}

void OS_SHIM_CDECL curl_easy_reset(XCurlEasyHandle curl) {
    RecordOk("curl_easy_reset");
    if (!curl) return;
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.easy_handles.find(curl);
    if (it != st.easy_handles.end()) {
        it->second->Reset();
    }
}

int32_t OS_SHIM_CDECL curl_easy_setopt(XCurlEasyHandle curl, int32_t option, ...) {
    if (!curl) {
        return RecordCode("curl_easy_setopt", XCURLE_BAD_FUNCTION_ARGUMENT);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.easy_handles.find(curl);
    if (it == st.easy_handles.end()) {
        return RecordCode("curl_easy_setopt", XCURLE_BAD_FUNCTION_ARGUMENT);
    }
    EasyObject& easy = *it->second;

    va_list ap;
    va_start(ap, option);
    int32_t res = XCURLE_OK;

    switch (option) {
        case XCURLOPT_URL: {
            const char* s = va_arg(ap, const char*);
            easy.url = s ? s : "";
            break;
        }
        case XCURLOPT_USERAGENT: {
            const char* s = va_arg(ap, const char*);
            easy.user_agent = s ? s : "";
            break;
        }
        case XCURLOPT_CUSTOMREQUEST: {
            const char* s = va_arg(ap, const char*);
            if (s && *s) easy.method = s;
            break;
        }
        case XCURLOPT_POST: {
            const long v = va_arg(ap, long);
            easy.method = (v != 0L) ? "POST" : "GET";
            break;
        }
        case XCURLOPT_HTTPGET: {
            const long v = va_arg(ap, long);
            if (v != 0L) easy.method = "GET";
            break;
        }
        case XCURLOPT_POSTFIELDS:
        case XCURLOPT_COPYPOSTFIELDS: {
            const char* data = va_arg(ap, const char*);
            if (data) {
                const size_t len = std::strlen(data);
                easy.post_body.assign(
                    reinterpret_cast<const uint8_t*>(data),
                    reinterpret_cast<const uint8_t*>(data) + len);
                if (easy.method == "GET") easy.method = "POST";
            } else {
                easy.post_body.clear();
            }
            break;
        }
        case XCURLOPT_POSTFIELDSIZE: {
            const long sz = va_arg(ap, long);
            if (sz >= 0L && static_cast<size_t>(sz) < easy.post_body.size()) {
                easy.post_body.resize(static_cast<size_t>(sz));
            }
            break;
        }
        case XCURLOPT_HTTPHEADER: {
            const XCurlSlist* list = va_arg(ap, const XCurlSlist*);
            easy.headers.clear();
            while (list) {
                if (list->data) {
                    easy.headers.emplace_back(list->data);
                }
                list = list->next;
            }
            break;
        }
        case XCURLOPT_WRITEFUNCTION: {
            easy.write_cb = va_arg(ap, XCurlWriteCallback);
            break;
        }
        case XCURLOPT_WRITEDATA: {
            easy.write_data = va_arg(ap, void*);
            break;
        }
        case XCURLOPT_HEADERFUNCTION: {
            easy.header_cb = va_arg(ap, XCurlWriteCallback);
            break;
        }
        case XCURLOPT_HEADERDATA: {
            easy.header_data = va_arg(ap, void*);
            break;
        }
        case XCURLOPT_PRIVATE: {
            easy.private_ptr = va_arg(ap, void*);
            break;
        }
        case XCURLOPT_TIMEOUT: {
            const long secs = va_arg(ap, long);
            easy.timeout_ms = secs * 1000L;
            break;
        }
        case XCURLOPT_TIMEOUT_MS: {
            easy.timeout_ms = va_arg(ap, long);
            break;
        }
        case XCURLOPT_CONNECTTIMEOUT_MS: {
            easy.connect_timeout_ms = va_arg(ap, long);
            break;
        }
        case XCURLOPT_FOLLOWLOCATION: {
            easy.follow_location = (va_arg(ap, long) != 0L);
            break;
        }
        case XCURLOPT_NOBODY: {
            easy.nobody = (va_arg(ap, long) != 0L);
            break;
        }
        case XCURLOPT_SSL_VERIFYPEER: {
            easy.verify_peer = (va_arg(ap, long) != 0L);
            break;
        }
        case XCURLOPT_SSL_VERIFYHOST: {
            easy.verify_host = (va_arg(ap, long) != 0L);
            break;
        }
        case XCURLOPT_VERBOSE: {
            easy.verbose = (va_arg(ap, long) != 0L);
            break;
        }
        case XCURLOPT_NOSIGNAL: {
            (void)va_arg(ap, long);
            break;
        }
        default: {
            res = XCURLE_UNKNOWN_OPTION;
            break;
        }
    }

    va_end(ap);
    return RecordCode("curl_easy_setopt", res);
}

int32_t OS_SHIM_CDECL curl_easy_perform(XCurlEasyHandle curl) {
    if (!curl) {
        return RecordCode("curl_easy_perform", XCURLE_BAD_FUNCTION_ARGUMENT);
    }
    EasyObject snapshot{};
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.easy_handles.find(curl);
        if (it == st.easy_handles.end()) {
            return RecordCode("curl_easy_perform", XCURLE_BAD_FUNCTION_ARGUMENT);
        }
        snapshot = *it->second;
    }

    const int32_t res = PerformOfflineTransfer(snapshot);

    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.easy_handles.find(curl);
        if (it != st.easy_handles.end()) {
            it->second->response_code = snapshot.response_code;
            it->second->header_size = snapshot.header_size;
            it->second->request_size = snapshot.request_size;
            it->second->total_time = snapshot.total_time;
            it->second->size_download = snapshot.size_download;
            it->second->effective_url = snapshot.effective_url;
            it->second->content_type = snapshot.content_type;
        }
    }

    return RecordCode("curl_easy_perform", res);
}

int32_t OS_SHIM_CDECL curl_easy_getinfo(XCurlEasyHandle curl, int32_t info, ...) {
    if (!curl) {
        return RecordCode("curl_easy_getinfo", XCURLE_BAD_FUNCTION_ARGUMENT);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.easy_handles.find(curl);
    if (it == st.easy_handles.end()) {
        return RecordCode("curl_easy_getinfo", XCURLE_BAD_FUNCTION_ARGUMENT);
    }
    const EasyObject& easy = *it->second;

    va_list ap;
    va_start(ap, info);
    int32_t res = XCURLE_OK;

    switch (info) {
        case XCURLINFO_RESPONSE_CODE: {
            long* out = va_arg(ap, long*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.response_code;
            break;
        }
        case XCURLINFO_HEADER_SIZE: {
            long* out = va_arg(ap, long*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.header_size;
            break;
        }
        case XCURLINFO_REQUEST_SIZE: {
            long* out = va_arg(ap, long*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.request_size;
            break;
        }
        case XCURLINFO_TOTAL_TIME: {
            double* out = va_arg(ap, double*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.total_time;
            break;
        }
        case XCURLINFO_SIZE_DOWNLOAD: {
            double* out = va_arg(ap, double*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.size_download;
            break;
        }
        case XCURLINFO_EFFECTIVE_URL: {
            const char** out = va_arg(ap, const char**);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.effective_url.c_str();
            break;
        }
        case XCURLINFO_CONTENT_TYPE: {
            const char** out = va_arg(ap, const char**);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.content_type.c_str();
            break;
        }
        case XCURLINFO_PRIVATE: {
            void** out = va_arg(ap, void**);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.private_ptr;
            break;
        }
        default: {
            res = XCURLE_UNKNOWN_OPTION;
            break;
        }
    }

    va_end(ap);
    return RecordCode("curl_easy_getinfo", res);
}

const char* OS_SHIM_CDECL curl_easy_strerror(int32_t code) {
    RecordOk("curl_easy_strerror");
    switch (code) {
        case XCURLE_OK: return "No error";
        case XCURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
        case XCURLE_FAILED_INIT: return "Failed initialization";
        case XCURLE_URL_MALFORMAT: return "URL using bad/illegal format or missing URL";
        case XCURLE_OUT_OF_MEMORY: return "Out of memory";
        case XCURLE_WRITE_ERROR: return "Failed writing received data to application";
        case XCURLE_BAD_FUNCTION_ARGUMENT: return "A libcurl function was given a bad argument";
        case XCURLE_UNKNOWN_OPTION: return "An unknown option was passed in to libcurl";
        default: return "Unknown XCurl error";
    }
}

XCurlSlist* OS_SHIM_CDECL curl_slist_append(XCurlSlist* list, const char* string) {
    RecordOk("curl_slist_append");
    if (!string) {
        return list;
    }
    const size_t len = std::strlen(string);
    auto* node = static_cast<XCurlSlist*>(std::malloc(sizeof(XCurlSlist)));
    if (!node) {
        return nullptr;
    }
    auto* copy = static_cast<char*>(std::malloc(len + 1u));
    if (!copy) {
        std::free(node);
        return nullptr;
    }
    std::memcpy(copy, string, len + 1u);
    node->data = copy;
    node->next = nullptr;

    if (!list) {
        return node;
    }
    XCurlSlist* tail = list;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = node;
    return list;
}

void OS_SHIM_CDECL curl_slist_free_all(XCurlSlist* list) {
    RecordOk("curl_slist_free_all");
    while (list) {
        XCurlSlist* next = list->next;
        std::free(list->data);
        std::free(list);
        list = next;
    }
}

XCurlMultiHandle OS_SHIM_CDECL curl_multi_init(void) {
    auto obj = std::make_unique<MultiObject>();
    void* raw = obj.get();
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.multi_handles[raw] = std::move(obj);
    }
    RecordOk("curl_multi_init");
    return raw;
}

int32_t OS_SHIM_CDECL curl_multi_cleanup(XCurlMultiHandle multi_handle) {
    if (!multi_handle) {
        return RecordCode("curl_multi_cleanup", XCURLM_BAD_HANDLE);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.multi_handles.find(multi_handle);
    if (it == st.multi_handles.end()) {
        return RecordCode("curl_multi_cleanup", XCURLM_BAD_HANDLE);
    }
    st.multi_handles.erase(it);
    return RecordCode("curl_multi_cleanup", XCURLM_OK);
}

int32_t OS_SHIM_CDECL curl_multi_add_handle(
    XCurlMultiHandle multi_handle,
    XCurlEasyHandle curl_handle) {
    if (!multi_handle) {
        return RecordCode("curl_multi_add_handle", XCURLM_BAD_HANDLE);
    }
    if (!curl_handle) {
        return RecordCode("curl_multi_add_handle", XCURLM_BAD_EASY_HANDLE);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto mit = st.multi_handles.find(multi_handle);
    if (mit == st.multi_handles.end()) {
        return RecordCode("curl_multi_add_handle", XCURLM_BAD_HANDLE);
    }
    if (st.easy_handles.find(curl_handle) == st.easy_handles.end()) {
        return RecordCode("curl_multi_add_handle", XCURLM_BAD_EASY_HANDLE);
    }
    auto& m = *mit->second;
    if (std::find(m.attached.begin(), m.attached.end(), curl_handle) != m.attached.end()) {
        return RecordCode("curl_multi_add_handle", XCURLM_ADDED_ALREADY);
    }
    m.attached.push_back(curl_handle);
    m.pending.push_back(curl_handle);
    return RecordCode("curl_multi_add_handle", XCURLM_OK);
}

int32_t OS_SHIM_CDECL curl_multi_remove_handle(
    XCurlMultiHandle multi_handle,
    XCurlEasyHandle curl_handle) {
    if (!multi_handle) {
        return RecordCode("curl_multi_remove_handle", XCURLM_BAD_HANDLE);
    }
    if (!curl_handle) {
        return RecordCode("curl_multi_remove_handle", XCURLM_BAD_EASY_HANDLE);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto mit = st.multi_handles.find(multi_handle);
    if (mit == st.multi_handles.end()) {
        return RecordCode("curl_multi_remove_handle", XCURLM_BAD_HANDLE);
    }
    auto& m = *mit->second;
    m.attached.erase(std::remove(m.attached.begin(), m.attached.end(), curl_handle), m.attached.end());
    m.pending.erase(std::remove(m.pending.begin(), m.pending.end(), curl_handle), m.pending.end());
    return RecordCode("curl_multi_remove_handle", XCURLM_OK);
}

int32_t OS_SHIM_CDECL curl_multi_perform(
    XCurlMultiHandle multi_handle,
    int32_t* running_handles) {
    if (!multi_handle || !running_handles) {
        return RecordCode("curl_multi_perform", XCURLM_BAD_HANDLE);
    }
    std::vector<XCurlEasyHandle> to_run;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto mit = st.multi_handles.find(multi_handle);
        if (mit == st.multi_handles.end()) {
            return RecordCode("curl_multi_perform", XCURLM_BAD_HANDLE);
        }
        to_run.swap(mit->second->pending);
    }

    std::vector<XCurlMsg> done_msgs;
    done_msgs.reserve(to_run.size());
    for (XCurlEasyHandle easy : to_run) {
        const int32_t res = curl_easy_perform(easy);
        XCurlMsg msg{};
        msg.msg = XCURLMSG_DONE;
        msg.easy_handle = easy;
        msg.data.result = res;
        done_msgs.push_back(msg);
    }

    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto mit = st.multi_handles.find(multi_handle);
        if (mit != st.multi_handles.end()) {
            for (const auto& msg : done_msgs) {
                mit->second->completed_msgs.push_back(msg);
            }
        }
    }

    *running_handles = 0;
    return RecordCode("curl_multi_perform", XCURLM_OK);
}

XCurlMsg* OS_SHIM_CDECL curl_multi_info_read(
    XCurlMultiHandle multi_handle,
    int32_t* msgs_in_queue) {
    if (msgs_in_queue) {
        *msgs_in_queue = 0;
    }
    if (!multi_handle) {
        RecordCode("curl_multi_info_read", XCURLM_BAD_HANDLE);
        return nullptr;
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto mit = st.multi_handles.find(multi_handle);
    if (mit == st.multi_handles.end()) {
        RecordCode("curl_multi_info_read", XCURLM_BAD_HANDLE);
        return nullptr;
    }
    auto& m = *mit->second;
    if (m.completed_msgs.empty()) {
        RecordOk("curl_multi_info_read");
        return nullptr;
    }
    m.current_msg = m.completed_msgs.front();
    m.completed_msgs.pop_front();
    if (msgs_in_queue) {
        *msgs_in_queue = static_cast<int32_t>(m.completed_msgs.size());
    }
    RecordOk("curl_multi_info_read");
    return &m.current_msg;
}

}  // extern "C"
