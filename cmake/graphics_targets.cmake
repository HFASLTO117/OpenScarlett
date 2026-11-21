# Modular graphics subsystem build rules (owned by graphics subsystem)

target_sources(openscarlett_core PRIVATE
    src/gpu/d3d12_adapter.cpp
    src/gpu/xg_layout.cpp
    src/middleware/ffx_gpu_fsr.cpp
)

add_library(d3d12_xs SHARED
    src/gpu/d3d12_xs/device_x.cpp
    src/gpu/d3d12_xs/command_queue_x.cpp
    src/gpu/d3d12_xs/command_list_x.cpp
    src/shims/d3d12_xs_shim.cpp
)
set_target_properties(d3d12_xs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "d3d12_xs"
)
target_include_directories(d3d12_xs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(d3d12_xs PRIVATE OPENSCARLETT_BUILDING_D3D12_XS=1)
target_link_libraries(d3d12_xs PRIVATE openscarlett_core d3d12 dxgi)
if(MINGW)
    target_sources(d3d12_xs PRIVATE src/shims/d3d12_xs_mingw.def)
endif()
if(MSVC)
    target_compile_options(d3d12_xs PRIVATE /W4)
endif()

add_library(xg_xs SHARED
    src/shims/xg_xs_shim.cpp
)
set_target_properties(xg_xs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "xg_xs"
)
target_link_libraries(xg_xs PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(xg_xs PRIVATE /W4)
endif()

openscarlett_add_test(test_d3d12_xs_live tests/test_d3d12_xs_live.cpp)
target_link_libraries(test_d3d12_xs_live PRIVATE d3d12_xs xg_xs)
