#include "openscarlett/audio/audio_sync.h"
#include "openscarlett/audio/audio_trace.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define OPENSCARLETT_CHAD_API __declspec(dllexport)
#  define OPENSCARLETT_CHAD_CALL WINAPI
#else
#  define OPENSCARLETT_CHAD_API __attribute__((visibility("default")))
#  define OPENSCARLETT_CHAD_CALL
using HRESULT = int32_t;
using HANDLE = void*;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_POINTER = static_cast<HRESULT>(0x80004003);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000E);
#endif

namespace {

using openscarlett::audio::SrwLock;
using openscarlett::audio::SrwLockGuard;

constexpr const char* kModName = "chad.dll";
constexpr uint32_t kMaxDspClients = 1024u;
constexpr uint32_t kMaxDspContexts = 4096u;
constexpr uint32_t kMaxStreamLimit = 0x100u;
constexpr uint32_t kMaxPriority = 2u;
constexpr uint32_t kMaxIrFloats = 16u * 1024u * 1024u;
constexpr uint32_t kSentinelIrMetadata = 0xFFFFACE1u;

constexpr uint32_t kKindConvolution = 1u;
constexpr uint32_t kKindFft = 2u;
constexpr uint32_t kKindIfft = 3u;

constexpr uint32_t kFlagDeinterleaved = 0x2u;
constexpr uint32_t kFlagEnablePolarFormat = 0x4u;
constexpr uint32_t kFlagStereoIr = 0x8u;

constexpr uintptr_t kClientHandleTag = 0x4348414400000000ull;
constexpr uintptr_t kContextHandleTag = 0x4348535400000000ull;

bool CheckMemoryAccess(const void* ptr, size_t bytes, bool require_write) noexcept {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000ull || bytes == 0u) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    if (start + bytes < start) {
        return false;
    }
#if defined(_WIN32)
    uintptr_t cursor = start;
    const uintptr_t end = start + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
            return false;
        }
        const DWORD prot = mbi.Protect & 0xFFu;
        const bool ok = require_write
            ? (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
               prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY)
            : (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
               prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
               prot == PAGE_EXECUTE_WRITECOPY);
        if (!ok) {
            return false;
        }
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor) {
            return false;
        }
        cursor = region_end;
    }
    return true;
#else
    (void)require_write;
    return true;
#endif
}

bool IsReadablePointer(const void* ptr, size_t bytes) noexcept {
    return CheckMemoryAccess(ptr, bytes, false);
}

bool IsWritablePointer(void* ptr, size_t bytes) noexcept {
    return CheckMemoryAccess(ptr, bytes, true);
}

bool ShouldTraceCall(std::atomic<uint64_t>& counter) noexcept {
    if (!openscarlett::audio::IsRetailTraceEnabled()) {
        return false;
    }
    const uint64_t n = counter.fetch_add(1u, std::memory_order_relaxed) + 1u;
    return n <= 16u || (n & 1023u) == 0u;
}

inline HRESULT RecordHr(const char* sym, HRESULT hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        static_cast<int32_t>(hr),
        -1);
    return hr;
}

struct Complex {
    float re = 0.0f;
    float im = 0.0f;
};

void FftInPlace(std::vector<Complex>& a, bool inverse) noexcept {
    const size_t n = a.size();
    if (n <= 1u || (n & (n - 1u)) != 0u) {
        return;
    }

    for (size_t i = 1u, j = 0u; i < n; ++i) {
        size_t bit = n >> 1u;
        for (; (j & bit) != 0u; bit >>= 1u) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }

    constexpr double kTwoPi = 6.28318530717958647692;
    for (size_t len = 2u; len <= n; len <<= 1u) {
        const double ang = (inverse ? kTwoPi : -kTwoPi) / static_cast<double>(len);
        const double wlen_re = std::cos(ang);
        const double wlen_im = std::sin(ang);
        const size_t half = len >> 1u;
        for (size_t i = 0u; i < n; i += len) {
            double w_re = 1.0;
            double w_im = 0.0;
            for (size_t j = 0u; j < half; ++j) {
                const Complex u = a[i + j];
                const Complex t = a[i + j + half];
                const float wr = static_cast<float>(w_re);
                const float wi = static_cast<float>(w_im);
                const Complex v{
                    t.re * wr - t.im * wi,
                    t.re * wi + t.im * wr
                };
                a[i + j] = Complex{u.re + v.re, u.im + v.im};
                a[i + j + half] = Complex{u.re - v.re, u.im - v.im};
                const double next_re = w_re * wlen_re - w_im * wlen_im;
                const double next_im = w_re * wlen_im + w_im * wlen_re;
                w_re = next_re;
                w_im = next_im;
            }
        }
    }

    if (inverse) {
        const float inv_n = 1.0f / static_cast<float>(n);
        for (size_t i = 0u; i < n; ++i) {
            a[i].re *= inv_n;
            a[i].im *= inv_n;
        }
    }
}

