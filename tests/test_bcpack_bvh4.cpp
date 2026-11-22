#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "openscarlett/gpu/bvh4_decoder.h"
#include "openscarlett/gpu/rt_bvh.h"
#include "openscarlett/io/bcpack.h"
#include "os_check.h"

namespace {

using namespace openscarlett;

void WriteLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void WriteLe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
}

// Helper to craft a single-plane-payload-corrupted valid-CRC BCPack stream for entropy decoder tests.
std::vector<uint8_t> CraftBc1StreamWithCustomPlane(
    uint32_t width,
    uint32_t height,
    uint32_t target_plane,
    BcPlaneTransform transform,
    BcPlaneCodec codec,
    const std::vector<uint8_t>& custom_payload) {
    std::vector<uint8_t> surf;
    OS_REQUIRE(BcGenerateSyntheticSurface(BcFormat::BC1, width, height, 7u, surf).ok());
    std::vector<uint8_t> base;
    BcPackCompressOptions opts{};
    opts.force_codec = true;
    opts.forced_codec = BcPlaneCodec::Raw;
    opts.force_transform = true;
    opts.forced_transform = BcPlaneTransform::None;
    OS_REQUIRE(BcPackCompressWithOptions(BcFormat::BC1, width, height, surf.data(), surf.size(), opts, base).ok());

    BcPackHeader hdr{};
    OS_REQUIRE(BcPackParseHeader(base.data(), base.size(), hdr).ok());
    OS_REQUIRE(target_plane < hdr.plane_count);

    std::vector<std::vector<uint8_t>> payloads(hdr.plane_count);
    for (uint32_t p = 0; p < hdr.plane_count; ++p) {
        if (p == target_plane) {
            payloads[p] = custom_payload;
        } else {
            const auto& d = hdr.planes[p];
            payloads[p].assign(base.begin() + static_cast<ptrdiff_t>(d.data_offset),
                               base.begin() + static_cast<ptrdiff_t>(d.data_offset + d.compressed_size));
        }
    }

    const size_t table_end = 36u + static_cast<size_t>(hdr.plane_count) * 20u;
    std::vector<uint8_t> out(base.begin(), base.begin() + static_cast<ptrdiff_t>(table_end));
    uint32_t cur_off = static_cast<uint32_t>(table_end);
    for (uint32_t p = 0; p < hdr.plane_count; ++p) {
        uint8_t* desc = out.data() + 36u + static_cast<size_t>(p) * 20u;
        if (p == target_plane) {
            desc[1] = static_cast<uint8_t>(transform);
            desc[2] = static_cast<uint8_t>(codec);
        }
        WriteLe32(desc + 8u, static_cast<uint32_t>(payloads[p].size()));
        WriteLe32(desc + 12u, cur_off);
        WriteLe32(desc + 16u, BcPackCrc32(payloads[p].data(), payloads[p].size()));
        cur_off += static_cast<uint32_t>(payloads[p].size());
    }
    for (uint32_t p = 0; p < hdr.plane_count; ++p) {
        out.insert(out.end(), payloads[p].begin(), payloads[p].end());
    }
    return out;
}

void TestBcPackAllFormatsRoundTrip() {
    const std::array<BcFormat, 7> formats = {
        BcFormat::BC1, BcFormat::BC2, BcFormat::BC3,
        BcFormat::BC4, BcFormat::BC5, BcFormat::BC6H, BcFormat::BC7,
    };

    for (BcFormat fmt : formats) {
        std::vector<uint8_t> src;
        OS_REQUIRE(BcGenerateSyntheticSurface(fmt, 64u, 64u, static_cast<uint32_t>(fmt) * 19u + 3u, src).ok());
        OS_REQUIRE(src.size() == BcCalculateSurfaceSize(fmt, 64u, 64u));

        std::vector<uint8_t> packed;
        BcPackStats stats{};
        OS_REQUIRE(BcPackCompress(fmt, 64u, 64u, src.data(), src.size(), packed, &stats).ok());
        OS_REQUIRE(stats.original_size == src.size());
        OS_REQUIRE(stats.compressed_size == packed.size());
        OS_REQUIRE(stats.compression_ratio < 1.0);

        std::vector<uint8_t> unpacked;
        BcPackHeader hdr{};
        OS_REQUIRE(BcPackDecompress(packed.data(), packed.size(), unpacked, &hdr).ok());
        OS_REQUIRE(hdr.format == fmt);
        OS_REQUIRE(hdr.width == 64u && hdr.height == 64u);
        OS_REQUIRE(hdr.block_count == 256u);
        OS_REQUIRE(unpacked == src);

        // BcPackDecompressInto exact buffer + undersized/null buffer rejection
        std::vector<uint8_t> fixed_buf(src.size(), 0xCCu);
        size_t written = 0;
        OS_REQUIRE(BcPackDecompressInto(packed.data(), packed.size(),
                                        fixed_buf.data(), fixed_buf.size(), &written).ok());
        OS_REQUIRE(written == src.size());
        OS_REQUIRE(fixed_buf == src);

        OS_REQUIRE(!BcPackDecompressInto(packed.data(), packed.size(),
                                         fixed_buf.data(), src.size() - 1u, &written).ok());
        OS_REQUIRE(written == 0u);
        OS_REQUIRE(!BcPackDecompressInto(packed.data(), packed.size(),
                                         nullptr, src.size(), &written).ok());
    }
    std::printf("[PASS] BCPack BC1-BC7 lossless round-trip & bounded buffer checks\n");
}

