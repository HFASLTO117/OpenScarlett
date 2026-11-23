#define OPENSCARLETT_BUILDING_BINK2 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/middleware/bink2_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kModName = "Bink2Scarlett.dll";

struct BinkSession {
    BinkStruct pub{};  // Must be first member so HBink == &session->pub
    openscarlett::Bink2Decoder decoder;
    openscarlett::Bink2DecodedFrame cached_frame{};
    bool has_cached_frame = false;
    bool needs_seek = false;
    uint32_t total_decoded_frames = 0;
};

struct BinkGlobalState {
    std::mutex mutex;
    std::string last_error;
    std::unordered_map<HBink, std::unique_ptr<BinkSession>> sessions;
    std::mutex gpu_mutex;
    std::unique_ptr<openscarlett::Bink2GpuColorConverter> gpu_hw;
    std::unique_ptr<openscarlett::Bink2GpuColorConverter> gpu_warp;
};

BinkGlobalState& GetBinkState() {
    static BinkGlobalState* s = new BinkGlobalState();
    return *s;
}

void SetLastErrorStr(const std::string& msg) {
    auto& st = GetBinkState();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.last_error = msg;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

inline void RecordErr(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Error, OS_SHIM_E_FAIL, -1);
}

BinkSession* LookupSessionLocked(BinkGlobalState& st, HBink bink) {
    if (!bink) return nullptr;
    auto it = st.sessions.find(bink);
    return it == st.sessions.end() ? nullptr : it->second.get();
}

HBink CreateSessionFromBytes(std::vector<uint8_t> bytes, uint32_t flags, const char* sym) {
    auto session = std::make_unique<BinkSession>();
    openscarlett::Status st = session->decoder.OpenFromMemory(std::move(bytes));
    if (!st.ok()) {
        SetLastErrorStr(st.message.empty() ? "Failed to open Bink2 container" : st.message);
        RecordErr(sym);
        return nullptr;
    }

    const auto& hdr = session->decoder.Header();
    session->pub.Width = hdr.width;
    session->pub.Height = hdr.height;
    session->pub.Frames = hdr.frame_count;
    session->pub.FrameNum = 1u;
    session->pub.LastFrameNum = 0u;
    session->pub.FrameRate = hdr.fps_num;
    session->pub.FrameRateDiv = hdr.fps_den;
    session->pub.OpenFlags = flags;
    session->pub.Paused = 0u;
    session->pub.SoundOn = 1u;

    HBink handle = &session->pub;
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        g.last_error.clear();
        g.sessions[handle] = std::move(session);
    }
    RecordOk(sym);
    return handle;
}

}  // namespace

