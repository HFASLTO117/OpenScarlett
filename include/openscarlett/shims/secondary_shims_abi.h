#pragma once

// Clean-room MIT Secondary Console / GDK Shim ABI definitions for OpenScarlett:
//   1. pixevt.dll
//   2. xmem.dll (forwards to kernelx.dll)
//   3. xcrdapi.dll
//   4. libHttpClient.GDK.dll
//   5. XCurl.dll
//   6. Bink2Scarlett.dll
//   7. amd_fidelityfx_upscaler_xbox.dll
//   8. amd_fidelityfx_loader_xbox.dll
//   9. XboxBvhBuilder.dll
//
// Self-contained (does not include <windows.h>) to avoid Windows SDK collisions.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  define OS_SHIM_CALL __stdcall
#  define OS_SHIM_CDECL __cdecl
#  if defined(OPENSCARLETT_BUILDING_PIXEVT)
#    define OS_PIXEVT_API __declspec(dllexport)
#  else
#    define OS_PIXEVT_API __declspec(dllimport)
#  endif
#  if defined(OPENSCARLETT_BUILDING_XCRDAPI)
#    define OS_XCRDAPI_API __declspec(dllexport)
#  else
#    define OS_XCRDAPI_API __declspec(dllimport)
#  endif
#  if defined(OPENSCARLETT_BUILDING_LIBHTTPCLIENT)
#    define OS_HC_API __declspec(dllexport)
#  else
#    define OS_HC_API __declspec(dllimport)
#  endif
#  if defined(OPENSCARLETT_BUILDING_XCURL)
#    define OS_XCURL_API __declspec(dllexport)
#  else
#    define OS_XCURL_API __declspec(dllimport)
#  endif
#  if defined(OPENSCARLETT_BUILDING_BINK2)
#    define OS_BINK2_API __declspec(dllexport)
#  else
#    define OS_BINK2_API __declspec(dllimport)
#  endif
#  if defined(OPENSCARLETT_BUILDING_FFX_UPSCALER)
#    define OS_FFX_UPSCALER_API __declspec(dllexport)
#  else
#    define OS_FFX_UPSCALER_API __declspec(dllimport)
#  endif
#  if defined(OPENSCARLETT_BUILDING_FFX_LOADER)
#    define OS_FFX_LOADER_API __declspec(dllexport)
#  else
#    define OS_FFX_LOADER_API __declspec(dllimport)
#  endif
#  if defined(OPENSCARLETT_BUILDING_XBOX_BVH)
#    define OS_BVH_API __declspec(dllexport)
#  else
#    define OS_BVH_API __declspec(dllimport)
#  endif
#else
#  define OS_SHIM_CALL
#  define OS_SHIM_CDECL
#  define OS_PIXEVT_API __attribute__((visibility("default")))
#  define OS_XCRDAPI_API __attribute__((visibility("default")))
#  define OS_HC_API __attribute__((visibility("default")))
#  define OS_XCURL_API __attribute__((visibility("default")))
#  define OS_BINK2_API __attribute__((visibility("default")))
#  define OS_FFX_UPSCALER_API __attribute__((visibility("default")))
#  define OS_FFX_LOADER_API __attribute__((visibility("default")))
#  define OS_BVH_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration matching openscarlett/gdk/xgameruntime_abi.h
struct XAsyncBlock;

// Common HRESULT constants for secondary shims
constexpr int32_t OS_SHIM_S_OK = 0x00000000;
constexpr int32_t OS_SHIM_E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t OS_SHIM_E_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t OS_SHIM_E_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t OS_SHIM_E_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t OS_SHIM_E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t OS_SHIM_E_NOT_SUFFICIENT_BUFFER = static_cast<int32_t>(0x8007007Au);
constexpr int32_t OS_SHIM_E_NOT_FOUND = static_cast<int32_t>(0x80070490u);

// ===========================================================================
// 1. pixevt.dll — PIX CPU / GPU Event & Marker Shim
// ===========================================================================
OS_PIXEVT_API void OS_SHIM_CALL PIXBeginEventOnCpu(uint64_t color, const char* formatString);
OS_PIXEVT_API void OS_SHIM_CALL PIXEndEventOnCpu(void);
OS_PIXEVT_API void OS_SHIM_CALL PIXSetMarkerOnCpu(uint64_t color, const char* formatString);