void TestBcPackBc1SlidingWindowAndIndexTableEdgeCases() {
    // 1. BC1 endpoint ring-buffer wraparound (> 64 distinct RGB565 endpoints) + channel wraparound + repeats
    {
        constexpr uint32_t w = 64u, h = 32u;  // 16 x 8 = 128 blocks (> 64 ring capacity)
        const size_t block_count = (w / 4u) * (h / 4u);
        std::vector<uint8_t> bc1(block_count * 8u, 0u);

        auto pack565 = [](uint32_t r, uint32_t g, uint32_t b) -> uint16_t {
            return static_cast<uint16_t>(((r & 31u) << 11u) | ((g & 63u) << 5u) | (b & 31u));
        };

        for (size_t b = 0; b < block_count; ++b) {
            uint8_t* blk = bc1.data() + b * 8u;
            uint16_t c0 = 0;
            uint16_t c1 = 0;
            if (b < 72u) {
                // 72 distinct endpoints forcing full 64-entry ring buffer wraparound
                c0 = pack565((static_cast<uint32_t>(b) * 3u) & 31u,
                             (static_cast<uint32_t>(b) * 5u) & 63u,
                             (static_cast<uint32_t>(b) * 7u) & 31u);
                // Small +/-1..3 deltas crossing 0/31 and 0/63 boundaries
                c1 = pack565((31u + static_cast<uint32_t>(b)) & 31u,
                             (63u - static_cast<uint32_t>(b)) & 63u,
                             (30u + static_cast<uint32_t>(b)) & 31u);
            } else if (b < 100u) {
                // Repeat-most-recent runs & recent ring references
                c0 = pack565(31u, 63u, 31u);
                c1 = pack565(0u, 0u, 0u);
            } else {
                // Reference entries from earlier in the ring
                c0 = pack565(((static_cast<uint32_t>(b) - 10u) * 3u) & 31u,
                             ((static_cast<uint32_t>(b) - 10u) * 5u) & 63u,
                             ((static_cast<uint32_t>(b) - 10u) * 7u) & 31u);
                c1 = pack565(0u, 63u, 0u);
            }
            WriteLe16(blk + 0u, c0);
            WriteLe16(blk + 2u, c1);

            // Mix of repeated palette selectors, literal unique selectors, and sliding-window patterns
            uint32_t sel = 0;
            if (b < 16u) {
                sel = 0x55555555u;  // long repeated indexed selector
            } else if (b < 32u) {
                sel = 0xA0000000u | static_cast<uint32_t>(b * 0x10101u);  // literal selectors
            } else if (b < 48u) {
                // Repeat previous 16-element pattern (triggers sliding-window back-ref)
                sel = 0xA0000000u | static_cast<uint32_t>((b - 16u) * 0x10101u);
            } else {
                static constexpr std::array<uint32_t, 4> kPal = {
                    0x00000000u, 0xAAAAAAAAu, 0xFFFFFFFFu, 0x0F0F0F0Fu
                };
                sel = kPal[b & 3u];
            }
            WriteLe32(blk + 4u, sel);
        }

        // Test auto mode + explicit forced EndpointRingWindow & SelectorIndexTable & every codec/transform pair
        const std::array<BcPlaneCodec, 6> codecs = {
            BcPlaneCodec::Raw,
            BcPlaneCodec::LzRle,
            BcPlaneCodec::CanonicalHuffman,
            BcPlaneCodec::LzRleHuffman,
            BcPlaneCodec::EndpointRingWindow,
            BcPlaneCodec::SelectorIndexTable,
        };
        const std::array<BcPlaneTransform, 6> transforms = {
            BcPlaneTransform::None,
            BcPlaneTransform::Delta8,
            BcPlaneTransform::Xor8,
            BcPlaneTransform::Delta16,
            BcPlaneTransform::Rgb565Delta,
            BcPlaneTransform::TransposeDelta8,
        };

        for (BcPlaneCodec codec : codecs) {
            for (BcPlaneTransform tr : transforms) {
                BcPackCompressOptions opts{};
                opts.force_codec = true;
                opts.forced_codec = codec;
                opts.force_transform = true;
                opts.forced_transform = tr;

                std::vector<uint8_t> packed, unpacked;
                BcPackHeader hdr{};
                OS_REQUIRE(BcPackCompressWithOptions(BcFormat::BC1, w, h, bc1.data(), bc1.size(),
                                                     opts, packed).ok());
                OS_REQUIRE(BcPackDecompress(packed.data(), packed.size(), unpacked, &hdr).ok());
                OS_REQUIRE(unpacked == bc1);
                for (const auto& p : hdr.planes) {
                    OS_REQUIRE(p.codec == codec);
                }
            }
        }
    }

    // 2. Non-power-of-two & sub-block dimensions (1x1, 1x3, 3x1, 3x3, 5x7, 13x9, 17x33, 63x31)
    const std::array<std::pair<uint32_t, uint32_t>, 8> npot_dims = {{
        {1u, 1u}, {1u, 3u}, {3u, 1u}, {3u, 3u},
        {5u, 7u}, {13u, 9u}, {17u, 33u}, {63u, 31u},
    }};
    for (const auto& [w, h] : npot_dims) {
        for (BcFormat fmt : {BcFormat::BC1, BcFormat::BC4, BcFormat::BC7}) {
            std::vector<uint8_t> src, packed, unpacked;
            OS_REQUIRE(BcGenerateSyntheticSurface(fmt, w, h, w * 31u + h, src).ok());
            OS_REQUIRE(BcPackCompress(fmt, w, h, src.data(), src.size(), packed).ok());
            BcPackHeader hdr{};
            OS_REQUIRE(BcPackDecompress(packed.data(), packed.size(), unpacked, &hdr).ok());
            OS_REQUIRE(hdr.width == w && hdr.height == h);
            OS_REQUIRE(unpacked == src);
        }
    }

    // 3. Full multi-mip chain round-trip (NPOT 53x37 down to 1x1)
    {
        constexpr uint32_t base_w = 53u, base_h = 37u;
        const uint32_t mip_count = BcCalculateMaxMipLevels(base_w, base_h);
        OS_REQUIRE(mip_count == 6u);  // 53x37 -> 26x18 -> 13x9 -> 6x4 -> 3x2 -> 1x1
        std::vector<std::vector<uint8_t>> mips(mip_count);
        for (uint32_t m = 0; m < mip_count; ++m) {
            BcPackMipLevelInfo info{};
            OS_REQUIRE(BcCalculateMipLevelInfo(BcFormat::BC1, base_w, base_h, m, info));
            OS_REQUIRE(BcGenerateSyntheticSurface(BcFormat::BC1, info.width, info.height, m + 100u, mips[m]).ok());
            OS_REQUIRE(mips[m].size() == info.byte_size);
        }
        std::vector<uint8_t> packed_chain;
        OS_REQUIRE(BcPackCompressMipChain(BcFormat::BC1, base_w, base_h, mips, packed_chain).ok());

        std::vector<std::vector<uint8_t>> decoded_mips;
        BcPackMipChainHeader mhdr{};
        OS_REQUIRE(BcPackDecompressMipChain(packed_chain.data(), packed_chain.size(), decoded_mips, &mhdr).ok());
        OS_REQUIRE(mhdr.mip_count == mip_count && mhdr.base_width == base_w && mhdr.base_height == base_h);
        OS_REQUIRE(decoded_mips == mips);

        // Corrupt mip chain magic & truncated chain rejection
        std::vector<uint8_t> bad_chain = packed_chain;
        bad_chain[0] ^= 0xFFu;
        std::vector<std::vector<uint8_t>> dummy;
        OS_REQUIRE(!BcPackDecompressMipChain(bad_chain.data(), bad_chain.size(), dummy).ok());
        OS_REQUIRE(!BcPackDecompressMipChain(packed_chain.data(), 10u, dummy).ok());
    }

    // 4. Skewed Fibonacci-like symbol frequencies forcing length-limited Huffman (>15 bits) rebalancing
    {
        constexpr uint32_t w = 256u, h = 128u;  // 2048 blocks -> 8192 bytes in ColorIndices
        std::vector<uint8_t> bc1(2048u * 8u, 0u);
        // Place geometric-decay symbol frequencies in ColorIndices bytes (offsets 4..7)
        size_t pos = 0;
        const size_t total_sel_bytes = 2048u * 4u;
        std::vector<uint8_t> skewed(total_sel_bytes, 0u);
        uint32_t count = 1u;
        for (uint32_t sym = 1u; sym <= 22u && pos < total_sel_bytes; ++sym) {
            const size_t n = std::min<size_t>(count, total_sel_bytes - pos);
            std::memset(skewed.data() + pos, static_cast<int>(sym), n);
            pos += n;
            if (count < 4096u) count = (count * 3u) / 2u + 1u;
        }
        for (size_t b = 0; b < 2048u; ++b) {
            std::memcpy(bc1.data() + b * 8u + 4u, skewed.data() + b * 4u, 4u);
        }
        BcPackCompressOptions opts{};
        opts.force_codec = true;
        opts.forced_codec = BcPlaneCodec::CanonicalHuffman;
        opts.force_transform = true;
        opts.forced_transform = BcPlaneTransform::None;
        std::vector<uint8_t> packed, unpacked;
        OS_REQUIRE(BcPackCompressWithOptions(BcFormat::BC1, w, h, bc1.data(), bc1.size(), opts, packed).ok());
        OS_REQUIRE(BcPackDecompress(packed.data(), packed.size(), unpacked).ok());
        OS_REQUIRE(unpacked == bc1);
    }

    std::printf("[PASS] BCPack BC1 sliding-window, index-table, NPOT, multi-mip & deep Huffman edge cases\n");
}

