// Sprint 14 Worker 4 Test Suite:
//  1. Real RFC 1950/1951 Zlib decompression (stored, fixed Huffman, dynamic Huffman) through
//     Xbox 0x48-byte DSTORAGE_REQUEST::Options.ZlibDecompress + strict corrupt stream rejection
//  2. Real public Xbox hardware BCPack BC1-BC7 bitstream decode through Options.BcpackMode
//     (plus chained Zlib + BCPack and adversarial truncation/fuzz rejection)
//  3. Options.SwizzleMode (all 4 Xbox 64 KiB modes), Options.DestinationIsPhysicalPages &
//     Options.SourceIsPhysicalPages (kernelx.dll + TitleVAMirror), and reserved-bit E_INVALIDARG
//  4. IDStorageFactoryX2 / IDStorageQueueX2 COM QueryInterface, vtable preservation of X/X1,
//     CreateQueue1 (DisableAutoSubmit), Query1, EnqueueSignal (D3D12Fence), GetStats, SetCpuAffinity
//  5. Multi-threaded concurrent positioned OVERLAPPED / IOCP async file I/O including a > 64 GiB
//     NTFS sparse file (FSCTL_SET_SPARSE at 65 GiB offset) in std::filesystem::temp_directory_path()
//  6. MSAA (2x, 4x, 8x) tiled texture placement, sample-count alignment, planar depth-stencil,
//     FMASK/DCC/CMASK/HTILE metadata, and bijective sample-indexed tiling/untiling in xg_layout.cpp

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <dxgiformat.h>
#  include <windows.h>
#  include <winioctl.h>

#  if defined(_MSC_VER)
#    define OS_UUIDOF(T) __uuidof(T)
#  elif defined(__CRT_UUID_DECL)
#    define OS_UUIDOF(T) __mingw_uuidof<T>()
#  else
#    define OS_UUIDOF(T) IID_##T
#  endif
#endif

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/kernel/kernelx_abi.h"
#include "openscarlett/kernel/title_va_mirror.h"
#include "os_check.h"

namespace fs = std::filesystem;

// Forward declarations for Sprint 14 MSAA helpers linked from openscarlett_core (src/gpu/xg_layout.cpp).
namespace openscarlett::xg {

struct XgMsaaPlacementInfo {
    uint32_t sample_count = 1u;
    uint32_t bytes_per_sample = 0u;
    uint32_t tile_width_pixels = 0u;
    uint32_t tile_height_pixels = 0u;
    uint32_t tile_size_bytes = 0u;
    uint32_t padded_width_pixels = 0u;
    uint32_t padded_height_pixels = 0u;
    uint32_t tiles_x = 0u;
    uint32_t tiles_y = 0u;
    uint32_t plane_count = 1u;
    uint64_t slice_pitch_bytes = 0ull;
    uint64_t base_alignment_bytes = 0ull;
    uint64_t total_size_bytes = 0ull;
    uint64_t fmask_size_bytes = 0ull;
    uint64_t fmask_alignment_bytes = 0ull;
};

bool ComputeMsaaTexture2DPlacement(
    const XG_TEXTURE2D_DESC& desc,
    XgMsaaPlacementInfo& out_info) noexcept;

uint64_t ComputeMsaaElementByteOffset2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t plane,
    uint32_t slice_index,
    uint32_t element_x,
    uint32_t element_y,
    uint32_t sample_index) noexcept;

bool TileMsaaSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t plane,
    uint32_t slice_index,
    uint32_t sample_index,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept;

bool UntileMsaaSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t plane,
    uint32_t slice_index,
    uint32_t sample_index,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept;

}  // namespace openscarlett::xg