OS_PIXEVT_API void OS_SHIM_CALL PIXBeginEventOnCommandList(
    void* commandList,
    uint64_t color,
    const char* formatString);
OS_PIXEVT_API void OS_SHIM_CALL PIXEndEventOnCommandList(void* commandList);
OS_PIXEVT_API void OS_SHIM_CALL PIXSetMarkerOnCommandList(
    void* commandList,
    uint64_t color,
    const char* formatString);

OS_PIXEVT_API void OS_SHIM_CALL PIXBeginEventOnCommandQueue(
    void* commandQueue,
    uint64_t color,
    const char* formatString);
OS_PIXEVT_API void OS_SHIM_CALL PIXEndEventOnCommandQueue(void* commandQueue);
OS_PIXEVT_API void OS_SHIM_CALL PIXSetMarkerOnCommandQueue(
    void* commandQueue,
    uint64_t color,
    const char* formatString);

OS_PIXEVT_API void OS_SHIM_CALL PIXNotifyWakeFromFenceSignal(void* eventHandle);
OS_PIXEVT_API void OS_SHIM_CALL PIXReportCounter(const wchar_t* name, float value);
OS_PIXEVT_API int32_t OS_SHIM_CALL PIXGpuCaptureNextFrames(
    const wchar_t* fileName,
    uint32_t numFrames);
OS_PIXEVT_API uint64_t OS_SHIM_CALL PIXGetEventCountForTest(void);

// ===========================================================================
// 2. xmem.dll — Function-pointer ABI typedefs (exports forward to kernelx.dll)
// ===========================================================================
typedef void*(OS_SHIM_CALL* PFN_XMemAlloc)(size_t dwSize, uint64_t dwAllocAttributes);
typedef void*(OS_SHIM_CALL* PFN_XMemAllocDefault)(size_t dwSize, uint64_t dwAllocAttributes);
typedef void(OS_SHIM_CALL* PFN_XMemFree)(void* pAddress, uint64_t dwAllocAttributes);
typedef void(OS_SHIM_CALL* PFN_XMemFreeDefault)(void* pAddress, uint64_t dwAllocAttributes);
typedef size_t(OS_SHIM_CALL* PFN_XMemSize)(void* pAddress, uint64_t dwAllocAttributes);
typedef size_t(OS_SHIM_CALL* PFN_XMemSizeDefault)(void* pAddress, uint64_t dwAllocAttributes);
typedef void*(OS_SHIM_CALL* PFN_XMemGetAuxiliaryTitleMemory)(
    size_t requestedSize,
    size_t* outActualSize);
typedef int32_t(OS_SHIM_CALL* PFN_XMemReleaseAuxiliaryTitleMemory)(void* baseAddress);
typedef void*(OS_SHIM_CALL* PFN_XMemMapPhysicalPages)(
    void* virtualAddress,
    uint64_t numberOfPages,
    uint32_t flAllocationType,
    uint32_t flProtect,
    const uint64_t* pPageArray);
typedef int32_t(OS_SHIM_CALL* PFN_XMemUnmapPhysicalPages)(
    void* virtualAddress,
    uint64_t numberOfPages);
typedef int32_t(OS_SHIM_CALL* PFN_XMemGetTelemetryCallCountForTest)(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls);

// ===========================================================================
// 3. xcrdapi.dll — Console Runtime Diagnostics & Crash Reporting API
// ===========================================================================
struct XCrdDiagnosticInfo {
    uint32_t structSize;
    uint32_t initialized;
    uint32_t breadcrumbCount;
    uint32_t errorReportCount;
    uint32_t lastErrorCode;
    uint32_t flags;
    char consoleFamily[64];
    char lastErrorComponent[64];
    char lastErrorMessage[256];
};

OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdInitialize(uint32_t flags);
OS_XCRDAPI_API void OS_SHIM_CALL XCrdUninitialize(void);
OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdGetDiagnosticInfo(XCrdDiagnosticInfo* outInfo);
OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdSetBreadcrumb(
    uint32_t slot,
    const char* tag,
    uint64_t value);
OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdGetBreadcrumb(
    uint32_t slot,
    char* outTag,
    size_t tagBufferSize,
    uint64_t* outValue);
OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdReportError(
    uint32_t errorCode,
    const char* component,
    const char* message);
OS_XCRDAPI_API int32_t OS_SHIM_CALL XCrdFlushDiagnostics(const char* optionalOutputPath);

// ===========================================================================
// 4. libHttpClient.GDK.dll — Microsoft libHttpClient C ABI (offline/XAsync)
// ===========================================================================
typedef struct HCCallObject* HCCallHandle;

enum HCTraceLevel : uint32_t {
    HCTraceLevel_Off = 0,
    HCTraceLevel_Error = 1,
    HCTraceLevel_Warning = 2,
    HCTraceLevel_Important = 3,
    HCTraceLevel_Information = 4,
    HCTraceLevel_Verbose = 5,
};

struct HCInitArgs {
    uint32_t flags;
    void* reserved;
};

OS_HC_API int32_t OS_SHIM_CALL HCInitialize(const HCInitArgs* args);
OS_HC_API void OS_SHIM_CALL HCCleanup(void);
OS_HC_API int32_t OS_SHIM_CALL HCCleanupAsync(XAsyncBlock* async);
OS_HC_API int32_t OS_SHIM_CALL HCGetLibVersion(const char** version);
OS_HC_API void OS_SHIM_CALL HCTraceSetTraceToDebugger(bool traceToDebugger);
OS_HC_API int32_t OS_SHIM_CALL HCSettingsSetTraceLevel(HCTraceLevel traceLevel);

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallCreate(HCCallHandle* call);
OS_HC_API HCCallHandle OS_SHIM_CALL HCHttpCallDuplicateHandle(HCCallHandle call);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallCloseHandle(HCCallHandle call);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetUrl(
    HCCallHandle call,
    const char* method,
    const char* url);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetHeader(
    HCCallHandle call,
    const char* headerName,
    const char* headerValue,
    bool allowTracing);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* requestBodyBytes,
    uint32_t requestBodySize);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetRequestBodyString(
    HCCallHandle call,
    const char* requestBodyString);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallPerformAsync(
    HCCallHandle call,
    XAsyncBlock* asyncBlock);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetStatusCode(
    HCCallHandle call,
    uint32_t* statusCode);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseString(
    HCCallHandle call,
    const char** responseString);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseBodyBytesSize(
    HCCallHandle call,
    size_t* bufferSize);
OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetHeader(
    HCCallHandle call,
    const char* headerName,
    const char** headerValue);
OS_HC_API int32_t OS_SHIM_CALL HCAddMockResponseForTest(
    const char* method,
    const char* urlPrefix,
    uint32_t statusCode,
    const char* contentType,
    const uint8_t* body,
    size_t bodySize);
OS_HC_API void OS_SHIM_CALL HCClearMockResponsesForTest(void);

// ===========================================================================
// 5. XCurl.dll — Xbox Curl Easy + Multi C ABI (offline-safe loopback)
// ===========================================================================
typedef void* XCurlEasyHandle;
typedef void* XCurlMultiHandle;

struct XCurlSlist {
    char* data;
    XCurlSlist* next;
};

enum XCurlCode : int32_t {
    XCURLE_OK = 0,
    XCURLE_UNSUPPORTED_PROTOCOL = 1,
    XCURLE_FAILED_INIT = 2,
    XCURLE_URL_MALFORMAT = 3,
    XCURLE_OUT_OF_MEMORY = 7,
    XCURLE_WRITE_ERROR = 23,
    XCURLE_BAD_FUNCTION_ARGUMENT = 43,
    XCURLE_UNKNOWN_OPTION = 48,
};

enum XCurlMCode : int32_t {
    XCURLM_OK = 0,
    XCURLM_BAD_HANDLE = 1,
    XCURLM_BAD_EASY_HANDLE = 2,
    XCURLM_OUT_OF_MEMORY = 3,
    XCURLM_ADDED_ALREADY = 7,
};

