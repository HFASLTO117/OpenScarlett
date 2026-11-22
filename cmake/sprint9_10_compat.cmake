# OpenScarlett Sprint 9 + Sprint 10 (v0.5.0-alpha) Targets
# Full Compatibility Engine Architecture:
# - d3d12_xs 10 COM proxies, Xbox extensions & multi-queue thread safety
# - 64-bit Title-VA Unified Memory Mirror & RDNA2 V#/T#/S# Bindless SGPR Bridge
# - GPU Compute GFX10 Untiler/Retiler, HS/DS/GS, DXR SBT, Wave64-on-32 & Async PSO Pool
# - 7-Core Affinity Remap, WinRT Lifecycle & Non-Blocking Offline Auth
# - MFMediaEngine Video Decode, 64-Voice XAudio2 Effects, Bink2 Streaming & GameInput
# - First-Boot Probe VEH Crash Dumper, Per-Draw Frame Capture & Launcher UI

target_sources(openscarlett_core PRIVATE
    src/kernel/title_va_mirror.cpp
    src/gpu/descriptor_sgpr_bridge.cpp
    src/gpu/xg_gpu_untile.cpp
    src/gpu/rdna2_stages_ext.cpp
    src/loader/probe_veh.cpp
    src/gpu/frame_capture.cpp
)

if(TARGET d3d12_xs)
    target_sources(d3d12_xs PRIVATE
        src/gpu/d3d12_xs/proxies_x.cpp
    )
endif()

openscarlett_add_test(test_d3d12_xs_proxies_multiqueue tests/test_d3d12_xs_proxies_multiqueue.cpp)
target_link_libraries(test_d3d12_xs_proxies_multiqueue PRIVATE d3d12_xs d3d12 dxgi d3dcompiler)

openscarlett_add_test(test_unified_mem_bindless tests/test_unified_mem_bindless.cpp)
target_link_libraries(test_unified_mem_bindless PRIVATE openscarlett_core d3d12 dxgi d3dcompiler ole32 uuid)

openscarlett_add_test(test_xg_gpu_untile_and_stages tests/test_xg_gpu_untile_and_stages.cpp)
target_link_libraries(test_xg_gpu_untile_and_stages PRIVATE openscarlett_core d3d12 dxgi d3dcompiler ole32 uuid)

openscarlett_add_test(test_os_winrt_offline_auth tests/test_os_winrt_offline_auth.cpp)
target_link_libraries(test_os_winrt_offline_auth PRIVATE
    kernelx
    winrt_xs
    xgameruntime
    Microsoft.Xbox.Services.GDK.C
    XCurl
)

openscarlett_add_test(test_media_video_gameinput tests/test_media_video_gameinput.cpp)
target_link_libraries(test_media_video_gameinput PRIVATE
    XAudio2_9
    GameInput
)
add_dependencies(test_media_video_gameinput
    mfplat_xs
    Bink2Scarlett
)

openscarlett_add_test(test_probe_veh_capture tests/test_probe_veh_capture.cpp)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_d3d12_xs_proxies_multiqueue)
    _openscarlett_apply_portability_to_target(test_unified_mem_bindless)
    _openscarlett_apply_portability_to_target(test_xg_gpu_untile_and_stages)
    _openscarlett_apply_portability_to_target(test_os_winrt_offline_auth)
    _openscarlett_apply_portability_to_target(test_media_video_gameinput)
    _openscarlett_apply_portability_to_target(test_probe_veh_capture)
endif()
