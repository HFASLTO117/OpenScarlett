# Sprint 14 Worker 2: Enhanced Barriers Proxy Unwrapping, Agility SDK Exports & Discovery,
# Live VRAM Residency Manager + Sparse Window-0 Isolation, Xbox Series PSO Stream Subobjects,
# Proxy Resource1/2/Heap1 Hardening, Striped PSO Compilation, & Lock-Free FenceWaitDispatcher.

openscarlett_add_test(test_s14_d3d12_barriers_residency tests/test_s14_d3d12_barriers_residency.cpp)
target_include_directories(test_s14_d3d12_barriers_residency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_s14_d3d12_barriers_residency PRIVATE
    openscarlett_core
    d3d12_xs
)
if(WIN32)
    target_link_libraries(test_s14_d3d12_barriers_residency PRIVATE
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
    )
endif()

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s14_d3d12_barriers_residency)
endif()
