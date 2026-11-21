#include "openscarlett/gdk/xgameruntime_abi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

int g_failures = 0;

void CheckImpl(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, line, expr);
        ++g_failures;
    }
}

#define OS_REQUIRE(cond) CheckImpl((cond), #cond, __LINE__)

void SetEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

struct ScopedEnvVar {
    std::string name;
    bool had_old = false;
    std::string old_val;

    ScopedEnvVar(const char* n, const char* val) : name(n) {
#if defined(_WIN32)
        char buf[4096]{};
        const DWORD len = ::GetEnvironmentVariableA(n, buf, static_cast<DWORD>(sizeof(buf)));
        if (len > 0 && len < sizeof(buf)) {
            had_old = true;
            old_val.assign(buf, len);
        }
#else
        if (const char* cur = std::getenv(n)) {
            had_old = true;
            old_val = cur;
        }
#endif
        SetEnvVar(n, val);
    }

    ~ScopedEnvVar() {
        SetEnvVar(name.c_str(), had_old ? old_val.c_str() : nullptr);
    }
};

// All 292 exports from shipping XGamingRuntimeThunks.dll
constexpr const char* kAll292ThunksExports[] = {
    "QueryApiImpl",
    "XAppBroadcastGetStatus",
    "XAppBroadcastIsAppBroadcasting",
    "XAppBroadcastRegisterIsAppBroadcastingChanged",
    "XAppBroadcastShowUI",
    "XAppBroadcastUnregisterIsAppBroadcastingChanged",
    "XAppCaptureCloseScreenshotStream",
    "XAppCaptureDisableRecord",
    "XAppCaptureEnableRecord",
    "XAppCaptureMetadataAddDoubleEvent",
    "XAppCaptureMetadataAddInt32Event",
    "XAppCaptureMetadataAddStringEvent",
    "XAppCaptureMetadataRemainingStorageBytesAvailable",
    "XAppCaptureMetadataStartDoubleState",
    "XAppCaptureMetadataStartInt32State",
    "XAppCaptureMetadataStartStringState",
    "XAppCaptureMetadataStopAllStates",
    "XAppCaptureMetadataStopState",
    "XAppCaptureOpenScreenshotStream",
    "XAppCaptureReadScreenshotStream",
    "XAppCaptureRecordDiagnosticClip",
    "XAppCaptureRegisterMetadataPurged",
    "XAppCaptureTakeDiagnosticScreenshot",
    "XAppCaptureTakeScreenshot",
    "XAppCaptureUnRegisterMetadataPurged",
    "XAsyncBegin",
    "XAsyncCancel",
    "XAsyncComplete",
    "XAsyncGetResult",
    "XAsyncGetResultSize",
    "XAsyncGetStatus",
    "XAsyncRun",
    "XAsyncSchedule",
    "XClosedCaptionGetProperties",
    "XClosedCaptionSetEnabled",
    "XDisplayAcquireTimeoutDeferral",
    "XDisplayCloseTimeoutDeferralHandle",
    "XDisplayTryEnableHdrMode",
    "XErrorReport",
    "XErrorSetCallback",
    "XErrorSetOptions",
    "XGameEventWrite",
    "XGameGetXboxTitleId",
    "XGameInviteRegisterForEvent",
    "XGameInviteUnregisterForEvent",
    "XGameRuntimeInitialize",
    "XGameRuntimeIsFeatureAvailable",
    "XGameRuntimeUninitialize",
    "XGameSaveCloseContainer",
    "XGameSaveCloseProvider",
    "XGameSaveCloseUpdate",
    "XGameSaveCreateContainer",
    "XGameSaveCreateUpdate",
    "XGameSaveDeleteContainer",
    "XGameSaveDeleteContainerAsync",
    "XGameSaveDeleteContainerResult",
    "XGameSaveEnumerateBlobInfo",
    "XGameSaveEnumerateBlobInfoByName",
    "XGameSaveEnumerateContainerInfo",
    "XGameSaveEnumerateContainerInfoByName",
    "XGameSaveGetContainerInfo",
    "XGameSaveGetRemainingQuota",
    "XGameSaveGetRemainingQuotaAsync",
    "XGameSaveGetRemainingQuotaResult",
    "XGameSaveInitializeProvider",
    "XGameSaveInitializeProviderAsync",
    "XGameSaveInitializeProviderResult",
    "XGameSaveReadBlobData",
    "XGameSaveReadBlobDataAsync",
    "XGameSaveReadBlobDataResult",
    "XGameSaveSubmitBlobDelete",
    "XGameSaveSubmitBlobWrite",
    "XGameSaveSubmitUpdate",
    "XGameSaveSubmitUpdateAsync",
    "XGameSaveSubmitUpdateResult",
    "XGameUiSetNotificationPositionHint",
    "XGameUiShowAchievementsAsync",
    "XGameUiShowAchievementsResult",
    "XGameUiShowErrorDialogAsync",
    "XGameUiShowErrorDialogResult",
    "XGameUiShowMessageDialogAsync",
    "XGameUiShowMessageDialogResult",
    "XGameUiShowPlayerPickerAsync",
    "XGameUiShowPlayerPickerResult",
    "XGameUiShowPlayerPickerResultCount",
    "XGameUiShowPlayerProfileCardAsync",
    "XGameUiShowPlayerProfileCardResult",
    "XGameUiShowSendGameInviteAsync",
    "XGameUiShowSendGameInviteResult",
    "XGameUiShowTextEntryAsync",
    "XGameUiShowTextEntryResult",
    "XGameUiShowTextEntryResultSize",
    "XGameUiShowWebAuthenticationAsync",
    "XGameUiShowWebAuthenticationResult",
    "XGameUiShowWebAuthenticationResultSize",
    "XGameUiShowWebAuthenticationWithOptionsAsync",
    "XHighContrastGetMode",
    "XLaunchNewGame",
    "XLaunchUri",
    "XNetworkingGetConnectivityHint",
    "XNetworkingQueryPreferredLocalUdpMultiplayerPort",
    "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync",
    "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult",
    "XNetworkingQuerySecurityInformationForUrlAsync",
    "XNetworkingQuerySecurityInformationForUrlAsyncResult",
    "XNetworkingQuerySecurityInformationForUrlAsyncResultSize",
    "XNetworkingQuerySecurityInformationForUrlUtf16Async",
    "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult",
    "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize",
    "XNetworkingRegisterConnectivityHintChanged",
    "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged",
    "XNetworkingUnregisterConnectivityHintChanged",
    "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged",
    "XNetworkingVerifyServerCertificate",
    "XPackageChangeChunkInstallOrder",
    "XPackageCloseInstallationMonitorHandle",
    "XPackageCloseMountHandle",
    "XPackageCreateInstallationMonitor",
    "XPackageEnumerateChunkAvailability",
    "XPackageEnumerateFeatures",
    "XPackageEnumeratePackages",
    "XPackageEstimateDownloadSize",
    "XPackageFindChunkAvailability",
    "XPackageGetCurrentProcessPackageIdentifier",
    "XPackageGetInstallationProgress",
    "XPackageGetMountPath",
    "XPackageGetMountPathSize",
    "XPackageGetUserLocale",
    "XPackageGetWriteStats",
    "XPackageInstallChunks",
    "XPackageInstallChunksAsync",
    "XPackageInstallChunksResult",
    "XPackageIsPackagedProcess",
    "XPackageMount",
    "XPackageRegisterInstallationProgressChanged",
    "XPackageRegisterPackageInstalled",
    "XPackageUninstallChunks",
    "XPackageUninstallUWPInstance",
    "XPackageUnregisterInstallationProgressChanged",
    "XPackageUnregisterPackageInstalled",
    "XPackageUpdateInstallationMonitor",
    "XPersistentLocalStorageGetPath",
    "XPersistentLocalStorageGetPathSize",
    "XSpeechSynthesizerCloseHandle",
    "XSpeechSynthesizerCloseStreamHandle",
    "XSpeechSynthesizerCreate",
    "XSpeechSynthesizerCreateStreamFromSsml",
    "XSpeechSynthesizerCreateStreamFromText",
    "XSpeechSynthesizerEnumerateInstalledVoices",
    "XSpeechSynthesizerGetStreamData",
    "XSpeechSynthesizerGetStreamDataSize",
    "XSpeechSynthesizerSetCustomVoice",
    "XSpeechSynthesizerSetDefaultVoice",
    "XSpeechToTextBeginHypothesisString",
    "XSpeechToTextCancelHypothesisString",
    "XSpeechToTextFinalizeHypothesisString",
    "XSpeechToTextSendString",
    "XSpeechToTextSetPositionHint",
    "XSpeechToTextUpdateHypothesisString",
    "XStoreAcquireLicenseForDurablesAsync",
    "XStoreAcquireLicenseForDurablesResult",
    "XStoreAcquireLicenseForPackageAsync",
    "XStoreAcquireLicenseForPackageResult",
    "XStoreCanAcquireLicenseForPackageAsync",
    "XStoreCanAcquireLicenseForPackageResult",
    "XStoreCanAcquireLicenseForStoreIdAsync",
    "XStoreCanAcquireLicenseForStoreIdResult",
    "XStoreCloseContextHandle",
    "XStoreCloseLicenseHandle",
    "XStoreCloseProductsQueryHandle",
    "XStoreCreateContext",
    "XStoreDownloadAndInstallPackageUpdatesAsync",
    "XStoreDownloadAndInstallPackageUpdatesResult",
    "XStoreDownloadAndInstallPackagesAsync",
    "XStoreDownloadAndInstallPackagesResult",
    "XStoreDownloadAndInstallPackagesResultCount",
    "XStoreDownloadPackageUpdatesAsync",
    "XStoreDownloadPackageUpdatesResult",
    "XStoreEnumerateProductsQuery",
    "XStoreGetUserCollectionsIdAsync",
    "XStoreGetUserCollectionsIdResult",
    "XStoreGetUserCollectionsIdResultSize",
    "XStoreGetUserPurchaseIdAsync",
    "XStoreGetUserPurchaseIdResult",
    "XStoreGetUserPurchaseIdResultSize",
    "XStoreIsAvailabilityPurchasable",
    "XStoreIsLicenseValid",
    "XStoreProductsQueryHasMorePages",
    "XStoreProductsQueryNextPageAsync",
    "XStoreProductsQueryNextPageResult",
    "XStoreQueryAddOnLicensesAsync",
    "XStoreQueryAddOnLicensesResult",
    "XStoreQueryAddOnLicensesResultCount",
    "XStoreQueryAssociatedProductsAsync",
    "XStoreQueryAssociatedProductsResult",
    "XStoreQueryConsumableBalanceRemainingAsync",
    "XStoreQueryConsumableBalanceRemainingResult",
    "XStoreQueryEntitledProductsAsync",
    "XStoreQueryEntitledProductsResult",
    "XStoreQueryGameAndDlcPackageUpdatesAsync",
    "XStoreQueryGameAndDlcPackageUpdatesResult",
    "XStoreQueryGameAndDlcPackageUpdatesResultCount",
    "XStoreQueryGameLicenseAsync",
    "XStoreQueryGameLicenseResult",
    "XStoreQueryLicenseTokenAsync",
    "XStoreQueryLicenseTokenResult",
    "XStoreQueryLicenseTokenResultSize",
    "XStoreQueryPackageIdentifier",
    "XStoreQueryProductForCurrentGameAsync",
    "XStoreQueryProductForCurrentGameResult",
    "XStoreQueryProductForPackageAsync",
    "XStoreQueryProductForPackageResult",
    "XStoreQueryProductsAsync",
    "XStoreQueryProductsResult",
    "XStoreRegisterGameLicenseChanged",
    "XStoreRegisterPackageLicenseLost",
    "XStoreReportConsumableFulfillmentAsync",
    "XStoreReportConsumableFulfillmentResult",
    "XStoreShowAssociatedProductsUIAsync",
    "XStoreShowAssociatedProductsUIResult",
    "XStoreShowProductPageUIAsync",
    "XStoreShowProductPageUIResult",
    "XStoreShowPurchaseUIAsync",
    "XStoreShowPurchaseUIResult",
    "XStoreShowRateAndReviewUIAsync",
    "XStoreShowRateAndReviewUIResult",
    "XStoreShowRedeemTokenUIAsync",
    "XStoreShowRedeemTokenUIResult",
    "XStoreUnregisterGameLicenseChanged",
    "XStoreUnregisterPackageLicenseLost",
    "XSystemGetAnalyticsInfo",
    "XSystemGetConsoleId",
    "XSystemGetXboxLiveSandboxId",
    "XTaskQueueCloseHandle",
    "XTaskQueueCreate",
    "XTaskQueueCreateComposite",
    "XTaskQueueDispatch",
    "XTaskQueueDuplicateHandle",
    "XTaskQueueGetCurrentProcessTaskQueue",
    "XTaskQueueGetPort",
    "XTaskQueueRegisterMonitor",
    "XTaskQueueRegisterWaiter",
    "XTaskQueueSetCurrentProcessTaskQueue",
    "XTaskQueueSubmitCallback",
    "XTaskQueueSubmitDelayedCallback",
    "XTaskQueueTerminate",
    "XTaskQueueUnregisterMonitor",
    "XTaskQueueUnregisterWaiter",
    "XThreadAssertNotTimeSensitive",
    "XThreadIsTimeSensitive",
    "XThreadSetTimeSensitive",
    "XThreadVerifyNotTimeSensitive",
    "XUserAddAsync",
    "XUserAddResult",
    "XUserCheckPrivilege",
    "XUserCloseHandle",
    "XUserCloseSignOutDeferralHandle",
    "XUserCompare",
    "XUserDuplicateHandle",
    "XUserFindForDevice",
    "XUserFindUserById",
    "XUserFindUserByLocalId",
    "XUserGetAgeGroup",
    "XUserGetDefaultAudioEndpointUtf16",
    "XUserGetGamerPictureAsync",
    "XUserGetGamerPictureResult",
    "XUserGetGamerPictureResultSize",
    "XUserGetGamertag",
    "XUserGetId",
    "XUserGetIsGuest",
    "XUserGetLocalId",
    "XUserGetMaxUsers",
    "XUserGetSignOutDeferral",
    "XUserGetState",
    "XUserGetTokenAndSignatureAsync",
    "XUserGetTokenAndSignatureResult",
    "XUserGetTokenAndSignatureResultSize",
    "XUserGetTokenAndSignatureUtf16Async",
    "XUserGetTokenAndSignatureUtf16Result",
    "XUserGetTokenAndSignatureUtf16ResultSize",
    "XUserRegisterForChangeEvent",
    "XUserRegisterForDefaultAudioEndpointUtf16Changed",
    "XUserRegisterForDeviceAssociationChanged",
    "XUserResolveIssueWithUiAsync",
    "XUserResolveIssueWithUiResult",
    "XUserResolveIssueWithUiUtf16Async",
    "XUserResolveIssueWithUiUtf16Result",
    "XUserResolvePrivilegeWithUiAsync",
    "XUserResolvePrivilegeWithUiResult",
    "XUserUnregisterForChangeEvent",
    "XUserUnregisterForDefaultAudioEndpointUtf16Changed",
    "XUserUnregisterForDeviceAssociationChanged",
};

