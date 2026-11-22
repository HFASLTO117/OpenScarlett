#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_stages_ext.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/gpu/xg_xs_abi.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgiformat.h>
#include <windows.h>
#endif

using namespace openscarlett;

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, const std::string& detail = {}) {
    if (!cond) {
        ++g_failures;
        std::cerr << "[FAIL] " << expr;
        if (!detail.empty()) std::cerr << " :: " << detail;
        std::cerr << "\n";
    }
}

#define EXPECT_TRUE(cond) Check((cond), #cond)
#define EXPECT_OK(st) Check((st).ok(), #st, (st).message)

std::string FindOptionalSpirvVal() {
    std::error_code ec;
#if defined(_WIN32)
    char env_buf[1024]{};
    const DWORD env_len = GetEnvironmentVariableA(
        "OPENSCARLETT_SPIRV_VAL", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (env_len > 0 && env_len < sizeof(env_buf) && std::filesystem::exists(env_buf, ec)) {
        return std::string(env_buf, env_len);
    }
#else
    if (const char* env_p = std::getenv("OPENSCARLETT_SPIRV_VAL")) {
        if (*env_p && std::filesystem::exists(env_p, ec)) {
            return env_p;
        }
    }
#endif
    // Walk up from current working directory looking for .tools/spirv-val.exe (portable, no hardcoded drive).
    std::filesystem::path cur = std::filesystem::current_path(ec);
    for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
        auto cand = cur / ".tools" / "spirv-val.exe";
        if (std::filesystem::exists(cand, ec)) {
            return cand.string();
        }
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

std::vector<uint8_t> MakeDeterministicPattern(size_t size, uint32_t seed) {
    std::vector<uint8_t> buf(size);
    uint32_t x = seed ^ 0x9E3779B9u;
    for (size_t i = 0; i < size; ++i) {
        x ^= x << 13u;
        x ^= x >> 17u;
        x ^= x << 5u;
        buf[i] = static_cast<uint8_t>((x + i * 31u) & 0xFFu);
    }
    return buf;
}

void ValidateSpirvBoth(const std::vector<uint32_t>& spv, const std::string& spv_val_path, const char* label) {
    Status st = ValidateSpirvBinary(spv);
    Check(st.ok(), label, "ValidateSpirvBinary: " + st.message);
    if (!spv_val_path.empty()) {
        std::string diag;
        Status ext = ValidateSpirvExternal(spv, diag, spv_val_path, "vulkan1.2");
        Check(ext.ok(), label, "spirv-val.exe: " + ext.message + " " + diag);
    }
}

// ---------------------------------------------------------------------------
// Test 1: GPU Untile + Retile bit-exact parity vs CPU xg_layout
// ---------------------------------------------------------------------------
void RunUntileRetile2DCase(
    XgGpuUntiler& untiler,
    bool test_d3d12,
    bool test_vulkan,
    const char* name,
    XG_TEXTURE2D_DESC desc,
    uint32_t mip_level,
    uint32_t slice_index,
    uint32_t custom_row_pitch = 0u) {
    XG_RESOURCE_LAYOUT layout{};
    EXPECT_TRUE(xg::ComputeTexture2DLayout(desc, layout));
    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint32_t row_pitch = (custom_row_pitch == 0u) ? (ml.WidthElements * bpe) : custom_row_pitch;
    const size_t linear_bytes = static_cast<size_t>(row_pitch) * ml.HeightElements;

    std::vector<uint8_t> src_linear = MakeDeterministicPattern(linear_bytes, 0x100u + mip_level * 17u + slice_index * 7u);
    std::vector<uint8_t> cpu_tiled(static_cast<size_t>(layout.SizeBytes), 0u);
    EXPECT_TRUE(xg::TileSurface2D(desc, mip_level, slice_index, src_linear.data(), row_pitch,
                                  cpu_tiled.data(), cpu_tiled.size()));

    std::vector<uint8_t> cpu_untiled(linear_bytes, 0u);
    EXPECT_TRUE(xg::UntileSurface2D(desc, mip_level, slice_index, cpu_tiled.data(), cpu_tiled.size(),
                                    cpu_untiled.data(), row_pitch));
    // Sanity check CPU roundtrip on active row bytes
    for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
        EXPECT_TRUE(std::memcmp(src_linear.data() + static_cast<size_t>(y) * row_pitch,
                                cpu_untiled.data() + static_cast<size_t>(y) * row_pitch,
                                ml.WidthElements * bpe) == 0);
    }

    // Build GPU params for Untile and Retile
    XgGpuSurface2DRequest req_u{};
    req_u.desc = desc;
    req_u.mip_level = mip_level;
    req_u.slice_index = slice_index;
    req_u.direction = XgGpuDirection::Untile;
    req_u.linear_row_pitch_bytes = row_pitch;
    XgGpuUntileParams params_u{};
    EXPECT_OK(BuildXgGpuUntileParams2D(req_u, params_u));

    XgGpuSurface2DRequest req_r = req_u;
    req_r.direction = XgGpuDirection::Retile;
    XgGpuUntileParams params_r{};
    EXPECT_OK(BuildXgGpuUntileParams2D(req_r, params_r));

#if defined(_WIN32)
    if (test_d3d12 && untiler.IsD3D12Ready()) {
        std::vector<uint8_t> gpu_untiled(linear_bytes, 0u);
        XgGpuDispatchMetrics mu{};
        Status su = untiler.ExecuteD3D12(params_u, cpu_tiled, gpu_untiled, nullptr, &mu);
        Check(su.ok(), name, "D3D12 Untile: " + su.message);
        bool match_u = true;
        for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
            if (std::memcmp(cpu_untiled.data() + static_cast<size_t>(y) * row_pitch,
                            gpu_untiled.data() + static_cast<size_t>(y) * row_pitch,
                            ml.WidthElements * bpe) != 0) {
                match_u = false;
                break;
            }
        }
        Check(match_u, name, "D3D12 Untile bit-exact parity mismatch (" + mu.backend_name + ")");

        std::vector<uint8_t> gpu_retiled(static_cast<size_t>(layout.SizeBytes), 0u);
        XgGpuDispatchMetrics mr{};
        Status sr = untiler.ExecuteD3D12(params_r, src_linear, gpu_retiled, nullptr, &mr);
        Check(sr.ok(), name, "D3D12 Retile: " + sr.message);
        Check(gpu_retiled == cpu_tiled, name, "D3D12 Retile bit-exact parity mismatch (" + mr.backend_name + ")");
    }
#endif

    if (test_vulkan && untiler.IsVulkanHardwareIcd()) {
        std::vector<uint8_t> vk_untiled(linear_bytes, 0u);
        XgGpuDispatchMetrics mu{};
        Status su = untiler.ExecuteVulkan(params_u, cpu_tiled, vk_untiled, nullptr, &mu);
        Check(su.ok(), name, "Vulkan Untile: " + su.message);
        bool match_u = true;
        for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
            if (std::memcmp(cpu_untiled.data() + static_cast<size_t>(y) * row_pitch,
                            vk_untiled.data() + static_cast<size_t>(y) * row_pitch,
                            ml.WidthElements * bpe) != 0) {
                match_u = false;
                break;
            }
        }
        Check(match_u, name, "Vulkan Untile bit-exact parity mismatch");

        std::vector<uint8_t> vk_retiled(static_cast<size_t>(layout.SizeBytes), 0u);
        XgGpuDispatchMetrics mr{};
        Status sr = untiler.ExecuteVulkan(params_r, src_linear, vk_retiled, nullptr, &mr);
        Check(sr.ok(), name, "Vulkan Retile: " + sr.message);
        Check(vk_retiled == cpu_tiled, name, "Vulkan Retile bit-exact parity mismatch");
    }
}

