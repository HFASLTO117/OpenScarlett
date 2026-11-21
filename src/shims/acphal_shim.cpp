#define OPENSCARLETT_BUILDING_ACPHAL 1
#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kModName = "AcpHal.dll";
constexpr uint32_t kMaxAcpContexts = 256u;

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

bool GuidEquals(const AcpHalGuid* a, const AcpHalGuid& b) {
    return a && std::memcmp(a, &b, sizeof(AcpHalGuid)) == 0;
}

openscarlett::Xma2Config ToXma2Config(const AcpHalXmaContextDesc& desc) {
    openscarlett::Xma2Config cfg{};
    cfg.sample_rate = desc.sampleRate != 0 ? desc.sampleRate : 48000u;
    cfg.channels = desc.channels != 0 ? desc.channels : 2u;
    cfg.block_size = desc.blockSize != 0 ? desc.blockSize : openscarlett::kXma2DefaultBlockSize;
    cfg.subframes_per_frame = desc.subframesPerFrame != 0 ? desc.subframesPerFrame : 4u;
    cfg.quant_bits = desc.quantBits != 0 ? desc.quantBits : 12u;
    return cfg;
}

struct AcpShapeAllocation {
    uint32_t num_xma = 0;
    uint32_t num_src = 0;
    uint32_t num_eq = 0;
    uint32_t num_dma = 0;
};