static_assert(
    sizeof(kAll292ThunksExports) / sizeof(kAll292ThunksExports[0]) == 292,
    "Must verify all 292 shipping XGamingRuntimeThunks.dll exports");

void TestExportParity292() {
#if defined(_WIN32)
    HMODULE h_xgr = ::GetModuleHandleW(L"xgameruntime.dll");
    OS_REQUIRE(h_xgr != nullptr);
    if (!h_xgr) return;

    size_t found = 0;
    for (const char* name : kAll292ThunksExports) {
        FARPROC p = ::GetProcAddress(h_xgr, name);
        if (!p) {
            std::fprintf(stderr, "[MISSING EXPORT] %s\n", name);
        } else {
            ++found;
        }
    }
    OS_REQUIRE(found == 292u);
    OS_REQUIRE(::GetProcAddress(h_xgr, "InitializeApiImpl") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_xgr, "InitializeApiImplEx") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_xgr, "UninitializeApiImpl") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_xgr, "XPersistentLocalStorageGetSpaceInfo") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_xgr, "XPersistentLocalStoragePromptUserForSpaceAsync") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_xgr, "XPersistentLocalStoragePromptUserForSpaceResult") != nullptr);
    std::printf("[PASS] 100%% xgameruntime.dll export parity (%zu / 292 + PLS/InitEx)\n", found);
#endif
}

void OPENSCARLETT_GDK_CALL DummyBroadcastCb(void* /*ctx*/) {}
void OPENSCARLETT_GDK_CALL DummyMetadataPurgedCb(void* /*ctx*/) {}

