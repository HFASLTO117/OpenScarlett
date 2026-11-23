#pragma once

// Xbox Series X|S Audio Processing Unit client (xapu.dll) ABI definitions for OpenScarlett.
// Provides thread-safe connection handle management, command submission, and non-blocking
// completion result dequeueing for audio middleware (e.g. fmod.dll / UE5).

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_XAPU)
#    define OPENSCARLETT_XAPU_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_XAPU_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_XAPU_CALL __stdcall
#else
#  define OPENSCARLETT_XAPU_API __attribute__((visibility("default")))
#  define OPENSCARLETT_XAPU_CALL
#endif

constexpr int32_t XAPU_S_OK = 0x00000000;
constexpr int32_t XAPU_S_FALSE = 0x00000001;
constexpr int32_t XAPU_S_EMPTY = 0x00000001;
constexpr int32_t XAPU_E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t XAPU_E_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t XAPU_E_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t XAPU_E_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t XAPU_E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t XAPU_E_BUFFER_TOO_SMALL = static_cast<int32_t>(0x8007007Au);
constexpr int32_t XAPU_E_QUEUE_EMPTY = static_cast<int32_t>(0x8AC81001u);
constexpr int32_t XAPU_E_BUFFER_TOO_SMALL_APU = static_cast<int32_t>(0x8AC81006u);
constexpr int32_t XAPU_E_BAD_OPUS_PACKET = static_cast<int32_t>(0x8AC81008u);

typedef void* XAPU_CONNECTION_HANDLE;

enum XApuProcessType : uint32_t {
    XApuProcessType_DecodeOpus = 1,
    XApuProcessType_DecodeConvertOpus = 2,
    XApuProcessType_ConvertPCM = 3,
};

enum XApuConnectOptions : uint32_t {
    XApuConnectOption_None = 0u,
    XApuConnectOption_DeinterleavedInput = 2u,
    XApuConnectOption_DeinterleavedOutput = 4u,
    XApuConnectOption_EnableResultReadyEvent = 8u,
};

enum XApuRetailCommandType : uint32_t {
    XApuRetailCommand_Start = 1u,
    XApuRetailCommand_Stop = 2u,
    XApuRetailCommand_Data = 3u,
    XApuRetailCommand_Reset = 5u,
};

struct XApuConnectInput {
    uint32_t processType;
    uint32_t streamCount;
    uint32_t commandQueueDepth;
    uint32_t sharedMemoryBytes;
    uint32_t maxFrameCountPerOutput;
    uint32_t options;
};
static_assert(sizeof(XApuConnectInput) == 24, "XApuConnectInput must be 24 bytes");

struct XApuConnectOutput {
    void* baseAddress;
    uint64_t sizeOrReadyEvent;
};
static_assert(sizeof(XApuConnectOutput) == 16, "XApuConnectOutput must be 16 bytes");

struct XApuRetailCommandHeader {
    uint32_t type;
    uint32_t streamIndex;
    uint32_t sequence;
    uint32_t options;
    uint64_t context;
};
static_assert(sizeof(XApuRetailCommandHeader) == 24, "XApuRetailCommandHeader must be 24 bytes");

#pragma pack(push, 1)
struct XApuRetailDataCommand {
    XApuRetailCommandHeader header;
    float pitchRamp;
    float targetPitch;
    uint32_t firstFrame;
    uint32_t frameCount;
    uint32_t inputDataLength;
    uint64_t inputDataPtr;
    uint32_t maxOutputDataLength;
    uint64_t outputDataPtr;
};
static_assert(sizeof(XApuRetailDataCommand) == 0x40, "XApuRetailDataCommand must be 64 bytes");

struct XApuRetailResult {
    XApuRetailCommandHeader header;
    uint32_t state;
    uint16_t stateData[4];
    uint64_t execTimeUs;
    int32_t hresult;
    uint32_t outputFrameCount;
    uint32_t outputDataLength;
    uint64_t outputDataPtr;
    float currentPitch;
    uint32_t inputDataLength;
    uint64_t inputDataPtr;
};
static_assert(sizeof(XApuRetailResult) == 0x50, "XApuRetailResult must be 80 bytes");
#pragma pack(pop)

enum XApuCommandType : uint32_t {
    XApuCommandType_Nop = 0,
    XApuCommandType_DecodeOpus = 1,
    XApuCommandType_DecodeXma2 = 2,
    XApuCommandType_ProcessDsp = 3,
    XApuCommandType_Flush = 4,
};

struct XApuCommandHeader {
    uint32_t commandType;
    uint32_t commandSize;
    uint64_t commandTag;
    uint32_t flags;
    uint32_t reserved;
};
static_assert(sizeof(XApuCommandHeader) == 24, "XApuCommandHeader must be 24 bytes");

struct XApuResultHeader {
    uint32_t resultType;
    uint32_t resultSize;
    uint64_t commandTag;
    int32_t status;
    uint32_t bytesWritten;
};
static_assert(sizeof(XApuResultHeader) == 24, "XApuResultHeader must be 24 bytes");

#ifdef __cplusplus
extern "C" {
#endif

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuConnect(
    void* arg1,
    void* arg2
#ifdef __cplusplus
    = nullptr
#endif
    ,
    void* arg3
#ifdef __cplusplus
    = nullptr
#endif
);

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuEnqueueCommand(
    XAPU_CONNECTION_HANDLE connection,
    const void* command,
    uint32_t commandSizeOrFlags);

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuDequeueResult(
    XAPU_CONNECTION_HANDLE connection,
    void* resultBuffer,
    uint32_t resultBufferSize
#ifdef __cplusplus
    = 0u
#endif
    ,
    uint32_t* outResultSize
#ifdef __cplusplus
    = nullptr
#endif
);

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuGetResultCount(
    XAPU_CONNECTION_HANDLE connection,
    uint32_t* resultCount);

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuGetUtilization(
    uint32_t codecType,
    uint32_t kind,
    uint32_t* outCounts);

OPENSCARLETT_XAPU_API int32_t OPENSCARLETT_XAPU_CALL XApuDisconnect(
    XAPU_CONNECTION_HANDLE connection);

#ifdef __cplusplus
}
#endif
