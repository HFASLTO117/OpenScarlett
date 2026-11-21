# OpenScarlett Wave 4 — Agent F: SimpleLighting Multi-Frame 3D Blinn-Phong + XDisplay + X3DAudio/FX + Vulkan 1.4 Parity
# Owned by wave2/gdk-simple-lighting worktree.

# Standalone Xbox-ABI Guest Executable:
# Deliberately does NOT link openscarlett_core; imports ONLY Xbox/GDK shim DLLs.
openscarlett_add_gdk_guest_exe(gdk_simple_lighting tests/gdk_samples/gdk_simple_lighting.cpp)
target_link_libraries(gdk_simple_lighting PRIVATE
    d3d12_xs
    xgameruntime
    XAudio2_9
    pixevt
)

# Portable End-to-End Test Harness (launches zero-DLL package dir via openscarlett.exe run
# on D3D12 HW, D3D12 WARP, and --backend=vulkan, plus live Vulkan 1.4 Blinn-Phong parity)
openscarlett_add_test(test_gdk_simple_lighting tests/test_gdk_simple_lighting.cpp)
add_dependencies(test_gdk_simple_lighting
    openscarlett
    openscarlett_hook
    d3d12_xs
    xgameruntime
    XAudio2_9
    pixevt
    gdk_simple_lighting
)
set_tests_properties(test_gdk_simple_lighting PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)
