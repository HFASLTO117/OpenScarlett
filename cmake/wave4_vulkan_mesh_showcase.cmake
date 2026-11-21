# OpenScarlett Wave 4 — VulkanMeshShowcase (RDNA2 XS2 AS+MS+PS + 32x32x32 3D LUT)
# Cross-API Parity: Vulkan 1.4 VK_EXT_mesh_shader vs D3D12 MeshTier=10 (and WARP fallback)
# Owned exclusively by wave2/vulkan-mesh-showcase worktree.

# Standalone Xbox-ABI Guest Executable (deliberately does NOT link openscarlett_core!)
openscarlett_add_gdk_guest_exe(gdk_vulkan_mesh_showcase tests/gdk_samples/gdk_vulkan_mesh_showcase.cpp)
target_link_libraries(gdk_vulkan_mesh_showcase PRIVATE
    d3d12_xs
    xgameruntime
    pixevt
)

# Portable End-to-End Live Test Harness (Vulkan 1.4 VK_EXT_mesh_shader + D3D12 MeshTier=10 + WARP)
openscarlett_add_test(test_vulkan_mesh_showcase tests/test_vulkan_mesh_showcase.cpp)
add_dependencies(test_vulkan_mesh_showcase
    openscarlett
    openscarlett_hook
    d3d12_xs
    xgameruntime
    pixevt
    gdk_vulkan_mesh_showcase
)
