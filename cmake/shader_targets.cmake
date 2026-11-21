# OpenScarlett RDNA2 / GFX10.3 Shader Lifter targets (modular sprint2-rdna2 build file)

target_sources(openscarlett_core PRIVATE
    src/gpu/rdna2_lifter.cpp
)

openscarlett_add_test(test_rdna2_lifter_live tests/test_rdna2_lifter_live.cpp)
