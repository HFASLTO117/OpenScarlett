#define OPENSCARLETT_BUILDING_XAUDIO2 1
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <audioclient.h>
#  include <mmdeviceapi.h>
#endif

namespace {

constexpr const char* kModName = "XAudio2_9.dll";

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const XAudio2Guid* a, const XAudio2Guid& b) {
    return a && std::memcmp(a, &b, sizeof(XAudio2Guid)) == 0;
}

std::string GetEnvString(const char* name) {
#if defined(_WIN32)
    char buf[256]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) {
        return {};
    }
    return std::string(buf, len);
#elif defined(_MSC_VER)
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) {
        return {};
    }
    std::string out(buf);
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

bool IsNullAudioForced() {
    const std::string v = GetEnvString("OPENSCARLETT_AUDIO_NULL");
    return v == "1" || v == "true" || v == "TRUE" || v == "yes";
}

class XAudio2Impl;
class SubmixVoiceImpl;
class MasteringVoiceImpl;
class SourceVoiceImpl;

// ===========================================================================
// Decoded Queued PCM Buffer (Interleaved float32)
// ===========================================================================
struct QueuedAudioBuffer {
    std::vector<float> samples;  // interleaved float32 (frames * channels)
    uint32_t channels = 2;
    uint32_t total_frames = 0;
    double play_cursor = 0.0;    // current frame position (fractional for resampling)
    uint32_t loop_begin = 0;
    uint32_t loop_length = 0;
    uint32_t loops_remaining = 0;
    bool end_of_stream = false;
    bool started_notified = false;
    void* context = nullptr;
};

struct DeferredVoiceCallbacks {
    IXAudio2VoiceCallback* cb = nullptr;
    std::vector<void*> started;
    std::vector<void*> ended;
    bool stream_ended = false;
};

// ===========================================================================
// Submix Voice
// ===========================================================================
class SubmixVoiceImpl final : public IXAudio2SubmixVoice {
public:
    SubmixVoiceImpl(XAudio2Impl* engine, uint32_t channels, uint32_t sample_rate, uint32_t flags)
        : engine_(engine),
          channels_(std::max(1u, channels)),
          sample_rate_(std::max(1u, sample_rate)),
          flags_(flags),
          channel_volumes_(std::max(1u, channels), 1.0f) {}