enum XCurlOption : int32_t {
    XCURLOPT_TIMEOUT = 13,
    XCURLOPT_VERBOSE = 41,
    XCURLOPT_NOBODY = 44,
    XCURLOPT_POST = 47,
    XCURLOPT_FOLLOWLOCATION = 52,
    XCURLOPT_POSTFIELDSIZE = 60,
    XCURLOPT_SSL_VERIFYPEER = 64,
    XCURLOPT_HTTPGET = 80,
    XCURLOPT_SSL_VERIFYHOST = 81,
    XCURLOPT_NOSIGNAL = 99,
    XCURLOPT_TIMEOUT_MS = 155,
    XCURLOPT_CONNECTTIMEOUT_MS = 156,
    XCURLOPT_WRITEDATA = 10001,
    XCURLOPT_URL = 10002,
    XCURLOPT_POSTFIELDS = 10015,
    XCURLOPT_USERAGENT = 10018,
    XCURLOPT_HTTPHEADER = 10023,
    XCURLOPT_HEADERDATA = 10029,
    XCURLOPT_CUSTOMREQUEST = 10036,
    XCURLOPT_PRIVATE = 10103,
    XCURLOPT_COPYPOSTFIELDS = 10165,
    XCURLOPT_WRITEFUNCTION = 20011,
    XCURLOPT_HEADERFUNCTION = 20079,
};

enum XCurlInfo : int32_t {
    XCURLINFO_EFFECTIVE_URL = 0x100001,
    XCURLINFO_CONTENT_TYPE = 0x100012,
    XCURLINFO_RESPONSE_CODE = 0x200002,
    XCURLINFO_HEADER_SIZE = 0x20000B,
    XCURLINFO_REQUEST_SIZE = 0x20000C,
    XCURLINFO_TOTAL_TIME = 0x300003,
    XCURLINFO_SIZE_DOWNLOAD = 0x300008,
    XCURLINFO_PRIVATE = 0x400015,
};

enum XCurlMsgType : int32_t {
    XCURLMSG_NONE = 0,
    XCURLMSG_DONE = 1,
};

struct XCurlMsg {
    XCurlMsgType msg;
    XCurlEasyHandle easy_handle;
    union {
        void* whatever;
        int32_t result;
    } data;
};

typedef size_t(OS_SHIM_CDECL* XCurlWriteCallback)(
    char* ptr,
    size_t size,
    size_t nmemb,
    void* userdata);

constexpr long XCURL_GLOBAL_DEFAULT = 3L;

OS_XCURL_API int32_t OS_SHIM_CDECL curl_global_init(long flags);
OS_XCURL_API void OS_SHIM_CDECL curl_global_cleanup(void);
OS_XCURL_API const char* OS_SHIM_CDECL curl_version(void);

OS_XCURL_API XCurlEasyHandle OS_SHIM_CDECL curl_easy_init(void);
OS_XCURL_API void OS_SHIM_CDECL curl_easy_cleanup(XCurlEasyHandle curl);
OS_XCURL_API void OS_SHIM_CDECL curl_easy_reset(XCurlEasyHandle curl);
OS_XCURL_API int32_t OS_SHIM_CDECL curl_easy_setopt(
    XCurlEasyHandle curl,
    int32_t option,
    ...);
OS_XCURL_API int32_t OS_SHIM_CDECL curl_easy_perform(XCurlEasyHandle curl);
OS_XCURL_API int32_t OS_SHIM_CDECL curl_easy_getinfo(
    XCurlEasyHandle curl,
    int32_t info,
    ...);
OS_XCURL_API const char* OS_SHIM_CDECL curl_easy_strerror(int32_t code);

OS_XCURL_API XCurlSlist* OS_SHIM_CDECL curl_slist_append(
    XCurlSlist* list,
    const char* string);
OS_XCURL_API void OS_SHIM_CDECL curl_slist_free_all(XCurlSlist* list);

OS_XCURL_API XCurlMultiHandle OS_SHIM_CDECL curl_multi_init(void);
OS_XCURL_API int32_t OS_SHIM_CDECL curl_multi_cleanup(XCurlMultiHandle multi_handle);
OS_XCURL_API int32_t OS_SHIM_CDECL curl_multi_add_handle(
    XCurlMultiHandle multi_handle,
    XCurlEasyHandle curl_handle);
OS_XCURL_API int32_t OS_SHIM_CDECL curl_multi_remove_handle(
    XCurlMultiHandle multi_handle,
    XCurlEasyHandle curl_handle);
OS_XCURL_API int32_t OS_SHIM_CDECL curl_multi_perform(
    XCurlMultiHandle multi_handle,
    int32_t* running_handles);
