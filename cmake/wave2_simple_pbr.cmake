# OpenScarlett Wave 2 — Agent F: SimplePBR Multi-Frame 3D MSAA/Depth GDK Live Sample
# Owned by wave2/gdk-simple-pbr worktree.

# Standalone Xbox-ABI Guest Executable:
# Deliberately does NOT link openscarlett_core; imports ONLY Xbox/GDK shim DLLs.
openscarlett_add_gdk_guest_exe(gdk_simple_pbr tests/gdk_samples/gdk_simple_pbr.cpp)
target_link_libraries(gdk_simple_pbr PRIVATE
    d3d12_xs
    xgameruntime
    GameInput
    pixevt
)

# Portable End-to-End Test Harness (launches zero-DLL package dir via openscarlett.exe run)
openscarlett_add_test(test_gdk_simple_pbr tests/test_gdk_simple_pbr.cpp)
add_dependencies(test_gdk_simple_pbr
    openscarlett
    openscarlett_hook
    d3d12_xs
    xgameruntime
    GameInput
    pixevt
    gdk_simple_pbr
)