void RunUntileRetile3DCase(
    XgGpuUntiler& untiler,
    bool test_d3d12,
    bool test_vulkan,
    const char* name,
    XG_TEXTURE3D_DESC desc,
    uint32_t mip_level) {
    XG_RESOURCE_LAYOUT layout{};
    EXPECT_TRUE(xg::ComputeTexture3DLayout(desc, layout));
    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint32_t row_pitch = ml.WidthElements * bpe;
    const uint32_t slice_pitch = row_pitch * ml.HeightElements;
    const size_t linear_bytes = static_cast<size_t>(slice_pitch) * ml.DepthElements;

    std::vector<uint8_t> src_linear = MakeDeterministicPattern(linear_bytes, 0x777u + mip_level * 29u);
    std::vector<uint8_t> cpu_tiled(static_cast<size_t>(layout.SizeBytes), 0u);
    EXPECT_TRUE(xg::TileSurface3D(desc, mip_level, src_linear.data(), row_pitch, slice_pitch,
                                  cpu_tiled.data(), cpu_tiled.size()));

    std::vector<uint8_t> cpu_untiled(linear_bytes, 0u);
    EXPECT_TRUE(xg::UntileSurface3D(desc, mip_level, cpu_tiled.data(), cpu_tiled.size(),
                                    cpu_untiled.data(), row_pitch, slice_pitch));
    EXPECT_TRUE(cpu_untiled == src_linear);

    XgGpuSurface3DRequest req_u{};
    req_u.desc = desc;
    req_u.mip_level = mip_level;
    req_u.direction = XgGpuDirection::Untile;
    XgGpuUntileParams params_u{};
    EXPECT_OK(BuildXgGpuUntileParams3D(req_u, params_u));

    XgGpuSurface3DRequest req_r = req_u;
    req_r.direction = XgGpuDirection::Retile;
    XgGpuUntileParams params_r{};
    EXPECT_OK(BuildXgGpuUntileParams3D(req_r, params_r));

#if defined(_WIN32)
    if (test_d3d12 && untiler.IsD3D12Ready()) {
        std::vector<uint8_t> gpu_untiled(linear_bytes, 0u);
        EXPECT_OK(untiler.ExecuteD3D12(params_u, cpu_tiled, gpu_untiled));
        Check(gpu_untiled == cpu_untiled, name, "D3D12 3D Untile mismatch");

        std::vector<uint8_t> gpu_retiled(static_cast<size_t>(layout.SizeBytes), 0u);
        EXPECT_OK(untiler.ExecuteD3D12(params_r, src_linear, gpu_retiled));
        Check(gpu_retiled == cpu_tiled, name, "D3D12 3D Retile mismatch");
    }
#endif

    if (test_vulkan && untiler.IsVulkanHardwareIcd()) {
        std::vector<uint8_t> vk_untiled(linear_bytes, 0u);
        EXPECT_OK(untiler.ExecuteVulkan(params_u, cpu_tiled, vk_untiled));
        Check(vk_untiled == cpu_untiled, name, "Vulkan 3D Untile mismatch");

        std::vector<uint8_t> vk_retiled(static_cast<size_t>(layout.SizeBytes), 0u);
        EXPECT_OK(untiler.ExecuteVulkan(params_r, src_linear, vk_retiled));
        Check(vk_retiled == cpu_tiled, name, "Vulkan 3D Retile mismatch");
    }
}

