#define OPENSCARLETT_BUILDING_XCURL 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct XCurlU {
    std::string scheme;
    std::string user;
    std::string password;
    std::string options;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
    std::string fragment;
    std::string zoneid;
};

namespace {

constexpr const char* kModName = "XCurl.dll";
constexpr const char* kVersionStr = "XCurl/8.0.0-OpenScarlett (offline-loopback)";
constexpr const char* kHostStr = "x86_64-pc-win32-scarlett";
constexpr const char* kSslVersionStr = "OpenScarlett-SChannel/1.0";
constexpr const char* kZlibVersionStr = "1.3.0";
constexpr const char* kProtocols[] = {"http", "https", nullptr};

struct EasyObject {
    std::string url;
    std::string method = "GET";
    std::string user_agent = "OpenScarlett-XCurl/1.0";
    std::string accept_encoding;
    std::string proxy;
    std::string cainfo;
    std::string capath;
    std::vector<uint8_t> post_body;
    std::vector<std::string> headers;

    XCurlWriteCallback write_cb = nullptr;
    void* write_data = nullptr;
    XCurlWriteCallback header_cb = nullptr;
    void* header_data = nullptr;
    void* read_cb = nullptr;
    void* read_data = nullptr;
    void* private_ptr = nullptr;
    char* error_buffer = nullptr;

    long timeout_ms = 30000L;
    long connect_timeout_ms = 10000L;
    long max_redirs = 50L;
    long http_version = 0L;
    bool follow_location = true;
    bool nobody = false;
    bool verify_peer = true;
    bool verify_host = true;
    bool verbose = false;
    bool fail_on_error = false;

    // Last transfer results
    long response_code = 0L;
    long header_size = 0L;
    long request_size = 0L;
    double total_time = 0.0;
    double size_download = 0.0;
    int64_t size_upload_t = 0;
    std::string effective_url;
    std::string effective_scheme = "https";
    std::string content_type = "application/json";

    void Reset() {
        url.clear();
        method = "GET";
        user_agent = "OpenScarlett-XCurl/1.0";
        accept_encoding.clear();
        proxy.clear();
        cainfo.clear();
        capath.clear();
        post_body.clear();
        headers.clear();
        write_cb = nullptr;
        write_data = nullptr;
        header_cb = nullptr;
        header_data = nullptr;
        read_cb = nullptr;
        read_data = nullptr;
        private_ptr = nullptr;
        error_buffer = nullptr;
        timeout_ms = 30000L;
        connect_timeout_ms = 10000L;
        max_redirs = 50L;
        http_version = 0L;
        follow_location = true;
        nobody = false;
        verify_peer = true;
        verify_host = true;
        verbose = false;
        fail_on_error = false;
        response_code = 0L;
        header_size = 0L;
        request_size = 0L;
        total_time = 0.0;
        size_download = 0.0;
        size_upload_t = 0;
        effective_url.clear();
        effective_scheme = "https";
        content_type = "application/json";
    }
};

struct MultiObject {
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    bool wakeup_pending = false;
    bool closing = false;
    long max_connects = 0L;
    long max_host_connections = 0L;
    long max_total_connections = 0L;
    long pipelining = 0L;

    std::vector<XCurlEasyHandle> attached;
    std::vector<XCurlEasyHandle> pending;
    std::deque<XCurlMsg> completed_msgs;
    XCurlMsg current_msg{};
};

struct ShareObject {
    long share_mask = 0L;
    void* lock_fn = nullptr;
    void* unlock_fn = nullptr;
    void* user_data = nullptr;
};

struct XCurlState {
    std::mutex mutex;
    uint32_t global_init_count = 0;
    bool suspended = false;
    uint32_t request_limit = 0;

    XCurlMallocCallback mem_malloc = nullptr;
    XCurlFreeCallback mem_free = nullptr;
    XCurlReallocCallback mem_realloc = nullptr;
    XCurlStrdupCallback mem_strdup = nullptr;
    XCurlCallocCallback mem_calloc = nullptr;

    std::unordered_map<void*, std::unique_ptr<EasyObject>> easy_handles;
    std::unordered_map<void*, std::shared_ptr<MultiObject>> multi_handles;
    std::unordered_map<void*, std::unique_ptr<ShareObject>> share_handles;
    std::unordered_map<XCurlU*, std::unique_ptr<XCurlU>> url_handles;
};

XCurlState& GetXCurl() {
    static XCurlState* s = new XCurlState();
    return *s;
}

void* CurlAlloc(size_t size) {
    XCurlMallocCallback fn = nullptr;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        fn = st.mem_malloc;
    }
    return fn ? fn(size) : std::malloc(size);
}

void CurlDealloc(void* ptr) {
    if (!ptr) return;
    XCurlFreeCallback fn = nullptr;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        fn = st.mem_free;
    }
    if (fn) {
        fn(ptr);
    } else {
        std::free(ptr);
    }
}

char* CurlAllocCopyString(const std::string& str) {
    auto* out = static_cast<char*>(CurlAlloc(str.size() + 1u));
    if (!out) return nullptr;
    if (!str.empty()) {
        std::memcpy(out, str.data(), str.size());
    }
    out[str.size()] = '\0';
    return out;
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

bool IsUnreservedUrlChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

std::string PercentEncode(const char* data, size_t len) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2u);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (IsUnreservedUrlChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0Fu]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    return out;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool PercentDecode(const char* data, size_t len, std::string& out, bool strict) {
    out.clear();
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '%' && i + 2u < len) {
            const int hi = HexVal(data[i + 1u]);
            const int lo = HexVal(data[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                const unsigned char byte = static_cast<unsigned char>((hi << 4) | lo);
                if (strict && byte == 0u) {
                    return false;
                }
                out.push_back(static_cast<char>(byte));
                i += 2u;
                continue;
            }
            if (strict) return false;
        }
        out.push_back(data[i]);
    }
    return true;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

