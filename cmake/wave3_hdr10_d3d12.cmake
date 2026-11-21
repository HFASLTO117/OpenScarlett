# OpenScarlett Wave 3 HDR10 / ST.2084 Swapchains, Default Array SRVs,
# Unified-Memory Texture Tracking & Fail-Closed Stubs Test Target.
# Owned by wave1/hdr10-d3d12-catches worktree.

if(WIN32)
    openscarlett_add_test(test_hdr10_d3d12 tests/test_hdr10_d3d12.cpp)
    target_link_libraries(test_hdr10_d3d12 PRIVATE d3d12_xs xcrdapi xg_xs user32)
endif()
