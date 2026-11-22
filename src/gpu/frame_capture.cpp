#include "openscarlett/gpu/frame_capture.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace openscarlett {

namespace {

// ---------------------------------------------------------------------------
// Environment & String Utilities
// ---------------------------------------------------------------------------
std::string ReadEnvVarA(const char* name) {
#if defined(_WIN32)
    const DWORD needed = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string buf(needed, '\0');
    const DWORD written = ::GetEnvironmentVariableA(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return buf;
#else
    const char* raw = std::getenv(name);
    return raw ? std::string(raw) : std::string{};
#endif
}

std::string SanitizeFilenameToken(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        const unsigned char b = static_cast<unsigned char>(ch);
        if ((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_' || b == '-') {
            out.push_back(ch);
        } else if (b >= 'A' && b <= 'Z') {
            out.push_back(static_cast<char>(b - 'A' + 'a'));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        out = "stage";
    }
    return out;
}

std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const unsigned char b = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (b < 0x20u) {
                    char ubuf[8]{};
                    std::snprintf(ubuf, sizeof(ubuf), "\\u%04X", static_cast<unsigned>(b));
                    out.append(ubuf);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

std::string FormatHex64(uint64_t val) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(val));
    return std::string(buf);
}

std::string FormatHex32(uint32_t val) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(val));
    return std::string(buf);
}

uint64_t ComputeFnv1a64(const uint8_t* data, size_t size) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Pure C++20 PNG Writer (CRC-32 IEEE 802.3 + Adler-32 + Zlib Stored Blocks)
// ---------------------------------------------------------------------------
uint32_t ComputeCrc32(const uint8_t* data, size_t len, uint32_t initial = 0xFFFFFFFFu) noexcept {
    uint32_t crc = initial;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t ComputeAdler32(const uint8_t* data, size_t len) noexcept {
    constexpr uint32_t kModAdler = 65521u;
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + static_cast<uint32_t>(data[i])) % kModAdler;
        s2 = (s2 + s1) % kModAdler;
    }
    return (s2 << 16u) | s1;
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
}

void AppendPngChunk(std::vector<uint8_t>& out,
                    const char type[4],
                    const uint8_t* payload,
                    size_t payload_len) {
    AppendBe32(out, static_cast<uint32_t>(payload_len));
    const uint8_t type_bytes[4] = {
        static_cast<uint8_t>(type[0]),
        static_cast<uint8_t>(type[1]),
        static_cast<uint8_t>(type[2]),
        static_cast<uint8_t>(type[3]),
    };
    out.insert(out.end(), type_bytes, type_bytes + 4);
    if (payload && payload_len > 0) {
        out.insert(out.end(), payload, payload + payload_len);
    }
    uint32_t crc = ComputeCrc32(type_bytes, 4, 0xFFFFFFFFu);
    if (payload && payload_len > 0) {
        crc = ComputeCrc32(payload, payload_len, crc);
    }
    AppendBe32(out, ~crc);
}