class AcpHalImpl final : public IAcpHal {
public:
    int32_t OPENSCARLETT_ACPHAL_CALL QueryInterface(
        const AcpHalGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_AcpHal_IUnknown) || GuidEquals(riid, IID_IAcpHal)) {
            AddRef();
            *ppvObject = static_cast<IAcpHal*>(this);
            return RecordHr("IAcpHal::QueryInterface", ACPHAL_S_OK, 0);
        }
        return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_ACPHAL_CALL AddRef() override {
        RecordOk("IAcpHal::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_ACPHAL_CALL Release() override {
        RecordOk("IAcpHal::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_ACPHAL_CALL ConfigureXmaContext(
        uint32_t contextIndex, const AcpHalXmaContextDesc* desc) override {
        if (contextIndex >= kMaxAcpContexts || !desc) {
            return RecordHr("IAcpHal::ConfigureXmaContext", ACPHAL_E_INVALIDARG, 3);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto& dec = decoders_[contextIndex];
        if (!dec) {
            dec = std::make_unique<openscarlett::Xma2Decoder>();
        }
        const auto st = dec->Initialize(ToXma2Config(*desc));
        return RecordHr("IAcpHal::ConfigureXmaContext", st.ok() ? ACPHAL_S_OK : ACPHAL_E_INVALIDARG, 3);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommand(const AcpHalCommand* command) override {
        if (!command) {
            return RecordHr("IAcpHal::SubmitCommand", ACPHAL_E_INVALIDARG, 4);
        }
        const int32_t hr = ExecuteCommand(*command);
        return RecordHr("IAcpHal::SubmitCommand", hr, 4);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommands(
        uint32_t count, const AcpHalCommand* commands) override {
        if (count > 0 && !commands) {
            return RecordHr("IAcpHal::SubmitCommands", ACPHAL_E_INVALIDARG, 5);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const int32_t hr = ExecuteCommand(commands[i]);
            if (hr < 0) {
                return RecordHr("IAcpHal::SubmitCommands", hr, 5);
            }
        }
        return RecordHr("IAcpHal::SubmitCommands", ACPHAL_S_OK, 5);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL WaitIdle() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return RecordHr("IAcpHal::WaitIdle", ACPHAL_S_OK, 6);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL GetDecodedPacketCount(
        uint32_t contextIndex, uint64_t* packetCount) override {
        if (contextIndex >= kMaxAcpContexts || !packetCount) {
            return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_E_INVALIDARG, 7);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = decoders_.find(contextIndex);
        *packetCount = (it != decoders_.end() && it->second)
                           ? it->second->Stats().packets_parsed
                           : 0u;
        return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_S_OK, 7);
    }

private:
    int32_t ExecuteCommand(const AcpHalCommand& cmd) {
        std::lock_guard<std::mutex> lk(mutex_);
        switch (cmd.type) {
            case AcpHalCommand_Nop:
            case AcpHalCommand_Flush:
                return ACPHAL_S_OK;

            case AcpHalCommand_ResetContext: {
                if (cmd.contextIndex >= kMaxAcpContexts) {
                    return ACPHAL_E_INVALIDARG;
                }
                auto it = decoders_.find(cmd.contextIndex);
                if (it != decoders_.end() && it->second) {
                    it->second->Reset();
                }
                return ACPHAL_S_OK;
            }

            case AcpHalCommand_XmaDecode: {
                if (cmd.contextIndex >= kMaxAcpContexts ||
                    !cmd.inputData || cmd.inputBytes == 0) {
                    return ACPHAL_E_INVALIDARG;
                }
                auto& dec = decoders_[cmd.contextIndex];
                if (!dec) {
                    dec = std::make_unique<openscarlett::Xma2Decoder>();
                }
                if (!dec->IsInitialized()) {
                    const auto init_st = dec->Initialize(ToXma2Config(cmd.xmaConfig));
                    if (!init_st.ok()) {
                        return ACPHAL_E_INVALIDARG;
                    }
                }

                if (cmd.outputFloatPcm) {
                    std::vector<float> pcm;
                    const auto st = dec->DecodePackets(cmd.inputData, cmd.inputBytes, pcm);
                    if (!st.ok()) {
                        return ACPHAL_E_FAIL;
                    }
                    const uint32_t to_copy = std::min<uint32_t>(
                        cmd.outputCapacitySamples, static_cast<uint32_t>(pcm.size()));
                    if (to_copy > 0) {
                        std::memcpy(cmd.outputFloatPcm, pcm.data(), to_copy * sizeof(float));
                    }
                    if (cmd.outSamplesWritten) {
                        *cmd.outSamplesWritten = to_copy;
                    }
                    return ACPHAL_S_OK;
                }

                if (cmd.outputInt16Pcm) {
                    std::vector<int16_t> pcm;
                    const auto st = dec->DecodePacketsInt16(cmd.inputData, cmd.inputBytes, pcm);
                    if (!st.ok()) {
                        return ACPHAL_E_FAIL;
                    }
                    const uint32_t to_copy = std::min<uint32_t>(
                        cmd.outputCapacitySamples, static_cast<uint32_t>(pcm.size()));
                    if (to_copy > 0) {
                        std::memcpy(cmd.outputInt16Pcm, pcm.data(), to_copy * sizeof(int16_t));
                    }
                    if (cmd.outSamplesWritten) {
                        *cmd.outSamplesWritten = to_copy;
                    }
                    return ACPHAL_S_OK;
                }

                return ACPHAL_E_INVALIDARG;
            }

            default:
                return ACPHAL_E_INVALIDARG;
        }
    }

    std::atomic<uint32_t> ref_count_{1};
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<openscarlett::Xma2Decoder>> decoders_;
};

}  // namespace

extern "C" {

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalCreate(IAcpHal** ppAcpHal) {
    if (!ppAcpHal) {
        return RecordHr("AcpHalCreate", ACPHAL_E_POINTER);
    }
    *ppAcpHal = nullptr;
    auto* obj = new (std::nothrow) AcpHalImpl();
    if (!obj) {
        return RecordHr("AcpHalCreate", ACPHAL_E_OUTOFMEMORY);
    }
    *ppAcpHal = obj;
    return RecordHr("AcpHalCreate", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalAllocateShapeContexts(
    const AcpHalShapeContexts* pRequested,
    AcpHalShapeContexts* pAllocated) {
    if (!pRequested || !pAllocated) {
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_INVALIDARG);
    }
    auto* alloc = new (std::nothrow) AcpShapeAllocation();
    if (!alloc) {
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_OUTOFMEMORY);
    }
    alloc->num_xma = std::min<uint32_t>(pRequested->numXmaContexts, kMaxAcpContexts);
    alloc->num_src = pRequested->numSrcContexts;
    alloc->num_eq = pRequested->numEqContexts;
    alloc->num_dma = pRequested->numDmaContexts;

    pAllocated->numXmaContexts = alloc->num_xma;
    pAllocated->numSrcContexts = alloc->num_src;
    pAllocated->numEqContexts = alloc->num_eq;
    pAllocated->numDmaContexts = alloc->num_dma;
    pAllocated->internalAllocation = alloc;
    return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalReleaseShapeContexts(AcpHalShapeContexts* pAllocated) {
    if (!pAllocated) {
        return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_E_INVALIDARG);
    }
    delete static_cast<AcpShapeAllocation*>(pAllocated->internalAllocation);
    std::memset(pAllocated, 0, sizeof(*pAllocated));
    return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_S_OK);
}

}  // extern "C"