struct DspActivateDesc {
    uint32_t kind = 0;
    uint32_t flags = 0;
    uint32_t block_frames = 512u;
    uint32_t channels = 1u;
    uint32_t ir_len_floats = 0;
    uint64_t impulse_response_ptr = 0;
};

DspActivateDesc ParseActivateDesc(const void* p_desc) noexcept {
    DspActivateDesc desc{};
    if (!IsReadablePointer(p_desc, 28u)) {
        return desc;
    }
    const auto* raw = static_cast<const uint8_t*>(p_desc);
    uint32_t raw_block = 0u;
    uint32_t raw_ch = 0u;
    std::memcpy(&desc.kind, raw + 0x00u, sizeof(uint32_t));
    std::memcpy(&desc.flags, raw + 0x04u, sizeof(uint32_t));
    std::memcpy(&raw_block, raw + 0x08u, sizeof(uint32_t));
    std::memcpy(&raw_ch, raw + 0x0Cu, sizeof(uint32_t));
    std::memcpy(&desc.ir_len_floats, raw + 0x10u, sizeof(uint32_t));
    std::memcpy(&desc.impulse_response_ptr, raw + 0x14u, sizeof(uint64_t));

    if (raw_block == 512u || raw_block == 1024u || raw_block == 2048u) {
        desc.block_frames = raw_block;
    } else {
        desc.block_frames = 512u;
    }

    if (raw_ch == 1u || raw_ch == 2u) {
        desc.channels = raw_ch;
    } else {
        desc.channels = 1u;
    }
    return desc;
}

struct DspCommand {
    float gain_in = 1.0f;
    float gain_out = 1.0f;
    uint64_t input_ptr = 0;
    uint64_t output_ptr = 0;
    uint64_t mult_ptr = 0;
};

DspCommand ParseDspCommand(const void* p_cmd) noexcept {
    DspCommand cmd{};
    const auto* raw = static_cast<const uint8_t*>(p_cmd);
    std::memcpy(&cmd.gain_in, raw + 0x00u, sizeof(float));
    std::memcpy(&cmd.gain_out, raw + 0x04u, sizeof(float));
    std::memcpy(&cmd.input_ptr, raw + 0x08u, sizeof(uint64_t));
    std::memcpy(&cmd.output_ptr, raw + 0x10u, sizeof(uint64_t));
    std::memcpy(&cmd.mult_ptr, raw + 0x18u, sizeof(uint64_t));
    return cmd;
}

struct alignas(16) XDspStatusBlock {
    std::atomic<uint32_t> submittedSeq{0};
    std::atomic<uint32_t> completedSeq{0};
    std::atomic<uint32_t> firstFailedSeq{0};
    std::atomic<int32_t> result{0};
};
static_assert(sizeof(XDspStatusBlock) == 16u, "XDspStatusBlock must be 16 bytes");

struct XDspClientState {
    uint64_t id = 0;
    HANDLE handle = nullptr;
    void* memory = nullptr;
    uint32_t memory_size = 0;
    uintptr_t memory_start = 0;
    uintptr_t memory_end = 0;
    uint32_t max_activations = 0;
    uint32_t max_stream_limit = 0;
    uint32_t priority = 0;
    SrwLock state_lock;
    bool active = false;
    uint64_t submitted_commands = 0;
};

struct XDspContext {
    XDspStatusBlock status{};
    uint64_t id = 0;
    HANDLE handle = nullptr;
    HANDLE client_handle = nullptr;
    uint32_t kind = 0;
    uint32_t flags = 0;
    uint32_t block_frames = 512u;
    uint32_t channels = 1u;
    uint32_t fft_size = 1024u;
    SrwLock lock;
    bool active = true;
    uint32_t next_seq = 0;

    // Partitioned overlap-save convolution state
    std::vector<std::vector<Complex>> ir_partitions[2];
    std::vector<float> overlap_input[2];
    std::vector<std::vector<Complex>> fdl[2];
    size_t fdl_head[2] = {0u, 0u};

    // Pre-allocated scratch buffers for audio-thread execution
    std::vector<Complex> work_fft;
    std::vector<Complex> work_accum;
};