void TestNew31Exports() {
    // 1. XAppBroadcast* (5)
    OS_REQUIRE(!XAppBroadcastIsAppBroadcasting());
    XAppBroadcastStatus bstat{};
    OS_REQUIRE(XAppBroadcastGetStatus(nullptr, &bstat) == XGR_S_OK);
    OS_REQUIRE(!bstat.canStartBroadcast && !bstat.isAnyAppBroadcasting && bstat.isDisabledBySystem);
    OS_REQUIRE(XAppBroadcastGetStatus(nullptr, nullptr) == XGR_E_INVALIDARG);
    OS_REQUIRE(XAppBroadcastShowUI(nullptr) == XGR_S_OK);

    XTaskQueueRegistrationToken btok{};
    OS_REQUIRE(XAppBroadcastRegisterIsAppBroadcastingChanged(
                   nullptr, nullptr, &DummyBroadcastCb, &btok) == XGR_S_OK);
    OS_REQUIRE(btok.token != 0u);
    OS_REQUIRE(XAppBroadcastUnregisterIsAppBroadcastingChanged(btok, true));
    OS_REQUIRE(!XAppBroadcastUnregisterIsAppBroadcastingChanged(btok, false));

    // 2. XAppCapture* (19)
    OS_REQUIRE(XAppCaptureDisableRecord() == XGR_S_OK);
    OS_REQUIRE(XAppCaptureEnableRecord() == XGR_S_OK);

    uint64_t avail_before = 0;
    OS_REQUIRE(XAppCaptureMetadataRemainingStorageBytesAvailable(&avail_before) == XGR_S_OK);
    OS_REQUIRE(avail_before > 1024u);

    OS_REQUIRE(XAppCaptureMetadataAddStringEvent(
                   "Level", "Alexandria", XAppCaptureMetadataPriority_Informational) == XGR_S_OK);
    OS_REQUIRE(XAppCaptureMetadataAddInt32Event(
                   "Score", 4200, XAppCaptureMetadataPriority_Important) == XGR_S_OK);
    OS_REQUIRE(XAppCaptureMetadataAddDoubleEvent(
                   "LapTime", 73.25, XAppCaptureMetadataPriority_Informational) == XGR_S_OK);
    OS_REQUIRE(XAppCaptureMetadataStartStringState(
                   "Zone", "Library", XAppCaptureMetadataPriority_Informational) == XGR_S_OK);
    OS_REQUIRE(XAppCaptureMetadataStartInt32State(
                   "Health", 100, XAppCaptureMetadataPriority_Important) == XGR_S_OK);
    OS_REQUIRE(XAppCaptureMetadataStartDoubleState(
                   "Speed", 12.5, XAppCaptureMetadataPriority_Informational) == XGR_S_OK);
    OS_REQUIRE(XAppCaptureMetadataStopState("Zone") == XGR_S_OK);
    OS_REQUIRE(XAppCaptureMetadataStopAllStates() == XGR_S_OK);

    uint64_t avail_after = 0;
    OS_REQUIRE(XAppCaptureMetadataRemainingStorageBytesAvailable(&avail_after) == XGR_S_OK);
    OS_REQUIRE(avail_after < avail_before);

    XTaskQueueRegistrationToken mtok{};
    OS_REQUIRE(XAppCaptureRegisterMetadataPurged(
                   nullptr, nullptr, &DummyMetadataPurgedCb, &mtok) == XGR_S_OK);
    OS_REQUIRE(XAppCaptureUnRegisterMetadataPurged(mtok, true));

    // Take screenshot + open/read/close stream
    XAppCaptureTakeScreenshotResult shot{};
    OS_REQUIRE(XAppCaptureTakeScreenshot(nullptr, &shot) == XGR_S_OK);
    OS_REQUIRE(std::strlen(shot.localId) > 0u);
    OS_REQUIRE(shot.availableScreenshotFormats == XAppCaptureScreenshotFormatFlag_SDR);

    XAppCaptureScreenshotStreamHandle stream_h = nullptr;
    uint64_t total_bytes = 0;
    OS_REQUIRE(XAppCaptureOpenScreenshotStream(
                   shot.localId, XAppCaptureScreenshotFormatFlag_SDR, &stream_h, &total_bytes) ==
               XGR_S_OK);
    OS_REQUIRE(stream_h != nullptr && total_bytes > 32u);

    std::vector<uint8_t> png_buf(static_cast<size_t>(total_bytes), 0);
    uint32_t read_part1 = 0;
    uint32_t read_part2 = 0;
    OS_REQUIRE(XAppCaptureReadScreenshotStream(stream_h, 0, 16, png_buf.data(), &read_part1) ==
               XGR_S_OK);
    OS_REQUIRE(read_part1 == 16u);
    OS_REQUIRE(XAppCaptureReadScreenshotStream(
                   stream_h, 16, static_cast<uint32_t>(total_bytes), png_buf.data() + 16,
                   &read_part2) == XGR_S_OK);
    OS_REQUIRE(static_cast<uint64_t>(read_part1 + read_part2) == total_bytes);
    OS_REQUIRE(png_buf[0] == 0x89u && png_buf[1] == 'P' && png_buf[2] == 'N' && png_buf[3] == 'G');
    OS_REQUIRE(XAppCaptureCloseScreenshotStream(stream_h) == XGR_S_OK);

    // Diagnostic screenshot + diagnostic clip
    XAppCaptureDiagnosticScreenshotResult diag_shot{};
    OS_REQUIRE(XAppCaptureTakeDiagnosticScreenshot(
                   false, XAppCaptureScreenshotFormatFlag_SDR, "testdiag", &diag_shot) == XGR_S_OK);
    OS_REQUIRE(diag_shot.fileCount == 1u && diag_shot.files[0].fileSize > 32u);
    OS_REQUIRE(std::filesystem::exists(diag_shot.files[0].path));

    XAppCaptureRecordClipResult diag_clip{};
    OS_REQUIRE(XAppCaptureRecordDiagnosticClip(1700000000, 5000, "testclip", &diag_clip) ==
               XGR_S_OK);
    OS_REQUIRE(diag_clip.fileSize > 0u && diag_clip.durationInMs == 5000u);
    OS_REQUIRE(std::filesystem::exists(diag_clip.path));

    // 3. XSpeechSynthesizerCreateStreamFromSsml (1)
    XSpeechSynthesizerHandle synth = nullptr;
    OS_REQUIRE(XSpeechSynthesizerCreate(&synth) == XGR_S_OK && synth != nullptr);
    XSpeechSynthesizerStreamHandle ssml_stream = nullptr;
    const char* ssml =
        "<speak version='1.0' xml:lang='en-US'><prosody rate='medium'>Hello OpenScarlett</prosody></speak>";
    OS_REQUIRE(XSpeechSynthesizerCreateStreamFromSsml(synth, ssml, &ssml_stream) == XGR_S_OK);
    OS_REQUIRE(ssml_stream != nullptr);
    size_t wav_sz = 0;
    OS_REQUIRE(XSpeechSynthesizerGetStreamDataSize(ssml_stream, &wav_sz) == XGR_S_OK);
    OS_REQUIRE(wav_sz > 44u);
    std::vector<uint8_t> wav(wav_sz, 0);
    size_t wav_used = 0;
    OS_REQUIRE(XSpeechSynthesizerGetStreamData(ssml_stream, wav.size(), wav.data(), &wav_used) ==
               XGR_S_OK);
    OS_REQUIRE(wav_used == wav_sz);
    OS_REQUIRE(std::memcmp(wav.data(), "RIFF", 4) == 0 &&
               std::memcmp(wav.data() + 8, "WAVE", 4) == 0);
    OS_REQUIRE(XSpeechSynthesizerCloseStreamHandle(ssml_stream) == XGR_S_OK);
    OS_REQUIRE(XSpeechSynthesizerCloseHandle(synth) == XGR_S_OK);

    // 4. XSpeechToText* (6)
    OS_REQUIRE(XSpeechToTextSetPositionHint(XSpeechToTextPositionHint_TopCenter) == XGR_S_OK);
    OS_REQUIRE(XSpeechToTextSendString("Player1", "Ready!", XSpeechToTextType_Voice) == XGR_S_OK);
    uint32_t hyp1 = 0, hyp2 = 0;
    OS_REQUIRE(XSpeechToTextBeginHypothesisString(
                   "Player1", "Hel", XSpeechToTextType_Voice, &hyp1) == XGR_S_OK);
    OS_REQUIRE(hyp1 != 0u);
    OS_REQUIRE(XSpeechToTextUpdateHypothesisString(hyp1, "Hello") == XGR_S_OK);
    OS_REQUIRE(XSpeechToTextFinalizeHypothesisString(hyp1, "Hello world") == XGR_S_OK);
    OS_REQUIRE(XSpeechToTextUpdateHypothesisString(hyp1, "Late") == XGR_E_NOT_FOUND);

    OS_REQUIRE(XSpeechToTextBeginHypothesisString(
                   "Player2", "Wait", XSpeechToTextType_Text, &hyp2) == XGR_S_OK);
    OS_REQUIRE(hyp2 != hyp1);
    OS_REQUIRE(XSpeechToTextCancelHypothesisString(hyp2) == XGR_S_OK);
    OS_REQUIRE(XSpeechToTextCancelHypothesisString(hyp2) == XGR_E_NOT_FOUND);

    // Verify CallTelemetry logged new symbols with 0 NotImplemented
    uint64_t total_calls = 0, not_impl_calls = 0;
    OS_REQUIRE(XgrGetTelemetryCallCountForTest(
                   "XAppBroadcastGetStatus", &total_calls, &not_impl_calls) == 1);
    OS_REQUIRE(total_calls >= 1u && not_impl_calls == 0u);
    OS_REQUIRE(XgrGetTelemetryCallCountForTest(
                   "XSpeechSynthesizerCreateStreamFromSsml", &total_calls, &not_impl_calls) == 1);
    OS_REQUIRE(total_calls >= 1u && not_impl_calls == 0u);

    std::printf("[PASS] All 31 new XAppBroadcast/XAppCapture/XSpeech* exports verified\n");
}

bool OPENSCARLETT_GDK_CALL CaptureStoreProductCb(const XStoreProduct* prod, void* ctx) {
    auto* out = static_cast<std::vector<std::string>*>(ctx);
    if (prod && prod->storeId) {
        out->push_back(prod->storeId);
        OS_REQUIRE(prod->isInUserCollection);
        OS_REQUIRE(prod->hasDigitalDownload);
        OS_REQUIRE(prod->skusCount >= 1u && prod->skus != nullptr);
        OS_REQUIRE(prod->skus[0].availabilitiesCount >= 1u &&
                   prod->skus[0].availabilities != nullptr);
        OS_REQUIRE(XStoreIsAvailabilityPurchasable(prod->skus[0].availabilities[0]));
    }
    return true;
}