OS_XCURL_API XCurlMsg* OS_SHIM_CDECL curl_multi_info_read(
    XCurlMultiHandle multi_handle,
    int32_t* msgs_in_queue);

// ===========================================================================
// 6. Bink2Scarlett.dll — Bink2 Video Playback C ABI (backed by Bink2Decoder)
// ===========================================================================
constexpr uint32_t OS_BINKSURFACE32 = 3u;       // BGRA8
constexpr uint32_t OS_BINKSURFACE32RGBA = 6u;   // RGBA8
constexpr uint32_t OS_BINKCOPYALL = 0x80000000u;

struct BinkStruct {
    uint32_t Width;
    uint32_t Height;
    uint32_t Frames;
    uint32_t FrameNum;       // 1-based current frame index
    uint32_t LastFrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t OpenFlags;
    uint32_t Paused;
    uint32_t SoundOn;
};
typedef BinkStruct* HBink;

struct BinkSummary {
    uint32_t Width;
    uint32_t Height;
    uint32_t TotalFrames;
    uint32_t CurrentFrame;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t TotalDecodedFrames;
    uint32_t AudioTrackCount;
    uint32_t IsBink2;
    char FourCC[8];
};

OS_BINK2_API HBink OS_SHIM_CALL BinkOpen(const char* name, uint32_t flags);
OS_BINK2_API HBink OS_SHIM_CALL BinkOpenBuffer(
    const void* buffer,
    size_t bufferSize,
    uint32_t flags);
OS_BINK2_API void OS_SHIM_CALL BinkClose(HBink bink);
OS_BINK2_API int32_t OS_SHIM_CALL BinkDoFrame(HBink bink);
OS_BINK2_API void OS_SHIM_CALL BinkNextFrame(HBink bink);
OS_BINK2_API int32_t OS_SHIM_CALL BinkWait(HBink bink);
OS_BINK2_API int32_t OS_SHIM_CALL BinkCopyToBuffer(
    HBink bink,
    void* dest,
    int32_t destPitch,
    uint32_t destHeight,
    uint32_t destX,
    uint32_t destY,
    uint32_t flags);
OS_BINK2_API void OS_SHIM_CALL BinkGoto(HBink bink, uint32_t frame, uint32_t flags);
OS_BINK2_API void OS_SHIM_CALL BinkGetSummary(HBink bink, BinkSummary* summary);
OS_BINK2_API const char* OS_SHIM_CALL BinkGetError(void);
OS_BINK2_API void OS_SHIM_CALL BinkSetSoundOnOff(HBink bink, int32_t onOff);
OS_BINK2_API int32_t OS_SHIM_CALL BinkPause(HBink bink, int32_t pause);

// ===========================================================================
// 7. amd_fidelityfx_upscaler_xbox.dll — Live GPU FSR1 Compute Upscaler ABI
// ===========================================================================
typedef struct FfxUpscalerContext* FfxUpscalerContextHandle;

enum FfxReturnCode : int32_t {
    FFX_API_RETURN_OK = 0,
    FFX_API_RETURN_ERROR = 1,
    FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
    FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3,
    FFX_API_RETURN_NO_PROVIDER = 4,
    FFX_API_RETURN_ERROR_MEMORY = 5,
    FFX_API_RETURN_ERROR_PARAMETER = 6,
};

constexpr uint32_t FFX_UPSCALER_FLAG_NONE = 0x0u;
constexpr uint32_t FFX_UPSCALER_FLAG_PREFER_WARP = 0x1u;
constexpr uint32_t FFX_UPSCALER_FLAG_ENABLE_RCAS = 0x2u;
constexpr uint32_t FFX_UPSCALER_FLAG_ENABLE_DEBUG = 0x4u;

enum FfxUpscalerQualityMode : uint32_t {
    FFX_UPSCALER_QUALITY_NATIVEAA = 0,
    FFX_UPSCALER_QUALITY_ULTRA_QUALITY = 1,
    FFX_UPSCALER_QUALITY_QUALITY = 2,
    FFX_UPSCALER_QUALITY_BALANCED = 3,
    FFX_UPSCALER_QUALITY_PERFORMANCE = 4,
    FFX_UPSCALER_QUALITY_ULTRA_PERFORMANCE = 5,
};

