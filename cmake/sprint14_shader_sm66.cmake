# Sprint 14 Worker 5: Issue #29 AMD RDNA4 VOPC Fix, SM 6.6 + Off-C:\ Shader Cache,
# Wired RayQuery Bridge, Radar Shader Accuracy Catches, and 779/779 + Live MS/AS Coverage.

openscarlett_add_test(test_s14_shader_sm66_779 tests/test_s14_shader_sm66_779.cpp)
target_include_directories(test_s14_shader_sm66_779 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s14_shader_sm66_779)
endif()
