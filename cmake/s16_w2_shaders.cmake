# Sprint 16 Worker 2: RDNA2 / GFX10.3 Shader Compiler 100% Verification Suite

openscarlett_add_test(test_s16_shader_completeness tests/test_s16_shader_completeness.cpp)
target_include_directories(test_s16_shader_completeness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
if(WIN32)
    target_link_libraries(test_s16_shader_completeness PRIVATE d3d12 dxgi)
endif()
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s16_shader_completeness)
endif()