int32_t ParseFullUrl(const std::string& input, uint32_t flags, XCurlU& out) {
    if (input.empty()) {
        return XCURLUE_MALFORMED_INPUT;
    }

    XCurlU parsed{};
    size_t pos = 0;

    // 1. Scheme
    const size_t scheme_sep = input.find("://");
    if (scheme_sep != std::string::npos) {
        parsed.scheme = ToLowerAscii(input.substr(0, scheme_sep));
        if (parsed.scheme.empty()) {
            return XCURLUE_MALFORMED_INPUT;
        }
        if ((flags & XCURLU_NON_SUPPORT_SCHEME) == 0u &&
            parsed.scheme != "http" && parsed.scheme != "https") {
            return XCURLUE_UNSUPPORTED_SCHEME;
        }
        pos = scheme_sep + 3u;
    } else if ((flags & (XCURLU_DEFAULT_SCHEME | XCURLU_GUESS_SCHEME)) != 0u) {
        parsed.scheme = "https";
    } else {
        return XCURLUE_MALFORMED_INPUT;
    }

    // 2. Fragment
    size_t end = input.size();
    const size_t hash_pos = input.find('#', pos);
    if (hash_pos != std::string::npos) {
        parsed.fragment = input.substr(hash_pos + 1u);
        end = hash_pos;
    }

    // 3. Query
    const size_t query_pos = input.find('?', pos);
    if (query_pos != std::string::npos && query_pos < end) {
        parsed.query = input.substr(query_pos + 1u, end - (query_pos + 1u));
        end = query_pos;
    }

    // 4. Path
    const size_t slash_pos = input.find('/', pos);
    size_t auth_end = end;
    if (slash_pos != std::string::npos && slash_pos < end) {
        parsed.path = input.substr(slash_pos, end - slash_pos);
        auth_end = slash_pos;
    } else {
        parsed.path = "/";
    }

    // 5. Authority: [user[:password]@]host[:port]
    const std::string authority = input.substr(pos, auth_end - pos);
    if (authority.empty()) {
        return XCURLUE_MALFORMED_INPUT;
    }

    size_t host_start = 0;
    const size_t at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        if ((flags & XCURLU_DISALLOW_USER) != 0u) {
            return XCURLUE_USER_NOT_ALLOWED;
        }
        const std::string userinfo = authority.substr(0, at_pos);
        const size_t colon_pos = userinfo.find(':');
        if (colon_pos != std::string::npos) {
            parsed.user = userinfo.substr(0, colon_pos);
            parsed.password = userinfo.substr(colon_pos + 1u);
        } else {
            parsed.user = userinfo;
        }
        host_start = at_pos + 1u;
    }

    const std::string hostport = authority.substr(host_start);
    if (hostport.empty()) {
        return XCURLUE_MALFORMED_INPUT;
    }

    if (hostport.front() == '[') {
        const size_t close_b = hostport.find(']');
        if (close_b == std::string::npos) {
            return XCURLUE_MALFORMED_INPUT;
        }
        parsed.host = hostport.substr(0, close_b + 1u);
        if (close_b + 1u < hostport.size()) {
            if (hostport[close_b + 1u] != ':') {
                return XCURLUE_MALFORMED_INPUT;
            }
            parsed.port = hostport.substr(close_b + 2u);
        }
    } else {
        const size_t colon_pos = hostport.rfind(':');
        if (colon_pos != std::string::npos) {
            parsed.host = hostport.substr(0, colon_pos);
            parsed.port = hostport.substr(colon_pos + 1u);
        } else {
            parsed.host = hostport;
        }
    }

    if (parsed.host.empty()) {
        return XCURLUE_MALFORMED_INPUT;
    }

    if (!parsed.port.empty()) {
        unsigned long port_num = 0;
        for (char c : parsed.port) {
            if (c < '0' || c > '9') {
                return XCURLUE_BAD_PORT_NUMBER;
            }
            port_num = port_num * 10ul + static_cast<unsigned long>(c - '0');
            if (port_num > 65535ul) {
                return XCURLUE_BAD_PORT_NUMBER;
            }
        }
        if (port_num == 0ul) {
            return XCURLUE_BAD_PORT_NUMBER;
        }
    }

    out = std::move(parsed);
    return XCURLUE_OK;
}

std::string DefaultPortForScheme(const std::string& scheme) {
    if (scheme == "http") return "80";
    if (scheme == "https") return "443";
    return {};
}

std::string ComposeFullUrl(const XCurlU& u, uint32_t flags) {
    std::string out;
    if (!u.scheme.empty()) {
        out += u.scheme;
        out += "://";
    }
    if (!u.user.empty()) {
        out += u.user;
        if (!u.password.empty()) {
            out.push_back(':');
            out += u.password;
        }
        out.push_back('@');
    }
    out += u.host;

    const std::string def_port = DefaultPortForScheme(u.scheme);
    if (!u.port.empty()) {
        const bool suppress_default = ((flags & XCURLU_NO_DEFAULT_PORT) != 0u) && (u.port == def_port);
        if (!suppress_default) {
            out.push_back(':');
            out += u.port;
        }
    } else if ((flags & XCURLU_DEFAULT_PORT) != 0u && !def_port.empty()) {
        out.push_back(':');
        out += def_port;
    }

    if (u.path.empty()) {
        out.push_back('/');
    } else {
        if (u.path.front() != '/') out.push_back('/');
        out += u.path;
    }
    if (!u.query.empty()) {
        out.push_back('?');
        out += u.query;
    }
    if (!u.fragment.empty()) {
        out.push_back('#');
        out += u.fragment;
    }
    return out;
}

std::string Base64UrlEncode(const std::string& input) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((input.size() + 2u) / 3u) * 4u);
    size_t i = 0;
    while (i + 3u <= input.size()) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(input[i + 1u])) << 8) |
                           static_cast<uint32_t>(static_cast<unsigned char>(input[i + 2u]));
        out.push_back(kTable[(v >> 18) & 0x3Fu]);
        out.push_back(kTable[(v >> 12) & 0x3Fu]);
        out.push_back(kTable[(v >> 6) & 0x3Fu]);
        out.push_back(kTable[v & 0x3Fu]);
        i += 3u;
    }
    const size_t rem = input.size() - i;
    if (rem == 1u) {
        const uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16;
        out.push_back(kTable[(v >> 18) & 0x3Fu]);
        out.push_back(kTable[(v >> 12) & 0x3Fu]);
    } else if (rem == 2u) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(input[i + 1u])) << 8);
        out.push_back(kTable[(v >> 18) & 0x3Fu]);
        out.push_back(kTable[(v >> 12) & 0x3Fu]);
        out.push_back(kTable[(v >> 6) & 0x3Fu]);
    }
    return out;
}