namespace {

using namespace openscarlett;

void FillPattern(std::vector<uint8_t>& buf, uint32_t seed) {
    uint32_t s = seed ^ 0x9E3779B9u;
    for (size_t i = 0u; i < buf.size(); ++i) {
        s = s * 1664525u + 1013904223u;
        buf[i] = static_cast<uint8_t>((s >> 24u) ^ ((i * 19u + seed) & 0xFFu));
    }
}

void WaitStatus(IDStorageQueueX* q, IDStorageStatusArrayX* st, uint32_t idx) {
    q->Submit();
    for (int i = 0; i < 4000 && !st->IsComplete(idx); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    OS_REQUIRE(st->IsComplete(idx));
}

// ===========================================================================
// Minimal MIT RFC 1950 (Zlib) + RFC 1951 (DEFLATE) Encoder for Testing
// Emits real stored (BTYPE=0), fixed Huffman (BTYPE=1), and dynamic Huffman
// (BTYPE=2) streams with real LZ77 back-references and Adler-32 trailers.
// ===========================================================================
namespace test_zlib {

uint32_t Adler32(const uint8_t* data, size_t len) {
    constexpr uint32_t kMod = 65521u;
    uint32_t s1 = 1u, s2 = 0u;
    for (size_t i = 0u; i < len; ++i) {
        s1 = (s1 + data[i]) % kMod;
        s2 = (s2 + s1) % kMod;
    }
    return (s2 << 16u) | s1;
}

uint32_t ReverseBits(uint32_t v, uint32_t n) {
    uint32_t r = 0u;
    for (uint32_t i = 0u; i < n; ++i) {
        r = (r << 1u) | ((v >> i) & 1u);
    }
    return r;
}

class LsbBitWriter {
public:
    void Write(uint32_t val, uint32_t bits) {
        if (bits == 0u) return;
        buf_ |= (static_cast<uint64_t>(val & ((1ull << bits) - 1ull)) << cnt_);
        cnt_ += bits;
        while (cnt_ >= 8u) {
            out_.push_back(static_cast<uint8_t>(buf_ & 0xFFu));
            buf_ >>= 8u;
            cnt_ -= 8u;
        }
    }

    void AlignZero() {
        if (cnt_ > 0u) {
            out_.push_back(static_cast<uint8_t>(buf_ & 0xFFu));
            buf_ = 0ull;
            cnt_ = 0u;
        }
    }

    void PushByte(uint8_t b) {
        AlignZero();
        out_.push_back(b);
    }

    std::vector<uint8_t> Take() {
        AlignZero();
        return std::move(out_);
    }

private:
    std::vector<uint8_t> out_;
    uint64_t buf_ = 0ull;
    uint32_t cnt_ = 0u;
};

void AppendAdler32Be(std::vector<uint8_t>& out, uint32_t adler) {
    out.push_back(static_cast<uint8_t>((adler >> 24u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((adler >> 16u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((adler >> 8u) & 0xFFu));
    out.push_back(static_cast<uint8_t>(adler & 0xFFu));
}

// BTYPE = 0 (Stored uncompressed blocks)
std::vector<uint8_t> CompressStored(const uint8_t* src, size_t len) {
    std::vector<uint8_t> out;
    out.push_back(0x78u);  // CMF: CM=8, CINFO=7 (32 KiB)
    out.push_back(0x01u);  // FLG: 0x7801 % 31 == 0
    size_t pos = 0u;
    do {
        const size_t chunk = std::min<size_t>(len - pos, 65535u);
        const bool final_blk = (pos + chunk >= len);
        out.push_back(final_blk ? 0x01u : 0x00u);  // BFINAL | BTYPE=0
        const uint16_t l = static_cast<uint16_t>(chunk);
        const uint16_t nl = static_cast<uint16_t>(~l);
        out.push_back(static_cast<uint8_t>(l & 0xFFu));
        out.push_back(static_cast<uint8_t>((l >> 8u) & 0xFFu));
        out.push_back(static_cast<uint8_t>(nl & 0xFFu));
        out.push_back(static_cast<uint8_t>((nl >> 8u) & 0xFFu));
        out.insert(out.end(), src + pos, src + pos + chunk);
        pos += chunk;
    } while (pos < len);
    AppendAdler32Be(out, Adler32(src, len));
    return out;
}

void EmitFixedLitLen(LsbBitWriter& bw, uint32_t sym) {
    if (sym <= 143u) {
        bw.Write(ReverseBits(0x30u + sym, 8u), 8u);
    } else if (sym <= 255u) {
        bw.Write(ReverseBits(0x190u + (sym - 144u), 9u), 9u);
    } else if (sym <= 279u) {
        bw.Write(ReverseBits(sym - 256u, 7u), 7u);
    } else {
        bw.Write(ReverseBits(0xC0u + (sym - 280u), 8u), 8u);
    }
}

void MapLength(uint32_t len, uint32_t& out_sym, uint32_t& out_extra_bits, uint32_t& out_extra_val) {
    static constexpr std::array<uint16_t, 29> kBase = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<uint8_t, 29> kExt = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    for (uint32_t i = 0u; i < 29u; ++i) {
        const uint32_t hi = (i == 28u) ? 258u : static_cast<uint32_t>(kBase[i + 1u] - 1u);
        if (len >= kBase[i] && len <= hi) {
            out_sym = 257u + i;
            out_extra_bits = kExt[i];
            out_extra_val = len - kBase[i];
            return;
        }
    }
}

void MapDist(uint32_t dist, uint32_t& out_sym, uint32_t& out_extra_bits, uint32_t& out_extra_val) {
    static constexpr std::array<uint16_t, 30> kBase = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::array<uint8_t, 30> kExt = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (uint32_t i = 0u; i < 30u; ++i) {
        const uint32_t hi = (i == 29u) ? 32768u : static_cast<uint32_t>(kBase[i + 1u] - 1u);
        if (dist >= kBase[i] && dist <= hi) {
            out_sym = i;
            out_extra_bits = kExt[i];
            out_extra_val = dist - kBase[i];
            return;
        }
    }
}

// BTYPE = 1 (Fixed Huffman with real LZ77 + overlapping RLE matches)
std::vector<uint8_t> CompressFixedHuffman(const uint8_t* src, size_t len) {
    LsbBitWriter bw;
    bw.PushByte(0x78u);
    bw.PushByte(0x9Cu);  // 0x789C % 31 == 0
    bw.Write(1u, 1u);    // BFINAL = 1
    bw.Write(1u, 2u);    // BTYPE = 1 (fixed Huffman)

    size_t pos = 0u;
    while (pos < len) {
        // Simple greedy match finder over last 256 bytes (exercises RLE + LZ77 distance codes).
        size_t best_len = 0u;
        size_t best_dist = 0u;
        const size_t max_d = std::min<size_t>(pos, 256u);
        const size_t max_l = std::min<size_t>(len - pos, 258u);
        for (size_t d = 1u; d <= max_d; ++d) {
            size_t m = 0u;
            while (m < max_l && src[pos - d + m] == src[pos + m]) {
                ++m;
            }
            if (m > best_len) {
                best_len = m;
                best_dist = d;
                if (best_len == max_l) break;
            }
        }
        if (best_len >= 3u) {
            uint32_t lsym = 0u, lext = 0u, lval = 0u;
            MapLength(static_cast<uint32_t>(best_len), lsym, lext, lval);
            EmitFixedLitLen(bw, lsym);
            bw.Write(lval, lext);

            uint32_t dsym = 0u, dext = 0u, dval = 0u;
            MapDist(static_cast<uint32_t>(best_dist), dsym, dext, dval);
            bw.Write(ReverseBits(dsym, 5u), 5u);
            bw.Write(dval, dext);
            pos += best_len;
        } else {
            EmitFixedLitLen(bw, src[pos++]);
        }
    }
    EmitFixedLitLen(bw, 256u);  // End-of-block
    std::vector<uint8_t> out = bw.Take();
    AppendAdler32Be(out, Adler32(src, len));
    return out;
}

// BTYPE = 2 (Dynamic Huffman with explicit code-length RLE 16/17/18 + custom canonical trees)
std::vector<uint8_t> CompressDynamicHuffman(const uint8_t* src, size_t len) {
    // Construct a complete dynamic Huffman lit/len tree covering 0..255 + 256 (EOB) + 257..258 (len 3..4):
    //   256 symbols (0..255): length 8? Wait: 256 * 2^-8 = 1.0, so to make room for 256 & 257:
    //   0..253 -> 8 bits (254 * 2^-8), 254..257 -> 9 bits (4 * 2^-9 = 2 * 2^-8) => Kraft = 256/256 = 1.0 EXACT!
    //   Distance tree: symbols 0..1 -> 1 bit each (2 * 2^-1 = 1.0 EXACT!).
    constexpr uint32_t kHlit = 258u;  // symbols 0..257 (256=EOB, 257=len 3)
    constexpr uint32_t kHdist = 2u;   // dist symbols 0..1 (dist 1..2)
    std::array<uint8_t, kHlit> ll_lens{};
    for (uint32_t i = 0u; i < 254u; ++i) ll_lens[i] = 8u;
    ll_lens[254u] = 9u;
    ll_lens[255u] = 9u;
    ll_lens[256u] = 9u;
    ll_lens[257u] = 9u;

    std::array<uint16_t, kHlit> ll_codes{};
    {
        uint32_t code = 0u;
        for (uint32_t i = 0u; i < 254u; ++i) ll_codes[i] = static_cast<uint16_t>(code++);
        code <<= 1u;
        for (uint32_t i = 254u; i < 258u; ++i) ll_codes[i] = static_cast<uint16_t>(code++);
    }

    // Code-length alphabet: we only need symbols {1, 8, 9, 16} -> 2 bits each! (4 * 2^-2 = 1.0 EXACT!)
    // Order: {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15}
    // Index:   0   1   2  3  4  5  6  7   8  9  10 11  12 13  14 15  16 17  18
    // HCLEN = 18 (indices 0..17, covers symbol 1 at index 17).
    // Canonical 2-bit codes for {1, 8, 9, 16} (sorted by symbol value!):
    //   sym 1 -> 00 (0), sym 8 -> 01 (1), sym 9 -> 10 (2), sym 16 -> 11 (3).
    LsbBitWriter bw;
    bw.PushByte(0x78u);
    bw.PushByte(0xDAu);  // 0x78DA % 31 == 0
    bw.Write(1u, 1u);    // BFINAL = 1
    bw.Write(2u, 2u);    // BTYPE = 2 (dynamic Huffman)
    bw.Write(kHlit - 257u, 5u);
    bw.Write(kHdist - 1u, 5u);
    bw.Write(18u - 4u, 4u);  // HCLEN = 18

    static constexpr std::array<uint8_t, 18> kClLens = {
        2u, 0u, 0u, 0u, 2u, 0u, 2u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 2u};
    for (uint8_t l : kClLens) bw.Write(l, 3u);

    auto emit_cl = [&](uint32_t sym) {
        uint32_t c = 0u;
        if (sym == 1u) c = 0u;
        else if (sym == 8u) c = 1u;
        else if (sym == 9u) c = 2u;
        else if (sym == 16u) c = 3u;
        bw.Write(ReverseBits(c, 2u), 2u);
    };

    // Emit 254 copies of length 8 using literal 8 + RLE symbol 16 (repeat prev 3..6 times)!
    emit_cl(8u);
    uint32_t rem8 = 253u;
    while (rem8 >= 6u) {
        emit_cl(16u);
        bw.Write(3u, 2u);  // repeat 6 times
        rem8 -= 6u;
    }
    if (rem8 >= 3u) {
        emit_cl(16u);
        bw.Write(rem8 - 3u, 2u);
        rem8 = 0u;
    }
    while (rem8-- > 0u) emit_cl(8u);

    // Emit 4 copies of length 9 (symbols 254..257) using 9 + sym16(3)
    emit_cl(9u);
    emit_cl(16u);
    bw.Write(0u, 2u);  // repeat 3 times -> total 4

    // Emit 2 copies of length 1 (distance symbols 0..1)
    emit_cl(1u);
    emit_cl(1u);

    // Encode payload using dynamic tree (including real LZ77 len=3, dist=1/2 matches!)
    size_t pos = 0u;
    while (pos < len) {
        if (pos >= 1u && pos + 3u <= len &&
            src[pos] == src[pos - 1u] &&
            src[pos + 1u] == src[pos - 1u] &&
            src[pos + 2u] == src[pos - 1u]) {
            bw.Write(ReverseBits(ll_codes[257u], ll_lens[257u]), ll_lens[257u]);  // length 3
            bw.Write(0u, 1u);  // dist code 0 -> distance 1
            pos += 3u;
        } else if (pos >= 2u && pos + 3u <= len &&
                   src[pos] == src[pos - 2u] &&
                   src[pos + 1u] == src[pos - 1u] &&
                   src[pos + 2u] == src[pos - 2u]) {
            bw.Write(ReverseBits(ll_codes[257u], ll_lens[257u]), ll_lens[257u]);  // length 3
            bw.Write(1u, 1u);  // dist code 1 -> distance 2
            pos += 3u;
        } else {
            const uint8_t b = src[pos++];
            bw.Write(ReverseBits(ll_codes[b], ll_lens[b]), ll_lens[b]);
        }
    }
    // End-of-block (256)
    bw.Write(ReverseBits(ll_codes[256u], ll_lens[256u]), ll_lens[256u]);
    std::vector<uint8_t> out = bw.Take();
    AppendAdler32Be(out, Adler32(src, len));
    return out;
}

}  // namespace test_zlib

// ===========================================================================
// Test 1: Real RFC 1950/1951 Zlib Decompression Through Xbox DSTORAGE_REQUEST
// ===========================================================================
void TestXboxZlibDecompression(IDStorageFactoryX2* factory) {
    std::printf("[Test 1] Real RFC 1950/1951 Zlib (stored + fixed + dynamic Huffman) via Xbox DSTORAGE_REQUEST...\n");

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_REALTIME;
    qd.Name = "s14_zlib_q";

    IDStorageQueueX* q = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(8u, "s14_zlib_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

    // Payload with runs + repeating patterns + varied bytes to trigger LZ77 & overlapping RLE
    std::vector<uint8_t> plain(16384u);
    for (size_t i = 0u; i < plain.size(); ++i) {
        if ((i & 63u) < 20u) {
            plain[i] = static_cast<uint8_t>('A' + ((i >> 6u) & 7u));
        } else if ((i & 63u) < 36u) {
            plain[i] = ((i & 1u) == 0u) ? 0x55u : 0xAAu;
        } else {
            plain[i] = static_cast<uint8_t>((i * 29u + 17u) & 0xFFu);
        }
    }

    const std::vector<uint8_t> z_stored = test_zlib::CompressStored(plain.data(), plain.size());
    const std::vector<uint8_t> z_fixed = test_zlib::CompressFixedHuffman(plain.data(), plain.size());
    const std::vector<uint8_t> z_dyn = test_zlib::CompressDynamicHuffman(plain.data(), plain.size());
    OS_REQUIRE(z_fixed.size() < plain.size());

    auto run_zlib_ok = [&](const std::vector<uint8_t>& comp, uint32_t slot, const char* label) {
        std::vector<uint8_t> dst(plain.size(), 0xCCu);
        DSTORAGE_REQUEST req{};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.ZlibDecompress = 1u;
        req.Source = const_cast<uint8_t*>(comp.data());
        req.SourceSize = static_cast<uint32_t>(comp.size());
        req.Destination = dst.data();
        req.DestinationSize = static_cast<uint32_t>(dst.size());
        req.Name = label;
        q->EnqueueRequest(&req);
        q->EnqueueStatus(st, slot);
        WaitStatus(q, st, slot);
        OS_REQUIRE(st->GetHResult(slot) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(dst == plain);
    };

    run_zlib_ok(z_stored, 0u, "zlib_stored");
    run_zlib_ok(z_fixed, 1u, "zlib_fixed");
    run_zlib_ok(z_dyn, 2u, "zlib_dynamic");

    // Corrupt stream rejection cases (all MUST return OS_DSTORAGE_E_DECOMPRESSION_ERROR & write nothing)
    auto run_zlib_bad = [&](const std::vector<uint8_t>& bad, const char* label) {
        std::vector<uint8_t> dst(plain.size(), 0x00u);
        DSTORAGE_REQUEST req{};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.ZlibDecompress = 1u;
        req.Source = const_cast<uint8_t*>(bad.data());
        req.SourceSize = static_cast<uint32_t>(bad.size());
        req.Destination = dst.data();
        req.DestinationSize = static_cast<uint32_t>(dst.size());
        req.Name = label;
        q->EnqueueRequest(&req);
        q->EnqueueStatus(st, 3u);
        WaitStatus(q, st, 3u);
        OS_REQUIRE(st->GetHResult(3u) == OS_DSTORAGE_E_DECOMPRESSION_ERROR);
        for (uint8_t b : dst) OS_REQUIRE(b == 0u);
        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 1u);
        OS_REQUIRE(err.FirstFailure.HResult == OS_DSTORAGE_E_DECOMPRESSION_ERROR);
    };

    // Bad header
    run_zlib_bad(std::vector<uint8_t>(256u, 0x5Au), "zlib_bad_hdr");
    // Bad Adler-32 checksum
    {
        auto bad_crc = z_fixed;
        bad_crc.back() ^= 0xFFu;
        run_zlib_bad(bad_crc, "zlib_bad_adler32");
    }
    // Truncated stream
    {
        auto trunc = z_dyn;
        trunc.resize(trunc.size() / 2u);
        run_zlib_bad(trunc, "zlib_truncated");
    }
    // Bad stored NLEN
    {
        auto bad_nlen = z_stored;
        bad_nlen[5] ^= 0x01u;
        run_zlib_bad(bad_nlen, "zlib_bad_nlen");
    }
    // FDICT preset dictionary bit set (valid checksum FCHECK, invalid FDICT)
    {
        auto fdict = z_fixed;
        fdict[0] = 0x78u;
        fdict[1] = 0xBBu;  // 0x78BB % 31 == 0, FDICT bit 5 = 1
        run_zlib_bad(fdict, "zlib_fdict");
    }

    st->Release();
    q->Close();
    q->Release();
    std::printf("  [PASS] RFC 1950/1951 Zlib: stored=%zu B, fixed=%zu B, dynamic=%zu B (all bit-exact + 5 corrupt rejected)\n",
                z_stored.size(), z_fixed.size(), z_dyn.size());
}

// ===========================================================================
// Test 2: Real Xbox Hardware BCPack BC1-BC7 Bitstream Decode & Fuzz Safety
// ===========================================================================
void TestXboxBcpackBitstreams(IDStorageFactoryX2* factory) {
    std::printf("[Test 2] Xbox Hardware BCPack BC1..BC7 bitstream decode + chained Zlib+BCPack + fuzz...\n");

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_REALTIME;
    qd.Name = "s14_bcpack_q";

    IDStorageQueueX* q = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(4u, "s14_bcpack_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

    struct FmtSpec {
        BcFormat fmt;
        DSTORAGE_BCPACK_MODE mode;
        const char* name;
    };
    const std::array<FmtSpec, 7> formats = {{
        {BcFormat::BC1, DSTORAGE_BCPACK_MODE_BC1, "BC1"},
        {BcFormat::BC2, DSTORAGE_BCPACK_MODE_BC2, "BC2"},
        {BcFormat::BC3, DSTORAGE_BCPACK_MODE_BC3, "BC3"},
        {BcFormat::BC4, DSTORAGE_BCPACK_MODE_BC4, "BC4"},
        {BcFormat::BC5, DSTORAGE_BCPACK_MODE_BC5, "BC5"},
        {BcFormat::BC6H, DSTORAGE_BCPACK_MODE_BC6H, "BC6H"},
        {BcFormat::BC7, DSTORAGE_BCPACK_MODE_BC7, "BC7"},
    }};

    std::vector<uint8_t> sample_bc7_plain;
    std::vector<uint8_t> sample_bc7_stream;

    for (const auto& f : formats) {
        std::vector<uint8_t> plain;
        OS_REQUIRE(BcGenerateSyntheticSurface(f.fmt, 128u, 128u, 0x1400u + static_cast<uint32_t>(f.fmt), plain).ok());

        std::vector<uint8_t> bitstream;
        OS_REQUIRE(BcPackHwEncodeBitstream(f.fmt, plain.data(), plain.size(), bitstream, 32u).ok());
        OS_REQUIRE(BcPackHwIsValidBitstreamHeader(f.fmt, bitstream.data(), bitstream.size()));
        OS_REQUIRE(bitstream.size() < plain.size());

        std::vector<uint8_t> dst(plain.size(), 0xAAu);
        DSTORAGE_REQUEST req{};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.BcpackMode = f.mode;
        req.Source = bitstream.data();
        req.SourceSize = static_cast<uint32_t>(bitstream.size());
        req.Destination = dst.data();
        req.DestinationSize = static_cast<uint32_t>(dst.size());
        req.Name = f.name;
        q->EnqueueRequest(&req);
        q->EnqueueStatus(st, 0u);
        WaitStatus(q, st, 0u);
        OS_REQUIRE(st->GetHResult(0u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(dst == plain);

        if (f.fmt == BcFormat::BC7) {
            sample_bc7_plain = plain;
            sample_bc7_stream = bitstream;
        }
    }

    // Chained Zlib + BCPack BC7 via Xbox DSTORAGE_REQUEST (IntermediateSize set!)
    {
        const std::vector<uint8_t> z_bc7 = test_zlib::CompressFixedHuffman(
            sample_bc7_stream.data(), sample_bc7_stream.size());
        std::vector<uint8_t> dst(sample_bc7_plain.size(), 0u);
        DSTORAGE_REQUEST req{};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.ZlibDecompress = 1u;
        req.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC7;
        req.Source = const_cast<uint8_t*>(z_bc7.data());
        req.SourceSize = static_cast<uint32_t>(z_bc7.size());
        req.Destination = dst.data();
        req.DestinationSize = static_cast<uint32_t>(dst.size());
        req.IntermediateSize = static_cast<uint32_t>(sample_bc7_stream.size());
        req.Name = "zlib_plus_bc7";
        q->EnqueueRequest(&req);
        q->EnqueueStatus(st, 1u);
        WaitStatus(q, st, 1u);
        OS_REQUIRE(st->GetHResult(1u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(dst == sample_bc7_plain);
    }

    // Truncation bounds check across a smaller BC1 stream at EVERY byte offset 0..size-1
    {
        std::vector<uint8_t> bc1_plain;
        OS_REQUIRE(BcGenerateSyntheticSurface(BcFormat::BC1, 32u, 32u, 0x99u, bc1_plain).ok());
        std::vector<uint8_t> bc1_stream;
        OS_REQUIRE(BcPackHwEncodeBitstream(BcFormat::BC1, bc1_plain.data(), bc1_plain.size(), bc1_stream, 8u).ok());

        std::vector<uint8_t> out(bc1_plain.size(), 0u);
        for (size_t cut = 0u; cut < bc1_stream.size(); ++cut) {
            size_t written = 0u;
            OS_REQUIRE(!BcPackHwDecodeBitstream(
                           BcFormat::BC1, bc1_stream.data(), cut, out.data(), out.size(), &written)
                           .ok());
        }

        // Mode mismatch (BC1 stream passed to BC7 mode) via DirectStorage -> E_DSTORAGE_DECOMPRESSION_ERROR
        std::vector<uint8_t> dst(bc1_plain.size() * 2u, 0u);
        DSTORAGE_REQUEST bad_mode{};
        bad_mode.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        bad_mode.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC7;
        bad_mode.Source = bc1_stream.data();
        bad_mode.SourceSize = static_cast<uint32_t>(bc1_stream.size());
        bad_mode.Destination = dst.data();
        bad_mode.DestinationSize = static_cast<uint32_t>(dst.size());
        q->EnqueueRequest(&bad_mode);
        q->EnqueueStatus(st, 2u);
        WaitStatus(q, st, 2u);
        OS_REQUIRE(st->GetHResult(2u) == OS_DSTORAGE_E_DECOMPRESSION_ERROR);
        for (uint8_t b : dst) OS_REQUIRE(b == 0u);
        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);

        // Missing flush bit (trailing 0x00 byte) + 128 deterministic random fuzz streams
        auto no_flush = bc1_stream;
        no_flush.push_back(0x00u);
        OS_REQUIRE(!BcPackHwDecodeBitstream(BcFormat::BC1, no_flush.data(), no_flush.size(), out.data(), out.size()).ok());

        uint32_t rng = 0x514F0221u;
        for (uint32_t t = 0u; t < 128u; ++t) {
            auto fuzzed = bc1_stream;
            rng = rng * 1664525u + 1013904223u;
            const size_t idx = rng % fuzzed.size();
            fuzzed[idx] ^= static_cast<uint8_t>(1u << ((rng >> 24u) & 7u));
            OS_REQUIRE(!BcPackHwDecodeBitstream(BcFormat::BC1, fuzzed.data(), fuzzed.size(), out.data(), out.size()).ok());
        }
    }

    st->Release();
    q->Close();
    q->Release();
    std::printf("  [PASS] BCPack BC1..BC7 hardware bitstreams + Zlib+BC7 chain + full truncation & 128-case bit-flip fuzz\n");
}

// ===========================================================================
// Test 3: SwizzleMode, DestinationIsPhysicalPages, SourceIsPhysicalPages & Reserved Bits
// ===========================================================================
void TestXboxSwizzleAndPhysicalPages(IDStorageFactoryX2* factory) {
    std::printf("[Test 3] Xbox SwizzleMode (all 4 modes), PhysicalPages (kernelx + TitleVAMirror) & reserved bits...\n");

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_REALTIME;
    qd.Name = "s14_swiz_phys_q";

    IDStorageQueueX* q = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(8u, "s14_sp_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

    // 3A. All 4 Xbox SwizzleMode values on 64 KiB tile (distinct bijective 4 KiB page permutations)
    constexpr size_t kTile64K = 65536u;
    std::vector<uint8_t> src64k(kTile64K);
    for (size_t p = 0u; p < 16u; ++p) {
        std::memset(src64k.data() + p * 4096u, static_cast<int>(0x10u + p), 4096u);
    }

    std::array<std::vector<uint8_t>, 4> swizzled_outputs;
    for (uint32_t m = 1u; m <= 4u; ++m) {
        auto& dst = swizzled_outputs[m - 1u];
        dst.assign(kTile64K, 0u);
        DSTORAGE_REQUEST req{};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.SwizzleMode = m;
        req.Source = src64k.data();
        req.SourceSize = static_cast<uint32_t>(kTile64K);
        req.Destination = dst.data();
        req.DestinationSize = static_cast<uint32_t>(kTile64K);
        q->EnqueueRequest(&req);
        q->EnqueueStatus(st, 0u);
        WaitStatus(q, st, 0u);
        OS_REQUIRE(st->GetHResult(0u) == OS_DSTORAGE_S_OK);

        // Verify exact bijective permutation of all 16 distinct 4 KiB pages!
        std::array<uint32_t, 16> seen{};
        for (size_t p = 0u; p < 16u; ++p) {
            const uint8_t tag = dst[p * 4096u];
            OS_REQUIRE(tag >= 0x10u && tag < 0x20u);
            for (size_t b = 0u; b < 4096u; ++b) OS_REQUIRE(dst[p * 4096u + b] == tag);
            ++seen[tag - 0x10u];
        }
        for (uint32_t c : seen) OS_REQUIRE(c == 1u);
        OS_REQUIRE(dst != src64k);
    }
    // All 4 swizzle modes produce distinct page orderings.
    for (size_t a = 0u; a < 4u; ++a) {
        for (size_t b = a + 1u; b < 4u; ++b) {
            OS_REQUIRE(swizzled_outputs[a] != swizzled_outputs[b]);
        }
    }

    // Invalid swizzle size (1024 B) & reserved bit -> E_INVALIDARG
    {
        std::vector<uint8_t> small_src(1024u, 0x11u), small_dst(1024u, 0u);
        DSTORAGE_REQUEST bad_swiz{};
        bad_swiz.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        bad_swiz.Options.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_4K_8K;
        bad_swiz.Source = small_src.data();
        bad_swiz.SourceSize = 1024u;
        bad_swiz.Destination = small_dst.data();
        bad_swiz.DestinationSize = 1024u;
        q->EnqueueRequest(&bad_swiz);
        q->EnqueueStatus(st, 1u);
        WaitStatus(q, st, 1u);
        OS_REQUIRE(st->GetHResult(1u) == OS_DSTORAGE_E_INVALIDARG);

        DSTORAGE_REQUEST bad_res{};
        bad_res.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        bad_res.Options.AsUINT64 |= (1ull << 24u);  // lowest reserved bit
        bad_res.Source = small_src.data();
        bad_res.SourceSize = 1024u;
        bad_res.Destination = small_dst.data();
        bad_res.DestinationSize = 1024u;
        q->EnqueueRequest(&bad_res);
        q->EnqueueStatus(st, 2u);
        WaitStatus(q, st, 2u);
        OS_REQUIRE(st->GetHResult(2u) == OS_DSTORAGE_E_INVALIDARG);
        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
    }

#if defined(_WIN32)
    // 3B. Options.DestinationIsPhysicalPages + SourceIsPhysicalPages via kernelx.dll physical pages!
    {
        uint64_t page_count = 2u;  // 128 KiB
        uint64_t pfns[2] = {};
        OS_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS | MEM_COMMIT, &page_count, pfns) == 1);
        OS_REQUIRE(page_count == 2u);

        void* alias_va = XMemMapPhysicalPages(nullptr, 2u, MEM_GRAPHICS, PAGE_READWRITE, pfns);
        OS_REQUIRE(alias_va != nullptr);
        std::memset(alias_va, 0x22, 2u * kTile64K);

        // Compress a 256x256 BC7 surface (64 KiB) and write via DestinationIsPhysicalPages at offset 4096!
        std::vector<uint8_t> bc7_plain;
        OS_REQUIRE(BcGenerateSyntheticSurface(BcFormat::BC7, 256u, 256u, 0x514Bu, bc7_plain).ok());
        OS_REQUIRE(bc7_plain.size() == kTile64K);
        std::vector<uint8_t> bc7_hw;
        OS_REQUIRE(BcPackHwEncodeBitstream(BcFormat::BC7, bc7_plain.data(), bc7_plain.size(), bc7_hw, 64u).ok());

        uintptr_t dst_pages[2] = {static_cast<uintptr_t>(pfns[0]), static_cast<uintptr_t>(pfns[1])};
        constexpr uint16_t kPageOff = 4096u;
        DSTORAGE_REQUEST req{};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC7;
        req.Options.DestinationIsPhysicalPages = 1u;
        req.DestinationPageArray = dst_pages;
        req.DestinationPageOffset = kPageOff;
        req.DestinationSize = static_cast<uint32_t>(kTile64K);
        req.Source = bc7_hw.data();
        req.SourceSize = static_cast<uint32_t>(bc7_hw.size());
        req.Name = "phys_dst_bc7";
        q->EnqueueRequest(&req);
        q->EnqueueStatus(st, 3u);
        WaitStatus(q, st, 3u);
        OS_REQUIRE(st->GetHResult(3u) == OS_DSTORAGE_S_OK);

        // Independent physical alias MUST see exact decompressed BC7 at offset 4096 (and untouched guard bytes)!
        const auto* alias_bytes = static_cast<const uint8_t*>(alias_va);
        for (size_t i = 0u; i < kPageOff; ++i) OS_REQUIRE(alias_bytes[i] == 0x22u);
        OS_REQUIRE(std::memcmp(alias_bytes + kPageOff, bc7_plain.data(), kTile64K) == 0);

        // Now read BACK via SourceIsPhysicalPages = 1!
        std::vector<uint8_t> readback(kTile64K, 0u);
        DSTORAGE_REQUEST rd_req{};
        rd_req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        rd_req.Options.SourceIsPhysicalPages = 1u;
        rd_req.SourcePageArray = dst_pages;
        rd_req.SourcePageOffset = kPageOff;
        rd_req.SourceSize = static_cast<uint32_t>(kTile64K);
        rd_req.Destination = readback.data();
        rd_req.DestinationSize = static_cast<uint32_t>(kTile64K);
        rd_req.Name = "phys_src_readback";
        q->EnqueueRequest(&rd_req);
        q->EnqueueStatus(st, 4u);
        WaitStatus(q, st, 4u);
        OS_REQUIRE(st->GetHResult(4u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(readback == bc7_plain);

        OS_REQUIRE(XMemUnmapPhysicalPages(alias_va, 2u) == 1);
        OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 2u, pfns) == 1);
    }

    // 3C. Options.DestinationIsPhysicalPages via TitleVAMirror (verifying MEM_WRITE_WATCH dirty tracking!)
    {
        TitleVAMirror mirror;
        TitleVaRegionInfo rinfo{};
        constexpr uint64_t kGuestBaseVa = 0x0000001000000000ull;  // 64 KiB aligned Title VA
        OS_REQUIRE(mirror.AllocateRegion(2u * kTile64K, kTile64K, TitleVaResourceKind::Buffer,
                                         nullptr, rinfo, kGuestBaseVa)
                       .ok());
        OS_REQUIRE(mirror.ResetDirtyTracking(kGuestBaseVa).ok());
        OS_REQUIRE(OpenScarlettDStorageSetQueueTitleVaMirror(q, &mirror) == OS_DSTORAGE_S_OK);

        std::vector<uint8_t> payload(32768u);
        FillPattern(payload, 0x7171u);
        const std::vector<uint8_t> z_payload = test_zlib::CompressFixedHuffman(payload.data(), payload.size());

        // Pass page indices (TitleVA >> 16) for pages 0 and 1!
        uintptr_t mirror_pfns[2] = {
            static_cast<uintptr_t>(kGuestBaseVa >> 16u),
            static_cast<uintptr_t>((kGuestBaseVa + kTile64K) >> 16u),
        };
        DSTORAGE_REQUEST req{};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.ZlibDecompress = 1u;
        req.Options.DestinationIsPhysicalPages = 1u;
        req.DestinationPageArray = mirror_pfns;
        req.DestinationPageOffset = 8192u;
        req.DestinationSize = static_cast<uint32_t>(payload.size());
        req.Source = const_cast<uint8_t*>(z_payload.data());
        req.SourceSize = static_cast<uint32_t>(z_payload.size());
        req.Name = "title_va_mirror_phys";
        q->EnqueueRequest(&req);
        q->EnqueueStatus(st, 5u);
        WaitStatus(q, st, 5u);
        OS_REQUIRE(st->GetHResult(5u) == OS_DSTORAGE_S_OK);

        const auto* cpu = static_cast<const uint8_t*>(rinfo.cpu_backing_ptr);
        OS_REQUIRE(std::memcmp(cpu + 8192u, payload.data(), payload.size()) == 0);

        std::vector<DirtyPageRange> dirty_ranges;
        uint64_t dirty_pages = 0ull;
        OS_REQUIRE(mirror.QueryDirtyPageRanges(kGuestBaseVa, true, dirty_ranges, &dirty_pages).ok());
        OS_REQUIRE(dirty_pages >= 8u);  // 32 KiB == 8 x 4 KiB pages marked dirty!

        OS_REQUIRE(OpenScarlettDStorageSetQueueTitleVaMirror(q, nullptr) == OS_DSTORAGE_S_OK);
    }
#endif

    st->Release();
    q->Close();
    q->Release();
    std::printf("  [PASS] All 4 Xbox SwizzleModes + kernelx & TitleVAMirror PhysicalPages + reserved-bit rejection\n");
}

// ===========================================================================
// Test 4: IDStorageFactoryX2 & IDStorageQueueX2 COM Hierarchy & Methods
// ===========================================================================
void TestDirectStorageX2Interfaces() {
    std::printf("[Test 4] IDStorageFactoryX2 / IDStorageQueueX2 QueryInterface, vtable preservation & X2 methods...\n");

    IDStorageFactoryX* fx = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&fx)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(fx != nullptr);

    IDStorageFactoryX1* fx1 = nullptr;
    IDStorageFactoryX2* fx2 = nullptr;
    OS_REQUIRE(fx->QueryInterface(&IID_IDStorageFactoryX1, reinterpret_cast<void**>(&fx1)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(fx1->QueryInterface(&IID_IDStorageFactoryX2, reinterpret_cast<void**>(&fx2)) == OS_DSTORAGE_S_OK);
    // Single-inheritance COM identity: X, X1, X2 pointers match identically (vtable slots 0..7 preserved!).
    OS_REQUIRE(static_cast<void*>(fx) == static_cast<void*>(fx1));
    OS_REQUIRE(static_cast<void*>(fx) == static_cast<void*>(fx2));

    // X1 methods: SetCpuAffinity & GetStats
    OS_REQUIRE(fx1->SetCpuAffinity(0ull) == OS_DSTORAGE_E_INVALIDARG);
    OS_REQUIRE(fx1->SetCpuAffinity(0x3FFFull) == OS_DSTORAGE_S_OK);
    DSTORAGE_STATS stats{};
    OS_REQUIRE(fx1->GetStats(&stats) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(stats.TotalZlibDecompressions >= 3u);
    OS_REQUIRE(stats.TotalBcpackDecompressions >= 7u);
    OS_REQUIRE(stats.TotalSwizzleTransforms >= 4u);

    // X2 method: CreateQueue1 with DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT
    DSTORAGE_QUEUE_DESC1 qd1{};
    qd1.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd1.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;  // 128 (auto-submit threshold would be 64)
    qd1.Priority = DSTORAGE_PRIORITY_HIGH;
    qd1.Options = DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT;
    qd1.Name = "s14_x2_no_autosubmit";

    IDStorageQueueX2* q2 = nullptr;
    OS_REQUIRE(fx2->CreateQueue1(&qd1, &IID_IDStorageQueueX2, reinterpret_cast<void**>(&q2)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(q2 != nullptr);

    // Verify QueryInterface down to X and X1 preserves exact pointer & vtable
    IDStorageQueueX* q0 = nullptr;
    IDStorageQueueX1* q1 = nullptr;
    OS_REQUIRE(q2->QueryInterface(&IID_IDStorageQueueX, reinterpret_cast<void**>(&q0)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(q2->QueryInterface(&IID_IDStorageQueueX1, reinterpret_cast<void**>(&q1)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(static_cast<void*>(q0) == static_cast<void*>(q2));
    OS_REQUIRE(static_cast<void*>(q1) == static_cast<void*>(q2));

    OS_REQUIRE(q2->GetAutoSubmitDisabled());
    DSTORAGE_QUEUE_INFO1 qi1{};
    q2->Query1(&qi1);
    OS_REQUIRE(qi1.AutoSubmitDisabled == 1u);
    OS_REQUIRE(qi1.RequestCountUntilAutoSubmit == 0u);
    OS_REQUIRE(qi1.Desc.Options == DSTORAGE_QUEUE_OPTIONS_DISABLE_AUTO_SUBMIT);

    // Enqueue 80 requests (> 64 half-full!) WITHOUT Submit(): MUST stay staged while auto-submit disabled!
    constexpr uint32_t kCount = 80u;
    std::vector<std::array<uint8_t, 64>> srcs(kCount);
    std::vector<std::array<uint8_t, 64>> dsts(kCount);
    for (uint32_t i = 0u; i < kCount; ++i) {
        for (size_t k = 0u; k < 64u; ++k) srcs[i][k] = static_cast<uint8_t>((i * 11u + k * 3u) & 0xFFu);
        dsts[i].fill(0u);
        DSTORAGE_REQUEST r{};
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r.Source = srcs[i].data();
        r.SourceSize = 64u;
        r.Destination = dsts[i].data();
        r.DestinationSize = 64u;
        q2->EnqueueRequest(&r);
    }

    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(fx2->CreateStatusArray(1u, "x2_st", &IID_IDStorageStatusArrayX,
                                      reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
    q2->EnqueueStatus(st, 0u);

#if defined(_WIN32)
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OS_REQUIRE(ev != nullptr);
    q2->EnqueueSetEvent(ev);

    // Optional live D3D12 WARP fence test for IDStorageQueueX1::EnqueueSignal!
    ID3D12Device* warp_dev = nullptr;
    ID3D12Fence* fence = nullptr;
    {
        D3D12AdapterSelectionOptions opts{};
        opts.prefer_warp = true;
        opts.allow_warp_fallback = true;
        if (SelectD3D12AdapterAndCreateDevice(opts, &warp_dev, nullptr, nullptr).ok() && warp_dev) {
            if (SUCCEEDED(warp_dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_UUIDOF(ID3D12Fence),
                                                reinterpret_cast<void**>(&fence))) &&
                fence) {
                q1->EnqueueSignal(fence, 42ull);
            }
        }
    }

    // Must NOT have auto-submitted!
    OS_REQUIRE(WaitForSingleObject(ev, 40u) == WAIT_TIMEOUT);
    OS_REQUIRE(!st->IsComplete(0u));

    // Now explicit Submit() executes all 80 requests + status + event + D3D12 fence signal!
    q2->Submit();
    OS_REQUIRE(WaitForSingleObject(ev, 10000u) == WAIT_OBJECT_0);
    OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
    for (uint32_t i = 0u; i < kCount; ++i) OS_REQUIRE(dsts[i] == srcs[i]);

    if (fence) {
        for (int i = 0; i < 1000 && fence->GetCompletedValue() < 42ull; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        OS_REQUIRE(fence->GetCompletedValue() == 42ull);
        fence->Release();
    }
    if (warp_dev) warp_dev->Release();
    CloseHandle(ev);
#else
    q2->Submit();
    for (int i = 0; i < 2000 && !st->IsComplete(0u); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
#endif

    q2->Query1(&qi1);
    OS_REQUIRE(qi1.CompletedRequestCount >= kCount);
    OS_REQUIRE(q2->SetAutoSubmitDisabled(false) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(!q2->GetAutoSubmitDisabled());

    st->Release();
    q2->Close();
    q0->Release();
    q1->Release();
    q2->Release();
    fx2->Release();
    fx1->Release();
    fx->Release();
    std::printf("  [PASS] IDStorageFactoryX2 / IDStorageQueueX2 QI, vtables, DisableAutoSubmit & EnqueueSignal\n");
}

// ===========================================================================
// Test 5: Concurrent Positioned OVERLAPPED / IOCP Async File I/O (> 64 GiB Sparse File)
// ===========================================================================
void TestConcurrentIocpAndOver64GiBSparseFile(IDStorageFactoryX2* factory) {
    std::printf("[Test 5] Concurrent OVERLAPPED / IOCP async file I/O + > 64 GiB NTFS sparse file...\n");
#if defined(_WIN32)
    std::error_code ec;
    const fs::path tmp_dir = fs::temp_directory_path(ec);
    OS_REQUIRE(!ec && !tmp_dir.empty());
    const fs::path sparse_path =
        tmp_dir / ("openscarlett_s14_sparse_65gib_" +
                   std::to_string(GetCurrentProcessId()) + ".bin");

    struct FileCleanupGuard {
        fs::path path;
        ~FileCleanupGuard() {
            std::error_code rem_ec;
            fs::remove(path, rem_ec);
        }
    } cleanup{sparse_path};

    HANDLE h_create = CreateFileW(
        sparse_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    OS_REQUIRE(h_create != INVALID_HANDLE_VALUE);

    DWORD bytes_ret = 0u;
    const BOOL sparse_ok = DeviceIoControl(
        h_create, FSCTL_SET_SPARSE, nullptr, 0u, nullptr, 0u, &bytes_ret, nullptr);

    constexpr uint64_t k1GiB = 1024ull * 1024ull * 1024ull;
    constexpr uint32_t kChunk = 65536u;  // 64 KiB

    // Offsets spanning <4GiB, >4GiB (5 GiB), and >64GiB (65 GiB & 65 GiB + 512 KiB)!
    std::vector<uint64_t> offsets;
    if (sparse_ok) {
        offsets = {
            0ull,
            16ull * 1024ull * 1024ull,
            5ull * k1GiB + 4096ull,
            65ull * k1GiB,
            65ull * k1GiB + 512ull * 1024ull,
        };
    } else {
        offsets = {0ull, 65536ull, 131072ull, 262144ull, 524288ull};
    }

    std::vector<std::vector<uint8_t>> expected(offsets.size(), std::vector<uint8_t>(kChunk));
    for (size_t i = 0u; i < offsets.size(); ++i) {
        FillPattern(expected[i], 0x6500u + static_cast<uint32_t>(i));
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offsets[i]);
        OS_REQUIRE(SetFilePointerEx(h_create, pos, nullptr, FILE_BEGIN) != 0);
        DWORD written = 0u;
        OS_REQUIRE(WriteFile(h_create, expected[i].data(), kChunk, &written, nullptr) != 0);
        OS_REQUIRE(written == kChunk);
    }
    OS_REQUIRE(FlushFileBuffers(h_create) != 0);
    CloseHandle(h_create);

    IDStorageFileX* file = nullptr;
    const std::wstring wpath = sparse_path.wstring();
    OS_REQUIRE(factory->OpenFile(wpath.c_str(), &IID_IDStorageFileX, reinterpret_cast<void**>(&file)) == OS_DSTORAGE_S_OK);

    DSTORAGE_FILE_INFO finfo{};
    OS_REQUIRE(file->GetFileInformation(&finfo) == OS_DSTORAGE_S_OK);
    const uint64_t min_expected_size = offsets.back() + kChunk;
    OS_REQUIRE(finfo.FileSize >= min_expected_size);
    if (sparse_ok) {
        OS_REQUIRE(finfo.FileSize > 64ull * k1GiB);
    }

    // Issue 40 concurrent positioned reads across 2 parallel queues sharing the SAME IDStorageFileX handle!
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_REALTIME;
    qd.Name = "s14_iocp_q0";

    IDStorageQueueX* q0 = nullptr;
    IDStorageQueueX* q1 = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q0)) == OS_DSTORAGE_S_OK);
    qd.Name = "s14_iocp_q1";
    OS_REQUIRE(factory->CreateQueue(&qd, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q1)) == OS_DSTORAGE_S_OK);

    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(2u, "iocp_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

    constexpr size_t kReadsPerQueue = 20u;
    std::vector<std::vector<uint8_t>> dst0(kReadsPerQueue, std::vector<uint8_t>(kChunk, 0u));
    std::vector<std::vector<uint8_t>> dst1(kReadsPerQueue, std::vector<uint8_t>(kChunk, 0u));

    for (size_t i = 0u; i < kReadsPerQueue; ++i) {
        const size_t oi = i % offsets.size();
        DSTORAGE_REQUEST r0{};
        r0.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r0.File = file;
        r0.FileOffset = offsets[oi];
        r0.SourceSize = kChunk;
        r0.Destination = dst0[i].data();
        r0.DestinationSize = kChunk;
        q0->EnqueueRequest(&r0);

        const size_t oi1 = (i + 2u) % offsets.size();
        DSTORAGE_REQUEST r1{};
        r1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r1.File = file;
        r1.FileOffset = offsets[oi1];
        r1.SourceSize = kChunk;
        r1.Destination = dst1[i].data();
        r1.DestinationSize = kChunk;
        q1->EnqueueRequest(&r1);
    }
    q0->EnqueueStatus(st, 0u);
    q1->EnqueueStatus(st, 1u);

    // Submit both queues simultaneously to exercise true concurrent OVERLAPPED/IOCP I/O!
    q0->Submit();
    q1->Submit();
    for (int i = 0; i < 5000 && (!st->IsComplete(0u) || !st->IsComplete(1u)); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(st->IsComplete(1u) && st->GetHResult(1u) == OS_DSTORAGE_S_OK);

    for (size_t i = 0u; i < kReadsPerQueue; ++i) {
        OS_REQUIRE(dst0[i] == expected[i % offsets.size()]);
        OS_REQUIRE(dst1[i] == expected[(i + 2u) % offsets.size()]);
    }

    DSTORAGE_STATS stats{};
    OS_REQUIRE(factory->GetStats(&stats) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(stats.TotalIocpAsyncReads >= 40u);
    OS_REQUIRE(stats.TotalBytesRead >= 40ull * kChunk);
    if (sparse_ok) {
        OS_REQUIRE(stats.TotalOver64GiBReads >= 8u);
    }

    st->Release();
    q0->Close(); q0->Release();
    q1->Close(); q1->Release();
    file->Close(); file->Release();

    std::printf("  [PASS] IOCP async overlapped reads=%llu, max_concurrent=%llu, >64GiB_reads=%llu (file=%llu GiB)\n",
                static_cast<unsigned long long>(stats.TotalIocpAsyncReads),
                static_cast<unsigned long long>(stats.MaxConcurrentIocpReads),
                static_cast<unsigned long long>(stats.TotalOver64GiBReads),
                static_cast<unsigned long long>(finfo.FileSize / k1GiB));
#else
    (void)factory;
    std::printf("  [SKIP] Win32 IOCP sparse file test skipped on non-Windows\n");
#endif
}

// ===========================================================================
// Test 6: MSAA (2x, 4x, 8x) Tiled Texture Placement & Bijective Sample Addressing
// ===========================================================================
void TestXgMsaaTiledLayouts() {
    std::printf("[Test 6] xg_layout MSAA (2x, 4x, 8x) tiled placement, alignment, metadata & bijective samples...\n");

    const std::array<uint32_t, 4> sample_counts = {1u, 2u, 4u, 8u};
    const std::array<uint32_t, 5> bpe_formats = {
        DXGI_FORMAT_R8_UNORM,             // 1 B
        DXGI_FORMAT_R16_FLOAT,            // 2 B
        DXGI_FORMAT_R8G8B8A8_UNORM,       // 4 B
        DXGI_FORMAT_R16G16B16A16_FLOAT,   // 8 B
        DXGI_FORMAT_R32G32B32A32_FLOAT,   // 16 B
    };
    const std::array<XG_TILE_MODE, 3> modes = {
        XG_TILE_MODE_2D_THIN_256B_S,
        XG_TILE_MODE_2D_THIN_4KB_S,
        XG_TILE_MODE_2D_THIN_64KB_R_X,
    };

    // 6A. Tile geometry invariant: tile_w * tile_h * sample_count * bpe == tile_size_bytes
    for (XG_TILE_MODE mode : modes) {
        for (uint32_t fmt : bpe_formats) {
            uint64_t prev_size = 0ull;
            for (uint32_t sc : sample_counts) {
                XG_TEXTURE2D_DESC desc{};
                desc.Width = 256u;
                desc.Height = 256u;
                desc.MipLevels = 1u;
                desc.ArraySize = 1u;
                desc.Format = fmt;
                desc.SampleCount = sc;
                desc.BindFlags = XG_BIND_RENDER_TARGET | XG_BIND_SHADER_RESOURCE;
                desc.TileMode = mode;

                xg::XgMsaaPlacementInfo info{};
                OS_REQUIRE(xg::ComputeMsaaTexture2DPlacement(desc, info));
                OS_REQUIRE(info.sample_count == sc);
                OS_REQUIRE(static_cast<uint64_t>(info.tile_width_pixels) *
                               info.tile_height_pixels * sc * info.bytes_per_sample ==
                           info.tile_size_bytes);
                OS_REQUIRE(info.total_size_bytes % info.base_alignment_bytes == 0ull);
                if (prev_size > 0ull) {
                    // 256x256 is a multiple of every tile dimension -> exact 2x scaling per sample doubling!
                    OS_REQUIRE(info.total_size_bytes == prev_size * 2ull);
                }
                if (sc > 1u) {
                    OS_REQUIRE(info.fmask_size_bytes > 0ull);
                    OS_REQUIRE(info.fmask_alignment_bytes == info.tile_size_bytes);
                }
                prev_size = info.total_size_bytes;
            }
        }
    }

    // 6B. Exact 256x256 RGBA8 64KB_R_X size scaling: 1x=256KiB, 2x=512KiB, 4x=1MiB, 8x=2MiB
    for (uint32_t sc : sample_counts) {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = 256u;
        desc.Height = 256u;
        desc.MipLevels = 1u;
        desc.ArraySize = 1u;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleCount = sc;
        desc.BindFlags = XG_BIND_RENDER_TARGET | XG_BIND_SHADER_RESOURCE;
        desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_R_X;

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(desc, layout));
        OS_REQUIRE(layout.SizeBytes == 256ull * 1024ull * sc);

        XG_RESOURCE_METADATA_LAYOUT meta{};
        OS_REQUIRE(xg::ComputeTexture2DMetadataLayout(desc, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, meta));
        OS_REQUIRE(meta.BaseSurfaceSizeBytes == layout.SizeBytes);
        OS_REQUIRE(meta.Dcc.Enabled == 1u && meta.Cmask.Enabled == 1u);
        OS_REQUIRE(meta.Dcc.OffsetBytes >= meta.BaseSurfaceSizeBytes);
        OS_REQUIRE(meta.Cmask.OffsetBytes >= meta.Dcc.OffsetBytes + meta.Dcc.SizeBytes);
    }

    // 6C. Planar Depth-Stencil MSAA (D32_FLOAT_S8X24_UINT, 4x MSAA, 256x256, 64KB_Z_X)
    {
        XG_TEXTURE2D_DESC ds{};
        ds.Width = 256u;
        ds.Height = 256u;
        ds.MipLevels = 1u;
        ds.ArraySize = 1u;
        ds.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        ds.SampleCount = 4u;
        ds.BindFlags = XG_BIND_DEPTH_STENCIL;
        ds.TileMode = XG_TILE_MODE_2D_THIN_64KB_Z_X;

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(ds, layout));
        OS_REQUIRE(layout.PlaneCount == 2u);
        OS_REQUIRE(layout.PlaneSizes[0] == 1024ull * 1024ull);  // 256x256 * 4B * 4x
        OS_REQUIRE(layout.PlaneSizes[1] == 256ull * 1024ull);   // 256x256 * 1B * 4x
        OS_REQUIRE(layout.PlaneOffsets[1] >= layout.PlaneSizes[0]);

        XG_RESOURCE_METADATA_LAYOUT meta{};
        OS_REQUIRE(xg::ComputeTexture2DMetadataLayout(ds, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, meta));
        OS_REQUIRE(meta.Htile.Enabled == 1u && meta.Htile.OffsetBytes >= layout.SizeBytes);
    }

    // 6D. Invalid MSAA configurations rejected cleanly (+ MipLevels > 1 clamped to 1)
    {
        XG_RESOURCE_LAYOUT l{};
        XG_TEXTURE2D_DESC bad_sc{128u, 128u, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, 3u, XG_BIND_RENDER_TARGET, XG_TILE_MODE_2D_THIN_64KB_R_X};
        OS_REQUIRE(!xg::ComputeTexture2DLayout(bad_sc, l));

        XG_TEXTURE2D_DESC clamp_mips{128u, 128u, 4u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, 4u, XG_BIND_RENDER_TARGET, XG_TILE_MODE_2D_THIN_64KB_R_X};
        OS_REQUIRE(xg::ComputeTexture2DLayout(clamp_mips, l));
        OS_REQUIRE(l.MipLevels == 1u);

        XG_TEXTURE2D_DESC bad_lin{128u, 128u, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, 4u, XG_BIND_RENDER_TARGET, XG_TILE_MODE_LINEAR};
        OS_REQUIRE(!xg::ComputeTexture2DLayout(bad_lin, l));

        XG_TEXTURE2D_DESC bad_bc{128u, 128u, 1u, 1u, DXGI_FORMAT_BC7_UNORM, 4u, XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_2D_THIN_64KB_S};
        OS_REQUIRE(!xg::ComputeTexture2DLayout(bad_bc, l));
    }

    // 6E. Bijective sample-indexed tiling & untiling round-trip across ALL samples (2x, 4x, 8x)
    for (uint32_t sc : {2u, 4u, 8u}) {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = 140u;   // NPOT multi-tile
        desc.Height = 96u;
        desc.MipLevels = 1u;
        desc.ArraySize = 2u;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleCount = sc;
        desc.BindFlags = XG_BIND_RENDER_TARGET;
        desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_R_X;

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(xg::ComputeTexture2DLayout(desc, layout));
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xEEu);

        const uint32_t row_pitch = desc.Width * 4u;
        std::vector<std::vector<uint8_t>> sample_srcs(static_cast<size_t>(desc.ArraySize) * sc);
        for (uint32_t sl = 0u; sl < desc.ArraySize; ++sl) {
            for (uint32_t s = 0u; s < sc; ++s) {
                auto& buf = sample_srcs[static_cast<size_t>(sl) * sc + s];
                buf.resize(static_cast<size_t>(row_pitch) * desc.Height);
                FillPattern(buf, 0xA500u + sc * 100u + sl * 16u + s);
                OS_REQUIRE(xg::TileMsaaSurface2D(desc, 0u, sl, s, buf.data(), row_pitch, tiled.data(), tiled.size()));
            }
        }
        // Verify every single (slice, sample) untiles back 100% bit-exact with zero sample aliasing!
        for (uint32_t sl = 0u; sl < desc.ArraySize; ++sl) {
            for (uint32_t s = 0u; s < sc; ++s) {
                const auto& exp = sample_srcs[static_cast<size_t>(sl) * sc + s];
                std::vector<uint8_t> out(exp.size(), 0u);
                OS_REQUIRE(xg::UntileMsaaSurface2D(desc, 0u, sl, s, tiled.data(), tiled.size(), out.data(), row_pitch));
                OS_REQUIRE(out == exp);
            }
        }
    }

    std::printf("  [PASS] MSAA 2x/4x/8x tile placement, planar DS, FMASK/DCC/CMASK/HTILE & bijective sample round-trips\n");
}

}  // namespace

int main() {
    IDStorageFactoryX2* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX2, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(factory != nullptr);

    TestXboxZlibDecompression(factory);
    TestXboxBcpackBitstreams(factory);
    TestXboxSwizzleAndPhysicalPages(factory);
    TestDirectStorageX2Interfaces();
    TestConcurrentIocpAndOver64GiBSparseFile(factory);
    TestXgMsaaTiledLayouts();

    factory->Release();
    std::printf("\nALL SPRINT 14 DIRECTSTORAGE X2 + IOCP + BCPACK + MSAA TESTS PASSED!\n");
    return 0;
}