    void OPENSCARLETT_XAUDIO2_CALL GetVoiceDetails(
        OS_XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        RecordOk("IXAudio2SubmixVoice::GetVoiceDetails", 0);
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = flags_;
        pVoiceDetails->ActiveFlags = flags_;
        pVoiceDetails->InputChannels = channels_;
        pVoiceDetails->InputSampleRate = sample_rate_;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetOutputVoices(
        const OS_XAUDIO2_VOICE_SENDS* /*pSendList*/) override {
        return RecordHr("IXAudio2SubmixVoice::SetOutputVoices", OS_XAUDIO2_S_OK, 1);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float Volume, uint32_t /*OperationSet*/) override {
        volume_.store(std::clamp(Volume, -16777216.0f, 16777216.0f), std::memory_order_relaxed);
        return RecordHr("IXAudio2SubmixVoice::SetVolume", OS_XAUDIO2_S_OK, 2);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVolume(float* pVolume) override {
        RecordOk("IXAudio2SubmixVoice::GetVolume", 3);
        if (pVolume) *pVolume = volume_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetChannelVolumes(
        uint32_t Channels, const float* pVolumes, uint32_t /*OperationSet*/) override {
        if (!pVolumes || Channels != channels_) {
            return RecordHr("IXAudio2SubmixVoice::SetChannelVolumes", OS_XAUDIO2_E_INVALIDARG, 4);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        channel_volumes_.assign(pVolumes, pVolumes + Channels);
        return RecordHr("IXAudio2SubmixVoice::SetChannelVolumes", OS_XAUDIO2_S_OK, 4);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetChannelVolumes(uint32_t Channels, float* pVolumes) override {
        RecordOk("IXAudio2SubmixVoice::GetChannelVolumes", 5);
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t n = std::min<uint32_t>(Channels, static_cast<uint32_t>(channel_volumes_.size()));
        if (n > 0) std::memcpy(pVolumes, channel_volumes_.data(), n * sizeof(float));
    }

    void OPENSCARLETT_XAUDIO2_CALL DestroyVoice() override;

    float Volume() const { return volume_.load(std::memory_order_relaxed); }

private:
    XAudio2Impl* engine_;
    uint32_t channels_;
    uint32_t sample_rate_;
    uint32_t flags_;
    std::atomic<float> volume_{1.0f};
    std::mutex mutex_;
    std::vector<float> channel_volumes_;
};

// ===========================================================================
// Mastering Voice
// ===========================================================================
class MasteringVoiceImpl final : public IXAudio2MasteringVoice {
public:
    MasteringVoiceImpl(XAudio2Impl* engine, uint32_t channels, uint32_t sample_rate, uint32_t flags)
        : engine_(engine),
          channels_(channels != 0 ? channels : 2u),
          sample_rate_(sample_rate != 0 ? sample_rate : 48000u),
          flags_(flags),
          channel_volumes_(channels != 0 ? channels : 2u, 1.0f) {}

    void OPENSCARLETT_XAUDIO2_CALL GetVoiceDetails(
        OS_XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        RecordOk("IXAudio2MasteringVoice::GetVoiceDetails", 0);
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = flags_;
        pVoiceDetails->ActiveFlags = flags_;
        pVoiceDetails->InputChannels = channels_;
        pVoiceDetails->InputSampleRate = sample_rate_;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetOutputVoices(
        const OS_XAUDIO2_VOICE_SENDS* /*pSendList*/) override {
        return RecordHr("IXAudio2MasteringVoice::SetOutputVoices", OS_XAUDIO2_E_INVALID_CALL, 1);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float Volume, uint32_t /*OperationSet*/) override {
        volume_.store(Volume, std::memory_order_relaxed);
        return RecordHr("IXAudio2MasteringVoice::SetVolume", OS_XAUDIO2_S_OK, 2);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVolume(float* pVolume) override {
        RecordOk("IXAudio2MasteringVoice::GetVolume", 3);
        if (pVolume) *pVolume = volume_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetChannelVolumes(
        uint32_t Channels, const float* pVolumes, uint32_t /*OperationSet*/) override {
        if (!pVolumes || Channels != channels_) {
            return RecordHr("IXAudio2MasteringVoice::SetChannelVolumes", OS_XAUDIO2_E_INVALIDARG, 4);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        channel_volumes_.assign(pVolumes, pVolumes + Channels);
        return RecordHr("IXAudio2MasteringVoice::SetChannelVolumes", OS_XAUDIO2_S_OK, 4);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetChannelVolumes(uint32_t Channels, float* pVolumes) override {
        RecordOk("IXAudio2MasteringVoice::GetChannelVolumes", 5);
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t n = std::min<uint32_t>(Channels, static_cast<uint32_t>(channel_volumes_.size()));
        if (n > 0) std::memcpy(pVolumes, channel_volumes_.data(), n * sizeof(float));
    }

    void OPENSCARLETT_XAUDIO2_CALL DestroyVoice() override;

    int32_t OPENSCARLETT_XAUDIO2_CALL GetChannelMask(uint32_t* pChannelMask) override {
        if (!pChannelMask) {
            return RecordHr("IXAudio2MasteringVoice::GetChannelMask", OS_XAUDIO2_E_INVALIDARG, 7);
        }
        *pChannelMask = (channels_ == 1) ? 0x4u : (channels_ == 6 ? 0x3Fu : 0x3u);
        return RecordHr("IXAudio2MasteringVoice::GetChannelMask", OS_XAUDIO2_S_OK, 7);
    }

    uint32_t Channels() const { return channels_; }
    uint32_t SampleRate() const { return sample_rate_; }
    float Volume() const { return volume_.load(std::memory_order_relaxed); }

    std::vector<float> SnapshotChannelVolumes() {
        std::lock_guard<std::mutex> lk(mutex_);
        return channel_volumes_;
    }

private:
    XAudio2Impl* engine_;
    uint32_t channels_;
    uint32_t sample_rate_;
    uint32_t flags_;
    std::atomic<float> volume_{1.0f};
    std::mutex mutex_;
    std::vector<float> channel_volumes_;
};

// ===========================================================================
// Source Voice (PCM8/16/24/32, Float32, and native Xbox XMA2 0x0166)
// ===========================================================================
class SourceVoiceImpl final : public IXAudio2SourceVoice {
public:
    SourceVoiceImpl(
        XAudio2Impl* engine,
        const OS_WAVEFORMATEX& fmt,
        uint32_t flags,
        float max_freq_ratio,
        IXAudio2VoiceCallback* cb)
        : engine_(engine),
          format_(fmt),
          flags_(flags),
          max_freq_ratio_(std::max(1.0f, max_freq_ratio)),
          callback_(cb),
          channel_volumes_(std::max<uint16_t>(1u, fmt.nChannels), 1.0f) {
        if (format_.wFormatTag == OS_WAVE_FORMAT_EXTENSIBLE) {
            // Resolved from extensible tag during CreateSourceVoice.
            effective_format_tag_ = OS_WAVE_FORMAT_PCM;
        } else {
            effective_format_tag_ = format_.wFormatTag;
        }
    }

    void SetEffectiveFormatTag(uint16_t tag) { effective_format_tag_ = tag; }
    void SetAdpcmSamplesPerBlock(uint16_t spb) { adpcm_samples_per_block_ = spb; }
    void SetOpusPreSkip(uint16_t pre_skip) { opus_pre_skip_ = pre_skip; }
    bool IsOpusMftActive() const {
        return opus_mft_active_.load(std::memory_order_acquire);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVoiceDetails(
        OS_XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        RecordOk("IXAudio2SourceVoice::GetVoiceDetails", 0);
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = flags_;
        pVoiceDetails->ActiveFlags = flags_;
        pVoiceDetails->InputChannels = format_.nChannels;
        pVoiceDetails->InputSampleRate = sample_rate_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetOutputVoices(
        const OS_XAUDIO2_VOICE_SENDS* pSendList) override {
        std::lock_guard<std::mutex> lk(mutex_);
        target_submix_ = nullptr;
        if (pSendList && pSendList->SendCount > 0 && pSendList->pSends) {
            for (uint32_t i = 0; i < pSendList->SendCount; ++i) {
                if (auto* sub = dynamic_cast<SubmixVoiceImpl*>(pSendList->pSends[i].pOutputVoice)) {
                    target_submix_ = sub;
                    break;
                }
            }
        }
        return RecordHr("IXAudio2SourceVoice::SetOutputVoices", OS_XAUDIO2_S_OK, 1);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float Volume, uint32_t /*OperationSet*/) override {
        volume_.store(Volume, std::memory_order_relaxed);
        return RecordHr("IXAudio2SourceVoice::SetVolume", OS_XAUDIO2_S_OK, 2);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVolume(float* pVolume) override {
        RecordOk("IXAudio2SourceVoice::GetVolume", 3);
        if (pVolume) *pVolume = volume_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetChannelVolumes(
        uint32_t Channels, const float* pVolumes, uint32_t /*OperationSet*/) override {
        if (!pVolumes || Channels != format_.nChannels) {
            return RecordHr("IXAudio2SourceVoice::SetChannelVolumes", OS_XAUDIO2_E_INVALIDARG, 4);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        channel_volumes_.assign(pVolumes, pVolumes + Channels);
        return RecordHr("IXAudio2SourceVoice::SetChannelVolumes", OS_XAUDIO2_S_OK, 4);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetChannelVolumes(uint32_t Channels, float* pVolumes) override {
        RecordOk("IXAudio2SourceVoice::GetChannelVolumes", 5);
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t n = std::min<uint32_t>(Channels, static_cast<uint32_t>(channel_volumes_.size()));
        if (n > 0) std::memcpy(pVolumes, channel_volumes_.data(), n * sizeof(float));
    }

    void OPENSCARLETT_XAUDIO2_CALL DestroyVoice() override;

    int32_t OPENSCARLETT_XAUDIO2_CALL Start(uint32_t /*Flags*/, uint32_t /*OperationSet*/) override {
        playing_.store(true, std::memory_order_release);
        return RecordHr("IXAudio2SourceVoice::Start", OS_XAUDIO2_S_OK, 7);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL Stop(uint32_t /*Flags*/, uint32_t /*OperationSet*/) override {
        playing_.store(false, std::memory_order_release);
        return RecordHr("IXAudio2SourceVoice::Stop", OS_XAUDIO2_S_OK, 8);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SubmitSourceBuffer(
        const OS_XAUDIO2_BUFFER* pBuffer,
        const OS_XAUDIO2_BUFFER_WMA* /*pBufferWMA*/) override {
        if (!pBuffer || (pBuffer->AudioBytes > 0 && !pBuffer->pAudioData)) {
            return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_E_INVALIDARG, 9);
        }

        const uint32_t ch = std::max<uint32_t>(1u, format_.nChannels);
        std::vector<float> decoded;

        if (effective_format_tag_ == OS_WAVE_FORMAT_XMA2) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (!xma2_decoder_) {
                xma2_decoder_ = std::make_unique<openscarlett::Xma2Decoder>();
                openscarlett::Xma2Config cfg{};
                cfg.sample_rate = format_.nSamplesPerSec != 0 ? format_.nSamplesPerSec : 48000u;
                cfg.channels = static_cast<uint16_t>(std::min<uint32_t>(2u, ch));
                if (!xma2_decoder_->Initialize(cfg).ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_XMA_DECODER_ERROR, 9);
                }
            }
            if (pBuffer->AudioBytes > 0) {
                const auto st = xma2_decoder_->DecodePackets(
                    pBuffer->pAudioData, pBuffer->AudioBytes, decoded);
                if (!st.ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_XMA_DECODER_ERROR, 9);
                }
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_IMA_ADPCM ||
                   effective_format_tag_ == OS_WAVE_FORMAT_XBOX_ADPCM) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (!adpcm_decoder_) {
                adpcm_decoder_ = std::make_unique<openscarlett::ImaAdpcmDecoder>();
                openscarlett::ImaAdpcmConfig cfg{};
                cfg.sample_rate = format_.nSamplesPerSec != 0 ? format_.nSamplesPerSec : 48000u;
                cfg.channels = static_cast<uint16_t>(std::min<uint32_t>(2u, ch));
                cfg.block_align = format_.nBlockAlign;
                cfg.samples_per_block = adpcm_samples_per_block_;
                cfg.xbox_adpcm = (effective_format_tag_ == OS_WAVE_FORMAT_XBOX_ADPCM);
                if (!adpcm_decoder_->Initialize(cfg).ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
            }
            if (pBuffer->AudioBytes > 0) {
                const auto st = adpcm_decoder_->DecodeBlocks(
                    pBuffer->pAudioData, pBuffer->AudioBytes, decoded);
                if (!st.ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_OPUS) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (!opus_decoder_) {
                opus_decoder_ = std::make_unique<openscarlett::OpusMftDecoder>();
                openscarlett::OpusDecoderConfig cfg{};
                cfg.sample_rate = format_.nSamplesPerSec != 0 ? format_.nSamplesPerSec : 48000u;
                cfg.channels = static_cast<uint16_t>(std::min<uint32_t>(2u, ch));
                cfg.pre_skip = opus_pre_skip_;
                cfg.allow_fallback = true;
                if (!opus_decoder_->Initialize(cfg).ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
                opus_mft_active_.store(opus_decoder_->IsNativeMftActive(), std::memory_order_release);
                const uint32_t out_sr = opus_decoder_->Stats().output_sample_rate;
                if (out_sr != 0u) {
                    sample_rate_.store(out_sr, std::memory_order_relaxed);
                }
            }
            if (pBuffer->AudioBytes > 0) {
                const auto st = opus_decoder_->DecodeStream(
                    pBuffer->pAudioData, pBuffer->AudioBytes, decoded,
                    openscarlett::OpusPacketFraming::Auto);
                if (!st.ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
                if ((pBuffer->Flags & OS_XAUDIO2_END_OF_STREAM) != 0u) {
                    (void)opus_decoder_->Drain(decoded);
                }
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_IEEE_FLOAT) {
            if (format_.wBitsPerSample != 32) {
                return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                OS_XAUDIO2_E_INVALIDARG, 9);
            }
            const size_t sample_count = pBuffer->AudioBytes / sizeof(float);
            decoded.resize(sample_count);
            if (sample_count > 0) {
                std::memcpy(decoded.data(), pBuffer->pAudioData, sample_count * sizeof(float));
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_PCM) {
            if (format_.wBitsPerSample == 16) {
                const size_t sample_count = pBuffer->AudioBytes / sizeof(int16_t);
                decoded.resize(sample_count);
                const auto* src = reinterpret_cast<const int16_t*>(pBuffer->pAudioData);
                for (size_t i = 0; i < sample_count; ++i) {
                    decoded[i] = static_cast<float>(src[i]) / 32768.0f;
                }
            } else if (format_.wBitsPerSample == 8) {
                const size_t sample_count = pBuffer->AudioBytes;
                decoded.resize(sample_count);
                for (size_t i = 0; i < sample_count; ++i) {
                    decoded[i] = (static_cast<float>(pBuffer->pAudioData[i]) - 128.0f) / 128.0f;
                }
            } else if (format_.wBitsPerSample == 32) {
                const size_t sample_count = pBuffer->AudioBytes / sizeof(int32_t);
                decoded.resize(sample_count);
                const auto* src = reinterpret_cast<const int32_t*>(pBuffer->pAudioData);
                for (size_t i = 0; i < sample_count; ++i) {
                    decoded[i] = static_cast<float>(static_cast<double>(src[i]) / 2147483648.0);
                }
            } else {
                return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                OS_XAUDIO2_E_INVALIDARG, 9);
            }
        } else {
            return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_E_INVALIDARG, 9);
        }

        uint32_t eff_ch = ch;
        if (effective_format_tag_ == OS_WAVE_FORMAT_XMA2 ||
            effective_format_tag_ == OS_WAVE_FORMAT_IMA_ADPCM ||
            effective_format_tag_ == OS_WAVE_FORMAT_XBOX_ADPCM) {
            eff_ch = std::min<uint32_t>(2u, ch);
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_OPUS) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (opus_decoder_ && opus_decoder_->Stats().output_channels > 0u) {
                eff_ch = opus_decoder_->Stats().output_channels;
            } else {
                eff_ch = std::min<uint32_t>(2u, ch);
            }
        }
        uint32_t total_frames = static_cast<uint32_t>(decoded.size() / std::max<uint32_t>(1u, eff_ch));

        // Apply optional PlayBegin / PlayLength range.
        if (pBuffer->PlayBegin > 0 || pBuffer->PlayLength > 0) {
            const uint32_t start = std::min<uint32_t>(pBuffer->PlayBegin, total_frames);
            const uint32_t max_avail = total_frames - start;
            const uint32_t len = (pBuffer->PlayLength > 0)
                                     ? std::min<uint32_t>(pBuffer->PlayLength, max_avail)
                                     : max_avail;
            std::vector<float> sliced(len * eff_ch);
            if (len > 0) {
                std::memcpy(sliced.data(), decoded.data() + start * eff_ch,
                            len * eff_ch * sizeof(float));
            }
            decoded = std::move(sliced);
            total_frames = len;
        }

        QueuedAudioBuffer qb{};
        qb.samples = std::move(decoded);
        qb.channels = eff_ch;
        qb.total_frames = total_frames;
        qb.play_cursor = 0.0;
        qb.loop_begin = std::min(pBuffer->LoopBegin, total_frames);
        qb.loop_length = (pBuffer->LoopLength > 0 && qb.loop_begin + pBuffer->LoopLength <= total_frames)
                             ? pBuffer->LoopLength
                             : (total_frames - qb.loop_begin);
        qb.loops_remaining = pBuffer->LoopCount;
        qb.end_of_stream = (pBuffer->Flags & OS_XAUDIO2_END_OF_STREAM) != 0;
        qb.context = pBuffer->pContext;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            buffers_.push_back(std::move(qb));
        }

        return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_S_OK, 9);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL FlushSourceBuffers() override {
        std::vector<void*> flushed_ctxs;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (const auto& b : buffers_) {
                flushed_ctxs.push_back(b.context);
            }
            buffers_.clear();
        }
        if (callback_) {
            for (void* ctx : flushed_ctxs) {
                callback_->OnBufferEnd(ctx);
            }
        }
        return RecordHr("IXAudio2SourceVoice::FlushSourceBuffers", OS_XAUDIO2_S_OK, 10);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL Discontinuity() override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!buffers_.empty()) {
            buffers_.back().end_of_stream = true;
        }
        return RecordHr("IXAudio2SourceVoice::Discontinuity", OS_XAUDIO2_S_OK, 11);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL ExitLoop(uint32_t /*OperationSet*/) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!buffers_.empty()) {
            buffers_.front().loops_remaining = 0;
        }
        return RecordHr("IXAudio2SourceVoice::ExitLoop", OS_XAUDIO2_S_OK, 12);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetState(
        OS_XAUDIO2_VOICE_STATE* pVoiceState, uint32_t /*Flags*/) override {
        RecordOk("IXAudio2SourceVoice::GetState", 13);
        if (!pVoiceState) return;
        std::lock_guard<std::mutex> lk(mutex_);
        pVoiceState->BuffersQueued = static_cast<uint32_t>(buffers_.size());
        pVoiceState->pCurrentBufferContext = buffers_.empty() ? nullptr : buffers_.front().context;
        pVoiceState->SamplesPlayed = samples_played_;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetFrequencyRatio(
        float Ratio, uint32_t /*OperationSet*/) override {
        if (Ratio <= 0.0f || Ratio > max_freq_ratio_) {
            return RecordHr("IXAudio2SourceVoice::SetFrequencyRatio", OS_XAUDIO2_E_INVALIDARG, 14);
        }
        freq_ratio_.store(Ratio, std::memory_order_relaxed);
        return RecordHr("IXAudio2SourceVoice::SetFrequencyRatio", OS_XAUDIO2_S_OK, 14);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetFrequencyRatio(float* pRatio) override {
        RecordOk("IXAudio2SourceVoice::GetFrequencyRatio", 15);
        if (pRatio) *pRatio = freq_ratio_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetSourceSampleRate(uint32_t NewSourceSampleRate) override {
        if (NewSourceSampleRate == 0) {
            return RecordHr("IXAudio2SourceVoice::SetSourceSampleRate", OS_XAUDIO2_E_INVALIDARG, 16);
        }
        sample_rate_.store(NewSourceSampleRate, std::memory_order_relaxed);
        return RecordHr("IXAudio2SourceVoice::SetSourceSampleRate", OS_XAUDIO2_S_OK, 16);
    }

    bool IsPlaying() const { return playing_.load(std::memory_order_acquire); }
    bool IsXma2() const { return effective_format_tag_ == OS_WAVE_FORMAT_XMA2; }

    void MixInto(
        float* out_interleaved,
        uint32_t out_frames,
        uint32_t out_channels,
        uint32_t out_sample_rate,
        DeferredVoiceCallbacks& out_cbs) {
        if (!playing_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (buffers_.empty()) {
            return;
        }
        out_cbs.cb = callback_;

        const float voice_vol = volume_.load(std::memory_order_relaxed);
        const float submix_vol = target_submix_ ? target_submix_->Volume() : 1.0f;
        const float combined_vol = voice_vol * submix_vol;
        const double src_rate = static_cast<double>(sample_rate_.load(std::memory_order_relaxed));
        const double ratio = static_cast<double>(freq_ratio_.load(std::memory_order_relaxed));
        const double step = (src_rate / static_cast<double>(std::max(1u, out_sample_rate))) * ratio;

        for (uint32_t f = 0; f < out_frames; ++f) {
            while (!buffers_.empty()) {
                auto& buf = buffers_.front();
                if (!buf.started_notified) {
                    buf.started_notified = true;
                    out_cbs.started.push_back(buf.context);
                }
                if (buf.total_frames == 0 ||
                    buf.play_cursor >= static_cast<double>(buf.total_frames)) {
                    if (buf.loops_remaining > 0 && buf.loop_length > 0) {
                        if (buf.loops_remaining != OS_XAUDIO2_LOOP_INFINITE) {
                            --buf.loops_remaining;
                        }
                        buf.play_cursor = static_cast<double>(buf.loop_begin);
                        break;
                    }
                    const bool eos = buf.end_of_stream;
                    out_cbs.ended.push_back(buf.context);
                    buffers_.pop_front();
                    if (eos) {
                        out_cbs.stream_ended = true;
                    }
                    continue;
                }
                break;
            }
            if (buffers_.empty()) {
                break;
            }

            auto& buf = buffers_.front();
            const uint32_t i0 = static_cast<uint32_t>(buf.play_cursor);
            const uint32_t i1 = std::min<uint32_t>(i0 + 1, buf.total_frames - 1);
            const float frac = static_cast<float>(buf.play_cursor - static_cast<double>(i0));

            const uint32_t src_ch = buf.channels;
            for (uint32_t oc = 0; oc < out_channels; ++oc) {
                const uint32_t sc = (src_ch == 1) ? 0u : (oc % src_ch);
                const float s0 = buf.samples[static_cast<size_t>(i0) * src_ch + sc];
                const float s1 = buf.samples[static_cast<size_t>(i1) * src_ch + sc];
                const float sample = s0 + (s1 - s0) * frac;
                const float ch_gain = (sc < channel_volumes_.size()) ? channel_volumes_[sc] : 1.0f;
                out_interleaved[static_cast<size_t>(f) * out_channels + oc] +=
                    sample * combined_vol * ch_gain;
            }

            buf.play_cursor += step;
            ++samples_played_;

            if (buf.loops_remaining > 0 && buf.loop_length > 0 &&
                buf.play_cursor >= static_cast<double>(buf.loop_begin + buf.loop_length)) {
                if (buf.loops_remaining != OS_XAUDIO2_LOOP_INFINITE) {
                    --buf.loops_remaining;
                }
                buf.play_cursor = static_cast<double>(buf.loop_begin);
            }
        }
    }

private:
    XAudio2Impl* engine_;
    OS_WAVEFORMATEX format_{};
    uint16_t effective_format_tag_ = OS_WAVE_FORMAT_PCM;
    uint32_t flags_ = 0;
    float max_freq_ratio_ = 2.0f;
    IXAudio2VoiceCallback* callback_ = nullptr;

    std::atomic<bool> playing_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> freq_ratio_{1.0f};
    std::atomic<uint32_t> sample_rate_{48000u};

    std::mutex decoder_mutex_;
    std::unique_ptr<openscarlett::Xma2Decoder> xma2_decoder_;
    std::unique_ptr<openscarlett::ImaAdpcmDecoder> adpcm_decoder_;
    std::unique_ptr<openscarlett::OpusMftDecoder> opus_decoder_;
    uint16_t adpcm_samples_per_block_ = 0u;
    uint16_t opus_pre_skip_ = 0u;
    std::atomic<bool> opus_mft_active_{false};

    std::mutex mutex_;
    SubmixVoiceImpl* target_submix_ = nullptr;
    std::vector<float> channel_volumes_;
    std::deque<QueuedAudioBuffer> buffers_;
    uint64_t samples_played_ = 0;
};

// ===========================================================================
// IXAudio2 Engine Implementation (Software Mixer + Optional Live WASAPI Sink)
// ===========================================================================
class XAudio2Impl final : public IXAudio2 {
public:
    XAudio2Impl() = default;

    ~XAudio2Impl() {
        StopWasapiThread();
        std::lock_guard<std::mutex> lk(mutex_);
        source_voices_.clear();
        submix_voices_.clear();
        mastering_voice_.reset();
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_XAudio2_IUnknown) || GuidEquals(riid, IID_OS_IXAudio2)) {
            AddRef();
            *ppvObject = static_cast<IXAudio2*>(this);
            return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_S_OK, 0);
        }
        return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() override {
        RecordOk("IXAudio2::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL Release() override {
        RecordOk("IXAudio2::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL RegisterForCallbacks(
        IXAudio2EngineCallback* pCallback) override {
        if (!pCallback) {
            return RecordHr("IXAudio2::RegisterForCallbacks", OS_XAUDIO2_E_INVALIDARG, 3);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (std::find(engine_callbacks_.begin(), engine_callbacks_.end(), pCallback) ==
            engine_callbacks_.end()) {
            engine_callbacks_.push_back(pCallback);
        }
        return RecordHr("IXAudio2::RegisterForCallbacks", OS_XAUDIO2_S_OK, 3);
    }

    void OPENSCARLETT_XAUDIO2_CALL UnregisterForCallbacks(
        IXAudio2EngineCallback* pCallback) override {
        RecordOk("IXAudio2::UnregisterForCallbacks", 4);
        std::lock_guard<std::mutex> lk(mutex_);
        engine_callbacks_.erase(
            std::remove(engine_callbacks_.begin(), engine_callbacks_.end(), pCallback),
            engine_callbacks_.end());
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CreateSourceVoice(
        IXAudio2SourceVoice** ppSourceVoice,
        const OS_WAVEFORMATEX* pSourceFormat,
        uint32_t Flags,
        float MaxFrequencyRatio,
        IXAudio2VoiceCallback* pCallback,
        const OS_XAUDIO2_VOICE_SENDS* pSendList,
        const void* /*pEffectChain*/) override {
        if (!ppSourceVoice || !pSourceFormat) {
            return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_E_INVALIDARG, 5);
        }
        *ppSourceVoice = nullptr;
        if (pSourceFormat->nChannels == 0 || pSourceFormat->nSamplesPerSec == 0) {
            return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_E_INVALIDARG, 5);
        }

        uint16_t effective_tag = pSourceFormat->wFormatTag;
        if (effective_tag == OS_WAVE_FORMAT_EXTENSIBLE) {
            if (pSourceFormat->cbSize >= 22) {
                const auto* ext = reinterpret_cast<const OS_WAVEFORMATEXTENSIBLE*>(pSourceFormat);
                effective_tag = static_cast<uint16_t>(ext->SubFormat.Data1 & 0xFFFFu);
            } else {
                effective_tag = OS_WAVE_FORMAT_PCM;
            }
        }
        if (effective_tag != OS_WAVE_FORMAT_PCM &&
            effective_tag != OS_WAVE_FORMAT_IEEE_FLOAT &&
            effective_tag != OS_WAVE_FORMAT_IMA_ADPCM &&
            effective_tag != OS_WAVE_FORMAT_XBOX_ADPCM &&
            effective_tag != OS_WAVE_FORMAT_XMA2 &&
            effective_tag != OS_WAVE_FORMAT_OPUS) {
            return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_E_INVALIDARG, 5);
        }

        auto voice = std::make_unique<SourceVoiceImpl>(
            this, *pSourceFormat, Flags, MaxFrequencyRatio, pCallback);
        voice->SetEffectiveFormatTag(effective_tag);
        voice->SetSourceSampleRate(pSourceFormat->nSamplesPerSec);
        if ((effective_tag == OS_WAVE_FORMAT_IMA_ADPCM ||
             effective_tag == OS_WAVE_FORMAT_XBOX_ADPCM) &&
            pSourceFormat->cbSize >= 2u) {
            const auto* ima = reinterpret_cast<const OS_IMAADPCMWAVEFORMAT*>(pSourceFormat);
            voice->SetAdpcmSamplesPerBlock(ima->wSamplesPerBlock);
        } else if (effective_tag == OS_WAVE_FORMAT_OPUS && pSourceFormat->cbSize >= 2u) {
            const auto* opus = reinterpret_cast<const OS_OPUSWAVEFORMAT*>(pSourceFormat);
            voice->SetOpusPreSkip(opus->PreSkip);
        }
        if (pSendList) {
            voice->SetOutputVoices(pSendList);
        }

        SourceVoiceImpl* raw = voice.get();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            source_voices_.push_back(std::move(voice));
        }
        *ppSourceVoice = raw;
        return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_S_OK, 5);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CreateSubmixVoice(
        IXAudio2SubmixVoice** ppSubmixVoice,
        uint32_t InputChannels,
        uint32_t InputSampleRate,
        uint32_t Flags,
        uint32_t /*ProcessingStage*/,
        const OS_XAUDIO2_VOICE_SENDS* /*pSendList*/,
        const void* /*pEffectChain*/) override {
        if (!ppSubmixVoice || InputChannels == 0 || InputSampleRate == 0) {
            return RecordHr("IXAudio2::CreateSubmixVoice", OS_XAUDIO2_E_INVALIDARG, 6);
        }
        auto sub = std::make_unique<SubmixVoiceImpl>(this, InputChannels, InputSampleRate, Flags);
        SubmixVoiceImpl* raw = sub.get();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            submix_voices_.push_back(std::move(sub));
        }
        *ppSubmixVoice = raw;
        return RecordHr("IXAudio2::CreateSubmixVoice", OS_XAUDIO2_S_OK, 6);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CreateMasteringVoice(
        IXAudio2MasteringVoice** ppMasteringVoice,
        uint32_t InputChannels,
        uint32_t InputSampleRate,
        uint32_t Flags,
        const wchar_t* /*szDeviceId*/,
        const void* /*pEffectChain*/,
        uint32_t /*StreamCategory*/) override {
        if (!ppMasteringVoice) {
            return RecordHr("IXAudio2::CreateMasteringVoice", OS_XAUDIO2_E_INVALIDARG, 7);
        }
        *ppMasteringVoice = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            mastering_voice_ = std::make_unique<MasteringVoiceImpl>(
                this,
                InputChannels != 0 ? InputChannels : 2u,
                InputSampleRate != 0 ? InputSampleRate : 48000u,
                Flags);
            *ppMasteringVoice = mastering_voice_.get();
        }
        StartWasapiThreadIfAllowed();
        return RecordHr("IXAudio2::CreateMasteringVoice", OS_XAUDIO2_S_OK, 7);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL StartEngine() override {
        engine_running_.store(true, std::memory_order_release);
        StartWasapiThreadIfAllowed();
        return RecordHr("IXAudio2::StartEngine", OS_XAUDIO2_S_OK, 8);
    }

    void OPENSCARLETT_XAUDIO2_CALL StopEngine() override {
        RecordOk("IXAudio2::StopEngine", 9);
        engine_running_.store(false, std::memory_order_release);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CommitChanges(uint32_t /*OperationSet*/) override {
        return RecordHr("IXAudio2::CommitChanges", OS_XAUDIO2_S_OK, 10);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetPerformanceData(
        OS_XAUDIO2_PERFORMANCE_DATA* pPerfData) override {
        RecordOk("IXAudio2::GetPerformanceData", 11);
        if (!pPerfData) return;
        std::memset(pPerfData, 0, sizeof(*pPerfData));
        std::lock_guard<std::mutex> lk(mutex_);
        uint32_t active_src = 0;
        uint32_t active_xma = 0;
        for (const auto& v : source_voices_) {
            if (v->IsPlaying()) {
                ++active_src;
                if (v->IsXma2()) ++active_xma;
            }
        }
        pPerfData->ActiveSourceVoiceCount = active_src;
        pPerfData->TotalSourceVoiceCount = static_cast<uint32_t>(source_voices_.size());
        pPerfData->ActiveSubmixVoiceCount = static_cast<uint32_t>(submix_voices_.size());
        pPerfData->ActiveXmaSourceVoices = active_xma;
        pPerfData->ActiveXmaStreams = active_xma;
        pPerfData->CurrentLatencyInSamples = 480u;
    }

    void OPENSCARLETT_XAUDIO2_CALL SetDebugConfiguration(
        const OS_XAUDIO2_DEBUG_CONFIGURATION* /*pDebugConfiguration*/,
        void* /*pReserved*/) override {
        RecordOk("IXAudio2::SetDebugConfiguration", 12);
    }

    void RemoveSourceVoice(SourceVoiceImpl* v) {
        std::lock_guard<std::mutex> lk(mutex_);
        source_voices_.erase(
            std::remove_if(source_voices_.begin(), source_voices_.end(),
                           [&](const std::unique_ptr<SourceVoiceImpl>& p) { return p.get() == v; }),
            source_voices_.end());
    }

    void RemoveSubmixVoice(SubmixVoiceImpl* v) {
        std::lock_guard<std::mutex> lk(mutex_);
        submix_voices_.erase(
            std::remove_if(submix_voices_.begin(), submix_voices_.end(),
                           [&](const std::unique_ptr<SubmixVoiceImpl>& p) { return p.get() == v; }),
            submix_voices_.end());
    }

    void RemoveMasteringVoice(MasteringVoiceImpl* v) {
        StopWasapiThread();
        std::lock_guard<std::mutex> lk(mutex_);
        if (mastering_voice_.get() == v) {
            mastering_voice_.reset();
        }
    }

    int32_t RenderMixFrames(
        float* out_frames,
        uint32_t frame_count,
        uint32_t override_channels = 0,
        uint32_t override_sample_rate = 0) {
        if (!out_frames || frame_count == 0) {
            return OS_XAUDIO2_E_INVALIDARG;
        }

        std::vector<IXAudio2EngineCallback*> eng_cbs;
        std::vector<DeferredVoiceCallbacks> voice_cbs;
        uint32_t out_ch = 2u;
        uint32_t out_sr = 48000u;
        float master_vol = 1.0f;
        std::vector<float> master_ch_vol;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            eng_cbs = engine_callbacks_;
            if (mastering_voice_) {
                out_ch = mastering_voice_->Channels();
                out_sr = mastering_voice_->SampleRate();
                master_vol = mastering_voice_->Volume();
                master_ch_vol = mastering_voice_->SnapshotChannelVolumes();
            }
            if (override_channels != 0) out_ch = override_channels;
            if (override_sample_rate != 0) out_sr = override_sample_rate;

            std::fill_n(out_frames, static_cast<size_t>(frame_count) * out_ch, 0.0f);

            if (engine_running_.load(std::memory_order_acquire)) {
                voice_cbs.reserve(source_voices_.size());
                for (auto& v : source_voices_) {
                    DeferredVoiceCallbacks dcb{};
                    v->MixInto(out_frames, frame_count, out_ch, out_sr, dcb);
                    if (dcb.cb && (!dcb.started.empty() || !dcb.ended.empty() || dcb.stream_ended)) {
                        voice_cbs.push_back(std::move(dcb));
                    }
                }
            }
        }

        // Apply mastering volume + clamp to [-1, 1].
        for (uint32_t f = 0; f < frame_count; ++f) {
            for (uint32_t c = 0; c < out_ch; ++c) {
                const float cg = (c < master_ch_vol.size()) ? master_ch_vol[c] : 1.0f;
                float& s = out_frames[static_cast<size_t>(f) * out_ch + c];
                s = std::clamp(s * master_vol * cg, -1.0f, 1.0f);
            }
        }

        // Deliver callbacks outside internal locks.
        for (auto* ecb : eng_cbs) {
            if (ecb) ecb->OnProcessingPassStart();
        }
        for (const auto& dcb : voice_cbs) {
            for (void* ctx : dcb.started) dcb.cb->OnBufferStart(ctx);
            for (void* ctx : dcb.ended) dcb.cb->OnBufferEnd(ctx);
            if (dcb.stream_ended) dcb.cb->OnStreamEnd();
        }
        for (auto* ecb : eng_cbs) {
            if (ecb) ecb->OnProcessingPassEnd();
        }
        return OS_XAUDIO2_S_OK;
    }

    void GetBackendInfo(OS_XAUDIO2_BACKEND_INFO& out) {
        std::memset(&out, 0, sizeof(out));
        out.null_audio_forced = IsNullAudioForced() ? 1u : 0u;
#if defined(_WIN32)
        out.wasapi_active = wasapi_active_.load(std::memory_order_acquire) ? 1u : 0u;
        out.wasapi_event_driven = wasapi_event_driven_.load(std::memory_order_acquire) ? 1u : 0u;
        out.wasapi_sample_rate = wasapi_sample_rate_.load(std::memory_order_acquire);
        out.wasapi_channels = wasapi_channels_.load(std::memory_order_acquire);
        out.wasapi_bits_per_sample = wasapi_bits_per_sample_.load(std::memory_order_acquire);
        out.wasapi_buffer_frames = wasapi_buffer_frames_.load(std::memory_order_acquire);
        out.wasapi_frames_rendered = wasapi_frames_rendered_.load(std::memory_order_acquire);
        out.wasapi_last_hresult = wasapi_last_hr_.load(std::memory_order_acquire);
#endif
        std::lock_guard<std::mutex> lk(mutex_);
        uint32_t opus_mft_count = 0u;
        for (const auto& v : source_voices_) {
            if (v->IsOpusMftActive()) {
                ++opus_mft_count;
            }
        }
        out.opus_mft_active_voices = opus_mft_count;
    }

private:
    void StartWasapiThreadIfAllowed() {
#if defined(_WIN32)
        if (IsNullAudioForced()) {
            return;
        }
        std::unique_lock<std::mutex> lk(wasapi_mutex_);
        if (wasapi_thread_.joinable()) {
            return;
        }
        wasapi_stop_.store(false, std::memory_order_release);
        wasapi_init_done_ = false;
        if (!wasapi_stop_event_) {
            wasapi_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        } else {
            ResetEvent(wasapi_stop_event_);
        }
        wasapi_thread_ = std::thread(&XAudio2Impl::WasapiRenderLoop, this);
        wasapi_init_cv_.wait_for(lk, std::chrono::milliseconds(500), [&]() {
            return wasapi_init_done_;
        });
#endif
    }

    void StopWasapiThread() {
#if defined(_WIN32)
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(wasapi_mutex_);
            wasapi_stop_.store(true, std::memory_order_release);
            if (wasapi_stop_event_) {
                SetEvent(wasapi_stop_event_);
            }
            t.swap(wasapi_thread_);
        }
        if (t.joinable()) {
            t.join();
        }
        {
            std::lock_guard<std::mutex> lk(wasapi_mutex_);
            if (wasapi_stop_event_) {
                CloseHandle(wasapi_stop_event_);
                wasapi_stop_event_ = nullptr;
            }
        }
#endif
    }

#if defined(_WIN32)
    void SignalWasapiInitDone() {
        {
            std::lock_guard<std::mutex> lk(wasapi_mutex_);
            wasapi_init_done_ = true;
        }
        wasapi_init_cv_.notify_all();
    }

    void WasapiRenderLoop() {
        const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool co_init = SUCCEEDED(co_hr);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render_client = nullptr;
        WAVEFORMATEX* mix_fmt = nullptr;
        HANDLE ready_event = nullptr;
        bool init_signaled = false;

        auto signal_once = [&]() {
            if (!init_signaled) {
                init_signaled = true;
                SignalWasapiInitDone();
            }
        };

        auto cleanup = [&]() {
            wasapi_active_.store(false, std::memory_order_release);
            signal_once();
            if (client) client->Stop();
            if (ready_event) CloseHandle(ready_event);
            if (mix_fmt) CoTaskMemFree(mix_fmt);
            if (render_client) render_client->Release();
            if (client) client->Release();
            if (device) device->Release();
            if (enumerator) enumerator->Release();
            if (co_init) CoUninitialize();
        };

        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !enumerator) {
            cleanup();
            return;
        }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || !device) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        }
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !device) {
            cleanup();
            return;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !client) {
            cleanup();
            return;
        }

        hr = client->GetMixFormat(&mix_fmt);
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !mix_fmt) {
            cleanup();
            return;
        }

        REFERENCE_TIME default_period = 200000;  // 20 ms default
        REFERENCE_TIME min_period = 0;
        (void)client->GetDevicePeriod(&default_period, &min_period);
        if (default_period <= 0) default_period = 200000;

        bool event_driven = false;
        ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ready_event) {
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                default_period,
                0,
                mix_fmt,
                nullptr);
            if (SUCCEEDED(hr)) {
                hr = client->SetEventHandle(ready_event);
                if (SUCCEEDED(hr)) {
                    event_driven = true;
                }
            }
            if (!event_driven) {
                // Re-activate fresh IAudioClient for polling fallback (Initialize is single-use).
                client->Release();
                client = nullptr;
                CloseHandle(ready_event);
                ready_event = nullptr;
                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&client));
                if (FAILED(hr) || !client) {
                    wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
                    cleanup();
                    return;
                }
            }
        }

        if (!event_driven) {
            constexpr REFERENCE_TIME kPollBufferDuration = 500000;  // 50 ms
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED, 0, kPollBufferDuration, 0, mix_fmt, nullptr);
            wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
            if (FAILED(hr)) {
                cleanup();
                return;
            }
        }

        UINT32 buffer_frames = 0;
        hr = client->GetBufferSize(&buffer_frames);
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || buffer_frames == 0) {
            cleanup();
            return;
        }

        hr = client->GetService(__uuidof(IAudioRenderClient),
                                reinterpret_cast<void**>(&render_client));
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !render_client) {
            cleanup();
            return;
        }

        const uint32_t out_ch = std::max<uint32_t>(1u, mix_fmt->nChannels);
        const uint32_t out_sr = std::max<uint32_t>(1u, mix_fmt->nSamplesPerSec);
        bool is_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
            is_float = (ext->SubFormat.Data1 == 0x00000003u);
        }

        // Pre-fill initial buffer with silence/mix before Start() to prevent startup glitches.
        std::vector<float> mix_buf;
        auto pump_available = [&]() -> bool {
            UINT32 padding = 0;
            const HRESULT pad_hr = client->GetCurrentPadding(&padding);
            if (FAILED(pad_hr)) {
                wasapi_last_hr_.store(static_cast<int32_t>(pad_hr), std::memory_order_release);
                return false;
            }
            const UINT32 avail = (padding < buffer_frames) ? (buffer_frames - padding) : 0u;
            if (avail == 0u) {
                return true;
            }
            BYTE* dst = nullptr;
            const HRESULT buf_hr = render_client->GetBuffer(avail, &dst);
            if (FAILED(buf_hr) || !dst) {
                wasapi_last_hr_.store(static_cast<int32_t>(buf_hr), std::memory_order_release);
                return buf_hr != AUDCLNT_E_DEVICE_INVALIDATED;
            }
            mix_buf.resize(static_cast<size_t>(avail) * out_ch);
            RenderMixFrames(mix_buf.data(), avail, out_ch, out_sr);
            if (is_float && mix_fmt->wBitsPerSample == 32u) {
                std::memcpy(dst, mix_buf.data(), mix_buf.size() * sizeof(float));
            } else if (mix_fmt->wBitsPerSample == 16u) {
                auto* out16 = reinterpret_cast<int16_t*>(dst);
                for (size_t i = 0u; i < mix_buf.size(); ++i) {
                    const float clamped = std::clamp(mix_buf[i], -1.0f, 1.0f);
                    out16[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
                }
            } else if (mix_fmt->wBitsPerSample == 32u) {
                auto* out32 = reinterpret_cast<int32_t*>(dst);
                for (size_t i = 0u; i < mix_buf.size(); ++i) {
                    const float clamped = std::clamp(mix_buf[i], -1.0f, 1.0f);
                    out32[i] = static_cast<int32_t>(static_cast<double>(clamped) * 2147483647.0);
                }
            } else {
                std::memset(dst, 0, static_cast<size_t>(avail) * mix_fmt->nBlockAlign);
            }
            const HRESULT rel_hr = render_client->ReleaseBuffer(avail, 0);
            if (SUCCEEDED(rel_hr)) {
                wasapi_frames_rendered_.fetch_add(avail, std::memory_order_relaxed);
            } else {
                wasapi_last_hr_.store(static_cast<int32_t>(rel_hr), std::memory_order_release);
            }
            return rel_hr != AUDCLNT_E_DEVICE_INVALIDATED;
        };

        (void)pump_available();

        hr = client->Start();
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr)) {
            cleanup();
            return;
        }

        wasapi_event_driven_.store(event_driven, std::memory_order_release);
        wasapi_sample_rate_.store(out_sr, std::memory_order_release);
        wasapi_channels_.store(out_ch, std::memory_order_release);
        wasapi_bits_per_sample_.store(mix_fmt->wBitsPerSample, std::memory_order_release);
        wasapi_buffer_frames_.store(buffer_frames, std::memory_order_release);
        wasapi_active_.store(true, std::memory_order_release);
        signal_once();

        while (!wasapi_stop_.load(std::memory_order_acquire)) {
            if (event_driven && ready_event && wasapi_stop_event_) {
                HANDLE wait_handles[2] = {wasapi_stop_event_, ready_event};
                const DWORD wr = WaitForMultipleObjects(2u, wait_handles, FALSE, 50u);
                if (wr == WAIT_OBJECT_0 || wasapi_stop_.load(std::memory_order_acquire)) {
                    break;
                }
            } else if (wasapi_stop_event_) {
                if (WaitForSingleObject(wasapi_stop_event_, 10u) == WAIT_OBJECT_0) {
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!pump_available()) {
                break;
            }
        }

        cleanup();
    }
#endif

    std::atomic<uint32_t> ref_count_{1};
    std::atomic<bool> engine_running_{true};
    std::mutex mutex_;
    std::unique_ptr<MasteringVoiceImpl> mastering_voice_;
    std::vector<std::unique_ptr<SubmixVoiceImpl>> submix_voices_;
    std::vector<std::unique_ptr<SourceVoiceImpl>> source_voices_;
    std::vector<IXAudio2EngineCallback*> engine_callbacks_;

#if defined(_WIN32)
    std::mutex wasapi_mutex_;
    std::condition_variable wasapi_init_cv_;
    bool wasapi_init_done_ = false;
    HANDLE wasapi_stop_event_ = nullptr;
    std::atomic<bool> wasapi_stop_{false};
    std::atomic<bool> wasapi_active_{false};
    std::atomic<bool> wasapi_event_driven_{false};
    std::atomic<uint32_t> wasapi_sample_rate_{0u};
    std::atomic<uint32_t> wasapi_channels_{0u};
    std::atomic<uint32_t> wasapi_bits_per_sample_{0u};
    std::atomic<uint32_t> wasapi_buffer_frames_{0u};
    std::atomic<uint64_t> wasapi_frames_rendered_{0u};
    std::atomic<int32_t> wasapi_last_hr_{0};
    std::thread wasapi_thread_;
#endif
};

void OPENSCARLETT_XAUDIO2_CALL SubmixVoiceImpl::DestroyVoice() {
    RecordOk("IXAudio2SubmixVoice::DestroyVoice", 6);
    if (engine_) {
        engine_->RemoveSubmixVoice(this);
    }
}

void OPENSCARLETT_XAUDIO2_CALL MasteringVoiceImpl::DestroyVoice() {
    RecordOk("IXAudio2MasteringVoice::DestroyVoice", 6);
    if (engine_) {
        engine_->RemoveMasteringVoice(this);
    }
}

void OPENSCARLETT_XAUDIO2_CALL SourceVoiceImpl::DestroyVoice() {
    RecordOk("IXAudio2SourceVoice::DestroyVoice", 6);
    if (engine_) {
        engine_->RemoveSourceVoice(this);
    }
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2Create(
    IXAudio2** ppXAudio2,
    uint32_t /*Flags*/,
    uint32_t /*XAudio2Processor*/) {
    if (!ppXAudio2) {
        return RecordHr("XAudio2Create", OS_XAUDIO2_E_POINTER);
    }
    *ppXAudio2 = nullptr;
    auto* engine = new (std::nothrow) XAudio2Impl();
    if (!engine) {
        return RecordHr("XAudio2Create", OS_XAUDIO2_E_OUTOFMEMORY);
    }
    *ppXAudio2 = engine;
    return RecordHr("XAudio2Create", OS_XAUDIO2_S_OK);
}

int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2CreateWithVersionInfo(
    IXAudio2** ppXAudio2,
    uint32_t Flags,
    uint32_t XAudio2Processor,
    uint32_t /*ntddiVersion*/) {
    const int32_t hr = XAudio2Create(ppXAudio2, Flags, XAudio2Processor);
    return RecordHr("XAudio2CreateWithVersionInfo", hr);
}

int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2RenderMixForTest(
    IXAudio2* pXAudio2,
    float* out_frames,
    uint32_t frame_count) {
    if (!pXAudio2 || !out_frames || frame_count == 0) {
        return RecordHr("OpenScarlettXAudio2RenderMixForTest", OS_XAUDIO2_E_INVALIDARG);
    }
    auto* impl = static_cast<XAudio2Impl*>(pXAudio2);
    const int32_t hr = impl->RenderMixFrames(out_frames, frame_count);
    return RecordHr("OpenScarlettXAudio2RenderMixForTest", hr);
}

int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2GetBackendInfo(
    IXAudio2* pXAudio2,
    OS_XAUDIO2_BACKEND_INFO* out_info) {
    if (!pXAudio2 || !out_info) {
        return RecordHr("OpenScarlettXAudio2GetBackendInfo", OS_XAUDIO2_E_INVALIDARG);
    }
    auto* impl = static_cast<XAudio2Impl*>(pXAudio2);
    impl->GetBackendInfo(*out_info);
    return RecordHr("OpenScarlettXAudio2GetBackendInfo", OS_XAUDIO2_S_OK);
}

}  // extern "C"