void InitializeConvolutionState(XDspContext& ctx, const DspActivateDesc& desc) {
    const uint32_t B = desc.block_frames;
    const uint32_t M = 2u * B;
    ctx.fft_size = M;
    ctx.work_fft.assign(M, Complex{});
    ctx.work_accum.assign(M, Complex{});

    for (uint32_t c = 0u; c < ctx.channels; ++c) {
        ctx.overlap_input[c].assign(M, 0.0f);
    }

    if (desc.ir_len_floats == 0u || desc.ir_len_floats > kMaxIrFloats ||
        desc.impulse_response_ptr == 0u) {
        return;
    }

    const float* ir_ptr = reinterpret_cast<const float*>(
        static_cast<uintptr_t>(desc.impulse_response_ptr));
    const size_t ir_bytes = static_cast<size_t>(desc.ir_len_floats) * sizeof(float);
    if (!IsReadablePointer(ir_ptr, ir_bytes)) {
        return;
    }

    const bool stereo_ir = (desc.flags & kFlagStereoIr) != 0u;
    const bool deinterleaved = (desc.flags & kFlagDeinterleaved) != 0u;
    const uint32_t ir_channels = stereo_ir ? 2u : 1u;
    const uint32_t per_ch_len = stereo_ir ? (desc.ir_len_floats / 2u) : desc.ir_len_floats;
    if (per_ch_len == 0u) {
        return;
    }

    const uint32_t num_raw_parts = (per_ch_len + B - 1u) / B;
    std::vector<Complex> part_fft(M);

    for (uint32_t c_ir = 0u; c_ir < ir_channels; ++c_ir) {
        for (uint32_t p = 0u; p < num_raw_parts; ++p) {
            const uint32_t base = p * B;
            const uint32_t avail = (base < per_ch_len) ? std::min(B, per_ch_len - base) : 0u;
            if (avail == 0u) {
                continue;
            }

            float first_sample = 0.0f;
            if (ir_channels == 1u) {
                first_sample = ir_ptr[base];
            } else if (deinterleaved) {
                first_sample = ir_ptr[static_cast<size_t>(c_ir) * per_ch_len + base];
            } else {
                first_sample = ir_ptr[static_cast<size_t>(base) * 2u + c_ir];
            }

            uint32_t first_word = 0u;
            std::memcpy(&first_word, &first_sample, sizeof(uint32_t));
            if (first_word == kSentinelIrMetadata) {
                continue;
            }

            std::fill(part_fft.begin(), part_fft.end(), Complex{});
            for (uint32_t i = 0u; i < avail; ++i) {
                float s = 0.0f;
                if (ir_channels == 1u) {
                    s = ir_ptr[base + i];
                } else if (deinterleaved) {
                    s = ir_ptr[static_cast<size_t>(c_ir) * per_ch_len + base + i];
                } else {
                    s = ir_ptr[static_cast<size_t>(base + i) * 2u + c_ir];
                }
                part_fft[i] = Complex{s, 0.0f};
            }

            FftInPlace(part_fft, false);
            ctx.ir_partitions[c_ir].push_back(part_fft);
        }
    }

    const size_t max_parts = stereo_ir
        ? std::max(ctx.ir_partitions[0].size(), ctx.ir_partitions[1].size())
        : ctx.ir_partitions[0].size();
    if (max_parts > 0u) {
        for (uint32_t c = 0u; c < ctx.channels; ++c) {
            ctx.fdl[c].assign(max_parts, std::vector<Complex>(M, Complex{}));
            ctx.fdl_head[c] = 0u;
        }
    }
}

