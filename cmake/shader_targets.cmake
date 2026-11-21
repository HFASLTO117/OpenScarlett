# OpenScarlett RDNA2 / GFX10.3 Shader Lifter, Reference Interpreter, & Cache targets

target_sources(openscarlett_core PRIVATE
    src/gpu/rdna2_lifter.cpp
    src/gpu/rdna2_interpreter.cpp
    src/gpu/shader_cache.cpp
)

openscarlett_add_test(test_rdna2_lifter_live tests/test_rdna2_lifter_live.cpp)

openscarlett_add_test(test_d3d12_xs_autolift tests/test_d3d12_xs_autolift.cpp)
target_link_libraries(test_d3d12_xs_autolift PRIVATE d3d12_xs)
