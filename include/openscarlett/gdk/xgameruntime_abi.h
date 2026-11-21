#pragma once

// Clean-room MIT Microsoft GDK xgameruntime.dll ABI definitions for OpenScarlett.
// Self-contained (does not include <windows.h>) to avoid Windows SDK collisions.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_XGAMERUNTIME)
#    define OPENSCARLETT_GDK_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_GDK_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_GDK_CALL __stdcall
#else
#  define OPENSCARLETT_GDK_API __attribute__((visibility("default")))
#  define OPENSCARLETT_GDK_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Standard HRESULT-compatible result type (32-bit signed integer).
typedef int32_t XGameResult;

// Standard Win32 / COM / GDK HRESULT codes.
constexpr XGameResult XGR_S_OK = 0x00000000;
constexpr XGameResult XGR_S_FALSE = 0x00000001;
constexpr XGameResult XGR_E_NOTIMPL = static_cast<XGameResult>(0x80004001u);
constexpr XGameResult XGR_E_NOINTERFACE = static_cast<XGameResult>(0x80004002u);
constexpr XGameResult XGR_E_POINTER = static_cast<XGameResult>(0x80004003u);
constexpr XGameResult XGR_E_ABORT = static_cast<XGameResult>(0x80004004u);
constexpr XGameResult XGR_E_FAIL = static_cast<XGameResult>(0x80004005u);
constexpr XGameResult XGR_E_PENDING = static_cast<XGameResult>(0x8000000Au);
constexpr XGameResult XGR_E_ACCESSDENIED = static_cast<XGameResult>(0x80070005u);
constexpr XGameResult XGR_E_HANDLE = static_cast<XGameResult>(0x80070006u);
constexpr XGameResult XGR_E_OUTOFMEMORY = static_cast<XGameResult>(0x8007000Eu);
constexpr XGameResult XGR_E_NOT_SUPPORTED = static_cast<XGameResult>(0x80070032u);
constexpr XGameResult XGR_E_INVALIDARG = static_cast<XGameResult>(0x80070057u);
constexpr XGameResult XGR_E_NOT_SUFFICIENT_BUFFER = static_cast<XGameResult>(0x8007007Au);
constexpr XGameResult XGR_E_NO_MORE_ITEMS = static_cast<XGameResult>(0x80070103u);
constexpr XGameResult XGR_E_NOT_FOUND = static_cast<XGameResult>(0x80070490u);
constexpr XGameResult XGR_E_GAMERUNTIME_NOT_INITIALIZED = static_cast<XGameResult>(0x89240100u);
constexpr XGameResult XGR_E_GS_UPDATE_TOO_BIG = static_cast<XGameResult>(0x80830002u);
constexpr XGameResult XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<XGameResult>(0x80830007u);
constexpr XGameResult XGR_E_GS_BLOB_NOT_FOUND = static_cast<XGameResult>(0x80830008u);
constexpr XGameResult XGR_E_GS_CONTAINER_NOT_IN_SYNC = static_cast<XGameResult>(0x8083000Bu);

// ---------------------------------------------------------------------------
// Game Runtime Lifecycle
// ---------------------------------------------------------------------------
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameRuntimeInitialize(void);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameRuntimeUninitialize(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameGetXboxTitleId(uint32_t* titleId);

// ---------------------------------------------------------------------------
// XTaskQueue
// ---------------------------------------------------------------------------
typedef struct XTaskQueueObject* XTaskQueueHandle;
typedef struct XTaskQueuePortObject* XTaskQueuePortHandle;

enum XTaskQueueDispatchMode : uint32_t {
    XTaskQueueDispatchMode_ThreadPool = 0,
    XTaskQueueDispatchMode_SerializedThreadPool = 1,
    XTaskQueueDispatchMode_Manual = 2,
    XTaskQueueDispatchMode_Immediate = 3,
};

enum XTaskQueuePort : uint32_t {
    XTaskQueuePort_Work = 0,
    XTaskQueuePort_Completion = 1,
};

struct XTaskQueueRegistrationToken {
    uint64_t token;
};

typedef void(OPENSCARLETT_GDK_CALL* XTaskQueueCallback)(void* context, bool canceled);
typedef void(OPENSCARLETT_GDK_CALL* XTaskQueueMonitorCallback)(
    void* context,
    XTaskQueueHandle queue,
    XTaskQueuePort port);
typedef void(OPENSCARLETT_GDK_CALL* XTaskQueueTerminatedCallback)(void* context);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueCloseHandle(XTaskQueueHandle queue);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XTaskQueueGetCurrentProcessTaskQueue(
    XTaskQueueHandle* queue);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueSetCurrentProcessTaskQueue(
    XTaskQueueHandle queue);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback callback);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueGetPort(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    XTaskQueuePortHandle* portHandle);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueCreateComposite(
    XTaskQueuePortHandle workPort,
    XTaskQueuePortHandle completionPort,
    XTaskQueueHandle* queue);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XTaskQueueRegisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* waitHandle,
    void* callbackContext,
    XTaskQueueCallback callback,
    XTaskQueueRegistrationToken* token);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XTaskQueueUnregisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);