void ExecuteConvolution(
    XDspContext& ctx,
    const DspCommand& cmd,
    const float* p_envelope) noexcept {
    const uint32_t B = ctx.block_frames;
    const uint32_t M = ctx.fft_size;
    const uint32_t channels = ctx.channels;
    const size_t total_floats = static_cast<size_t>(channels) * B;
    const size_t buf_bytes = total_floats * sizeof(float);

    const auto* in_ptr = reinterpret_cast<const float*>(
        static_cast<uintptr_t>(cmd.input_ptr));
    auto* out_ptr = reinterpret_cast<float*>(
        static_cast<uintptr_t>(cmd.output_ptr));
    if (!IsReadablePointer(in_ptr, buf_bytes) || !IsWritablePointer(out_ptr, buf_bytes)) {
        return;
    }

    const bool deinterleaved = (ctx.flags & kFlagDeinterleaved) != 0u;
    const bool stereo_ir = (ctx.flags & kFlagStereoIr) != 0u;
    const bool has_env = (p_envelope != nullptr) &&
                         IsReadablePointer(p_envelope, static_cast<size_t>(B) * sizeof(float));

    const bool no_ir = ctx.ir_partitions[0].empty() &&
                       (!stereo_ir || ctx.ir_partitions[1].empty());
    if (no_ir) {
        const float total_gain = cmd.gain_in * cmd.gain_out;
        for (uint32_t c = 0u; c < channels; ++c) {
            for (uint32_t i = 0u; i < B; ++i) {
                const size_t idx = (channels == 1u || deinterleaved)
                    ? (static_cast<size_t>(c) * B + i)
                    : (static_cast<size_t>(i) * 2u + c);
                const float env = has_env ? p_envelope[i] : 1.0f;
                out_ptr[idx] = in_ptr[idx] * total_gain * env;
            }
        }
        return;
    }

    for (uint32_t c = 0u; c < channels; ++c) {
        const uint32_t c_ir = (stereo_ir && c < 2u) ? c : 0u;
        const auto& parts = ctx.ir_partitions[c_ir];
        auto& hist = ctx.overlap_input[c];

        std::memmove(hist.data(), hist.data() + B, static_cast<size_t>(B) * sizeof(float));
        for (uint32_t i = 0u; i < B; ++i) {
            const size_t idx = (channels == 1u || deinterleaved)
                ? (static_cast<size_t>(c) * B + i)
                : (static_cast<size_t>(i) * 2u + c);
            hist[B + i] = in_ptr[idx] * cmd.gain_in;
        }

        if (parts.empty()) {
            for (uint32_t i = 0u; i < B; ++i) {
                const size_t idx = (channels == 1u || deinterleaved)
                    ? (static_cast<size_t>(c) * B + i)
                    : (static_cast<size_t>(i) * 2u + c);
                out_ptr[idx] = 0.0f;
            }
            continue;
        }

        for (uint32_t i = 0u; i < M; ++i) {
            ctx.work_fft[i] = Complex{hist[i], 0.0f};
        }
        FftInPlace(ctx.work_fft, false);

        const size_t fdl_size = ctx.fdl[c].size();
        const size_t cur = ctx.fdl_head[c];
        ctx.fdl[c][cur] = ctx.work_fft;

        std::fill(ctx.work_accum.begin(), ctx.work_accum.end(), Complex{});
        const size_t num_parts = parts.size();
        for (size_t p = 0u; p < num_parts; ++p) {
            const size_t fdl_idx = (cur + fdl_size - p) % fdl_size;
            const auto& x_blk = ctx.fdl[c][fdl_idx];
            const auto& h_blk = parts[p];
            for (uint32_t k = 0u; k < M; ++k) {
                const float xr = x_blk[k].re;
                const float xi = x_blk[k].im;
                const float hr = h_blk[k].re;
                const float hi = h_blk[k].im;
                ctx.work_accum[k].re += xr * hr - xi * hi;
                ctx.work_accum[k].im += xr * hi + xi * hr;
            }
        }
        ctx.fdl_head[c] = (cur + 1u) % fdl_size;

        FftInPlace(ctx.work_accum, true);

        for (uint32_t i = 0u; i < B; ++i) {
            const size_t idx = (channels == 1u || deinterleaved)
                ? (static_cast<size_t>(c) * B + i)
                : (static_cast<size_t>(i) * 2u + c);
            const float env = has_env ? p_envelope[i] : 1.0f;
            out_ptr[idx] = ctx.work_accum[B + i].re * cmd.gain_out * env;
        }
    }
}