void TestFastClearMetadataPassthrough(XgGpuUntiler& untiler, bool test_d3d12, bool test_vulkan) {
    // 64x64 RGBA8 64KB_R_X surface: 8x8 blocks of 8x8 pixels (64 DCC blocks / 64 HTILE words / 64 CMASK nibbles)
    XG_TEXTURE2D_DESC desc{};
    desc.Width = 64u;
    desc.Height = 64u;
    desc.MipLevels = 1u;
    desc.ArraySize = 1u;
    desc.Format = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    desc.SampleCount = 1u;
    desc.BindFlags = XG_BIND_RENDER_TARGET | XG_BIND_SHADER_RESOURCE;
    desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_R_X;

    XG_RESOURCE_LAYOUT layout{};
    XG_RESOURCE_METADATA_LAYOUT meta_layout{};
    EXPECT_TRUE(xg::ComputeTexture2DLayout(desc, layout));
    EXPECT_TRUE(xg::ComputeTexture2DMetadataLayout(desc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, meta_layout));

    const uint32_t row_pitch = 64u * 4u;
    const size_t linear_bytes = static_cast<size_t>(row_pitch) * 64u;
    std::vector<uint8_t> orig_linear = MakeDeterministicPattern(linear_bytes, 0xCAFEu);
    std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0u);
    EXPECT_TRUE(xg::TileSurface2D(desc, 0u, 0u, orig_linear.data(), row_pitch, tiled.data(), tiled.size()));

    // 1) DCC fast-clear passthrough: mix 0x00, 0x40, 0x80, 0xC0, 0x20, and 0xFF (uncompressed/untouched!)
    {
        std::vector<uint8_t> dcc(static_cast<size_t>(meta_layout.Dcc.SlicePitchBytes), 0xFFu);
        // Set blocks (0,0)=0x00, (1,0)=0x40, (2,0)=0x80, (3,0)=0xC0, (4,0)=0x20; leave rest 0xFF!
        // PipeAligned = 0 for direct deterministic block indexing check
        dcc[0] = 0x00u;
        dcc[1] = 0x40u;
        dcc[2] = 0x80u;
        dcc[3] = 0xC0u;
        dcc[4] = 0x20u;

        XgGpuSurface2DRequest req{};
        req.desc = desc;
        req.direction = XgGpuDirection::Untile;
        req.metadata_mode = XgGpuMetadataMode::Dcc;
        req.dcc_pipe_aligned = 0u;
        req.clear_reg_value = {0x11223344u, 0u, 0u, 0u};

        XgGpuUntileParams params{};
        EXPECT_OK(BuildXgGpuUntileParams2D(req, params));

        auto verify_dcc = [&](const std::vector<uint8_t>& out, const char* tag) {
            const uint32_t expected_keys[5] = {0x00000000u, 0xFF000000u, 0x00FFFFFFu, 0xFFFFFFFFu, 0x11223344u};
            bool ok = true;
            for (uint32_t y = 0u; y < 64u && ok; ++y) {
                for (uint32_t x = 0u; x < 64u && ok; ++x) {
                    const uint32_t bx = x / 8u;
                    const uint32_t by = y / 8u;
                    uint32_t got = 0u;
                    std::memcpy(&got, out.data() + (y * 64u + x) * 4u, 4u);
                    if (by == 0u && bx < 5u) {
                        if (got != expected_keys[bx]) ok = false;
                    } else {
                        uint32_t orig = 0u;
                        std::memcpy(&orig, orig_linear.data() + (y * 64u + x) * 4u, 4u);
                        if (got != orig) ok = false;
                    }
                }
            }
            Check(ok, tag, "DCC fast-clear passthrough / non-cleared preservation mismatch");
        };

#if defined(_WIN32)
        if (test_d3d12 && untiler.IsD3D12Ready()) {
            std::vector<uint8_t> out(linear_bytes, 0u);
            EXPECT_OK(untiler.ExecuteD3D12(params, tiled, out, &dcc));
            verify_dcc(out, "D3D12 DCC fast-clear passthrough");
        }
#endif
        if (test_vulkan && untiler.IsVulkanHardwareIcd()) {
            std::vector<uint8_t> out(linear_bytes, 0u);
            EXPECT_OK(untiler.ExecuteVulkan(params, tiled, out, &dcc));
            verify_dcc(out, "Vulkan DCC fast-clear passthrough");
        }
    }

    // 2) HTILE z-clear passthrough on D32_FLOAT
    {
        XG_TEXTURE2D_DESC ddesc = desc;
        ddesc.Format = static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT);
        ddesc.BindFlags = XG_BIND_DEPTH_STENCIL;
        ddesc.TileMode = XG_TILE_MODE_2D_THIN_64KB_Z_X;
        XG_RESOURCE_LAYOUT dlayout{};
        XG_RESOURCE_METADATA_LAYOUT dmeta{};
        EXPECT_TRUE(xg::ComputeTexture2DLayout(ddesc, dlayout));
        EXPECT_TRUE(xg::ComputeTexture2DMetadataLayout(ddesc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, dmeta));

        std::vector<uint8_t> dtiled(static_cast<size_t>(dlayout.SizeBytes), 0u);
        EXPECT_TRUE(xg::TileSurface2D(ddesc, 0u, 0u, orig_linear.data(), row_pitch, dtiled.data(), dtiled.size()));

        std::vector<uint8_t> htile(static_cast<size_t>(dmeta.Htile.SlicePitchBytes), 0xFFu);
        // Block (2,3): set HTILE zmask == 0 (fast z-clear to 1.0f = 0x3F800000); all others have zmask == 0xF!
        const uint32_t clear_block_idx = 3u * 8u + 2u;
        const uint32_t clear_hword = 0xFFFFFC00u;  // zmask == 0
        std::memcpy(htile.data() + clear_block_idx * 4u, &clear_hword, 4u);

        XgGpuSurface2DRequest req{};
        req.desc = ddesc;
        req.direction = XgGpuDirection::Untile;
        req.metadata_mode = XgGpuMetadataMode::Htile;
        req.clear_reg_value = {0x3F800000u, 0u, 0u, 0u};
        XgGpuUntileParams params{};
        EXPECT_OK(BuildXgGpuUntileParams2D(req, params));

        auto verify_htile = [&](const std::vector<uint8_t>& out, const char* tag) {
            bool ok = true;
            for (uint32_t y = 0u; y < 64u && ok; ++y) {
                for (uint32_t x = 0u; x < 64u && ok; ++x) {
                    uint32_t got = 0u;
                    std::memcpy(&got, out.data() + (y * 64u + x) * 4u, 4u);
                    if ((x >> 3u) == 2u && (y >> 3u) == 3u) {
                        if (got != 0x3F800000u) ok = false;
                    } else {
                        uint32_t orig = 0u;
                        std::memcpy(&orig, orig_linear.data() + (y * 64u + x) * 4u, 4u);
                        if (got != orig) ok = false;
                    }
                }
            }
            Check(ok, tag, "HTILE z-clear passthrough mismatch");
        };

#if defined(_WIN32)
        if (test_d3d12 && untiler.IsD3D12Ready()) {
            std::vector<uint8_t> out(linear_bytes, 0u);
            EXPECT_OK(untiler.ExecuteD3D12(params, dtiled, out, &htile));
            verify_htile(out, "D3D12 HTILE z-clear passthrough");
        }
#endif
        if (test_vulkan && untiler.IsVulkanHardwareIcd()) {
            std::vector<uint8_t> out(linear_bytes, 0u);
            EXPECT_OK(untiler.ExecuteVulkan(params, dtiled, out, &htile));
            verify_htile(out, "Vulkan HTILE z-clear passthrough");
        }
    }

    // 3) CMASK clear nibble passthrough
    {
        std::vector<uint8_t> cmask(static_cast<size_t>(meta_layout.Cmask.SlicePitchBytes), 0xFFu);
        // Clear block 5 (odd nibble of byte 2 -> high nibble = 0, low nibble stays 0xF!)
        cmask[2] = 0x0Fu;

        XgGpuSurface2DRequest req{};
        req.desc = desc;
        req.direction = XgGpuDirection::Untile;
        req.metadata_mode = XgGpuMetadataMode::Cmask;
        req.clear_reg_value = {0xAABBCCDDu, 0u, 0u, 0u};
        XgGpuUntileParams params{};
        EXPECT_OK(BuildXgGpuUntileParams2D(req, params));

        auto verify_cmask = [&](const std::vector<uint8_t>& out, const char* tag) {
            bool ok = true;
            for (uint32_t y = 0u; y < 64u && ok; ++y) {
                for (uint32_t x = 0u; x < 64u && ok; ++x) {
                    const uint32_t bidx = (y >> 3u) * 8u + (x >> 3u);
                    uint32_t got = 0u;
                    std::memcpy(&got, out.data() + (y * 64u + x) * 4u, 4u);
                    if (bidx == 5u) {
                        if (got != 0xAABBCCDDu) ok = false;
                    } else {
                        uint32_t orig = 0u;
                        std::memcpy(&orig, orig_linear.data() + (y * 64u + x) * 4u, 4u);
                        if (got != orig) ok = false;
                    }
                }
            }
            Check(ok, tag, "CMASK clear nibble passthrough mismatch");
        };

#if defined(_WIN32)
        if (test_d3d12 && untiler.IsD3D12Ready()) {
            std::vector<uint8_t> out(linear_bytes, 0u);
            EXPECT_OK(untiler.ExecuteD3D12(params, tiled, out, &cmask));
            verify_cmask(out, "D3D12 CMASK clear nibble passthrough");
        }
#endif
        if (test_vulkan && untiler.IsVulkanHardwareIcd()) {
            std::vector<uint8_t> out(linear_bytes, 0u);
            EXPECT_OK(untiler.ExecuteVulkan(params, tiled, out, &cmask));
            verify_cmask(out, "Vulkan CMASK clear nibble passthrough");
        }
    }
}