// ---------------------------------------------------------------------------
// XAsync
// ---------------------------------------------------------------------------
struct XAsyncBlock;
typedef void(OPENSCARLETT_GDK_CALL* XAsyncCompletionRoutine)(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle queue;
    void* context;
    XAsyncCompletionRoutine callback;
    unsigned char internal[sizeof(void*) * 4];
};

enum XAsyncOp : uint32_t {
    XAsyncOp_Begin = 0,
    XAsyncOp_DoWork = 1,
    XAsyncOp_GetResult = 2,
    XAsyncOp_Cancel = 3,
    XAsyncOp_Cleanup = 4,
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    size_t bufferSize;
    void* buffer;
    void* context;
};

typedef XGameResult(OPENSCARLETT_GDK_CALL* XAsyncProvider)(
    XAsyncOp op,
    const XAsyncProviderData* data);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider provider);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncSchedule(
    XAsyncBlock* asyncBlock,
    uint32_t delayInMs);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XAsyncComplete(
    XAsyncBlock* asyncBlock,
    XGameResult result,
    size_t requiredBufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncGetStatus(
    XAsyncBlock* asyncBlock,
    bool wait);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResultSize(
    XAsyncBlock* asyncBlock,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XAsyncCancel(XAsyncBlock* asyncBlock);

typedef XGameResult(OPENSCARLETT_GDK_CALL* XAsyncWork)(XAsyncBlock* asyncBlock);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XAsyncRun(
    XAsyncBlock* asyncBlock,
    XAsyncWork work);

// ---------------------------------------------------------------------------
// XUser
// ---------------------------------------------------------------------------
typedef struct XUser* XUserHandle;

struct XUserLocalId {
    uint64_t value;
};

enum XUserAddOptions : uint32_t {
    XUserAddOptions_None = 0x00,
    XUserAddOptions_AddDefaultUserSilently = 0x01,
    XUserAddOptions_AllowGuests = 0x02,
    XUserAddOptions_AddDefaultUserAllowingUI = 0x04,
};

enum XUserGamertagComponent : uint32_t {
    XUserGamertagComponent_Classic = 0,
    XUserGamertagComponent_Modern = 1,
    XUserGamertagComponent_ModernSuffix = 2,
    XUserGamertagComponent_UniqueModern = 3,
};

enum XUserState : uint32_t {
    XUserState_SignedIn = 0,
    XUserState_SigningOut = 1,
    XUserState_SignedOut = 2,
};

enum XUserAgeGroup : uint32_t {
    XUserAgeGroup_Unknown = 0,
    XUserAgeGroup_Child = 1,
    XUserAgeGroup_Teen = 2,
    XUserAgeGroup_Adult = 3,
};

enum XUserPrivilegeOptions : uint32_t {
    XUserPrivilegeOptions_None = 0x00,
    XUserPrivilegeOptions_AllUsers = 0x01,
};

enum XUserPrivilege : uint32_t {
    XUserPrivilege_CrossPlay = 185,
    XUserPrivilege_Clubs = 188,
    XUserPrivilege_Sessions = 189,
    XUserPrivilege_Broadcast = 190,
    XUserPrivilege_ManageProfilePrivacy = 196,
    XUserPrivilege_GameDvr = 198,
    XUserPrivilege_MultiplayerParties = 203,
    XUserPrivilege_CloudManageSession = 207,
    XUserPrivilege_CloudJoinSession = 208,
    XUserPrivilege_CloudSavedGames = 209,
    XUserPrivilege_SocialNetworkSharing = 220,
    XUserPrivilege_UserGeneratedContent = 247,
    XUserPrivilege_Communications = 252,
    XUserPrivilege_Multiplayer = 254,
    XUserPrivilege_AddFriends = 255,
};

enum XUserPrivilegeDenyReason : uint32_t {
    XUserPrivilegeDenyReason_None = 0,
    XUserPrivilegeDenyReason_PurchaseRequired = 1,
    XUserPrivilegeDenyReason_Restricted = 2,
    XUserPrivilegeDenyReason_Banned = 3,
    XUserPrivilegeDenyReason_Unknown = 0xFFFFFFFFu,
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserAddAsync(
    XUserAddOptions options,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XUserCloseHandle(XUserHandle user);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetId(
    XUserHandle user,
    uint64_t* userId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetLocalId(
    XUserHandle user,
    XUserLocalId* userLocalId);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetState(
    XUserHandle user,
    XUserState* state);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetAgeGroup(
    XUserHandle user,
    XUserAgeGroup* ageGroup);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetMaxUsers(uint32_t* maxUsers);
enum XUserGamerPictureSize : uint32_t {
    XUserGamerPictureSize_Small = 0,
    XUserGamerPictureSize_Medium = 1,
    XUserGamerPictureSize_Large = 2,
    XUserGamerPictureSize_ExtraLarge = 3,
};

constexpr size_t XUserGamertagComponentClassicMaxBytes = 16;
constexpr size_t XUserGamertagComponentModernMaxBytes = 97;
constexpr size_t XUserGamertagComponentModernSuffixMaxBytes = 15;
constexpr size_t XUserGamertagComponentUniqueModernMaxBytes = 101;

OPENSCARLETT_GDK_API int32_t OPENSCARLETT_GDK_CALL XUserCompare(
    XUserHandle user1,
    XUserHandle user2);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize pictureSize,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed);

// ---------------------------------------------------------------------------
// XGameSave
// ---------------------------------------------------------------------------
typedef struct XGameSaveProvider* XGameSaveProviderHandle;
typedef struct XGameSaveContainer* XGameSaveContainerHandle;
typedef struct XGameSaveUpdate* XGameSaveUpdateHandle;

struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    int64_t lastModifiedTime;
    bool needsSync;
};

struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};

typedef bool(OPENSCARLETT_GDK_CALL* XGameSaveContainerInfoCallback)(
    const XGameSaveContainerInfo* container,
    void* context);
typedef bool(OPENSCARLETT_GDK_CALL* XGameSaveBlobInfoCallback)(
    const XGameSaveBlobInfo* info,
    void* context);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XGameSaveProviderHandle* provider);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameSaveCloseProvider(
    XGameSaveProviderHandle provider);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* result);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameSaveCloseContainer(
    XGameSaveContainerHandle context);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* result);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdate(
    XGameSaveUpdateHandle updateContext);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XGameSaveCloseUpdate(
    XGameSaveUpdateHandle updateContext);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateContainerInfoByName(
    XGameSaveProviderHandle provider,
    const char* containerNamePrefix,
    void* context,
    XGameSaveContainerInfoCallback callback);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdateResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainerAsync(
    XGameSaveProviderHandle provider,
    const char* name,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainerResult(
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuotaAsync(
    XGameSaveProviderHandle provider,
    XAsyncBlock* async);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota);

// ---------------------------------------------------------------------------
// XSystem
// ---------------------------------------------------------------------------
enum XSystemDeviceType : uint32_t {
    XSystemDeviceType_Unknown = 0x00,
    XSystemDeviceType_Pc = 0x01,
    XSystemDeviceType_XboxOne = 0x02,
    XSystemDeviceType_XboxOneS = 0x03,
    XSystemDeviceType_XboxOneX = 0x04,
    XSystemDeviceType_XboxOneXDevkit = 0x05,
    XSystemDeviceType_XboxScarlettLockhart = 0x06,
    XSystemDeviceType_XboxScarlettAnaconda = 0x07,
    XSystemDeviceType_XboxScarlettDevkit = 0x08,
};

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4201)
#endif
struct XVersion {
    union {
        struct {
            uint16_t major;
            uint16_t minor;
            uint16_t build;
            uint16_t revision;
        };
        uint64_t Value;
    };
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

static_assert(sizeof(XVersion) == 8, "XVersion must be 8 bytes");

struct XSystemAnalyticsInfo {
    XVersion osVersion;
    XVersion hostingOsVersion;
    char family[64];
    char form[64];
};

struct XSystemRuntimeInfo {
    XVersion runtimeVersion;
    XVersion availableVersion;
};

OPENSCARLETT_GDK_API XSystemDeviceType OPENSCARLETT_GDK_CALL XSystemGetDeviceType(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed);
OPENSCARLETT_GDK_API XSystemAnalyticsInfo OPENSCARLETT_GDK_CALL XSystemGetAnalyticsInfo(void);
OPENSCARLETT_GDK_API XSystemRuntimeInfo OPENSCARLETT_GDK_CALL XSystemGetRuntimeInfo(void);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XSystemIsHandleValid(void* handle);

// ---------------------------------------------------------------------------
// XMem
// ---------------------------------------------------------------------------
typedef void*(OPENSCARLETT_GDK_CALL* XMemAllocFunction)(size_t size, uint64_t attributes);
typedef void(OPENSCARLETT_GDK_CALL* XMemFreeFunction)(void* pointer, uint64_t attributes);

OPENSCARLETT_GDK_API void* OPENSCARLETT_GDK_CALL XMemAllocDefault(
    size_t size,
    uint64_t attributes);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XMemFreeDefault(
    void* pointer,
    uint64_t attributes);
OPENSCARLETT_GDK_API void* OPENSCARLETT_GDK_CALL XMemAlloc(
    size_t size,
    uint64_t attributes);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XMemFree(
    void* pointer,
    uint64_t attributes);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XMemSetAllocationHooks(
    XMemAllocFunction allocHook,
    XMemFreeFunction freeHook);

// ---------------------------------------------------------------------------
// XPackage
// ---------------------------------------------------------------------------
enum XPackageChunkSelectorType : uint32_t {
    XPackageChunkSelectorType_Language = 1,
    XPackageChunkSelectorType_Tag = 2,
    XPackageChunkSelectorType_Feature = 3,
    XPackageChunkSelectorType_Chunk = 4,
};

struct XPackageChunkSelector {
    XPackageChunkSelectorType type;
    union {
        const char* language;
        const char* tag;
        const char* feature;
        uint32_t chunkId;
    };
};

enum XPackageChunkAvailability : uint32_t {
    XPackageChunkAvailability_Ready = 0,
    XPackageChunkAvailability_Pending = 1,
    XPackageChunkAvailability_Installable = 2,
    XPackageChunkAvailability_Unavailable = 3,
};

typedef bool(OPENSCARLETT_GDK_CALL* XPackageChunkAvailabilityCallback)(
    void* context,
    const XPackageChunkSelector* selector,
    XPackageChunkAvailability availability);

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize,
    char* buffer);
OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XPackageIsPackagedProcess(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XPackageEnumerateChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    void* context,
    XPackageChunkAvailabilityCallback callback);

// ---------------------------------------------------------------------------
// XDisplay
// ---------------------------------------------------------------------------
enum XDisplayHdrModeResult : uint32_t {
    XDisplayHdrModeResult_Unknown = 0,
    XDisplayHdrModeResult_Enabled = 1,
    XDisplayHdrModeResult_Disabled = 2,
};

struct XDisplayHdrModeInfo {
    float minToneMapLuminance;
    float maxToneMapLuminance;
    float maxFullFrameToneMapLuminance;
};

typedef struct XDisplayTimeoutDeferral* XDisplayTimeoutDeferralHandle;

OPENSCARLETT_GDK_API XDisplayHdrModeResult OPENSCARLETT_GDK_CALL XDisplayTryEnableHdrMode(
    uint32_t preferredHdrMode,
    XDisplayHdrModeInfo* displayHdrModeInfo);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XDisplayTryAcquireTimeoutDeferral(
    XDisplayTimeoutDeferralHandle* handle);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL XDisplayCloseTimeoutDeferralHandle(
    XDisplayTimeoutDeferralHandle handle);

// ---------------------------------------------------------------------------
// GDK Static Import Runtime Dispatch (InitializeApiImpl / QueryApiImpl)
// ---------------------------------------------------------------------------
struct XgrGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};
static_assert(sizeof(XgrGuid) == 16, "XgrGuid must be 16 bytes");