void ExecuteFft(XDspContext& ctx, const DspCommand& cmd) noexcept {
    const uint32_t N = ctx.block_frames;
    const auto* in_ptr = reinterpret_cast<const float*>(
        static_cast<uintptr_t>(cmd.input_ptr));
    auto* out_ptr = reinterpret_cast<float*>(
        static_cast<uintptr_t>(cmd.output_ptr));
    const auto* mult_ptr = reinterpret_cast<const float*>(
        static_cast<uintptr_t>(cmd.mult_ptr));

    uint32_t proc_channels = 1u;
    if (ctx.channels == 2u &&
        IsReadablePointer(in_ptr, static_cast<size_t>(2u * N) * sizeof(float)) &&
        IsWritablePointer(out_ptr, static_cast<size_t>(2u * N) * sizeof(float))) {
        proc_channels = 2u;
    } else if (!IsReadablePointer(in_ptr, static_cast<size_t>(N) * sizeof(float)) ||
               !IsWritablePointer(out_ptr, static_cast<size_t>(N) * sizeof(float))) {
        return;
    }

    const bool has_mult_2ch = (proc_channels == 2u) && (mult_ptr != nullptr) &&
                              IsReadablePointer(mult_ptr, static_cast<size_t>(2u * N) * sizeof(float));
    const bool has_mult_1ch = (mult_ptr != nullptr) &&
                              IsReadablePointer(mult_ptr, static_cast<size_t>(N) * sizeof(float));
    const float total_gain = cmd.gain_in * cmd.gain_out;
    const bool polar = (ctx.flags & kFlagEnablePolarFormat) != 0u;

    for (uint32_t c = 0u; c < proc_channels; ++c) {
        const size_t ch_off = static_cast<size_t>(c) * N;
        const float* ch_mult = has_mult_2ch ? (mult_ptr + ch_off) : (has_mult_1ch ? mult_ptr : nullptr);

        for (uint32_t i = 0u; i < N; ++i) {
            const float m = ch_mult ? ch_mult[i] : 1.0f;
            ctx.work_fft[i] = Complex{in_ptr[ch_off + i] * m * total_gain, 0.0f};
        }
        FftInPlace(ctx.work_fft, false);

        out_ptr[ch_off + 0u] = ctx.work_fft[0u].re;
        out_ptr[ch_off + 1u] = ctx.work_fft[N / 2u].re;
        for (uint32_t k = 1u; k < N / 2u; ++k) {
            const float re = ctx.work_fft[k].re;
            const float im = ctx.work_fft[k].im;
            if (polar) {
                out_ptr[ch_off + 2u * k] = std::hypot(re, im);
                out_ptr[ch_off + 2u * k + 1u] = std::atan2(im, re);
            } else {
                out_ptr[ch_off + 2u * k] = re;
                out_ptr[ch_off + 2u * k + 1u] = im;
            }
        }
    }
}

void ExecuteIfft(XDspContext& ctx, const DspCommand& cmd) noexcept {
    const uint32_t N = ctx.block_frames;
    const auto* in_ptr = reinterpret_cast<const float*>(
        static_cast<uintptr_t>(cmd.input_ptr));
    auto* out_ptr = reinterpret_cast<float*>(
        static_cast<uintptr_t>(cmd.output_ptr));

    uint32_t proc_channels = 1u;
    if (ctx.channels == 2u &&
        IsReadablePointer(in_ptr, static_cast<size_t>(2u * N) * sizeof(float)) &&
        IsWritablePointer(out_ptr, static_cast<size_t>(2u * N) * sizeof(float))) {
        proc_channels = 2u;
    } else if (!IsReadablePointer(in_ptr, static_cast<size_t>(N) * sizeof(float)) ||
               !IsWritablePointer(out_ptr, static_cast<size_t>(N) * sizeof(float))) {
        return;
    }

    const float total_gain = cmd.gain_in * cmd.gain_out;
    const bool polar = (ctx.flags & kFlagEnablePolarFormat) != 0u;

    for (uint32_t c = 0u; c < proc_channels; ++c) {
        const size_t ch_off = static_cast<size_t>(c) * N;
        ctx.work_fft[0u] = Complex{in_ptr[ch_off + 0u], 0.0f};
        ctx.work_fft[N / 2u] = Complex{in_ptr[ch_off + 1u], 0.0f};
        for (uint32_t k = 1u; k < N / 2u; ++k) {
            float re = in_ptr[ch_off + 2u * k];
            float im = in_ptr[ch_off + 2u * k + 1u];
            if (polar) {
                const float mag = re;
                const float phase = im;
                re = mag * std::cos(phase);
                im = mag * std::sin(phase);
            }
            ctx.work_fft[k] = Complex{re, im};
            ctx.work_fft[N - k] = Complex{re, -im};
        }

        FftInPlace(ctx.work_fft, true);

        for (uint32_t i = 0u; i < N; ++i) {
            out_ptr[ch_off + i] = ctx.work_fft[i].re * total_gain;
        }
    }
}

class XDspRegistry {
public:
    static XDspRegistry& Instance() {
        static XDspRegistry* reg = new XDspRegistry();
        return *reg;
    }

    HRESULT ConnectWithMaximumStreamLimit(
        void* p_memory,
        uint32_t memory_size,
        uint32_t max_activations,
        uint32_t max_stream_limit,
        uint32_t priority,
        void** pp_out_handle) {
        if (!IsWritablePointer(pp_out_handle, sizeof(void*))) {
            return E_POINTER;
        }
        *pp_out_handle = nullptr;

        if (max_stream_limit > kMaxStreamLimit || priority > kMaxPriority) {
            return E_INVALIDARG;
        }

        uintptr_t mem_start = 0u;
        uintptr_t mem_end = 0u;
        if (p_memory != nullptr && memory_size > 0u) {
            mem_start = reinterpret_cast<uintptr_t>(p_memory);
            mem_end = mem_start + static_cast<uintptr_t>(memory_size);
            if (mem_end < mem_start) {
                return E_INVALIDARG;
            }
        }

        std::shared_ptr<XDspClientState> state;
        try {
            state = std::make_shared<XDspClientState>();
        } catch (...) {
            return E_OUTOFMEMORY;
        }

        const uint64_t handle_val = next_handle_id_.fetch_add(1u, std::memory_order_relaxed);
        auto* raw_handle = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(kClientHandleTag | (handle_val & 0xFFFFFFFFull)));

