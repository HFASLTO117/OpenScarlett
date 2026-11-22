# OpenScarlett Sprint 14 P0/P1 Shims, FFX API 2.0, Smart Middleware Routing & Config Tests
openscarlett_add_test(test_s14_p0_shims_ffx_config tests/test_s14_p0_shims_ffx_config.cpp)
target_link_libraries(test_s14_p0_shims_ffx_config PRIVATE
    openscarlett_core
    xgameruntime
    XAudio2_9
    kernelx
    XCurl
    libHttpClient.GDK
    amd_fidelityfx_loader_xbox
    amd_fidelityfx_upscaler_xbox
)
add_dependencies(test_s14_p0_shims_ffx_config xmem)
set_tests_properties(test_s14_p0_shims_ffx_config PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)
