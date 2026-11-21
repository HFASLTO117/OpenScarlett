#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace openscarlett {

enum class VfsDrive : uint8_t {
    None = 0,
    Game = 1,     // G: -> Game package root (read-only package view)
    Scratch = 2,  // T: -> Title scratch directory on E:
    Save = 3,     // S: -> Persistent save directory on E:
};

enum class VfsTranslateStatus : uint8_t {
    NotVirtual = 0,         // Path does not target G:, T:, or S:
    Translated = 1,         // Successfully mapped to host backing path
    RejectedTraversal = 2,  // Attempted '..' escape above mount root or invalid drive injection
    UnconfiguredMount = 3,  // Virtual drive matched, but backing root is empty
};

struct VfsMountConfig {
    std::wstring g_root;  // Backing host directory for G:
    std::wstring t_root;  // Backing host directory for T:
    std::wstring s_root;  // Backing host directory for S:
};

// Clean-room UTF-8 / UTF-16 conversion helpers (safe on Windows x64).
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

// Creates a directory tree iteratively using Win32 CreateDirectoryW (no C:\ fallback).
bool EnsureDirectoryTreeW(std::wstring_view path);

// Clean-room virtual drive translator for Xbox Series X|S G:\, T:\, and S:\ mounts.
// Immutable once configured, making TranslateW / TranslateUtf8 lock-free and thread-safe.
class VfsMounts {
public:
    VfsMounts() = default;
    explicit VfsMounts(const VfsMountConfig& config);

    // Builds default mount config honoring OPENSCARLETT_VFS_G/T/S and OPENSCARLETT_SAVE_ROOT,
    // defaulting T:\ and S:\ under E:\OpenScarlett\.tmp\vfs\ (never C:\).
    static VfsMountConfig BuildDefaultConfig(std::wstring_view default_g_root = {},
                                             std::wstring_view default_vfs_root = {});

    void Configure(const VfsMountConfig& config);

    // Creates T:\ and S:\ backing directories on disk if configured.
    bool EnsureWritableMounts() const;

    // Fast O(1) prefix check to identify potential G:\, T:\, S:\ virtual paths without allocations.
    static VfsDrive DetectVirtualDriveW(std::wstring_view path) noexcept;
    static VfsDrive DetectVirtualDriveA(std::string_view path) noexcept;

    // Translates a wide Win32 path (supports case-insensitive drives, '/' and '\', and '\\?\' prefix).
    VfsTranslateStatus TranslateW(std::wstring_view virtual_path,
                                  std::wstring& out_host_path,
                                  bool ensure_writable_dirs = true) const;

    // Translates a UTF-8 path string.
    VfsTranslateStatus TranslateUtf8(std::string_view virtual_path,
                                     std::string& out_host_path,
                                     bool ensure_writable_dirs = true) const;

    const std::wstring& GetGameRoot() const noexcept { return g_root_; }
    const std::wstring& GetScratchRoot() const noexcept { return t_root_; }
    const std::wstring& GetSaveRoot() const noexcept { return s_root_; }

private:
    static std::wstring NormalizeRoot(std::wstring_view root);

    std::wstring g_root_;
    std::wstring t_root_;
    std::wstring s_root_;
};

}  // namespace openscarlett