Status EncodePngInternal(uint32_t width,
                         uint32_t height,
                         uint8_t color_type,
                         uint32_t bytes_per_pixel,
                         const uint8_t* pixels,
                         size_t pixel_bytes,
                         std::vector<uint8_t>& out_png) {
    out_png.clear();
    if (width == 0 || height == 0 || !pixels) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid PNG dimensions or null buffer");
    }
    const uint64_t row_bytes_64 = static_cast<uint64_t>(width) * bytes_per_pixel;
    const uint64_t expected_bytes_64 = row_bytes_64 * height;
    if (expected_bytes_64 == 0 || expected_bytes_64 > static_cast<uint64_t>(pixel_bytes)) {
        return Status::Error(StatusCode::InvalidArgument, "Pixel buffer smaller than width*height*bpp");
    }
    const size_t row_bytes = static_cast<size_t>(row_bytes_64);

    // Build filtered scanlines (filter type 0 = None per row).
    std::vector<uint8_t> raw_scanlines;
    raw_scanlines.reserve(static_cast<size_t>(height) * (row_bytes + 1u));
    for (uint32_t y = 0; y < height; ++y) {
        raw_scanlines.push_back(0x00u);
        const uint8_t* row = pixels + static_cast<size_t>(y) * row_bytes;
        raw_scanlines.insert(raw_scanlines.end(), row, row + row_bytes);
    }

    // Wrap raw scanlines in a valid RFC 1950 zlib stream with RFC 1951 stored DEFLATE blocks.
    std::vector<uint8_t> zlib_stream;
    const size_t max_block = 65535u;
    const size_t num_blocks = std::max<size_t>(1u, (raw_scanlines.size() + max_block - 1u) / max_block);
    zlib_stream.reserve(2u + raw_scanlines.size() + num_blocks * 5u + 4u);

    // Zlib header: CMF=0x78 (DEFLATE, 32K window), FLG=0x01 ((0x78*256 + 0x01) % 31 == 0).
    zlib_stream.push_back(0x78u);
    zlib_stream.push_back(0x01u);

    size_t offset = 0;
    for (size_t b = 0; b < num_blocks; ++b) {
        const size_t remain = raw_scanlines.size() - offset;
        const uint16_t chunk_len = static_cast<uint16_t>(std::min<size_t>(remain, max_block));
        const bool is_final = (b + 1u == num_blocks);
        zlib_stream.push_back(is_final ? 0x01u : 0x00u);  // BFINAL | BTYPE=00 (stored)
        AppendLe16(zlib_stream, chunk_len);
        AppendLe16(zlib_stream, static_cast<uint16_t>(~chunk_len));
        if (chunk_len > 0) {
            zlib_stream.insert(zlib_stream.end(),
                               raw_scanlines.data() + offset,
                               raw_scanlines.data() + offset + chunk_len);
            offset += chunk_len;
        }
    }

    const uint32_t adler = ComputeAdler32(raw_scanlines.data(), raw_scanlines.size());
    AppendBe32(zlib_stream, adler);

    // PNG Signature
    static constexpr uint8_t kPngSig[8] = {0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    out_png.reserve(8u + 25u + zlib_stream.size() + 24u);
    out_png.insert(out_png.end(), kPngSig, kPngSig + sizeof(kPngSig));

    // IHDR chunk (13 bytes)
    std::vector<uint8_t> ihdr;
    ihdr.reserve(13u);
    AppendBe32(ihdr, width);
    AppendBe32(ihdr, height);
    ihdr.push_back(8u);          // bit depth
    ihdr.push_back(color_type);  // 6 = RGBA, 0 = Grayscale
    ihdr.push_back(0u);          // compression = deflate
    ihdr.push_back(0u);          // filter = adaptive
    ihdr.push_back(0u);          // interlace = none
    AppendPngChunk(out_png, "IHDR", ihdr.data(), ihdr.size());

    // IDAT chunk
    AppendPngChunk(out_png, "IDAT", zlib_stream.data(), zlib_stream.size());

    // IEND chunk
    AppendPngChunk(out_png, "IEND", nullptr, 0);
    return Status::Ok();
}

Status WriteBinaryFile(const std::filesystem::path& path, const uint8_t* data, size_t size) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::Error(StatusCode::IoError, "Failed to open capture file: " + path.string());
    }
    if (data && size > 0) {
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!ofs.good()) {
        return Status::Error(StatusCode::IoError, "Failed to write capture file: " + path.string());
    }
    return Status::Ok();
}

}  // namespace

const char* CapturedDrawTypeName(CapturedDrawType type) noexcept {
    switch (type) {
        case CapturedDrawType::DrawInstanced:        return "DrawInstanced";
        case CapturedDrawType::DrawIndexedInstanced: return "DrawIndexedInstanced";
        case CapturedDrawType::DispatchMesh:         return "DispatchMesh";
        case CapturedDrawType::Dispatch:             return "Dispatch";
        case CapturedDrawType::ExecuteIndirect:      return "ExecuteIndirect";
        case CapturedDrawType::Unknown:
        default:                                     return "Unknown";
    }
}

Status EncodePngRgba8(uint32_t width,
                      uint32_t height,
                      const uint8_t* rgba_bytes,
                      size_t byte_size,
                      std::vector<uint8_t>& out_png) {
    return EncodePngInternal(width, height, 6u, 4u, rgba_bytes, byte_size, out_png);
}