void TestHeadlessStoreAndPlsAndGameConfig() {
    // Write a synthetic MicrosoftGame.Config inside our temp dir on E:\ to test XML parsing
    std::filesystem::path cfg_dir =
        std::filesystem::temp_directory_path() / "openscarlett_xgr_cfg_test";
    std::error_code ec;
    std::filesystem::create_directories(cfg_dir, ec);
    std::filesystem::path cfg_file = cfg_dir / "MicrosoftGame.Config";
    {
        std::ofstream ofs(cfg_file, std::ios::binary);
        ofs << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            << "<Game configVersion=\"1\">\n"
            << "  <Identity Name=\"OpenScarlett.TestTitle\" Publisher=\"CN=Test\" Version=\"1.0.0.0\" />\n"
            << "  <ShellVisuals DefaultDisplayName=\"Synthetic Test Game\" />\n"
            << "  <!-- <StoreId>000000000000</StoreId> -->\n"
            << "  <StoreId>9SYNTH000042</StoreId>\n"
            << "  <TitleId>6BFFDF10</TitleId>\n"
            << "</Game>\n";
    }

    {
        ScopedEnvVar clear_tid("OPENSCARLETT_TITLE_ID", nullptr);
        ScopedEnvVar clear_sid("OPENSCARLETT_STORE_ID", nullptr);
        ScopedEnvVar set_cfg("OPENSCARLETT_GAME_CONFIG", cfg_file.string().c_str());

        uint32_t tid = 0;
        OS_REQUIRE(XGameGetXboxTitleId(&tid) == XGR_S_OK);
        OS_REQUIRE(tid == 0x6BFFDF10u);

        XStoreContextHandle sctx = nullptr;
        OS_REQUIRE(XStoreCreateContext(nullptr, &sctx) == XGR_S_OK && sctx != nullptr);

        // Headless PC-licensed game license
        XAsyncBlock lic_ab{};
        OS_REQUIRE(XStoreQueryGameLicenseAsync(sctx, &lic_ab) == XGR_S_OK);
        XStoreGameLicense lic{};
        OS_REQUIRE(XStoreQueryGameLicenseResult(&lic_ab, &lic) == XGR_S_OK);
        OS_REQUIRE(lic.isActive && !lic.isTrial && !lic.isDiscLicense);
        OS_REQUIRE(std::strcmp(lic.skuStoreId, "9SYNTH000042/0001") == 0);

        // Current game product reflects StoreId from MicrosoftGame.Config
        XAsyncBlock prod_ab{};
        OS_REQUIRE(XStoreQueryProductForCurrentGameAsync(sctx, &prod_ab) == XGR_S_OK);
        XStoreProductQueryHandle qh = nullptr;
        OS_REQUIRE(XStoreQueryProductForCurrentGameResult(&prod_ab, &qh) == XGR_S_OK &&
                   qh != nullptr);
        std::vector<std::string> ids;
        OS_REQUIRE(XStoreEnumerateProductsQuery(qh, &ids, &CaptureStoreProductCb) == XGR_S_OK);
        OS_REQUIRE(ids.size() == 1u && ids[0] == "9SYNTH000042");
        XStoreCloseProductsQueryHandle(qh);

        // Add-on licenses via OPENSCARLETT_STORE_ADDON_IDS
        {
            ScopedEnvVar set_addons("OPENSCARLETT_STORE_ADDON_IDS", "9ADDON000001, 9ADDON000002");
            XAsyncBlock add_ab{};
            OS_REQUIRE(XStoreQueryAddOnLicensesAsync(sctx, &add_ab) == XGR_S_OK);
            uint32_t cnt = 0;
            OS_REQUIRE(XStoreQueryAddOnLicensesResultCount(&add_ab, &cnt) == XGR_S_OK);
            OS_REQUIRE(cnt == 2u);
            XStoreAddonLicense addons[2]{};
            OS_REQUIRE(XStoreQueryAddOnLicensesResult(&add_ab, 2u, addons) == XGR_S_OK);
            OS_REQUIRE(addons[0].isActive &&
                       std::strcmp(addons[0].skuStoreId, "9ADDON000001/0001") == 0);
            OS_REQUIRE(addons[1].isActive &&
                       std::strcmp(addons[1].skuStoreId, "9ADDON000002/0001") == 0);
        }

        XStoreCloseContextHandle(sctx);
    }

    // Headless PLS space info + prompt user for space (non-blocking S_OK)
    XPersistentLocalStorageSpaceInfo space_info{};
    OS_REQUIRE(XPersistentLocalStorageGetSpaceInfo(&space_info) == XGR_S_OK);
    OS_REQUIRE(space_info.totalBytes > 0u && space_info.availableFreeBytes > 0u);

    XAsyncBlock pls_ab{};
    OS_REQUIRE(XPersistentLocalStoragePromptUserForSpaceAsync(64ULL * 1024ULL * 1024ULL, &pls_ab) ==
               XGR_S_OK);
    OS_REQUIRE(XPersistentLocalStoragePromptUserForSpaceResult(&pls_ab) == XGR_S_OK);

    std::filesystem::remove_all(cfg_dir, ec);
    std::printf("[PASS] Headless XStore, MicrosoftGame.Config, and XPersistentLocalStorage verified\n");
}

struct SerialOrderState {
    std::atomic<int> in_flight{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> done_count{0};
    std::mutex mtx;
    std::vector<int> order;
};

struct SerialItem {
    SerialOrderState* state;
    int seq;
};

void OPENSCARLETT_GDK_CALL SerialWorkCb(void* ctx, bool canceled) {
    auto* item = static_cast<SerialItem*>(ctx);
    OS_REQUIRE(!canceled);
    const int cur = item->state->in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
    int prev_max = item->state->max_concurrent.load(std::memory_order_acquire);
    while (cur > prev_max && !item->state->max_concurrent.compare_exchange_weak(prev_max, cur)) {
    }
    {
        std::lock_guard<std::mutex> lk(item->state->mtx);
        item->state->order.push_back(item->seq);
    }
    item->state->in_flight.fetch_sub(1, std::memory_order_acq_rel);
    item->state->done_count.fetch_add(1, std::memory_order_release);
}

struct CancelProviderCtx {
    std::atomic<int> cancel_calls{0};
    std::atomic<int> work_calls{0};
    std::atomic<int> cleanup_calls{0};
    uint32_t payload = 0xCAFEBABEu;
};

XGameResult OPENSCARLETT_GDK_CALL CancelTestProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<CancelProviderCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            ctx->work_calls.fetch_add(1, std::memory_order_acq_rel);
            XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (!data->buffer || data->bufferSize < sizeof(uint32_t)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, &ctx->payload, sizeof(uint32_t));
            return XGR_S_OK;
        case XAsyncOp_Cancel:
            ctx->cancel_calls.fetch_add(1, std::memory_order_acq_rel);
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            ctx->cleanup_calls.fetch_add(1, std::memory_order_acq_rel);
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct StressItem {
    XAsyncBlock ab{};
    uint32_t expected = 0;
    std::atomic<bool> cb_fired{false};
    std::atomic<uint32_t> got_val{0};
};

XGameResult OPENSCARLETT_GDK_CALL StressWorkFn(XAsyncBlock* ab) {
    auto* item = static_cast<StressItem*>(ab->context);
    item->got_val.store(item->expected * 3u + 7u, std::memory_order_release);
    return XGR_S_OK;
}

void OPENSCARLETT_GDK_CALL StressCompletionCb(XAsyncBlock* ab) {
    auto* item = static_cast<StressItem*>(ab->context);
    OS_REQUIRE(XAsyncGetStatus(ab, true) == XGR_S_OK);
    item->cb_fired.store(true, std::memory_order_release);
}

void OPENSCARLETT_GDK_CALL ManualDrainCancelCb(void* ctx, bool canceled) {
    auto* count = static_cast<std::atomic<int>*>(ctx);
    if (canceled) {
        count->fetch_add(1, std::memory_order_acq_rel);
    }
}

void OPENSCARLETT_GDK_CALL TerminatedMarkerCb(void* ctx) {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
}

void OPENSCARLETT_GDK_CALL InCallbackGetResultCb(XAsyncBlock* ab) {
    auto* out_val = static_cast<uint32_t*>(ab->context);
    OS_REQUIRE(XAsyncGetStatus(ab, true) == XGR_S_OK);
    size_t sz = 0;
    OS_REQUIRE(XAsyncGetResultSize(ab, &sz) == XGR_S_OK && sz == sizeof(uint32_t));
    size_t used = 0;
    OS_REQUIRE(XAsyncGetResult(ab, nullptr, sizeof(uint32_t), out_val, &used) == XGR_S_OK);
    OS_REQUIRE(used == sizeof(uint32_t));
}

void TestTaskQueueAndAsyncConformanceStress() {
    // 1. SerializedThreadPool strict FIFO & single-concurrency guarantee
    {
        XTaskQueueHandle sq = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_SerializedThreadPool,
                       XTaskQueueDispatchMode_Manual,
                       &sq) == XGR_S_OK);
        SerialOrderState state;
        constexpr int kCount = 150;
        std::vector<SerialItem> items(kCount);
        for (int i = 0; i < kCount; ++i) {
            items[i] = SerialItem{&state, i};
            OS_REQUIRE(XTaskQueueSubmitCallback(sq, XTaskQueuePort_Work, &items[i], &SerialWorkCb) ==
                       XGR_S_OK);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (state.done_count.load(std::memory_order_acquire) < kCount &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        OS_REQUIRE(state.done_count.load() == kCount);
        OS_REQUIRE(state.max_concurrent.load() == 1);
        OS_REQUIRE(state.order.size() == static_cast<size_t>(kCount));
        for (int i = 0; i < kCount; ++i) {
            OS_REQUIRE(state.order[static_cast<size_t>(i)] == i);
        }
        XTaskQueueCloseHandle(sq);
    }

    // 2. Manual queue + XAsyncCancel before work dispatch + double-cancel safety
    {
        XTaskQueueHandle mq = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_Manual, &mq) ==
                   XGR_S_OK);

