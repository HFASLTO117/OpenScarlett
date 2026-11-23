# OpenScarlett UE5 Xbox Series X Import Bringup — Worker 1
# Targets: dxcompiler_xs.dll, XCRDAPI->DStorageGetFactory dependency,
# xg_xs.dll XGCreateTextureComputer / XGCreateTexture1DComputer, and live verification test.

# 1. dxcompiler_xs.dll (forwards DxcCreateInstance / DxcCreateInstance2 to host dxcompiler.dll)
add_library(dxcompiler_xs SHARED src/shims/dxcompiler_xs_shim.cpp)
set_target_properties(dxcompiler_xs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dxcompiler_xs"
)
target_link_libraries(dxcompiler_xs PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(dxcompiler_xs PRIVATE /W4)
endif()
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(dxcompiler_xs)
endif()
add_dependencies(openscarlett dxcompiler_xs)

# 2. Ensure dstorage_xs.dll is built alongside xcrdapi.dll for DStorageGetFactory forwarding
if(TARGET xcrdapi AND TARGET dstorage_xs)
    add_dependencies(xcrdapi dstorage_xs)
endif()

# 3. Live DLL-loading verification test (LoadLibraryW + GetProcAddress)
openscarlett_add_test(test_ue5_dxc_xcrdapi_xg tests/test_ue5_dxc_xcrdapi_xg.cpp)
add_dependencies(test_ue5_dxc_xcrdapi_xg
    dxcompiler_xs
    xcrdapi
    dstorage_xs
    xg_xs
)
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_ue5_dxc_xcrdapi_xg)
endif()
