#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace openscarlett {

enum class VfsDrive : uint8_t {
    None = 0,
    Game = 1,     // G: or GAME: -> Game package root (read-only base + CoW overlay)
    Scratch = 2,  // T: -> Title scratch directory on E:
    Save = 3,     // S: -> Persistent save directory on E:
};

enum class VfsAccessMode : uint8_t {
    ReadOnly = 0,     // Check whiteout -> overlay -> read-only base
    WriteCreate = 1,  // Never touch g_root; target g_overlay_root, clear whiteout, ensure parent dirs
    WriteModify = 2,  // Never touch g_root; CoW-copy base file into g_overlay_root if not yet overlaid
    Delete = 3,       // Never touch g_root; remove from overlay and record whiteout if present in base
};

enum class VfsTranslateStatus : uint8_t {
    NotVirtual = 0,         // Path does not target G:, GAME:, T:, or S:
    Translated = 1,         // Successfully mapped to host backing path
    RejectedTraversal = 2,  // Attempted '..' escape above mount root or invalid drive injection
    UnconfiguredMount = 3,  // Virtual drive matched, but backing root is empty
    DeletedWhiteout = 4,    // Path on G:/GAME: is hidden by a CoW whiteout marker
    ReadOnlyViolation = 5,  // Write/Delete on G:/GAME: attempted with no CoW overlay configured
};

struct VfsMountConfig {
    std::wstring g_root;          // Backing host directory for G: / GAME: (strictly read-only)
    std::wstring t_root;          // Backing host directory for T:
    std::wstring s_root;          // Backing host directory for S:
    std::wstring g_overlay_root;  // CoW overlay & whiteout directory for G: / GAME:
};

// Clean-room UTF-8 / UTF-16 conversion helpers (safe on Windows x64).
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

// Creates a directory tree iteratively using Win32 CreateDirectoryW (no C:\ fallback).
bool EnsureDirectoryTreeW(std::wstring_view path);

// Clean-room virtual drive translator for Xbox Series X|S G:\, GAME:\, T:\, and S:\ mounts,
// with Read-Only Copy-on-Write (CoW) overlay & whiteout tracking for G:\ / GAME:\.
class VfsMounts {
public:
    VfsMounts() = default;
    explicit VfsMounts(const VfsMountConfig& config);

    // Builds default mount config honoring OPENSCARLETT_VFS_G/T/S/G_OVERLAY and
    // OPENSCARLETT_SAVE_ROOT, defaulting writable dirs under %TEMP%\vfs\ or
    // E:\OpenScarlett\.tmp\vfs\ (never C:\).
    static VfsMountConfig BuildDefaultConfig(std::wstring_view default_g_root = {},
                                             std::wstring_view default_vfs_root = {});

    void Configure(const VfsMountConfig& config);

    // Creates T:\, S:\, and G_overlay backing directories on disk if configured (never touches g_root).
    bool EnsureWritableMounts() const;

    // Fast O(1) prefix check for G:, GAME:, T:, S: (case-insensitive, supports '\\?\').
    static VfsDrive DetectVirtualDriveW(std::wstring_view path) noexcept;
    static VfsDrive DetectVirtualDriveA(std::string_view path) noexcept;

    // Access-aware translation implementing Read-Only CoW overlay and whiteout semantics on G:/GAME:.
    VfsTranslateStatus TranslateForAccessW(std::wstring_view virtual_path,
                                           VfsAccessMode mode,
                                           std::wstring& out_host_path,
                                           bool* out_was_whiteout = nullptr,
                                           bool ensure_writable_dirs = true) const;

    // Translates a wide Win32 path for read/default access (supports G:, GAME:, T:, S:, '/' and '\', '\\?\').
    VfsTranslateStatus TranslateW(std::wstring_view virtual_path,
                                  std::wstring& out_host_path,
                                  bool ensure_writable_dirs = true) const;

    // Translates a UTF-8 path string for read/default access.
    VfsTranslateStatus TranslateUtf8(std::string_view virtual_path,
                                     std::string& out_host_path,
                                     bool ensure_writable_dirs = true) const;

    // CoW overlay & whiteout helpers for G:/GAME: (never modify g_root).
    bool IsWhiteoutedW(std::wstring_view virtual_path) const;
    bool IsVisibleGamePathW(std::wstring_view virtual_path, bool* out_is_dir = nullptr) const;
    bool DeleteVirtualFileW(std::wstring_view virtual_path, uint32_t& out_win32_error) const;
    bool RemoveVirtualDirectoryW(std::wstring_view virtual_path, uint32_t& out_win32_error) const;
    bool MoveVirtualPathW(std::wstring_view src_path,
                          std::wstring_view dst_path,
                          uint32_t flags,
                          uint32_t& out_win32_error) const;

    // Resolves both read-only base and overlay paths for a G:/GAME: path (used by FindFirstFileW).
    VfsTranslateStatus ResolveGamePairW(std::wstring_view virtual_path,
                                        std::wstring& out_base_path,
                                        std::wstring& out_overlay_path,
                                        std::wstring& out_rel_path,
                                        bool* out_was_whiteout = nullptr) const;

    const std::wstring& GetGameRoot() const noexcept { return g_root_; }
    const std::wstring& GetGameOverlayRoot() const noexcept { return g_overlay_root_; }
    const std::wstring& GetScratchRoot() const noexcept { return t_root_; }
    const std::wstring& GetSaveRoot() const noexcept { return s_root_; }

private:
    static std::wstring NormalizeRoot(std::wstring_view root);

    std::wstring g_root_;
    std::wstring t_root_;
    std::wstring s_root_;
    std::wstring g_overlay_root_;
};

}  // namespace openscarlett