enum FfxUpscalerQueryType : uint32_t {
    FFX_UPSCALER_QUERY_VERSION = 1,
    FFX_UPSCALER_QUERY_ADAPTER_INFO = 2,
    FFX_UPSCALER_QUERY_RENDER_RESOLUTION = 3,
};

struct FfxUpscalerCreateDesc {
    uint32_t maxRenderWidth;
    uint32_t maxRenderHeight;
    uint32_t maxDisplayWidth;
    uint32_t maxDisplayHeight;
    uint32_t flags;
    float defaultSharpness;
    void* nativeD3D12Device;
};

struct FfxUpscalerDispatchDesc {
    const uint8_t* inputRgba8;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint8_t* outputRgba8;
    size_t outputBufferBytes;
    uint32_t outputWidth;
    uint32_t outputHeight;
    float sharpness;        // < 0.0f uses context configured sharpness
    uint32_t enableRcas;    // 0 = off, 1 = on, 0xFFFFFFFFu = use context default
};

struct FfxUpscalerDispatchResult {
    uint64_t dispatchCount;
    uint64_t outputFnv1aHash;
    uint32_t easuGroupsX;
    uint32_t easuGroupsY;
    uint32_t rcasGroupsX;
    uint32_t rcasGroupsY;
    uint32_t usedGpuCompute;
    uint32_t isWarp;
    char adapterDescription[128];
};

struct FfxUpscalerConfigDesc {
    float sharpness;
    uint32_t enableRcas;
};

struct FfxUpscalerQueryDesc {
    uint32_t queryType;
    uint32_t version;
    uint32_t isWarp;
    uint32_t qualityMode;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t outRenderWidth;
    uint32_t outRenderHeight;
    char adapterDescription[128];
};

OS_FFX_UPSCALER_API uint32_t OS_SHIM_CALL ffxUpscalerGetVersion(void);
OS_FFX_UPSCALER_API int32_t OS_SHIM_CALL ffxUpscalerCreateContext(
    FfxUpscalerContextHandle* outContext,
    const FfxUpscalerCreateDesc* desc);
OS_FFX_UPSCALER_API int32_t OS_SHIM_CALL ffxUpscalerDestroyContext(
    FfxUpscalerContextHandle context);
OS_FFX_UPSCALER_API int32_t OS_SHIM_CALL ffxUpscalerDispatchRgba8(
    FfxUpscalerContextHandle context,
    const FfxUpscalerDispatchDesc* desc,
    FfxUpscalerDispatchResult* outResult);
OS_FFX_UPSCALER_API int32_t OS_SHIM_CALL ffxUpscalerConfigure(
    FfxUpscalerContextHandle context,
    const FfxUpscalerConfigDesc* config);
OS_FFX_UPSCALER_API int32_t OS_SHIM_CALL ffxUpscalerQuery(
    FfxUpscalerContextHandle context,
    FfxUpscalerQueryDesc* query);

// ===========================================================================
// 8. amd_fidelityfx_loader_xbox.dll — FidelityFX SDK 2.0 Loader C ABI
// ===========================================================================
typedef void* FfxContext;

struct FfxApiHeader {
    uint64_t type;
    FfxApiHeader* pNext;
};

typedef FfxApiHeader FfxCreateContextDescHeader;
typedef FfxApiHeader FfxConfigureDescHeader;
typedef FfxApiHeader FfxDispatchDescHeader;
typedef FfxApiHeader FfxQueryDescHeader;

typedef void*(OS_SHIM_CALL* FfxAllocFn)(void* pUserData, uint64_t size);
typedef void(OS_SHIM_CALL* FfxDeallocFn)(void* pUserData, void* pMem);

struct FfxAllocationCallbacks {
    void* pUserData;
    FfxAllocFn alloc;
    FfxDeallocFn dealloc;
};

enum FfxApiStructType : uint64_t {
    FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = 0x00000002ull,
    FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE = 0x00010000ull,
    FFX_API_DISPATCH_DESC_TYPE_UPSCALE = 0x00010001ull,
    FFX_API_QUERY_DESC_TYPE_GET_VERSIONS = 0x00010002ull,
    FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE = 0x00010003ull,
    FFX_API_QUERY_DESC_TYPE_UPSCALE_GETADAPTERINFO = 0x00010004ull,
    FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE = 0x00010005ull,
};