extern "C" {

HBink OS_SHIM_CALL BinkOpen(const char* name, uint32_t flags) {
    if (!name || !*name) {
        SetLastErrorStr("BinkOpen: null or empty file path");
        RecordErr("BinkOpen");
        return nullptr;
    }
    std::ifstream ifs(name, std::ios::binary | std::ios::ate);
    if (!ifs) {
        SetLastErrorStr(std::string("BinkOpen: unable to open file: ") + name);
        RecordErr("BinkOpen");
        return nullptr;
    }
    const std::streamoff sz = ifs.tellg();
    if (sz <= 0) {
        SetLastErrorStr("BinkOpen: empty file");
        RecordErr("BinkOpen");
        return nullptr;
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    ifs.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!ifs) {
        SetLastErrorStr("BinkOpen: read failed");
        RecordErr("BinkOpen");
        return nullptr;
    }
    return CreateSessionFromBytes(std::move(bytes), flags, "BinkOpen");
}

HBink OS_SHIM_CALL BinkOpenBuffer(const void* buffer, size_t bufferSize, uint32_t flags) {
    if (!buffer || bufferSize == 0u) {
        SetLastErrorStr("BinkOpenBuffer: invalid memory buffer");
        RecordErr("BinkOpenBuffer");
        return nullptr;
    }
    const auto* src = static_cast<const uint8_t*>(buffer);
    std::vector<uint8_t> bytes(src, src + bufferSize);
    return CreateSessionFromBytes(std::move(bytes), flags, "BinkOpenBuffer");
}

void OS_SHIM_CALL BinkClose(HBink bink) {
    if (bink) {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        auto it = g.sessions.find(bink);
        if (it != g.sessions.end()) {
            it->second->decoder.Close();
            g.sessions.erase(it);
        }
    }
    RecordOk("BinkClose");
}

int32_t OS_SHIM_CALL BinkDoFrame(HBink bink) {
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || s->pub.Frames == 0u) {
        g.last_error = "BinkDoFrame: invalid Bink handle";
        RecordErr("BinkDoFrame");
        return -1;
    }

    const uint32_t zero_idx = (s->pub.FrameNum > 0u) ? (s->pub.FrameNum - 1u) : 0u;
    openscarlett::Status st = s->needs_seek
        ? s->decoder.SeekToFrame(zero_idx, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame)
        : s->decoder.DecodeFrame(zero_idx, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame);

    if (!st.ok()) {
        g.last_error = st.message.empty() ? "BinkDoFrame: decode failed" : st.message;
        RecordErr("BinkDoFrame");
        return -1;
    }

    s->needs_seek = false;
    s->has_cached_frame = true;
    s->pub.LastFrameNum = s->pub.FrameNum;
    ++s->total_decoded_frames;
    g.last_error.clear();
    RecordOk("BinkDoFrame");
    return 0;
}

void OS_SHIM_CALL BinkNextFrame(HBink bink) {
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || s->pub.Frames == 0u) {
        RecordErr("BinkNextFrame");
        return;
    }
    if (s->pub.FrameNum >= s->pub.Frames) {
        s->pub.FrameNum = 1u;
        s->needs_seek = true;
    } else {
        ++s->pub.FrameNum;
    }
    RecordOk("BinkNextFrame");
}

int32_t OS_SHIM_CALL BinkWait(HBink bink) {
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s) {
        RecordErr("BinkWait");
        return 1;
    }
    RecordOk("BinkWait");
    return s->pub.Paused != 0u ? 1 : 0;
}

int32_t OS_SHIM_CALL BinkCopyToBuffer(
    HBink bink,
    void* dest,
    int32_t destPitch,
    uint32_t destHeight,
    uint32_t destX,
    uint32_t destY,
    uint32_t flags) {
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || !dest || destPitch <= 0 || destHeight == 0u) {
        g.last_error = "BinkCopyToBuffer: invalid arguments";
        RecordErr("BinkCopyToBuffer");
        return -1;
    }

    if (!s->has_cached_frame) {
        const uint32_t zero_idx = (s->pub.FrameNum > 0u) ? (s->pub.FrameNum - 1u) : 0u;
        openscarlett::Status st = s->decoder.SeekToFrame(
            zero_idx, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame);
        if (!st.ok()) {
            g.last_error = st.message.empty() ? "BinkCopyToBuffer: decode failed" : st.message;
            RecordErr("BinkCopyToBuffer");
            return -1;
        }
        s->has_cached_frame = true;
    }

    const uint32_t src_w = s->cached_frame.width;
    const uint32_t src_h = s->cached_frame.height;
    if (destY >= destHeight || static_cast<uint32_t>(destPitch) < (destX + src_w) * 4u) {
        g.last_error = "BinkCopyToBuffer: destination buffer too small for pitch/offset";
        RecordErr("BinkCopyToBuffer");
        return -1;
    }

    const uint32_t copy_rows = std::min(src_h, destHeight - destY);
    const bool want_bgra = ((flags & 0xFFu) == OS_BINKSURFACE32);
    auto* dst_base = static_cast<uint8_t*>(dest);
    const uint8_t* src_base = s->cached_frame.pixels.data();

    for (uint32_t y = 0; y < copy_rows; ++y) {
        uint8_t* dst_row =
            dst_base + static_cast<size_t>(destY + y) * static_cast<size_t>(destPitch) +
            static_cast<size_t>(destX) * 4u;
        const uint8_t* src_row = src_base + static_cast<size_t>(y) * static_cast<size_t>(src_w) * 4u;
        if (!want_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(src_w) * 4u);
        } else {
            for (uint32_t x = 0; x < src_w; ++x) {
                dst_row[x * 4u + 0u] = src_row[x * 4u + 2u];
                dst_row[x * 4u + 1u] = src_row[x * 4u + 1u];
                dst_row[x * 4u + 2u] = src_row[x * 4u + 0u];
                dst_row[x * 4u + 3u] = src_row[x * 4u + 3u];
            }
        }
    }

    g.last_error.clear();
    RecordOk("BinkCopyToBuffer");
    return 0;
}

