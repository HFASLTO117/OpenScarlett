#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/gdeflate.h"
#include "openscarlett/middleware/bink2_decoder.h"
#include "os_check.h"

int main() {
    using namespace openscarlett;

    // 1. Microsoft GDeflate v1 (0x04 0xFB) multi-tile roundtrip
    {
        std::vector<uint8_t> src(140000);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<uint8_t>(((i * 7) + (i / 128) * 13) & 0xFFu);
        }
        std::vector<uint8_t> comp;
        OS_REQUIRE(GDeflateCompress(src.data(), src.size(), comp, GDeflateBlockMode::Auto));
        OS_REQUIRE(comp.size() >= 8 && comp[0] == kGDeflateCodecId && comp[1] == kGDeflateMagic);
        OS_REQUIRE(comp.size() < src.size());
        std::vector<uint8_t> dec(src.size(), 0);
        size_t out_sz = 0;
        OS_REQUIRE(GDeflateDecompress(comp.data(), comp.size(), dec.data(), dec.size(), &out_sz) == GDeflateResult::Ok);
        OS_REQUIRE(out_sz == src.size() && dec == src);
    }

    // 2. Xbox BCPack BC7 roundtrip & compression ratio
    {
        constexpr uint32_t w = 64, h = 64;
        const size_t blocks = (w / 4) * (h / 4);
        std::vector<uint8_t> bc7(blocks * 16);
        for (size_t b = 0; b < blocks; ++b) {
            bc7[b * 16 + 0] = 0x40;
            bc7[b * 16 + 1] = static_cast<uint8_t>((b * 3) & 0xFF);
            bc7[b * 16 + 2] = static_cast<uint8_t>((b * 5) & 0xFF);
            for (size_t k = 3; k < 16; ++k) bc7[b * 16 + k] = static_cast<uint8_t>((b + k * 11) & 0xFF);
        }
        std::vector<uint8_t> packed, unpacked;
        OS_REQUIRE(BcPackCompress(BcFormat::BC7, w, h, bc7.data(), bc7.size(), packed).ok());
        OS_REQUIRE(packed.size() < bc7.size());
        BcPackHeader hdr{};
        OS_REQUIRE(BcPackDecompress(packed.data(), packed.size(), unpacked, &hdr).ok());
        OS_REQUIRE(hdr.width == w && hdr.height == h && hdr.format == BcFormat::BC7);
        OS_REQUIRE(unpacked == bc7);
    }

    // 3. Native XMA2 2048-byte packet encode -> IMDCT decode
    {
        Xma2Config cfg{};
        cfg.sample_rate = 48000;
        cfg.channels = 1;
        constexpr size_t frames = 4096;
        std::vector<float> pcm(frames);
        for (size_t i = 0; i < frames; ++i) {
            pcm[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(cfg.sample_rate));
        }
        std::vector<uint8_t> packets;
        OS_REQUIRE(Xma2EncodePcmToPackets(pcm.data(), frames, cfg, packets).ok());
        OS_REQUIRE(!packets.empty() && (packets.size() % kXma2PacketSize) == 0);
        Xma2Decoder dec;
        OS_REQUIRE(dec.Initialize(cfg).ok());
        std::vector<float> out_pcm;
        OS_REQUIRE(dec.DecodePackets(packets.data(), packets.size(), out_pcm).ok());
        OS_REQUIRE(out_pcm.size() >= frames);
    }

    // 4. Bink2 KB2i multi-frame encode -> decode
    {
        constexpr uint32_t w = 32, h = 32;
        std::vector<std::vector<uint8_t>> frames(3, std::vector<uint8_t>(w * h * 4));
        for (size_t f = 0; f < frames.size(); ++f) {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    size_t idx = (y * w + x) * 4;
                    frames[f][idx + 0] = static_cast<uint8_t>((x * 8 + f * 20) & 0xFF);
                    frames[f][idx + 1] = static_cast<uint8_t>((y * 8) & 0xFF);
                    frames[f][idx + 2] = static_cast<uint8_t>(64 + f * 30);
                    frames[f][idx + 3] = 255;
                }
            }
        }
        Bink2EncodeParams ep{};
        ep.width = w;
        ep.height = h;
        std::vector<uint8_t> stream;
        OS_REQUIRE(Bink2EncodeTestStream(frames, ep, stream).ok());
        Bink2Decoder dec;
        OS_REQUIRE(dec.OpenFromMemory(stream.data(), stream.size()).ok());
        OS_REQUIRE(dec.Header().frame_count == 3);
        Bink2DecodedFrame out_frame{};
        OS_REQUIRE(dec.DecodeFrame(0, Bink2PixelFormat::Rgba8, out_frame).ok());
        OS_REQUIRE(out_frame.pixels.size() == w * h * 4);
    }

    std::printf("[PASS] test_codecs (GDeflate v1, BCPack, XMA2 IMDCT, Bink2 KB2i)\n");
    return 0;
}
