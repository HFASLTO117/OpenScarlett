#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

enum class CapturedDrawType : uint8_t {
    DrawInstanced = 0,
    DrawIndexedInstanced = 1,
    DispatchMesh = 2,
    Dispatch = 3,
    ExecuteIndirect = 4,
    Unknown = 5,
};

const char* CapturedDrawTypeName(CapturedDrawType type) noexcept;

struct CapturedDescriptorBinding {
    uint32_t root_parameter_index = 0;
    std::string kind;  // "CBV", "SRV", "UAV", "Sampler", "DescriptorTable"
    uint64_t gpu_va = 0;
    uint32_t descriptor_count = 1;
    std::string label;
};

struct CapturedDrawSummary {
    uint32_t draw_index = 0;
    CapturedDrawType type = CapturedDrawType::Unknown;
    std::string label;
    std::string dir_name;
    uint32_t shader_count = 0;
    uint32_t root_arg_count = 0;
    uint32_t descriptor_count = 0;
    uint32_t color_width = 0;
    uint32_t color_height = 0;
    uint32_t depth_width = 0;
    uint32_t depth_height = 0;
};

// Per-draw frame & shader artifact dumper gated by OPENSCARLETT_CAPTURE_DIR or Initialize().
// Zero overhead when disabled (single atomic load fast-path).
// Writes pure C++20 PNGs (valid CRC-32 + Adler-32 zlib-stored IDAT, zero external libraries).
class FrameCaptureRecorder {
public:
    FrameCaptureRecorder();
    ~FrameCaptureRecorder();

    FrameCaptureRecorder(const FrameCaptureRecorder&) = delete;
    FrameCaptureRecorder& operator=(const FrameCaptureRecorder&) = delete;

    static FrameCaptureRecorder& Global();

    // Reads OPENSCARLETT_CAPTURE_DIR (and optional OPENSCARLETT_CAPTURE_MAX_DRAWS).
    // Leaves recorder disabled (returns Ok) if OPENSCARLETT_CAPTURE_DIR is unset/empty.
    Status InitializeFromEnv(uint32_t default_max_draws = 64);

    // Explicitly enables capture into `capture_dir` for up to `max_draws` draws.
    Status Initialize(const std::string& capture_dir, uint32_t max_draws = 64);

    // Disables capture and resets recorded state.
    void Reset();

    // Fast-path inline check: zero lock/allocation overhead when disabled.
    bool IsEnabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    // Begins capturing a single draw call (#0..#N). Returns false if disabled or limit reached.
    bool BeginDraw(uint32_t draw_index, CapturedDrawType type, const std::string& label = {});

    void AttachShaderBlob(const std::string& stage, const uint8_t* bytes, size_t size);
    void AttachShaderBlob(const std::string& stage, const std::vector<uint8_t>& bytes);

    void AttachRootArguments(const std::vector<uint32_t>& dwords);
    void AttachDescriptorBinding(const CapturedDescriptorBinding& binding);

    void AttachColorTargetRgba8(uint32_t width,
                                uint32_t height,
                                const uint8_t* rgba_bytes,
                                size_t byte_size);
    void AttachColorTargetRgba8(uint32_t width,
                                uint32_t height,
                                const std::vector<uint8_t>& rgba_bytes);

    void AttachDepthTargetF32(uint32_t width,
                              uint32_t height,
                              const float* depth_floats,
                              size_t count);
    void AttachDepthTargetF32(uint32_t width,
                              uint32_t height,
                              const std::vector<float>& depth_floats);

    // Flushes the active draw to <capture_dir>/draw_XXXX/ (metadata.json, shader_*.bin, PNGs).
    Status EndDraw();

    // Writes <capture_dir>/capture_index.json summarizing all captured draws.
    Status WriteCaptureIndexJson() const;

    std::vector<CapturedDrawSummary> GetCapturedDraws() const;
    std::string GetCaptureDir() const;

private:
    std::atomic<bool> enabled_{false};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pure C++20 uncompressed/zlib-stored PNG encoders (valid IEEE CRC-32 & Adler-32).
Status EncodePngRgba8(uint32_t width,
                      uint32_t height,
                      const uint8_t* rgba_bytes,
                      size_t byte_size,
                      std::vector<uint8_t>& out_png);

Status EncodePngGray8(uint32_t width,
                      uint32_t height,
                      const uint8_t* gray_bytes,
                      size_t byte_size,
                      std::vector<uint8_t>& out_png);

}  // namespace openscarlett
