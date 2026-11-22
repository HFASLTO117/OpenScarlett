# OpenScarlett Sprint 8 Targets
# Full GFX10.3 (RDNA2) Scalar/Vector & Memory/Image/LDS/FLAT/EXP ISA Modules,
# Exact 779-Opcode Coverage Auditor & 3,120-Shader Corpus Benchmark.

target_sources(openscarlett_core PRIVATE
    src/gpu/rdna2_isa_scalar_vector.cpp
    src/gpu/rdna2_isa_mem_img.cpp
    src/gpu/rdna2_coverage_tool.cpp
)

target_include_directories(openscarlett_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
)

openscarlett_add_test(test_rdna2_isa_scalar_vector tests/test_rdna2_isa_scalar_vector.cpp)
openscarlett_add_test(test_rdna2_isa_mem_img tests/test_rdna2_isa_mem_img.cpp)
openscarlett_add_test(test_rdna2_corpus_bench tests/test_rdna2_corpus_bench.cpp)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_rdna2_isa_scalar_vector)
    _openscarlett_apply_portability_to_target(test_rdna2_isa_mem_img)
    _openscarlett_apply_portability_to_target(test_rdna2_corpus_bench)
endif()