        CancelProviderCtx pctx;
        std::atomic<bool> comp_fired{false};
        XAsyncBlock ab{};
        ab.queue = mq;
        ab.context = &comp_fired;
        ab.callback = [](XAsyncBlock* b) {
            static_cast<std::atomic<bool>*>(b->context)->store(true, std::memory_order_release);
        };
        OS_REQUIRE(XAsyncBegin(&ab, &pctx, &pctx, "CancelTest", &CancelTestProvider) == XGR_S_OK);
        OS_REQUIRE(XAsyncSchedule(&ab, 0) == XGR_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&ab, false) == XGR_E_PENDING);

        XAsyncCancel(&ab);
        XAsyncCancel(&ab);  // second cancel must be a safe no-op
        OS_REQUIRE(pctx.cancel_calls.load() == 1);

        OS_REQUIRE(XTaskQueueDispatch(mq, XTaskQueuePort_Work, 0));
        OS_REQUIRE(pctx.work_calls.load() == 0);  // canceled before DoWork ran
        OS_REQUIRE(XTaskQueueDispatch(mq, XTaskQueuePort_Completion, 0));
        OS_REQUIRE(comp_fired.load());
        OS_REQUIRE(XAsyncGetStatus(&ab, false) == XGR_E_ABORT);
        OS_REQUIRE(pctx.cleanup_calls.load() == 1);

        // Cancel after completion must NOT crash or invoke XAsyncOp_Cancel again
        XAsyncCancel(&ab);
        OS_REQUIRE(pctx.cancel_calls.load() == 1);

        // In-callback synchronous XAsyncGetResult without deadlocking
        CancelProviderCtx pctx2;
        uint32_t out_val = 0;
        XAsyncBlock ab2{};
        ab2.queue = mq;
        ab2.context = &out_val;
        ab2.callback = &InCallbackGetResultCb;
        OS_REQUIRE(XAsyncBegin(&ab2, &pctx2, &pctx2, "InCbTest", &CancelTestProvider) == XGR_S_OK);
        OS_REQUIRE(XAsyncSchedule(&ab2, 0) == XGR_S_OK);
        OS_REQUIRE(XTaskQueueDispatch(mq, XTaskQueuePort_Work, 0));
        OS_REQUIRE(XTaskQueueDispatch(mq, XTaskQueuePort_Completion, 0));
        OS_REQUIRE(out_val == 0xCAFEBABEu);
        OS_REQUIRE(pctx2.cleanup_calls.load() == 1);

        // Queue termination cancels pending manual items
        std::atomic<int> canceled_pending{0};
        for (int i = 0; i < 5; ++i) {
            OS_REQUIRE(XTaskQueueSubmitCallback(
                           mq, XTaskQueuePort_Work, &canceled_pending, &ManualDrainCancelCb) ==
                       XGR_S_OK);
        }
        std::atomic<bool> term_called{false};
        OS_REQUIRE(XTaskQueueTerminate(mq, true, &term_called, &TerminatedMarkerCb) == XGR_S_OK);
        OS_REQUIRE(term_called.load());
        OS_REQUIRE(canceled_pending.load() == 5);
        OS_REQUIRE(XTaskQueueSubmitCallback(
                       mq, XTaskQueuePort_Work, &canceled_pending, &ManualDrainCancelCb) ==
                   XGR_E_ABORT);

        XTaskQueueCloseHandle(mq);
    }

    // 3. ThreadPool high-concurrency XAsyncRun stress test (200 ops across 4 submitter threads)
    {
        XTaskQueueHandle tpq = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &tpq) ==
                   XGR_S_OK);

        constexpr uint32_t kOps = 200;
        std::vector<StressItem> items(kOps);
        for (uint32_t i = 0; i < kOps; ++i) {
            items[i].expected = i + 1u;
            items[i].ab.queue = tpq;
            items[i].ab.context = &items[i];
            items[i].ab.callback = &StressCompletionCb;
        }

        std::vector<std::thread> submitters;
        for (uint32_t t = 0; t < 4; ++t) {
            submitters.emplace_back([&, t]() {
                for (uint32_t i = t; i < kOps; i += 4) {
                    const XGameResult hr = XAsyncRun(&items[i].ab, &StressWorkFn);
                    OS_REQUIRE(hr == XGR_S_OK);
                }
            });
        }
        for (auto& th : submitters) {
            th.join();
        }
        for (uint32_t i = 0; i < kOps; ++i) {
            OS_REQUIRE(XAsyncGetStatus(&items[i].ab, true) == XGR_S_OK);
            OS_REQUIRE(items[i].cb_fired.load(std::memory_order_acquire));
            OS_REQUIRE(items[i].got_val.load(std::memory_order_acquire) ==
                       items[i].expected * 3u + 7u);
        }
        XTaskQueueCloseHandle(tpq);
    }

    std::printf("[PASS] XTaskQueue & XAsync multi-scenario conformance & 200-op stress suite\n");
}