constexpr XgrGuid XGR_IID_IUNKNOWN = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr XgrGuid XGR_API_GUID_TASK_QUEUE = {
    0x4F534752u, 0x0001u, 0x4000u, {0x80, 0x01, 0x00, 0x58, 0x54, 0x51, 0x00, 0x01}};
constexpr XgrGuid XGR_API_GUID_ASYNC = {
    0x4F534752u, 0x0002u, 0x4000u, {0x80, 0x02, 0x00, 0x58, 0x41, 0x53, 0x00, 0x02}};
constexpr XgrGuid XGR_API_GUID_USER = {
    0x4F534752u, 0x0003u, 0x4000u, {0x80, 0x03, 0x00, 0x58, 0x55, 0x53, 0x00, 0x03}};
constexpr XgrGuid XGR_API_GUID_GAME_SAVE = {
    0x4F534752u, 0x0004u, 0x4000u, {0x80, 0x04, 0x00, 0x58, 0x47, 0x53, 0x00, 0x04}};
constexpr XgrGuid XGR_API_GUID_SYSTEM = {
    0x4F534752u, 0x0005u, 0x4000u, {0x80, 0x05, 0x00, 0x58, 0x53, 0x59, 0x00, 0x05}};
constexpr XgrGuid XGR_API_GUID_MEM = {
    0x4F534752u, 0x0006u, 0x4000u, {0x80, 0x06, 0x00, 0x58, 0x4D, 0x45, 0x00, 0x06}};