Status EncodePngGray8(uint32_t width,
                      uint32_t height,
                      const uint8_t* gray_bytes,
                      size_t byte_size,
                      std::vector<uint8_t>& out_png) {
    return EncodePngInternal(width, height, 0u, 1u, gray_bytes, byte_size, out_png);
}

// ===========================================================================
// FrameCaptureRecorder Implementation
// ===========================================================================
struct FrameCaptureRecorder::Impl {
    mutable std::mutex mutex;
    std::string capture_dir;
    uint32_t max_draws = 64;
    std::vector<CapturedDrawSummary> summaries;

    struct ShaderBlob {
        std::string stage;
        std::vector<uint8_t> bytes;
    };

    struct ActiveDraw {
        bool active = false;
        uint32_t draw_index = 0;
        CapturedDrawType type = CapturedDrawType::Unknown;
        std::string label;
        std::vector<ShaderBlob> shaders;
        std::vector<uint32_t> root_args;
        std::vector<CapturedDescriptorBinding> descriptors;
        uint32_t color_width = 0;
        uint32_t color_height = 0;
        std::vector<uint8_t> color_rgba;
        uint32_t depth_width = 0;
        uint32_t depth_height = 0;
        std::vector<float> depth_f32;
    } current;
};

FrameCaptureRecorder::FrameCaptureRecorder() : impl_(std::make_unique<Impl>()) {}

FrameCaptureRecorder::~FrameCaptureRecorder() = default;

FrameCaptureRecorder& FrameCaptureRecorder::Global() {
    static FrameCaptureRecorder recorder;
    return recorder;
}

Status FrameCaptureRecorder::InitializeFromEnv(uint32_t default_max_draws) {
    const std::string dir = ReadEnvVarA("OPENSCARLETT_CAPTURE_DIR");
    if (dir.empty()) {
        Reset();
        return Status::Ok();
    }
    uint32_t max_draws = default_max_draws;
    const std::string max_str = ReadEnvVarA("OPENSCARLETT_CAPTURE_MAX_DRAWS");
    if (!max_str.empty()) {
        char* end_ptr = nullptr;
        const unsigned long parsed = std::strtoul(max_str.c_str(), &end_ptr, 10);
        if (end_ptr != max_str.c_str() && parsed > 0) {
            max_draws = static_cast<uint32_t>(parsed);
        }
    }
    return Initialize(dir, max_draws);
}

Status FrameCaptureRecorder::Initialize(const std::string& capture_dir, uint32_t max_draws) {
    if (capture_dir.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "capture_dir is empty");
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(capture_dir), ec);
    if (ec) {
        return Status::Error(StatusCode::IoError,
                             "Failed to create capture directory: " + ec.message());
    }

    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->capture_dir = capture_dir;
    impl_->max_draws = max_draws;
    impl_->summaries.clear();
    impl_->current = Impl::ActiveDraw{};
    enabled_.store(true, std::memory_order_release);
    return Status::Ok();
}

void FrameCaptureRecorder::Reset() {
    enabled_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->capture_dir.clear();
    impl_->max_draws = 64;
    impl_->summaries.clear();
    impl_->current = Impl::ActiveDraw{};
}

