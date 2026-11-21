#pragma once

// Clean-room MIT Microsoft Xbox Services GDK C API (Microsoft.Xbox.Services.GDK.C.dll) ABI.

#include <cstddef>
#include <cstdint>

#include "openscarlett/gdk/xgameruntime_abi.h"

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_XSAPI)
#    define OPENSCARLETT_XSAPI_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_XSAPI_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_XSAPI_CALL __stdcall
#else
#  define OPENSCARLETT_XSAPI_API __attribute__((visibility("default")))
#  define OPENSCARLETT_XSAPI_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Standard XSAPI HRESULT error codes.
constexpr XGameResult XBL_S_OK = XGR_S_OK;
constexpr XGameResult XBL_E_RUNTIME_ERROR = static_cast<XGameResult>(0x89235100u);
constexpr XGameResult XBL_E_ALREADY_INITIALIZED = static_cast<XGameResult>(0x89235105u);
constexpr XGameResult XBL_E_NOT_INITIALIZED = static_cast<XGameResult>(0x89235106u);

// Standard XSAPI string buffer capacities.
constexpr size_t XBL_SCID_LENGTH = 38;
constexpr size_t XBL_GAMERTAG_CHAR_SIZE = 16 * 3;
constexpr size_t XBL_MODERN_GAMERTAG_CHAR_SIZE = 12 * 4 + 1;
constexpr size_t XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE = 14 + 1;
constexpr size_t XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE = 16 * 4 + 1;
constexpr size_t XBL_DISPLAY_NAME_CHAR_SIZE = 128;
constexpr size_t XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE = 225;
constexpr size_t XBL_GAMERSCORE_CHAR_SIZE = 16;

// ---------------------------------------------------------------------------
// Memory Hooks & Lifecycle & Context
// ---------------------------------------------------------------------------
typedef void*(OPENSCARLETT_XSAPI_CALL* XblMemAllocFunction)(size_t size, uint32_t memoryType);
typedef void(OPENSCARLETT_XSAPI_CALL* XblMemFreeFunction)(void* pointer, uint32_t memoryType);

typedef struct XblContext* XblContextHandle;