void TestQueryApiImpl2ArgAnd3Arg() {
    OS_REQUIRE(InitializeApiImpl() == XGR_S_OK);
    OS_REQUIRE(InitializeApiImplEx(0x55F000002716ULL, 0x55F00000110AULL, 2ULL) == XGR_S_OK);

    // 1. Backwards-compatible 2-arg OpenScarlett internal GUIDs
    const XgrGuid kInternal2ArgGuids[] = {
        XGR_API_GUID_TASK_QUEUE,
        XGR_API_GUID_ASYNC,
        XGR_API_GUID_USER,
        XGR_API_GUID_GAME_SAVE,
        XGR_API_GUID_SYSTEM,
        XGR_API_GUID_MEM,
        XGR_API_GUID_PACKAGE,
        XGR_API_GUID_DISPLAY,
        XGR_API_GUID_GAME_UI,
        XGR_API_GUID_GAME_INVITE,
        XGR_API_GUID_ACCESSIBILITY,
        XGR_API_GUID_SPEECH_SYNTHESIZER,
        XGR_API_GUID_STORE,
        XGR_API_GUID_NETWORKING,
        XGR_API_GUID_CORE_RUNTIME,
    };
    for (const auto& g : kInternal2ArgGuids) {
        void* vt = nullptr;
        OS_REQUIRE(QueryApiImpl(&g, &vt) == XGR_S_OK && vt != nullptr);
    }

    // 2. All 24 real shipping XGamingRuntimeThunks.dll 3-arg (CLSID, IID, ppv) pairs
    struct Pair {
        XgrGuid clsid;
        XgrGuid iid;
    };
    static const Pair kReal24Pairs[] = {
        {{0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}},
         {0x1BF2F8C5u, 0xD507u, 0x4E52u, {0xBB, 0x05, 0xF7, 0x26, 0xD0, 0xE7, 0x11, 0x61}}},
        {{0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}},
         {0xCEF4FAC0u, 0x7676u, 0x4A94u, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}}},
        {{0x03F0FE74u, 0xFDD9u, 0x4E5Cu, {0xB6, 0x30, 0xF9, 0x33, 0x9C, 0x47, 0xAC, 0xC5}},
         {0x35F07670u, 0x706Eu, 0x4BFBu, {0x94, 0x76, 0x09, 0x07, 0x98, 0xC5, 0xEB, 0xF3}}},
        {{0x0651AAE2u, 0x4012u, 0x4077u, {0xBF, 0x84, 0x8B, 0x90, 0x97, 0x09, 0x0E, 0x2C}},
         {0x0651AAE2u, 0x4012u, 0x4077u, {0xBF, 0x84, 0x8B, 0x90, 0x97, 0x09, 0x0E, 0x2C}}},
        {{0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}},
         {0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}}},
        {{0x0DD112ACu, 0x7C24u, 0x448Cu, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}},
         {0xDE3DBDD4u, 0x0B37u, 0x4BDBu, {0xA1, 0x0E, 0xAC, 0xF3, 0xA3, 0x54, 0xD0, 0x6A}}},
        {{0x0DD112ACu, 0x7C24u, 0x448Cu, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}},
         {0x60B09F4Eu, 0x1B85u, 0x45B1u, {0x82, 0x6C, 0x16, 0x91, 0x18, 0xE2, 0x30, 0xE1}}},
        {{0x0DD112ACu, 0x7C24u, 0x448Cu, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}},
         {0x2D42FEA5u, 0xE71Du, 0x4B76u, {0x97, 0xCD, 0xC5, 0x0A, 0xFB, 0xB3, 0xAE, 0x5D}}},
        {{0x186D5592u, 0xA72Du, 0x45FBu, {0x95, 0x60, 0x11, 0xAE, 0xD0, 0xE6, 0x64, 0x7A}},
         {0x186D5592u, 0xA72Du, 0x45FBu, {0x95, 0x60, 0x11, 0xAE, 0xD0, 0xE6, 0x64, 0x7A}}},
        {{0x1B339674u, 0x328Du, 0x4283u, {0xA2, 0x00, 0x31, 0x71, 0xF1, 0x8D, 0x36, 0x39}},
         {0x1B339674u, 0x328Du, 0x4283u, {0xA2, 0x00, 0x31, 0x71, 0xF1, 0x8D, 0x36, 0x39}}},
        {{0x37E56907u, 0x2F10u, 0x41E8u, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}},
         {0x37E56907u, 0x2F10u, 0x41E8u, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}}},
        {{0x3E241977u, 0x6237u, 0x41E9u, {0x85, 0x59, 0x10, 0x2C, 0x2D, 0x99, 0x83, 0xF1}},
         {0xD722B373u, 0x8C4Du, 0x4692u, {0x9E, 0x51, 0xD6, 0xAD, 0x9B, 0x37, 0xAA, 0x7D}}},
        {{0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}},
         {0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6E, 0xE2}}},
        {{0x7D824997u, 0x10DCu, 0x45ABu, {0x86, 0xB7, 0x27, 0x37, 0x76, 0x7C, 0x0B, 0xF1}},
         {0x0CC6A956u, 0xE7E1u, 0x4FDFu, {0x93, 0x41, 0x9D, 0x5D, 0xA6, 0x49, 0xEB, 0xC8}}},
        {{0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8D, 0xAD, 0x05, 0xF0, 0xD2, 0xCD, 0x5B, 0x40}},
         {0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8D, 0xAD, 0x05, 0xF0, 0xD2, 0xCD, 0x5B, 0x40}}},
        {{0x8CA467F7u, 0x22E8u, 0x4096u, {0x84, 0x56, 0xBB, 0x8A, 0xA1, 0x3F, 0x79, 0xD8}},
         {0x8CA467F7u, 0x22E8u, 0x4096u, {0x84, 0x56, 0xBB, 0x8A, 0xA1, 0x3F, 0x79, 0xD8}}},
        {{0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}},
         {0x2549F142u, 0x6419u, 0x4A06u, {0x97, 0xB5, 0x93, 0x1A, 0xAB, 0x7C, 0x2F, 0x34}}},
        {{0xA4F1AEE2u, 0x4BF1u, 0x4485u, {0xB0, 0x08, 0xA7, 0xC2, 0x6D, 0x52, 0xAC, 0x27}},
         {0x3A949778u, 0x772Eu, 0x4799u, {0xBD, 0xEA, 0x0A, 0x66, 0x39, 0xE9, 0x6B, 0xAA}}},
        {{0xAF406016u, 0xE850u, 0x4AA8u, {0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E}},
         {0xF92D8712u, 0x2B27u, 0x4D8Au, {0xBF, 0x01, 0x11, 0xA6, 0xF8, 0xE3, 0xEB, 0x42}}},
        {{0xB884675Du, 0xB738u, 0x4A9Cu, {0x81, 0x5D, 0x9A, 0x9A, 0x1E, 0x0C, 0x6C, 0x9B}},
         {0xB884675Du, 0xB738u, 0x4A9Cu, {0x81, 0x5D, 0x9A, 0x9A, 0x1E, 0x0C, 0x6C, 0x9B}}},
        {{0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5, 0xCA, 0xED, 0xF0, 0x54, 0x96, 0x0B, 0x4D}},
         {0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5, 0xCA, 0xED, 0xF0, 0x54, 0x96, 0x0B, 0x4D}}},
        {{0xDFCD4649u, 0x4FF8u, 0x4043u, {0xBA, 0x07, 0x35, 0xD6, 0x07, 0xDF, 0x98, 0xB0}},
         {0x72C4E70Fu, 0x4A67u, 0x481Bu, {0xBB, 0x84, 0xAA, 0x72, 0xED, 0xA1, 0x4F, 0xE5}}},
        {{0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}},
         {0x67CE4BFCu, 0xB1D1u, 0x4AC7u, {0xBC, 0x3A, 0xCB, 0x92, 0x19, 0xA9, 0x7A, 0x85}}},
        {{0xF4FAF4D4u, 0x2D04u, 0x4FCEu, {0xB3, 0xE0, 0x47, 0x4A, 0x71, 0x3A, 0x3E, 0x84}},
         {0xD29411DFu, 0x0794u, 0x4553u, {0x8B, 0x27, 0x95, 0xFC, 0x02, 0xD0, 0xF7, 0x5D}}},
    };

    struct ComBase {
        void** lpVtbl;
    };
    using FnQi = XGameResult(OPENSCARLETT_GDK_CALL*)(void*, const XgrGuid*, void**);
    using FnAddRef = uint32_t(OPENSCARLETT_GDK_CALL*)(void*);
    using FnRelease = uint32_t(OPENSCARLETT_GDK_CALL*)(void*);

    for (const auto& p : kReal24Pairs) {
        void* iface = nullptr;
        const XGameResult hr = QueryApiImpl(
            &p.clsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&p.iid)), &iface);
        OS_REQUIRE(hr == XGR_S_OK && iface != nullptr);
        if (iface) {
            auto* obj = static_cast<ComBase*>(iface);
            OS_REQUIRE(obj->lpVtbl != nullptr);
            void* unk = nullptr;
            auto qi = reinterpret_cast<FnQi>(obj->lpVtbl[0]);
            auto addref = reinterpret_cast<FnAddRef>(obj->lpVtbl[1]);
            auto rel = reinterpret_cast<FnRelease>(obj->lpVtbl[2]);
            OS_REQUIRE(qi(iface, &XGR_IID_IUNKNOWN, &unk) == XGR_S_OK && unk == iface);
            (void)addref(iface);
            (void)rel(iface);
            (void)rel(iface);
            (void)rel(iface);
        }
    }

    // Unknown 3-arg CLSID/IID must cleanly return XGR_E_NOINTERFACE and null ppv
    static const XgrGuid kUnkClsid = {
        0x11223344u, 0x5566u, 0x7788u, {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01}};
    static const XgrGuid kUnkIid = {
        0x11223344u, 0x5566u, 0x7788u, {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x02}};
    void* unk_out = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234u));
    OS_REQUIRE(QueryApiImpl(&kUnkClsid, reinterpret_cast<void**>(const_cast<XgrGuid*>(&kUnkIid)),
                            &unk_out) == XGR_E_NOINTERFACE);
    OS_REQUIRE(unk_out == nullptr);

    UninitializeApiImpl();
    UninitializeApiImpl();
    std::printf("[PASS] 2-arg (15 GUIDs) + 3-arg (24 real COM CLSID/IIDs) QueryApiImpl verified\n");
}

