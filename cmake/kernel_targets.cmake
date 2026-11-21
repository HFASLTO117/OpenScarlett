# OpenScarlett Kernel / Unified Memory / GPU Tile Pool / kernelx.dll targets

target_sources(openscarlett_core PRIVATE
    src/kernel/xmemory.cpp
    src/kernel/gpu_tile_pool.cpp
)

add_library(kernelx SHARED
    src/shims/kernelx_shim.cpp
)
set_target_properties(kernelx PROPERTIES
    PREFIX ""
    OUTPUT_NAME "kernelx"
)
target_link_libraries(kernelx PRIVATE openscarlett_core)
target_compile_definitions(kernelx PRIVATE OPENSCARLETT_BUILDING_KERNELX=1)
if(MSVC)
    target_compile_options(kernelx PRIVATE /W4)
endif()

openscarlett_add_test(test_kernelx tests/test_kernelx.cpp)
target_link_libraries(test_kernelx PRIVATE kernelx)
