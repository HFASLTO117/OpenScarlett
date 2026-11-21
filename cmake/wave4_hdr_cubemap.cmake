# OpenScarlett Wave 4 — Agent G: HDRCubeMap Real-Binary GDK Workload (#3)
# Owned by wave2/gdk-hdr-cubemap worktree.

# Standalone Xbox-ABI Guest Executable:
# Deliberately does NOT link openscarlett_core; imports ONLY Xbox/GDK shim DLLs.
openscarlett_add_gdk_guest_exe(gdk_hdr_cubemap tests/gdk_samples/gdk_hdr_cubemap.cpp)
target_link_libraries(gdk_hdr_cubemap PRIVATE
    d3d12_xs
    xgameruntime
    pixevt
)

# Portable End-to-End Test Harness (launches zero-DLL package dir via openscarlett.exe run)
openscarlett_add_test(test_gdk_hdr_cubemap tests/test_gdk_hdr_cubemap.cpp)
add_dependencies(test_gdk_hdr_cubemap
    openscarlett
    openscarlett_hook
    d3d12_xs
    xgameruntime
    pixevt
    gdk_hdr_cubemap
)