void RunUntileSuiteForBackend(XgGpuUntiler& untiler, bool test_d3d12, bool test_vulkan) {
    // 1. 64KB_S across R8 (1B atomic), R16 (2B atomic), RGBA8 (4B), RGBA16F (8B), RGBA32F (16B)
    {
        const uint32_t fmts[] = {
            static_cast<uint32_t>(DXGI_FORMAT_R8_UNORM),
            static_cast<uint32_t>(DXGI_FORMAT_R16_FLOAT),
            static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
            static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT),
            static_cast<uint32_t>(DXGI_FORMAT_R32G32B32A32_FLOAT),
        };
        const char* names[] = {"64KB_S R8", "64KB_S R16F", "64KB_S RGBA8", "64KB_S RGBA16F", "64KB_S RGBA32F"};
        for (size_t i = 0; i < 5; ++i) {
            XG_TEXTURE2D_DESC d{192u, 144u, 1u, 1u, fmts[i], 1u, XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_2D_THIN_64KB_S};
            RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, names[i], d, 0u, 0u);
        }
    }

    // 2. 64KB_D (Displayable 2D)
    {
        XG_TEXTURE2D_DESC d{160u, 136u, 1u, 1u, static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
                            1u, XG_BIND_DISPLAYABLE, XG_TILE_MODE_2D_THIN_64KB_D};
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "64KB_D RGBA8", d, 0u, 0u);
    }

    // 3. 64KB_R_X (Rotated/XOR 2D across array slice 2)
    {
        XG_TEXTURE2D_DESC d{144u, 132u, 1u, 3u, static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
                            1u, XG_BIND_RENDER_TARGET, XG_TILE_MODE_2D_THIN_64KB_R_X};
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "64KB_R_X RGBA8 slice2", d, 0u, 2u);
    }

    // 4. Mip-tail packing (256x256 RGBA8 64KB_R_X: mip 0 full-tile, mip 2 & mip 4 in mip tail!)
    {
        XG_TEXTURE2D_DESC d{256u, 256u, 6u, 2u, static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
                            1u, XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_2D_THIN_64KB_R_X};
        XG_RESOURCE_LAYOUT lay{};
        EXPECT_TRUE(xg::ComputeTexture2DLayout(d, lay));
        EXPECT_TRUE(lay.Mips[0].InMipTail == 0u);
        EXPECT_TRUE(lay.Mips[2].InMipTail == 1u);
        EXPECT_TRUE(lay.Mips[4].InMipTail == 1u);
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "MipChain mip0 full", d, 0u, 1u);
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "MipTail mip2 packed", d, 2u, 1u);
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "MipTail mip4 packed", d, 4u, 1u);
    }

    // 5. Block-compressed formats BC1 (8B), BC3 (16B), BC7 (16B)
    {
        XG_TEXTURE2D_DESC bc1{128u, 96u, 1u, 1u, static_cast<uint32_t>(DXGI_FORMAT_BC1_UNORM),
                              1u, XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_2D_THIN_64KB_S};
        XG_TEXTURE2D_DESC bc3{128u, 96u, 1u, 1u, static_cast<uint32_t>(DXGI_FORMAT_BC3_UNORM),
                              1u, XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_2D_THIN_64KB_D};
        XG_TEXTURE2D_DESC bc7{128u, 96u, 1u, 1u, static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM),
                              1u, XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_2D_THIN_64KB_R_X};
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "BC1 64KB_S", bc1, 0u, 0u);
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "BC3 64KB_D", bc3, 0u, 0u);
        RunUntileRetile2DCase(untiler, test_d3d12, test_vulkan, "BC7 64KB_R_X", bc7, 0u, 0u);
    }

    // 6. 3D thick tiling (64KB_S, 64KB_S_X, 64KB_Z, 64KB_R_X + 3D mip-tail!)
    {
        XG_TEXTURE3D_DESC v_s{40u, 36u, 20u, 4u, static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
                              XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_3D_THICK_64KB_S};
        XG_TEXTURE3D_DESC v_sx{36u, 32u, 18u, 1u, static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
                               XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_3D_THICK_64KB_S_X};
        XG_TEXTURE3D_DESC v_rx{32u, 32u, 16u, 1u, static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT),
                               XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_3D_THICK_64KB_R_X};
        RunUntileRetile3DCase(untiler, test_d3d12, test_vulkan, "3D 64KB_S mip0", v_s, 0u);
        RunUntileRetile3DCase(untiler, test_d3d12, test_vulkan, "3D 64KB_S mip2 tail", v_s, 2u);
        RunUntileRetile3DCase(untiler, test_d3d12, test_vulkan, "3D 64KB_S_X", v_sx, 0u);
        RunUntileRetile3DCase(untiler, test_d3d12, test_vulkan, "3D 64KB_R_X RGBA16F", v_rx, 0u);
    }

    // 7. DCC / HTILE / CMASK fast-clear passthrough
    TestFastClearMetadataPassthrough(untiler, test_d3d12, test_vulkan);
}