constexpr XgrGuid XGR_API_GUID_PACKAGE = {
    0x4F534752u, 0x0007u, 0x4000u, {0x80, 0x07, 0x00, 0x58, 0x50, 0x4B, 0x00, 0x07}};
constexpr XgrGuid XGR_API_GUID_DISPLAY = {
    0x4F534752u, 0x0008u, 0x4000u, {0x80, 0x08, 0x00, 0x58, 0x44, 0x50, 0x00, 0x08}};

typedef XGameResult(OPENSCARLETT_GDK_CALL* XgrQueryInterfaceFn)(
    const void* self,
    const XgrGuid* riid,
    void** ppvObject);
typedef uint32_t(OPENSCARLETT_GDK_CALL* XgrAddRefFn)(const void* self);
typedef uint32_t(OPENSCARLETT_GDK_CALL* XgrReleaseFn)(const void* self);

struct IXgrTaskQueueApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueCreate)(
        XTaskQueueDispatchMode workDispatchMode,
        XTaskQueueDispatchMode completionDispatchMode,
        XTaskQueueHandle* queue);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueDuplicateHandle)(
        XTaskQueueHandle queueHandle,
        XTaskQueueHandle* duplicatedHandle);
    void(OPENSCARLETT_GDK_CALL* XTaskQueueCloseHandle)(XTaskQueueHandle queue);
    bool(OPENSCARLETT_GDK_CALL* XTaskQueueGetCurrentProcessTaskQueue)(XTaskQueueHandle* queue);
    void(OPENSCARLETT_GDK_CALL* XTaskQueueSetCurrentProcessTaskQueue)(XTaskQueueHandle queue);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueSubmitCallback)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        void* callbackContext,
        XTaskQueueCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueSubmitDelayedCallback)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        uint32_t delayMs,
        void* callbackContext,
        XTaskQueueCallback callback);
    bool(OPENSCARLETT_GDK_CALL* XTaskQueueDispatch)(
        XTaskQueueHandle queue,
        XTaskQueuePort port,
        uint32_t timeoutInMs);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueTerminate)(
        XTaskQueueHandle queue,
        bool wait,
        void* callbackContext,
        XTaskQueueTerminatedCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XTaskQueueRegisterMonitor)(
        XTaskQueueHandle queue,
        void* callbackContext,
        XTaskQueueMonitorCallback callback,
        XTaskQueueRegistrationToken* token);
    void(OPENSCARLETT_GDK_CALL* XTaskQueueUnregisterMonitor)(
        XTaskQueueHandle queue,
        XTaskQueueRegistrationToken token);
};

