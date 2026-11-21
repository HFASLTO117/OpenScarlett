# OpenScarlett PresentX Interactive Windowing & GameInput Test Targets
# Owned by wave1/presentx-windowing worktree.

if(WIN32)
    openscarlett_add_test(test_presentx_windowing tests/test_presentx_windowing.cpp)
    target_link_libraries(test_presentx_windowing PRIVATE d3d12_xs GameInput user32)
endif()