void OS_SHIM_CALL BinkGoto(HBink bink, uint32_t frame, uint32_t /*flags*/) {
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || s->pub.Frames == 0u) {
        g.last_error = "BinkGoto: invalid handle";
        RecordErr("BinkGoto");
        return;
    }
    const uint32_t clamped = std::clamp(frame == 0u ? 1u : frame, 1u, s->pub.Frames);
    s->pub.FrameNum = clamped;
    openscarlett::Status st = s->decoder.SeekToFrame(
        clamped - 1u, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame);
    if (!st.ok()) {
        s->needs_seek = true;
        g.last_error = st.message.empty() ? "BinkGoto: seek failed" : st.message;
        RecordErr("BinkGoto");
        return;
    }
    s->needs_seek = false;
    s->has_cached_frame = true;
    s->pub.LastFrameNum = clamped;
    g.last_error.clear();
    RecordOk("BinkGoto");
}

void OS_SHIM_CALL BinkGetSummary(HBink bink, BinkSummary* summary) {
    if (!summary) {
        RecordErr("BinkGetSummary");
        return;
    }
    std::memset(summary, 0, sizeof(BinkSummary));
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s) {
        RecordErr("BinkGetSummary");
        return;
    }
    const auto& hdr = s->decoder.Header();
    summary->Width = hdr.width;
    summary->Height = hdr.height;
    summary->TotalFrames = hdr.frame_count;
    summary->CurrentFrame = s->pub.FrameNum;
    summary->FrameRate = hdr.fps_num;
    summary->FrameRateDiv = hdr.fps_den;
    summary->TotalDecodedFrames = s->total_decoded_frames;
    summary->AudioTrackCount = hdr.audio_track_count;
    summary->IsBink2 = hdr.is_bink2 ? 1u : 0u;
    std::snprintf(summary->FourCC, sizeof(summary->FourCC), "%s", hdr.fourcc);
    RecordOk("BinkGetSummary");
}

const char* OS_SHIM_CALL BinkGetError(void) {
    RecordOk("BinkGetError");
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    static thread_local std::string tls_err;
    tls_err = g.last_error;
    return tls_err.c_str();
}

void OS_SHIM_CALL BinkSetSoundOnOff(HBink bink, int32_t onOff) {
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (s) {
        s->pub.SoundOn = onOff != 0 ? 1u : 0u;
        RecordOk("BinkSetSoundOnOff");
    } else {
        RecordErr("BinkSetSoundOnOff");
    }
}

int32_t OS_SHIM_CALL BinkPause(HBink bink, int32_t pause) {
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s) {
        RecordErr("BinkPause");
        return 0;
    }
    s->pub.Paused = pause != 0 ? 1u : 0u;
    RecordOk("BinkPause");
    return static_cast<int32_t>(s->pub.Paused);
}