        state->id = handle_val;
        state->handle = raw_handle;
        state->memory = p_memory;
        state->memory_size = memory_size;
        state->memory_start = mem_start;
        state->memory_end = mem_end;
        state->max_activations = max_activations;
        state->max_stream_limit = max_stream_limit;
        state->priority = priority;

        {
            SrwLockGuard lk(table_lock_);
            if (clients_.size() >= kMaxDspClients) {
                return E_OUTOFMEMORY;
            }
            clients_[raw_handle] = std::move(state);
        }

        *pp_out_handle = raw_handle;
        return S_OK;
    }

    HRESULT Disconnect(HANDLE client_handle) {
        if (!client_handle) {
            return E_INVALIDARG;
        }

        std::shared_ptr<XDspClientState> removed_client;
        std::vector<std::shared_ptr<XDspContext>> removed_contexts;
        {
            SrwLockGuard lk(table_lock_);
            auto it = clients_.find(client_handle);
            if (it == clients_.end()) {
                return E_INVALIDARG;
            }
            removed_client = std::move(it->second);
            clients_.erase(it);

            for (auto ctx_it = contexts_.begin(); ctx_it != contexts_.end();) {
                if (ctx_it->second && ctx_it->second->client_handle == client_handle) {
                    removed_contexts.push_back(std::move(ctx_it->second));
                    ctx_it = contexts_.erase(ctx_it);
                } else {
                    ++ctx_it;
                }
            }
        }

        for (auto& ctx : removed_contexts) {
            if (ctx) {
                SrwLockGuard lk(ctx->lock);
                ctx->active = false;
            }
        }
        if (removed_client) {
            SrwLockGuard lk(removed_client->state_lock);
            removed_client->active = false;
        }
        return S_OK;
    }

    HRESULT Activate(
        HANDLE client_handle,
        const void* p_desc,
        uint32_t** pp_out_status,
        void** pp_out_stream,
        void* ret_addr) {
        const bool has_out_status = IsWritablePointer(pp_out_status, sizeof(void*));
        const bool has_out_stream = IsWritablePointer(pp_out_stream, sizeof(void*));
        if (has_out_status) {
            *pp_out_status = nullptr;
        }
        if (has_out_stream) {
            *pp_out_stream = nullptr;
        }

        std::shared_ptr<XDspClientState> client = LookupClient(client_handle);
        if (!client) {
            return E_INVALIDARG;
        }

        const DspActivateDesc desc = ParseActivateDesc(p_desc);
        if (desc.kind != kKindConvolution && desc.kind != kKindFft && desc.kind != kKindIfft) {
            openscarlett::audio::LogUnimplAudio(
                "chad",
                ret_addr,
                "XDspActivate unknown kind=%u flags=0x%X blockFrames=%u channels=%u",
                desc.kind,
                desc.flags,
                desc.block_frames,
                desc.channels);
        }

        std::shared_ptr<XDspContext> ctx;
        try {
            ctx = std::make_shared<XDspContext>();
            ctx->client_handle = client_handle;
            ctx->kind = desc.kind;
            ctx->flags = desc.flags;
            ctx->block_frames = desc.block_frames;
            ctx->channels = desc.channels;
            ctx->fft_size = desc.block_frames;
            ctx->active = true;

            if (desc.kind == kKindConvolution) {
                InitializeConvolutionState(*ctx, desc);
            } else {
                ctx->work_fft.assign(desc.block_frames, Complex{});
            }
        } catch (...) {
            return E_OUTOFMEMORY;
        }

        const uint64_t ctx_id = next_handle_id_.fetch_add(1u, std::memory_order_relaxed);
        auto* ctx_handle = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(kContextHandleTag | (ctx_id & 0xFFFFFFFFull)));
        ctx->id = ctx_id;
        ctx->handle = ctx_handle;

        {
            SrwLockGuard lk(table_lock_);
            if (clients_.find(client_handle) == clients_.end()) {
                return E_INVALIDARG;
            }
            if (contexts_.size() >= kMaxDspContexts) {
                return E_OUTOFMEMORY;
            }
            contexts_[ctx_handle] = ctx;
        }

        {
            SrwLockGuard lk(client->state_lock);
            client->active = true;
        }

        if (has_out_status) {
            *pp_out_status = reinterpret_cast<uint32_t*>(&ctx->status);
        }
        if (has_out_stream) {
            *pp_out_stream = ctx_handle;
        }
        return S_OK;
    }

    HRESULT Deactivate(HANDLE handle) {
        if (!handle) {
            return E_INVALIDARG;
        }

        std::shared_ptr<XDspContext> removed_ctx;
        std::shared_ptr<XDspClientState> client;
        {
            SrwLockGuard lk(table_lock_);
            auto ctx_it = contexts_.find(handle);
            if (ctx_it != contexts_.end()) {
                removed_ctx = std::move(ctx_it->second);
                contexts_.erase(ctx_it);
            } else {
                auto cli_it = clients_.find(handle);
                if (cli_it != clients_.end()) {
                    client = cli_it->second;
                }
            }
        }

        if (removed_ctx) {
            SrwLockGuard lk(removed_ctx->lock);
            removed_ctx->active = false;
            return S_OK;
        }
        if (client) {
            SrwLockGuard lk(client->state_lock);
            client->active = false;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    HRESULT SubmitCommand(
        HANDLE handle,
        const void* p_cmd,
        const float* p_envelope,
        uint32_t* p_out_token) {
        if (!handle) {
            return E_INVALIDARG;
        }

        std::shared_ptr<XDspContext> ctx;
        std::shared_ptr<XDspClientState> client;
        {
            SrwLockGuard lk(table_lock_);
            auto ctx_it = contexts_.find(handle);
            if (ctx_it != contexts_.end()) {
                ctx = ctx_it->second;
            } else {
                auto cli_it = clients_.find(handle);
                if (cli_it != clients_.end()) {
                    client = cli_it->second;
                }
            }
        }

        if (ctx) {
            SrwLockGuard lk(ctx->lock);
            if (!ctx->active) {
                return E_INVALIDARG;
            }
            const uint32_t seq = ++ctx->next_seq;
            ctx->status.submittedSeq.store(seq, std::memory_order_release);
            if (IsWritablePointer(p_out_token, sizeof(uint32_t))) {
                *p_out_token = seq;
            }

            if (IsReadablePointer(p_cmd, 32u)) {
                const DspCommand cmd = ParseDspCommand(p_cmd);
                if (cmd.input_ptr != 0u && cmd.output_ptr != 0u) {
                    if (ctx->kind == kKindConvolution) {
                        ExecuteConvolution(*ctx, cmd, p_envelope);
                    } else if (ctx->kind == kKindFft) {
                        ExecuteFft(*ctx, cmd);
                    } else if (ctx->kind == kKindIfft) {
                        ExecuteIfft(*ctx, cmd);
                    }
                }
            }

            ctx->status.result.store(S_OK, std::memory_order_release);
            ctx->status.completedSeq.store(seq, std::memory_order_release);
            return S_OK;
        }

        if (client) {
            SrwLockGuard lk(client->state_lock);
            ++client->submitted_commands;
            const uint32_t seq = static_cast<uint32_t>(client->submitted_commands);
            if (IsWritablePointer(p_out_token, sizeof(uint32_t))) {
                *p_out_token = seq;
            }
            return S_OK;
        }

        return E_INVALIDARG;
    }

private:
    std::shared_ptr<XDspClientState> LookupClient(HANDLE handle) {
        if (!handle) {
            return nullptr;
        }
        SrwLockGuard lk(table_lock_);
        auto it = clients_.find(handle);
        if (it == clients_.end()) {
            return nullptr;
        }
        return it->second;
    }

    std::atomic<uint64_t> next_handle_id_{1u};
    SrwLock table_lock_;
    std::unordered_map<HANDLE, std::shared_ptr<XDspClientState>> clients_;
    std::unordered_map<HANDLE, std::shared_ptr<XDspContext>> contexts_;
};

}  // namespace