// ---------------------------------------------------------------------------
// Test 2: HS / DS / GS Stage Lifting (SM 6.5 DXIL + SPIR-V 1.3)
// ---------------------------------------------------------------------------
void TestExtendedShaderStages(const std::string& spv_val_path) {
    std::cout << "\n[2] Testing RDNA2 HS / DS / GS Stage Lifting (SM 6.5 DXIL + SPIR-V 1.3)...\n";

    // Build real RDNA2 HS workload
    Rdna2Assembler hs_asm;
    hs_asm.EmitVop2(Rdna2Opcode::V_ADD_F32, 0u, Rdna2Operand::Vgpr(0u), 1u);
    hs_asm.EmitVop2(Rdna2Opcode::V_MUL_F32, 0u, Rdna2Operand::Vgpr(0u), 2u);
    hs_asm.EmitSopp(Rdna2Opcode::S_ENDPGM, 0);

    Xs2ShaderContainer hs_xs2{};
    hs_xs2.stage = ShaderStage::Hull;
    hs_xs2.code_words = hs_asm.Words();

    Rdna2ExtendedLiftOptions lopts{};
    lopts.emit_hlsl = true;
    lopts.emit_spirv = true;
    lopts.compile_dxil_with_dxc = true;
    lopts.resign_dxil = true;
    lopts.validate_spirv_external = !spv_val_path.empty();
    lopts.spirv_val_path = spv_val_path;

    // Test HS across Tri, Quad, Isoline
    for (TessDomain dom : {TessDomain::Tri, TessDomain::Quad, TessDomain::Isoline}) {
        Rdna2HullStageContract hc{};
        hc.domain = dom;
        hc.partitioning = TessPartitioning::FractionalOdd;
        hc.output_topology = (dom == TessDomain::Isoline) ? TessOutputTopology::Line : TessOutputTopology::TriangleCw;
        hc.input_control_points = (dom == TessDomain::Quad) ? 4u : ((dom == TessDomain::Isoline) ? 2u : 3u);
        hc.output_control_points = hc.input_control_points;

        Rdna2ExtendedStageLiftResult res{};
        Status st = LiftRdna2HullShader(hs_xs2, hc, res, lopts);
        Check(st.ok(), "LiftRdna2HullShader", st.message + " " + res.dxc_diagnostics + " " + res.spirv_val_diagnostics);
        EXPECT_TRUE(res.hlsl_target == "hs_6_5");
        EXPECT_TRUE(!res.pc_dxil.empty());
        EXPECT_TRUE(res.dxbc_hash_verified);
        EXPECT_TRUE(res.lifted_alu_ops >= 2u);
        ValidateSpirvBoth(res.spirv_words, spv_val_path, "HS SPIR-V 1.3");
    }

    // Test DS across Tri, Quad, Isoline
    Rdna2Assembler ds_asm;
    ds_asm.EmitVop3(Rdna2Opcode::V_FMA_F32, 0u, Rdna2Operand::Vgpr(0u), Rdna2Operand::Vgpr(1u), Rdna2Operand::Vgpr(2u));
    ds_asm.EmitVop2(Rdna2Opcode::V_ADD_F32, 0u, Rdna2Operand::Vgpr(0u), 3u);
    ds_asm.EmitSopp(Rdna2Opcode::S_ENDPGM, 0);

    Xs2ShaderContainer ds_xs2{};
    ds_xs2.stage = ShaderStage::Domain;
    ds_xs2.code_words = ds_asm.Words();

    for (TessDomain dom : {TessDomain::Tri, TessDomain::Quad, TessDomain::Isoline}) {
        Rdna2DomainStageContract dc{};
        dc.domain = dom;
        dc.partitioning = TessPartitioning::FractionalEven;
        dc.output_topology = TessOutputTopology::TriangleCcw;
        dc.input_control_points = (dom == TessDomain::Quad) ? 4u : ((dom == TessDomain::Isoline) ? 2u : 3u);

        Rdna2ExtendedStageLiftResult res{};
        Status st = LiftRdna2DomainShader(ds_xs2, dc, res, lopts);
        Check(st.ok(), "LiftRdna2DomainShader", st.message + " " + res.dxc_diagnostics + " " + res.spirv_val_diagnostics);
        EXPECT_TRUE(res.hlsl_target == "ds_6_5");
        EXPECT_TRUE(!res.pc_dxil.empty());
        EXPECT_TRUE(res.dxbc_hash_verified);
        ValidateSpirvBoth(res.spirv_words, spv_val_path, "DS SPIR-V 1.3");
    }

    // Test GS with real S_SENDMSG GS_EMIT + GS_EMIT_CUT instructions
    Rdna2Assembler gs_asm;
    gs_asm.EmitVop2(Rdna2Opcode::V_ADD_F32, 0u, Rdna2Operand::Vgpr(0u), 1u);
    gs_asm.EmitSopp(Rdna2Opcode::S_SENDMSG, PackRdna2GsSendMsg(kRdna2SendMsgIdGs, kRdna2GsOpEmit, 0u));
    gs_asm.EmitSopp(Rdna2Opcode::S_SENDMSG, PackRdna2GsSendMsg(kRdna2SendMsgIdGs, kRdna2GsOpEmit, 0u));
    gs_asm.EmitSopp(Rdna2Opcode::S_SENDMSG, PackRdna2GsSendMsg(kRdna2SendMsgIdGs, kRdna2GsOpEmitCut, 0u));
    gs_asm.EmitSopp(Rdna2Opcode::S_SENDMSG, PackRdna2GsSendMsg(kRdna2SendMsgIdGsDone, kRdna2GsOpNop, 0u));
    gs_asm.EmitSopp(Rdna2Opcode::S_ENDPGM, 0);

    Xs2ShaderContainer gs_xs2{};
    gs_xs2.stage = ShaderStage::Geometry;
    gs_xs2.code_words = gs_asm.Words();

    Rdna2GeometryStageContract gc{};
    gc.input_primitive = GsInputPrimitive::Triangle;
    gc.output_primitive = GsOutputPrimitive::TriangleStream;
    gc.max_vertex_count = 6u;
    gc.instance_count = 2u;

    Rdna2ExtendedStageLiftResult gs_res{};
    Status gst = LiftRdna2GeometryShader(gs_xs2, gc, gs_res, lopts);
    Check(gst.ok(), "LiftRdna2GeometryShader", gst.message + " " + gs_res.dxc_diagnostics + " " + gs_res.spirv_val_diagnostics);
    EXPECT_TRUE(gs_res.hlsl_target == "gs_6_5");
    EXPECT_TRUE(!gs_res.pc_dxil.empty());
    EXPECT_TRUE(gs_res.dxbc_hash_verified);
    EXPECT_TRUE(gs_res.gs_emit_count == 3u);
    EXPECT_TRUE(gs_res.gs_cut_count == 1u);
    ValidateSpirvBoth(gs_res.spirv_words, spv_val_path, "GS SPIR-V 1.3");

    std::cout << "  [OK] HS (Tri/Quad/Isoline), DS (Tri/Quad/Isoline), GS (3x GS_EMIT + 1x GS_CUT): "
                 "DXIL SM 6.5 hash-verified + SPIR-V 1.3 validated!\n";
}