bool FrameCaptureRecorder::BeginDraw(uint32_t draw_index,
                                     CapturedDrawType type,
                                     const std::string& label) {
    if (!IsEnabled()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (impl_->max_draws > 0 && impl_->summaries.size() >= impl_->max_draws) {
        return false;
    }
    impl_->current = Impl::ActiveDraw{};
    impl_->current.active = true;
    impl_->current.draw_index = draw_index;
    impl_->current.type = type;
    impl_->current.label = label;
    return true;
}

void FrameCaptureRecorder::AttachShaderBlob(const std::string& stage,
                                            const uint8_t* bytes,
                                            size_t size) {
    if (!IsEnabled() || !bytes || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->current.active) {
        return;
    }
    Impl::ShaderBlob blob{};
    blob.stage = SanitizeFilenameToken(stage);
    blob.bytes.assign(bytes, bytes + size);
    impl_->current.shaders.push_back(std::move(blob));
}

void FrameCaptureRecorder::AttachShaderBlob(const std::string& stage,
                                            const std::vector<uint8_t>& bytes) {
    if (!bytes.empty()) {
        AttachShaderBlob(stage, bytes.data(), bytes.size());
    }
}

void FrameCaptureRecorder::AttachRootArguments(const std::vector<uint32_t>& dwords) {
    if (!IsEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->current.active) {
        return;
    }
    impl_->current.root_args = dwords;
}

void FrameCaptureRecorder::AttachDescriptorBinding(const CapturedDescriptorBinding& binding) {
    if (!IsEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->current.active) {
        return;
    }
    impl_->current.descriptors.push_back(binding);
}

void FrameCaptureRecorder::AttachColorTargetRgba8(uint32_t width,
                                                  uint32_t height,
                                                  const uint8_t* rgba_bytes,
                                                  size_t byte_size) {
    if (!IsEnabled() || width == 0 || height == 0 || !rgba_bytes) {
        return;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (byte_size < expected) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->current.active) {
        return;
    }
    impl_->current.color_width = width;
    impl_->current.color_height = height;
    impl_->current.color_rgba.assign(rgba_bytes, rgba_bytes + expected);
}

void FrameCaptureRecorder::AttachColorTargetRgba8(uint32_t width,
                                                  uint32_t height,
                                                  const std::vector<uint8_t>& rgba_bytes) {
    if (!rgba_bytes.empty()) {
        AttachColorTargetRgba8(width, height, rgba_bytes.data(), rgba_bytes.size());
    }
}

void FrameCaptureRecorder::AttachDepthTargetF32(uint32_t width,
                                                uint32_t height,
                                                const float* depth_floats,
                                                size_t count) {
    if (!IsEnabled() || width == 0 || height == 0 || !depth_floats) {
        return;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (count < expected) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->current.active) {
        return;
    }
    impl_->current.depth_width = width;
    impl_->current.depth_height = height;
    impl_->current.depth_f32.assign(depth_floats, depth_floats + expected);
}

void FrameCaptureRecorder::AttachDepthTargetF32(uint32_t width,
                                                uint32_t height,
                                                const std::vector<float>& depth_floats) {
    if (!depth_floats.empty()) {
        AttachDepthTargetF32(width, height, depth_floats.data(), depth_floats.size());
    }
}

Status FrameCaptureRecorder::EndDraw() {
    if (!IsEnabled()) {
        return Status::Ok();
    }

    Impl::ActiveDraw draw{};
    std::string capture_dir;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        if (!impl_->current.active) {
            return Status::Ok();
        }
        draw = std::move(impl_->current);
        impl_->current = Impl::ActiveDraw{};
        capture_dir = impl_->capture_dir;
    }

    char dir_buf[32]{};
    std::snprintf(dir_buf, sizeof(dir_buf), "draw_%04u", static_cast<unsigned>(draw.draw_index));
    const std::string dir_name(dir_buf);
    const std::filesystem::path draw_dir = std::filesystem::path(capture_dir) / dir_name;

    std::error_code ec;
    std::filesystem::create_directories(draw_dir, ec);
    if (ec) {
        return Status::Error(StatusCode::IoError,
                             "Failed to create draw capture directory: " + ec.message());
    }

    struct ShaderMeta {
        std::string stage;
        std::string file;
        size_t size_bytes = 0;
        uint64_t hash = 0;
    };
    std::vector<ShaderMeta> shader_metas;
    shader_metas.reserve(draw.shaders.size());
    for (const auto& s : draw.shaders) {
        const std::string fname = "shader_" + s.stage + ".bin";
        const Status st = WriteBinaryFile(draw_dir / fname, s.bytes.data(), s.bytes.size());
        if (!st.ok()) {
            return st;
        }
        ShaderMeta sm{};
        sm.stage = s.stage;
        sm.file = fname;
        sm.size_bytes = s.bytes.size();
        sm.hash = ComputeFnv1a64(s.bytes.data(), s.bytes.size());
        shader_metas.push_back(std::move(sm));
    }

    bool wrote_color = false;
    if (draw.color_width > 0 && draw.color_height > 0 && !draw.color_rgba.empty()) {
        std::vector<uint8_t> png_bytes;
        const Status enc_st = EncodePngRgba8(draw.color_width,
                                             draw.color_height,
                                             draw.color_rgba.data(),
                                             draw.color_rgba.size(),
                                             png_bytes);
        if (!enc_st.ok()) {
            return enc_st;
        }
        const Status wr_st = WriteBinaryFile(draw_dir / "color_rt0.png", png_bytes.data(), png_bytes.size());
        if (!wr_st.ok()) {
            return wr_st;
        }
        wrote_color = true;
    }

    bool wrote_depth = false;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
    if (draw.depth_width > 0 && draw.depth_height > 0 && !draw.depth_f32.empty()) {
        float mn = std::numeric_limits<float>::infinity();
        float mx = -std::numeric_limits<float>::infinity();
        for (float d : draw.depth_f32) {
            if (std::isfinite(d)) {
                mn = std::min(mn, d);
                mx = std::max(mx, d);
            }
        }
        if (!std::isfinite(mn) || !std::isfinite(mx)) {
            mn = 0.0f;
            mx = 1.0f;
        }
        min_depth = mn;
        max_depth = mx;
        const float span = mx - mn;

        std::vector<uint8_t> gray(draw.depth_f32.size(), 0u);
        for (size_t i = 0; i < draw.depth_f32.size(); ++i) {
            const float d = draw.depth_f32[i];
            float norm = 0.0f;
            if (std::isfinite(d)) {
                norm = (span > 1e-7f) ? ((d - mn) / span) : std::clamp(d, 0.0f, 1.0f);
            }
            norm = std::clamp(norm, 0.0f, 1.0f);
            gray[i] = static_cast<uint8_t>(std::lround(norm * 255.0f));
        }

        std::vector<uint8_t> png_bytes;
        const Status enc_st = EncodePngGray8(draw.depth_width,
                                             draw.depth_height,
                                             gray.data(),
                                             gray.size(),
                                             png_bytes);
        if (!enc_st.ok()) {
            return enc_st;
        }
        const Status wr_st = WriteBinaryFile(draw_dir / "depth_d32.png", png_bytes.data(), png_bytes.size());
        if (!wr_st.ok()) {
            return wr_st;
        }
        wrote_depth = true;
    }

    // Write metadata.json
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"schema\": \"openscarlett.frame_capture_draw.v1\",\n";
    oss << "  \"draw_index\": " << draw.draw_index << ",\n";
    oss << "  \"type\": \"" << CapturedDrawTypeName(draw.type) << "\",\n";
    oss << "  \"label\": \"" << JsonEscape(draw.label) << "\",\n";

    oss << "  \"root_arguments\": [";
    for (size_t i = 0; i < draw.root_args.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "\"" << FormatHex32(draw.root_args[i]) << "\"";
    }
    oss << "],\n";

    oss << "  \"descriptors\": [\n";
    for (size_t i = 0; i < draw.descriptors.size(); ++i) {
        const auto& d = draw.descriptors[i];
        oss << "    {"
            << "\"root_parameter_index\": " << d.root_parameter_index << ", "
            << "\"kind\": \"" << JsonEscape(d.kind) << "\", "
            << "\"gpu_va\": \"" << FormatHex64(d.gpu_va) << "\", "
            << "\"descriptor_count\": " << d.descriptor_count << ", "
            << "\"label\": \"" << JsonEscape(d.label) << "\""
            << "}" << (i + 1 < draw.descriptors.size() ? "," : "") << "\n";
    }
    oss << "  ],\n";

    oss << "  \"shaders\": [\n";
    for (size_t i = 0; i < shader_metas.size(); ++i) {
        const auto& s = shader_metas[i];
        oss << "    {"
            << "\"stage\": \"" << JsonEscape(s.stage) << "\", "
            << "\"file\": \"" << JsonEscape(s.file) << "\", "
            << "\"size_bytes\": " << s.size_bytes << ", "
            << "\"fnv1a_64\": \"" << FormatHex64(s.hash) << "\""
            << "}" << (i + 1 < shader_metas.size() ? "," : "") << "\n";
    }
    oss << "  ],\n";

    oss << "  \"color_rt0\": {"
        << "\"present\": " << (wrote_color ? "true" : "false") << ", "
        << "\"file\": \"" << (wrote_color ? "color_rt0.png" : "") << "\", "
        << "\"width\": " << (wrote_color ? draw.color_width : 0u) << ", "
        << "\"height\": " << (wrote_color ? draw.color_height : 0u)
        << "},\n";

    oss << "  \"depth_d32\": {"
        << "\"present\": " << (wrote_depth ? "true" : "false") << ", "
        << "\"file\": \"" << (wrote_depth ? "depth_d32.png" : "") << "\", "
        << "\"width\": " << (wrote_depth ? draw.depth_width : 0u) << ", "
        << "\"height\": " << (wrote_depth ? draw.depth_height : 0u) << ", "
        << "\"min_depth\": " << min_depth << ", "
        << "\"max_depth\": " << max_depth
        << "}\n";
    oss << "}\n";

    const std::string meta_json = oss.str();
    const Status meta_st = WriteBinaryFile(
        draw_dir / "metadata.json",
        reinterpret_cast<const uint8_t*>(meta_json.data()),
        meta_json.size());
    if (!meta_st.ok()) {
        return meta_st;
    }

    CapturedDrawSummary summary{};
    summary.draw_index = draw.draw_index;
    summary.type = draw.type;
    summary.label = draw.label;
    summary.dir_name = dir_name;
    summary.shader_count = static_cast<uint32_t>(shader_metas.size());
    summary.root_arg_count = static_cast<uint32_t>(draw.root_args.size());
    summary.descriptor_count = static_cast<uint32_t>(draw.descriptors.size());
    summary.color_width = wrote_color ? draw.color_width : 0u;
    summary.color_height = wrote_color ? draw.color_height : 0u;
    summary.depth_width = wrote_depth ? draw.depth_width : 0u;
    summary.depth_height = wrote_depth ? draw.depth_height : 0u;

    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        impl_->summaries.push_back(std::move(summary));
    }

    return Status::Ok();
}