extern "C" {

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspConnect(
    void* pMemory,
    uint32_t memorySize,
    uint32_t maxActivations,
    void** ppOutHandle) {
    static std::atomic<uint64_t> trace_count{0u};
    if (ShouldTraceCall(trace_count)) {
        openscarlett::audio::TraceRetailAudio(
            "chad",
            OPENSCARLETT_CALLER_ADDR(),
            "XDspConnect(pMem=%p, size=0x%X, maxAct=%u, ppOut=%p)",
            pMemory,
            memorySize,
            maxActivations,
            static_cast<void*>(ppOutHandle));
    }
    return RecordHr(
        "XDspConnect",
        XDspRegistry::Instance().ConnectWithMaximumStreamLimit(
            pMemory, memorySize, maxActivations, 0x100u, 2u, ppOutHandle));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspConnectWithMaximumStreamLimit(
    void* pMemory,
    uint32_t memorySize,
    uint32_t maxActivations,
    uint32_t maxStreamLimit,
    uint32_t priority,
    void** ppOutHandle) {
    static std::atomic<uint64_t> trace_count{0u};
    if (ShouldTraceCall(trace_count)) {
        openscarlett::audio::TraceRetailAudio(
            "chad",
            OPENSCARLETT_CALLER_ADDR(),
            "XDspConnectWithMaximumStreamLimit(pMem=%p, size=0x%X, maxAct=%u, maxStreams=%u, prio=%u, ppOut=%p)",
            pMemory,
            memorySize,
            maxActivations,
            maxStreamLimit,
            priority,
            static_cast<void*>(ppOutHandle));
    }
    return RecordHr(
        "XDspConnectWithMaximumStreamLimit",
        XDspRegistry::Instance().ConnectWithMaximumStreamLimit(
            pMemory, memorySize, maxActivations, maxStreamLimit, priority, ppOutHandle));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspDisconnect(HANDLE clientHandle) {
    static std::atomic<uint64_t> trace_count{0u};
    if (ShouldTraceCall(trace_count)) {
        openscarlett::audio::TraceRetailAudio(
            "chad",
            OPENSCARLETT_CALLER_ADDR(),
            "XDspDisconnect(handle=%p)",
            clientHandle);
    }
    return RecordHr("XDspDisconnect", XDspRegistry::Instance().Disconnect(clientHandle));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspActivate(
    HANDLE clientHandle,
    const void* pDesc,
    uint32_t** ppOutStatus,
    void** ppOutStream) {
    void* const ret_addr = OPENSCARLETT_CALLER_ADDR();
    static std::atomic<uint64_t> trace_count{0u};
    if (ShouldTraceCall(trace_count)) {
        openscarlett::audio::TraceRetailAudio(
            "chad",
            ret_addr,
            "XDspActivate(client=%p, pDesc=%p, ppStatus=%p, ppStream=%p)",
            clientHandle,
            pDesc,
            static_cast<void*>(ppOutStatus),
            static_cast<void*>(ppOutStream));
    }
    return RecordHr(
        "XDspActivate",
        XDspRegistry::Instance().Activate(clientHandle, pDesc, ppOutStatus, ppOutStream, ret_addr));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspDeactivate(HANDLE handle) {
    static std::atomic<uint64_t> trace_count{0u};
    if (ShouldTraceCall(trace_count)) {
        openscarlett::audio::TraceRetailAudio(
            "chad",
            OPENSCARLETT_CALLER_ADDR(),
            "XDspDeactivate(handle=%p)",
            handle);
    }
    return RecordHr("XDspDeactivate", XDspRegistry::Instance().Deactivate(handle));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspSubmitCommand(
    HANDLE handle,
    const void* pCmd,
    uint32_t* pOutToken) {
    static std::atomic<uint64_t> trace_count{0u};
    if (ShouldTraceCall(trace_count)) {
        openscarlett::audio::TraceRetailAudio(
            "chad",
            OPENSCARLETT_CALLER_ADDR(),
            "XDspSubmitCommand(handle=%p, pCmd=%p, pOutToken=%p)",
            handle,
            pCmd,
            static_cast<void*>(pOutToken));
    }
    return RecordHr(
        "XDspSubmitCommand",
        XDspRegistry::Instance().SubmitCommand(handle, pCmd, nullptr, pOutToken));
}

OPENSCARLETT_CHAD_API HRESULT OPENSCARLETT_CHAD_CALL XDspSubmitCommandWithEnvelope(
    HANDLE handle,
    const void* pCmd,
    const float* pEnvelope,
    uint32_t* pOutToken) {
    static std::atomic<uint64_t> trace_count{0u};
    if (ShouldTraceCall(trace_count)) {
        openscarlett::audio::TraceRetailAudio(
            "chad",
            OPENSCARLETT_CALLER_ADDR(),
            "XDspSubmitCommandWithEnvelope(handle=%p, pCmd=%p, pEnv=%p, pOutToken=%p)",
            handle,
            pCmd,
            static_cast<const void*>(pEnvelope),
            static_cast<void*>(pOutToken));
    }
    return RecordHr(
        "XDspSubmitCommandWithEnvelope",
        XDspRegistry::Instance().SubmitCommand(handle, pCmd, pEnvelope, pOutToken));
}

}  // extern "C"

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    (void)fdwReason;
    (void)lpvReserved;
    return TRUE;
}
#endif