// ---------------------------------------------------------------------------
// Test 3: DXR Shader Binding Table (SBT) Mapper
// ---------------------------------------------------------------------------
void TestDxrSbtMapper() {
    std::cout << "\n[3] Testing DXR Shader Binding Table (SBT) Mapper...\n";

    DxrSbtMapper mapper;
    std::array<uint8_t, 32> rg_pc_id{};
    rg_pc_id.fill(0xA1u);
    std::array<uint8_t, 32> miss0_pc_id{};
    miss0_pc_id.fill(0xB2u);
    std::array<uint8_t, 32> hg0_pc_id{};
    hg0_pc_id.fill(0xC3u);

    mapper.RegisterPcShaderIdentifier("MyRayGen", rg_pc_id);
    mapper.RegisterPcShaderIdentifier("PrimaryMiss", miss0_pc_id);
    mapper.RegisterPcShaderIdentifier("OpaqueHitGroup", hg0_pc_id);
    mapper.RegisterHitGroup("OpaqueHitGroup", "OpaqueCHS", "OpaqueAHS", "");

    std::vector<XboxDxrShaderRecord> recs;
    {
        XboxDxrShaderRecord r{};
        r.kind = DxrShaderRecordKind::RayGen;
        r.export_name = "MyRayGen";
        r.xbox_shader_id.fill(0x11u);
        r.local_root_args = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};  // 12B -> stride aligns to 64B!
        recs.push_back(r);
    }
    {
        XboxDxrShaderRecord r{};
        r.kind = DxrShaderRecordKind::Miss;
        r.export_name = "PrimaryMiss";
        r.xbox_shader_id.fill(0x22u);
        r.local_root_args = {0xAA, 0xBB, 0xCC, 0xDD};  // 4B -> stride aligns to 64B
        recs.push_back(r);
    }
    {
        XboxDxrShaderRecord r{};
        r.kind = DxrShaderRecordKind::Miss;
        r.export_name = "ShadowMiss";
        r.xbox_shader_id.fill(0x23u);
        recs.push_back(r);
    }
    {
        XboxDxrShaderRecord ch{};
        ch.kind = DxrShaderRecordKind::ClosestHit;
        ch.export_name = "OpaqueCHS";
        ch.hit_group_name = "OpaqueHitGroup";
        ch.xbox_shader_id.fill(0x33u);
        ch.local_root_args = {0xDE, 0xAD, 0xBE, 0xEF, 0x10, 0x20, 0x30, 0x40};
        recs.push_back(ch);

        XboxDxrShaderRecord ah{};
        ah.kind = DxrShaderRecordKind::AnyHit;
        ah.export_name = "OpaqueAHS";
        ah.hit_group_name = "OpaqueHitGroup";
        recs.push_back(ah);
    }
    {
        XboxDxrShaderRecord ch{};
        ch.kind = DxrShaderRecordKind::ClosestHit;
        ch.export_name = "ProceduralCHS";
        ch.hit_group_name = "ProceduralHitGroup";
        ch.xbox_shader_id.fill(0x44u);
        recs.push_back(ch);

        XboxDxrShaderRecord is{};
        is.kind = DxrShaderRecordKind::Intersection;
        is.export_name = "SphereIS";
        is.hit_group_name = "ProceduralHitGroup";
        recs.push_back(is);
    }
    {
        XboxDxrShaderRecord c{};
        c.kind = DxrShaderRecordKind::Callable;
        c.export_name = "MaterialEvalCallable";
        c.xbox_shader_id.fill(0x55u);
        c.local_root_args.resize(40u, 0x7Fu);  // 32 + 40 = 72 -> stride aligns to 96B!
        recs.push_back(c);
    }

    const uint64_t base_va = 0x0000000100001000ull;  // 64B aligned
    MappedDxrSbt sbt{};
    EXPECT_OK(mapper.MapXboxSbt(recs, base_va, 1920u, 1080u, 1u, sbt));

    // Verify D3D12 64B table alignment & 32B stride alignment
    EXPECT_TRUE((sbt.dispatch_rays_desc.RayGenerationShaderRecord.StartAddress % 64ull) == 0ull);
    EXPECT_TRUE((sbt.dispatch_rays_desc.MissShaderTable.StartAddress % 64ull) == 0ull);
    EXPECT_TRUE((sbt.dispatch_rays_desc.HitGroupTable.StartAddress % 64ull) == 0ull);
    EXPECT_TRUE((sbt.dispatch_rays_desc.CallableShaderTable.StartAddress % 64ull) == 0ull);
    EXPECT_TRUE((sbt.raygen_stride % 32ull) == 0ull && sbt.raygen_stride == 64ull);
    EXPECT_TRUE((sbt.miss_stride % 32ull) == 0ull && sbt.miss_stride == 64ull);
    EXPECT_TRUE((sbt.hit_group_stride % 32ull) == 0ull && sbt.hit_group_stride == 64ull);
    EXPECT_TRUE((sbt.callable_stride % 32ull) == 0ull && sbt.callable_stride == 96ull);
    EXPECT_TRUE(sbt.dispatch_rays_desc.Width == 1920u && sbt.dispatch_rays_desc.Height == 1080u);

    // Verify identifier & local root args placement in buffer
    EXPECT_TRUE(std::memcmp(sbt.buffer.data() + sbt.raygen_offset, rg_pc_id.data(), 32u) == 0);
    EXPECT_TRUE(sbt.buffer[sbt.raygen_offset + 32u] == 1u && sbt.buffer[sbt.raygen_offset + 43u] == 12u);

    // Verify DXR hit-group lookup formula: idx = ray_contrib(1) + geom_stride(1) * geom_idx(0) + inst_contrib(0) = 1
    DxrHitGroupBinding hg{};
    EXPECT_OK(mapper.LookupHitGroup(sbt, 0u, 0u, 1u, 1u, hg));
    EXPECT_TRUE(hg.hit_group_name == "ProceduralHitGroup");
    EXPECT_TRUE(hg.closest_hit_export == "ProceduralCHS");
    EXPECT_TRUE(hg.intersection_export == "SphereIS");

    // Verify in-place identifier patching (simulating async RTPSO completion)
    std::array<uint8_t, 32> patched_id{};
    patched_id.fill(0xE7u);
    EXPECT_OK(mapper.PatchShaderIdentifierInPlace(sbt, "ProceduralHitGroup", patched_id));
    EXPECT_OK(mapper.LookupHitGroupByName(sbt, "ProceduralHitGroup", hg));
    EXPECT_TRUE(hg.pc_shader_id == patched_id);
    EXPECT_TRUE(std::memcmp(sbt.buffer.data() + hg.record_offset_bytes, patched_id.data(), 32u) == 0);

    std::cout << "  [OK] DxrSbtMapper: 64B table / 32B stride aligned, " << sbt.hit_groups.size()
              << " hit groups, in-place patching & DXR indexing verified!\n";
}

// ---------------------------------------------------------------------------
// Test 4: Live GPU Wave64-on-Wave32 Exact Cross-Half Execution
// ---------------------------------------------------------------------------
void VerifyWave64Parity(const Wave64OpTestOutput& ref, const Wave64OpTestOutput& got, const char* tag) {
    Check(ref.permlane16 == got.permlane16, tag, "v_permlane16_b32 mismatch across lanes 0..63");
    Check(ref.permlanex16 == got.permlanex16, tag, "v_permlanex16_b32 mismatch across lanes 0..63");
    Check(ref.readlane == got.readlane, tag, "cross-half v_readlane_b32 mismatch across lanes 0..63");
    Check(ref.wqm_lo == got.wqm_lo, tag, "s_wqm_b64 lo mismatch");
    Check(ref.wqm_hi == got.wqm_hi, tag, "s_wqm_b64 hi mismatch");
    Check(ref.mbcnt_lo == got.mbcnt_lo, tag, "v_mbcnt_lo_u32_b32 mismatch");
    Check(ref.mbcnt_total == got.mbcnt_total, tag, "v_mbcnt_hi_u32_b32 total mismatch");
}