void TestBcPackMalformedAndFuzzRejection() {
    std::vector<uint8_t> valid_src, valid_packed;
    OS_REQUIRE(BcGenerateSyntheticSurface(BcFormat::BC1, 16u, 16u, 42u, valid_src).ok());
    OS_REQUIRE(BcPackCompress(BcFormat::BC1, 16u, 16u, valid_src.data(), valid_src.size(), valid_packed).ok());

    std::vector<uint8_t> out;
    BcPackHeader hdr{};

    // 1. Header corruption tests
    OS_REQUIRE(!BcPackParseHeader(nullptr, 0u, hdr).ok());
    OS_REQUIRE(!BcPackParseHeader(valid_packed.data(), 12u, hdr).ok());

    auto expect_header_fail = [&](auto mutator) {
        std::vector<uint8_t> bad = valid_packed;
        mutator(bad);
        BcPackHeader h{};
        OS_REQUIRE(!BcPackParseHeader(bad.data(), bad.size(), h).ok());
    };

    expect_header_fail([](auto& b) { b[0] = 'X'; });                              // bad magic
    expect_header_fail([](auto& b) { WriteLe16(b.data() + 4, 2u); });             // bad version
    expect_header_fail([](auto& b) { WriteLe16(b.data() + 6, 1u); });             // non-zero flags
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 8, 99u); });            // unknown BC format
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 12, 0u); });            // zero width
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 16, 20000u); });        // overflowing height
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 20, 9999u); });         // block_count mismatch
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 24, 9999u); });         // original_size mismatch
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 28, 7u); });            // wrong plane_count
    expect_header_fail([](auto& b) { b[36] = 99u; });                             // mismatched plane kind
    expect_header_fail([](auto& b) { b[37] = 99u; });                             // unknown transform
    expect_header_fail([](auto& b) { b[38] = 99u; });                             // unknown codec
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 36 + 8, 0u); });        // zero compressed_size
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 36 + 12, 16u); });      // data_offset inside header
    expect_header_fail([](auto& b) { WriteLe32(b.data() + 36 + 12, 0xFFFF0000u); });  // offset overflow
    // Overlapping plane payloads (make plane 1 point to plane 0's data_offset)
    expect_header_fail([](auto& b) {
        const uint32_t off0 = static_cast<uint32_t>(b[36 + 12]) |
                              (static_cast<uint32_t>(b[36 + 13]) << 8u) |
                              (static_cast<uint32_t>(b[36 + 14]) << 16u) |
                              (static_cast<uint32_t>(b[36 + 15]) << 24u);
        WriteLe32(b.data() + 36 + 20 + 12, off0);
    });

    // 2. Entropy layer bounds rejection (valid container header + valid plane CRC, malformed codec payload)
    // 2a. LZ-RLE: distance == 0 and distance > out.size()
    {
        auto s1 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None, BcPlaneCodec::LzRle,
                                                {0xC0u, 0x00u, 0x00u});  // dist = 0
        OS_REQUIRE(!BcPackDecompress(s1.data(), s1.size(), out).ok());

        auto s2 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None, BcPlaneCodec::LzRle,
                                                {0x00u, 0xABu, 0xC0u, 0x05u, 0x00u});  // dist 5 > emitted 1
        OS_REQUIRE(!BcPackDecompress(s2.data(), s2.size(), out).ok());
    }

    // 2b. CanonicalHuffman: duplicate sparse symbol, Kraft sum overflow, non-zero padding bits
    {
        // Duplicate sparse symbol (count=2, sym 0x41 len 1, sym 0x41 len 1)
        std::vector<uint8_t> dup_huff = {32u, 0u, 0u, 0u, 1u, 2u, 0x41u, 1u, 0x41u, 1u, 0u, 0u, 0u, 0u};
        auto s1 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None,
                                                BcPlaneCodec::CanonicalHuffman, dup_huff);
        OS_REQUIRE(!BcPackDecompress(s1.data(), s1.size(), out).ok());

        // Kraft sum overflow (3 symbols each of length 1 -> 3 * 16384 > 32768)
        std::vector<uint8_t> kraft_huff = {32u, 0u, 0u, 0u, 1u, 3u, 1u, 1u, 2u, 1u, 3u, 1u, 0u, 0u, 0u, 0u};
        auto s2 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None,
                                                BcPlaneCodec::CanonicalHuffman, kraft_huff);
        OS_REQUIRE(!BcPackDecompress(s2.data(), s2.size(), out).ok());

        // Trailing garbage byte after single-symbol valid stream
        std::vector<uint8_t> trail_huff = {32u, 0u, 0u, 0u, 1u, 1u, 0x55u, 1u, 0u, 0u, 0u, 0u, 0u};
        auto s3 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None,
                                                BcPlaneCodec::CanonicalHuffman, trail_huff);
        OS_REQUIRE(!BcPackDecompress(s3.data(), s3.size(), out).ok());
    }

    // 2c. EndpointRingWindow: back-ref on empty ring, age >= populated, repeat on empty ring, truncated literal
    {
        auto s1 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None,
                                                BcPlaneCodec::EndpointRingWindow, {0x00u});  // back-ref empty
        OS_REQUIRE(!BcPackDecompress(s1.data(), s1.size(), out).ok());

        auto s2 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None,
                                                BcPlaneCodec::EndpointRingWindow, {0x40u});  // repeat empty
        OS_REQUIRE(!BcPackDecompress(s2.data(), s2.size(), out).ok());

        auto s3 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None,
                                                BcPlaneCodec::EndpointRingWindow, {0xA0u, 0x11u, 0x22u, 0x05u});  // age 5 >= pop 1
        OS_REQUIRE(!BcPackDecompress(s3.data(), s3.size(), out).ok());

        auto s4 = CraftBc1StreamWithCustomPlane(16u, 16u, 0u, BcPlaneTransform::None,
                                                BcPlaneCodec::EndpointRingWindow, {0xA0u, 0x11u});  // truncated 2B literal
        OS_REQUIRE(!BcPackDecompress(s4.data(), s4.size(), out).ok());
    }

    // 2d. SelectorIndexTable: table_size > 256, index >= table_size, sliding-window dist > emitted
    {
        auto s1 = CraftBc1StreamWithCustomPlane(16u, 16u, 2u, BcPlaneTransform::None,
                                                BcPlaneCodec::SelectorIndexTable, {0x01u, 0x01u});  // table_size = 257
        OS_REQUIRE(!BcPackDecompress(s1.data(), s1.size(), out).ok());

        auto s2 = CraftBc1StreamWithCustomPlane(16u, 16u, 2u, BcPlaneTransform::None,
                                                BcPlaneCodec::SelectorIndexTable, {0x00u, 0x00u, 0x40u, 0x00u});  // table_size 0, idx 0
        OS_REQUIRE(!BcPackDecompress(s2.data(), s2.size(), out).ok());

        auto s3 = CraftBc1StreamWithCustomPlane(16u, 16u, 2u, BcPlaneTransform::None,
                                                BcPlaneCodec::SelectorIndexTable,
                                                {0x00u, 0x00u, 0xC0u, 0x01u, 0x00u});  // dist 1 > emitted 0
        OS_REQUIRE(!BcPackDecompress(s3.data(), s3.size(), out).ok());
    }

    // 3. Deterministic byte-mutation & truncation fuzzing across valid stream
    for (size_t trunc = 0; trunc < valid_packed.size(); ++trunc) {
        OS_REQUIRE(!BcPackDecompress(valid_packed.data(), trunc, out).ok());
    }
    for (size_t i = 0; i < valid_packed.size(); ++i) {
        std::vector<uint8_t> mutated = valid_packed;
        mutated[i] ^= 0x80u;
        OS_REQUIRE(!BcPackDecompress(mutated.data(), mutated.size(), out).ok());
    }

    std::printf("[PASS] BCPack malformed headers, entropy OOB rejection & mutation fuzzing\n");
}