struct IXgrAsyncApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncBegin)(
        XAsyncBlock* asyncBlock,
        void* context,
        const void* identity,
        const char* identityName,
        XAsyncProvider provider);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncSchedule)(
        XAsyncBlock* asyncBlock,
        uint32_t delayInMs);
    void(OPENSCARLETT_GDK_CALL* XAsyncComplete)(
        XAsyncBlock* asyncBlock,
        XGameResult result,
        size_t requiredBufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncGetStatus)(
        XAsyncBlock* asyncBlock,
        bool wait);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncGetResultSize)(
        XAsyncBlock* asyncBlock,
        size_t* bufferSize);
    XGameResult(OPENSCARLETT_GDK_CALL* XAsyncGetResult)(
        XAsyncBlock* asyncBlock,
        const void* identity,
        size_t bufferSize,
        void* buffer,
        size_t* bufferUsed);
    void(OPENSCARLETT_GDK_CALL* XAsyncCancel)(XAsyncBlock* asyncBlock);
};

struct IXgrUserApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XUserAddAsync)(
        XUserAddOptions options,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserAddResult)(
        XAsyncBlock* async,
        XUserHandle* newUser);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserDuplicateHandle)(
        XUserHandle handle,
        XUserHandle* duplicatedHandle);
    void(OPENSCARLETT_GDK_CALL* XUserCloseHandle)(XUserHandle user);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetId)(XUserHandle user, uint64_t* userId);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetLocalId)(
        XUserHandle user,
        XUserLocalId* userLocalId);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetGamertag)(
        XUserHandle user,
        XUserGamertagComponent gamertagComponent,
        size_t gamertagSize,
        char* gamertag,
        size_t* gamertagUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetState)(XUserHandle user, XUserState* state);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetAgeGroup)(
        XUserHandle user,
        XUserAgeGroup* ageGroup);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserCheckPrivilege)(
        XUserHandle user,
        XUserPrivilegeOptions options,
        XUserPrivilege privilege,
        bool* hasPrivilege,
        XUserPrivilegeDenyReason* reason);
    XGameResult(OPENSCARLETT_GDK_CALL* XUserGetMaxUsers)(uint32_t* maxUsers);
    int32_t(OPENSCARLETT_GDK_CALL* XUserCompare)(XUserHandle user1, XUserHandle user2);
};

struct IXgrGameSaveApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveInitializeProvider)(
        XUserHandle requestingUser,
        const char* configurationId,
        bool syncOnDemand,
        XGameSaveProviderHandle* provider);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveInitializeProviderAsync)(
        XUserHandle requestingUser,
        const char* configurationId,
        bool syncOnDemand,
        XAsyncBlock* async);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveInitializeProviderResult)(
        XAsyncBlock* async,
        XGameSaveProviderHandle* provider);
    void(OPENSCARLETT_GDK_CALL* XGameSaveCloseProvider)(XGameSaveProviderHandle provider);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveCreateContainer)(
        XGameSaveProviderHandle provider,
        const char* name,
        XGameSaveContainerHandle* result);
    void(OPENSCARLETT_GDK_CALL* XGameSaveCloseContainer)(XGameSaveContainerHandle context);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveCreateUpdate)(
        XGameSaveContainerHandle container,
        const char* containerDisplayName,
        XGameSaveUpdateHandle* result);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitBlobWrite)(
        XGameSaveUpdateHandle updateContext,
        const char* blobName,
        const uint8_t* data,
        size_t byteCount);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitBlobDelete)(
        XGameSaveUpdateHandle updateContext,
        const char* blobName);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveSubmitUpdate)(XGameSaveUpdateHandle updateContext);
    void(OPENSCARLETT_GDK_CALL* XGameSaveCloseUpdate)(XGameSaveUpdateHandle updateContext);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveReadBlobData)(
        XGameSaveContainerHandle container,
        const char** blobNames,
        uint32_t* countOfBlobs,
        size_t blobsSize,
        XGameSaveBlob* blobData);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveEnumerateContainerInfo)(
        XGameSaveProviderHandle provider,
        void* context,
        XGameSaveContainerInfoCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveEnumerateBlobInfo)(
        XGameSaveContainerHandle container,
        void* context,
        XGameSaveBlobInfoCallback callback);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveDeleteContainer)(
        XGameSaveProviderHandle provider,
        const char* name);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameSaveGetRemainingQuota)(
        XGameSaveProviderHandle provider,
        int64_t* remainingQuota);
};

