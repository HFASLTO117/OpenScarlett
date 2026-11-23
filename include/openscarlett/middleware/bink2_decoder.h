#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

enum class Bink2ChromaFormat : uint8_t {
    YCbCr420 = 0,
    YCbCr444 = 1,
};

enum class Bink2PixelFormat : uint8_t {
    Rgba8 = 0,
    Bgra8 = 1,
};

enum class Bink2ColorMatrix : uint8_t {
    Bt709 = 0,  // Full-range ITU-R BT.709
    Bt601 = 1,  // Full-range ITU-R BT.601
};

struct Bink2GpuConverterStats {
    bool initialized = false;
    bool using_gpu = false;
    bool is_warp = false;
    uint64_t gpu_conversions = 0;
    uint64_t cpu_fallback_conversions = 0;
    std::string adapter_description;
};

struct Bink2FrameIndexEntry {
    uint32_t offset = 0;
    uint32_t size = 0;
    bool is_keyframe = false;
};

struct Bink2ContainerHeader {
    char fourcc[5] = {};  // Null-terminated FourCC: "KB2i", "KB2j", "BIKi", etc.
    bool is_bink2 = false;
    uint32_t file_size_field = 0;     // File size minus 8 bytes per Bink spec
    uint32_t frame_count = 0;
    uint32_t largest_frame_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;
    uint32_t video_flags = 0;
    uint32_t audio_track_count = 0;
    Bink2ChromaFormat chroma_format = Bink2ChromaFormat::YCbCr420;
    std::vector<Bink2FrameIndexEntry> frame_index;
};

struct Bink2DecodedFrame {
    uint32_t frame_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_keyframe = false;
    Bink2PixelFormat pixel_format = Bink2PixelFormat::Rgba8;
    std::vector<uint8_t> pixels;  // width * height * 4
};

struct Bink2EncodeParams {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;
    std::string fourcc = "KB2i";
    Bink2ChromaFormat chroma_format = Bink2ChromaFormat::YCbCr420;
    uint32_t keyframe_interval = 10;
    uint8_t quant_y = 2;
    uint8_t quant_c = 3;
    uint32_t audio_track_count = 0;
};

// 8x8 2D Forward & Inverse DCT (exact orthonormal Type-II / Type-III DCT).
void Bink2ForwardDct8x8(const float in[64], float out[64]) noexcept;
void Bink2InverseDct8x8(const float in[64], float out[64]) noexcept;

// BT.709 & BT.601 full-range RGB <-> YCbCr conversion helpers.
void Bink2RgbToYCbCr709(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, uint8_t& cb, uint8_t& cr) noexcept;
void Bink2YCbCr709ToRgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t& r, uint8_t& g, uint8_t& b) noexcept;
void Bink2RgbToYCbCr601(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, uint8_t& cb, uint8_t& cr) noexcept;
void Bink2YCbCr601ToRgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t& r, uint8_t& g, uint8_t& b) noexcept;

// Reference CPU planar YUV 4:2:0 (+ optional Alpha) -> RGBA8/BGRA8 color-space converter.
Status Bink2ConvertYuv420pToRgbaCpu(
    uint32_t width,
    uint32_t height,
    const uint8_t* plane_y,
    uint32_t pitch_y,
    const uint8_t* plane_u,
    uint32_t pitch_u,
    const uint8_t* plane_v,
    uint32_t pitch_v,
    const uint8_t* plane_a,
    uint32_t pitch_a,
    Bink2ColorMatrix matrix,
    Bink2PixelFormat pixel_format,
    std::vector<uint8_t>& out_rgba);

// Live D3D12 compute-shader planar YUV 4:2:0 (+ optional Alpha) -> RGBA8/BGRA8 converter
// supporting BT.601 & BT.709 matrices on both HW GPU and D3D12 WARP with seamless CPU fallback.
class Bink2GpuColorConverter {
public:
    Bink2GpuColorConverter();
    ~Bink2GpuColorConverter();

    Bink2GpuColorConverter(const Bink2GpuColorConverter&) = delete;
    Bink2GpuColorConverter& operator=(const Bink2GpuColorConverter&) = delete;

    // Initializes D3D12 compute pipeline on HW GPU (prefer_warp=false) or WARP (prefer_warp=true).
    Status Initialize(bool prefer_warp = false, bool allow_cpu_fallback = true);
    void Shutdown();

    bool IsGpuReady() const noexcept;
    Bink2GpuConverterStats Stats() const;

    Status ConvertYuv420pToRgba(
        uint32_t width,
        uint32_t height,
        const uint8_t* plane_y,
        uint32_t pitch_y,
        const uint8_t* plane_u,
        uint32_t pitch_u,
        const uint8_t* plane_v,
        uint32_t pitch_v,
        const uint8_t* plane_a,
        uint32_t pitch_a,
        Bink2ColorMatrix matrix,
        Bink2PixelFormat pixel_format,
        std::vector<uint8_t>& out_rgba,
        bool* out_used_gpu = nullptr);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Encoder: generates a valid Bink2 (KB2i/KB2j/BIKi) container from RGBA8 frames
// using BT.709 YCbCr 4:2:0 / 4:4:4 planes, 8x8 2D DCT keyframes, and motion/residual P-frames.
Status Bink2EncodeTestStream(
    const std::vector<std::vector<uint8_t>>& rgba_frames,
    const Bink2EncodeParams& params,
    std::vector<uint8_t>& out_container);

class Bink2Decoder {
public:
    Bink2Decoder() = default;

    // Parses container header and frame index table from an in-memory Bink/Bink2 byte buffer.
    Status OpenFromMemory(const uint8_t* data, size_t size);
    Status OpenFromMemory(std::vector<uint8_t> data);
    void Close();

    bool IsOpen() const noexcept { return is_open_; }
    const Bink2ContainerHeader& Header() const noexcept { return header_; }
    uint32_t CurrentFrameIndex() const noexcept { return current_frame_; }

    // Decodes the current frame (or next sequential frame) into RGBA8 or BGRA8.
    Status DecodeFrame(
        uint32_t frame_index,
        Bink2PixelFormat pixel_format,
        Bink2DecodedFrame& out_frame);

    // Seeks to `frame_index` by locating the nearest preceding keyframe and decoding forward.
    Status SeekToFrame(
        uint32_t frame_index,
        Bink2PixelFormat pixel_format,
        Bink2DecodedFrame& out_frame);

private:
    Status DecodeSingleFrameInternal(uint32_t frame_index);
    void ConvertPlanesToPixels(Bink2PixelFormat pixel_format, std::vector<uint8_t>& out_pixels) const;

    bool is_open_ = false;
    std::vector<uint8_t> container_;
    Bink2ContainerHeader header_{};

    uint32_t plane_w_y_ = 0;
    uint32_t plane_h_y_ = 0;
    uint32_t plane_w_c_ = 0;
    uint32_t plane_h_c_ = 0;

    std::vector<uint8_t> plane_y_;
    std::vector<uint8_t> plane_cb_;
    std::vector<uint8_t> plane_cr_;

    std::vector<uint8_t> ref_y_;
    std::vector<uint8_t> ref_cb_;
    std::vector<uint8_t> ref_cr_;

    uint32_t current_frame_ = 0;
    bool has_decoded_frame_ = false;
    uint32_t last_decoded_frame_ = 0;
};

}  // namespace openscarlett