void TestRdna2Bvh4PointerAndFp16Primitives() {
    // 1. 3-bit low node-type pointer tagging in 64-byte-shifted node pointers
    for (uint8_t t = 0; t < 8u; ++t) {
        const auto type = static_cast<Rdna2BvhNodeType>(t);
        const uint64_t off = 256ull + static_cast<uint64_t>(t) * 128ull;
        const uint32_t ptr = EncodeRdna2NodePtr(off, type);
        OS_REQUIRE(IsRdna2NodePtrValid(ptr));
        OS_REQUIRE(DecodeRdna2NodeType(ptr) == type);
        OS_REQUIRE(DecodeRdna2NodeByteOffset(ptr) == off);
    }
    // Unaligned offset must be rejected
    OS_REQUIRE(EncodeRdna2NodePtr(260ull, Rdna2BvhNodeType::Box32) == kRdna2BvhInvalidNodePtr);
    OS_REQUIRE(EncodeRdna2NodePtr(63ull, Rdna2BvhNodeType::Box16) == kRdna2BvhInvalidNodePtr);
    OS_REQUIRE(!IsRdna2NodePtrValid(kRdna2BvhInvalidNodePtr));

    // Node byte sizes match GFX10.3 spec (64B vs 128B)
    OS_REQUIRE(Rdna2NodeByteSize(Rdna2BvhNodeType::Triangle0) == 64u);
    OS_REQUIRE(Rdna2NodeByteSize(Rdna2BvhNodeType::Box16) == 64u);
    OS_REQUIRE(Rdna2NodeByteSize(Rdna2BvhNodeType::Box32) == 128u);
    OS_REQUIRE(Rdna2NodeByteSize(Rdna2BvhNodeType::Instance) == 128u);
    OS_REQUIRE(Rdna2NodeByteSize(Rdna2BvhNodeType::Procedural) == 64u);

    // 2. Conservative IEEE-754 binary16 AABB rounding guarantees enclosure
    const std::array<float, 14> test_vals = {
        -12345.678f, -100.33333f, -1.0005f, -0.1f, -1e-4f, -1e-6f, -1e-9f,
        0.0f, 1e-9f, 1e-6f, 1e-4f, 0.1f, 1.0005f, 12345.678f,
    };
    for (float x : test_vals) {
        const float lo = Fp16ToFloat32(Float32ToFp16RoundDown(x));
        const float hi = Fp16ToFloat32(Float32ToFp16RoundUp(x));
        OS_REQUIRE(std::isfinite(lo) && std::isfinite(hi));
        OS_REQUIRE(lo <= x);
        OS_REQUIRE(hi >= x);
    }

    // 3. Single-node binary encode/decode bit-exact round-trip
    std::vector<uint8_t> scratch(1024u, 0u);

    // Box32
    {
        Rdna2BvhBox32Node in{};
        in.children = {
            EncodeRdna2NodePtr(256u, Rdna2BvhNodeType::Triangle0),
            EncodeRdna2NodePtr(320u, Rdna2BvhNodeType::Box16),
            kRdna2BvhInvalidNodePtr,
            kRdna2BvhInvalidNodePtr,
        };
        in.bbox[0] = {{-1.5f, -2.5f, -3.5f}, {1.25f, 2.25f, 3.25f}};
        in.bbox[1] = {{10.0f, 20.0f, 30.0f}, {15.0f, 25.0f, 35.0f}};
        in.parent_ptr = EncodeRdna2NodePtr(128u, Rdna2BvhNodeType::Box32);
        in.child_count = 2u;
        in.flags = 0x1234u;
        OS_REQUIRE(EncodeRdna2Box32Node(in, scratch.data() + 256u, 128u).ok());
        Rdna2BvhBox32Node out{};
        OS_REQUIRE(DecodeRdna2Box32Node(scratch.data(), scratch.size(),
                                        EncodeRdna2NodePtr(256u, Rdna2BvhNodeType::Box32), out).ok());
        OS_REQUIRE(out.children == in.children);
        OS_REQUIRE(out.parent_ptr == in.parent_ptr && out.child_count == 2u && out.flags == 0x1234u);
        OS_REQUIRE(out.bbox[0].min.x == -1.5f && out.bbox[1].max.z == 35.0f);
    }

    // Triangle leaf
    {
        Rdna2BvhTriangleNode in{};
        in.v0 = {1.0f, 2.0f, 3.0f};
        in.v1 = {4.0f, 5.0f, 6.0f};
        in.v2 = {7.0f, 8.0f, 9.0f};
        in.triangle_id = 1u;
        in.primitive_index = 42u;
        in.geometry_index = 7u;
        in.geometry_flags = 0x5Au;
        OS_REQUIRE(EncodeRdna2TriangleNode(in, scratch.data() + 384u, 64u).ok());
        Rdna2BvhTriangleNode out{};
        OS_REQUIRE(DecodeRdna2TriangleNode(scratch.data(), scratch.size(),
                                           EncodeRdna2NodePtr(384u, Rdna2BvhNodeType::Triangle0), out).ok());
        OS_REQUIRE(out.primitive_index == 42u && out.geometry_index == 7u && out.geometry_flags == 0x5Au);
        OS_REQUIRE(out.v0.x == 1.0f && out.v1.y == 5.0f && out.v2.z == 9.0f);
    }

    // Instance & Procedural leaves via TLAS builder
    {
        Rdna2BvhTlasInstanceInput inst0{};
        inst0.blas_base_va = 0x100000ull;
        inst0.blas_root_ptr = EncodeRdna2NodePtr(256u, Rdna2BvhNodeType::Box32);
        inst0.blas_local_aabb = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
        MakeRotationY3x4(0.75f, 3.0f, -1.0f, 2.0f, inst0.object_to_world);
        inst0.instance_id = 101u;
        inst0.instance_mask = 0xF0u;
        inst0.sbt_offset = 3u;
        inst0.instance_flags = 0x05u;

        Rdna2BvhTlasInstanceInput inst1{};
        inst1.blas_base_va = 0x200000ull;
        inst1.blas_root_ptr = EncodeRdna2NodePtr(256u, Rdna2BvhNodeType::Box16);
        inst1.blas_local_aabb = {{-0.5f, 0.0f, -0.5f}, {0.5f, 2.0f, 0.5f}};
        MakeScaleTranslation3x4(2.0f, 1.5f, 0.5f, -4.0f, 0.5f, 1.0f, inst1.object_to_world);
        inst1.instance_id = 202u;

        Rdna2BvhProceduralInput proc0{};
        proc0.bounds = {{5.0f, 0.0f, -2.0f}, {7.0f, 2.0f, 0.0f}};
        proc0.primitive_index = 99u;
        proc0.geometry_index = 2u;
        proc0.geometry_flags = 0x01u;
        proc0.user_data[0] = 0xDEADBEEFu;

        std::vector<uint8_t> tlas_buf;
        Rdna2BvhHeader thdr{};
        Rdna2BvhEncodeOptions opts{};
        opts.use_box16 = false;
        OS_REQUIRE(BuildRdna2Bvh4Tlas({inst0, inst1}, {proc0}, opts, tlas_buf, &thdr).ok());
        OS_REQUIRE(thdr.as_type == Rdna2BvhAsType::TopLevel);
        OS_REQUIRE(thdr.leaf_node_count == 3u);

        Rdna2BvhValidationReport rep{};
        OS_REQUIRE(ValidateRdna2Bvh4(tlas_buf.data(), tlas_buf.size(), &rep).ok());
        OS_REQUIRE(rep.visited_instance_nodes == 2u);
        OS_REQUIRE(rep.visited_procedural_nodes == 1u);
        OS_REQUIRE(rep.visited_box32_nodes == 1u);

        // Verify world_to_object * object_to_world == Identity on decoded instance node
        Rdna2BvhBox32Node root_box{};
        OS_REQUIRE(DecodeRdna2Box32Node(tlas_buf.data(), tlas_buf.size(), thdr.root_node_ptr, root_box).ok());
        bool found_inst0 = false;
        bool found_proc0 = false;
        for (uint32_t c = 0; c < 4u; ++c) {
            const uint32_t cptr = root_box.children[c];
            if (!IsRdna2NodePtrValid(cptr)) continue;
            if (DecodeRdna2NodeType(cptr) == Rdna2BvhNodeType::Instance) {
                Rdna2BvhInstanceNode in{};
                OS_REQUIRE(DecodeRdna2InstanceNode(tlas_buf.data(), tlas_buf.size(), cptr, in).ok());
                const Vec3 p{0.7f, -0.3f, 0.9f};
                const Vec3 w = TransformPoint3x4(in.object_to_world, p);
                const Vec3 back = TransformPoint3x4(in.world_to_object, w);
                OS_REQUIRE(std::fabs(back.x - p.x) < 1e-4f);
                OS_REQUIRE(std::fabs(back.y - p.y) < 1e-4f);
                OS_REQUIRE(std::fabs(back.z - p.z) < 1e-4f);
                if (in.instance_id == 101u) {
                    OS_REQUIRE(in.instance_mask == 0xF0u && in.sbt_offset == 3u && in.instance_flags == 0x05u);
                    found_inst0 = true;
                }
            } else if (DecodeRdna2NodeType(cptr) == Rdna2BvhNodeType::Procedural) {
                Rdna2BvhProceduralNode pn{};
                OS_REQUIRE(DecodeRdna2ProceduralNode(tlas_buf.data(), tlas_buf.size(), cptr, pn).ok());
                OS_REQUIRE(pn.primitive_index == 99u && pn.user_data[0] == 0xDEADBEEFu);
                found_proc0 = true;
            }
        }
        OS_REQUIRE(found_inst0 && found_proc0);
    }

    std::printf("[PASS] RDNA2 BVH4 pointer tagging, FP16 AABB rounding, node codecs & TLAS\n");
}