struct XblInitArgs {
    XTaskQueueHandle queue;
    const char* scid;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblMemSetFunctions(
    XblMemAllocFunction memAllocFunc,
    XblMemFreeFunction memFreeFunc);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblInitialize(
    const XblInitArgs* args);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblCleanup(void);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblCleanupAsync(XAsyncBlock* async);

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextCreateHandle(
    XUserHandle user,
    XblContextHandle* context);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextDuplicateHandle(
    XblContextHandle handle,
    XblContextHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblContextCloseHandle(
    XblContextHandle handle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextGetUser(
    XblContextHandle context,
    XUserHandle* user);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblContextGetXboxUserId(
    XblContextHandle context,
    uint64_t* xboxUserId);

// ---------------------------------------------------------------------------
// Achievements
// ---------------------------------------------------------------------------
typedef struct XblAchievementsResult* XblAchievementsResultHandle;

enum XblAchievementType : uint32_t {
    XblAchievementType_Unknown = 0,
    XblAchievementType_All = 1,
    XblAchievementType_Persistent = 2,
    XblAchievementType_Challenge = 3,
};

enum XblAchievementOrderBy : uint32_t {
    XblAchievementOrderBy_DefaultOrder = 0,
    XblAchievementOrderBy_TitleId = 1,
    XblAchievementOrderBy_UnlockTime = 2,
};

enum XblAchievementProgressState : uint32_t {
    XblAchievementProgressState_Unknown = 0,
    XblAchievementProgressState_Achieved = 1,
    XblAchievementProgressState_NotStarted = 2,
    XblAchievementProgressState_InProgress = 3,
};

enum XblAchievementParticipationType : uint32_t {
    XblAchievementParticipationType_Unknown = 0,
    XblAchievementParticipationType_Individual = 1,
    XblAchievementParticipationType_Group = 2,
};

struct XblAchievementRequirement {
    const char* id;
    const char* currentProgressValue;
    const char* targetProgressValue;
};

struct XblAchievementProgression {
    XblAchievementRequirement* requirements;
    size_t requirementsCount;
    int64_t timeUnlocked;
};

struct XblAchievement {
    const char* id;
    const char* serviceConfigurationId;
    const char* name;
    uint32_t titleId;
    XblAchievementProgressState progressState;
    XblAchievementProgression progression;
    const char* unlockedDescription;
    const char* lockedDescription;
    XblAchievementType type;
    XblAchievementParticipationType participationType;
    uint32_t percentComplete;
    uint32_t gamerscore;
    bool isSecret;
    bool isRevoked;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    uint32_t type,
    bool unlockedOnly,
    uint32_t orderBy,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsGetAchievementsForTitleIdResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsResultGetAchievements(
    XblAchievementsResultHandle resultHandle,
    const XblAchievement** achievements,
    size_t* achievementsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultHasNext(
    XblAchievementsResultHandle resultHandle,
    bool* hasNext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsResultDuplicateHandle(
    XblAchievementsResultHandle handle,
    XblAchievementsResultHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblAchievementsResultCloseHandle(
    XblAchievementsResultHandle resultHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblAchievementsUpdateAchievementForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    const char* serviceConfigurationId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async);

// ---------------------------------------------------------------------------
// Presence
// ---------------------------------------------------------------------------
typedef struct XblPresenceRecord* XblPresenceRecordHandle;

enum XblPresenceUserState : uint32_t {
    XblPresenceUserState_Unknown = 0,
    XblPresenceUserState_Online = 1,
    XblPresenceUserState_Away = 2,
    XblPresenceUserState_Offline = 3,
};

enum XblPresenceDeviceType : uint32_t {
    XblPresenceDeviceType_Unknown = 0,
    XblPresenceDeviceType_PC = 5,
    XblPresenceDeviceType_XboxOne = 7,
    XblPresenceDeviceType_Scarlett = 14,
};

struct XblPresenceRichPresenceIds {
    char scid[XBL_SCID_LENGTH];
    const char* presenceId;
    const char** presenceTokenIds;
    size_t presenceTokenIdsCount;
};

struct XblPresenceTitleRecord {
    uint32_t titleId;
    const char* titleName;
    int64_t lastModified;
    bool titleActive;
    const char* richPresenceString;
    uint32_t viewState;
};

struct XblPresenceDeviceRecord {
    XblPresenceDeviceType deviceType;
    const XblPresenceTitleRecord* titleRecords;
    size_t titleRecordsCount;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xuid,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetXuid(
    XblPresenceRecordHandle handle,
    uint64_t* xuid);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetUserState(
    XblPresenceRecordHandle handle,
    XblPresenceUserState* userState);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetDeviceRecords(
    XblPresenceRecordHandle handle,
    const XblPresenceDeviceRecord** deviceRecords,
    size_t* deviceRecordsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordDuplicateHandle(
    XblPresenceRecordHandle handle,
    XblPresenceRecordHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL XblPresenceRecordCloseHandle(
    XblPresenceRecordHandle handle);

// ---------------------------------------------------------------------------
// Profile
// ---------------------------------------------------------------------------
struct XblUserProfile {
    uint64_t xboxUserId;
    char appDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char appDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gameDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char gameDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gamerscore[XBL_GAMERSCORE_CHAR_SIZE];
    char gamertag[XBL_GAMERTAG_CHAR_SIZE];
    char modernGamertag[XBL_MODERN_GAMERTAG_CHAR_SIZE];
    char modernGamertagSuffix[XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE];
    char uniqueModernGamertag[XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE];
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfileResult(
    XAsyncBlock* async,
    XblUserProfile* profile);

// ---------------------------------------------------------------------------
// Social Relationships
// ---------------------------------------------------------------------------
typedef struct XblSocialRelationshipResult* XblSocialRelationshipResultHandle;

enum XblSocialRelationshipFilter : uint32_t {
    XblSocialRelationshipFilter_All = 0,
    XblSocialRelationshipFilter_Favorite = 1,
    XblSocialRelationshipFilter_LegacyXboxLiveFriends = 2,
};

struct XblSocialRelationship {
    uint64_t xboxUserId;
    bool isFavorite;
    bool isFollowingCaller;
    const char** socialNetworks;
    size_t socialNetworksCount;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialGetSocialRelationshipsAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XblSocialRelationshipFilter socialRelationshipFilter,
    size_t startIndex,
    size_t maxItems,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialGetSocialRelationshipsResult(
    XAsyncBlock* async,
    XblSocialRelationshipResultHandle* handle);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultGetRelationships(
    XblSocialRelationshipResultHandle resultHandle,
    const XblSocialRelationship** relationships,
    size_t* relationshipsCount);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultHasNext(
    XblSocialRelationshipResultHandle resultHandle,
    bool* hasNext);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultDuplicateHandle(
    XblSocialRelationshipResultHandle handle,
    XblSocialRelationshipResultHandle* duplicatedHandle);
OPENSCARLETT_XSAPI_API void OPENSCARLETT_XSAPI_CALL
XblSocialRelationshipResultCloseHandle(XblSocialRelationshipResultHandle resultHandle);

// ---------------------------------------------------------------------------
// User Statistics & Title-Managed Statistics
// ---------------------------------------------------------------------------
struct XblStatistic {
    const char* statisticName;
    const char* statisticType;
    const char* value;
};

struct XblServiceConfigurationStatistic {
    char serviceConfigurationId[XBL_SCID_LENGTH];
    XblStatistic* statistics;
    size_t statisticsCount;
};

struct XblUserStatisticsResult {
    uint64_t xboxUserId;
    XblServiceConfigurationStatistic* serviceConfigStatistics;
    size_t serviceConfigStatisticsCount;
};

enum XblTitleManagedStatType : uint32_t {
    XblTitleManagedStatType_Number = 0,
    XblTitleManagedStatType_String = 1,
};

struct XblTitleManagedStatistic {
    const char* statisticName;
    XblTitleManagedStatType statisticType;
    double numberValue;
    const char* stringValue;
};

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XAsyncBlock* async);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetSingleUserStatisticResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed);
OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL XblTitleManagedStatsWriteAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async);

#ifdef __cplusplus
}
#endif