// Live GPU YUV420p(+A) -> RGBA8/BGRA8 compute-shader conversion export (BT.709 / BT.601;
// supports HW GPU when preferWarp==0 and D3D12 WARP when preferWarp!=0, with CPU fallback).
OS_BINK2_API int32_t OS_SHIM_CALL BinkConvertYuvToRgbaGpu(
    uint32_t width,
    uint32_t height,
    const uint8_t* planeY,
    uint32_t pitchY,
    const uint8_t* planeU,
    uint32_t pitchU,
    const uint8_t* planeV,
    uint32_t pitchV,
    const uint8_t* planeA,
    uint32_t pitchA,
    uint32_t matrixMode,
    uint32_t pixelFormat,
    uint32_t preferWarp,
    uint8_t* outPixels,
    size_t outPixelsCapacity,
    uint32_t* outUsedGpu) {
    if (outUsedGpu) *outUsedGpu = 0u;
    if (!outPixels || width == 0u || height == 0u) {
        SetLastErrorStr("BinkConvertYuvToRgbaGpu: invalid arguments");
        RecordErr("BinkConvertYuvToRgbaGpu");
        return -1;
    }
    const uint64_t required = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
    if (required > outPixelsCapacity) {
        SetLastErrorStr("BinkConvertYuvToRgbaGpu: output buffer too small");
        RecordErr("BinkConvertYuvToRgbaGpu");
        return -1;
    }

    auto& g = GetBinkState();
    std::lock_guard<std::mutex> glk(g.gpu_mutex);
    auto& conv_ptr = (preferWarp != 0u) ? g.gpu_warp : g.gpu_hw;
    if (!conv_ptr) {
        conv_ptr = std::make_unique<openscarlett::Bink2GpuColorConverter>();
        (void)conv_ptr->Initialize(preferWarp != 0u, /*allow_cpu_fallback=*/true);
    }

    std::vector<uint8_t> rgba;
    bool used_gpu = false;
    const auto mat = (matrixMode != 0u)
                         ? openscarlett::Bink2ColorMatrix::Bt601
                         : openscarlett::Bink2ColorMatrix::Bt709;
    const auto pfmt = (pixelFormat != 0u)
                          ? openscarlett::Bink2PixelFormat::Bgra8
                          : openscarlett::Bink2PixelFormat::Rgba8;
    const openscarlett::Status st = conv_ptr->ConvertYuv420pToRgba(
        width, height, planeY, pitchY, planeU, pitchU, planeV, pitchV, planeA, pitchA,
        mat, pfmt, rgba, &used_gpu);
    if (!st.ok() || rgba.size() != static_cast<size_t>(required)) {
        SetLastErrorStr(st.message.empty() ? "BinkConvertYuvToRgbaGpu failed" : st.message);
        RecordErr("BinkConvertYuvToRgbaGpu");
        return -1;
    }
    std::memcpy(outPixels, rgba.data(), rgba.size());
    if (outUsedGpu) *outUsedGpu = used_gpu ? 1u : 0u;
    RecordOk("BinkConvertYuvToRgbaGpu");
    return 0;
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkGetGpuConverterStats(
    uint32_t preferWarp,
    uint32_t* outUsingGpu,
    uint32_t* outIsWarp,
    uint64_t* outGpuConversions,
    uint64_t* outCpuFallbacks) {
    if (outUsingGpu) *outUsingGpu = 0u;
    if (outIsWarp) *outIsWarp = 0u;
    if (outGpuConversions) *outGpuConversions = 0ull;
    if (outCpuFallbacks) *outCpuFallbacks = 0ull;
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> glk(g.gpu_mutex);
    auto& conv_ptr = (preferWarp != 0u) ? g.gpu_warp : g.gpu_hw;
    if (!conv_ptr) {
        RecordOk("BinkGetGpuConverterStats");
        return 0;
    }
    const auto st = conv_ptr->Stats();
    if (outUsingGpu) *outUsingGpu = st.using_gpu ? 1u : 0u;
    if (outIsWarp) *outIsWarp = st.is_warp ? 1u : 0u;
    if (outGpuConversions) *outGpuConversions = st.gpu_conversions;
    if (outCpuFallbacks) *outCpuFallbacks = st.cpu_fallback_conversions;
    RecordOk("BinkGetGpuConverterStats");
    return 0;
}

}  // extern "C"