void TestRdna2Bvh4ValidationRejection() {
    // Build a valid baseline Box32 BVH4
    BvhAccelerationStructure bvh2;
    OS_REQUIRE(bvh2.Build(BuildDefaultRtTestScene()).ok());
    std::vector<uint8_t> valid_buf;
    Rdna2BvhHeader valid_hdr{};
    Rdna2BvhEncodeOptions opts{};
    OS_REQUIRE(EncodeBvh2ToRdna2Bvh4(bvh2, opts, valid_buf, &valid_hdr).ok());
    OS_REQUIRE(ValidateRdna2Bvh4(valid_buf.data(), valid_buf.size()).ok());

    // 1. Corrupt magic / version / CRC32 / truncated buffer
    {
        auto b = valid_buf;
        b[0] ^= 0xFFu;
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());

        b = valid_buf;
        WriteLe32(b.data() + 4, 99u);
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());

        b = valid_buf;
        b[valid_hdr.nodes_offset_bytes + 8u] ^= 0x01u;  // CRC mismatch
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());

        OS_REQUIRE(!ValidateRdna2Bvh4(valid_buf.data(), valid_buf.size() - 64u).ok());
    }

    // 2. Self-loop cycle in Box32 node (disable CRC32 to test structural cycle detector directly)
    {
        auto b = valid_buf;
        WriteLe32(b.data() + 80, 0u);  // disable CRC so structural validator runs
        const uint64_t root_off = DecodeRdna2NodeByteOffset(valid_hdr.root_node_ptr);
        WriteLe32(b.data() + root_off + 0u, valid_hdr.root_node_ptr);  // child 0 -> self!
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());
    }

    // 3. Out-of-bounds child pointer
    {
        auto b = valid_buf;
        WriteLe32(b.data() + 80, 0u);
        const uint64_t root_off = DecodeRdna2NodeByteOffset(valid_hdr.root_node_ptr);
        const uint32_t oob_ptr = EncodeRdna2NodePtr(valid_buf.size() + 256u, Rdna2BvhNodeType::Triangle0);
        WriteLe32(b.data() + root_off + 0u, oob_ptr);
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());
    }

    // 4. Child AABB failing to enclose subtree (shrunk to degenerate point outside subtree)
    {
        auto b = valid_buf;
        WriteLe32(b.data() + 80, 0u);
        const uint64_t root_off = DecodeRdna2NodeByteOffset(valid_hdr.root_node_ptr);
        // Set child 0 AABB to tiny box far away at [999, 1000]
        float far_min = 999.0f;
        float far_max = 1000.0f;
        for (int a = 0; a < 3; ++a) {
            std::memcpy(b.data() + root_off + 16u + static_cast<size_t>(a) * 4u, &far_min, 4u);
            std::memcpy(b.data() + root_off + 28u + static_cast<size_t>(a) * 4u, &far_max, 4u);
        }
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());
    }

    // 5. Inverted AABB (min > max) and NaN AABB
    {
        auto b = valid_buf;
        WriteLe32(b.data() + 80, 0u);
        const uint64_t root_off = DecodeRdna2NodeByteOffset(valid_hdr.root_node_ptr);
        float hi = 5.0f, lo = -5.0f;
        std::memcpy(b.data() + root_off + 16u, &hi, 4u);
        std::memcpy(b.data() + root_off + 28u, &lo, 4u);
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());

        b = valid_buf;
        WriteLe32(b.data() + 80, 0u);
        const float qnan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(b.data() + root_off + 16u, &qnan, 4u);
        OS_REQUIRE(!ValidateRdna2Bvh4(b.data(), b.size()).ok());
    }

    // 6. Excessive tree depth (> kRdna2BvhMaxDepth = 64) chain of 70 Box32 nodes
    {
        constexpr uint32_t kChainLen = 70u;
        std::vector<uint8_t> deep_buf(kRdna2BvhHeaderSize + kChainLen * kRdna2BvhBox32Size + kRdna2BvhTriangleNodeSize, 0u);

        const uint32_t tri_off = kRdna2BvhHeaderSize + kChainLen * kRdna2BvhBox32Size;
        Rdna2BvhTriangleNode tri{};
        tri.v0 = {0.0f, 0.0f, 0.0f};
        tri.v1 = {1.0f, 0.0f, 0.0f};
        tri.v2 = {0.0f, 1.0f, 0.0f};
        tri.triangle_id = 1u;
        OS_REQUIRE(EncodeRdna2TriangleNode(tri, deep_buf.data() + tri_off, kRdna2BvhTriangleNodeSize).ok());

        uint32_t child_ptr = EncodeRdna2NodePtr(tri_off, Rdna2BvhNodeType::Triangle0);
        for (int i = static_cast<int>(kChainLen) - 1; i >= 0; --i) {
            const uint32_t off = kRdna2BvhHeaderSize + static_cast<uint32_t>(i) * kRdna2BvhBox32Size;
            Rdna2BvhBox32Node box{};
            box.children[0] = child_ptr;
            box.bbox[0] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
            box.child_count = 1u;
            OS_REQUIRE(EncodeRdna2Box32Node(box, deep_buf.data() + off, kRdna2BvhBox32Size).ok());
            child_ptr = EncodeRdna2NodePtr(off, Rdna2BvhNodeType::Box32);
        }

        Rdna2BvhHeader hdr{};
        hdr.total_size_bytes = static_cast<uint32_t>(deep_buf.size());
        hdr.nodes_size_bytes = static_cast<uint32_t>(deep_buf.size() - kRdna2BvhHeaderSize);
        hdr.root_node_ptr = child_ptr;
        hdr.internal_node_count = kChainLen;
        hdr.leaf_node_count = 1u;
        hdr.primitive_count = 1u;
        hdr.max_depth = kRdna2BvhMaxDepth;
        hdr.root_aabb = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
        OS_REQUIRE(EncodeRdna2BvhHeader(hdr, deep_buf.data(), deep_buf.size()).ok());

        OS_REQUIRE(!ValidateRdna2Bvh4(deep_buf.data(), deep_buf.size()).ok());
    }

    std::printf("[PASS] RDNA2 BVH4 corrupt/OOB/cyclic/bounds/depth validation rejection\n");
}

