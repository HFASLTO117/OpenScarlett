# OpenScarlett Wave 2 — MeshletCull (AS + MS + PS GPU Frustum & Normal-Cone Culling)
# Owned exclusively by wave2/gdk-meshlet-cull worktree.

# Standalone Xbox-ABI Guest Executable (deliberately does NOT link openscarlett_core!)
openscarlett_add_gdk_guest_exe(gdk_meshlet_cull tests/gdk_samples/gdk_meshlet_cull.cpp)
target_link_libraries(gdk_meshlet_cull PRIVATE
    d3d12_xs
    xgameruntime
    pixevt
)

# Portable End-to-End Live Test Harness (Hardware GTX 1650 Ti MeshTier=10 + WARP Fallback)
openscarlett_add_test(test_gdk_meshlet_cull tests/test_gdk_meshlet_cull.cpp)
add_dependencies(test_gdk_meshlet_cull
    openscarlett
    openscarlett_hook
    d3d12_xs
    xgameruntime
    pixevt
    gdk_meshlet_cull
)