bool IsOfflineModeEnvEnabled() {
#if defined(_MSC_VER)
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "OPENSCARLETT_OFFLINE") != 0 || !buf) {
        return false;
    }
    const std::string val(buf);
    std::free(buf);
    return val == "1" || val == "true" || val == "TRUE";
#else
    const char* v = std::getenv("OPENSCARLETT_OFFLINE");
    if (!v) return false;
    const std::string val(v);
    return val == "1" || val == "true" || val == "TRUE";
#endif
}

std::string ExtractUrlHostLower(const std::string& url) {
    XCurlU parsed{};
    if (ParseFullUrl(url, XCURLU_NON_SUPPORT_SCHEME, parsed) != XCURLUE_OK) {
        return {};
    }
    return ToLowerAscii(parsed.host);
}

bool IsXboxLiveAuthHost(const std::string& host_lower) {
    return host_lower == "xsts.auth.xboxlive.com" ||
           host_lower == "user.auth.xboxlive.com" ||
           host_lower == "device.auth.xboxlive.com" ||
           host_lower == "title.auth.xboxlive.com" ||
           host_lower == "sisu.xboxlive.com";
}

bool IsXboxLiveTitleMgtHost(const std::string& host_lower) {
    return host_lower == "title.mgt.xboxlive.com";
}

std::string BuildOfflineXstsJwt() {
    const std::string hdr = "{\"alg\":\"ES256\",\"typ\":\"JWT\"}";
    const std::string body =
        "{\"xuid\":\"2533274790396727\",\"uhs\":\"0000000000000001\","
        "\"gtg\":\"ScarlettPlayer\",\"agg\":\"Adult\","
        "\"iss\":\"https://xsts.auth.xboxlive.com\",\"offline\":true}";
    const std::string sig = "OpenScarlett-XCurl-Offline-XSTS-Sig-V1";
    return Base64UrlEncode(hdr) + "." + Base64UrlEncode(body) + "." + Base64UrlEncode(sig);
}

int32_t PerformOfflineTransfer(EasyObject& easy) {
    if (easy.url.empty()) {
        return XCURLE_URL_MALFORMAT;
    }
    if (!HasSupportedScheme(easy.url)) {
        return XCURLE_UNSUPPORTED_PROTOCOL;
    }

    const std::string host_lower = ExtractUrlHostLower(easy.url);
    const bool is_xsts_auth = IsXboxLiveAuthHost(host_lower);
    const bool is_title_mgt = IsXboxLiveTitleMgtHost(host_lower);
    const bool offline_forced = IsOfflineModeEnvEnabled();

    easy.effective_url = easy.url;
    easy.effective_scheme = (easy.url.rfind("http://", 0) == 0) ? "http" : "https";
    easy.response_code = 200L;
    easy.content_type = "application/json";
    easy.total_time = 0.001;
    easy.size_upload_t = static_cast<int64_t>(easy.post_body.size());
    easy.request_size = static_cast<long>(easy.url.size() + easy.method.size() + easy.post_body.size());

    std::string payload;
    if (!easy.nobody) {
        std::ostringstream oss;
        if (is_xsts_auth) {
            const std::string jwt = BuildOfflineXstsJwt();
            oss << "{\"IssueInstant\":\"2026-01-01T00:00:00.0000000Z\","
                << "\"NotAfter\":\"2099-01-01T00:00:00.0000000Z\","
                << "\"Token\":\"" << jwt << "\","
                << "\"DisplayClaims\":{\"xui\":[{\"uhs\":\"0000000000000001\","
                << "\"xid\":\"2533274790396727\",\"gtg\":\"ScarlettPlayer\","
                << "\"agg\":\"Adult\",\"usr\":\"191\",\"prv\":\"191\"}]},"
                << "\"service\":\"openscarlett-xcurl\",\"offline\":true,\"status\":200,"
                << "\"method\":\"" << EscapeJson(easy.method) << "\","
                << "\"url\":\"" << EscapeJson(easy.url) << "\","
                << "\"postBytes\":" << easy.post_body.size() << "}";
        } else if (is_title_mgt) {
            oss << "{\"titleId\":\"5CA00001\","
                << "\"scid\":\"00000000-0000-0000-0000-00005ca00001\","
                << "\"service\":\"openscarlett-xcurl\",\"offline\":true,\"status\":200,"
                << "\"method\":\"" << EscapeJson(easy.method) << "\","
                << "\"url\":\"" << EscapeJson(easy.url) << "\","
                << "\"postBytes\":" << easy.post_body.size() << "}";
        } else {
            oss << "{\"service\":\"openscarlett-xcurl\",\"status\":200";
            if (offline_forced) {
                oss << ",\"offline\":true";
            }
            oss << ",\"method\":\"" << EscapeJson(easy.method) << "\",\"url\":\""
                << EscapeJson(easy.url) << "\",\"postBytes\":"
                << easy.post_body.size() << "}";
        }
        payload = oss.str();
    }

    const std::string hdrs =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Server: OpenScarlett-XCurl/1.0\r\n"
        "X-OpenScarlett-Offline: 1\r\n"
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

int32_t MultiWaitImpl(
    const char* sym,
    XCurlMultiHandle multi_handle,
    XCurlWaitFd extra_fds[],
    uint32_t extra_nfds,
    int32_t timeout_ms,
    int32_t* numfds,
    bool is_poll) {
    if (numfds) {
        *numfds = 0;
    }
    if (!multi_handle) {
        return RecordCode(sym, XCURLM_BAD_HANDLE);
    }
    if (timeout_ms < 0) {
        return RecordCode(sym, XCURLM_BAD_FUNCTION_ARGUMENT);
    }
    if (extra_nfds > 0u && !extra_fds) {
        return RecordCode(sym, XCURLM_BAD_FUNCTION_ARGUMENT);
    }
    for (uint32_t i = 0; i < extra_nfds; ++i) {
        extra_fds[i].revents = 0;
    }

    std::shared_ptr<MultiObject> multi;
    int32_t ready_count = 0;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto mit = st.multi_handles.find(multi_handle);
        if (mit == st.multi_handles.end()) {
            return RecordCode(sym, XCURLM_BAD_HANDLE);
        }
        multi = mit->second;
        ready_count = static_cast<int32_t>(multi->pending.size() + multi->completed_msgs.size());
    }

    if (ready_count > 0) {
        if (numfds) {
            *numfds = ready_count;
        }
        return RecordCode(sym, XCURLM_OK);
    }

    if (!is_poll && extra_nfds == 0u) {
        // Per libcurl spec: curl_multi_wait returns immediately with numfds=0
        // when there are no active descriptors to wait on.
        return RecordCode(sym, XCURLM_OK);
    }

    if (timeout_ms == 0) {
        std::lock_guard<std::mutex> wlk(multi->wait_mutex);
        multi->wakeup_pending = false;
        return RecordCode(sym, XCURLM_OK);
    }

    // Cap single offline wait slice to 50ms so idle background loops never hang
    // shutdown while still yielding CPU and waking immediately on curl_multi_wakeup
    // or curl_multi_add_handle.
    const int32_t bounded_ms = std::min<int32_t>(timeout_ms, 50);
    {
        std::unique_lock<std::mutex> wlk(multi->wait_mutex);
        if (multi->wakeup_pending || multi->closing) {
            multi->wakeup_pending = false;
        } else {
            multi->wait_cv.wait_for(
                wlk,
                std::chrono::milliseconds(bounded_ms),
                [&]() { return multi->wakeup_pending || multi->closing; });
            multi->wakeup_pending = false;
        }
    }

    // Re-check if a handle became pending while waiting
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto mit = st.multi_handles.find(multi_handle);
        if (mit != st.multi_handles.end()) {
            ready_count = static_cast<int32_t>(
                mit->second->pending.size() + mit->second->completed_msgs.size());
        }
    }
    if (numfds) {
        *numfds = ready_count;
    }
    return RecordCode(sym, XCURLM_OK);
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CDECL curl_global_init(long /*flags*/) {
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.global_init_count;
    return RecordCode("curl_global_init", XCURLE_OK);
}