void TestWave64OnWave32(const std::string& spv_val_path) {
    std::cout << "\n[4] Testing Exact Cross-Half Wave64-on-Wave32 Emulation (HW GPU + WARP + Vulkan)...\n";

    Wave64OpTestInput inp{};
    for (uint32_t lane = 0u; lane < 64u; ++lane) {
        inp.lane_values[lane] = 0x10000000u | (lane << 16u) | ((lane * 37u + 13u) & 0xFFFFu);
        // Deliberately exercise cross-half reads: lo lanes read hi half, hi lanes read lo half!
        if (lane == 0u) inp.readlane_targets[lane] = 47u;
        else if (lane == 55u) inp.readlane_targets[lane] = 12u;
        else if (lane == 31u) inp.readlane_targets[lane] = 32u;
        else if (lane == 32u) inp.readlane_targets[lane] = 31u;
        else if (lane == 63u) inp.readlane_targets[lane] = 0u;
        else inp.readlane_targets[lane] = (lane ^ 32u ^ 5u) & 63u;
    }
    inp.permlane_sel_lo = 0x52703614u;
    inp.permlane_sel_hi = 0x8F9EADCBu;
    inp.ballot_mask_lo = 0x80102005u;  // Sparse quads active in lo half
    inp.ballot_mask_hi = 0x10004081u;  // Sparse quads active in hi half

    const Wave64OpTestOutput ref = Wave64On32Emulator::ExecuteCpuReference(inp);
    // Spot-check cross-half reference expectations
    EXPECT_TRUE(ref.readlane[0] == inp.lane_values[47]);
    EXPECT_TRUE(ref.readlane[55] == inp.lane_values[12]);
    EXPECT_TRUE(ref.wqm_lo[0] == 0xF0F0F00Fu);
    EXPECT_TRUE(ref.wqm_hi[63] == 0xF000F0FFu);

    // Validate SPIR-V 1.3 Wave64 shader
    std::vector<uint32_t> spv;
    EXPECT_OK(Wave64On32Emulator::EmitTestComputeSpirv(spv));
    ValidateSpirvBoth(spv, spv_val_path, "Wave64-on-Wave32 SPIR-V 1.3");

#if defined(_WIN32)
    // 4A. Hardware GPU (NVIDIA GTX 1650 Ti — Wave32 native!)
    {
        Wave64OpTestOutput hw_out{};
        Wave64GpuRunInfo info{};
        Status st = Wave64On32Emulator::ExecuteTestOnD3D12(inp, hw_out, false, &info);
        if (st.ok()) {
            VerifyWave64Parity(ref, hw_out, "D3D12-HW Wave64-on-Wave32");
            std::cout << "  [OK] D3D12 HW GPU (" << info.adapter_name << ", " << info.shader_target
                      << ", hw_wave=" << info.hw_wave_lane_min << ".." << info.hw_wave_lane_max
                      << "): 448/448 dwords BIT-EXACT!\n";
        } else {
            std::cout << "  [SKIP] D3D12 HW GPU unavailable (headless CI): " << st.message << "\n";
        }
    }

    // 4B. D3D12 WARP
    {
        Wave64OpTestOutput warp_out{};
        Wave64GpuRunInfo info{};
        Status st = Wave64On32Emulator::ExecuteTestOnD3D12(inp, warp_out, true, &info);
        EXPECT_OK(st);
        VerifyWave64Parity(ref, warp_out, "D3D12-WARP Wave64-on-Wave32");
        std::cout << "  [OK] D3D12 WARP (" << info.adapter_name << ", " << info.shader_target
                  << "): 448/448 dwords BIT-EXACT!\n";
    }
#endif

    // 4C. Vulkan 1.4 HW ICD (gracefully skipped if no ICD on headless CI)
    {
        Wave64OpTestOutput vk_out{};
        Wave64GpuRunInfo info{};
        Status st = Wave64On32Emulator::ExecuteTestOnVulkan(inp, vk_out, false, &info);
        if (st.ok()) {
            VerifyWave64Parity(ref, vk_out, "Vulkan-HW Wave64-on-Wave32");
            std::cout << "  [OK] Vulkan HW ICD (" << info.adapter_name << ", subgroup="
                      << info.hw_wave_lane_min << "): 448/448 dwords BIT-EXACT!\n";
        } else {
            std::cout << "  [SKIP] Vulkan HW ICD unavailable (headless CI): " << st.message << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Test 5: Async Background PSO Pre-Compilation Worker Pool Stress Test
// ---------------------------------------------------------------------------
void TestAsyncPsoCompilePool() {
    std::cout << "\n[5] Testing AsyncPsoCompilePool (20 concurrent jobs, 4 workers, dedup + live D3D12 PSO)...\n";

#if defined(_WIN32)
    ID3D12Device* warp_dev = nullptr;
    D3D12AdapterSelectionOptions sel{};
    sel.prefer_warp = true;
    D3D12AdapterInfo ainfo{};
    Status dev_st = SelectD3D12AdapterAndCreateDevice(sel, &warp_dev, nullptr, &ainfo);
    EXPECT_OK(dev_st);
    const std::string target = (ainfo.highest_shader_model >= 0x65u && !ainfo.is_warp) ? "cs_6_5" : "cs_6_2";
    AsyncPsoCompilePool pool(4u, warp_dev);
#else
    const std::string target = "cs_6_5";
    AsyncPsoCompilePool pool(4u);
#endif

    // Create 12 unique RDNA2 XS2 compute shader blobs + 8 intentional duplicates (= 20 submissions)
    std::vector<AsyncPsoJobDesc> jobs;
    jobs.reserve(20u);
    for (uint32_t i = 0u; i < 12u; ++i) {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 0u, Rdna2Operand::InlineInt(static_cast<int32_t>(i + 1u)));
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 1u, Rdna2Operand::Vgpr(0u), 0u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM, 0);

        Xs2ShaderContainer c{};
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 64u;
        c.code_words = a.Words();

        AsyncPsoJobDesc d{};
        d.debug_name = "xs2_compute_" + std::to_string(i);
        d.shader_blob = BuildXs2Container(c);
        d.target_profile = target;
        d.priority = static_cast<PsoCompilePriority>(i % 4u);
        d.emit_spirv = true;
        d.compile_dxil = true;
        d.create_d3d12_pso = true;
        jobs.push_back(std::move(d));
    }
    // Add 8 intentional duplicates to stress both in-flight coalescing and completed cache deduplication!
    for (uint32_t i = 0u; i < 8u; ++i) {
        AsyncPsoJobDesc dup = jobs[i];
        dup.priority = PsoCompilePriority::Critical;
        jobs.push_back(std::move(dup));
    }

    std::atomic<uint32_t> callbacks_invoked{0u};
    std::vector<std::future<AsyncPsoJobResult>> futures;
    futures.reserve(jobs.size());
    for (const auto& j : jobs) {
        futures.push_back(pool.Submit(j, [&](const AsyncPsoJobResult& r) {
            if (r.status.ok()) callbacks_invoked.fetch_add(1u, std::memory_order_relaxed);
        }));
    }

    uint32_t pso_created_cnt = 0u;
    uint32_t cache_hit_cnt = 0u;
    for (size_t i = 0; i < futures.size(); ++i) {
        AsyncPsoJobResult res = futures[i].get();
        Check(res.status.ok(), "AsyncPsoCompilePool job", res.status.message);
        EXPECT_TRUE(!res.dxil.empty());
        EXPECT_TRUE(VerifyDxbcContainerHash(res.dxil.data(), res.dxil.size()));
        EXPECT_TRUE(!res.spirv.empty());
        EXPECT_OK(ValidateSpirvBinary(res.spirv));
        if (res.d3d12_pso_created) ++pso_created_cnt;
        if (res.cache_hit) ++cache_hit_cnt;
    }

    pool.WaitForIdle();
    const AsyncPsoPoolStats st = pool.Stats();
    EXPECT_TRUE(st.worker_count == 4u);
    EXPECT_TRUE(st.jobs_submitted == 20u);
    EXPECT_TRUE(st.jobs_compiled == 12u);
    EXPECT_TRUE(st.jobs_cache_hits == 8u);
    EXPECT_TRUE(st.jobs_failed == 0u);
    EXPECT_TRUE(callbacks_invoked.load() == 20u);
    EXPECT_TRUE(cache_hit_cnt == 8u);
#if defined(_WIN32)
    EXPECT_TRUE(pso_created_cnt == 20u);
#endif

    // Verify TryGetCached works immediately on completed hash
    AsyncPsoJobResult cached{};
    EXPECT_TRUE(pool.TryGetCached(AsyncPsoCompilePool::ComputeJobContentHash(jobs[0]), cached));
    EXPECT_TRUE(cached.cache_hit && !cached.dxil.empty());

    pool.Shutdown(false);
#if defined(_WIN32)
    if (warp_dev) warp_dev->Release();
#endif

    std::cout << "  [OK] AsyncPsoCompilePool: 20 submitted -> " << st.jobs_compiled << " unique compiled, "
              << st.jobs_cache_hits << " deduplicated/cache hits (" << st.jobs_dedup_coalesced
              << " coalesced in-flight), " << pso_created_cnt << " live D3D12 PSOs, 0 failures!\n";
}

}  // namespace