enum FfxUpscaleConfigureKey : uint64_t {
    FFX_UPSCALE_CONFIGURE_KEY_SHARPNESS = 1ull,
    FFX_UPSCALE_CONFIGURE_KEY_ENABLE_RCAS = 2ull,
};

struct FfxCreateBackendDX12Desc {
    FfxCreateContextDescHeader header;
    void* device;
};

struct FfxCreateContextDescUpscale {
    FfxCreateContextDescHeader header;
    uint32_t flags;
    uint32_t maxRenderWidth;
    uint32_t maxRenderHeight;
    uint32_t maxUpscaleWidth;
    uint32_t maxUpscaleHeight;
    float defaultSharpness;
};

struct FfxDispatchDescUpscale {
    FfxDispatchDescHeader header;
    const uint8_t* inputRgba8;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint8_t* outputRgba8;
    size_t outputBufferBytes;
    uint32_t outputWidth;
    uint32_t outputHeight;
    float sharpness;
    uint32_t enableRcas;
    FfxUpscalerDispatchResult* outResult;
};

struct FfxConfigureDescUpscaleKeyValue {
    FfxConfigureDescHeader header;
    uint64_t key;
    uint64_t u64Value;
    void* ptr;
};

struct FfxQueryDescGetVersions {
    FfxQueryDescHeader header;
    uint64_t createDescType;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
};

struct FfxQueryDescUpscaleGetRenderResolutionFromQualityMode {
    FfxQueryDescHeader header;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t qualityMode;
    uint32_t* pOutRenderWidth;
    uint32_t* pOutRenderHeight;
};

struct FfxQueryDescUpscaleGetAdapterInfo {
    FfxQueryDescHeader header;
    uint32_t outIsWarp;
    char adapterDescription[128];
};

OS_FFX_LOADER_API int32_t OS_SHIM_CALL ffxQuery(
    FfxContext* context,
    FfxQueryDescHeader* header);
OS_FFX_LOADER_API int32_t OS_SHIM_CALL ffxCreateContext(
    FfxContext* context,
    FfxCreateContextDescHeader* desc,
    const FfxAllocationCallbacks* memCb);
OS_FFX_LOADER_API int32_t OS_SHIM_CALL ffxDestroyContext(
    FfxContext* context,
    const FfxAllocationCallbacks* memCb);
OS_FFX_LOADER_API int32_t OS_SHIM_CALL ffxConfigure(
    FfxContext* context,
    const FfxConfigureDescHeader* desc);
OS_FFX_LOADER_API int32_t OS_SHIM_CALL ffxDispatch(
    FfxContext* context,
    const FfxDispatchDescHeader* desc);

// ===========================================================================
// 9. XboxBvhBuilder.dll — Xbox Hardware DXR / SAH BVH Acceleration Builder
// ===========================================================================
typedef struct XboxBvhBuilderObject* XboxBvhBuilderHandle;

constexpr uint32_t XBOX_BVH_AS_TYPE_TOP_LEVEL = 0u;
constexpr uint32_t XBOX_BVH_AS_TYPE_BOTTOM_LEVEL = 1u;

constexpr uint32_t XBOX_BVH_BUILD_NONE = 0x0u;
constexpr uint32_t XBOX_BVH_BUILD_ALLOW_UPDATE = 0x1u;
constexpr uint32_t XBOX_BVH_BUILD_ALLOW_COMPACTION = 0x2u;
constexpr uint32_t XBOX_BVH_BUILD_PREFER_FAST_TRACE = 0x4u;
constexpr uint32_t XBOX_BVH_BUILD_PREFER_FAST_BUILD = 0x8u;
constexpr uint32_t XBOX_BVH_BUILD_MINIMIZE_MEMORY = 0x10u;

struct XboxBvhCreateDesc {
    uint32_t preferWarp;
    uint32_t forceSoftwarePath;
    uint32_t enableDebugLayer;
    void* nativeD3D12Device;
};