int32_t OS_SHIM_CDECL curl_global_init_mem(
    long /*flags*/,
    XCurlMallocCallback m,
    XCurlFreeCallback f,
    XCurlReallocCallback r,
    XCurlStrdupCallback s,
    XCurlCallocCallback c) {
    if (!m || !f || !r || !s || !c) {
        return RecordCode("curl_global_init_mem", XCURLE_FAILED_INIT);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.mem_malloc = m;
    st.mem_free = f;
    st.mem_realloc = r;
    st.mem_strdup = s;
    st.mem_calloc = c;
    ++st.global_init_count;
    return RecordCode("curl_global_init_mem", XCURLE_OK);
}

void OS_SHIM_CDECL curl_global_cleanup(void) {
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.global_init_count > 0u) {
        --st.global_init_count;
    }
    if (st.global_init_count == 0u) {
        st.mem_malloc = nullptr;
        st.mem_free = nullptr;
        st.mem_realloc = nullptr;
        st.mem_strdup = nullptr;
        st.mem_calloc = nullptr;
    }
    RecordOk("curl_global_cleanup");
}

int32_t OS_SHIM_CDECL curl_global_sslset(
    int32_t /*id*/,
    const char* /*name*/,
    const void*** avail) {
    if (avail) {
        *avail = nullptr;
    }
    return RecordCode("curl_global_sslset", XCURLSSLSET_OK);
}

const char* OS_SHIM_CDECL curl_version(void) {
    RecordOk("curl_version");
    return kVersionStr;
}

XCurlVersionInfoData* OS_SHIM_CDECL curl_version_info(int32_t /*age*/) {
    static XCurlVersionInfoData info = {
        XCURLVERSION_NOW,
        "8.0.0",
        0x080000u,
        kHostStr,
        0x00000004 | 0x00000080 | 0x00000200,  // SSL | ASYNCHDNS | LARGEFILE
        kSslVersionStr,
        0L,
        kZlibVersionStr,
        kProtocols,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0u,
        nullptr,
        0u,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
    };
    RecordOk("curl_version_info");
    return &info;
}

