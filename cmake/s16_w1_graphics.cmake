# Sprint 16 Workstream 1: Graphics Engine 100% Completeness Verification Suite

openscarlett_add_test(test_s16_graphics_completeness tests/test_s16_graphics_completeness.cpp)
target_link_libraries(test_s16_graphics_completeness PRIVATE openscarlett_core d3d12_xs xg_xs)
if(WIN32)
    target_link_libraries(test_s16_graphics_completeness PRIVATE d3d12 dxgi ole32 uuid)
endif()
target_include_directories(test_s16_graphics_completeness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s16_graphics_completeness)
endif()
if(MSVC)
    target_compile_options(test_s16_graphics_completeness PRIVATE /W4)
endif()