int main() {
    std::cout << "=== OpenScarlett Sprint 9+10: GPU Compute Untiler & Extended Shader Stages Test ===\n";
    const std::string spv_val_path = FindOptionalSpirvVal();
    std::cout << "Optional spirv-val.exe: " << (spv_val_path.empty() ? "(not found - structural check only)" : spv_val_path) << "\n";

    // [1] GPU Untiler SPIR-V + DXIL + HW D3D12 + WARP D3D12 + HW Vulkan
    std::cout << "\n[1] Testing GFX10 GPU Compute Untiler/Retiler (64KB_S/D/R_X, 3D, Mip-Tail, BC1/3/7, DCC/HTILE/CMASK)...\n";
    {
        std::vector<uint32_t> untile_spv;
        EXPECT_OK(EmitXgGpuUntileSpirv(untile_spv));
        ValidateSpirvBoth(untile_spv, spv_val_path, "XgGpuUntile SPIR-V 1.3");
        std::cout << "  [OK] XgGpuUntile SPIR-V 1.3 binary (" << untile_spv.size() << " words) validated!\n";

        std::vector<uint8_t> dxil65, dxil62;
        std::string diag;
        EXPECT_OK(CompileXgGpuUntileDxil("cs_6_5", dxil65, diag));
        EXPECT_TRUE(VerifyDxbcContainerHash(dxil65.data(), dxil65.size()));
        EXPECT_OK(CompileXgGpuUntileDxil("cs_6_2", dxil62, diag));
        EXPECT_TRUE(VerifyDxbcContainerHash(dxil62.data(), dxil62.size()));
        std::cout << "  [OK] XgGpuUntile DXIL compiled & hash-verified (cs_6_5=" << dxil65.size()
                  << " B, cs_6_2=" << dxil62.size() << " B)\n";

        // 1A. D3D12 Hardware GPU (GTX 1650 Ti) + Vulkan 1.4 HW ICD
        XgGpuUntiler hw_untiler;
        bool has_d3d12_hw = false;
#if defined(_WIN32)
        Status hw_st = hw_untiler.InitializeD3D12(false);
        if (hw_st.ok()) {
            has_d3d12_hw = true;
            std::cout << "  [LIVE] D3D12 HW GPU: " << hw_untiler.D3D12AdapterName()
                      << " (" << hw_untiler.D3D12ShaderTarget() << ")\n";
        } else {
            std::cout << "  [SKIP] D3D12 HW GPU unavailable (headless CI): " << hw_st.message << "\n";
        }
#endif
        bool has_vk_hw = false;
        Status vk_st = hw_untiler.InitializeVulkan(false);
        if (vk_st.ok() && hw_untiler.IsVulkanHardwareIcd()) {
            has_vk_hw = true;
            std::cout << "  [LIVE] Vulkan HW ICD: " << hw_untiler.VulkanDeviceName() << "\n";
        } else {
            std::cout << "  [SKIP] Vulkan HW ICD unavailable (headless CI): " << vk_st.message << "\n";
        }
        if (has_d3d12_hw || has_vk_hw) {
            RunUntileSuiteForBackend(hw_untiler, has_d3d12_hw, has_vk_hw);
            std::cout << "  [OK] Hardware GPU suite (2D/3D/MipTail/BC1-BC7/DCC/HTILE/CMASK) 100% BIT-EXACT!\n";
        }
        hw_untiler.Shutdown();

#if defined(_WIN32)
        // 1B. D3D12 WARP (cs_6_2 — always runs even on headless CI!)
        XgGpuUntiler warp_untiler;
        EXPECT_OK(warp_untiler.InitializeD3D12(true, "cs_6_2"));
        std::cout << "  [LIVE] D3D12 WARP: " << warp_untiler.D3D12AdapterName()
                  << " (" << warp_untiler.D3D12ShaderTarget() << ")\n";
        RunUntileSuiteForBackend(warp_untiler, true, false);
        std::cout << "  [OK] D3D12 WARP suite (2D/3D/MipTail/BC1-BC7/DCC/HTILE/CMASK) 100% BIT-EXACT!\n";
        warp_untiler.Shutdown();
#endif
    }

    TestExtendedShaderStages(spv_val_path);
    TestDxrSbtMapper();
    TestWave64OnWave32(spv_val_path);
    TestAsyncPsoCompilePool();

    if (g_failures > 0) {
        std::cerr << "\nFAILED with " << g_failures << " failure(s).\n";
        return 1;
    }
    std::cout << "\nALL SPRINT 9+10 GPU UNTILER & EXTENDED SHADER STAGE TESTS PASSED!\n";
    return 0;
}