Status FrameCaptureRecorder::WriteCaptureIndexJson() const {
    std::string capture_dir;
    uint32_t max_draws = 0;
    std::vector<CapturedDrawSummary> summaries;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        capture_dir = impl_->capture_dir;
        max_draws = impl_->max_draws;
        summaries = impl_->summaries;
    }

    if (capture_dir.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "FrameCaptureRecorder is not initialized");
    }

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"schema\": \"openscarlett.frame_capture_index.v1\",\n";
    oss << "  \"max_draws\": " << max_draws << ",\n";
    oss << "  \"captured_draws\": " << summaries.size() << ",\n";
    oss << "  \"draws\": [\n";
    for (size_t i = 0; i < summaries.size(); ++i) {
        const auto& s = summaries[i];
        oss << "    {"
            << "\"draw_index\": " << s.draw_index << ", "
            << "\"type\": \"" << CapturedDrawTypeName(s.type) << "\", "
            << "\"label\": \"" << JsonEscape(s.label) << "\", "
            << "\"dir\": \"" << JsonEscape(s.dir_name) << "\", "
            << "\"shader_count\": " << s.shader_count << ", "
            << "\"root_arg_count\": " << s.root_arg_count << ", "
            << "\"descriptor_count\": " << s.descriptor_count << ", "
            << "\"color_width\": " << s.color_width << ", "
            << "\"color_height\": " << s.color_height << ", "
            << "\"depth_width\": " << s.depth_width << ", "
            << "\"depth_height\": " << s.depth_height
            << "}" << (i + 1 < summaries.size() ? "," : "") << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";

    const std::string json = oss.str();
    return WriteBinaryFile(
        std::filesystem::path(capture_dir) / "capture_index.json",
        reinterpret_cast<const uint8_t*>(json.data()),
        json.size());
}

std::vector<CapturedDrawSummary> FrameCaptureRecorder::GetCapturedDraws() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->summaries;
}

std::string FrameCaptureRecorder::GetCaptureDir() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->capture_dir;
}

}  // namespace openscarlett