void TestLiveShippingXGamingRuntimeThunks() {
#if defined(_WIN32)
    const wchar_t* kThunksDllPath =
        L"E:\\Program Files\\Alexandria IV DEMO\\Content\\Alexandria IV DEMO_Data\\Plugins\\x86_64\\XGamingRuntimeThunks.dll";
    const char* kAlexConfigPath =
        "E:\\Program Files\\Alexandria IV DEMO\\Content\\MicrosoftGame.Config";

    if (!std::filesystem::exists(kThunksDllPath)) {
        std::printf("[SKIP] Real shipping XGamingRuntimeThunks.dll not found locally\n");
        return;
    }

    ScopedEnvVar clear_tid("OPENSCARLETT_TITLE_ID", nullptr);
    ScopedEnvVar clear_sid("OPENSCARLETT_STORE_ID", nullptr);
    ScopedEnvVar set_cfg("OPENSCARLETT_GAME_CONFIG", kAlexConfigPath);

    // Ensure our built xgameruntime.dll is already loaded in this process before loading Thunks.
    OS_REQUIRE(::GetModuleHandleW(L"xgameruntime.dll") != nullptr);

    HMODULE h_thunks = ::LoadLibraryW(kThunksDllPath);
    OS_REQUIRE(h_thunks != nullptr);
    if (!h_thunks) return;

#define LOAD_THUNK(fn) auto p_##fn = reinterpret_cast<decltype(&fn)>(::GetProcAddress(h_thunks, #fn)); OS_REQUIRE(p_##fn != nullptr)

    LOAD_THUNK(XGameRuntimeInitialize);
    LOAD_THUNK(XGameRuntimeUninitialize);
    LOAD_THUNK(XGameRuntimeIsFeatureAvailable);
    LOAD_THUNK(XGameGetXboxTitleId);
    LOAD_THUNK(XTaskQueueCreate);
    LOAD_THUNK(XTaskQueueSubmitCallback);
    LOAD_THUNK(XTaskQueueDispatch);
    LOAD_THUNK(XTaskQueueCloseHandle);
    LOAD_THUNK(XAsyncRun);
    LOAD_THUNK(XAsyncGetStatus);
    LOAD_THUNK(XUserAddAsync);
    LOAD_THUNK(XUserAddResult);
    LOAD_THUNK(XUserGetId);
    LOAD_THUNK(XUserGetLocalId);
    LOAD_THUNK(XUserGetGamertag);
    LOAD_THUNK(XUserGetState);
    LOAD_THUNK(XUserCheckPrivilege);
    LOAD_THUNK(XUserCloseHandle);
    LOAD_THUNK(XGameSaveInitializeProvider);
    LOAD_THUNK(XGameSaveCreateContainer);
    LOAD_THUNK(XGameSaveCreateUpdate);
    LOAD_THUNK(XGameSaveSubmitBlobWrite);
    LOAD_THUNK(XGameSaveSubmitUpdate);
    LOAD_THUNK(XGameSaveCloseUpdate);
    LOAD_THUNK(XGameSaveCloseContainer);
    LOAD_THUNK(XGameSaveCloseProvider);
    LOAD_THUNK(XStoreCreateContext);
    LOAD_THUNK(XStoreQueryGameLicenseAsync);
    LOAD_THUNK(XStoreQueryGameLicenseResult);
    LOAD_THUNK(XStoreQueryProductForCurrentGameAsync);
    LOAD_THUNK(XStoreQueryProductForCurrentGameResult);
    LOAD_THUNK(XStoreEnumerateProductsQuery);
    LOAD_THUNK(XStoreCloseProductsQueryHandle);
    LOAD_THUNK(XStoreIsAvailabilityPurchasable);
    LOAD_THUNK(XStoreAcquireLicenseForDurablesAsync);
    LOAD_THUNK(XStoreAcquireLicenseForDurablesResult);
    LOAD_THUNK(XStoreIsLicenseValid);
    LOAD_THUNK(XStoreCloseLicenseHandle);
    LOAD_THUNK(XStoreCloseContextHandle);
    LOAD_THUNK(XPackageGetCurrentProcessPackageIdentifier);
    LOAD_THUNK(XPackageIsPackagedProcess);
    LOAD_THUNK(XPackageGetUserLocale);
    LOAD_THUNK(XDisplayAcquireTimeoutDeferral);
    LOAD_THUNK(XDisplayCloseTimeoutDeferralHandle);
    LOAD_THUNK(XDisplayTryEnableHdrMode);
    LOAD_THUNK(XSystemGetAnalyticsInfo);
    LOAD_THUNK(XSystemGetConsoleId);
    LOAD_THUNK(XSystemGetXboxLiveSandboxId);
    LOAD_THUNK(XNetworkingGetConnectivityHint);
    LOAD_THUNK(XNetworkingQueryPreferredLocalUdpMultiplayerPort);
    LOAD_THUNK(XPersistentLocalStorageGetPathSize);
    LOAD_THUNK(XPersistentLocalStorageGetPath);
    LOAD_THUNK(XGameUiSetNotificationPositionHint);
    LOAD_THUNK(XGameUiShowMessageDialogAsync);
    LOAD_THUNK(XGameUiShowMessageDialogResult);
    LOAD_THUNK(XAppBroadcastGetStatus);
    LOAD_THUNK(XAppBroadcastIsAppBroadcasting);
    LOAD_THUNK(XAppCaptureMetadataAddDoubleEvent);
    LOAD_THUNK(XAppCaptureTakeScreenshot);
    LOAD_THUNK(XAppCaptureOpenScreenshotStream);
    LOAD_THUNK(XAppCaptureReadScreenshotStream);
    LOAD_THUNK(XAppCaptureCloseScreenshotStream);
    LOAD_THUNK(XSpeechSynthesizerCreate);
    LOAD_THUNK(XSpeechSynthesizerCreateStreamFromSsml);
    LOAD_THUNK(XSpeechSynthesizerCloseStreamHandle);
    LOAD_THUNK(XSpeechSynthesizerCloseHandle);
    LOAD_THUNK(XSpeechToTextBeginHypothesisString);
    LOAD_THUNK(XSpeechToTextFinalizeHypothesisString);
    LOAD_THUNK(XGameInviteRegisterForEvent);
    LOAD_THUNK(XGameInviteUnregisterForEvent);
    LOAD_THUNK(XErrorReport);
    LOAD_THUNK(XGameEventWrite);

#undef LOAD_THUNK

    // 1. Lifecycle & Alexandria IV DEMO TitleId (0x6BFFDF10)
    uint64_t qapi_before = 0, qapi_notimpl_before = 0;
    (void)XgrGetTelemetryCallCountForTest("QueryApiImpl", &qapi_before, &qapi_notimpl_before);

    OS_REQUIRE(p_XGameRuntimeInitialize() == XGR_S_OK);
    OS_REQUIRE(p_XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XStore));
    uint32_t alex_tid = 0;
    OS_REQUIRE(p_XGameGetXboxTitleId(&alex_tid) == XGR_S_OK);
    OS_REQUIRE(alex_tid == 0x6BFFDF10u);

    // 2. XTaskQueue & XAsync through real shipping thunks
    XTaskQueueHandle q = nullptr;
    OS_REQUIRE(p_XTaskQueueCreate(
                   XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_Manual, &q) == XGR_S_OK);
    std::atomic<int> thunk_cb_ran{0};
    OS_REQUIRE(p_XTaskQueueSubmitCallback(
                   q, XTaskQueuePort_Work, &thunk_cb_ran,
                   [](void* c, bool canceled) {
                       if (!canceled) static_cast<std::atomic<int>*>(c)->fetch_add(1);
                   }) == XGR_S_OK);
    OS_REQUIRE(p_XTaskQueueDispatch(q, XTaskQueuePort_Work, 0));
    OS_REQUIRE(thunk_cb_ran.load() == 1);

    XAsyncBlock run_ab{};
    run_ab.queue = q;
    OS_REQUIRE(p_XAsyncRun(&run_ab, [](XAsyncBlock*) -> XGameResult { return XGR_S_OK; }) ==
               XGR_S_OK);
    OS_REQUIRE(p_XTaskQueueDispatch(q, XTaskQueuePort_Work, 0));
    OS_REQUIRE(p_XAsyncGetStatus(&run_ab, false) == XGR_S_OK);
    p_XTaskQueueCloseHandle(q);

    // 3. XUser (both IIDs: main + gamertag)
    XAsyncBlock uab{};
    OS_REQUIRE(p_XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &uab) == XGR_S_OK);
    XUserHandle user = nullptr;
    OS_REQUIRE(p_XUserAddResult(&uab, &user) == XGR_S_OK && user != nullptr);
    uint64_t xuid = 0;
    OS_REQUIRE(p_XUserGetId(user, &xuid) == XGR_S_OK && xuid != 0u);
    XUserLocalId lid{};
    OS_REQUIRE(p_XUserGetLocalId(user, &lid) == XGR_S_OK && lid.value != 0u);
    char gt[64]{};
    size_t gt_used = 0;
    OS_REQUIRE(p_XUserGetGamertag(
                   user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) == XGR_S_OK);
    OS_REQUIRE(gt_used > 1u);
    XUserState ust = XUserState_SignedOut;
    OS_REQUIRE(p_XUserGetState(user, &ust) == XGR_S_OK && ust == XUserState_SignedIn);
    bool has_priv = false;
    XUserPrivilegeDenyReason deny = XUserPrivilegeDenyReason_Unknown;
    OS_REQUIRE(p_XUserCheckPrivilege(
                   user, XUserPrivilegeOptions_None, XUserPrivilege_Multiplayer, &has_priv,
                   &deny) == XGR_S_OK);
    OS_REQUIRE(has_priv && deny == XUserPrivilegeDenyReason_None);

    // 4. XGameSave
    XGameSaveProviderHandle gsp = nullptr;
    OS_REQUIRE(p_XGameSaveInitializeProvider(
                   user, "00000000-0000-0000-0000-00006bffdf10", false, &gsp) == XGR_S_OK);
    XGameSaveContainerHandle gsc = nullptr;
    OS_REQUIRE(p_XGameSaveCreateContainer(gsp, "thunk_live_slot", &gsc) == XGR_S_OK);
    XGameSaveUpdateHandle gsu = nullptr;
    OS_REQUIRE(p_XGameSaveCreateUpdate(gsc, "Live Slot", &gsu) == XGR_S_OK);
    const uint8_t blob_bytes[4] = {1, 2, 3, 4};
    OS_REQUIRE(p_XGameSaveSubmitBlobWrite(gsu, "save.bin", blob_bytes, sizeof(blob_bytes)) ==
               XGR_S_OK);
    OS_REQUIRE(p_XGameSaveSubmitUpdate(gsu) == XGR_S_OK);
    p_XGameSaveCloseUpdate(gsu);
    p_XGameSaveCloseContainer(gsc);
    p_XGameSaveCloseProvider(gsp);

    // 5. XStore (all 3 IIDs: main + IsAvailabilityPurchasable + AcquireLicenseForDurables)
    XStoreContextHandle sctx = nullptr;
    OS_REQUIRE(p_XStoreCreateContext(user, &sctx) == XGR_S_OK && sctx != nullptr);
    XAsyncBlock slab{};
    OS_REQUIRE(p_XStoreQueryGameLicenseAsync(sctx, &slab) == XGR_S_OK);
    XStoreGameLicense slic{};
    OS_REQUIRE(p_XStoreQueryGameLicenseResult(&slab, &slic) == XGR_S_OK);
    OS_REQUIRE(slic.isActive && !slic.isTrial);
    OS_REQUIRE(std::strcmp(slic.skuStoreId, "9ND00FD7RLN1/0001") == 0);

    XAsyncBlock spab{};
    OS_REQUIRE(p_XStoreQueryProductForCurrentGameAsync(sctx, &spab) == XGR_S_OK);
    XStoreProductQueryHandle sqh = nullptr;
    OS_REQUIRE(p_XStoreQueryProductForCurrentGameResult(&spab, &sqh) == XGR_S_OK && sqh != nullptr);
    std::vector<std::string> sids;
    OS_REQUIRE(p_XStoreEnumerateProductsQuery(sqh, &sids, &CaptureStoreProductCb) == XGR_S_OK);
    OS_REQUIRE(sids.size() == 1u && sids[0] == "9ND00FD7RLN1");
    p_XStoreCloseProductsQueryHandle(sqh);

    XStoreAvailability savail{};
    savail.availabilityId = "AVAIL001";
    OS_REQUIRE(p_XStoreIsAvailabilityPurchasable(savail));

    XAsyncBlock sdab{};
    OS_REQUIRE(p_XStoreAcquireLicenseForDurablesAsync(sctx, "9ND00FD7RLN1", &sdab) == XGR_S_OK);
    XStoreLicenseHandle dlic = nullptr;
    OS_REQUIRE(p_XStoreAcquireLicenseForDurablesResult(&sdab, &dlic) == XGR_S_OK &&
               dlic != nullptr);
    OS_REQUIRE(p_XStoreIsLicenseValid(dlic));
    p_XStoreCloseLicenseHandle(dlic);
    p_XStoreCloseContextHandle(sctx);

    // 6. XPackage, XDisplay, XSystem (including 144-byte XSystemGetAnalyticsInfo!), XNetworking, PLS
    char pkg_id[128]{};
    OS_REQUIRE(p_XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id) == XGR_S_OK);
    (void)p_XPackageIsPackagedProcess();
    char locale[32]{};
    OS_REQUIRE(p_XPackageGetUserLocale(sizeof(locale), locale) == XGR_S_OK &&
               std::strlen(locale) > 0u);

    XDisplayTimeoutDeferralHandle ddef = nullptr;
    OS_REQUIRE(p_XDisplayAcquireTimeoutDeferral(&ddef) == XGR_S_OK && ddef != nullptr);
    p_XDisplayCloseTimeoutDeferralHandle(ddef);
    XDisplayHdrModeInfo hdr_info{};
    (void)p_XDisplayTryEnableHdrMode(XDisplayHdrModePreference_PreferHdr, &hdr_info);

    const XSystemAnalyticsInfo ainfo = p_XSystemGetAnalyticsInfo();
    OS_REQUIRE(std::strlen(ainfo.family) > 0u && std::strlen(ainfo.form) > 0u);
    char cid[64]{};
    size_t cid_used = 0;
    OS_REQUIRE(p_XSystemGetConsoleId(sizeof(cid), cid, &cid_used) == XGR_S_OK && cid_used > 1u);
    char sbid[64]{};
    size_t sbid_used = 0;
    OS_REQUIRE(p_XSystemGetXboxLiveSandboxId(sizeof(sbid), sbid, &sbid_used) == XGR_S_OK &&
               sbid_used > 1u);

    XNetworkingConnectivityHint nhint{};
    OS_REQUIRE(p_XNetworkingGetConnectivityHint(&nhint) == XGR_S_OK);
    uint16_t udp_port = 0;
    OS_REQUIRE(p_XNetworkingQueryPreferredLocalUdpMultiplayerPort(&udp_port) == XGR_S_OK &&
               udp_port > 0u);

    size_t pls_sz = 0;
    OS_REQUIRE(p_XPersistentLocalStorageGetPathSize(&pls_sz) == XGR_S_OK && pls_sz > 1u);
    std::string pls_path(pls_sz, '\0');
    size_t pls_used = 0;
    OS_REQUIRE(p_XPersistentLocalStorageGetPath(pls_path.size(), pls_path.data(), &pls_used) ==
               XGR_S_OK);

    // 7. XGameUi, XAppBroadcast, XAppCapture (incl. double XMM2 arg!), XSpeech, XGameInvite, XError, XGameEvent
    OS_REQUIRE(p_XGameUiSetNotificationPositionHint(XGameUiNotificationPositionHint_TopRight) ==
               XGR_S_OK);
    XAsyncBlock uiab{};
    OS_REQUIRE(p_XGameUiShowMessageDialogAsync(
                   &uiab, "Title", "Body", "OK", nullptr, nullptr,
                   XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_First) == XGR_S_OK);
    XGameUiMessageDialogButton uibtn = XGameUiMessageDialogButton_Third;
    OS_REQUIRE(p_XGameUiShowMessageDialogResult(&uiab, &uibtn) == XGR_S_OK &&
               uibtn == XGameUiMessageDialogButton_First);

    XAppBroadcastStatus tbstat{};
    OS_REQUIRE(p_XAppBroadcastGetStatus(user, &tbstat) == XGR_S_OK);
    OS_REQUIRE(!p_XAppBroadcastIsAppBroadcasting());
    OS_REQUIRE(p_XAppCaptureMetadataAddDoubleEvent(
                   "Fps", 60.0, XAppCaptureMetadataPriority_Informational) == XGR_S_OK);
    XAppCaptureTakeScreenshotResult tshot{};
    OS_REQUIRE(p_XAppCaptureTakeScreenshot(user, &tshot) == XGR_S_OK);
    XAppCaptureScreenshotStreamHandle tsh = nullptr;
    uint64_t tsz = 0;
    OS_REQUIRE(p_XAppCaptureOpenScreenshotStream(
                   tshot.localId, XAppCaptureScreenshotFormatFlag_SDR, &tsh, &tsz) == XGR_S_OK);
    uint8_t sig[4]{};
    uint32_t sw = 0;
    OS_REQUIRE(p_XAppCaptureReadScreenshotStream(tsh, 0, 4, sig, &sw) == XGR_S_OK && sw == 4u &&
               sig[0] == 0x89u && sig[1] == 'P');
    OS_REQUIRE(p_XAppCaptureCloseScreenshotStream(tsh) == XGR_S_OK);

    XSpeechSynthesizerHandle tsynth = nullptr;
    OS_REQUIRE(p_XSpeechSynthesizerCreate(&tsynth) == XGR_S_OK);
    XSpeechSynthesizerStreamHandle tssml = nullptr;
    OS_REQUIRE(p_XSpeechSynthesizerCreateStreamFromSsml(
                   tsynth, "<speak>Live thunk test</speak>", &tssml) == XGR_S_OK);
    OS_REQUIRE(p_XSpeechSynthesizerCloseStreamHandle(tssml) == XGR_S_OK);
    OS_REQUIRE(p_XSpeechSynthesizerCloseHandle(tsynth) == XGR_S_OK);

    uint32_t thyp = 0;
    OS_REQUIRE(p_XSpeechToTextBeginHypothesisString(
                   "Alex", "Hi", XSpeechToTextType_Voice, &thyp) == XGR_S_OK);
    OS_REQUIRE(p_XSpeechToTextFinalizeHypothesisString(thyp, "Hi there") == XGR_S_OK);

    XTaskQueueRegistrationToken itok{};
    OS_REQUIRE(p_XGameInviteRegisterForEvent(
                   nullptr, nullptr, [](void*, const char*) {}, &itok) == XGR_S_OK);
    OS_REQUIRE(p_XGameInviteUnregisterForEvent(itok, true));
    p_XErrorReport(XGR_S_OK, "live thunk error report check");
    OS_REQUIRE(p_XGameEventWrite(0, nullptr, 0, nullptr) == 0u);

    p_XUserCloseHandle(user);
    p_XGameRuntimeUninitialize();
    ::FreeLibrary(h_thunks);

    uint64_t qapi_after = 0, qapi_notimpl_after = 0;
    OS_REQUIRE(XgrGetTelemetryCallCountForTest("QueryApiImpl", &qapi_after, &qapi_notimpl_after) == 1);
    OS_REQUIRE(qapi_after > qapi_before + 30u);
    OS_REQUIRE(qapi_notimpl_after == qapi_notimpl_before);

    std::printf(
        "[PASS] LIVE shipping XGamingRuntimeThunks.dll E2E (Alexandria IV DEMO TitleId=0x%08X, StoreId=9ND00FD7RLN1, %llu 3-arg QueryApiImpl calls, ZERO E_NOINTERFACE!)\n",
        alex_tid,
        static_cast<unsigned long long>(qapi_after - qapi_before));
#endif
}

}  // namespace

int main() {
    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    TestExportParity292();
    TestNew31Exports();
    TestHeadlessStoreAndPlsAndGameConfig();
    TestTaskQueueAndAsyncConformanceStress();
    TestQueryApiImpl2ArgAnd3Arg();
    TestLiveShippingXGamingRuntimeThunks();

    XGameRuntimeUninitialize();

    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("=== ALL xgameruntime 100%% parity & 3-arg QueryApiImpl conformance tests PASSED ===\n");
    return 0;
}