struct IXgrSystemApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XGameRuntimeInitialize)(void);
    void(OPENSCARLETT_GDK_CALL* XGameRuntimeUninitialize)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XGameGetXboxTitleId)(uint32_t* titleId);
    XSystemDeviceType(OPENSCARLETT_GDK_CALL* XSystemGetDeviceType)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XSystemGetConsoleId)(
        size_t consoleIdSize,
        char* consoleId,
        size_t* consoleIdUsed);
    XGameResult(OPENSCARLETT_GDK_CALL* XSystemGetXboxLiveSandboxId)(
        size_t sandboxIdSize,
        char* sandboxId,
        size_t* sandboxIdUsed);
    XSystemAnalyticsInfo(OPENSCARLETT_GDK_CALL* XSystemGetAnalyticsInfo)(void);
    XSystemRuntimeInfo(OPENSCARLETT_GDK_CALL* XSystemGetRuntimeInfo)(void);
    bool(OPENSCARLETT_GDK_CALL* XSystemIsHandleValid)(void* handle);
};

struct IXgrMemApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    void*(OPENSCARLETT_GDK_CALL* XMemAllocDefault)(size_t size, uint64_t attributes);
    void(OPENSCARLETT_GDK_CALL* XMemFreeDefault)(void* pointer, uint64_t attributes);
    void*(OPENSCARLETT_GDK_CALL* XMemAlloc)(size_t size, uint64_t attributes);
    void(OPENSCARLETT_GDK_CALL* XMemFree)(void* pointer, uint64_t attributes);
    XGameResult(OPENSCARLETT_GDK_CALL* XMemSetAllocationHooks)(
        XMemAllocFunction allocHook,
        XMemFreeFunction freeHook);
};

struct IXgrPackageApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XGameResult(OPENSCARLETT_GDK_CALL* XPackageGetCurrentProcessPackageIdentifier)(
        size_t bufferSize,
        char* buffer);
    bool(OPENSCARLETT_GDK_CALL* XPackageIsPackagedProcess)(void);
    XGameResult(OPENSCARLETT_GDK_CALL* XPackageEnumerateChunkAvailability)(
        const char* packageIdentifier,
        uint32_t selectorCount,
        const XPackageChunkSelector* selectors,
        void* context,
        XPackageChunkAvailabilityCallback callback);
};

struct IXgrDisplayApiVtable {
    XgrQueryInterfaceFn QueryInterface;
    XgrAddRefFn AddRef;
    XgrReleaseFn Release;

    XDisplayHdrModeResult(OPENSCARLETT_GDK_CALL* XDisplayTryEnableHdrMode)(
        uint32_t preferredHdrMode,
        XDisplayHdrModeInfo* displayHdrModeInfo);
    XGameResult(OPENSCARLETT_GDK_CALL* XDisplayTryAcquireTimeoutDeferral)(
        XDisplayTimeoutDeferralHandle* handle);
    void(OPENSCARLETT_GDK_CALL* XDisplayCloseTimeoutDeferralHandle)(
        XDisplayTimeoutDeferralHandle handle);
};

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL InitializeApiImpl(void);
OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL UninitializeApiImpl(void);
OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL QueryApiImpl(
    const XgrGuid* apiId,
    void** ppApiVtable);
OPENSCARLETT_GDK_API int32_t OPENSCARLETT_GDK_CALL XgrGetTelemetryCallCountForTest(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outNotImplCalls);

#ifdef __cplusplus
}
#endif