int64_t OS_SHIM_CDECL curl_getdate(const char* p, const int64_t* /*unused*/) {
    RecordOk("curl_getdate");
    if (!p || !*p) return -1;
    // Basic ISO-8601 / RFC-style YYYY-MM-DD[T| ]HH:MM:SS parsing
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
#if defined(_MSC_VER)
    const int parsed =
        (sscanf_s(p, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) >= 3)
            ? 3
            : sscanf_s(p, "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
#else
    const int parsed =
        (std::sscanf(p, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) >= 3)
            ? 3
            : std::sscanf(p, "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
#endif
    if (parsed >= 3) {
        std::tm tm_val{};
        tm_val.tm_year = year - 1900;
        tm_val.tm_mon = mon - 1;
        tm_val.tm_mday = day;
        tm_val.tm_hour = hour;
        tm_val.tm_min = min;
        tm_val.tm_sec = sec;
#if defined(_MSC_VER)
        return static_cast<int64_t>(_mkgmtime(&tm_val));
#else
        return static_cast<int64_t>(std::mktime(&tm_val));
#endif
    }
    return -1;
}

void OS_SHIM_CDECL curl_free(void* p) {
    RecordOk("curl_free");
    CurlDealloc(p);
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

XCurlEasyHandle OS_SHIM_CDECL curl_easy_duphandle(XCurlEasyHandle curl) {
    if (!curl) {
        RecordCode("curl_easy_duphandle", XCURLE_BAD_FUNCTION_ARGUMENT);
        return nullptr;
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.easy_handles.find(curl);
    if (it == st.easy_handles.end()) {
        RecordCode("curl_easy_duphandle", XCURLE_BAD_FUNCTION_ARGUMENT);
        return nullptr;
    }
    auto copy = std::make_unique<EasyObject>(*it->second);
    void* raw = copy.get();
    st.easy_handles[raw] = std::move(copy);
    RecordOk("curl_easy_duphandle");
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
        case XCURLOPT_PUT:
        case XCURLOPT_UPLOAD: {
            const long v = va_arg(ap, long);
            if (v != 0L) easy.method = "PUT";
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
        case XCURLOPT_POSTFIELDSIZE_LARGE:
        case XCURLOPT_INFILESIZE_LARGE: {
            const int64_t sz = va_arg(ap, int64_t);
            if (option == XCURLOPT_POSTFIELDSIZE_LARGE &&
                sz >= 0 && static_cast<size_t>(sz) < easy.post_body.size()) {
                easy.post_body.resize(static_cast<size_t>(sz));
            }
            break;
        }
        case XCURLOPT_INFILESIZE: {
            (void)va_arg(ap, long);
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
        case XCURLOPT_READFUNCTION: {
            easy.read_cb = va_arg(ap, void*);
            break;
        }
        case XCURLOPT_READDATA: {
            easy.read_data = va_arg(ap, void*);
            break;
        }
        case XCURLOPT_ERRORBUFFER: {
            easy.error_buffer = va_arg(ap, char*);
            if (easy.error_buffer) easy.error_buffer[0] = '\0';
            break;
        }
        case XCURLOPT_PRIVATE: {
            easy.private_ptr = va_arg(ap, void*);
            break;
        }
        case XCURLOPT_ACCEPT_ENCODING: {
            const char* s = va_arg(ap, const char*);
            easy.accept_encoding = s ? s : "";
            break;
        }
        case XCURLOPT_PROXY: {
            const char* s = va_arg(ap, const char*);
            easy.proxy = s ? s : "";
            break;
        }
        case XCURLOPT_CAINFO: {
            const char* s = va_arg(ap, const char*);
            easy.cainfo = s ? s : "";
            break;
        }
        case XCURLOPT_CAPATH: {
            const char* s = va_arg(ap, const char*);
            easy.capath = s ? s : "";
            break;
        }
        case XCURLOPT_TIMEOUT: {
            const long secs = va_arg(ap, long);
            easy.timeout_ms = secs * 1000L;
            break;
        }
        case XCURLOPT_CONNECTTIMEOUT: {
            const long secs = va_arg(ap, long);
            easy.connect_timeout_ms = secs * 1000L;
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
        case XCURLOPT_MAXREDIRS: {
            easy.max_redirs = va_arg(ap, long);
            break;
        }
        case XCURLOPT_NOBODY: {
            easy.nobody = (va_arg(ap, long) != 0L);
            break;
        }
        case XCURLOPT_FAILONERROR: {
            easy.fail_on_error = (va_arg(ap, long) != 0L);
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
        case XCURLOPT_HTTP_VERSION: {
            easy.http_version = va_arg(ap, long);
            break;
        }
        case XCURLOPT_VERBOSE: {
            easy.verbose = (va_arg(ap, long) != 0L);
            break;
        }
        case XCURLOPT_NOSIGNAL:
        case XCURLOPT_NOPROGRESS:
        case XCURLOPT_LOW_SPEED_LIMIT:
        case XCURLOPT_LOW_SPEED_TIME:
        case XCURLOPT_PROXYPORT:
        case XCURLOPT_FRESH_CONNECT:
        case XCURLOPT_FORBID_REUSE:
        case XCURLOPT_DNS_CACHE_TIMEOUT:
        case XCURLOPT_BUFFERSIZE:
        case XCURLOPT_TCP_KEEPALIVE:
        case XCURLOPT_PIPEWAIT: {
            (void)va_arg(ap, long);
            break;
        }
        case XCURLOPT_SHARE:
        case XCURLOPT_XFERINFODATA:
        case XCURLOPT_DEBUGDATA:
        case XCURLOPT_SSL_CTX_DATA:
        case XCURLOPT_PROGRESSFUNCTION:
        case XCURLOPT_DEBUGFUNCTION:
        case XCURLOPT_SSL_CTX_FUNCTION:
        case XCURLOPT_XFERINFOFUNCTION: {
            (void)va_arg(ap, void*);
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
            it->second->size_upload_t = snapshot.size_upload_t;
            it->second->effective_url = snapshot.effective_url;
            it->second->effective_scheme = snapshot.effective_scheme;
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
        case XCURLINFO_REDIRECT_COUNT:
        case XCURLINFO_HTTP_CONNECTCODE:
        case XCURLINFO_OS_ERRNO: {
            long* out = va_arg(ap, long*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = 0L;
            break;
        }
        case XCURLINFO_NUM_CONNECTS: {
            long* out = va_arg(ap, long*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = (easy.response_code != 0L) ? 1L : 0L;
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
        case XCURLINFO_SIZE_UPLOAD_T: {
            int64_t* out = va_arg(ap, int64_t*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.size_upload_t;
            break;
        }
        case XCURLINFO_SIZE_DOWNLOAD_T:
        case XCURLINFO_CONTENT_LENGTH_DOWNLOAD_T: {
            int64_t* out = va_arg(ap, int64_t*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = static_cast<int64_t>(easy.size_download);
            break;
        }
        case XCURLINFO_SPEED_DOWNLOAD_T: {
            int64_t* out = va_arg(ap, int64_t*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = static_cast<int64_t>(easy.size_download * 1000.0);
            break;
        }
        case XCURLINFO_TOTAL_TIME_T: {
            int64_t* out = va_arg(ap, int64_t*);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = static_cast<int64_t>(easy.total_time * 1000000.0);
            break;
        }
        case XCURLINFO_EFFECTIVE_URL: {
            const char** out = va_arg(ap, const char**);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.effective_url.c_str();
            break;
        }
        case XCURLINFO_SCHEME: {
            const char** out = va_arg(ap, const char**);
            if (!out) res = XCURLE_BAD_FUNCTION_ARGUMENT;
            else *out = easy.effective_scheme.c_str();
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

char* OS_SHIM_CDECL curl_easy_escape(
    XCurlEasyHandle /*curl*/,
    const char* string,
    int32_t length) {
    RecordOk("curl_easy_escape");
    if (!string || length < 0) {
        return nullptr;
    }
    const size_t len = (length == 0) ? std::strlen(string) : static_cast<size_t>(length);
    return CurlAllocCopyString(PercentEncode(string, len));
}

char* OS_SHIM_CDECL curl_easy_unescape(
    XCurlEasyHandle /*curl*/,
    const char* string,
    int32_t length,
    int32_t* outlength) {
    RecordOk("curl_easy_unescape");
    if (outlength) *outlength = 0;
    if (!string || length < 0) {
        return nullptr;
    }
    const size_t len = (length == 0) ? std::strlen(string) : static_cast<size_t>(length);
    std::string decoded;
    PercentDecode(string, len, decoded, false);
    if (outlength) {
        *outlength = static_cast<int32_t>(decoded.size());
    }
    return CurlAllocCopyString(decoded);
}

char* OS_SHIM_CDECL curl_escape(const char* string, int32_t length) {
    RecordOk("curl_escape");
    return curl_easy_escape(nullptr, string, length);
}

char* OS_SHIM_CDECL curl_unescape(const char* string, int32_t length) {
    RecordOk("curl_unescape");
    return curl_easy_unescape(nullptr, string, length, nullptr);
}

XCurlSlist* OS_SHIM_CDECL curl_slist_append(XCurlSlist* list, const char* string) {
    RecordOk("curl_slist_append");
    if (!string) {
        return list;
    }
    const size_t len = std::strlen(string);
    auto* node = static_cast<XCurlSlist*>(CurlAlloc(sizeof(XCurlSlist)));
    if (!node) {
        return nullptr;
    }
    auto* copy = static_cast<char*>(CurlAlloc(len + 1u));
    if (!copy) {
        CurlDealloc(node);
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
        CurlDealloc(list->data);
        CurlDealloc(list);
        list = next;
    }
}

XCurlMultiHandle OS_SHIM_CDECL curl_multi_init(void) {
    auto obj = std::make_shared<MultiObject>();
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
    std::shared_ptr<MultiObject> dropped;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.multi_handles.find(multi_handle);
        if (it == st.multi_handles.end()) {
            return RecordCode("curl_multi_cleanup", XCURLM_BAD_HANDLE);
        }
        dropped = std::move(it->second);
        st.multi_handles.erase(it);
    }
    if (dropped) {
        {
            std::lock_guard<std::mutex> wlk(dropped->wait_mutex);
            dropped->closing = true;
            dropped->wakeup_pending = true;
        }
        dropped->wait_cv.notify_all();
    }
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
    std::shared_ptr<MultiObject> multi;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto mit = st.multi_handles.find(multi_handle);
        if (mit == st.multi_handles.end()) {
            return RecordCode("curl_multi_add_handle", XCURLM_BAD_HANDLE);
        }
        if (st.easy_handles.find(curl_handle) == st.easy_handles.end()) {
            return RecordCode("curl_multi_add_handle", XCURLM_BAD_EASY_HANDLE);
        }
        multi = mit->second;
        if (std::find(multi->attached.begin(), multi->attached.end(), curl_handle) !=
            multi->attached.end()) {
            return RecordCode("curl_multi_add_handle", XCURLM_ADDED_ALREADY);
        }
        multi->attached.push_back(curl_handle);
        multi->pending.push_back(curl_handle);
    }
    {
        std::lock_guard<std::mutex> wlk(multi->wait_mutex);
        multi->wakeup_pending = true;
    }
    multi->wait_cv.notify_all();
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

int32_t OS_SHIM_CDECL curl_multi_poll(
    XCurlMultiHandle multi_handle,
    XCurlWaitFd extra_fds[],
    uint32_t extra_nfds,
    int32_t timeout_ms,
    int32_t* numfds) {
    return MultiWaitImpl(
        "curl_multi_poll", multi_handle, extra_fds, extra_nfds, timeout_ms, numfds, true);
}

int32_t OS_SHIM_CDECL curl_multi_wait(
    XCurlMultiHandle multi_handle,
    XCurlWaitFd extra_fds[],
    uint32_t extra_nfds,
    int32_t timeout_ms,
    int32_t* numfds) {
    return MultiWaitImpl(
        "curl_multi_wait", multi_handle, extra_fds, extra_nfds, timeout_ms, numfds, false);
}

int32_t OS_SHIM_CDECL curl_multi_wakeup(XCurlMultiHandle multi_handle) {
    if (!multi_handle) {
        return RecordCode("curl_multi_wakeup", XCURLM_BAD_HANDLE);
    }
    std::shared_ptr<MultiObject> multi;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto mit = st.multi_handles.find(multi_handle);
        if (mit == st.multi_handles.end()) {
            return RecordCode("curl_multi_wakeup", XCURLM_BAD_HANDLE);
        }
        multi = mit->second;
    }
    {
        std::lock_guard<std::mutex> wlk(multi->wait_mutex);
        multi->wakeup_pending = true;
    }
    multi->wait_cv.notify_all();
    return RecordCode("curl_multi_wakeup", XCURLM_OK);
}

int32_t OS_SHIM_CDECL curl_multi_setopt(
    XCurlMultiHandle multi_handle,
    int32_t option,
    ...) {
    if (!multi_handle) {
        return RecordCode("curl_multi_setopt", XCURLM_BAD_HANDLE);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto mit = st.multi_handles.find(multi_handle);
    if (mit == st.multi_handles.end()) {
        return RecordCode("curl_multi_setopt", XCURLM_BAD_HANDLE);
    }
    MultiObject& m = *mit->second;

    va_list ap;
    va_start(ap, option);
    int32_t res = XCURLM_OK;
    switch (option) {
        case XCURLMOPT_PIPELINING:
            m.pipelining = va_arg(ap, long);
            break;
        case XCURLMOPT_MAXCONNECTS:
            m.max_connects = va_arg(ap, long);
            break;
        case XCURLMOPT_MAX_HOST_CONNECTIONS:
            m.max_host_connections = va_arg(ap, long);
            break;
        case XCURLMOPT_MAX_TOTAL_CONNECTIONS:
            m.max_total_connections = va_arg(ap, long);
            break;
        case XCURLMOPT_MAX_PIPELINE_LENGTH:
        case XCURLMOPT_MAX_CONCURRENT_STREAMS:
            (void)va_arg(ap, long);
            break;
        case XCURLMOPT_SOCKETDATA:
        case XCURLMOPT_TIMERDATA:
        case XCURLMOPT_SOCKETFUNCTION:
        case XCURLMOPT_TIMERFUNCTION:
            (void)va_arg(ap, void*);
            break;
        default:
            res = XCURLM_UNKNOWN_OPTION;
            break;
    }
    va_end(ap);
    return RecordCode("curl_multi_setopt", res);
}

const char* OS_SHIM_CDECL curl_multi_strerror(int32_t code) {
    RecordOk("curl_multi_strerror");
    switch (code) {
        case XCURLM_CALL_MULTI_PERFORM: return "Please call curl_multi_perform() soon";
        case XCURLM_OK: return "No error";
        case XCURLM_BAD_HANDLE: return "Invalid multi handle";
        case XCURLM_BAD_EASY_HANDLE: return "Invalid easy handle";
        case XCURLM_OUT_OF_MEMORY: return "Out of memory";
        case XCURLM_INTERNAL_ERROR: return "Internal error";
        case XCURLM_BAD_SOCKET: return "Invalid socket argument";
        case XCURLM_UNKNOWN_OPTION: return "Unknown option";
        case XCURLM_ADDED_ALREADY: return "The easy handle is already added to a multi handle";
        case XCURLM_RECURSIVE_API_CALL: return "API function called from within callback";
        case XCURLM_WAKEUP_FAILURE: return "Wakeup is unavailable or failed";
        case XCURLM_BAD_FUNCTION_ARGUMENT: return "A libcurl function was given a bad argument";
        default: return "Unknown XCurl multi error";
    }
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

XCurlShareHandle OS_SHIM_CDECL curl_share_init(void) {
    auto obj = std::make_unique<ShareObject>();
    void* raw = obj.get();
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.share_handles[raw] = std::move(obj);
    }
    RecordOk("curl_share_init");
    return raw;
}

int32_t OS_SHIM_CDECL curl_share_cleanup(XCurlShareHandle share) {
    if (!share) {
        return RecordCode("curl_share_cleanup", XCURLSHE_INVALID);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.share_handles.find(share);
    if (it == st.share_handles.end()) {
        return RecordCode("curl_share_cleanup", XCURLSHE_INVALID);
    }
    st.share_handles.erase(it);
    return RecordCode("curl_share_cleanup", XCURLSHE_OK);
}

int32_t OS_SHIM_CDECL curl_share_setopt(XCurlShareHandle share, int32_t option, ...) {
    if (!share) {
        return RecordCode("curl_share_setopt", XCURLSHE_INVALID);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.share_handles.find(share);
    if (it == st.share_handles.end()) {
        return RecordCode("curl_share_setopt", XCURLSHE_INVALID);
    }
    va_list ap;
    va_start(ap, option);
    int32_t res = XCURLSHE_OK;
    switch (option) {
        case 1:  // CURLSHOPT_SHARE
            it->second->share_mask |= va_arg(ap, long);
            break;
        case 2:  // CURLSHOPT_UNSHARE
            it->second->share_mask &= ~va_arg(ap, long);
            break;
        case 3:  // CURLSHOPT_LOCKFUNC
            it->second->lock_fn = va_arg(ap, void*);
            break;
        case 4:  // CURLSHOPT_UNLOCKFUNC
            it->second->unlock_fn = va_arg(ap, void*);
            break;
        case 5:  // CURLSHOPT_USERDATA
            it->second->user_data = va_arg(ap, void*);
            break;
        default:
            res = XCURLSHE_BAD_OPTION;
            break;
    }
    va_end(ap);
    return RecordCode("curl_share_setopt", res);
}

const char* OS_SHIM_CDECL curl_share_strerror(int32_t code) {
    RecordOk("curl_share_strerror");
    switch (code) {
        case XCURLSHE_OK: return "No error";
        case XCURLSHE_BAD_OPTION: return "Unknown share option";
        case XCURLSHE_IN_USE: return "Share currently in use";
        case XCURLSHE_INVALID: return "Invalid share handle";
        case XCURLSHE_NOMEM: return "Out of memory";
        case XCURLSHE_NOT_BUILT_IN: return "Feature not enabled in this library";
        default: return "Unknown XCurl share error";
    }
}

XCurlUHandle OS_SHIM_CDECL curl_url(void) {
    auto obj = std::make_unique<XCurlU>();
    XCurlUHandle raw = obj.get();
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.url_handles[raw] = std::move(obj);
    }
    RecordOk("curl_url");
    return raw;
}

void OS_SHIM_CDECL curl_url_cleanup(XCurlUHandle handle) {
    RecordOk("curl_url_cleanup");
    if (!handle) return;
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.url_handles.erase(handle);
}

XCurlUHandle OS_SHIM_CDECL curl_url_dup(XCurlUHandle in) {
    if (!in) {
        RecordCode("curl_url_dup", XCURLUE_BAD_HANDLE);
        return nullptr;
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.url_handles.find(in);
    if (it == st.url_handles.end()) {
        RecordCode("curl_url_dup", XCURLUE_BAD_HANDLE);
        return nullptr;
    }
    auto copy = std::make_unique<XCurlU>(*it->second);
    XCurlUHandle raw = copy.get();
    st.url_handles[raw] = std::move(copy);
    RecordOk("curl_url_dup");
    return raw;
}

int32_t OS_SHIM_CDECL curl_url_set(
    XCurlUHandle handle,
    int32_t what,
    const char* part,
    uint32_t flags) {
    if (!handle) {
        return RecordCode("curl_url_set", XCURLUE_BAD_HANDLE);
    }
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.url_handles.find(handle);
    if (it == st.url_handles.end()) {
        return RecordCode("curl_url_set", XCURLUE_BAD_HANDLE);
    }
    XCurlU& u = *it->second;

    std::string val;
    if (part) {
        if ((flags & XCURLU_URLENCODE) != 0u && what != XCURLUPART_URL) {
            val = PercentEncode(part, std::strlen(part));
        } else {
            val = part;
        }
    }

    int32_t res = XCURLUE_OK;
    switch (what) {
        case XCURLUPART_URL:
            if (!part) {
                u = XCurlU{};
            } else {
                res = ParseFullUrl(val, flags, u);
            }
            break;
        case XCURLUPART_SCHEME:
            u.scheme = part ? ToLowerAscii(val) : "";
            break;
        case XCURLUPART_USER:
            u.user = val;
            break;
        case XCURLUPART_PASSWORD:
            u.password = val;
            break;
        case XCURLUPART_OPTIONS:
            u.options = val;
            break;
        case XCURLUPART_HOST:
            u.host = val;
            break;
        case XCURLUPART_PORT:
            if (!val.empty()) {
                unsigned long pnum = 0;
                for (char c : val) {
                    if (c < '0' || c > '9') {
                        return RecordCode("curl_url_set", XCURLUE_BAD_PORT_NUMBER);
                    }
                    pnum = pnum * 10ul + static_cast<unsigned long>(c - '0');
                    if (pnum > 65535ul) {
                        return RecordCode("curl_url_set", XCURLUE_BAD_PORT_NUMBER);
                    }
                }
                if (pnum == 0ul) {
                    return RecordCode("curl_url_set", XCURLUE_BAD_PORT_NUMBER);
                }
            }
            u.port = val;
            break;
        case XCURLUPART_PATH:
            if (!val.empty() && val.front() != '/') {
                u.path = "/" + val;
            } else {
                u.path = val;
            }
            break;
        case XCURLUPART_QUERY:
            if ((flags & XCURLU_APPENDQUERY) != 0u && !u.query.empty() && !val.empty()) {
                u.query += "&";
                u.query += val;
            } else {
                u.query = val;
            }
            break;
        case XCURLUPART_FRAGMENT:
            u.fragment = val;
            break;
        case XCURLUPART_ZONEID:
            u.zoneid = val;
            break;
        default:
            res = XCURLUE_UNKNOWN_PART;
            break;
    }

    return RecordCode("curl_url_set", res);
}

int32_t OS_SHIM_CDECL curl_url_get(
    XCurlUHandle handle,
    int32_t what,
    char** part,
    uint32_t flags) {
    if (!handle) {
        return RecordCode("curl_url_get", XCURLUE_BAD_HANDLE);
    }
    if (!part) {
        return RecordCode("curl_url_get", XCURLUE_BAD_PARTPOINTER);
    }
    *part = nullptr;

    std::string out;
    {
        auto& st = GetXCurl();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.url_handles.find(handle);
        if (it == st.url_handles.end()) {
            return RecordCode("curl_url_get", XCURLUE_BAD_HANDLE);
        }
        const XCurlU& u = *it->second;

        switch (what) {
            case XCURLUPART_URL:
                if (u.host.empty()) return RecordCode("curl_url_get", XCURLUE_NO_HOST);
                out = ComposeFullUrl(u, flags);
                break;
            case XCURLUPART_SCHEME:
                if (u.scheme.empty()) return RecordCode("curl_url_get", XCURLUE_NO_SCHEME);
                out = u.scheme;
                break;
            case XCURLUPART_USER:
                if (u.user.empty()) return RecordCode("curl_url_get", XCURLUE_NO_USER);
                out = u.user;
                break;
            case XCURLUPART_PASSWORD:
                if (u.password.empty()) return RecordCode("curl_url_get", XCURLUE_NO_PASSWORD);
                out = u.password;
                break;
            case XCURLUPART_OPTIONS:
                if (u.options.empty()) return RecordCode("curl_url_get", XCURLUE_NO_OPTIONS);
                out = u.options;
                break;
            case XCURLUPART_HOST:
                if (u.host.empty()) return RecordCode("curl_url_get", XCURLUE_NO_HOST);
                out = u.host;
                break;
            case XCURLUPART_PORT:
                if (!u.port.empty()) {
                    out = u.port;
                } else if ((flags & XCURLU_DEFAULT_PORT) != 0u) {
                    out = DefaultPortForScheme(u.scheme);
                    if (out.empty()) return RecordCode("curl_url_get", XCURLUE_NO_PORT);
                } else {
                    return RecordCode("curl_url_get", XCURLUE_NO_PORT);
                }
                break;
            case XCURLUPART_PATH:
                out = u.path.empty() ? "/" : u.path;
                break;
            case XCURLUPART_QUERY:
                if (u.query.empty()) return RecordCode("curl_url_get", XCURLUE_NO_QUERY);
                out = u.query;
                break;
            case XCURLUPART_FRAGMENT:
                if (u.fragment.empty()) return RecordCode("curl_url_get", XCURLUE_NO_FRAGMENT);
                out = u.fragment;
                break;
            case XCURLUPART_ZONEID:
                if (u.zoneid.empty()) return RecordCode("curl_url_get", XCURLUE_NO_HOST);
                out = u.zoneid;
                break;
            default:
                return RecordCode("curl_url_get", XCURLUE_UNKNOWN_PART);
        }
    }

    if ((flags & XCURLU_URLDECODE) != 0u && what != XCURLUPART_URL) {
        std::string decoded;
        if (!PercentDecode(out.data(), out.size(), decoded, true)) {
            return RecordCode("curl_url_get", XCURLUE_URLDECODE);
        }
        out = std::move(decoded);
    }

    char* copy = CurlAllocCopyString(out);
    if (!copy) {
        return RecordCode("curl_url_get", XCURLUE_OUT_OF_MEMORY);
    }
    *part = copy;
    return RecordCode("curl_url_get", XCURLUE_OK);
}

int32_t OS_SHIM_CDECL xcurl_global_init_mem(
    long flags,
    XCurlMallocCallback m,
    XCurlFreeCallback f,
    XCurlReallocCallback r,
    XCurlStrdupCallback s,
    XCurlCallocCallback c) {
    const int32_t rc = curl_global_init_mem(flags, m, f, r, s, c);
    return RecordCode("xcurl_global_init_mem", rc);
}

void OS_SHIM_CDECL xcurl_global_suspend(void) {
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.suspended = true;
    RecordOk("xcurl_global_suspend");
}

void OS_SHIM_CDECL xcurl_global_resume(void) {
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.suspended = false;
    RecordOk("xcurl_global_resume");
}

int32_t OS_SHIM_CDECL xcurl_global_set_request_limit(uint32_t requestLimit) {
    auto& st = GetXCurl();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.request_limit = requestLimit;
    return RecordCode("xcurl_global_set_request_limit", XCURLE_OK);
}

}  // extern "C"