struct XboxBvhCapabilities {
    uint32_t hardwareTier;
    uint32_t supportsDevice5;
    uint32_t supportsStateObjects;
    uint32_t supportsInlineRt;
    uint32_t usingHardwarePath;
    uint32_t isWarp;
    uint32_t vendorId;
    uint32_t deviceId;
    uint64_t dedicatedVideoMemory;
    char adapterDescription[128];
};

struct XboxBvhPrebuildInputs {
    uint32_t asType;
    uint32_t buildFlags;
    uint32_t primitiveCount;
    uint32_t geometryCount;
    uint32_t vertexStride;
    uint32_t isOpaque;
};

struct XboxBvhPrebuildInfo {
    uint64_t resultDataMaxSizeBytes;
    uint64_t scratchDataSizeBytes;
    uint64_t updateScratchDataSizeBytes;
    uint32_t fromHardwareDriver;
};

struct XboxBvhTriangle {
    float v0[3];
    float v1[3];
    float v2[3];
    float color[3];
    float reflectivity;
};

struct XboxBvhInstanceDesc {
    uint64_t blasId;
    float transform[3][4];
    uint32_t instanceId;
    uint32_t instanceMask;
    uint32_t hitGroupContribution;
    uint32_t flags;
};

struct XboxBvhRay {
    float origin[3];
    float direction[3];
    float tMin;
    float tMax;
};

struct XboxBvhHit {
    uint32_t hit;
    float t;
    float u;
    float v;
    uint32_t triangleIndex;
    float normal[3];
    float color[3];
    float reflectivity;
};

struct XboxBvhDispatchDesc {
    uint32_t width;
    uint32_t height;
    uint32_t maxBounces;
    float cameraPos[3];
    float cameraLookAt[3];
    float lightPos[3];
};

struct XboxBvhDispatchStats {
    uint64_t primaryRays;
    uint64_t primaryHits;
    uint64_t shadowRays;
    uint64_t shadowHits;
    uint64_t reflectionRays;
    uint64_t aabbTests;
    uint64_t triangleTests;
    uint32_t bvhNodes;
    uint32_t bvhLeafNodes;
    uint32_t maxTreeDepth;
    uint64_t frameHash;
};

OS_BVH_API int32_t OS_SHIM_CALL XboxBvhCreateBuilder(
    const XboxBvhCreateDesc* desc,
    XboxBvhBuilderHandle* outBuilder);
OS_BVH_API void OS_SHIM_CALL XboxBvhDestroyBuilder(XboxBvhBuilderHandle builder);
OS_BVH_API int32_t OS_SHIM_CALL XboxBvhGetCapabilities(
    XboxBvhBuilderHandle builder,
    XboxBvhCapabilities* outCaps);
OS_BVH_API int32_t OS_SHIM_CALL XboxBvhGetPrebuildInfo(
    XboxBvhBuilderHandle builder,
    const XboxBvhPrebuildInputs* inputs,
    XboxBvhPrebuildInfo* outInfo);
OS_BVH_API int32_t OS_SHIM_CALL XboxBvhBuildBlas(
    XboxBvhBuilderHandle builder,
    const XboxBvhTriangle* triangles,
    uint32_t triangleCount,
    uint32_t buildFlags,
    uint64_t* outBlasId);
OS_BVH_API int32_t OS_SHIM_CALL XboxBvhUpdateBlas(
    XboxBvhBuilderHandle builder,
    uint64_t blasId,
    const XboxBvhTriangle* triangles,
    uint32_t triangleCount);
OS_BVH_API int32_t OS_SHIM_CALL XboxBvhBuildTlas(
    XboxBvhBuilderHandle builder,
    const XboxBvhInstanceDesc* instances,
    uint32_t instanceCount,
    uint32_t buildFlags,
    uint64_t* outTlasId);
OS_BVH_API int32_t OS_SHIM_CALL XboxBvhTraceRay(
    XboxBvhBuilderHandle builder,
    uint64_t asId,
    const XboxBvhRay* ray,
    XboxBvhHit* outHit);
OS_BVH_API int32_t OS_SHIM_CALL XboxBvhDispatchRays(
    XboxBvhBuilderHandle builder,
    uint64_t tlasId,
    const XboxBvhDispatchDesc* desc,
    uint8_t* outRgba8,
    size_t outBufferBytes,
    XboxBvhDispatchStats* outStats);

#ifdef __cplusplus
}
#endif
