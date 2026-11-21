#include "openscarlett/loader/vfs_mounts.h"

#include <algorithm>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace openscarlett {

namespace {

constexpr wchar_t kDefaultVfsRootW[] = L"E:\\OpenScarlett\\.tmp\\vfs";

wchar_t AsciiLowerW(wchar_t c) noexcept {
    if (c >= L'A' && c <= L'Z') {
        return static_cast<wchar_t>(c - L'A' + L'a');
    }
    return c;
}

char AsciiLowerA(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool IsExtendedPrefixW(std::wstring_view p) noexcept {
    return p.size() >= 4 &&
           (p[0] == L'\\' || p[0] == L'/') &&
           (p[1] == L'\\' || p[1] == L'/') &&
           p[2] == L'?' &&
           (p[3] == L'\\' || p[3] == L'/');
}

bool IsExtendedPrefixA(std::string_view p) noexcept {
    return p.size() >= 4 &&
           (p[0] == '\\' || p[0] == '/') &&
           (p[1] == '\\' || p[1] == '/') &&
           p[2] == '?' &&
           (p[3] == '\\' || p[3] == '/');
}

VfsDrive DriveCharToEnum(wchar_t c) noexcept {
    switch (AsciiLowerW(c)) {
        case L'g': return VfsDrive::Game;
        case L't': return VfsDrive::Scratch;
        case L's': return VfsDrive::Save;
        default: return VfsDrive::None;
    }
}

std::wstring ReadEnvVarW(const wchar_t* name) {
#if defined(_WIN32)
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring out(needed, L'\0');
    DWORD written = ::GetEnvironmentVariableW(name, out.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    out.resize(written);
    return out;
#else
    (void)name;
    return {};
#endif
}

bool HasAbsoluteDrivePrefixW(std::wstring_view p) noexcept {
    return p.size() >= 3 &&
           ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
           p[1] == L':' &&
           (p[2] == L'\\' || p[2] == L'/');
}

}  // namespace

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
#if defined(_WIN32)
    int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
#if defined(_WIN32)
    int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

bool EnsureDirectoryTreeW(std::wstring_view path) {
    if (path.empty()) {
        return false;
    }
#if defined(_WIN32)
    std::wstring normalized(path);
    for (wchar_t& c : normalized) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    while (normalized.size() > 3 && normalized.back() == L'\\') {
        normalized.pop_back();
    }

    size_t start = 0;
    if (IsExtendedPrefixW(normalized)) {
        start = 4;
    }
    if (normalized.size() >= start + 3 && normalized[start + 1] == L':' && normalized[start + 2] == L'\\') {
        start += 3;
    } else if (normalized.size() >= 2 && normalized[0] == L'\\' && normalized[1] == L'\\') {
        // UNC path: skip \\server\share\ prefix
        size_t pos = normalized.find(L'\\', 2);
        if (pos == std::wstring::npos) {
            return false;
        }
        pos = normalized.find(L'\\', pos + 1);
        start = (pos == std::wstring::npos) ? normalized.size() : (pos + 1);
    }

    for (size_t i = start; i <= normalized.size(); ++i) {
        if (i == normalized.size() || normalized[i] == L'\\') {
            if (i == start) {
                continue;
            }
            std::wstring prefix = normalized.substr(0, i);
            if (!::CreateDirectoryW(prefix.c_str(), nullptr)) {
                DWORD err = ::GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    DWORD attrs = ::GetFileAttributesW(prefix.c_str());
                    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        return false;
                    }
                }
            }
        }
    }
    DWORD attrs = ::GetFileAttributesW(normalized.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    return false;
#endif
}

VfsMounts::VfsMounts(const VfsMountConfig& config) {
    Configure(config);
}

VfsMountConfig VfsMounts::BuildDefaultConfig(std::wstring_view default_g_root,
                                             std::wstring_view default_vfs_root) {
    VfsMountConfig cfg{};

    std::wstring base_vfs;
    if (!default_vfs_root.empty()) {
        base_vfs = NormalizeRoot(default_vfs_root);
    } else {
        std::wstring env_temp = ReadEnvVarW(L"TEMP");
        if (!env_temp.empty()) {
            base_vfs = NormalizeRoot(env_temp + L"\\vfs");
        } else {
            base_vfs = std::wstring(kDefaultVfsRootW);
        }
    }

    std::wstring env_g = ReadEnvVarW(L"OPENSCARLETT_VFS_G");
    if (!env_g.empty()) {
        cfg.g_root = NormalizeRoot(env_g);
    } else if (!default_g_root.empty()) {
        cfg.g_root = NormalizeRoot(default_g_root);
    }

    std::wstring env_t = ReadEnvVarW(L"OPENSCARLETT_VFS_T");
    if (!env_t.empty()) {
        cfg.t_root = NormalizeRoot(env_t);
    } else {
        cfg.t_root = NormalizeRoot(base_vfs + L"\\T");
    }

    std::wstring env_s = ReadEnvVarW(L"OPENSCARLETT_VFS_S");
    if (!env_s.empty()) {
        cfg.s_root = NormalizeRoot(env_s);
    } else {
        std::wstring env_save_root = ReadEnvVarW(L"OPENSCARLETT_SAVE_ROOT");
        if (!env_save_root.empty()) {
            cfg.s_root = NormalizeRoot(env_save_root);
        } else {
            cfg.s_root = NormalizeRoot(base_vfs + L"\\S");
        }
    }

    return cfg;
}

std::wstring VfsMounts::NormalizeRoot(std::wstring_view root) {
    if (root.empty()) {
        return {};
    }
    std::wstring out(root);
    for (wchar_t& c : out) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    // Strip trailing backslashes except for bare drive roots like "E:\" or "\\?\E:\"
    const size_t min_len = IsExtendedPrefixW(out) ? 7u : 3u;
    while (out.size() > min_len && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

void VfsMounts::Configure(const VfsMountConfig& config) {
    g_root_ = NormalizeRoot(config.g_root);
    t_root_ = NormalizeRoot(config.t_root);
    s_root_ = NormalizeRoot(config.s_root);
}

bool VfsMounts::EnsureWritableMounts() const {
    bool ok = true;
    if (!t_root_.empty()) {
        ok = EnsureDirectoryTreeW(t_root_) && ok;
    }
    if (!s_root_.empty()) {
        ok = EnsureDirectoryTreeW(s_root_) && ok;
    }
    return ok;
}

VfsDrive VfsMounts::DetectVirtualDriveW(std::wstring_view path) noexcept {
    if (IsExtendedPrefixW(path)) {
        path.remove_prefix(4);
    }
    if (path.size() < 2 || path[1] != L':') {
        return VfsDrive::None;
    }
    return DriveCharToEnum(path[0]);
}

VfsDrive VfsMounts::DetectVirtualDriveA(std::string_view path) noexcept {
    if (IsExtendedPrefixA(path)) {
        path.remove_prefix(4);
    }
    if (path.size() < 2 || path[1] != ':') {
        return VfsDrive::None;
    }
    switch (AsciiLowerA(path[0])) {
        case 'g': return VfsDrive::Game;
        case 't': return VfsDrive::Scratch;
        case 's': return VfsDrive::Save;
        default: return VfsDrive::None;
    }
}

VfsTranslateStatus VfsMounts::TranslateW(std::wstring_view virtual_path,
                                         std::wstring& out_host_path,
                                         bool ensure_writable_dirs) const {
    bool had_extended = false;
    std::wstring_view rest = virtual_path;
    if (IsExtendedPrefixW(rest)) {
        had_extended = true;
        rest.remove_prefix(4);
    }

    if (rest.size() < 2 || rest[1] != L':') {
        return VfsTranslateStatus::NotVirtual;
    }

    const VfsDrive drive = DriveCharToEnum(rest[0]);
    if (drive == VfsDrive::None) {
        return VfsTranslateStatus::NotVirtual;
    }

    const std::wstring* root = nullptr;
    bool is_writable = false;
    switch (drive) {
        case VfsDrive::Game:
            root = &g_root_;
            break;
        case VfsDrive::Scratch:
            root = &t_root_;
            is_writable = true;
            break;
        case VfsDrive::Save:
            root = &s_root_;
            is_writable = true;
            break;
        case VfsDrive::None:
            return VfsTranslateStatus::NotVirtual;
    }

    if (!root || root->empty()) {
        return VfsTranslateStatus::UnconfiguredMount;
    }

    // Parse remainder after "X:" into normalized path segments and block any escape above root.
    std::wstring_view sub = rest.substr(2);
    std::vector<std::wstring_view> parts;
    size_t pos = 0;
    while (pos < sub.size()) {
        while (pos < sub.size() && (sub[pos] == L'\\' || sub[pos] == L'/')) {
            ++pos;
        }
        if (pos >= sub.size()) {
            break;
        }
        size_t end = pos;
        while (end < sub.size() && sub[end] != L'\\' && sub[end] != L'/') {
            ++end;
        }
        std::wstring_view comp = sub.substr(pos, end - pos);
        pos = end;

        if (comp == L".") {
            continue;
        }
        if (comp == L"..") {
            if (parts.empty()) {
                return VfsTranslateStatus::RejectedTraversal;
            }
            parts.pop_back();
            continue;
        }
        // Reject embedded drive-letter colons inside virtual sub-paths.
        if (comp.find(L':') != std::wstring_view::npos) {
            return VfsTranslateStatus::RejectedTraversal;
        }
        parts.push_back(comp);
    }

    if (ensure_writable_dirs && is_writable) {
        EnsureDirectoryTreeW(*root);
    }

    out_host_path.clear();
    if (had_extended && !IsExtendedPrefixW(*root) && HasAbsoluteDrivePrefixW(*root)) {
        out_host_path.append(L"\\\\?\\");
    }
    out_host_path.append(*root);

    for (const auto& part : parts) {
        if (!out_host_path.empty() && out_host_path.back() != L'\\') {
            out_host_path.push_back(L'\\');
        }
        out_host_path.append(part);
    }

    return VfsTranslateStatus::Translated;
}

VfsTranslateStatus VfsMounts::TranslateUtf8(std::string_view virtual_path,
                                            std::string& out_host_path,
                                            bool ensure_writable_dirs) const {
    if (DetectVirtualDriveA(virtual_path) == VfsDrive::None) {
        return VfsTranslateStatus::NotVirtual;
    }
    std::wstring wide_in = Utf8ToWide(virtual_path);
    std::wstring wide_out;
    VfsTranslateStatus st = TranslateW(wide_in, wide_out, ensure_writable_dirs);
    if (st == VfsTranslateStatus::Translated) {
        out_host_path = WideToUtf8(wide_out);
    }
    return st;
}

}  // namespace openscarlett
