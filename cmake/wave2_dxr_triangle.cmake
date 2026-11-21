# OpenScarlett Wave 2 — Agent H: DXRTriangle (Hardware RT-Core DXR + SAH Software BVH Fallback)
# Strictly owned by wave2/gdk-dxr-triangle worktree.

# Standalone Xbox-ABI Guest Executable:
# Deliberately does NOT link openscarlett_core! Resolves Xbox DLLs via PE IAT + openscarlett run APC hook.
openscarlett_add_gdk_guest_exe(gdk_dxr_triangle tests/gdk_samples/gdk_dxr_triangle.cpp)
target_link_libraries(gdk_dxr_triangle PRIVATE
    d3d12_xs
    xgameruntime
    XboxBvhBuilder
    pixevt
)

# Portable End-to-End Live Test Harness (runs gdk_dxr_triangle via openscarlett.exe run <package_dir>)
openscarlett_add_test(test_gdk_dxr_triangle tests/test_gdk_dxr_triangle.cpp)
add_dependencies(test_gdk_dxr_triangle
    openscarlett
    openscarlett_hook
    d3d12_xs
    xgameruntime
    XboxBvhBuilder
    pixevt
    gdk_dxr_triangle
)