void TestSahBvh2ToRdna2Bvh4RayEquivalence() {
    // Construct a rich scene combining the Cornell test scene + 8 transformed cubes (108 triangles)
    std::vector<RtTriangle> scene = BuildDefaultRtTestScene();
    const std::vector<RtTriangle> cube = BuildUnitCubeMesh({0.3f, 0.6f, 0.9f}, 0.2f);
    for (int i = 0; i < 8; ++i) {
        float xform[3][4]{};
        const float angle = static_cast<float>(i) * 0.45f;
        const float tx = -1.6f + static_cast<float>(i % 4) * 1.05f;
        const float ty = 0.45f + static_cast<float>(i / 4) * 0.95f;
        const float tz = -0.8f + static_cast<float>(i / 4) * 1.4f;
        MakeRotationY3x4(angle, tx, ty, tz, xform);
        for (const auto& t : cube) {
            RtTriangle scaled = t;
            scaled.v0 = {t.v0.x * 0.35f, t.v0.y * 0.35f, t.v0.z * 0.35f};
            scaled.v1 = {t.v1.x * 0.35f, t.v1.y * 0.35f, t.v1.z * 0.35f};
            scaled.v2 = {t.v2.x * 0.35f, t.v2.y * 0.35f, t.v2.z * 0.35f};
            scene.push_back(TransformTriangle3x4(xform, scaled));
        }
    }
    OS_REQUIRE(scene.size() == 108u);

    // 1. Build reference binary SAH BVH
    BvhAccelerationStructure bvh2;
    OS_REQUIRE(bvh2.Build(scene).ok());

    // 2. Encode to BOTH RDNA2 BVH4 Box32 (128B FP32) and Box16 (64B FP16)
    std::vector<uint8_t> buf_box32, buf_box16;
    Rdna2BvhHeader hdr32{}, hdr16{};

    Rdna2BvhEncodeOptions opt32{};
    opt32.use_box16 = false;
    OS_REQUIRE(EncodeBvh2ToRdna2Bvh4(bvh2, opt32, buf_box32, &hdr32).ok());

    Rdna2BvhEncodeOptions opt16{};
    opt16.use_box16 = true;
    OS_REQUIRE(EncodeBvh2ToRdna2Bvh4(bvh2, opt16, buf_box16, &hdr16).ok());

    // Box16 buffer should be strictly smaller than Box32 buffer!
    OS_REQUIRE(buf_box16.size() < buf_box32.size());

    // 3. Load & validate both BVH4 trees
    Rdna2Bvh4Tree tree32, tree16;
    OS_REQUIRE(tree32.LoadFromBuffer(buf_box32.data(), buf_box32.size()).ok());
    OS_REQUIRE(tree16.LoadFromBuffer(buf_box16.data(), buf_box16.size()).ok());
    tree32.AttachTriangleMetadata(scene);
    tree16.AttachTriangleMetadata(scene);

    OS_REQUIRE(tree32.ValidationReport().visited_triangle_nodes == scene.size());
    OS_REQUIRE(tree16.ValidationReport().visited_triangle_nodes == scene.size());
    OS_REQUIRE(tree32.ValidationReport().visited_box32_nodes > 0u);
    OS_REQUIRE(tree16.ValidationReport().visited_box16_nodes > 0u);

    // 4. Cast 64x48 = 3,072 primary rays + shadow rays and assert exact equivalence!
    constexpr uint32_t kW = 64u, kH = 48u;
    const Vec3 cam{0.0f, 1.0f, -3.5f};
    const Vec3 light{1.5f, 3.0f, -1.5f};
    uint32_t total_hits = 0;
    uint32_t total_shadow_hits = 0;

    for (uint32_t y = 0; y < kH; ++y) {
        const float ndc_y = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
        for (uint32_t x = 0; x < kW; ++x) {
            const float ndc_x = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(kW) - 1.0f;
            Vec3 d{ndc_x * 0.85f, ndc_y * 0.65f - 0.14f, 1.0f};
            const float inv_len = 1.0f / std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            d = {d.x * inv_len, d.y * inv_len, d.z * inv_len};

            RtRay ray{cam, d, 0.001f, 1e30f};
            RtHit h_ref{}, h_32{}, h_16{};
            const bool hit_ref = bvh2.TraceRay(ray, h_ref);
            const bool hit_32 = tree32.TraceRay(ray, h_32);
            const bool hit_16 = tree16.TraceRay(ray, h_16);

            OS_REQUIRE(hit_ref == hit_32);
            OS_REQUIRE(hit_ref == hit_16);
            if (hit_ref) {
                ++total_hits;
                OS_REQUIRE(std::fabs(h_ref.t - h_32.t) < 1e-5f);
                OS_REQUIRE(std::fabs(h_ref.t - h_16.t) < 1e-5f);
                OS_REQUIRE(h_ref.triangle_index == h_32.triangle_index);
                OS_REQUIRE(h_ref.triangle_index == h_16.triangle_index);
                OS_REQUIRE(std::fabs(h_ref.u - h_32.u) < 1e-5f);
                OS_REQUIRE(std::fabs(h_ref.v - h_32.v) < 1e-5f);
                OS_REQUIRE(std::fabs(h_ref.normal.x - h_16.normal.x) < 1e-5f);
                OS_REQUIRE(std::fabs(h_ref.normal.y - h_16.normal.y) < 1e-5f);
                OS_REQUIRE(std::fabs(h_ref.normal.z - h_16.normal.z) < 1e-5f);

                // Verify shadow ray equivalence from hit point toward light
                const Vec3 hp{
                    ray.origin.x + ray.direction.x * h_ref.t + h_ref.normal.x * 0.002f,
                    ray.origin.y + ray.direction.y * h_ref.t + h_ref.normal.y * 0.002f,
                    ray.origin.z + ray.direction.z * h_ref.t + h_ref.normal.z * 0.002f,
                };
                Vec3 to_l{light.x - hp.x, light.y - hp.y, light.z - hp.z};
                const float ldist = std::sqrt(to_l.x * to_l.x + to_l.y * to_l.y + to_l.z * to_l.z);
                to_l = {to_l.x / ldist, to_l.y / ldist, to_l.z / ldist};
                RtRay sray{hp, to_l, 0.001f, ldist - 0.002f};

                const bool sh_ref = bvh2.TraceShadowRay(sray);
                const bool sh_32 = tree32.TraceShadowRay(sray);
                const bool sh_16 = tree16.TraceShadowRay(sray);
                OS_REQUIRE(sh_ref == sh_32);
                OS_REQUIRE(sh_ref == sh_16);
                if (sh_ref) ++total_shadow_hits;
            }
        }
    }

    OS_REQUIRE(total_hits > 1500u);
    OS_REQUIRE(total_shadow_hits > 100u);
    std::printf("[PASS] SAH BVH2 -> RDNA2 BVH4 (Box32=%zuB, Box16=%zuB) ray equivalence (%u hits, %u shadows)\n",
                buf_box32.size(), buf_box16.size(), total_hits, total_shadow_hits);
}

}  // namespace

int main() {
    TestBcPackAllFormatsRoundTrip();
    TestBcPackBc1SlidingWindowAndIndexTableEdgeCases();
    TestBcPackMalformedAndFuzzRejection();
    TestRdna2Bvh4PointerAndFp16Primitives();
    TestRdna2Bvh4ValidationRejection();
    TestSahBvh2ToRdna2Bvh4RayEquivalence();
    std::printf("[PASS] test_bcpack_bvh4 (all BCPack BC1-BC7 & RDNA2 BVH4 tests passed)\n");
    return 0;
}
